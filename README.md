# tick2order

A latency harness for the market-data-to-order path: how long between a tick
arriving on the wire and the order bytes leaving the machine.

**p50 = 3.71 µs, p99 = 4.86 µs**, over 300K replayed ticks of real OKX
BTC-USDT-SWAP data on Apple Silicon.

## Where the time goes

| stage | p50 | share | how |
|---|---|---|---|
| JSON parse | 1984 ns | 55% | NEON quote scan, strtod-free float path |
| `send()` | 1600 ns | 45% | kernel TCP stack, `TCP_NODELAY` |
| risk check | <41 ns | ~0 | below timer resolution |
| order encode | <41 ns | ~0 | fixed-decimal formatting, no allocation |
| WS frame + mask | <41 ns | ~0 | memcpy + XOR |

Everything hand-written is free — it disappears under the 41 ns timer
resolution. The whole budget is parsing and one syscall. That is the useful
result: there is nothing left to micro-optimize in the strategy or encode path,
and the next real win is `writev`/`sendmmsg` batching or kernel bypass.

```
p50     3,712 ns
p99     4,864 ns
p99.9   8,192 ns
max    13,084 ns
```

## Cache warming

OKX pushes about 6 messages/sec, so between ticks the i-cache, d-cache and
branch predictors all go cold. Running the identical code path unmeasured
against a throwaway sink during the idle gap recovers it:

| scenario | p50 | p99 |
|---|---|---|
| back-to-back (unrealistic) | 3584 ns | 4864 ns |
| 2 ms idle gap, no warming | 7936 ns | 10240 ns |
| 2 ms idle gap, warming | 3584 ns | 4608 ns |

This also explained an earlier live-OKX measurement of 8.7 µs that had been
written off as network noise. It was cold cache, and it reproduces
deterministically.

Warming is not free: it costs the extreme tail (p99.9 and max) because the CPU
never idles. A production system would tune the warming cadence against that
trade-off rather than leaving it on.

Technique from arXiv 2309.04259, A/B-tested here rather than assumed.

## What is and isn't measured

Orders terminate at a loopback TCP sink, not at OKX. So the number covers JSON
parse, order encode, WebSocket framing and the `send()` syscall — all real — and
excludes TLS encryption on the order socket (~1–2 µs) and the ~13 ms of wire
flight to the exchange.

Real order submission needs credentials and capital. This measures how fast the
local execution stack runs, which is the part that's actually under your
control.

## Capture once, replay forever

Benchmarking against a live venue is close to useless: the sample count changes
every run and market noise swamps whatever you just optimized. So frames are
captured once and replayed deterministically.

```bash
./bin/capture data/my_frames.jsonl --frames 1000 --seconds 60
./bin/tick2order --replay data/okx_frames.jsonl --iters 300000
```

`data/okx_frames.jsonl` already holds 633 real BTC-USDT-SWAP tickers. Same
input, same number every time, so an optimization shows up as a delta instead
of noise.

## Build and run

```bash
make                # all binaries
make test           # 234 unit tests

./bin/tick2order --replay data/okx_frames.jsonl --iters 300000
./bin/tick2order --replay data/okx_frames.jsonl --iters 3000 --cadence-us 2000 --warm
```

## Design notes

**Allocation-free JSON scan instead of a library.** RapidJSON allocates and
builds a tree; only 5 known fields per message are needed. Scanning in place
costs nothing on the hot path and has no malloc jitter. The price is that it's
schema-specific and does no validation — fine for fixed venue messages with
tests behind them.

**NEON quote scan, memchr fallback.** Finding the next `"` is the inner loop.
On aarch64 a 16-byte NEON compare beats a byte loop; elsewhere it falls back to
memchr, which libc has already vectorized.

**Hand-rolled float parser.** `strtod` consults the locale on every call. For
the bounded numeric tokens an exchange sends, a mantissa-in-`uint64_t` fast
path is faster and still correctly rounded. It defers to `strtod` for the rare
pathological token (>19 digits, or |exp| > 22).

**Thread pinning and `mlockall`.** `affinity.hpp` pins threads to eliminate
scheduler jitter. Enforced on Linux; on macOS the API is advisory and the code
says so rather than pretending otherwise.

**Fixed-bucket histogram.** O(1) record, correct tail percentiles over millions
of samples.

## Layout

```
cpp/include/
  clock.hpp        ns monotonic clock (mach_absolute_time / CLOCK_MONOTONIC_RAW)
  net.hpp, tls.hpp, websocket.hpp   raw TCP + TLS + WebSocket, no client libs
  json_scan.hpp    allocation-free JSON scan: NEON + strtod-free atof
  order.hpp        OrderReq POD + allocation-free OKX encoder
  spsc_ring.hpp    lock-free SPSC ring, cache-line padded
  signal.hpp       microprice / imbalance
  hist.hpp         fixed-bucket latency histogram
  affinity.hpp     thread pinning + mlockall

cpp/src/
  tick2order_main.cpp   the benchmark
  capture_main.cpp      frame capture (the only tool that touches the network)

cpp/tests/
  test_all.cpp     81 tests: JSON, risk, venue routing
  test_infra.cpp   153 tests: SPSC ring, clock, WebSocket, venue parsers
```

## Scope

Not a strategy — no signals, no PnL, no hedging. Not a live trading system — no
real order submission, no position tracking, no cancellation or crash recovery.
It's a measurement tool for the execution path.

## References

- arXiv 2309.04259 — C++ design patterns for low-latency applications
- RFC 6455 — the WebSocket protocol
