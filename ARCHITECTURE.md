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
| Options | `parse()`: `--config FILE` reads `prism.json`, one object whose keys are the options; flags override it. One name per option, `--max-clients` and `max_clients`, durations in `_ms`. A bad value exits `EX_USAGE`, an unreadable file, certificate or key `EX_CONFIG`, checked before the fork so a detached start exits the same. No `--upstream-ca`: Prism dials nothing |
| Process | `task::run_daemon(--daemonize, RUNTIME_DIR/prism-PORT.log, serve)`; inside it `task::ProcessLock(RUNTIME_DIR/prism-PORT.pid)`, then the log. A second instance finds the lock held and exits 0 |
| Signals | `task::stop_signals`, the first thread after the fork: SIGINT or SIGTERM stops, a second hurries (it cancels the requests in flight at once) and a third flushes and dies. SIGPIPE is ignored |
| Limits and descriptors | The options fill one `http::Limits` (`max_clients`, `max_in_flight`, `idle_timeout`, `shutdown_grace`; the rest are http's defaults) that the `HttpAsioService` constructor takes whole, plain and TLS alike. `reactor::raise_open_files(max_clients + 64)` runs before any component: short of it an explicit `max_clients` exits `EX_CONFIG` and the default shrinks to fit, with a warning |
| TLS and privileges | `reactor::tls::server_context`, which refuses a key readable by group or others and a certificate not yet valid or expired and sets `strict`; a failure exits `EX_CONFIG`, and a certificate within 14 days of expiry is warned about. The context goes to the service as `http::Tls`. No privilege drop |
| Loop and components | An `asio_scheduler` on the main thread runs the lifecycle; the components are one, `Http` (start binds and serves, stop closes the listeners with `stop_accepting()`, drain waits for the requests in flight, then cancels the rest and runs http's stop). `DemoApp` and `AccessLog` are handlers with no lifecycle |
| `lifecycle`, ready | `task::lifecycle(loop, signals, {shutdown_timeout_ms, 5 s}, ready, http)`; `ready` reports to the waiting parent when detached, then prints the banner and one line |
| Housekeeping | None, and nothing polls: the loop waits on the lifecycle |
| Stop | The lifecycle: every `stop()`, then `Http`'s drain, then the flushes; 1 on a failure after ready. `Http`'s drain waits for the requests in flight until the bound less the grace (or the second signal), then cancels them and gives a response being written the grace (at most the bound), so it reports nothing left and the `EX_TEMPFAIL` path does not arise for Prism |

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

Where Prism's code path still differs from the note, and why (none of the limits, descriptor,
TLS and stop steps does):

| Deviation | Reason | Status |
| :---- | :---- | :---- |
| The configuration file only by `--config`, with no default directory | Prism installs nowhere, so it has no configuration directory | Stays |
| No privilege drop | Prism has no `--uid` or `--gid` | Stays |
| TLS compiled out | OpenSSL 3 is optional | Stays: a build option |

`src/test/lifecycle_test.cc` (`ctest`, test `prism_lifecycle`) runs the built binary: detached,
it serves before the parent returns, holds the pid lock against a second instance and releases it
at SIGTERM; in the foreground under load, a response being written at SIGTERM arrives whole, no
response is cut (a request after the stop is refused with a whole 503), and prism exits 0 within the
shutdown bound; a request whose body is still arriving at SIGTERM finishes and is answered whole
while new connections are refused, and a second SIGTERM cancels it; a stalled peer is cut at the
bound. The same test runs prism under a descriptor limit (an explicit `max_clients` that does not
fit exits `EX_CONFIG`, the default shrinks) and, with OpenSSL, refuses a group-readable key and
serves HTTPS with an owner-only one.
