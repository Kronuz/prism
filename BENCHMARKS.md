# prism — Benchmarks

**Author:** [Germán Méndez Bravo](mailto:german.mb@gmail.com)
**Date:** 2026-07-08
**Machine:** Apple M4 Pro, 14 cores, macOS 26.5.1
**Build:** Release (`-O2`); load with `wrk` 4.x and `ab`, client co-located, over loopback (`127.0.0.1`)

## TL;DR

prism sustains **~51k req/s** for a small keep-alive response on one Apple M4 Pro, at **~5 ms p99**, with **zero failed requests** over millions served. That number is the *test harness* talking, not the server: prism sits at **~2 of 14 cores** the whole time, HTTP pipelining nearly doubles it to **~94k**, and a single-threaded Node reference on the same loopback also caps around **59k**. So prism's real ceiling is well past 60k and can't be read off a co-located loopback client. To measure the actual limit you need Linux with the load generator on a *separate* host.

Two findings came out of the run:

1. The request/response logging cost is small in throughput terms (**~5%**) but real in CPU (**+40%**), because prism isn't CPU-bound to begin with.
2. The logging middleware was copying every response body and headers a second time, per request, even when nothing was logged. That is now a **zero-copy readback**; Xapiand's logged search service gets the same win for free.

## Method

Single host, loopback, client and server sharing the same 14 cores. `wrk` for latency distributions and thread scaling, `ab` as a second opinion, a one-file Node server as a "how fast can this harness even go" control. Request logging is turned **off** (`prism -q`) for the throughput runs unless stated, so the logger doesn't dominate.

The response bodies are deliberately small, to measure the request/response cycle rather than the payload:

| Route | Bytes | Content-Type |
| --- | --- | --- |
| `/hello` | 17 | `text/plain` |
| `/` | 401 | `text/plain` |
| `/json` | 30 | `application/json` |
| `/image` | 313 | `image/png` |

**Caveat up front.** This is a co-located loopback benchmark. There is no network, no TLS, no real handler work (no database, no disk), and the load generator competes with the server for the same cores. Treat the absolute numbers as a floor set by the harness, and the *shape* of the results (where the cores go, how it scales) as the real signal.

## Throughput and latency (`/hello`, logging off)

Peak, `wrk -t6 -c256`, 15 s:

| Metric | Value |
| --- | --- |
| Requests/sec | **50,622** |
| p50 | 4.92 ms |
| p90 | 5.20 ms |
| p99 | 5.66 ms |
| Transfer | 8.26 MB/s |
| Failed | 0 |

`ab` agrees: 52,747 req/s, 0 failed, 50,000 requests at `-c100 -k`.

### It saturates at *low* concurrency

