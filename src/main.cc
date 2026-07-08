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
//   ./prism 8880 -vv
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
#include <atomic>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>

#include "http_handler.h"
#include "http_router.h"
#include "http_asio.h"
#include "traceback.h"
#include "colors.h"         // term-color: rgb()/brgb()/CLEAR_COLOR (stacked escapes)
#include "color_tools.hh"   // term-color: the runtime color class
#include "logger.h"         // Kronuz/logger: Logging, LogConfig, the L_* macros

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
// base64 (used for the iTerm2 image escape).
// ---------------------------------------------------------------------------
static std::string b64encode(std::string_view in) {
	static const char* T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	std::string out;
	out.reserve((in.size() + 2) / 3 * 4);
	std::size_t i = 0;
	for (; i + 2 < in.size(); i += 3) {
		unsigned n = (unsigned char)in[i] << 16 | (unsigned char)in[i + 1] << 8 | (unsigned char)in[i + 2];
		out += T[n >> 18 & 63]; out += T[n >> 12 & 63]; out += T[n >> 6 & 63]; out += T[n & 63];
	}
	if (i < in.size()) {
		unsigned n = (unsigned char)in[i] << 16;
		if (i + 1 < in.size()) n |= (unsigned char)in[i + 1] << 8;
		out += T[n >> 18 & 63]; out += T[n >> 12 & 63];
		out += (i + 1 < in.size()) ? T[n >> 6 & 63] : '=';
		out += '=';
	}
	return out;
}
static std::string b64decode(std::string_view in) {
	auto val = [](char c) -> int {
		if (c >= 'A' && c <= 'Z') return c - 'A';
		if (c >= 'a' && c <= 'z') return c - 'a' + 26;
		if (c >= '0' && c <= '9') return c - '0' + 52;
		if (c == '+') return 62;
		if (c == '/') return 63;
		return -1;
	};
	std::string out;
	int buf = 0, bits = 0;
	for (char c : in) {
		int v = val(c);
		if (v < 0) continue;
		buf = buf << 6 | v;
		bits += 6;
		if (bits >= 8) { bits -= 8; out += char((buf >> bits) & 0xFF); }
	}
	return out;
}

// A 48x48 spectrum PNG, embedded so /image needs no asset at runtime.
static const char* kPrismPngB64 =
	"iVBORw0KGgoAAAANSUhEUgAAADAAAAAwCAIAAADYYG7QAAABAElEQVR42s3O4WACAAAF4aN5AiNIIIIEIhhBAhEkEEECESQwggje3jLYvxO4+2jSr/SQHtNTek6/00t6TW/pPX2kz/SV/qTvtHk3P82reTaP5t7cmmtzab6bc3Nqjs2h+fqr/2+ASrMBKs0GqDQboNJsgEqzASrNBqg0G6DSbIBKswEqzQaoNBug0myASrMBKs0GqDQboNJsgEqzOirN6qg0q6PSrI5KszoqzeqoNKuj0qyOSrM6Ks3qqDSro9KsjkqzOirN6qg0q6PSvD8gkaYfkEizASrNBqg0G6DSbIBKswEqzQaoNBug0myASrMBKs0GqDQboNJsgEqzASrNBr8MX5RP/5PNSQAAAABJRU5ErkJggg==";

