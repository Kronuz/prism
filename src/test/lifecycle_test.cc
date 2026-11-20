// Runs the built prism (argv[1]) through the shared lifecycle:
//   daemon  --daemonize returns once prism serves; the pid file is locked while it runs, a second
//           instance leaves it alone, and SIGTERM stops it and releases the lock.
//   load    SIGTERM while clients hammer it and a slow reader is mid-response: that response
//           arrives whole, no response is cut short, and prism exits 0 within the shutdown bound.
//   stalled SIGTERM while a peer never reads its 4 MiB response: prism still exits 0 within a
//           2 s shutdown bound, cutting that connection.
//   drain   SIGTERM stops accepting at once while a request whose body is still arriving
//           finishes and is answered whole; a second SIGTERM cancels the one still waiting.
//   limits  a descriptor limit below max_clients: an explicit value exits EX_CONFIG, the default
//           shrinks to fit and serves.
//   tls     the server context's checks: a key readable by group exits EX_CONFIG, an owner-only
//           key serves HTTPS.

#include <algorithm>
#include <atomic>
#include <cctype>
#include <csignal>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sysexits.h>
#include <sys/wait.h>
#include <unistd.h>

#include "process.hh"  // task::locked_pid

namespace {

using clock_type = std::chrono::steady_clock;
using namespace std::chrono_literals;

int failures = 0;
#define CHECK(cond)                                                                \
	do {                                                                           \
		if (!(cond)) {                                                             \
			std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
			++failures;                                                            \
		}                                                                          \
	} while (0)

std::string prism;
std::filesystem::path scratch;

// A port below the ephemeral range, each used once: a client port left in TIME_WAIT by the load
// test, or one another process holds, would refuse a bind on a port taken from the ephemeral range.
unsigned short free_port() {
	static unsigned short next = static_cast<unsigned short>(20000 + ::getpid() % 20000);
	for (;; ++next) {
		int fd = ::socket(AF_INET, SOCK_STREAM, 0);
		sockaddr_in a{};
		a.sin_family = AF_INET;
		a.sin_port = htons(next);
		a.sin_addr.s_addr = htonl(INADDR_ANY);
		const bool free = ::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0;
		::close(fd);
		if (free) { return next++; }
	}
}

pid_t spawn(std::vector<std::string> args, std::size_t open_files = 0, const std::string& stderr_to = {}) {
	args.insert(args.begin(), prism);
	pid_t pid = ::fork();
	if (pid == 0) {
		if (open_files) {
			rlimit limit{};
			::getrlimit(RLIMIT_NOFILE, &limit);
			limit.rlim_cur = limit.rlim_max = open_files;
			if (::setrlimit(RLIMIT_NOFILE, &limit) != 0) { ::_exit(126); }
		}
		if (!stderr_to.empty()) {
			const int fd = ::open(stderr_to.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
			if (fd >= 0) { ::dup2(fd, 2); }
		}
		std::vector<char*> argv;
		for (auto& a : args) { argv.push_back(a.data()); }
		argv.push_back(nullptr);
		::execv(argv[0], argv.data());
		::_exit(127);
	}
	return pid;
}

int exit_status(pid_t pid) {
	int status = 0;
	while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
	return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

int connect_to(unsigned short port, int rcvbuf = 0) {
	int fd = ::socket(AF_INET, SOCK_STREAM, 0);
	if (rcvbuf) { ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf)); }
	sockaddr_in a{};
	a.sin_family = AF_INET;
	a.sin_port = htons(port);
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
		::close(fd);
		return -1;
	}
	return fd;
}

bool send_all(int fd, const std::string& bytes) {
	std::size_t sent = 0;
	while (sent < bytes.size()) {
		auto n = ::send(fd, bytes.data() + sent, bytes.size() - sent, 0);
		if (n < 0 && errno == EINTR) { continue; }
		if (n <= 0) { return false; }
		sent += std::size_t(n);
	}
	return true;
}

std::string request(const std::string& method, const std::string& path, const std::string& body = {}) {
	return method + " " + path + " HTTP/1.1\r\nHost: localhost\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
}

