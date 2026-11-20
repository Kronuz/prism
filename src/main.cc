// prism — a flashy HTTP application server built on the Kronuz libraries.
//
// This is the framework/demo skeleton: everything that is *not* Search or Storage
// from an app like Xapiand, assembled on the extracted libraries. The application
// itself is a plain http::Router of views; the framework wraps it with the bells
// and whistles: a splash banner, colored and leveled request/response logging
// (with iTerm2 inline-image previews), content negotiation, transparent response
// compression, and a formatted traceback on an unhandled error. The logging and
// color run on Kronuz/logger + term-color (the same stack Xapiand uses); the
// traceback plugs into the logger's backtrace hook.
//
//   ./prism --port 8880 -vv
//   ./prism --config prism.json --daemonize     # detaches once serving
//   curl -s localhost:8880/                       # welcome, route list
//   curl -s localhost:8880/hello -H 'Accept: application/json'
//   curl -s --compressed localhost:8880/hello     # gzip/zstd negotiated
//   curl -s localhost:8880/echo -XPOST -d 'hi'    # echoes the request
//   curl -s localhost:8880/image -o /tmp/p.png    # a PNG (previewed in the log)
//   curl -s localhost:8880/boom                   # 500 + traceback in the log
//
// The framework pieces here (Splash, AccessLog) are the first candidates to
// extract into their own libs; see framework-plan / README.

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>
#include <sysexits.h>
#include <unistd.h>

#include <asio/io_context.hpp>
#include <rapidjson/document.h>

#include "http_handler.h"
#include "http_router.h"
#include "http_asio.h"
#include "traceback.h"
#include "lifecycle.hh"     // task: components, stop_signals, lifecycle
#include "process.hh"       // task: run_daemon, ProcessLock
#include "task_asio.hh"     // task: asio_scheduler, the control loop
#ifdef PRISM_TLS
#include "http_asio_tls.h"  // http: the TLS service constructor
#include "reactor_tls.h"    // reactor: TLS contexts (the Server role)
#endif
#include "colors.h"         // term-color: rgb()/brgb()/CLEAR_COLOR (stacked escapes)
#include "color_tools.hh"   // term-color: the runtime color class
#include "logger.h"         // Kronuz/logger: Logging, LogConfig, the L_* macros
#include "http_log.h"       // Kronuz/http-log: the request/response logging middleware
#include "cppcodec/base64_rfc4648.hpp"   // base64 (reached via http-log's include path)

// ---------------------------------------------------------------------------
// col — colors as term-color stacked escapes (16/256/truecolor). Kronuz/logger's
// sink resolves them to the terminal's depth and honors --color / NO_COLOR at
// write time, so nothing here gates on the terminal: the logger owns that policy.
// Bold rides inside the color (color::ansi(true)), never a bare SGR, so the
// stacked triple stays intact for collapse().
// ---------------------------------------------------------------------------
namespace col {
inline std::string fg(int r, int g, int b) { ::color c(r, g, b); return c.ansi(false); }
inline std::string bfg(int r, int g, int b) { ::color c(r, g, b); return c.ansi(true); }
inline std::string reset() { return std::string(std::string_view(CLEAR_COLOR)); }
inline std::string grey() { return fg(105, 105, 105); }
}  // namespace col

// ---------------------------------------------------------------------------
// The embedded PNG is base64-decoded with cppcodec (the codec the rest of the
// Kronuz libs use), reached through http-log's include path. See /image below.
// ---------------------------------------------------------------------------

