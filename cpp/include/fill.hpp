// Paper fill model — THE ONLY SIMULATED COMPONENT. No real order is ever sent.
// A marketable order is filled by crossing the *real* observed top-of-book (buy
// at the real best ask, sell at the real best bid), so slippage/spread cost is
// taken from live market data rather than invented. Fees use the venue taker
// rate. This is what keeps the demo from touching real money while every other
// layer (networking, routing, risk, latency) stays real.
#pragma once
#include "types.hpp"
#include <string>

namespace fa {

struct PaperFill {
    bool   filled = false;
    double price = 0.0;      // real crossed price
    double qty = 0.0;        // base units
    double notional = 0.0;   // price * qty
    double fee = 0.0;
    std::string reason;      // set when not filled
};

// bid/ask are the current real top-of-book on the chosen venue.
inline PaperFill paper_fill(Side side, double target_notional,
                            double bid, double ask, double taker_fee) {
    PaperFill f;
    double px = (side == Side::Buy) ? ask : bid;   // cross the real spread
    if (px <= 0.0 || bid <= 0.0 || ask <= 0.0) {
        f.reason = "no_top_of_book";
        return f;
    }
    f.filled = true;
    f.price = px;
    f.qty = target_notional / px;
    f.notional = f.qty * px;
    f.fee = taker_fee * f.notional;
    return f;
}

// Close/execute a specific base quantity (used to unwind exactly what was opened
// so the spot/perp hedge stays matched).
inline PaperFill paper_fill_qty(Side side, double qty,
                                double bid, double ask, double taker_fee) {
    PaperFill f;
    double px = (side == Side::Buy) ? ask : bid;
    if (px <= 0.0 || bid <= 0.0 || ask <= 0.0) {
        f.reason = "no_top_of_book";
        return f;
    }
    f.filled = true;
    f.price = px;
    f.qty = qty;
    f.notional = qty * px;
    f.fee = taker_fee * f.notional;
    return f;
}

} // namespace fa