// One response read off a connection. `started` once any byte came; `whole` once the head and
// Content-Length bytes of body came. `pace` reads `chunk` bytes at a time with a sleep between.
struct Response {
	bool started = false, whole = false;
	int status = 0;
	std::string body;
};

struct Reader {
	int fd;
	std::string buffered;
	std::size_t chunk = 64 * 1024;
	std::chrono::milliseconds pace{0};
	std::function<void()> on_head;  // once the head is in

	bool fill() {
		if (pace.count()) { std::this_thread::sleep_for(pace); }
		std::string b(chunk, '\0');
		auto n = ::recv(fd, b.data(), b.size(), 0);
		while (n < 0 && errno == EINTR) { n = ::recv(fd, b.data(), b.size(), 0); }
		if (n <= 0) { return false; }
		buffered.append(b.data(), std::size_t(n));
		return true;
	}

	Response read() {
		Response r;
		std::size_t end;
		while ((end = buffered.find("\r\n\r\n")) == std::string::npos) {
			if (!fill()) { r.started = !buffered.empty(); return r; }
		}
		r.started = true;
		const std::string head = buffered.substr(0, end);
		buffered.erase(0, end + 4);
		r.status = std::atoi(head.c_str() + head.find(' ') + 1);
		std::string lower = head;
		std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
		const auto at = lower.find("\r\ncontent-length:");
		const std::size_t length = at == std::string::npos ? 0 : std::strtoull(head.c_str() + at + 17, nullptr, 10);
		if (on_head) { on_head(); }
		while (buffered.size() < length) {
			if (!fill()) { return r; }
		}
		r.body = buffered.substr(0, length);
		buffered.erase(0, length);
		r.whole = true;
		return r;
	}
};