// A 48x48 spectrum PNG, embedded so /image needs no asset at runtime.
static const char* kPrismPngB64 =
	"iVBORw0KGgoAAAANSUhEUgAAADAAAAAwCAIAAADYYG7QAAABAElEQVR42s3O4WACAAAF4aN5AiNIIIIEIhhBAhEkEEECESQwggje3jLYvxO4+2jSr/SQHtNTek6/00t6TW/pPX2kz/SV/qTvtHk3P82reTaP5t7cmmtzab6bc3Nqjs2h+fqr/2+ASrMBKs0GqDQboNJsgEqzASrNBqg0G6DSbIBKswEqzQaoNBug0myASrMBKs0GqDQboNJsgEqzOirN6qg0q6PSrI5KszoqzeqoNKuj0qyOSrM6Ks3qqDSro9KsjkqzOirN6qg0q6PSvD8gkaYfkEizASrNBqg0G6DSbIBKswEqzQaoNBug0myASrMBKs0GqDQboNJsgEqzASrNBr8MX5RP/5PNSQAAAABJRU5ErkJggg==";

// ---------------------------------------------------------------------------
// banner — a glass prism dispersing the wordmark into a spectrum, printed
// through the logger sink at NOTICE (gated by the log level and routed through
// the sink, the way Xapiand's banner() prints — not the immediate L_PRINT).
// The extracted logger's L() has no base-color argument (Xapiand's does); the
// colors live in the message and it ends on a reset. Mirrors Xapiand's layout:
// an icon on the left, an ASCII wordmark on the right in figlet's Standard font
// (the same font as Xapiand's "apiand"). The prism is colorless glass; "prism"
// leaves the far side split into a spectrum, one rainbow stop per row.
// (Phase 2: extract as Kronuz/splash, data driven by name/version/subtitle.)
// ---------------------------------------------------------------------------
static void banner(unsigned port, std::size_t reactors, bool https) {
	if (Logging::config.log_level < LOG_NOTICE) {
		return;
	}
	// The glass prism icon (left) and the "prism" wordmark (right, figlet
	// Standard font, with the "p" and "m" hand-stylized into descenders — the
	// way Xapiand hand-stylizes its "X"). Backslashes are C++-escaped.
	static const char* prism[7] = {
		"          ",
		"    /\\    ",
		"   /  \\   ",
		"  /    \\  ",
		" /      \\ ",
		"/________\\",
		"          ",
	};
	static const char* word[7] = {
		"            _",
		" _ __  _ __(_)___ _ __ ___",
		"| '_ \\| '__| / __| '_ ` _ \\",
		"| |_) | |  | \\__ \\ | | | | |",
		"| .__/|_|  |_|___/_| |_| | |",
		"| |                       \\|",  // "m" descender frame; the tagline is injected into the gap (loop below)
		"|/",
	};
	// Colorless glass (top-lit), and the spectrum it disperses top-to-bottom
	// (the six stops, the last reused for the descender tail).
	static const int glass[7][3] = {
		{0, 0, 0}, {245, 245, 245}, {220, 220, 220}, {195, 195, 195}, {170, 170, 170}, {145, 145, 145}, {0, 0, 0},
	};
	static const int spectrum[6][3] = {
		{233, 30, 99}, {255, 152, 0}, {255, 235, 59}, {76, 175, 80}, {33, 150, 243}, {103, 58, 183},
	};
	std::string b = "\n\n";
	for (int i = 0; i < 7; ++i) {
		const auto& g = glass[i];
		const auto& s = spectrum[i < 6 ? i : 5];
		b += col::bfg(g[0], g[1], g[2]) + prism[i] + col::reset() + " ";
		if (i == 5) {
			// The tagline nestles inside the "m" descender, framed by its two
			// strokes; " the flashy app server " is exactly the 23-col gap, so
			// the closing "\|" stays aligned under the stem above it.
			b += col::bfg(s[0], s[1], s[2]) + "| |" + col::reset() +
				col::grey() + " the flashy app server " + col::reset() +
				col::bfg(s[0], s[1], s[2]) + "\\|" + col::reset();
		} else {
			b += col::bfg(s[0], s[1], s[2]) + word[i] + col::reset();
		}
		b += "\n";
	}
	b += "\n  " + col::fg(76, 175, 80) + "listening" + col::reset() + (https ? " https://127.0.0.1:" : " http://127.0.0.1:") +
		std::to_string(port) + "/   " + col::grey() + "(" + std::to_string(reactors) + " reactors)" + col::reset() + "\n";
	L(-LOG_NOTICE, "{}", b);
}

