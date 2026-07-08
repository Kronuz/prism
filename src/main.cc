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

// The request/response logging middleware is Kronuz/http-log (http_log::AccessLog);
// prism injects the JSON reindenter above as its body prettifier. See main().

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
			static const std::string png = cppcodec::base64_rfc4648::decode<std::string>(std::string_view(kPrismPngB64));
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
	// The request/response logging middleware, with prism's JSON reindenter as the
	// body prettifier (Xapiand injects a MsgPack/YAML one instead). prism lowers the
	// levels from the Xapiand-faithful defaults (DEBUG) so the rich blocks show at
	// -v, and previews images at -vv.
	http_log::Options logopts;
	logopts.request_level = LOG_INFO;
	logopts.level_2xx = LOG_INFO;
	logopts.image_level = LOG_INFO;
	logopts.prettify = [](std::string_view ct, std::string_view body) -> std::optional<std::string> {
		auto first = body.find_first_not_of(" \t\r\n");
		bool json = ct.find("json") != std::string_view::npos ||
			(first != std::string_view::npos && (body[first] == '{' || body[first] == '['));
		if (json) return pretty_json(body);
		return std::nullopt;
	};
	http_log::AccessLog access(app, std::move(logopts));

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
