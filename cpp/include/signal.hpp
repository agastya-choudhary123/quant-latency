// The consumer's hot-path computation: a market-maker style fair-value signal.
//
// This is deliberately NOT a taker strategy — it never crosses the spread, so it
// has no fee exposure to be "killed" by. It is the kind of arithmetic a pricing
// engine runs on every book update: from the two-sided top of book (prices and
// sizes) it computes the size-weighted fair value (microprice) and the order-book
// imbalance, and emits a fair value with a directional skew. What we measure is
// how fast the system turns an arriving tick into that decision — reaction
// latency — which is a real, honest low-latency metric independent of any P&L.
//
// Pure functions of a quote: unit-testable with no network.
#pragma once

namespace fa {

struct BookTop {
    double bid = 0, ask = 0;
    double bid_sz = 0, ask_sz = 0;

    bool valid() const {
        return bid > 0 && ask > 0 && ask >= bid && bid_sz > 0 && ask_sz > 0;
    }
    double mid() const { return 0.5 * (bid + ask); }
    double spread_bps() const {
        double m = mid();
        return m > 0 ? (ask - bid) / m * 1e4 : 0.0;
    }
};

// Order-book imbalance in [-1, +1]: >0 means more bid size (upward pressure).
// Use reciprocal to replace division with multiplication.
inline double imbalance(const BookTop& b) {
    double d = b.bid_sz + b.ask_sz;
    if (d <= 0) return 0.0;
    double inv_d = 1.0 / d;
    return (b.bid_sz - b.ask_sz) * inv_d;
}

// Microprice: the size-weighted mid. When the bid is larger, fair value sits
// closer to the ask (buyers will lift it), and vice versa. This is the standard
// weighting: price on each side is weighted by the OPPOSITE side's size.
// Use reciprocal to replace division with multiplication.
inline double microprice(const BookTop& b) {
    double d = b.bid_sz + b.ask_sz;
    if (d <= 0) return b.mid();
    double inv_d = 1.0 / d;
    return (b.bid * b.ask_sz + b.ask * b.bid_sz) * inv_d;
}

struct Signal {
    bool   valid = false;
    double fair = 0.0;        // microprice fair value
    double imb = 0.0;         // imbalance [-1,1]
    double edge_bps = 0.0;    // microprice deviation from mid, in bps (signed)
};

// The decision computed per tick. Cheap, branch-light, no allocation.
// Optimized to minimize divisions: compute reciprocals once and reuse.
inline Signal compute_signal(const BookTop& b) {
    Signal s;
    if (!b.valid()) return s;

    double d = b.bid_sz + b.ask_sz;
    double mid_val = 0.5 * (b.bid + b.ask);
    double inv_d = 1.0 / d;
    double inv_mid = 1.0 / mid_val;

    double mp = (b.bid * b.ask_sz + b.ask * b.bid_sz) * inv_d;

    s.valid = true;
    s.fair = mp;
    s.imb = (b.bid_sz - b.ask_sz) * inv_d;
    s.edge_bps = (mp - mid_val) * inv_mid * 1e4;
    return s;
}

} // namespace fa