std::string slurp(const std::filesystem::path& p) {
	std::ifstream in(p);
	return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

template <typename F>
bool eventually(std::chrono::milliseconds bound, F f) {
	for (auto until = clock_type::now() + bound; clock_type::now() < until; std::this_thread::sleep_for(10ms)) {
		if (f()) { return true; }
	}
	return f();
}

void daemon_test() {
	const auto dir = scratch / "daemon";
	const auto port = std::to_string(free_port());
	const std::vector<std::string> args{"--daemonize", "--runtime-dir", dir.string(), "--port", port, "--reactors", "2", "--no-color"};
	CHECK(exit_status(spawn(args)) == 0);

	// Serving before the parent returned: the first connect is answered.
	int fd = connect_to(static_cast<unsigned short>(std::stoi(port)));
	CHECK(fd >= 0);
	if (fd >= 0) {
		CHECK(send_all(fd, request("GET", "/hello")));
		Reader reader{fd};
		auto r = reader.read();
		CHECK(r.whole && r.status == 200 && r.body == "hello from prism\n");
		::close(fd);
	}

	const auto pidfile = dir / ("prism-" + port + ".pid");
	const auto pid = task::locked_pid(pidfile);
	CHECK(pid.has_value());
	if (!pid) { return; }

	// A second instance finds the lock held and leaves the first serving.
	CHECK(exit_status(spawn(args)) == 0);
	CHECK(task::locked_pid(pidfile) == pid);
	CHECK(::kill(*pid, 0) == 0);

	CHECK(::kill(*pid, SIGTERM) == 0);
	CHECK(eventually(10s, [&] { return !task::locked_pid(pidfile); }));
	const auto log = slurp(dir / ("prism-" + port + ".log"));
	CHECK(log.find("already running") != std::string::npos);
	CHECK(log.find("stopped") != std::string::npos);
	if (failures) { std::fprintf(stderr, "daemon log:\n%s\n", log.c_str()); }
}

void load_test() {
	const auto dir = scratch / "load";
	const unsigned short port = free_port();
	const auto bound = 10s;
	const auto child = spawn({"--runtime-dir", dir.string(), "--port", std::to_string(port), "--reactors", "2",
		"--shutdown-timeout-ms", std::to_string(std::chrono::milliseconds(bound).count()), "-q"});
	int probe = -1;
	CHECK(eventually(10s, [&] { return (probe = connect_to(port)) >= 0; }));
	if (probe < 0) { ::kill(child, SIGKILL); exit_status(child); return; }
	::close(probe);

	// Hammering clients: keep-alive requests until the stop; a response that starts must end.
	std::atomic<bool> done{false};
	std::atomic<int> whole{0}, cut{0};
	std::vector<std::thread> hammers;
	for (int t = 0; t < 4; ++t) {
		hammers.emplace_back([&, t] {
			const std::string req = t % 2 ? request("GET", "/hello") : request("POST", "/echo", std::string(2048, 'x'));
			while (!done.load()) {
				int fd = connect_to(port);
				if (fd < 0) { std::this_thread::sleep_for(1ms); continue; }
				Reader reader{fd};
				while (!done.load() && send_all(fd, req)) {
					auto r = reader.read();
					if (r.whole && r.status == 200) { ++whole; continue; }
					if (r.started && !(r.whole && r.status == 503)) { ++cut; }  // a refusal after the stop is whole
					break;
				}
				::close(fd);
			}
		});
	}
	std::this_thread::sleep_for(300ms);

	// The slow reader: a 4 MiB echo read 64 KiB per 10 ms, so the stop lands mid-response.
	std::string body(4u << 20, '\0');
	std::mt19937_64 random(42);
	for (auto& c : body) { c = static_cast<char>(random()); }
	int fd = connect_to(port, 16 * 1024);
	CHECK(fd >= 0);
	clock_type::time_point signalled{};
	Response slow;
	if (fd >= 0) {
		CHECK(send_all(fd, request("POST", "/echo", body)));
		Reader reader{fd};
		reader.pace = 10ms;
		reader.on_head = [&] {
			signalled = clock_type::now();
			CHECK(::kill(child, SIGTERM) == 0);
		};
		slow = reader.read();
		::close(fd);
	}
	CHECK(slow.whole && slow.status == 200);
	CHECK(slow.body.size() > body.size() && slow.body.compare(slow.body.size() - body.size() - 1, body.size(), body) == 0);

	const int status = exit_status(child);
	const auto took = clock_type::now() - signalled;
	done = true;
	for (auto& h : hammers) { h.join(); }
	CHECK(status == 0);
	CHECK(took < bound);
	CHECK(whole.load() > 0);
	CHECK(cut.load() == 0);
	std::fprintf(stderr, "load: %d whole responses, %d cut; exit %d %lld ms after SIGTERM\n", whole.load(), cut.load(), status,
		static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(took).count()));
}

// The status of `pid` once it exits, or nullopt after `bound` (then it is killed and reaped).
std::optional<int> exit_within(pid_t pid, std::chrono::milliseconds bound) {
	int status = 0;
	if (!eventually(bound, [&] { return ::waitpid(pid, &status, WNOHANG) == pid; })) {
		::kill(pid, SIGKILL);
		exit_status(pid);
		return std::nullopt;
	}
	return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

void stalled_test() {
	const auto dir = scratch / "stalled";
	const unsigned short port = free_port();
	const auto bound = 2s;
	const auto child = spawn({"--runtime-dir", dir.string(), "--port", std::to_string(port), "--reactors", "1",
		"--shutdown-timeout-ms", std::to_string(std::chrono::milliseconds(bound).count()), "-q"});
	int probe = -1;
	CHECK(eventually(10s, [&] { return (probe = connect_to(port)) >= 0; }));
	if (probe < 0) { ::kill(child, SIGKILL); exit_status(child); return; }
	::close(probe);

	// A peer that reads the response's first bytes, then nothing: the write stalls on it.
	std::string body(4u << 20, 'x');
	int fd = connect_to(port, 16 * 1024);
	CHECK(fd >= 0 && send_all(fd, request("POST", "/echo", body)));
	char head[1024];
	CHECK(fd >= 0 && ::recv(fd, head, sizeof(head), 0) > 0);
	std::this_thread::sleep_for(200ms);

	const auto signalled = clock_type::now();
	CHECK(::kill(child, SIGTERM) == 0);
	const auto status = exit_within(child, 10s);
	const auto took = clock_type::now() - signalled;
	if (fd >= 0) { ::close(fd); }
	CHECK(status == 0);
	CHECK(took < bound + 1s);
	std::fprintf(stderr, "stalled: exit %d %lld ms after SIGTERM (bound %lld ms)\n", status.value_or(-1),
		static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(took).count()),
		static_cast<long long>(std::chrono::milliseconds(bound).count()));
}

