# AGENTS.md — prism

An HTTP application server assembled from the Kronuz libraries; also the worked
example of how they fit together.

## Repo map

```
src/main.cc          The whole app: banner, access-logging middleware, DemoApp routes, main().
assets/prism.png     The 48x48 spectrum PNG that /image serves; shown in the README.
assets/prism.png.b64 Its base64, pasted into kPrismPngB64 in main.cc (not read by the build).
BENCHMARKS.md        Throughput, latency, logging cost (2026-07-08, M4 Pro).
ARCHITECTURE.md      Why it exists, the pieces, and how to build a service on it.
```

## Conventions

- Dependencies come from FetchContent at their tips; fix a library bug in its own repo,
  not here.
- The banner wordmark is figlet's Standard font (`figlet prism`), with hand-stylized
  "p" and "m" descenders; don't regenerate it blind.
- The banner prints through the logger at NOTICE, so `-q` hides it.
- Keep the demo small: it exists to keep the wiring legible.

## Verify a change

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build
./build/prism 8880 -v &
curl -s localhost:8880/hello -H 'Accept: application/json' && curl -s localhost:8880/boom
kill %1
```