Throughput is flat from 16 to 1024 connections; only the latency grows (Little's Law: latency ≈ concurrency / throughput).

| Connections | RPS | p50 | p99 |
| --- | --- | --- | --- |
| 16 | 51,935 | 227 µs | 282 µs |
| 64 | 51,827 | 1.15 ms | 1.32 ms |
| 256 | 50,568 | 4.91 ms | 5.73 ms |
| 1024 | 51,218 | 19.31 ms | 25.92 ms |

A server that hits its ceiling at 16 connections and doesn't move for the next 1008 is not concurrency-starved. Something serializes it at ~51k.

## The ceiling is the harness, not prism

The tell is CPU. Under full load prism draws only **~210% CPU — about 2.1 of 14 cores** — and `ps -M` shows all 14 reactor threads *evenly* busy at ~13% each. So connections distribute fine; every reactor just sits mostly idle. The machine is 85% unused at the "ceiling."

Ruling things out, one at a time:

- **`wrk` isn't the limit.** Threads 2 → 10 all land at ~51k; two independent `wrk` processes sum to 25k + 25k = 50k, the same wall.
- **Nagle isn't it.** `TCP_NODELAY` is set on accepted sockets, and the response is written head+body in one scatter-gather `async_write`.
- **A single-thread reference goes faster.** A one-file Node server on the same loopback does **59,203 req/s at 100% CPU (one core)**. A single saturated core beating prism's fourteen idle reactors means the wall is the loopback ping-pong, not prism's capacity.
- **Pipelining proves the headroom.** With 32 requests batched per write, prism jumps to **94,129 req/s** (CPU rises to ~4 cores). Push harder and it plateaus around 94k with *both* `wrk` (~1 core) and prism (~4 cores) still idle — that plateau is macOS `lo0` plus the co-located client, not the server.

**Reading:** the ~51k non-pipelined figure is the round-trip ceiling of a keep-alive connection with one request in flight at a time, over macOS loopback, against a client fighting for the same cores. prism's processing capacity is at least 94k on this box with 10 cores to spare. The honest number for "how fast in production" is *not measurable here*; it needs Linux and an off-box client.

## What logging costs

Same `/hello` peak, request + response blocks logged at NOTICE:

| Logging | RPS | vs off | CPU |
| --- | --- | --- | --- |
| off (`-q`) | 50,622 | — | 210% |
| on → `/dev/null` | 48,047 | −5% | 292% |
| on → file (disk) | 48,653 | −4% | (112 MB / 3.8M lines in ~8 s) |

The throughput hit is small because prism has idle cores to absorb the logger's formatting; the CPU cost (**+40%, ~0.8 core**) is where it actually shows. The async logger keeps up with a disk sink at this rate without shedding lines.

## A real fix found on the way: zero-copy response logging

The `http-log` middleware (`AccessLog`, used by prism *and* Xapiand) wrapped every response in a `CapturingWriter` that copied the headers and body **a second time, on every request** — even at `-q`, where the copy is then thrown away unlogged. The underlying `AsioResponseWriter` already buffers the full response (it must, to set `Content-Length`), and that buffer is still alive when the middleware logs.

So the copy was redundant. The fix gives the writer a small `BufferedResponse` capability (`response_code/started/headers/body/content_type`) and has `AccessLog` read the response **back by reference** after `handle()` instead of wrapping and copying. The old `CapturingWriter` stays only as a fallback for a writer that doesn't retain its response.

Verified: request, response, and exception blocks are byte-identical; large bodies still render as a `<body N>` summary; the error path (`on_error` → 500 fallback → `L_EXC` at CRIT on a genuine 5xx) is unchanged. With a 63 KB response body, logging-on throughput is now within ~2.5% of logging-off (19,077 vs 19,565 req/s) — the per-request copy is gone. Xapiand's logged `SearchService` inherits the same win.

## Is it any good for production?

**What the run establishes.** prism is correct and stable under sustained load: over a million requests per run with zero failures, flat latency, no leaks (RSS steady at ~37 MB). Its processing capacity on a single machine is comfortably past 60k (94k pipelined) with most of the cores idle, so headroom is not the concern.

**What it does not establish.** No TLS, no HTTP/2, no real handler work, no network. The absolute req/s is a harness floor. Anyone quoting "51k" as prism's limit is quoting macOS loopback.

**To get a production number**, run it on Linux (where `SO_REUSEPORT` kernel-balances the accept across reactors) with the load generator on a separate host, and with handlers that do representative work. The reactor ships its own `echo_bench` for a harness-free lower bound on the runtime itself.

## What's missing for production: TLS and HTTP/2

Both live in the shared reactor/http stack, so prism and Xapiand would gain them together.

- **TLS — moderate.** Asio ships `asio::ssl` (an OpenSSL wrapper). The reactor's session is a coroutine over a concrete `tcp::socket`; TLS means abstracting that stream (`tcp::socket` vs `ssl::stream<tcp::socket>`), an `async_handshake` after accept, certificate config, and an OpenSSL dependency. The coroutine read/write code is unchanged — it already works on any async stream. Contained to the reactor's session setup plus the `http_asio` read/write. Well-trodden. Effort: days, low conceptual risk. (ALPN in the handshake is also what HTTP/2 negotiation rides on, so it's the right first step regardless.)
- **HTTP/2 — hard.** The parser is HTTP/1.x (`Kronuz/http-parser`). HTTP/2 is a different, binary, multiplexed protocol with HPACK header compression and per-stream flow control, negotiated by ALPN over TLS. The pragmatic path is to integrate `nghttp2` (it does the framing, HPACK, and flow control) and map its streams onto the existing `Router`; writing the protocol from scratch is a much larger job. Requires TLS + ALPN first. Effort: weeks, higher risk. HTTP/3 (QUIC) is a bigger step again.

## Reproducing

```sh
# server, logging off
./build/prism -q 8880 --no-color

# peak throughput + latency
wrk -t6 -c256 -d15s --latency http://127.0.0.1:8880/hello

# is the client or the server the wall? (RPS should not move with -t)
for t in 2 6 10; do wrk -t$t -c256 -d8s http://127.0.0.1:8880/hello; done

# processing headroom: pipeline N requests per connection
#   pipeline.lua: init builds N wrk.format("GET","/hello"); request returns them
wrk -t6 -c256 -d8s -s pipeline.lua http://127.0.0.1:8880/hello

# cores actually used (1400% == all 14)
ps -o %cpu= -p "$(lsof -nP -iTCP:8880 -sTCP:LISTEN -t)"
```
