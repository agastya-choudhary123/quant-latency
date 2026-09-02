// Real pre-trade risk engine. Every intended order passes check() before it can
// be routed; fills update the book of record. Enforces per-order notional, net
// position per symbol, and portfolio gross-exposure limits, with a latching
// kill-switch. Pure/deterministic and fully unit-tested.
#pragma once
#include <string>
#include <unordered_map>
#include <cmath>

namespace fa {

struct RiskLimits {
    double max_order_notional = 25000.0;   // per single order
    double max_symbol_notional = 100000.0; // |net position| per symbol
    double max_gross_notional = 250000.0;  // sum |position| across symbols
};

enum class RiskDecision { Accept, RejectOrderSize, RejectSymbolLimit,
                          RejectGrossLimit, RejectKilled };

inline const char* to_string(RiskDecision d) {
    switch (d) {
        case RiskDecision::Accept:            return "ACCEPT";
        case RiskDecision::RejectOrderSize:   return "REJECT_ORDER_SIZE";
        case RiskDecision::RejectSymbolLimit: return "REJECT_SYMBOL_LIMIT";
        case RiskDecision::RejectGrossLimit:  return "REJECT_GROSS_LIMIT";
        case RiskDecision::RejectKilled:      return "REJECT_KILLED";
    }
    return "?";
}

class RiskEngine {
public:
    explicit RiskEngine(RiskLimits lim = {}) : lim_(lim) {}

    // signed_notional: +buy / -sell, in quote currency, at intended price.
    RiskDecision check(const std::string& symbol, double signed_notional) const {
        if (killed_) return RiskDecision::RejectKilled;
        if (std::fabs(signed_notional) > lim_.max_order_notional)
            return RiskDecision::RejectOrderSize;

        double cur = position(symbol);
        double proposed = cur + signed_notional;
        if (std::fabs(proposed) > lim_.max_symbol_notional)
            return RiskDecision::RejectSymbolLimit;

        // Recompute gross with the proposed change swapped in.
        double gross = 0.0;
        for (const auto& [s, p] : pos_)
            gross += std::fabs(s == symbol ? proposed : p);
        if (pos_.find(symbol) == pos_.end())
            gross += std::fabs(proposed);
        if (gross > lim_.max_gross_notional)
            return RiskDecision::RejectGrossLimit;

        return RiskDecision::Accept;
    }

    // Apply a fill that already passed check().
    void on_fill(const std::string& symbol, double signed_notional) {
        pos_[symbol] += signed_notional;
    }

    double position(const std::string& symbol) const {
        auto it = pos_.find(symbol);
        return it == pos_.end() ? 0.0 : it->second;
    }

    double gross_notional() const {
        double g = 0.0;
        for (const auto& [s, p] : pos_) { (void)s; g += std::fabs(p); }
        return g;
    }

    void kill() { killed_ = true; }
    void reset_kill() { killed_ = false; }
    bool killed() const { return killed_; }

private:
    RiskLimits lim_;
    std::unordered_map<std::string, double> pos_;
    bool killed_ = false;
};

} // namespace fa
