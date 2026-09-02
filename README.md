tick2order
----------

tick2order measures the latency from a market data tick arriving on the wire to
the order bytes leaving the machine. It hand-rolls the whole path: TLS and
WebSocket over raw sockets, an allocation-free JSON scanner, a lock-free SPSC
ring, and an allocation-free order encoder.

**p50 3.71 µs, p99 4.86 µs** over 300K replayed ticks of real OKX
BTC-USDT-SWAP data.

### Documentation quick links

* [Building](#building)
* [Usage](#usage)
* [Benchmarks](#benchmarks)
* [What is not measured](#what-is-not-measured)
* [TECHNICAL_CONCEPTS.md](TECHNICAL_CONCEPTS.md)

### Building

```
$ make              # all binaries
$ make test         # 234 unit tests
```

macOS or Linux, aarch64 or x86-64. The NEON scan path is aarch64 only and falls
back to `memchr` elsewhere.

### Usage

```
$ ./bin/tick2order --replay data/okx_frames.jsonl --iters 300000
$ ./bin/tick2order --replay data/okx_frames.jsonl --iters 3000 --cadence-us 2000 --warm
$ ./bin/capture data/my_frames.jsonl --frames 1000 --seconds 60
```

`capture` is the only tool that touches the network. `data/okx_frames.jsonl`
already holds 633 real tickers, so replay works out of the box.

### Benchmarks

Apple silicon, 300K iterations replayed from the captured corpus.

```
p50     3,712 ns
p99     4,864 ns
p99.9   8,192 ns
max    13,084 ns
```

Where it goes:

| stage | p50 | share |
|---|---|---|
| JSON parse | 1984 ns | 55% |
| `send()` syscall | 1600 ns | 45% |
| risk check | <41 ns | ~0 |
| order encode | <41 ns | ~0 |
| WS frame + mask | <41 ns | ~0 |

Everything hand-written falls below the 41 ns timer resolution. The entire
budget is JSON parsing and one syscall, which means there is nothing left to
win in the strategy or encode path and the next real step is `writev`/
`sendmmsg` batching, or kernel bypass on Linux.

#### Cache warming

OKX pushes about 6 messages/sec, so between ticks the i-cache, d-cache and
branch predictors go cold. Running the identical code path unmeasured against a
throwaway sink during the idle gap recovers it:

| scenario | p50 | p99 |
|---|---|---|
| back-to-back (unrealistic) | 3584 ns | 4864 ns |
| 2 ms idle gap, no warming | 7936 ns | 10240 ns |
| 2 ms idle gap, warming | 3584 ns | 4608 ns |

This also explained an earlier live measurement of 8.7 µs that had been written
off as network noise. It was cold cache, and it reproduces on demand.

Warming costs the extreme tail, p99.9 and max, because the CPU never idles.
Production would tune the cadence against that rather than leaving it on.
Technique from arXiv 2309.04259, A/B tested here rather than assumed.

### What is not measured

Orders terminate at a loopback TCP sink, not at OKX. The number covers JSON
parse, order encode, WebSocket framing and `send()`. It excludes TLS
encryption on the order socket, worth roughly 1-2 µs, and the ~13 ms of wire
flight to the exchange.

Real order submission needs credentials and capital. This measures the local
execution stack, which is the part under your control.

This is not a trading strategy and not a live trading system. There are no
signals, no PnL, no position tracking, no order cancellation and no crash
recovery.

### Why replay

Benchmarking against a live venue is close to useless: the sample count changes
every run and market noise swamps whatever you just changed. Frames are
captured once and replayed deterministically, so the same input gives the same
number and an optimization shows up as a delta.

### Design notes

RapidJSON allocates and builds a tree, and only 5 known fields per message are
needed, so the scanner reads in place with no allocation on the hot path. The
cost is that it is schema-specific and does no validation, which is acceptable
for fixed venue messages with tests behind them.

`strtod` consults the locale on every call. For the bounded numeric tokens an
exchange sends, a mantissa-in-`uint64_t` fast path is faster and still
correctly rounded; it defers to `strtod` for tokens over 19 digits or |exp| >
22.

`affinity.hpp` pins threads and calls `mlockall` to cut scheduler jitter. That
is enforced on Linux. On macOS the affinity API is advisory and the code says
so rather than pretending otherwise.

### Layout

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
  capture_main.cpp      frame capture

cpp/tests/
  test_all.cpp     81 tests: JSON, risk, venue routing
  test_infra.cpp   153 tests: SPSC ring, clock, WebSocket, venue parsers
```
