// TICK-TO-ORDER — the actual execution-engine metric.
//
//   tick_to_order = (moment the order bytes leave us) - (moment the tick arrived)
//
// The full path, every stage of which is real code we wrote:
//
//   socket read -> recv_ns STAMP
//     -> JSON parse into POD           (allocation-free, NEON-assisted)
//     -> book update
//     -> signal / decision             (microprice)
//     -> pre-trade risk check          (notional + position + kill switch)
//     -> order encode to venue format  (allocation-free, hand-rolled numerics)
//     -> WebSocket frame + mask        (zero-alloc, pooled mask keys)
//     -> send() syscall                -> order_sent_ns STAMP
//
// HONEST BOUNDARY: orders terminate at a LOCAL TCP SINK on loopback, not at
// OKX. Sending live orders needs credentials and real capital — a different
// project. Everything up to and including the send() syscall is the real code
// path; what is excluded is (a) TLS encrypt on the order socket and (b) the
// ~13ms wire flight to OKX. Both are named in the report rather than hidden.
//
// Two drivers, ONE code path:
//   --live       real OKX feed. Proves it works on real data, but OKX pushes
//                ~6 msgs/sec so there are far too few samples for a real p99.
//   --synthetic  replays a realistic frame at full rate for millions of
//                samples, which is what the tail percentiles need.
//
// Usage: tick2order [--synthetic N | --live SECONDS]
#include "clock.hpp"
#include "spsc_ring.hpp"
#include "feed.hpp"
#include "venues.hpp"
#include "signal.hpp"
#include "risk.hpp"
#include "order.hpp"
#include "hist.hpp"
#include "affinity.hpp"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
#include <fstream>

using namespace fa;

namespace {

const char* kFrame =
    R"({"arg":{"channel":"tickers","instId":"BTC-USDT-SWAP"},"data":[{)"
    R"("instType":"SWAP","instId":"BTC-USDT-SWAP","last":"64123.4",)"
    R"("bidPx":"64123.3","bidSz":"12.5","askPx":"64123.5","askSz":"8.2",)"
    R"("open24h":"63000.0","vol24h":"123456.0","ts":"1710000000000"}]})";

template <typename T>
inline void keep(const T& v) { asm volatile("" : : "r,m"(v) : "memory"); }

// A real loopback TCP sink: a listener thread accepts one connection and
// drains it. This gives us a genuine send() syscall through the real kernel
// network stack, which is the point — not a memcpy pretending to be I/O.
class LocalSink {
public:
    bool start() {
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd_ < 0) return false;
        int yes = 1;
        ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = 0;                       // ephemeral
        if (::bind(listen_fd_, (sockaddr*)&a, sizeof a) < 0) return false;
        if (::listen(listen_fd_, 1) < 0) return false;
        socklen_t sl = sizeof a;
        if (::getsockname(listen_fd_, (sockaddr*)&a, &sl) < 0) return false;
        port_ = ntohs(a.sin_port);

        drain_ = std::thread([this] {
            int c = ::accept(listen_fd_, nullptr, nullptr);
            if (c < 0) return;
            accepted_fd_ = c;
            std::vector<char> buf(1 << 16);
            while (run_.load(std::memory_order_acquire)) {
                ssize_t r = ::recv(c, buf.data(), buf.size(), 0);
                if (r <= 0) break;
                bytes_.fetch_add(uint64_t(r), std::memory_order_relaxed);
            }
        });

