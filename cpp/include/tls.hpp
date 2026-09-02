// Real TLS client over a connected TCP socket (OpenSSL 3). Does SNI, verifies
// the handshake completes, and measures real TLS-handshake latency. Exchange WS
// endpoints are all wss:// so this sits under the WebSocket layer.
#pragma once
#include "net.hpp"
#include "clock.hpp"
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <string>
#include <stdexcept>

namespace fa {

class TlsClient {
public:
    TlsClient() {
        static bool inited = [] {
            SSL_library_init();
            SSL_load_error_strings();
            OpenSSL_add_all_algorithms();
            return true;
        }();
        (void)inited;
        ctx_ = SSL_CTX_new(TLS_client_method());
        if (!ctx_) throw std::runtime_error("SSL_CTX_new failed");
        SSL_CTX_set_min_proto_version(ctx_, TLS1_2_VERSION);
    }
    ~TlsClient() {
        if (ssl_) { SSL_shutdown(ssl_); SSL_free(ssl_); }
        if (ctx_) SSL_CTX_free(ctx_);
    }
    TlsClient(const TlsClient&) = delete;
    TlsClient& operator=(const TlsClient&) = delete;

    // Connect TCP then run the TLS handshake over it. host is used for SNI.
    bool connect(const std::string& host, const std::string& port) {
        if (!tcp_.connect(host, port)) return false;
        ssl_ = SSL_new(ctx_);
        if (!ssl_) return false;
        SSL_set_fd(ssl_, tcp_.fd());
        SSL_set_tlsext_host_name(ssl_, host.c_str());  // SNI
        X509_VERIFY_PARAM* vp = SSL_get0_param(ssl_);
        X509_VERIFY_PARAM_set1_host(vp, host.c_str(), 0);

        uint64_t t0 = Clock::ticks();
        int rc = SSL_connect(ssl_);
        handshake_ns_ = Clock::ticks_to_ns(Clock::ticks() - t0);
        return rc == 1;
    }

    int write(const void* buf, size_t n) { return SSL_write(ssl_, buf, (int)n); }
    int read(void* buf, size_t n)        { return SSL_read(ssl_, buf, (int)n); }

    void set_recv_timeout_ms(int ms) { tcp_.set_recv_timeout_ms(ms); }

    uint64_t tcp_connect_ns()   const { return tcp_.connect_latency_ns(); }
    uint64_t tls_handshake_ns() const { return handshake_ns_; }
    bool valid() const { return ssl_ != nullptr; }

private:
    TcpSocket tcp_;
    SSL_CTX*  ctx_ = nullptr;
    SSL*      ssl_ = nullptr;
    uint64_t  handshake_ns_ = 0;
};

} // namespace fa
