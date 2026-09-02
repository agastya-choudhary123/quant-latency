// Order representation + allocation-free wire encoding — the execution half of
// the tick-to-order path.
//
// Everything here runs AFTER the decision and BEFORE the syscall, so every
// nanosecond lands directly in the headline metric. Consequently:
//
//   * No std::string, no stream, no snprintf. snprintf parses a format string
//     and consults the locale at runtime; it costs hundreds of ns per call and
//     we call it several times per order. All number formatting is hand-rolled.
//   * Prices/sizes are formatted as fixed-decimal from a scaled integer, which
//     is both faster and avoids the float->shortest-repr problem (we must send
//     what the venue's tick size expects, not "6.4123500000000004e4").
//   * The caller owns the buffer. encode_* writes into it and returns the byte
//     count, so an engine can keep one buffer per session and never allocate.
#pragma once
#include "types.hpp"
#include <cstdint>
#include <cstring>
#include <cmath>

namespace fa {

// Fixed-size POD order request. Sized to sit in a ring or on the stack.
struct OrderReq {
    char     inst[24] = {0};   // e.g. "BTC-USDT-SWAP"
    Side     side = Side::Buy;
    double   px = 0.0;         // limit price
    double   sz = 0.0;         // size, in venue units (contracts/base)
    uint64_t cl_id = 0;        // client order id
    int      px_dp = 1;        // price decimals (venue tick size)
    int      sz_dp = 0;        // size decimals (venue lot size)
};

// ---- hand-rolled number formatting --------------------------------------

// Write an unsigned integer, no padding. Returns bytes written.
inline size_t fast_u64(char* p, uint64_t v) {
    if (v == 0) { *p = '0'; return 1; }
    char tmp[20];
    size_t n = 0;
    while (v) { tmp[n++] = char('0' + (v % 10)); v /= 10; }
    for (size_t i = 0; i < n; ++i) p[i] = tmp[n - 1 - i];
    return n;
}

inline uint64_t pow10_u64(int e) {
    static const uint64_t k[] = {1ull,10ull,100ull,1000ull,10000ull,100000ull,
        1000000ull,10000000ull,100000000ull,1000000000ull};
    return k[e];
}

// Write a non-negative value with exactly `dp` decimal places, from a scaled
// integer so we emit precisely what the venue's tick size expects.
inline size_t fast_fixed(char* p, double v, int dp) {
    if (dp < 0) dp = 0;
    if (dp > 9) dp = 9;
    uint64_t scale = pow10_u64(dp);
    // llround gives round-half-away-from-zero, which matches how venues round
    // a tick. Negative prices are not a thing here; clamp defensively.
    if (v < 0) v = 0;
    uint64_t scaled = uint64_t(std::llround(v * double(scale)));
    uint64_t ip = scaled / scale;
    uint64_t fp = scaled % scale;
    size_t n = fast_u64(p, ip);
    if (dp == 0) return n;
    p[n++] = '.';
    // zero-pad the fraction to exactly dp digits
    for (int i = dp - 1; i >= 0; --i) {
        p[n + i] = char('0' + (fp % 10));
        fp /= 10;
    }
    return n + size_t(dp);
}

// Append a literal without a strlen call at runtime.
#define FA_LIT(dst, off, s) do { \
    static const char _l[] = s; \
    std::memcpy((dst) + (off), _l, sizeof(_l) - 1); \
    (off) += sizeof(_l) - 1; \
} while (0)

// ---- OKX order encoding --------------------------------------------------
// OKX WebSocket order entry:
// {"id":"7","op":"order","args":[{"instId":"BTC-USDT-SWAP","tdMode":"cross",
//  "side":"buy","ordType":"limit","px":"64123.5","sz":"1"}]}
//
// Returns bytes written, or 0 if the buffer is too small.
inline size_t encode_okx_order(char* buf, size_t cap, const OrderReq& o) {
    // Worst case is well under 256B; bail early rather than risk a partial write.
    if (cap < 256) return 0;
    size_t n = 0;
    FA_LIT(buf, n, R"({"id":")");
    n += fast_u64(buf + n, o.cl_id);
    FA_LIT(buf, n, R"(","op":"order","args":[{"instId":")");
    size_t il = std::strlen(o.inst);
    std::memcpy(buf + n, o.inst, il); n += il;
    FA_LIT(buf, n, R"(","tdMode":"cross","side":")");
    if (o.side == Side::Buy) FA_LIT(buf, n, "buy");
    else                     FA_LIT(buf, n, "sell");
    FA_LIT(buf, n, R"(","ordType":"limit","px":")");
    n += fast_fixed(buf + n, o.px, o.px_dp);
    FA_LIT(buf, n, R"(","sz":")");
    n += fast_fixed(buf + n, o.sz, o.sz_dp);
    FA_LIT(buf, n, R"("}]})");
    return n;
}

#undef FA_LIT

} // namespace fa
