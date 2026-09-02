// Minimal allocation-light JSON field scanning for exchange messages. We never
// build a DOM on the hot path — we scan for the specific keys we need, and we do
// it WITHOUT allocating: no std::string is constructed per field. Handles both
// quoted ("markPx":"123.4") and bare ("price":123.4) numbers.
//
// Two hot-path optimizations over the original scalar version:
//
//   1. Quote scanning is SIMD. Finding the next '"' is the inner loop of key
//      lookup; on Apple Silicon (aarch64) we scan 16 bytes per iteration with a
//      hand-rolled NEON compare. On other targets we use memchr, which is itself
//      vectorized in libc. (The NEON path is the primary target; memchr is a
//      portability fallback — see json_find_quote.)
//
//   2. Number parsing does NOT call strtod on the hot path. strtod is
//      locale-aware (it consults the C locale for the decimal point on every
//      call) and comparatively slow. json_fast_atof parses the bounded numeric
//      token directly with an exact fast path (mantissa in a u64 * an exact
//      power of ten). It only defers to strtod for the rare pathological token
//      (>19 significant digits or |exp|>22), where naive accumulation could
//      double-round; that fallback is noted here and reported in the README.
#pragma once
#include <string>
#include <string_view>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <cstdint>

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

namespace fa {

// Scan for the next occurrence of '"' at or after `from`. SIMD on aarch64.
inline size_t json_find_quote(std::string_view js, size_t from) {
    const size_t n = js.size();
    if (from >= n) return std::string_view::npos;
    const char* base = js.data();
#if defined(__aarch64__)
    size_t i = from;
    const uint8x16_t q = vdupq_n_u8('"');
    // 16 bytes at a time. We only vectorize the bulk; the <16 tail is scalar.
    for (; i + 16 <= n; i += 16) {
        uint8x16_t chunk = vld1q_u8(reinterpret_cast<const uint8_t*>(base + i));
        uint8x16_t eq = vceqq_u8(chunk, q);
        // Cheap "any match?" reduction: max across lanes is 0xFF iff a lane hit.
        if (vmaxvq_u8(eq) != 0) {
            // Locate the first hit within the 16-byte block.
            for (size_t j = 0; j < 16; ++j)
                if (base[i + j] == '"') return i + j;
        }
    }
    for (; i < n; ++i) if (base[i] == '"') return i;
    return std::string_view::npos;
#else
    const void* hit = std::memchr(base + from, '"', n - from);
    return hit ? size_t(static_cast<const char*>(hit) - base) : std::string_view::npos;
#endif
}

// Exact powers of ten that are representable in a double with no rounding error.
// 10^0 .. 10^22 are all exact (well within the 2^53 mantissa). Beyond that the
// fast path bails to strtod.
inline double json_pow10_exact(int e) {
    static const double kPow10[] = {
        1e0,1e1,1e2,1e3,1e4,1e5,1e6,1e7,1e8,1e9,1e10,1e11,
        1e12,1e13,1e14,1e15,1e16,1e17,1e18,1e19,1e20,1e21,1e22};
    return kPow10[e];
}

// Parse a bounded numeric token [p, p+len) into `out`. Returns false on a
// malformed token. Fast path (the overwhelmingly common case for exchange
// prices/sizes) is exact and strtod-free; the rare slow path defers to strtod.
inline bool json_fast_atof(const char* p, size_t len, double& out) {
    if (len == 0) return false;
    const char* end = p + len;
    const char* c = p;

    bool neg = false;
    if (*c == '+' || *c == '-') { neg = (*c == '-'); ++c; }

    uint64_t mant = 0;
    int mant_digits = 0;     // significant digits accumulated into `mant`
    int frac_exp = 0;        // digits seen after the decimal point
    bool any_digit = false;
    bool overflow_digits = false;

    for (; c < end && *c >= '0' && *c <= '9'; ++c) {
        any_digit = true;
        if (mant_digits < 19) { mant = mant * 10 + uint64_t(*c - '0'); ++mant_digits; }
        else overflow_digits = true;   // too many digits for the u64 fast path
    }
    if (c < end && *c == '.') {
        ++c;
        for (; c < end && *c >= '0' && *c <= '9'; ++c) {
            any_digit = true;
            if (mant_digits < 19) {
                mant = mant * 10 + uint64_t(*c - '0'); ++mant_digits; ++frac_exp;
            } else overflow_digits = true;
        }
    }
    if (!any_digit) return false;

    int exp10 = 0;
    if (c < end && (*c == 'e' || *c == 'E')) {
        ++c;
        bool eneg = false;
        if (c < end && (*c == '+' || *c == '-')) { eneg = (*c == '-'); ++c; }
        if (c >= end || *c < '0' || *c > '9') return false;  // 'e' with no digits
        int ev = 0;
        for (; c < end && *c >= '0' && *c <= '9'; ++c) ev = ev * 10 + (*c - '0');
        exp10 = eneg ? -ev : ev;
    }
    if (c != end) return false;   // trailing garbage -> not a clean number

    int scale = exp10 - frac_exp;   // net power of ten to apply to `mant`

    // Fast, exactly-rounded path: mantissa fits in 53 bits and |scale| <= 22 so
    // the power of ten is itself exact. One rounding, correctly rounded.
    if (!overflow_digits && mant < (1ull << 53) && scale >= -22 && scale <= 22) {
        double v = double(mant);
        if (scale >= 0) v *= json_pow10_exact(scale);
        else            v /= json_pow10_exact(-scale);
        out = neg ? -v : v;
        return true;
    }

    // Slow path (rare): let strtod do the correctly-rounded heavy lifting on a
    // NUL-terminated copy. Reached only for >19 significant digits or |scale|>22.
    char buf[64];
    if (len >= sizeof buf) return false;
    std::memcpy(buf, p, len);
    buf[len] = '\0';
    char* se = nullptr;
    double v = std::strtod(buf, &se);
    if (se == buf) return false;
    out = v;
    return true;
}

// Find the position just after `"key":` in `js`, starting at `from`. Returns
// npos if not found. No allocation: matches the quoted key by scanning.
inline size_t json_find_value(std::string_view js, std::string_view key,
                              size_t from = 0) {
    const char* base = js.data();
    const size_t n = js.size();
    while (true) {
        size_t q = json_find_quote(js, from);
        if (q == std::string_view::npos) return std::string_view::npos;
        size_t after = q + 1;
        size_t key_end = after + key.size();
        if (key_end < n &&
            js.compare(after, key.size(), key) == 0 &&
            base[key_end] == '"') {
            // Found "key"; require a colon next (skipping spaces).
            size_t i = key_end + 1;
            // Unroll space-skipping: check first, then loop only if needed
            if (i < n && base[i] == ' ') {
                do { ++i; } while (i < n && base[i] == ' ');
            }
            if (i < n && base[i] == ':') return i + 1;
        }
        from = q + 1;
    }
}

// Find "key" and return the numeric value that follows the colon. `from` lets a
// caller resume scanning past an earlier match (e.g. arrays of objects).
inline bool json_number(std::string_view js, std::string_view key, double& out,
                        size_t from = 0) {
    size_t i = json_find_value(js, key, from);
    if (i == std::string_view::npos) return false;
    const char* base = js.data();
    const size_t n = js.size();
    // Skip leading spaces and quotes
    while (i < n && (base[i] == ' ' || base[i] == '"')) ++i;
    size_t start = i;
    // Scan number: branch-light digit/operator checking
    while (i < n) {
        char c = base[i];
        if ((c >= '0' && c <= '9') || c=='-' || c=='+' || c=='.' || c=='e' || c=='E') {
            ++i;
        } else {
            break;
        }
    }
    size_t len = i - start;
    if (len == 0 || len >= 63) return false;   // reject empty / absurd
    return json_fast_atof(base + start, len, out);
}

// Extract a string field value into out (no unescaping needed for our fields).
// This one still assigns into a std::string because callers keep the value; it
// is off the latency-critical number path.
inline bool json_string(std::string_view js, std::string_view key,
                        std::string& out, size_t from = 0) {
    size_t i = json_find_value(js, key, from);
    if (i == std::string_view::npos) return false;
    while (i < js.size() && js[i] == ' ') ++i;
    if (i >= js.size() || js[i] != '"') return false;
    ++i;
    size_t start = i;
    while (i < js.size() && js[i] != '"') ++i;
    out.assign(js.substr(start, i - start));
    return true;
}

} // namespace fa
