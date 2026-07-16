#pragma once
// TLS 1.3 transport for star://, built on OpenSSL. TlsContext wraps a shared
// SSL_CTX (server or client); TlsConn is one handshaken connection implementing
// the Conn interface so STWP message code reads/writes it like a plain socket.

#include "conn.hpp"
#include <openssl/ssl.h>
#include <string>
#include <memory>

// Handshake facts captured for the UI (lock indicator, cert viewer) and policy.
struct TlsInfo {
    std::string version;        // e.g. "TLSv1.3"
    std::string cipher;         // e.g. "TLS_AES_256_GCM_SHA384"
    std::string alpn;           // negotiated ALPN protocol, e.g. "stwp/1.0"
    std::string peer_subject;   // server leaf subject
    std::string peer_issuer;    // signing CA
    std::string not_before;
    std::string not_after;
    long verify_result = 0;     // X509_V_OK on success
    bool verified = false;
};

class TlsContext {
public:
    // TLS 1.3-only server context serving the given cert chain + private key.
    static std::unique_ptr<TlsContext> make_server(const std::string& cert_path,
                                                    const std::string& key_path,
                                                    std::string& err);
    // TLS 1.3-only client context trusting only the given CA bundle.
    static std::unique_ptr<TlsContext> make_client(const std::string& ca_path,
                                                    std::string& err);
    ~TlsContext();
    TlsContext(const TlsContext&) = delete;
    TlsContext& operator=(const TlsContext&) = delete;

    SSL_CTX* raw() const { return ctx_; }

private:
    explicit TlsContext(SSL_CTX* ctx) : ctx_(ctx) {}
    SSL_CTX* ctx_ = nullptr;
};

class TlsConn : public Conn {
public:
    // Server side: run SSL_accept over an already-accepted TCP socket.
    static std::unique_ptr<TlsConn> accept(TlsContext& ctx, net::socket_t fd,
                                           std::string& err);
    // Client side: run SSL_connect over a connected TCP socket, sending SNI and
    // enforcing hostname verification against `hostname`.
    static std::unique_ptr<TlsConn> connect(TlsContext& ctx, net::socket_t fd,
                                            const std::string& hostname,
                                            std::string& err);
    ~TlsConn() override;

    net::ssize_t_ read(void* buf, size_t len) override;
    net::ssize_t_ write(const void* buf, size_t len) override;
    net::socket_t fd() const override { return fd_; }
    void close() override;

    const TlsInfo& info() const { return info_; }

private:
    TlsConn(SSL* ssl, net::socket_t fd) : ssl_(ssl), fd_(fd) {}
    void capture_info();

    SSL* ssl_ = nullptr;
    net::socket_t fd_ = net::kInvalidSocket;
    TlsInfo info_;
};
