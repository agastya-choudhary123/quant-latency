// Shared value types for the funding-arb engine.
#pragma once
#include <string>
#include <cstdint>

namespace fa {

using Nanos = int64_t;   // nanoseconds since epoch (monotonic in sim)

enum class Side { Buy, Sell };

inline const char* to_string(Side s) { return s == Side::Buy ? "BUY" : "SELL"; }

// A single funding observation for one symbol on one venue.
// funding_rate is the per-interval rate (fraction, e.g. 0.0001 = 1bp per 8h).
struct FundingTick {
    Nanos       ts = 0;
    std::string symbol;
    std::string venue;
    double      funding_rate = 0.0;  // per interval
    double      mark_price   = 0.0;  // perp mark
    double      spot_price   = 0.0;  // spot reference
};

// Result of submitting an order to a venue.
struct OrderResult {
    bool        accepted = false;
    std::string order_id;
    double      fill_price = 0.0;
    double      fill_qty   = 0.0;
    double      fee        = 0.0;    // absolute quote-currency fee
    Nanos       submit_ts  = 0;
    Nanos       ack_ts     = 0;      // when we received the ack
    std::string venue;
    std::string reject_reason;

    Nanos latency_ns() const { return ack_ts - submit_ts; }
};

struct Order {
    std::string symbol;
    Side        side = Side::Buy;
    double      qty  = 0.0;          // base units
    bool        is_perp = false;     // true = perp, false = spot
};

// A completed arbitrage position (long spot / short perp), fully accounted.
struct TradeRecord {
    Nanos  open_ts = 0;
    Nanos  close_ts = 0;
    std::string symbol;
    std::string spot_venue;
    std::string perp_venue;
    double notional = 0.0;           // quote-currency notional per leg
    double entry_funding_annual = 0.0;
    int    intervals_held = 0;
    double funding_collected = 0.0;  // gross funding received by short
    double fees = 0.0;               // entry+exit fees, both legs
    double slippage = 0.0;           // entry+exit slippage cost
    double pnl = 0.0;                // net
};

} // namespace fa