// A request whose body is still arriving at the stop is not reset: the drain stops accepting and
// waits for it. A second signal then cancels the request still waiting.
void drain_test() {
	const auto dir = scratch / "drain";
	const unsigned short port = free_port();
	const auto child = spawn({"--runtime-dir", dir.string(), "--port", std::to_string(port), "--reactors", "2",
		"--shutdown-timeout-ms", "20000", "-q"});
	int probe = -1;
	CHECK(eventually(10s, [&] { return (probe = connect_to(port)) >= 0; }));
	if (probe < 0) { ::kill(child, SIGKILL); exit_status(child); return; }
	::close(probe);

	const std::string body(1000, 'b');
	const std::string head = "POST /echo HTTP/1.1\r\nHost: localhost\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n";
	int slow = connect_to(port);
	CHECK(slow >= 0 && send_all(slow, head + body.substr(0, 400)));
	std::this_thread::sleep_for(200ms);  // admitted: its first byte is in
	CHECK(::kill(child, SIGTERM) == 0);
	std::this_thread::sleep_for(500ms);

	// Stopped accepting: a new connection is refused or closed unanswered.
	int late = connect_to(port);
	if (late >= 0) {
		Reader reader{late};
		send_all(late, request("GET", "/hello"));
		CHECK(!reader.read().whole || reader.buffered.empty());
		::close(late);
	}
	// Still running: the request in flight was not cancelled.
	CHECK(::waitpid(child, nullptr, WNOHANG) == 0);

	CHECK(slow >= 0 && send_all(slow, body.substr(400)));
	Reader reader{slow};
	auto r = slow >= 0 ? reader.read() : Response{};
	CHECK(r.whole && r.status == 200 && r.body.size() >= body.size());
	if (slow >= 0) { ::close(slow); }
	const auto status = exit_within(child, 10s);
	CHECK(status == 0);

	// A second SIGTERM hurries: the request still being received is cancelled at once.
	const unsigned short port2 = free_port();
	const auto child2 = spawn({"--runtime-dir", dir.string(), "--port", std::to_string(port2), "--reactors", "2",
		"--shutdown-timeout-ms", "20000", "-q"});
	CHECK(eventually(10s, [&] { return (probe = connect_to(port2)) >= 0; }));
	if (probe < 0) { ::kill(child2, SIGKILL); exit_status(child2); return; }
	::close(probe);
	int stuck = connect_to(port2);
	CHECK(stuck >= 0 && send_all(stuck, head + body.substr(0, 400)));
	std::this_thread::sleep_for(200ms);
	CHECK(::kill(child2, SIGTERM) == 0);
	std::this_thread::sleep_for(500ms);
	CHECK(::waitpid(child2, nullptr, WNOHANG) == 0);
	const auto hurried = clock_type::now();
	CHECK(::kill(child2, SIGTERM) == 0);
	const auto status2 = exit_within(child2, 15s);
	const auto took = clock_type::now() - hurried;
	if (stuck >= 0) { ::close(stuck); }
	CHECK(status2 == 0);
	CHECK(took < 8s);
	std::fprintf(stderr, "drain: finished request %d, hurried exit %d %lld ms after the second SIGTERM\n", r.status,
		status2.value_or(-1), static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(took).count()));
}

