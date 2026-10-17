# prism

[![CI](https://github.com/Kronuz/prism/actions/workflows/ci.yml/badge.svg)](https://github.com/Kronuz/prism/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)

The flashy app server: everything around your HTTP views, in C++20. Content
negotiation, compression, conditional and range requests, colored request logs, and a
traceback when a handler throws. You write the routes.

<p align="center"><img src="assets/prism.png" alt="the spectrum image prism serves at /image" width="96"></p>

## ✨ Highlights

- ⚡ **A real runtime**: HTTP/1.1 on standalone Asio with C++20 coroutines, one
  reactor per core, and a worker pool so a slow handler can't stall its neighbors.
  About 51k req/s at 5 ms p99 on one M4 Pro, limited by the co-located load
  generator ([BENCHMARKS.md](BENCHMARKS.md)).
- 🤝 **Content negotiation**: one view answers JSON, HTML, or plain text from `Accept`.
- 🗜️ **Transparent compression**: gzip or deflate with a correct
  `Vary: Accept-Encoding`, plus `ETag`/`If-*` and range requests.
- 🌈 **Colored, leveled logs** of every exchange: request line, headers, status, time,
  and a prettified body preview; images preview inline on iTerm2.
- 🧯 **Tracebacks, not silent 500s**: an unhandled exception logs a demangled stack
  trace.
- 🧩 **A worked example**: one `main.cc` shows how [http](https://github.com/Kronuz/http),
  [reactor](https://github.com/Kronuz/reactor),
  [logger](https://github.com/Kronuz/logger),
  [term-color](https://github.com/Kronuz/term-color), and
  [traceback](https://github.com/Kronuz/traceback) fit together.

## 📦 Installation

Requires a C++20 compiler and CMake 3.14 or newer; the dependencies are fetched at
configure time.

```sh
git clone https://github.com/Kronuz/prism.git && cd prism
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build
```

## 🚀 Quick start

```sh
./build/prism 8880 -v
```

In another terminal:

```sh
curl -s localhost:8880/                                       # welcome and route list
curl -s localhost:8880/hello -H 'Accept: application/json'
curl -s localhost:8880/hello -H 'Accept: text/html'
curl -s --compressed localhost:8880/echo -d "$(head -c 4000 </dev/zero)"   # gzip
curl -s localhost:8880/json -d '{"a":[1,2],"b":{}}' -H 'Content-Type: application/json'
curl -s localhost:8880/image -o prism.png                     # previewed in the log
curl -s localhost:8880/boom                                   # 500 + traceback in the log
```

## ⚙️ Configuration

```
prism [PORT] [-q | -v | -vv ...] [--color=MODE | --no-color] [--no-tracebacks]
      [--tls-cert=FILE --tls-key=FILE]

  PORT              port to listen on (default 8880)
  -q                warnings and errors only
  -v, -vv, ...      one verbosity level above the default per v (requests log by default)
  --color=MODE      auto (default), always, never, truecolor, 256, 16, or stacked
  --no-color        same as --color=never; NO_COLOR is honored too
  --no-tracebacks   no stack trace on an unhandled error
```

## 🎯 When to use it

- ✅ A starting point for a small C++ HTTP service, or a map of how the Kronuz HTTP
  libraries combine.
- ❌ A turnkey production server: it serves plain HTTP/1.1 with no config file.
  [http](https://github.com/Kronuz/http) has TLS, but prism doesn't wire it.

## 📚 Documentation

- [ARCHITECTURE.md](ARCHITECTURE.md): why it exists, each piece in detail, and how
  to build your service on it.
- [BENCHMARKS.md](BENCHMARKS.md): throughput, latency, and the cost of logging.
- [AGENTS.md](AGENTS.md): repository map and conventions.

## 🤝 Contributing

Issues and pull requests are welcome. Read [AGENTS.md](AGENTS.md) first. CI must
pass on every platform.

## 📄 License

MIT, Copyright (c) 2026 Germán Méndez Bravo. See [LICENSE](LICENSE).