        // Connect the sending side.
        send_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (send_fd_ < 0) return false;
        sockaddr_in d{};
        d.sin_family = AF_INET;
        d.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        d.sin_port = htons(port_);
        if (::connect(send_fd_, (sockaddr*)&d, sizeof d) < 0) return false;
        int one = 1;
        ::setsockopt(send_fd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        return true;
    }
    int fd() const { return send_fd_; }
    uint64_t bytes() const { return bytes_.load(std::memory_order_acquire); }
    void stop() {
        run_.store(false, std::memory_order_release);
        if (send_fd_ >= 0) { ::close(send_fd_); send_fd_ = -1; }
        if (accepted_fd_ >= 0) { ::shutdown(accepted_fd_, SHUT_RDWR); }
        if (drain_.joinable()) drain_.join();
        if (listen_fd_ >= 0) { ::close(listen_fd_); listen_fd_ = -1; }
    }
    ~LocalSink() { stop(); }
private:
    int listen_fd_ = -1, send_fd_ = -1, accepted_fd_ = -1;
    uint16_t port_ = 0;
    std::thread drain_;
    std::atomic<bool> run_{true};
    std::atomic<uint64_t> bytes_{0};
};

// Mask a payload into scratch as a WebSocket text frame (client frames must be
// masked). Same logic as WebSocket::send_text_fast, but writing to a raw fd so
// we can measure the execution path without a TLS server on loopback.
inline size_t ws_frame(uint8_t* scratch, const char* payload, size_t n,
                       const uint8_t* mk) {
    size_t h = 0;
    scratch[h++] = 0x80 | 0x1;
    if (n < 126) scratch[h++] = 0x80 | uint8_t(n);
    else { scratch[h++] = 0x80 | 126;
           scratch[h++] = uint8_t((n >> 8) & 0xff);
           scratch[h++] = uint8_t(n & 0xff); }
    std::memcpy(scratch + h, mk, 4); h += 4;
    for (size_t i = 0; i < n; ++i)
        scratch[h + i] = uint8_t(payload[i]) ^ mk[i & 3];
    return h + n;
}

struct Stages { Histogram parse, decide, risk, encode, frame, syscall; };

void report(const char* name, const Histogram& h, const char* unit = "ns") {
    std::cout << "  " << std::left << std::setw(16) << name
              << " p50=" << std::setw(8) << h.p50()
              << " p99=" << std::setw(8) << h.p99()
              << " p99.9=" << std::setw(9) << h.p999()
              << " max=" << h.max() << unit << "\n";
}

} // namespace

// Load a captured frame corpus (one raw frame per line). Replaying real frames
// beats replaying one hardcoded string: real frames vary in price magnitude,
// field order and length, so the cache and branch predictor behave the way they
// would in production instead of being unrealistically hot on one constant.
std::vector<std::string> load_corpus(const std::string& path) {
    std::vector<std::string> v;
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) if (!line.empty()) v.push_back(line);
    return v;
}