// A structural JSON pretty-printer for the log. It re-indents rather than
// validates: whitespace outside strings is normalized, string literals (with
// their escapes) pass through untouched, and empty {} / [] stay on one line. Good
// enough to make a minified request/response body readable in the verbose log.
static std::string pretty_json(std::string_view in) {
	std::string out;
	out.reserve(in.size() + in.size() / 4);
	int depth = 0;
	bool in_str = false, esc = false, just_opened = false;
	auto newline = [&](int d) { out += '\n'; out.append(std::size_t(d) * 2, ' '); };
	for (char c : in) {
		if (in_str) {
			out += c;
			if (esc) esc = false;
			else if (c == '\\') esc = true;
			else if (c == '"') in_str = false;
			continue;
		}
		if (c == ' ' || c == '\t' || c == '\n' || c == '\r') continue;  // drop; we re-indent
		// A pending newline from a just-opened container, unless it closes empty.
		if (just_opened && c != '}' && c != ']') { newline(depth); }
		just_opened = false;
		switch (c) {
			case '"': out += c; in_str = true; break;
			case '{': case '[': out += c; ++depth; just_opened = true; break;
			case '}': case ']': --depth; if (out.size() && out.back() != '{' && out.back() != '[') newline(depth); out += c; break;
			case ',': out += c; newline(depth); break;
			case ':': out += ": "; break;
			default: out += c;
		}
	}
	return out;
}

// The request/response logging middleware is Kronuz/http-log (http_log::AccessLog);
// prism injects the JSON reindenter above as its body prettifier. See main().

// ---------------------------------------------------------------------------
// The application: a plain Router of views. Everything above is the framework.
// ---------------------------------------------------------------------------
class DemoApp : public http::HttpHandler {
	http::Router router_;
public:
	DemoApp() {
		router_.route("GET", "/", [](http::Request&, http::ResponseWriter& resp, const http::Params&) -> task::task<void> {
			resp.send(200,
				"prism -- a flashy HTTP application server\n\n"
				"  GET  /        this page\n"
				"  GET  /hello   content-negotiated greeting (Accept: text/html | application/json | text/plain)\n"
				"  ANY  /echo    echoes your request\n"
				"  ANY  /json    echoes a JSON body (prettified in the verbose log)\n"
				"  GET  /image   a spectrum PNG (previewed inline in the log on iTerm2)\n"
				"  GET  /boom    raises an error -> 500 + traceback in the log\n");
			co_return;
		});
		router_.route("GET", "/hello", [](http::Request& req, http::ResponseWriter& resp, const http::Params&) -> task::task<void> {
			auto accept = req.header("Accept");
			if (accept.find("application/json") != std::string_view::npos)
				resp.send(200, "{\"hello\":\"world\",\"from\":\"prism\"}\n", "application/json");
			else if (accept.find("text/html") != std::string_view::npos)
				resp.send(200, "<h1>hello from <em>prism</em></h1>\n", "text/html; charset=utf-8");
			else
				resp.send(200, "hello from prism\n");
			co_return;
		});
		auto echo = [](http::Request& req, http::ResponseWriter& resp, const http::Params&) -> task::task<void> {
			std::string out = req.method + " " + req.path + "\n";
			for (auto& [k, v] : req.headers) { out += k; out += ": "; out += v; out += "\n"; }
			if (!req.body.empty()) { out += "\n"; out += req.body; out += "\n"; }
			resp.send(200, out);
			co_return;
		};
		router_.route("GET", "/echo", echo);
		router_.route("POST", "/echo", echo);
		router_.route("PUT", "/echo", echo);
		auto json_echo = [](http::Request& req, http::ResponseWriter& resp, const http::Params&) -> task::task<void> {
			std::string body = req.body.empty() ? "{\"posted\":null,\"from\":\"prism\"}" : req.body;
			resp.send(200, body, "application/json");
			co_return;
		};
		router_.route("POST", "/json", json_echo);
		router_.route("GET", "/json", json_echo);
		router_.route("GET", "/image", [](http::Request&, http::ResponseWriter& resp, const http::Params&) -> task::task<void> {
			static const std::string png = cppcodec::base64_rfc4648::decode<std::string>(std::string_view(kPrismPngB64));
			resp.send(200, png, "image/png");
			co_return;
		});
		router_.route("GET", "/boom", [](http::Request&, http::ResponseWriter&, const http::Params&) -> task::task<void> {
			throw std::runtime_error("the /boom route always raises");
			co_return;
		});
	}
	task::task<void> handle(http::Request& req, http::ResponseWriter& resp) override { co_await router_.handle(req, resp); }
};


