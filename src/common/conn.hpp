#pragma once
// A stream connection the STWP message code can read/write without caring
// whether it's plaintext TCP (moon://) or TLS (star://). PlainConn is the
// plaintext transport; TlsConn (tls.hpp) is the encrypted one.

#include "net.hpp"
#include <string>

struct Conn {
    virtual ~Conn() = default;

    // Return bytes moved, 0 on clean close, <0 on error — matching recv/send.
    virtual net::ssize_t_ read(void* buf, size_t len) = 0;
    virtual net::ssize_t_ write(const void* buf, size_t len) = 0;

    // The underlying socket, exposed so a fetch can be cancelled from another
    // thread by closing the fd out from under the transport.
    virtual net::socket_t fd() const = 0;

    virtual void close() = 0;
};

struct PlainConn : Conn {
    explicit PlainConn(net::socket_t s) : sock_(s) {}
    ~PlainConn() override { close(); }

    net::ssize_t_ read(void* buf, size_t len) override {
        return recv(sock_, (char*)buf, (int)len, 0);
    }
    net::ssize_t_ write(const void* buf, size_t len) override {
        return send(sock_, (const char*)buf, (int)len, 0);
    }
    net::socket_t fd() const override { return sock_; }
    void close() override {
        if (net::is_valid(sock_)) {
            net::close(sock_);
            sock_ = net::kInvalidSocket;
        }
    }

private:
    net::socket_t sock_ = net::kInvalidSocket;
};

// Write an entire buffer, looping over short writes. False on any error.
inline bool write_all(Conn& c, const void* data, size_t len) {
    const char* p = (const char*)data;
    size_t sent = 0;
    while (sent < len) {
        net::ssize_t_ n = c.write(p + sent, len - sent);
        if (n <= 0) return false;
        sent += (size_t)n;
    }
    return true;
}
