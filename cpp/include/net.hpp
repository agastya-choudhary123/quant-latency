// Real TCP client socket. getaddrinfo + connect, TCP_NODELAY (Nagle off — it
// matters for latency-sensitive request/response), and real connect-latency
// measurement via the ns clock.
#pragma once
#include "clock.hpp"
#include <string>
#include <stdexcept>
#include <cstring>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>

namespace fa {

class TcpSocket {
public:
    TcpSocket() = default;
    ~TcpSocket() { close_fd(); }
    TcpSocket(const TcpSocket&) = delete;
    TcpSocket& operator=(const TcpSocket&) = delete;
    TcpSocket(TcpSocket&& o) noexcept : fd_(o.fd_), connect_ns_(o.connect_ns_) { o.fd_ = -1; }

    // Blocking connect. Returns true on success; sets connect latency (ns).
    bool connect(const std::string& host, const std::string& port) {
        struct addrinfo hints{}, *res = nullptr;
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        if (getaddrinfo(host.c_str(), port.c_str(), &hints, &res) != 0) return false;

        uint64_t t0 = Clock::ticks();
        int fd = -1;
        for (auto* p = res; p; p = p->ai_next) {
            fd = ::socket(p->ai_family, p->ai_socktype, p->ai_protocol);
            if (fd < 0) continue;
            if (::connect(fd, p->ai_addr, p->ai_addrlen) == 0) break;
            ::close(fd); fd = -1;
        }
        freeaddrinfo(res);
        if (fd < 0) return false;
        connect_ns_ = Clock::ticks_to_ns(Clock::ticks() - t0);

        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        fd_ = fd;
        return true;
    }

    // Raw byte IO used by the TLS layer's BIO callbacks / plaintext transport.
    ssize_t send_raw(const void* buf, size_t n) { return ::send(fd_, buf, n, 0); }
    ssize_t recv_raw(void* buf, size_t n)       { return ::recv(fd_, buf, n, 0); }

    void set_recv_timeout_ms(int ms) {
        struct timeval tv{ ms / 1000, (ms % 1000) * 1000 };
        setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }

    int fd() const { return fd_; }
    uint64_t connect_latency_ns() const { return connect_ns_; }
    bool valid() const { return fd_ >= 0; }

private:
    void close_fd() { if (fd_ >= 0) { ::close(fd_); fd_ = -1; } }
    int fd_ = -1;
    uint64_t connect_ns_ = 0;
};

} // namespace fa