// ---------------------------------------------------------------------------
// The composition (cpp-libraries docs/server-composition.md): options, process,
// signals, limits, TLS, loop and components, lifecycle. Prism has no housekeeping.
// ---------------------------------------------------------------------------
namespace {

// A bad value exits EX_USAGE; an unreadable configuration or certificate, EX_CONFIG.
struct usage_error : std::runtime_error { using std::runtime_error::runtime_error; };
struct config_error : std::runtime_error { using std::runtime_error::runtime_error; };

// Every option has one name, `--max-clients` as a flag and `max_clients` in prism.json.
struct Options {
	unsigned short port = 8880;
	std::string bind_address;  // empty: every interface
	std::size_t reactors = std::max(1u, std::thread::hardware_concurrency());
	std::size_t max_clients = 1024;
	bool max_clients_set = false;  // an explicit value must fit the descriptor limit; the default shrinks
	std::size_t max_in_flight = 1024;  // per reactor; 0 = no cap
	std::chrono::milliseconds idle_timeout{75'000};
	std::chrono::milliseconds shutdown_timeout{35'000};
	std::filesystem::path tls_cert, tls_key;  // the key is the private file, named by path
	std::filesystem::path runtime_dir;
	bool daemonize = false;
	int verbosity = 0;  // -1 warnings only, 0 requests (NOTICE), each step one level more
	std::string color = "auto";
	bool tracebacks = true;
};

constexpr std::chrono::milliseconds kShutdownGrace{5'000};  // fixed (Settled 3)
constexpr std::size_t kDescriptorReserve = 64;  // open besides connections: log, pid lock, listeners, TLS files
constexpr auto kMaxDuration = std::chrono::milliseconds(std::chrono::hours(24));

const char* kUsage =
	"usage: prism [--config FILE] [--port N] [--bind-address ADDR] [--reactors N]\n"
	"             [--max-clients N] [--max-in-flight N] [--idle-timeout-ms MS]\n"
	"             [--shutdown-timeout-ms MS] [--tls-cert FILE --tls-key FILE]\n"
	"             [--daemonize] [--runtime-dir DIR] [-q | -v | -vv ... | --verbosity N]\n"
	"             [--color MODE | --no-color] [--no-tracebacks]\n"
	"\n"
	"prism.json (--config) holds the same options as keys: max_clients, idle_timeout_ms, ...;\n"
	"flags override it, and its relative paths are relative to the file. The TLS key is\n"
	"the private file. --daemonize detaches once serving, logging to RUNTIME_DIR/prism-PORT.log;\n"
	"RUNTIME_DIR/prism-PORT.pid is locked while prism runs. SIGINT or SIGTERM stops:\n"
	"prism stops accepting connections and lets the requests in flight finish, until the\n"
	"shutdown bound less the 5 s grace; then it cancels them, a response being written gets\n"
	"the grace (at most the bound) and its connection closes. A second signal cancels at\n"
	"once; a third flushes and dies.\n";

std::uint64_t unsigned_value(const std::string& key, const std::string& value, std::uint64_t minimum, std::uint64_t maximum) {
	std::uint64_t n = 0;
	auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), n);
	if (value.empty() || error != std::errc{} || end != value.data() + value.size() || n < minimum || n > maximum) {
		throw usage_error(key + " must be an integer between " + std::to_string(minimum) + " and " + std::to_string(maximum));
	}
	return n;
}

bool bool_value(const std::string& key, const std::string& value) {
	if (value == "true") { return true; }
	if (value == "false") { return false; }
	throw usage_error(key + " must be true or false");
}

// Applies one option by its key; `base` resolves a relative path (prism.json's directory).
void set_option(Options& o, const std::string& key, const std::string& value, const std::filesystem::path& base) {
	auto path = [&] {
		if (value.empty()) { throw usage_error(key + " must name a file"); }
		return base.empty() ? std::filesystem::path(value) : (base / value).lexically_normal();
	};
	auto ms = [&](std::uint64_t minimum) {
		return std::chrono::milliseconds(unsigned_value(key, value, minimum, std::uint64_t(kMaxDuration.count())));
	};
	if (key == "port") { o.port = static_cast<unsigned short>(unsigned_value(key, value, 0, 65535)); }
	else if (key == "bind_address") { o.bind_address = value; }
	else if (key == "reactors") { o.reactors = unsigned_value(key, value, 1, 1024); }
	else if (key == "max_clients") { o.max_clients = unsigned_value(key, value, 1, 1u << 24); o.max_clients_set = true; }
	else if (key == "max_in_flight") { o.max_in_flight = unsigned_value(key, value, 0, 1u << 24); }
	else if (key == "idle_timeout_ms") { o.idle_timeout = ms(0); }
	else if (key == "shutdown_timeout_ms") { o.shutdown_timeout = ms(1); }
	else if (key == "tls_cert") { o.tls_cert = path(); }
	else if (key == "tls_key") { o.tls_key = path(); }
	else if (key == "runtime_dir") { o.runtime_dir = path(); }
	else if (key == "daemonize") { o.daemonize = bool_value(key, value); }
	else if (key == "verbosity") { o.verbosity = static_cast<int>(unsigned_value(key, value, 0, 8)); }
	else if (key == "color") { o.color = value; }
	else if (key == "tracebacks") { o.tracebacks = bool_value(key, value); }
	else { throw usage_error("unknown option: " + key); }
}

// prism.json: one object whose keys are the options; a value is a string, a number or a boolean.
void load_config(Options& o, const std::filesystem::path& file) {
	std::ifstream in(file, std::ios::binary);
	if (!in) { throw config_error("cannot read " + file.string()); }
	const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	rapidjson::Document doc;
	doc.Parse<rapidjson::kParseIterativeFlag | rapidjson::kParseValidateEncodingFlag>(bytes.data(), bytes.size());
	if (doc.HasParseError() || !doc.IsObject()) { throw config_error(file.string() + ": expected a JSON object"); }
	const auto base = std::filesystem::absolute(file).parent_path();
	for (auto it = doc.MemberBegin(); it != doc.MemberEnd(); ++it) {
		const std::string key(it->name.GetString(), it->name.GetStringLength());
		const auto& v = it->value;
		std::string value;
		if (v.IsString()) { value.assign(v.GetString(), v.GetStringLength()); }
		else if (v.IsBool()) { value = v.GetBool() ? "true" : "false"; }
		else if (v.IsUint64()) { value = std::to_string(v.GetUint64()); }
		else { throw usage_error(file.string() + ": " + key + " must be a string, a nonnegative integer or a boolean"); }
		set_option(o, key, value, base);
	}
}

Options parse(int argc, char** argv) {
	std::vector<std::string> args(argv + 1, argv + argc);
	Options o;
	for (std::size_t i = 0; i < args.size(); ++i) {  // the file first: flags override it
		if (args[i] == "--config" || args[i].rfind("--config=", 0) == 0) {
			if (args[i] == "--config" && i + 1 == args.size()) { throw usage_error("--config requires a file"); }
			load_config(o, args[i] == "--config" ? args[i + 1] : args[i].substr(9));
		}
	}
	for (std::size_t i = 0; i < args.size(); ++i) {
		const std::string& a = args[i];
		if (a == "-q") { o.verbosity = -1; continue; }
		if (a.size() >= 2 && a[0] == '-' && a[1] == 'v' && a.find_first_not_of('v', 1) == std::string::npos) {
			o.verbosity = static_cast<int>(a.size()) - 1;  // -v, -vv, ...: one level each above NOTICE
			continue;
		}
		if (a == "--daemonize") { o.daemonize = true; continue; }
		if (a == "--no-color") { o.color = "never"; continue; }
		if (a == "--no-tracebacks") { o.tracebacks = false; continue; }
		if (a.rfind("--", 0) != 0) { throw usage_error("unexpected argument: " + a); }
		std::string flag = a.substr(2), value;
		if (auto eq = flag.find('='); eq != std::string::npos) {
			value = flag.substr(eq + 1);
			flag.resize(eq);
		} else {
			if (i + 1 == args.size()) { throw usage_error(a + " requires a value"); }
			value = args[++i];
		}
		if (flag == "config") { continue; }  // read above
		std::string key = flag;
		std::replace(key.begin(), key.end(), '-', '_');
		set_option(o, key, value, {});
	}
	if (o.tls_cert.empty() != o.tls_key.empty()) { throw usage_error("--tls-cert and --tls-key go together"); }
	if (o.runtime_dir.empty()) {
		const char* xdg = std::getenv("XDG_RUNTIME_DIR");
		o.runtime_dir = xdg && *xdg ? std::filesystem::path(xdg) / "prism"
		                            : std::filesystem::temp_directory_path() / ("prism-" + std::to_string(::geteuid()));
	}
	return o;
}

void configure_logging(const Options& o) {
	// The Kronuz/logger stack (it brings term-color + scheduler), wired the way Xapiand wires
	// it: a stderr sink (the log file when detached) and the backtrace hook on Kronuz/traceback.
	Logging::config.log_level = o.verbosity < 0 ? LOG_WARNING : LOG_NOTICE + o.verbosity;
	Logging::config.timestamp = LogTimestamp::iso8601;
	Logging::config.precision = LogPrecision::milliseconds;
	Logging::config.timestamp_gradient = true;
	Logging::config.tint = true;  // each message in its severity's color, as Xapiand's
	const std::string& color = o.color;
	if (color == "never" || color == "off") {
		Logging::config.color = LogColorMode::never;
	} else if (color == "always" || color == "on") {
		Logging::config.color = LogColorMode::always;
	} else if (color == "truecolor" || color == "24bit") {
		Logging::config.color = LogColorMode::always; Logging::config.color_depth = LogColorDepth::truecolor;
	} else if (color == "256" || color == "256color") {
		Logging::config.color = LogColorMode::always; Logging::config.color_depth = LogColorDepth::ansi256;
	} else if (color == "16" || color == "ansi") {
		Logging::config.color = LogColorMode::always; Logging::config.color_depth = LogColorDepth::ansi16;
	} else if (color == "stacked") {
		Logging::config.color = LogColorMode::always; Logging::config.color_depth = LogColorDepth::stacked;
	}  // else "auto": defaults (automatic mode + automatic depth)
	if (o.tracebacks) {
		Logging::hooks.backtrace = []() { return traceback::format(""); };
	}
	Logging::add_handler(std::make_unique<StderrLogger>());
}

// The HTTP component: start() binds and serves; stop() closes the listeners, and drain() waits for
// the requests in flight until its cancel token, then cancels them and runs http's stop (the
// grace, then the connections still open are forced).
struct Http {
	http::HttpAsioService& service;
	asio::io_context& io;
	unsigned short port;
	std::thread waiter;

