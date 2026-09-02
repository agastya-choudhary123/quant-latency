// Real market-data feed. One Feed == one live wss:// connection to a real
// exchange. It is a *pure producer*: on its own thread it receives real frames,
// stamps the real arrival time from the ns clock, parses the venue-specific JSON
// into a common POD MarketUpdate, and pushes it into the lock-free SPSC ring.
// All latency statistics, routing and risk live on the consumer side, so the
// hot hand-off stays lock-free.
#pragma once
#include "clock.hpp"
#include "spsc_ring.hpp"
#include "websocket.hpp"
#include "json_scan.hpp"
#include <atomic>
#include <thread>
#include <string>
#include <vector>
#include <functional>
#include <cstring>

namespace fa {

enum class UpdKind : uint8_t { Book = 0, Funding = 1 };

// Fixed-size POD so it lives in the ring with no indirection.
struct MarketUpdate {
    char     venue[12]  = {0};   // must fit "coinbase" (+NUL); do not shrink
    char     symbol[20] = {0};
    double   bid = 0, ask = 0, last = 0;
    double   bid_sz = 0, ask_sz = 0;   // top-of-book sizes (for microprice)
    double   funding_rate = 0;
    uint64_t exch_ns = 0;    // exchange-side timestamp if present (ms->ns)
    uint64_t recv_ns = 0;    // our real monotonic arrival time
    UpdKind  kind = UpdKind::Book;
};

inline void set_cstr(char* dst, size_t cap, std::string_view s) {
    size_t n = s.size() < cap - 1 ? s.size() : cap - 1;
    std::memcpy(dst, s.data(), n);
    dst[n] = 0;
}

// Parser: turn one raw message into zero or more updates (pushed via emit).
// Returns number emitted. Non-data messages (acks/status) return 0.
using Parser = std::function<int(std::string_view msg, uint64_t recv_ns,
                                 const std::function<void(const MarketUpdate&)>& emit)>;

struct VenueSpec {
    std::string venue;
    std::string host;
    std::string port;
    std::string path;
    std::vector<std::string> subscribes;
    Parser parser;
};

class Feed {
public:
    Feed(VenueSpec spec, SpscRing<MarketUpdate>* ring)
        : spec_(std::move(spec)), ring_(ring) {}
    ~Feed() { stop(); }

    bool start() {
        if (!ws_.connect(spec_.host, spec_.port, spec_.path)) return false;
        ws_.set_recv_timeout_ms(5000);
        for (auto& s : spec_.subscribes)
            if (!ws_.send_text(s)) return false;
        connected_.store(true, std::memory_order_release);
        run_.store(true, std::memory_order_release);
        th_ = std::thread([this] { loop(); });
        return true;
    }

    void stop() {
        run_.store(false, std::memory_order_release);
        if (th_.joinable()) th_.join();
    }

    bool connected() const { return connected_.load(std::memory_order_acquire); }
    uint64_t messages() const { return msgs_.load(std::memory_order_acquire); }
    uint64_t drops() const { return drops_.load(std::memory_order_acquire); }

    // Handshake latencies (measured once at connect).
    uint64_t tcp_connect_ns()   const { return ws_.tcp_connect_ns(); }
    uint64_t tls_handshake_ns() const { return ws_.tls_handshake_ns(); }
    uint64_t ws_upgrade_ns()    const { return ws_.ws_upgrade_ns(); }
    const std::string& venue() const { return spec_.venue; }

private:
    void loop() {
        std::string msg;
        auto emit = [this](const MarketUpdate& u) {
            if (!ring_->push(u)) drops_.fetch_add(1, std::memory_order_relaxed);
        };
        while (run_.load(std::memory_order_acquire)) {
            if (!ws_.recv_message(msg)) {
                // timeout or disconnect; keep looping until asked to stop
                if (!run_.load(std::memory_order_acquire)) break;
                continue;
            }
            uint64_t recv_ns = Clock::now_ns();
            int n = spec_.parser(msg, recv_ns, emit);
            if (n > 0) msgs_.fetch_add(n, std::memory_order_relaxed);
        }
        connected_.store(false, std::memory_order_release);
    }

    VenueSpec spec_;
    SpscRing<MarketUpdate>* ring_;
    WebSocket ws_;
    std::thread th_;
    std::atomic<bool> run_{false};
    std::atomic<bool> connected_{false};
    std::atomic<uint64_t> msgs_{0};
    std::atomic<uint64_t> drops_{0};
};

} // namespace fa
