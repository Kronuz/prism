# prism architecture

prism is the outer shell of an HTTP service, assembled from the Kronuz libraries.
This file covers why it exists, what each piece does, and how to build on it.

## Why it exists

Every service re-implements this same outer shell: parse a port, stand up a server,
negotiate encodings, log what came in and what went out, and turn a thrown exception
into a response instead of a crash. Xapiand had a good version of that shell, but it
was entangled with search and storage. prism is that shell pulled out clean, sitting
on libraries you can use one at a time: [http](https://github.com/Kronuz/http) (the
router, negotiation, compression, conditional/range handling) over
[reactor](https://github.com/Kronuz/reactor) (the Asio runtime),
[http-parser](https://github.com/Kronuz/http-parser),
[radix-router](https://github.com/Kronuz/radix-router),
[compressors](https://github.com/Kronuz/compressors),
[logger](https://github.com/Kronuz/logger) +
[term-color](https://github.com/Kronuz/term-color) (leveled, colored logging), and
[traceback](https://github.com/Kronuz/traceback). It uses them the way Xapiand does,
not a parallel re-implementation. The point of assembling them here is that the seams
are visible: you can read one file and see how a request becomes a logged, negotiated,
compressed response.

## The pieces in detail

- **A real request runtime, not a toy loop.** HTTP/1.1 on standalone Asio with C++20
  coroutines, a shared-nothing reactor pool (one per core), and an off-reactor
  worker pool so a slow handler can't stall its co-located connections.
- **Content negotiation.** One `/hello` view answers `application/json`, `text/html`,
  or `text/plain` off the client's `Accept`.
- **Transparent compression.** Responses over the size threshold are gzip/deflate
  compressed when the client advertises it, with a correct `Vary: Accept-Encoding`.
  Conditional (`ETag`/`If-*`) and range requests are on too.
- **A banner on boot.** A glass-prism icon and the "prism" wordmark in figlet's
  Standard font (the same font as Xapiand's "apiand"), the word split into a
  spectrum, with the "p" and "m" hand-stylized into descenders and the tagline
  ("the flashy app server") nestled inside the "m" descender, plus the address
  it's listening on. It prints through the logger sink at NOTICE, gated by the log
  level, the way Xapiand's `banner()` does.
- **Colored, leveled logging of every exchange.** The request line and headers, the
  response status, wall time, content type, and a body preview, each at its own
  verbosity. JSON request and response bodies are prettified in the log; on iTerm2,
  an image response is previewed inline. This runs on
  [logger](https://github.com/Kronuz/logger) and
  [term-color](https://github.com/Kronuz/term-color) (the same logging and color
  stack Xapiand uses), so `--color`, `NO_COLOR`, and the tty check are handled for
  you.
- **A traceback when a handler throws.** An unhandled exception becomes a `500` and,
  when enabled, a demangled, symbolized stack trace in the log, instead of a silent
  drop. The trace is [traceback](https://github.com/Kronuz/traceback), wired into
  the logger's backtrace hook exactly as Xapiand wires it.

## Building a service on it

The application is a plain `http::Router` of views (`DemoApp` in `src/main.cc`). To
make your own service you replace those routes with yours; the splash, logging,
negotiation, compression, and error handling come along unchanged. The framework
pieces here (the splash, the access-logging middleware, the lifecycle in `main`) are
the natural next things to lift into their own libraries so the glue shrinks to just
your views.
