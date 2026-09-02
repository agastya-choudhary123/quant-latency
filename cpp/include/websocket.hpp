// Real RFC-6455 WebSocket client over TLS. Performs the HTTP upgrade with a
// random Sec-WebSocket-Key, verifies the server's Sec-WebSocket-Accept, masks
// all client frames (required by spec), reassembles fragmented messages, and
// answers pings with pongs. No permessage-deflate is negotiated, so inbound
// frames are plain text/JSON.
#pragma once
#include "tls.hpp"
#include <openssl/sha.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <string>
#include <vector>
#include <cstdint>
#include <cstring>

namespace fa {

class WebSocket {
public:
    // Connect wss://host:port/path and subscribe (send `subscribe_msg` if set).
    bool connect(const std::string& host, const std::string& port,
                 const std::string& path) {
        if (!tls_.connect(host, port)) return false;
        return handshake(host, path);
    }

    bool send_text(const std::string& payload) {
        return send_frame(0x1, payload);
    }

    // Read one full application message into `out`. Handles control frames
    // (ping->pong, close) internally and continuation/fragmentation. Returns
    // false on error/close/timeout.
    bool recv_message(std::string& out) {
        out.clear();
        bool in_fragment = false;
        for (;;) {
            uint8_t h2[2];
            if (!read_exact(h2, 2)) return false;
            bool fin    = h2[0] & 0x80;
            uint8_t op  = h2[0] & 0x0f;
            bool masked = h2[1] & 0x80;      // servers must NOT mask
            uint64_t len = h2[1] & 0x7f;
            if (len == 126) {
                uint8_t e[2]; if (!read_exact(e, 2)) return false;
                len = (uint64_t(e[0]) << 8) | e[1];
            } else if (len == 127) {
                uint8_t e[8]; if (!read_exact(e, 8)) return false;
                len = 0; for (int i = 0; i < 8; ++i) len = (len << 8) | e[i];
            }
            uint8_t mkey[4] = {0,0,0,0};
            if (masked && !read_exact(mkey, 4)) return false;

            std::string payload(len, '\0');
            if (len && !read_exact(reinterpret_cast<uint8_t*>(&payload[0]), len))
                return false;
            if (masked)
                for (uint64_t i = 0; i < len; ++i) payload[i] ^= mkey[i & 3];

            switch (op) {
                case 0x0:  // continuation
                    out += payload;
                    if (fin) return true;
                    break;
                case 0x1:  // text
                case 0x2:  // binary
                    out += payload;
                    if (fin) return true;
                    in_fragment = true; (void)in_fragment;
                    break;
                case 0x8:  // close
                    return false;
                case 0x9:  // ping -> pong
                    send_frame(0xA, payload);
                    break;
                case 0xA:  // pong
                    break;
                default:
                    return false;
            }
        }
    }

    uint64_t tcp_connect_ns()   const { return tls_.tcp_connect_ns(); }
    uint64_t tls_handshake_ns() const { return tls_.tls_handshake_ns(); }
    uint64_t ws_upgrade_ns()    const { return upgrade_ns_; }
    void set_recv_timeout_ms(int ms) { tls_.set_recv_timeout_ms(ms); }

private:
    bool handshake(const std::string& host, const std::string& path) {
        uint8_t raw[16];
        RAND_bytes(raw, sizeof(raw));
        std::string key = b64(raw, sizeof(raw));
        std::string req =
            "GET " + path + " HTTP/1.1\r\n"
            "Host: " + host + "\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Key: " + key + "\r\n"
            "Sec-WebSocket-Version: 13\r\n\r\n";
        uint64_t t0 = Clock::ticks();
        if (tls_.write(req.data(), req.size()) <= 0) return false;

        // Read headers until CRLFCRLF.
        std::string resp;
        char c;
        while (resp.find("\r\n\r\n") == std::string::npos) {
            int r = tls_.read(&c, 1);
            if (r <= 0) return false;
            resp.push_back(c);
            if (resp.size() > 8192) return false;
        }
        upgrade_ns_ = Clock::ticks_to_ns(Clock::ticks() - t0);
        if (resp.find(" 101 ") == std::string::npos) return false;
        // Verify Sec-WebSocket-Accept = base64(sha1(key + GUID)).
        std::string expect = accept_for(key);
        return resp.find(expect) != std::string::npos;
    }

    bool send_frame(uint8_t opcode, const std::string& payload) {
        std::vector<uint8_t> f;
        f.push_back(0x80 | opcode);   // FIN + opcode
        uint64_t n = payload.size();
        if (n < 126) {
            f.push_back(0x80 | uint8_t(n));           // MASK + len
        } else if (n <= 0xffff) {
            f.push_back(0x80 | 126);
            f.push_back((n >> 8) & 0xff); f.push_back(n & 0xff);
        } else {
            f.push_back(0x80 | 127);
            for (int i = 7; i >= 0; --i) f.push_back((n >> (8*i)) & 0xff);
        }
        uint8_t mkey[4];
        RAND_bytes(mkey, 4);
        f.insert(f.end(), mkey, mkey + 4);
        size_t off = f.size();
        f.resize(off + n);
        for (uint64_t i = 0; i < n; ++i)
            f[off + i] = uint8_t(payload[i]) ^ mkey[i & 3];
        size_t sent = 0;
        while (sent < f.size()) {
            int w = tls_.write(f.data() + sent, f.size() - sent);
            if (w <= 0) return false;
            sent += w;
        }
        return true;
    }

    bool read_exact(uint8_t* buf, size_t n) {
        size_t got = 0;
        while (got < n) {
            int r = tls_.read(buf + got, n - got);
            if (r <= 0) return false;
            got += r;
        }
        return true;
    }

    static std::string b64(const uint8_t* data, size_t n) {
        int out_len = 4 * ((n + 2) / 3);
        std::string out(out_len, '\0');
        EVP_EncodeBlock(reinterpret_cast<uint8_t*>(&out[0]), data, (int)n);
        return out;
    }

    static std::string accept_for(const std::string& key) {
        static const char* GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
        std::string s = key + GUID;
        uint8_t digest[SHA_DIGEST_LENGTH];
        SHA1(reinterpret_cast<const uint8_t*>(s.data()), s.size(), digest);
        return b64(digest, SHA_DIGEST_LENGTH);
    }

    TlsClient tls_;
    uint64_t  upgrade_ns_ = 0;
};

} // namespace fa
