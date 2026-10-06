# quant-latency (tick2order)

Measures tick-to-order latency: the time from a market data message arriving
until the order bytes leave the machine. The whole path is written by hand in
C++20: TLS and WebSocket on raw sockets, a JSON scanner that doesn't allocate,
a lock-free SPSC ring, and an order encoder that doesn't allocate either.

Over 300K replayed ticks of real OKX BTC-USDT-SWAP data, latency is **3.71 µs
at p50 and 4.86 µs at p99**.

## Building

Requires macOS or Linux, clang with C++20, and OpenSSL 3 (`brew install
openssl@3`). The NEON scanning path only works on aarch64; other platforms
fall back to `memchr`.

```sh
make          # builds bin/tick2order, bin/capture, bin/tests_infra
make test     # unit tests (149 checks)
```

`make test_live` also runs a test against the live exchange feeds.

## Usage

```sh
./bin/tick2order --replay data/okx_frames.jsonl --iters 300000
./bin/tick2order --replay data/okx_frames.jsonl --iters 3000 --cadence-us 2000 --warm
./bin/tick2order --synthetic 1000000     # one built-in frame, as fast as possible
./bin/tick2order --live 60               # real OKX feed for 60 seconds

./bin/capture data/my_frames.jsonl --frames 1000 --seconds 60
```

`data/okx_frames.jsonl` contains 633 real ticker messages, so replay works
without a network connection. `capture` records new frames. Only `capture`
and `--live` use the network.

## Results

Apple silicon, 300K iterations replayed from the saved data:

```
p50     3,712 ns
p99     4,864 ns
p99.9   8,192 ns
max    13,084 ns
```

| stage | p50 | share |
|---|---|---|
| JSON parse | 1984 ns | 55% |
| `send()` syscall | 1600 ns | 45% |
| risk check | <41 ns | ~0 |
| order encode | <41 ns | ~0 |
| WebSocket frame + mask | <41 ns | ~0 |

Everything except JSON parsing and the `send()` call takes less than the
timer's 41 ns resolution. To get faster from here, the next things to try
are batching with `writev`/`sendmmsg` or kernel bypass on Linux.

### Cache warming

OKX sends about 6 messages per second. Between messages, the instruction
cache, data cache, and branch predictor go cold. Running the same code path
during idle time, without measuring it and with output going to a throwaway
sink, keeps them warm:

| scenario | p50 | p99 |
|---|---|---|
| back-to-back (unrealistic) | 3584 ns | 4864 ns |
| 2 ms gap, no warming | 7936 ns | 10240 ns |
| 2 ms gap, warming (`--warm`) | 3584 ns | 4608 ns |

This also explained an earlier live measurement of 8.7 µs that I'd blamed on
network noise. The downside is that warming makes p99.9 and max worse,
because the CPU never gets to idle. The idea comes from arXiv 2309.04259.

## What's not measured

- Orders go to a TCP sink on loopback, not to OKX. The measurement covers
  JSON parsing, order encoding, WebSocket framing, and `send()`. It doesn't
  include TLS encryption on the order connection (roughly 1–2 µs) or the
  ~13 ms network trip to the exchange.
- This isn't a trading strategy or a live trading system. There are no
  signals, PnL, position tracking, cancels, or crash recovery.

I used replay instead of live data because the live message rate and market
noise change from run to run. With replay, the same input gives the same
numbers, so the effect of a change is easy to see.

## Design notes

- **JSON.** RapidJSON allocates and builds a tree, but only 5 fields per
  message are needed. The scanner reads them in place without allocating. It
  only works for this specific message schema and doesn't validate input.
- **Number parsing.** `strtod` checks the locale on every call. For the
  short numbers an exchange sends, parsing the mantissa into a `uint64_t` is
  faster and still correctly rounded. It falls back to `strtod` for numbers
  with more than 19 digits or an exponent beyond ±22.
- **Pinning.** `affinity.hpp` pins threads and calls `mlockall`. Linux
  enforces the pinning; on macOS the affinity API is only a hint.

## Layout

```
cpp/include/
  clock.hpp          monotonic ns clock (mach_absolute_time / CLOCK_MONOTONIC_RAW)
  net.hpp, tls.hpp, websocket.hpp   TCP, TLS, WebSocket client
  json_scan.hpp      JSON scanner (NEON) and number parser
  order.hpp          order struct and OKX encoder
  feed.hpp, venues.hpp   market data feeds and venue message parsers
  risk.hpp, fill.hpp, signal.hpp   risk checks, fills, microprice/imbalance
  spsc_ring.hpp      lock-free SPSC ring
  hist.hpp           latency histogram
  affinity.hpp       thread pinning and mlockall
cpp/src/
  tick2order_main.cpp   the benchmark
  capture_main.cpp      frame capture
cpp/tests/test_infra.cpp   unit tests
data/okx_frames.jsonl      captured OKX frames
```