int main(int argc, char** argv) {
    uint64_t synth_n = 500000;
    int live_sec = 0;
    std::string replay_path;
    uint64_t cadence_us = 0;   // idle gap between ticks (0 = back-to-back)
    bool warm = false;         // cache warming during the idle gap
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--synthetic" && i + 1 < argc) synth_n = std::strtoull(argv[++i], nullptr, 10);
        else if (a == "--live" && i + 1 < argc) { live_sec = std::atoi(argv[++i]); synth_n = 0; }
        else if (a == "--replay" && i + 1 < argc) { replay_path = argv[++i]; }
        else if (a == "--iters" && i + 1 < argc) synth_n = std::strtoull(argv[++i], nullptr, 10);
        else if (a == "--cadence-us" && i + 1 < argc) cadence_us = std::strtoull(argv[++i], nullptr, 10);
        else if (a == "--warm") warm = true;
    }

    bool locked = lock_memory();
    bool pinned = pin_to_core(0);

    LocalSink sink, warm_sink;
    if (!sink.start()) { std::cerr << "failed to start local sink\n"; return 1; }
    if (!warm_sink.start()) { std::cerr << "failed to start warm sink\n"; return 1; }

    std::cout << "=== TICK-TO-ORDER ===\n";
    std::cout << "order sink = loopback TCP (TCP_NODELAY), mlockall="
              << (locked ? "on" : "off") << " pinned=" << (pinned ? "yes" : "advisory")
              << "\n\n";

    Parser parse = okx_perp_spec("BTC-USDT-SWAP").parser;
    RiskEngine risk(RiskLimits{});
    uint8_t mask[4] = {0x37, 0xfa, 0x21, 0x3d};   // fixed here; pooled in prod path
    char        order_buf[512];
    uint8_t     frame_buf[1024];
    int fd = sink.fd();
    int warm_fd = warm_sink.fd();

    Histogram total;
    Stages st;
    uint64_t orders = 0, rejected = 0, no_signal = 0;

    // One tick through the entire path. `staged` adds per-stage timestamps,
    // which themselves cost ~41ns each on macOS — so the staged pass inflates
    // the total and is reported separately from the clean 2-stamp total.
    auto one_tick = [&](std::string_view frame, uint64_t recv_ns, bool staged,
                        int out_fd, bool measure) {
        MarketUpdate u{};
        bool got = false;
        auto sink_fn = [&](const MarketUpdate& m) { u = m; got = true; };

        uint64_t a = staged ? Clock::ticks() : 0;
        parse(frame, recv_ns, sink_fn);
        uint64_t b = staged ? Clock::ticks() : 0;
        if (!got) return;

        BookTop bk{u.bid, u.ask, u.bid_sz, u.ask_sz};
        Signal s = compute_signal(bk);
        uint64_t c = staged ? Clock::ticks() : 0;
        if (!s.valid) { ++no_signal; return; }

        // Pre-trade risk on the intended notional. One BTC-USDT-SWAP contract
        // has 0.01 BTC of face value, so notional is price * face * contracts —
        // not price * contracts, which would blow the per-order limit on every
        // single tick and reject the whole run.
        const double kFace = 0.01;
        double qty = 1.0;                        // contracts
        double notional = s.fair * kFace * qty;
        double signed_notional = (s.edge_bps >= 0 ? +notional : -notional);
        RiskDecision rd = risk.check("BTC-USDT-SWAP", signed_notional);
        uint64_t d = staged ? Clock::ticks() : 0;
        if (rd != RiskDecision::Accept) { ++rejected; return; }

        OrderReq o;
        std::memcpy(o.inst, "BTC-USDT-SWAP", 14);
        o.side  = (s.edge_bps >= 0) ? Side::Buy : Side::Sell;
        // Price at the near touch; never cross (this is an execution-latency
        // measurement, not a strategy).
        o.px    = (o.side == Side::Buy) ? bk.bid : bk.ask;
        o.sz    = qty;
        o.cl_id = ++orders;
        o.px_dp = 1; o.sz_dp = 0;
        size_t olen = encode_okx_order(order_buf, sizeof order_buf, o);
        uint64_t e = staged ? Clock::ticks() : 0;

        size_t flen = ws_frame(frame_buf, order_buf, olen, mask);
        uint64_t f = staged ? Clock::ticks() : 0;

        ssize_t w = ::send(out_fd, frame_buf, flen, 0);
        uint64_t g = Clock::ticks();
        keep(w);

        if (staged && measure) {
            st.parse.record(Clock::ticks_to_ns(b - a));
            st.decide.record(Clock::ticks_to_ns(c - b));
            st.risk.record(Clock::ticks_to_ns(d - c));
            st.encode.record(Clock::ticks_to_ns(e - d));
            st.frame.record(Clock::ticks_to_ns(f - e));
            st.syscall.record(Clock::ticks_to_ns(g - f));
        } else {
            // g is absolute ticks; recv_ns is absolute ns from the same epoch
            // (Clock::now_ns() == ticks_to_ns(mach_absolute_time())).
            if (measure) total.record(Clock::ticks_to_ns(g) - recv_ns);
        }
    };

    if (synth_n > 0) {
        // Prefer a captured real-frame corpus; fall back to the single builtin
        // frame only if no corpus was given.
        std::vector<std::string> corpus;
        if (!replay_path.empty()) {
            corpus = load_corpus(replay_path);
            if (corpus.empty()) {
                std::cerr << "corpus " << replay_path << " empty or missing\n";
                return 1;
            }
            std::cout << "--- replay driver: " << corpus.size()
                      << " captured OKX frames, " << synth_n
                      << " ticks (deterministic) ---\n";
        } else {
            corpus.emplace_back(kFrame);
            std::cout << "--- synthetic driver: 1 builtin frame, " << synth_n
                      << " ticks ---\n";
        }
        const size_t cn = corpus.size();

        // Warm caches, branch predictors, and the socket buffers.
        for (int i = 0; i < 20000; ++i)
            one_tick(corpus[i % cn], Clock::now_ns(), false, fd, true);
        total = Histogram{};
        orders = 0; rejected = 0; no_signal = 0;

        if (cadence_us) {
            std::cout << "    idle gap between ticks: " << cadence_us << "us"
                      << "   cache warming: " << (warm ? "ON" : "off") << "\n";
        }
        for (uint64_t i = 0; i < synth_n; ++i) {
            uint64_t recv_ns = Clock::now_ns();       // "tick arrived"
            one_tick(corpus[i % cn], recv_ns, false, fd, true);

            // Emulate a real venue cadence. With a gap this large the entire
            // hot path falls out of L1/L2 and the branch predictor loses its
            // history, so the NEXT tick pays cold-start cost -- which is what
            // production actually looks like at ~6 msgs/sec.
            if (cadence_us) {
                uint64_t until = Clock::now_ns() + cadence_us * 1000;
                while (Clock::now_ns() < until) {
                    // CACHE WARMING: run the identical hot path against a
                    // throwaway sink so i-cache/d-cache/branch-predictor stay
                    // hot. Unmeasured, and it never touches the real order fd.
                    if (warm)
                        one_tick(corpus[(i + 7) % cn], Clock::now_ns(),
                                 false, warm_fd, false);
                }
            }
        }
        std::cout << "\nTICK-TO-ORDER (clean, 2 timestamps):\n";
        report("tick->order", total);

        // Staged pass for the breakdown.
        for (uint64_t i = 0; i < synth_n / 5; ++i)
            one_tick(corpus[i % cn], Clock::now_ns(), true, fd, true);
        std::cout << "\nPER-STAGE BREAKDOWN (extra timestamps inflate the sum;\n"
                     "each Clock::ticks() costs ~41ns on this machine):\n";
        report("1 parse", st.parse);
        report("2 decide", st.decide);
        report("3 risk", st.risk);
        report("4 encode", st.encode);
        report("5 ws frame", st.frame);
        report("6 send()", st.syscall);
    } else {
        std::cout << "--- live driver: OKX, " << live_sec << "s ---\n";
        SpscRing<MarketUpdate> ring(4096);
        Feed feed(okx_perp_spec("BTC-USDT-SWAP"), &ring);
        if (!feed.start()) { std::cerr << "failed to connect OKX\n"; return 1; }
        std::cout << "connected  tcp=" << feed.tcp_connect_ns()/1e6
                  << "ms tls=" << feed.tls_handshake_ns()/1e6 << "ms\n";

        // The live path re-runs the same stages on real MarketUpdates.
        uint64_t deadline = Clock::now_ns() + uint64_t(live_sec) * 1000000000ull;
        MarketUpdate u;
        while (Clock::now_ns() < deadline) {
            while (ring.pop(u)) {
                if (u.kind != UpdKind::Book) continue;
                BookTop bk{u.bid, u.ask, u.bid_sz, u.ask_sz};
                Signal s = compute_signal(bk);
                if (!s.valid) { ++no_signal; continue; }
                double notional = s.fair * 0.01;   // 1 contract = 0.01 BTC face
                RiskDecision rd = risk.check("BTC-USDT-SWAP",
                                             s.edge_bps >= 0 ? notional : -notional);
                if (rd != RiskDecision::Accept) { ++rejected; continue; }
                OrderReq o;
                std::memcpy(o.inst, "BTC-USDT-SWAP", 14);
                o.side = (s.edge_bps >= 0) ? Side::Buy : Side::Sell;
                o.px = (o.side == Side::Buy) ? bk.bid : bk.ask;
                o.sz = 1.0; o.cl_id = ++orders; o.px_dp = 1; o.sz_dp = 0;
                size_t olen = encode_okx_order(order_buf, sizeof order_buf, o);
                size_t flen = ws_frame(frame_buf, order_buf, olen, mask);
                ssize_t w = ::send(fd, frame_buf, flen, 0);
                keep(w);
                total.record(Clock::now_ns() - u.recv_ns);
            }
        }
        feed.stop();
        std::cout << "\nTICK-TO-ORDER on live OKX ticks (n=" << total.count()
                  << " — thin sample, OKX pushes ~6 msgs/sec):\n";
        report("tick->order", total);
    }

    std::cout << "\norders sent=" << orders
              << "  risk-rejected=" << rejected
              << "  no-signal=" << no_signal
              << "  bytes to sink=" << sink.bytes() << "\n";
    std::cout << "\nEXCLUDED from the number above (named, not hidden):\n"
                 "  * TLS encrypt on the order socket (~1-2us/frame)\n"
                 "  * ~13ms wire flight to OKX (measured separately by shootout)\n";
    sink.stop();
    return 0;
}