// ---------------------------------------------------------------------------
// Splash — an RGB spectrum banner, printed undecorated through the logger sink
// (so it is color-resolved by the same --color / NO_COLOR policy as the log).
// (Phase 2: extract as Kronuz/splash, data driven by name/version/subtitle.)
// ---------------------------------------------------------------------------
static void splash(unsigned port, std::size_t reactors) {
	const char* name = "prism";
	static const int stops[][3] = {
		{233, 30, 99}, {255, 152, 0}, {255, 235, 59}, {76, 175, 80}, {33, 150, 243}, {103, 58, 183},
	};
	// The wordmark in a rainbow sweep (bold rides inside each color).
	std::string wordmark;
	for (std::size_t i = 0; name[i]; ++i) {
		const auto& c = stops[i % 6];
		wordmark += col::bfg(c[0], c[1], c[2]) + std::string(1, name[i]);
	}
	wordmark += col::reset();
	// A little prism triangle splitting a white beam into the spectrum.
	std::string b = "\n";
	b += "  " + col::bfg(230, 230, 230) + "/\\" + col::reset() + "\n";
	b += "  " + col::bfg(210, 210, 210) + "/  \\" + col::reset() + "   " + wordmark + "\n";
	b += " " + col::fg(190, 190, 190) + "/____\\" + col::reset() + "  " +
		col::grey() + "a flashy HTTP application server" + col::reset() + "\n\n";
	b += "  " + col::fg(76, 175, 80) + "listening" + col::reset() + " http://127.0.0.1:" +
		std::to_string(port) + "/   " + col::grey() + "(" + std::to_string(reactors) + " reactors)" + col::reset();
	L_PRINT(b);
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

// A body preview for the log: JSON is prettified, other text is shown
// (truncated), binary is summarized.
static std::string preview_body(std::string_view ct, std::string_view body, std::size_t limit = 512) {
	if (body.empty()) return col::grey() + "(empty)" + col::reset();
	bool binary = ct.substr(0, 6) == "image/" || ct.find("octet-stream") != std::string_view::npos;
	if (binary) return col::grey() + "(" + std::to_string(body.size()) + " bytes, " + std::string(ct) + ")" + col::reset();
	std::string text;
	auto first = body.find_first_not_of(" \t\r\n");
	bool looks_json = ct.find("json") != std::string_view::npos ||
		(first != std::string_view::npos && (body[first] == '{' || body[first] == '['));
	if (looks_json) {
		std::string pretty = pretty_json(body);
		// Re-indent multi-line JSON so continuation lines align under the log's
		// 4-space body gutter.
		std::string indented;
		for (char c : pretty) { indented += c; if (c == '\n') indented += "    "; }
		text = std::move(indented);
	} else {
		text = std::string(body);
	}
	std::string s = text.substr(0, limit);
	if (text.size() > limit) s += col::grey() + " ...(+" + std::to_string(text.size() - limit) + " bytes)" + col::reset();
	return s;
}

// ---------------------------------------------------------------------------
// AccessLog — an HttpHandler decorator that logs each request and response, maps
// an unhandled exception to a 500 with a traceback, and previews image responses
// inline on iTerm2. (Phase 2: extract as Kronuz/access-log.)
// ---------------------------------------------------------------------------
class CapturingWriter : public http::ResponseWriter {
	http::ResponseWriter& real_;
	static bool iequal(std::string_view a, std::string_view b) {
		if (a.size() != b.size()) return false;
		for (std::size_t i = 0; i < a.size(); ++i)
			if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i])) return false;
		return true;
	}
public:
	int code = 200;
	std::string content_type = "text/plain";
	std::string body;
	bool started = false;
	explicit CapturingWriter(http::ResponseWriter& r) : real_(r) {}
	void status(int c) override { code = c; started = true; real_.status(c); }
	void set_header(std::string_view n, std::string_view v) override {
		if (iequal(n, "Content-Type")) content_type = std::string(v);
		real_.set_header(n, v);
	}
	void write(std::string_view c) override { started = true; body.append(c); real_.write(c); }
	void end() override { real_.end(); }
	void set_close() override { real_.set_close(); }
};

class AccessLog : public http::HttpHandler {
	http::HttpHandler& inner_;
public:
	explicit AccessLog(http::HttpHandler& inner) : inner_(inner) {}

	void handle(const http::Request& req, http::ResponseWriter& resp) override {
		auto t0 = std::chrono::steady_clock::now();
		L_NOTICE(col::bfg(120, 144, 156) + "-> " + col::reset() +
			col::fg(3, 169, 244) + req.method + col::reset() + " " + req.path);
		if (Logging::config.log_level >= LOG_INFO) {
			for (auto& [k, v] : req.headers)
				L_INFO("    " + col::grey() + k + ":" + col::reset() + " " + v);
			if (!req.body.empty())
				L_INFO("    " + preview_body(req.content_type(), req.body));
		}

		CapturingWriter cap(resp);
		try {
			inner_.handle(req, cap);
		} catch (...) {
			// L_EXC logs the in-flight exception; its description and (when enabled)
			// the backtrace come from the logger's hooks, which main() points at
			// Kronuz/traceback. Then finish the response and log the 500.
			L_EXC(std::string("unhandled exception in ") + req.method + " " + req.path);
			if (!cap.started) resp.send(500, "Internal Server Error\n");
			L_ERR(col::bfg(244, 67, 54) + "<- 500" + col::reset() + " (exception)");
			return;
		}

		double ms = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count() / 1000.0;
		int c = cap.code;
		std::string sc = c < 300 ? col::bfg(76, 175, 80)
			: c < 400 ? col::bfg(3, 169, 244)
			: c < 500 ? col::bfg(255, 152, 0)
			: col::bfg(244, 67, 54);
		char timing[32];
		std::snprintf(timing, sizeof timing, "%.2fms", ms);
		L_NOTICE(sc + "<- " + std::to_string(c) + col::reset() + " " +
			col::grey() + timing + col::reset() + "  " + cap.content_type);
		if (Logging::config.log_level >= LOG_INFO) {
			if (Logging::iterm2_available() && cap.content_type.substr(0, 6) == "image/" && !cap.body.empty()) {
				L_INFO("    \033]1337;File=inline=1;height=6:" + b64encode(cap.body) + "\a");
			} else {
				L_INFO("    " + preview_body(cap.content_type, cap.body));
			}
		}
	}
};