// A descriptor limit of 200 holds 136 connections with the reserve of 64.
void limits_test() {
	const auto dir = scratch / "limits";
	std::filesystem::create_directories(dir);
	const unsigned short refused_port = free_port();
	const auto refused_log = (dir / "refused.err").string();
	const auto refused = spawn({"--runtime-dir", dir.string(), "--port", std::to_string(refused_port), "--max-clients", "1024"}, 200, refused_log);
	CHECK(exit_status(refused) == EX_CONFIG);
	CHECK(slurp(refused_log).find("max_clients 1024 needs 1088 descriptors") != std::string::npos);

	const unsigned short port = free_port();
	const auto fitted_log = (dir / "fitted.err").string();
	const auto fitted = spawn({"--runtime-dir", dir.string(), "--port", std::to_string(port), "--reactors", "1", "--no-color"}, 200, fitted_log);
	int probe = -1;
	CHECK(eventually(10s, [&] { return (probe = connect_to(port)) >= 0; }));
	if (probe >= 0) {
		CHECK(send_all(probe, request("GET", "/hello")));
		Reader reader{probe};
		auto r = reader.read();
		CHECK(r.whole && r.status == 200);
		::close(probe);
	}
	::kill(fitted, SIGTERM);
	CHECK(exit_within(fitted, 10s) == 0);
	CHECK(slurp(fitted_log).find("max_clients is 136") != std::string::npos);
}

#ifdef PRISM_TLS
// The server context's checks, through the built binary: a key readable by group is refused, an
// owner-only one serves HTTPS. Skipped without the openssl and curl tools.
void tls_test() {
	if (std::system("openssl version >/dev/null 2>&1") != 0 || std::system("curl --version >/dev/null 2>&1") != 0) {
		std::fprintf(stderr, "tls: skipped (no openssl or curl)\n");
		return;
	}
	const auto dir = scratch / "tls";
	std::filesystem::create_directories(dir);
	const auto cert = dir / "cert.pem", key = dir / "key.pem";
	const std::string make = "openssl req -x509 -newkey rsa:2048 -nodes -days 2 -subj /CN=localhost -keyout " + key.string() +
		" -out " + cert.string() + " >/dev/null 2>&1";
	if (std::system(make.c_str()) != 0) { std::fprintf(stderr, "tls: skipped (openssl could not make a certificate)\n"); return; }

	::chmod(key.c_str(), 0644);
	const auto err = (dir / "group.err").string();
	const auto refused = spawn({"--runtime-dir", dir.string(), "--port", std::to_string(free_port()), "--tls-cert", cert.string(),
		"--tls-key", key.string()}, 0, err);
	CHECK(exit_status(refused) == EX_CONFIG);
	CHECK(slurp(err).find("readable by group or others") != std::string::npos);

	::chmod(key.c_str(), 0600);
	const unsigned short port = free_port();
	const auto child = spawn({"--runtime-dir", dir.string(), "--port", std::to_string(port), "--reactors", "1", "--tls-cert", cert.string(),
		"--tls-key", key.string(), "-q"});
	const std::string get = "curl -sk --max-time 5 https://127.0.0.1:" + std::to_string(port) + "/hello > " + (dir / "hello.out").string();
	CHECK(eventually(10s, [&] { return std::system(get.c_str()) == 0; }));
	CHECK(slurp(dir / "hello.out") == "hello from prism\n");
	::kill(child, SIGTERM);
	CHECK(exit_within(child, 10s) == 0);
}
#endif

}  // namespace

int main(int argc, char** argv) {
	if (argc != 2) {
		std::fprintf(stderr, "usage: prism_lifecycle_test PRISM\n");
		return 2;
	}
	prism = argv[1];
	std::signal(SIGPIPE, SIG_IGN);
	std::string tmpl = (std::filesystem::current_path() / "prism_lifecycle.XXXXXX").string();
	if (!::mkdtemp(tmpl.data())) {
		std::perror("mkdtemp");
		return 2;
	}
	scratch = tmpl;
	daemon_test();
	load_test();
	stalled_test();
	drain_test();
	limits_test();
#ifdef PRISM_TLS
	tls_test();
#endif
	std::error_code ec;
	if (!failures) { std::filesystem::remove_all(scratch, ec); }
	std::fprintf(stderr, failures ? "FAILED: %d checks\n" : "passed\n", failures);
	return failures ? 1 : 0;
}
