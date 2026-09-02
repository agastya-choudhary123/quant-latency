// Tests for the real infrastructure: lock-free ring, ns clock, risk engine,
// paper fill, JSON scanner, and the venue parsers. A `--live` flag additionally
// runs a real network integration test against the exchanges.
#include "clock.hpp"
#include "spsc_ring.hpp"
#include "json_scan.hpp"
#include "risk.hpp"
#include "fill.hpp"
#include "venues.hpp"
#include "signal.hpp"
#include "hist.hpp"
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

using namespace fa;

static int g_pass = 0, g_fail = 0;
#define CHECK(c) do { if (c) ++g_pass; else { ++g_fail; \
    std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << "  " << #c << "\n"; } } while (0)
#define CHECK_NEAR(a,b,e) do { double _a=(a),_b=(b); if (std::fabs(_a-_b)<=(e)) ++g_pass; \
    else { ++g_fail; std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << "  " \
    << #a << "(" << _a << ")!=" << #b << "(" << _b << ")\n"; } } while (0)

// ---------------------------------------------------------------- clock
static void test_clock() {
    uint64_t a = Clock::now_ns();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    uint64_t b = Clock::now_ns();
    CHECK(b > a);                         // monotonic, advances
    CHECK(b - a >= 500000);               // ~>=0.5ms elapsed as expected
    CHECK(Clock::ticks_to_ns(0) == 0);
    uint64_t timed = 0;
    { ScopedTimer t(timed); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
    CHECK(timed > 0);
}

// ---------------------------------------------------------------- SPSC ring
static void test_ring_basic() {
    SpscRing<int> r(8);
    CHECK(r.empty());
    int x;
    CHECK(!r.pop(x));
    for (int i = 0; i < 7; ++i) CHECK(r.push(i));   // cap-1 usable
    CHECK(!r.push(999));                            // full
    for (int i = 0; i < 7; ++i) { CHECK(r.pop(x)); CHECK(x == i); }
    CHECK(r.empty());
    bool threw = false;
    try { SpscRing<int> bad(6); } catch (...) { threw = true; }
    CHECK(threw);                                   // non-power-of-two rejected
}

// Concurrent producer/consumer: every value crosses intact and in order.
static void test_ring_threaded() {
    SpscRing<uint64_t> r(1024);
    const uint64_t N = 2000000;
    std::atomic<bool> ok{true};
    std::thread prod([&] {
        for (uint64_t i = 0; i < N; ++i)
            while (!r.push(i)) { /* spin on full */ }
    });
    uint64_t expect = 0, v;
    while (expect < N) {
        if (r.pop(v)) {
            if (v != expect) { ok = false; break; }
            ++expect;
        }
    }
    prod.join();
    CHECK(ok.load());
    CHECK(expect == N);
}

// ---------------------------------------------------------------- json scan
static void test_json() {
    std::string okx = R"({"arg":{"channel":"tickers","instId":"BTC-USDT-SWAP"},)"
                      R"("data":[{"instId":"BTC-USDT-SWAP","last":"64000.1",)"
                      R"("bidPx":"63999.5","askPx":"64000.5"}]})";
    double d; std::string s;
    CHECK(json_number(okx, "bidPx", d)); CHECK_NEAR(d, 63999.5, 1e-6);
    CHECK(json_number(okx, "askPx", d)); CHECK_NEAR(d, 64000.5, 1e-6);
    CHECK(json_string(okx, "instId", s)); CHECK(s == "BTC-USDT-SWAP");

    std::string kraken = R"({"channel":"ticker","type":"update","data":)"
                         R"([{"symbol":"BTC/USD","bid":64001.2,"ask":64002.3,"last":64001.9}]})";
    CHECK(json_number(kraken, "bid", d)); CHECK_NEAR(d, 64001.2, 1e-6);  // bare number
    CHECK(json_number(kraken, "ask", d)); CHECK_NEAR(d, 64002.3, 1e-6);
    CHECK(json_string(kraken, "symbol", s)); CHECK(s == "BTC/USD");

    CHECK(!json_number(okx, "nope", d));
    // resume-from offset finds the second occurrence
    std::string two = R"({"px":"1.0","x":0,"px":"2.0"})";
    CHECK(json_number(two, "px", d)); CHECK_NEAR(d, 1.0, 1e-9);
    CHECK(json_number(two, "px", d, 8)); CHECK_NEAR(d, 2.0, 1e-9);
}

// ---------------------------------------------------------------- risk
static void test_risk() {
    RiskLimits lim; lim.max_order_notional=25000; lim.max_symbol_notional=100000;
    lim.max_gross_notional=250000;
    RiskEngine r(lim);

    CHECK(r.check("BTC", 10000) == RiskDecision::Accept);
    CHECK(r.check("BTC", 30000) == RiskDecision::RejectOrderSize);   // > per-order

    // Build up to the symbol limit.
    for (int i = 0; i < 10; ++i) r.on_fill("BTC", 10000);   // pos=100000
    CHECK_NEAR(r.position("BTC"), 100000, 1e-6);
    CHECK(r.check("BTC", 10000) == RiskDecision::RejectSymbolLimit); // would exceed

    // Gross across symbols.
    RiskEngine g(lim);
    g.on_fill("A", 100000); g.on_fill("B", 100000); // gross 200000
    CHECK(g.check("C", 20000) == RiskDecision::Accept);
    g.on_fill("C", 40000);                           // gross 240000
    CHECK(g.check("D", 20000) == RiskDecision::RejectGrossLimit); // ->260000
    CHECK_NEAR(g.gross_notional(), 240000, 1e-6);

    // Kill switch latches.
    RiskEngine k(lim); k.kill();
    CHECK(k.killed());
    CHECK(k.check("BTC", 100) == RiskDecision::RejectKilled);
    k.reset_kill();
    CHECK(k.check("BTC", 100) == RiskDecision::Accept);

    // Netting reduces exposure.
    RiskEngine n(lim); n.on_fill("BTC", 50000); n.on_fill("BTC", -50000);
    CHECK_NEAR(n.position("BTC"), 0, 1e-6);
}

// ---------------------------------------------------------------- fill
static void test_fill() {
    // Buy crosses the ask, sell crosses the bid.
    PaperFill b = paper_fill(Side::Buy, 10000, 99.0, 101.0, 0.0005);
    CHECK(b.filled); CHECK_NEAR(b.price, 101.0, 1e-9);
    CHECK_NEAR(b.qty, 10000.0/101.0, 1e-9);
    CHECK_NEAR(b.fee, 0.0005 * b.notional, 1e-9);

    PaperFill s = paper_fill(Side::Sell, 10000, 99.0, 101.0, 0.0005);
    CHECK_NEAR(s.price, 99.0, 1e-9);

    // No book -> no fill.
    CHECK(!paper_fill(Side::Buy, 10000, 0, 0, 0.0005).filled);

    // qty-based close matches quantity exactly.
    PaperFill q = paper_fill_qty(Side::Sell, 2.5, 99.0, 101.0, 0.0005);
    CHECK_NEAR(q.qty, 2.5, 1e-12); CHECK_NEAR(q.price, 99.0, 1e-9);
}

// ---------------------------------------------------------------- parsers
static void test_parsers() {
    int emitted = 0; MarketUpdate got;
    auto emit = [&](const MarketUpdate& u){ got = u; ++emitted; };

    auto okx = okx_perp_spec("BTC-USDT-SWAP");
    std::string okx_tick = R"({"arg":{"channel":"tickers","instId":"BTC-USDT-SWAP"},)"
        R"("data":[{"instId":"BTC-USDT-SWAP","last":"64000.1","bidPx":"63999.5","askPx":"64000.5"}]})";
    emitted = 0;
    CHECK(okx.parser(okx_tick, 12345, emit) == 1);
    CHECK(got.kind == UpdKind::Book);
    CHECK_NEAR(got.bid, 63999.5, 1e-6); CHECK_NEAR(got.ask, 64000.5, 1e-6);
    CHECK(std::string(got.venue) == "okx");
    CHECK(got.recv_ns == 12345);

    std::string okx_fund = R"({"arg":{"channel":"funding-rate","instId":"BTC-USDT-SWAP"},)"
        R"("data":[{"instId":"BTC-USDT-SWAP","fundingRate":"0.0000531","nextFundingRate":"0.00006"}]})";
    emitted = 0;
    CHECK(okx.parser(okx_fund, 1, emit) == 1);
    CHECK(got.kind == UpdKind::Funding);
    CHECK_NEAR(got.funding_rate, 0.0000531, 1e-10);

    // subscribe ack -> no emit
    CHECK(okx.parser(R"({"event":"subscribe","arg":{"channel":"tickers"}})", 1, emit) == 0);

    auto kr = kraken_spot_spec("BTC/USD");
    std::string kr_tick = R"({"channel":"ticker","type":"update","data":)"
        R"([{"symbol":"BTC/USD","bid":64001.2,"ask":64002.3,"last":64001.9}]})";
    emitted = 0;
    CHECK(kr.parser(kr_tick, 7, emit) == 1);
    CHECK_NEAR(got.bid, 64001.2, 1e-6);
    CHECK(kr.parser(R"({"channel":"status","data":[{"system":"online"}]})", 1, emit) == 0);

    auto cb = coinbase_spot_spec("BTC-USD");
    std::string cb_tick = R"({"channel":"ticker","events":[{"tickers":[{"type":"ticker",)"
        R"("product_id":"BTC-USD","price":"64010.5","best_bid":"64010.0","best_ask":"64011.0"}]}]})";
    emitted = 0;
    CHECK(cb.parser(cb_tick, 9, emit) == 1);
    CHECK_NEAR(got.last, 64010.5, 1e-6);
    CHECK_NEAR(got.bid, 64010.0, 1e-6);
    // Regression: the venue name must survive intact (buffer big enough for
    // "coinbase" — an 8-char name that a char[8] would truncate to "coinbas").
    CHECK(std::string(got.venue) == "coinbase");
}

// ---------------------------------------------------------------- live (opt-in)
static void test_live() {
    std::cout << "\n[live] connecting to real exchanges...\n";
    SpscRing<MarketUpdate> ring(4096);
    Feed feed(okx_perp_spec("BTC-USDT-SWAP"), &ring);
    bool up = feed.start();
    CHECK(up);
    if (!up) { std::cerr << "[live] OKX connect failed\n"; return; }
    std::cout << "[live] okx tcp=" << feed.tcp_connect_ns()/1e6 << "ms tls="
              << feed.tls_handshake_ns()/1e6 << "ms ws=" << feed.ws_upgrade_ns()/1e6 << "ms\n";
    // Drain for ~3s and confirm we received real book updates.
    uint64_t deadline = Clock::now_ns() + 3ull*1000000000ull;
    int got_book = 0; MarketUpdate u;
    while (Clock::now_ns() < deadline) {
        while (ring.pop(u)) {
            if (u.kind == UpdKind::Book && u.bid > 0 && u.ask >= u.bid) ++got_book;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    feed.stop();
    std::cout << "[live] received " << got_book << " valid OKX book updates, "
              << feed.messages() << " msgs total\n";
    CHECK(got_book > 0);
}

// ---------------------------------------------------------------- basis
// ---------------------------------------------------------------- signal
static void test_signal() {
    // Balanced book: microprice == mid, imbalance == 0, no edge.
    {
        BookTop b{100.0, 100.2, 5.0, 5.0};
        Signal s = compute_signal(b);
        CHECK(s.valid);
        CHECK_NEAR(s.fair, 100.1, 1e-9);
        CHECK_NEAR(s.imb, 0.0, 1e-12);
        CHECK_NEAR(s.edge_bps, 0.0, 1e-9);
    }
    // Bid heavier -> microprice pulled toward the ask (upward pressure), so
    // imbalance > 0 and the microprice edge is positive.
    {
        BookTop b{100.0, 100.2, 9.0, 1.0};
        Signal s = compute_signal(b);
        CHECK(s.imb > 0.0);
        CHECK(s.fair > b.mid());
        CHECK(s.edge_bps > 0.0);
        // price weighted by opposite size: (100*1 + 100.2*9)/10 = 100.18
        CHECK_NEAR(s.fair, 100.18, 1e-9);
    }
    // Ask heavier -> mirror image.
    {
        BookTop b{100.0, 100.2, 1.0, 9.0};
        Signal s = compute_signal(b);
        CHECK(s.imb < 0.0);
        CHECK(s.fair < b.mid());
        CHECK(s.edge_bps < 0.0);
    }
    // Imbalance is bounded in [-1,1] at the extremes.
    CHECK_NEAR((imbalance(BookTop{100,100.2,10,0.0000001})), 1.0, 1e-5);
    CHECK_NEAR((imbalance(BookTop{100,100.2,0.0000001,10})), -1.0, 1e-5);
    // Invalid books (missing size, crossed, zero) produce no signal.
    CHECK(!compute_signal(BookTop{100,100.2,0,5}).valid);
    CHECK(!compute_signal(BookTop{100,100.2,5,0}).valid);
    CHECK(!compute_signal(BookTop{100.2,100.0,5,5}).valid);  // crossed
    CHECK(!compute_signal(BookTop{0,100.2,5,5}).valid);
    CHECK_NEAR((BookTop{100.0,100.2,5,5}.spread_bps()), 0.2/100.1*1e4, 1e-6);
}

// ---------------------------------------------------------------- histogram
static void test_hist() {
    Histogram h;
    CHECK(h.count() == 0 && h.p50() == 0);
    for (int i = 1; i <= 1000; ++i) h.record(uint64_t(i));
    CHECK(h.count() == 1000);
    CHECK(h.min() == 1);
    CHECK(h.max() == 1000);
    // Bucketed percentiles: within ~7% of the true value (1/16 resolution).
    auto near = [](uint64_t got, double want) {
        return std::fabs(double(got) - want) <= 0.08 * want + 1;
    };
    CHECK(near(h.p50(), 500));
    CHECK(near(h.p90(), 900));
    CHECK(near(h.p99(), 990));
    // Monotonic ordering always holds regardless of bucketing.
    CHECK(h.p50() <= h.p90());
    CHECK(h.p90() <= h.p99());
    CHECK(h.p99() <= h.max());
    // Zero and large values land in valid buckets.
    Histogram z; z.record(0); z.record(1); z.record(1ull<<40);
    CHECK(z.count() == 3);
    CHECK(z.max() >= (1ull<<40));
    // merge combines counts and preserves extremes.
    Histogram a, b;
    for (int i = 0; i < 100; ++i) a.record(10);
    for (int i = 0; i < 100; ++i) b.record(1000);
    a.merge(b);
    CHECK(a.count() == 200);
    CHECK(a.min() == 10 && a.max() == 1000);
}

// ---------------------------------------------------------------- no-alloc json
static void test_json_noalloc() {
    // Quoted-number field (OKX/Coinbase style) parses without allocation.
    std::string js = R"({"bidPx":"64123.3","bidSz":"12.5","askPx":"64123.5",)"
                     R"("askSz":"8.2","instId":"BTC-USDT-SWAP"})";
    double d;
    CHECK(json_number(js, "bidSz", d)); CHECK_NEAR(d, 12.5, 1e-9);
    CHECK(json_number(js, "askSz", d)); CHECK_NEAR(d, 8.2, 1e-9);
    // A key that is a prefix of another must not false-match: "bid" vs "bidPx".
    std::string pre = R"({"bidPx":"1.0","bid":"2.0"})";
    CHECK(json_number(pre, "bid", d)); CHECK_NEAR(d, 2.0, 1e-9);
    CHECK(json_number(pre, "bidPx", d)); CHECK_NEAR(d, 1.0, 1e-9);
    // Negative and exponent forms.
    std::string neg = R"({"fundingRate":"-0.00012","x":"1.5e-3"})";
    CHECK(json_number(neg, "fundingRate", d)); CHECK_NEAR(d, -0.00012, 1e-12);
    CHECK(json_number(neg, "x", d)); CHECK_NEAR(d, 1.5e-3, 1e-12);
    // Missing key fails cleanly.
    CHECK(!json_number(js, "absent", d));
    // Absurdly long number is rejected rather than overflowing the stack buffer.
    std::string big = "{\"k\":\"" + std::string(200, '9') + "\"}";
    CHECK(!json_number(big, "k", d));
}

// ---------------------------------------------------------------- binance parser
static void test_binance_parser() {
    int emitted = 0; MarketUpdate got;
    auto emit = [&](const MarketUpdate& u){ got = u; ++emitted; };
    auto bn = binance_perp_spec("BTCUSDT");

    // Path is URL-embedded, lower-cased, both streams present.
    CHECK(bn.path.find("btcusdt@bookTicker") != std::string::npos);
    CHECK(bn.path.find("btcusdt@markPrice") != std::string::npos);
    CHECK(bn.subscribes.empty());

    // bookTicker frame (single-letter keys b/B/a/A).
    std::string bt = R"({"stream":"btcusdt@bookTicker","data":{"e":"bookTicker",)"
        R"("u":1,"s":"BTCUSDT","b":"64000.1","B":"12.5","a":"64000.5","A":"8.2"}})";
    emitted = 0;
    CHECK(bn.parser(bt, 999, emit) == 1);
    CHECK(emitted == 1);
    CHECK(std::string(got.venue) == "binance");
    CHECK(std::string(got.symbol) == "BTCUSDT");
    CHECK_NEAR(got.bid, 64000.1, 1e-6);
    CHECK_NEAR(got.ask, 64000.5, 1e-6);
    CHECK_NEAR(got.bid_sz, 12.5, 1e-6);
    CHECK_NEAR(got.ask_sz, 8.2, 1e-6);
    CHECK(got.kind == UpdKind::Book);
    CHECK(got.recv_ns == 999);

    // markPrice frame carries the funding rate in "r".
    std::string mp = R"({"stream":"btcusdt@markPrice","data":{"e":"markPriceUpdate",)"
        R"("s":"BTCUSDT","p":"64010.0","r":"0.00012","T":1710000000000}})";
    emitted = 0;
    CHECK(bn.parser(mp, 7, emit) == 1);
    CHECK(got.kind == UpdKind::Funding);
    CHECK_NEAR(got.funding_rate, 0.00012, 1e-9);
    CHECK(std::string(got.symbol) == "BTCUSDT");

    // Unrelated frame yields nothing.
    CHECK(bn.parser(R"({"stream":"x","data":{"e":"other"}})", 1, emit) == 0);
}

int main(int argc, char** argv) {
    bool live = (argc > 1 && std::string(argv[1]) == "--live");
    test_clock();
    test_ring_basic();
    test_ring_threaded();
    test_json();
    test_risk();
    test_fill();
    test_parsers();
    test_binance_parser();
    test_signal();
    test_hist();
    test_json_noalloc();
    if (live) test_live();
    else std::cout << "(skipping live network test; run with --live)\n";

    std::cout << "\n" << (g_fail == 0 ? "ALL PASS" : "SOME FAILED")
              << "  passed=" << g_pass << " failed=" << g_fail << "\n";
    return g_fail == 0 ? 0 : 1;
}