// ---------------------------------------------------------------------------
// The application: a plain Router of views. Everything above is the framework.
// ---------------------------------------------------------------------------
class DemoApp : public http::HttpHandler {
	http::Router router_;
public:
	DemoApp() {
		router_.route("GET", "/", [](const http::Request&, http::ResponseWriter& resp, const http::Params&) {
			resp.send(200,
				"prism -- a flashy HTTP application server\n\n"
				"  GET  /        this page\n"
				"  GET  /hello   content-negotiated greeting (Accept: text/html | application/json | text/plain)\n"
				"  ANY  /echo    echoes your request\n"
				"  ANY  /json    echoes a JSON body (prettified in the verbose log)\n"
				"  GET  /image   a spectrum PNG (previewed inline in the log on iTerm2)\n"
				"  GET  /boom    raises an error -> 500 + traceback in the log\n");
		});
		router_.route("GET", "/hello", [](const http::Request& req, http::ResponseWriter& resp, const http::Params&) {
			auto accept = req.header("Accept");
			if (accept.find("application/json") != std::string_view::npos)
				resp.send(200, "{\"hello\":\"world\",\"from\":\"prism\"}\n", "application/json");
			else if (accept.find("text/html") != std::string_view::npos)
				resp.send(200, "<h1>hello from <em>prism</em></h1>\n", "text/html; charset=utf-8");
			else
				resp.send(200, "hello from prism\n");
		});
		auto echo = [](const http::Request& req, http::ResponseWriter& resp, const http::Params&) {
			std::string out = req.method + " " + req.path + "\n";
			for (auto& [k, v] : req.headers) { out += k; out += ": "; out += v; out += "\n"; }
			if (!req.body.empty()) { out += "\n"; out += req.body; out += "\n"; }
			resp.send(200, out);
		};
		router_.route("GET", "/echo", echo);
		router_.route("POST", "/echo", echo);
		router_.route("PUT", "/echo", echo);
		auto json_echo = [](const http::Request& req, http::ResponseWriter& resp, const http::Params&) {
			std::string body = req.body.empty() ? "{\"posted\":null,\"from\":\"prism\"}" : req.body;
			resp.send(200, body, "application/json");
		};
		router_.route("POST", "/json", json_echo);
		router_.route("GET", "/json", json_echo);
		router_.route("GET", "/image", [](const http::Request&, http::ResponseWriter& resp, const http::Params&) {
			static const std::string png = b64decode(kPrismPngB64);
			resp.send(200, png, "image/png");
		});
		router_.route("GET", "/boom", [](const http::Request&, http::ResponseWriter&, const http::Params&) {
			throw std::runtime_error("the /boom route always raises");
		});
	}
	void handle(const http::Request& req, http::ResponseWriter& resp) override { router_.handle(req, resp); }
};

// ---------------------------------------------------------------------------
static std::atomic<bool> g_stop{false};
static void on_signal(int) { g_stop.store(true); }

int main(int argc, char** argv) {
	unsigned port = 8880;
	int log_level = LOG_NOTICE;   // default: request/response lines
	bool tracebacks = true;
	std::string color = "auto";
	for (int i = 1; i < argc; ++i) {
		std::string a = argv[i];
		if (a == "-v") log_level = LOG_INFO;             // + headers + body previews
		else if (a == "-vv") log_level = LOG_DEBUG;      // + any debug-compiled lines
		else if (a == "-q") log_level = LOG_WARNING;     // warnings + errors only
		else if (a == "--no-tracebacks") tracebacks = false;
		else if (a == "--no-color") color = "never";
		else if (a.rfind("--color=", 0) == 0) color = a.substr(8);
		else if (!a.empty() && a[0] != '-') port = (unsigned)std::atoi(a.c_str());
	}
	std::size_t reactors = std::max(1u, std::thread::hardware_concurrency());

	// Logging: the Kronuz/logger stack (it brings term-color + scheduler), wired
	// the way Xapiand wires it -- config from the CLI, a stderr sink, and the
	// backtrace hook pointed at Kronuz/traceback.
	Logging::config.log_level = log_level;
	Logging::config.timestamp = LogTimestamp::iso8601;
	Logging::config.precision = LogPrecision::milliseconds;
	Logging::config.timestamp_gradient = true;
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
	if (tracebacks) {
		Logging::hooks.backtrace = []() { return traceback::format(""); };
	}
	Logging::add_handler(std::make_unique<StderrLogger>());

	DemoApp app;
	AccessLog access(app);

	http::HttpAsioService service(access, reactors, /*workers=*/2, /*queue_limit=*/256);
	service.enable_compression();
	service.enable_conditional();
	service.enable_ranges();

	std::signal(SIGINT, on_signal);
	std::signal(SIGTERM, on_signal);

	service.start(static_cast<unsigned short>(port));
	splash(port, reactors);
	L_NOTICE(col::grey() + "ready -- Ctrl-C to stop" + col::reset());

	while (!g_stop.load()) std::this_thread::sleep_for(std::chrono::milliseconds(100));

	L_NOTICE(col::grey() + "shutting down..." + col::reset());
	service.stop();
	Logging::finish();   // drain the LOG thread before statics tear down
	return 0;
}
