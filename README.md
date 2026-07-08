# prism

A flashy HTTP application server built on the Kronuz libraries. It is the part of a
service that isn't your domain: the request runtime, content negotiation,
transparent compression, conditional and range requests, a startup splash, and
colored, leveled request/response logging with a formatted traceback when a handler
throws. You bring the views; prism brings everything around them.

It is two things at once: a **base to build an HTTP service on**, and a **worked
example** of how the extracted Kronuz libraries fit together. The demo you get out
of the box is small on purpose, so the wiring stays legible.

## What you get

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

## Build and run

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/prism 8880 -v
```

The dependencies are fetched at configure time via CMake `FetchContent`; you need a
C++20 compiler and CMake 3.14+.

```
Usage: prism [PORT] [-v|-vv|-q] [--no-tracebacks]

  PORT              port to listen on (default 8880)
  -v                debug logging (request/response headers + body preview)
  -vv               trace logging
  -q                quiet (warnings and errors only)
  --color=<mode>    auto (default) | always | never | truecolor | 256 | 16 | stacked
  --no-color        alias for --color=never
  --no-tracebacks   don't print a stack trace on an unhandled error
```

## The demo routes

```sh
curl -s localhost:8880/                                  # welcome + route list
curl -s localhost:8880/hello -H 'Accept: application/json'
curl -s localhost:8880/hello -H 'Accept: text/html'
curl -s --compressed localhost:8880/echo -d "$(head -c 4000 </dev/zero)"  # gzip
curl -s localhost:8880/echo -XPOST -d 'hi'               # echoes the request
curl -s localhost:8880/json -XPOST -d '{"a":[1,2],"b":{}}' -H 'Content-Type: application/json'  # prettified in the log
curl -s localhost:8880/image -o /tmp/prism.png           # a PNG (previewed in the log)
curl -s localhost:8880/boom                              # 500 + traceback in the log
```

Run it under iTerm2 with `-v` and hit `/image` to see the response previewed inline
in the log.

## How you'd build a service on it

The application is a plain `http::Router` of views (`DemoApp` in `src/main.cc`). To
make your own service you replace those routes with yours; the splash, logging,
negotiation, compression, and error handling come along unchanged. The framework
pieces here (the splash, the access-logging middleware, the lifecycle in `main`) are
the natural next things to lift into their own libraries so the glue shrinks to just
your views.
