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
  coroutines, a shared-nothing reactor pool (one per core), and coroutine handlers that
  suspend instead of block, so a slow handler can't stall its co-located connections.
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

## How `main` is put together

`main` follows the composition every Kronuz daemon shares (cpp-libraries
[docs/server-composition.md](https://github.com/Kronuz/cpp-libraries/blob/04a75c0/docs/server-composition.md),
Detent D122), step by step. This is Prism's row of the note's Mains table, with the code
in `src/main.cc`:

| Step | Prism |
| :---- | :---- |
| Options | `parse()`: `--config FILE` reads `prism.json`, one object whose keys are the options; flags override it. One name per option, `--max-clients` and `max_clients`, durations in `_ms`. A bad value exits `EX_USAGE`, an unreadable file or certificate `EX_CONFIG`. No `--upstream-ca`: Prism dials nothing |
| Process | `task::run_daemon(--daemonize, RUNTIME_DIR/prism-PORT.log, serve)`; inside it `task::ProcessLock(RUNTIME_DIR/prism-PORT.pid)`, then the log. A second instance finds the lock held and exits 0 |
| Signals | `task::stop_signals`, the first thread after the fork: SIGINT or SIGTERM stops, a second hurries, a third flushes and dies. SIGPIPE is ignored |
| Limits and descriptors | `max_in_flight` and `max_clients` go to the `HttpAsioService` constructor, one connection bound for plain and TLS; `idle_timeout_ms` and the grace by setter. No descriptor check |
| TLS and privileges | `reactor::tls::configure` with the Server role, then `http::tls_connection_factory(context, 5 s, 64, max_clients)`. No privilege drop |
| Loop and components | An `asio_scheduler` on the main thread runs the lifecycle; the components are one, `Http` (start binds and serves, drain runs http's stop). `DemoApp` and `AccessLog` are handlers with no lifecycle |
| `lifecycle`, ready | `task::lifecycle(loop, signals, {shutdown_timeout_ms, 5 s}, ready, http)`; `ready` reports to the waiting parent when detached, then prints the banner and one line |
| Housekeeping | None, and nothing polls: the loop waits on the lifecycle |
| Stop | The lifecycle: every `stop()`, then `Http`'s drain, then the flushes; `EX_TEMPFAIL` when a drain leaves work, 1 on a failure after ready |

The options and their defaults:

| Flag, key | Default |
| :---- | :---- |
| `--port`, `port` | 8880 (0 binds a free port) |
| `--bind-address`, `bind_address` | every interface |
| `--reactors`, `reactors` | hardware threads |
| `--max-clients`, `max_clients` | 1024 |
| `--max-in-flight`, `max_in_flight` | 1024 per reactor; 0 is no cap |
| `--idle-timeout-ms`, `idle_timeout_ms` | 75000; 0 is off |
| `--shutdown-timeout-ms`, `shutdown_timeout_ms` | 35000 |
| `--tls-cert`, `--tls-key`, `tls_cert`, `tls_key` | unset: plain HTTP. The key is the private file, named by path |
| `--daemonize`, `daemonize` | foreground |
| `--runtime-dir`, `runtime_dir` | `$XDG_RUNTIME_DIR/prism`, else `$TMPDIR/prism-UID` |
| `-q`, `-v`..., `--verbosity`, `verbosity` | requests at NOTICE |
| `--color`, `--no-color`, `color`; `--no-tracebacks`, `tracebacks` | auto; tracebacks on |

Where Prism's code path still differs from the note, and why:

| Deviation | Reason | Status |
| :---- | :---- | :---- |
| Bounds set by the constructor and setters, not one `http::Limits` | http has no `Limits` yet (Xapiand quest http-limits) | Goes in Detent quest prism-server-composition-limits, after http-limits |
| No descriptor check | reactor has no `raise_open_files` yet (Xapiand quest reactor-server-composition); the reserve will be 64 | Goes in Detent quest prism-server-composition-limits, after reactor-server-composition |
| `configure` instead of `tls::server_context`, so a group-readable key, an expired certificate and `strict` are not checked | reactor has no `server_context` yet (reactor-server-composition) | Goes in Detent quest prism-server-composition-limits, after reactor-server-composition |
| `Http::stop()` closes nothing, and the drain runs http's stop, which cancels the requests in flight at once (their responses are still written) and blocks the loop for the grace | http has no `stop_accepting()` and reactor no stop phases yet; the grace is the smaller of 5 s and the bound, so the bound holds | Goes in Detent quest prism-server-composition-limits, after http-limits |
| The configuration file only by `--config`, with no default directory | Prism installs nowhere, so it has no configuration directory | Stays |
| No privilege drop | Prism has no `--uid` or `--gid` | Stays |
| TLS compiled out | OpenSSL 3 is optional | Stays: a build option |

`src/test/lifecycle_test.cc` (`ctest`, test `prism_lifecycle`) runs the built binary: detached,
it serves before the parent returns, holds the pid lock against a second instance and releases it
at SIGTERM; in the foreground under load, a response being written at SIGTERM arrives whole, no
response is cut, and prism exits 0 within the shutdown bound.