	~Http() { if (waiter.joinable()) { waiter.join(); } }
	task::task<void> start(task::cancel_token) {
		service.start(port);
		co_return;
	}
	void stop() { service.stop_accepting(); }
	task::task<std::vector<std::string>> drain(const task::stop_bound& bound) {
		// wait_requests blocks, so it runs on its own thread and the loop keeps its wheel.
		co_await task::await_asio<void(asio::error_code)>([&](auto handler) {
			waiter = std::thread([this, token = bound.cancel, handler = std::move(handler)]() mutable {
				service.wait_requests(token);
				asio::post(io, [done = std::move(handler)]() mutable { std::move(done)(asio::error_code{}); });
			});
		});
		waiter.join();
		service.cancel_requests();
		service.stop();
		co_return std::vector<std::string>{};
	}
	void flush() {}
	std::string name() const { return "http"; }
};

// The descriptor step: one per connection plus the reserve. Raises the soft limit toward the hard
// one; short of the budget, an explicit max_clients is refused and the default shrinks to fit.
void check_descriptors(Options& o) {
	const std::size_t wanted = o.max_clients + kDescriptorReserve;
	const std::size_t have = reactor::raise_open_files(wanted);
	if (have >= wanted) { return; }
	if (o.max_clients_set) {
		throw config_error("max_clients " + std::to_string(o.max_clients) + " needs " + std::to_string(wanted) +
			" descriptors (with a reserve of " + std::to_string(kDescriptorReserve) + "); the limit is " + std::to_string(have));
	}
	if (have <= kDescriptorReserve) {
		throw config_error("the descriptor limit " + std::to_string(have) + " leaves no room for connections");
	}
	o.max_clients = have - kDescriptorReserve;
	L_WARNING("prism: descriptor limit {}: max_clients is {}", have, o.max_clients);
}

// The TLS the service presents: reactor's server context (key mode, validity and strict checked),
// none for plain HTTP.
http::Tls make_tls(const Options& o) {
	http::Tls tls;
	if (o.tls_cert.empty()) { return tls; }
#ifdef PRISM_TLS
	try {
		tls.context = reactor::tls::server_context(reactor::tls::Options{o.tls_cert.string(), o.tls_key.string(), ""});
	} catch (const std::exception& e) {
		throw config_error(std::string("TLS: ") + e.what());
	}
	if (reactor::tls::seconds_to_expiry(*tls.context) < reactor::tls::expiry_warning.count()) {
		L_WARNING("prism: the TLS certificate {} expires within 14 days", o.tls_cert.string());
	}
	return tls;
#else
	throw config_error("this prism was built without TLS (OpenSSL 3 was not found)");
#endif
}

// Everything after the fork: the pid lock, the signals, the log, then the lifecycle.
int serve(Options o, const task::daemon_ready& daemon_ready) {
	task::ProcessLock lock(o.runtime_dir / ("prism-" + std::to_string(o.port) + ".pid"), false);
	if (!lock.acquired()) {
		std::printf("prism: already running on port %u\n", unsigned(o.port));
		std::fflush(stdout);  // a detached child ends in _exit
		return 0;
	}
	task::stop_signals signals;  // the first thread, so none crosses the fork
	std::signal(SIGPIPE, SIG_IGN);  // a write to a reset peer fails with EPIPE instead
	configure_logging(o);
	struct LogSession { ~LogSession() { Logging::finish(); } } log_session;  // drains the LOG thread last

	check_descriptors(o);
	[[maybe_unused]] auto tls = make_tls(o);

	DemoApp app;
	// The request/response logging middleware, with prism's JSON reindenter as the body
	// prettifier (Xapiand injects a MsgPack/YAML one instead). prism lowers the levels from
	// the Xapiand-faithful defaults (DEBUG) to NOTICE so the rich blocks show out of the box,
	// with image previews at -v and above.
	http_log::Options logopts;
	logopts.request_level = LOG_NOTICE;
	logopts.level_2xx = LOG_NOTICE;
	logopts.image_level = LOG_NOTICE;
	logopts.prettify = [](std::string_view ct, std::string_view body) -> std::optional<std::string> {
		auto first = body.find_first_not_of(" \t\r\n");
		bool json = ct.find("json") != std::string_view::npos ||
			(first != std::string_view::npos && (body[first] == '{' || body[first] == '['));
		if (json) return pretty_json(body);
		return std::nullopt;
	};
	http_log::AccessLog access(app, std::move(logopts));

	// Every bound in one http::Limits, plain and TLS alike; the rest are http's defaults.
	const http::Limits limits{
		.max_clients = o.max_clients,
		.max_in_flight = o.max_in_flight,
		.idle_timeout = o.idle_timeout,
		.shutdown_grace = std::min(kShutdownGrace, o.shutdown_timeout),
	};
#ifdef PRISM_TLS
	http::HttpAsioService service(access, o.reactors, limits, std::move(tls));
#else
	http::HttpAsioService service(access, o.reactors, limits);
#endif
	http::AsioBindOptions bind;
	bind.address = o.bind_address;
	service.set_bind_options(bind);
	service.enable_compression();
	service.enable_conditional();
	service.enable_ranges();

	asio::io_context io(1);
	task::asio_scheduler loop(io.get_executor());
	Http http{service, io, o.port};
	const bool https = !o.tls_cert.empty();
	auto ready = [&] {
		daemon_ready();
		banner(service.port(), o.reactors, https);
		L_NOTICE(col::grey() + "ready -- Ctrl-C to stop" + col::reset());
	};
	const task::stop_timing timing{o.shutdown_timeout, std::min<std::chrono::nanoseconds>(kShutdownGrace, o.shutdown_timeout)};
	task::stopped result;
	task::spawn(task::scheduler_ref::of(loop), task::lifecycle(loop, signals, timing, ready, http),
		[&](std::exception_ptr e, std::optional<task::stopped> r) {
			result = r ? std::move(*r) : task::stopped{e};
			io.stop();
		});
	io.run();  // the lifecycle holds the loop's work guard until every flush returned

	if (!result.unfinished.empty()) {
		for (const auto& line : result.unfinished) { L_WARNING("prism: unfinished: {}", line); }
		Logging::finish();
		std::_Exit(EX_TEMPFAIL);
	}
	loop.close();
	loop.destroy_roots();
	service.destroy_roots();
	if (result.failure) {
		if (!result.ready) { std::rethrow_exception(result.failure); }  // reaches the waiting parent
		try { std::rethrow_exception(result.failure); } catch (const std::exception& e) { L_ERR("prism: {}", e.what()); }
		return 1;
	}
	L_NOTICE(col::grey() + "stopped" + col::reset());
	return 0;
}

}  // namespace

int main(int argc, char** argv) {
	for (int i = 1; i < argc; ++i) {
		if (std::string_view(argv[i]) == "--help" || std::string_view(argv[i]) == "-h") {
			std::fputs(kUsage, stdout);
			return 0;
		}
	}
	Options o;
	try {
		o = parse(argc, argv);
	} catch (const usage_error& e) {
		std::fprintf(stderr, "prism: %s\n%s", e.what(), kUsage);
		return EX_USAGE;
	} catch (const config_error& e) {
		std::fprintf(stderr, "prism: %s\n", e.what());
		return EX_CONFIG;
	}
	const auto log = o.runtime_dir / ("prism-" + std::to_string(o.port) + ".log");
	for (const auto& file : {o.tls_cert, o.tls_key}) {  // before the fork, so a detached start exits EX_CONFIG too
		if (!file.empty() && !std::ifstream(file)) {
			std::fprintf(stderr, "prism: TLS: cannot read %s\n", file.c_str());
			return EX_CONFIG;
		}
	}
	try {
		return task::run_daemon(o.daemonize, log, [&](const task::daemon_ready& ready) { return serve(o, ready); });
	} catch (const config_error& e) {
		std::fprintf(stderr, "prism: %s\n", e.what());
		return EX_CONFIG;
	} catch (const std::exception& e) {
		std::fprintf(stderr, "prism: %s\n", e.what());
		return 1;
	}
}
