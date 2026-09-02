// Frame capture — the ONLY thing in this project that needs a live venue.
//
// An execution engine must not be benchmarked against a live market: the sample
// count is whatever the venue felt like sending, and the number changes every
// run, so no optimization can be A/B'd honestly. The industry answer is capture
// once, replay forever. This tool is the "capture once" half.
//
// It connects to OKX, records N raw frames verbatim to a file (one per line),
// and exits. After that the engine never needs the network again: bin/tick2order
// replays this file deterministically, so the same input always yields the same
// measurement and an optimization's effect is real rather than market noise.
//
// Re-run this only to refresh the corpus (e.g. to check the venue's message
// format has not drifted).
//
// Usage: capture <out_file> [--frames N] [--seconds S]
#include "clock.hpp"
#include "spsc_ring.hpp"
#include "websocket.hpp"
#include "venues.hpp"
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>

using namespace fa;

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: capture <out_file> [--frames N] [--seconds S]\n";
        return 2;
    }
    std::string out_path = argv[1];
    size_t want = 2000;
    int max_sec = 120;
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--frames" && i + 1 < argc) want = std::strtoull(argv[++i], nullptr, 10);
        else if (a == "--seconds" && i + 1 < argc) max_sec = std::atoi(argv[++i]);
    }

    VenueSpec spec = okx_perp_spec("BTC-USDT-SWAP");
    WebSocket ws;
    std::cout << "connecting to " << spec.host << spec.path << " ...\n";
    if (!ws.connect(spec.host, spec.port, spec.path)) {
        std::cerr << "connect failed\n";
        return 1;
    }
    ws.set_recv_timeout_ms(5000);
    for (auto& s : spec.subscribes) {
        if (!ws.send_text(s)) { std::cerr << "subscribe failed\n"; return 1; }
    }
    std::cout << "connected  tcp=" << ws.tcp_connect_ns() / 1e6
              << "ms tls=" << ws.tls_handshake_ns() / 1e6
              << "ms ws=" << ws.ws_upgrade_ns() / 1e6 << "ms\n";

    std::ofstream f(out_path, std::ios::binary | std::ios::trunc);
    if (!f) { std::cerr << "cannot open " << out_path << "\n"; return 1; }

    // Frames are newline-delimited. Exchange JSON never contains a raw newline,
    // so this is a safe framing for the corpus file.
    size_t kept = 0, skipped = 0;
    std::string msg;
    uint64_t deadline = Clock::now_ns() + uint64_t(max_sec) * 1000000000ull;
    while (kept < want && Clock::now_ns() < deadline) {
        if (!ws.recv_message(msg)) continue;          // timeout; keep waiting
        // Only keep data frames — subscribe acks and pings are not what the
        // hot path parses, and padding the corpus with them would understate
        // the real per-tick parse cost.
        if (msg.find("\"data\"") == std::string::npos) { ++skipped; continue; }
        if (msg.find('\n') != std::string::npos) { ++skipped; continue; }
        f << msg << '\n';
        ++kept;
        if (kept % 100 == 0) {
            std::cout << "\r  captured " << kept << "/" << want << std::flush;
        }
    }
    f.close();
    std::cout << "\ncaptured " << kept << " data frames (skipped "
              << skipped << " non-data) -> " << out_path << "\n";
    if (kept == 0) {
        std::cerr << "no frames captured; corpus not usable\n";
        return 1;
    }
    return 0;
}
