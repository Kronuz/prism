# prism

Part of [Kronuz C++ Libraries](https://github.com/Kronuz/cpp-libraries), a collection of small, reusable C++ libraries. Read its story in [Everything but the Views](https://kronuz.github.io/blog/prism/), a Familiars post.

[![CI](https://github.com/Kronuz/prism/actions/workflows/ci.yml/badge.svg)](https://github.com/Kronuz/prism/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)

HTTP application server for C++20 on the Kronuz libraries: content negotiation, compression, colored logs, tracebacks.
It serves HTTP/1.1, or HTTPS with a certificate you give it, on standalone Asio
with one reactor per core, and wraps a plain `http::Router` of views with
everything around them. Reach for it as the starting point of a small C++ HTTP
service, or as the worked example of how the libraries fit together.

<p align="center"><img src="assets/prism.png" alt="the spectrum image prism serves at /image" width="96"></p>

## ✨ Highlights

- **A coroutine runtime.** HTTP/1.1 on standalone Asio with C++20 coroutines and
  one reactor per core; a handler suspends instead of blocking, so a slow one
  does not stall its neighbors.
- **Content negotiation.** One view answers JSON, HTML, or plain text from
  `Accept`.
- **Transparent compression.** zstd or gzip with `Vary: Accept-Encoding`, plus
  weak `ETag`s answered with 304 on `If-None-Match`, and byte ranges.
- **Colored, leveled logs** of every exchange: request line, headers, status,
  time, and a prettified body preview; images preview inline on iTerm2.
- **Tracebacks on error.** An unhandled exception answers 500 and logs a
  demangled stack trace.
- **HTTPS** from a certificate chain and key alone, when CMake finds OpenSSL 3.
- **A worked example.** One `src/main.cc` shows how
  [http](https://github.com/Kronuz/http), [reactor](https://github.com/Kronuz/reactor),
  [http-log](https://github.com/Kronuz/http-log), [logger](https://github.com/Kronuz/logger),
  [term-color](https://github.com/Kronuz/term-color), and
  [traceback](https://github.com/Kronuz/traceback) fit together.

## 🚀 Quick start

```sh
./build/prism --port 8880 -v
```

In another terminal:

```sh
curl -s localhost:8880/                                       # welcome and route list
curl -s localhost:8880/hello -H 'Accept: application/json'
curl -s localhost:8880/hello -H 'Accept: text/html'
curl -s --compressed localhost:8880/echo -d "$(head -c 4000 </dev/zero)"   # compressed
curl -s localhost:8880/json -d '{"a":[1,2],"b":{}}' -H 'Content-Type: application/json'
curl -s localhost:8880/image -o prism.png                     # previewed in the log
curl -s localhost:8880/boom                                   # 500 + traceback in the log
```

The options:

```text
prism [--config FILE] [--port N] [--bind-address ADDR] [--reactors N]
      [--max-clients N] [--max-in-flight N] [--idle-timeout-ms MS]
      [--shutdown-timeout-ms MS] [--tls-cert FILE --tls-key FILE]
      [--daemonize] [--runtime-dir DIR] [-q | -v | -vv ... | --verbosity N]
      [--color MODE | --no-color] [--no-tracebacks]

  --config FILE     prism.json: the same options as keys (max_clients, idle_timeout_ms, ...);
                    flags override it, and its relative paths are relative to the file
  --port N          port to listen on (default 8880; 0 binds a free port)
  --bind-address A  address to listen on (default every interface)
  --reactors N      event loops (default one per hardware thread)
  --max-clients N   open connections, plain and TLS (default 1024)
  --max-in-flight N requests admitted per loop (default 1024; 0 is no cap)
  --idle-timeout-ms MS          close an idle connection (default 75000; 0 is off)
  --shutdown-timeout-ms MS      bound on a stop (default 35000)
  --tls-cert FILE   serve HTTPS with this PEM certificate chain and the key below
  --tls-key FILE    the private key file (no certificate authority needed; built when OpenSSL 3 is found)
  --daemonize       detach once serving, logging to RUNTIME_DIR/prism-PORT.log
  --runtime-dir DIR where prism-PORT.pid is locked while prism runs
  -q                warnings and errors only
  -v, -vv, ...      one verbosity level above the default per v (requests log by default)
  --color MODE      auto (default), always, never, truecolor, 256, 16, or stacked
  --no-color        same as --color never; NO_COLOR is honored too
  --no-tracebacks   no stack trace on an unhandled error

SIGINT or SIGTERM stops: a response being written gets the grace (5 s, at most the
shutdown bound), then its connection closes; a request still being received is reset
with no response. A second signal changes nothing yet, and a third flushes and dies.
Finishing those requests and hurrying on the second signal come with http's
stop_accepting() (Detent quest prism-server-composition-limits).
```

## 📦 Installation

Requires a C++20 compiler and CMake 3.14 or newer; CMake fetches the Kronuz
libraries above at pinned commits, and builds HTTPS support when it finds
OpenSSL 3. CI builds it on Linux (x86_64, arm64), macOS (arm64, x86_64), and
FreeBSD, plus ASan, UBSan, and TSan builds.

prism is an executable, not a library target. Clone it and build:

```sh
git clone https://github.com/Kronuz/prism.git
cd prism
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

To make your own service, replace the routes of `DemoApp` in `src/main.cc`; the
splash, logging, negotiation, compression, and error handling stay.

## 🎯 When to use it

Use it as the starting point for a small C++ HTTP service, or as a map of how the
Kronuz HTTP libraries combine.

Not for:

- **A turnkey production server.** It has no config file; the options are the
  flags above.
- **HTTP/2.** It serves HTTP/1.1, and HTTPS over it.
- **Linking into another program.** CMake builds the `prism` executable only;
  reuse comes from copying `src/main.cc`.

## 📚 Documentation

- [Everything but the Views](https://kronuz.github.io/blog/prism/):
  the Familiars post on why it exists and how it works.
- [ARCHITECTURE.md](ARCHITECTURE.md): why it exists, each piece in detail, and how
  to build your service on it.
- [BENCHMARKS.md](BENCHMARKS.md): throughput, latency, and the cost of logging,
  measured on 2026-07-08 on an Apple M4 Pro.
- [AGENTS.md](AGENTS.md): repository map and conventions.
- [Kronuz C++ Libraries](https://github.com/Kronuz/cpp-libraries/blob/main/AGENTS.md#prism): the catalog entry.

## 🛠️ Building and testing

```sh
cmake -B build
cmake --build build
./build/prism
```

`ctest --test-dir build` runs prism's lifecycle test (readiness, the pid lock, and a
SIGTERM under load); the libraries it assembles carry their own suites, and CI builds
and tests prism on every platform.

## 🤝 Contributing

Issues and pull requests are welcome. Read [AGENTS.md](AGENTS.md) and the
catalog's [rules of engagement](https://github.com/Kronuz/cpp-libraries/blob/main/AGENTS.md#rules-of-engagement)
first; CI must pass on every platform.

## 📄 License

MIT, Copyright (c) 2026 Germán Méndez Bravo. See [LICENSE](LICENSE).
