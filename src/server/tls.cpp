#include "server/tls.hpp"

#include <algorithm>
#include <limits>

#if defined(AINIUX_HAS_OPENSSL)
#include <openssl/err.h>
#include <openssl/ssl.h>
#endif

namespace ainiux::server {

struct TlsContext::Impl {
#if defined(AINIUX_HAS_OPENSSL)
    struct ContextDeleter {
        void operator()(SSL_CTX* value) const { if (value != nullptr) SSL_CTX_free(value); }
    };
    std::unique_ptr<SSL_CTX, ContextDeleter> context;
#endif
};

struct TlsConnection::Impl {
#if defined(AINIUX_HAS_OPENSSL)
    struct ConnectionDeleter {
        void operator()(SSL* value) const { if (value != nullptr) SSL_free(value); }
    };
    TlsContext* context = nullptr;
    std::unique_ptr<SSL, ConnectionDeleter> connection;
#endif
};

TlsContext::TlsContext() : impl_(std::make_unique<Impl>()) {}
TlsContext::~TlsContext() = default;

bool TlsContext::available() {
#if defined(AINIUX_HAS_OPENSSL)
    return true;
#else
    return false;
#endif
}

bool TlsContext::enabled() const {
#if defined(AINIUX_HAS_OPENSSL)
    return impl_->context != nullptr;
#else
    return false;
#endif
}

Error TlsContext::initialize(const std::string& certificate_file,
                             const std::string& private_key_file) {
#if defined(AINIUX_HAS_OPENSSL)
    impl_->context.reset();
#endif
    if (certificate_file.empty() && private_key_file.empty()) return ok_error();
    if (certificate_file.empty() || private_key_file.empty()) {
        return {ErrorCode::BadArgs, "TLS requires both --tls-cert and --tls-key"};
    }
#if !defined(AINIUX_HAS_OPENSSL)
    return {ErrorCode::UnsupportedFeature,
            "this build has no TLS support; install OpenSSL development files and rebuild"};
#else
    ERR_clear_error();
    std::unique_ptr<SSL_CTX, Impl::ContextDeleter> context(SSL_CTX_new(TLS_server_method()));
    if (!context) return {ErrorCode::Tls, "could not initialize the TLS server context"};
    if (SSL_CTX_set_min_proto_version(context.get(), TLS1_2_VERSION) != 1) {
        return {ErrorCode::Tls, "could not require TLS 1.2 or newer"};
    }
    SSL_CTX_set_options(context.get(), SSL_OP_NO_COMPRESSION);
    SSL_CTX_set_session_cache_mode(context.get(), SSL_SESS_CACHE_OFF);
    // Encrypted keys are deliberately rejected instead of prompting on server stdin.
    SSL_CTX_set_default_passwd_cb(context.get(),
        [](char*, int, int, void*) -> int { return 0; });
    if (SSL_CTX_use_certificate_chain_file(context.get(), certificate_file.c_str()) != 1) {
        ERR_clear_error();
        return {ErrorCode::Tls, "could not load the PEM certificate chain from --tls-cert"};
    }
    if (SSL_CTX_use_PrivateKey_file(context.get(), private_key_file.c_str(), SSL_FILETYPE_PEM) != 1) {
        ERR_clear_error();
        return {ErrorCode::Tls, "could not load an unencrypted PEM private key from --tls-key"};
    }
    if (SSL_CTX_check_private_key(context.get()) != 1) {
        ERR_clear_error();
        return {ErrorCode::Tls, "--tls-cert and --tls-key do not contain a matching key pair"};
    }
    impl_->context = std::move(context);
    return ok_error();
#endif
}

TlsConnection::TlsConnection(TlsContext& context) : impl_(std::make_unique<Impl>()) {
#if defined(AINIUX_HAS_OPENSSL)
    impl_->context = &context;
#else
    (void)context;
#endif
}

TlsConnection::~TlsConnection() = default;

Error TlsConnection::attach(std::uintptr_t socket) {
#if !defined(AINIUX_HAS_OPENSSL)
    (void)socket;
    return {ErrorCode::UnsupportedFeature, "TLS is unavailable in this build"};
#else
    if (impl_->context == nullptr || !impl_->context->enabled()) {
        return {ErrorCode::Internal, "TLS connection has no initialized context"};
    }
    ERR_clear_error();
    impl_->connection.reset(SSL_new(impl_->context->impl_->context.get()));
    if (!impl_->connection) return {ErrorCode::Tls, "could not allocate a TLS connection"};
    if (socket > static_cast<std::uintptr_t>(std::numeric_limits<int>::max()) ||
        SSL_set_fd(impl_->connection.get(), static_cast<int>(socket)) != 1) {
        impl_->connection.reset();
        ERR_clear_error();
        return {ErrorCode::Tls, "could not attach the accepted socket to TLS"};
    }
    SSL_set_accept_state(impl_->connection.get());
    return ok_error();
#endif
}

Error TlsConnection::accept() {
#if !defined(AINIUX_HAS_OPENSSL)
    return {ErrorCode::UnsupportedFeature, "TLS is unavailable in this build"};
#else
    if (!impl_->connection) return {ErrorCode::Internal, "TLS connection is not attached"};
    for (int attempt = 0; attempt < 15; ++attempt) {
        ERR_clear_error();
        const int result = SSL_accept(impl_->connection.get());
        if (result == 1) return ok_error();
        const int error = SSL_get_error(impl_->connection.get(), result);
        if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE) continue;
        ERR_clear_error();
        return {ErrorCode::Tls, "TLS handshake failed"};
    }
    return {ErrorCode::Timeout, "TLS handshake timed out"};
#endif
}

int TlsConnection::read(char* output, int size, bool& retry) {
    retry = false;
#if !defined(AINIUX_HAS_OPENSSL)
    (void)output;
    (void)size;
    return -1;
#else
    ERR_clear_error();
    const int result = SSL_read(impl_->connection.get(), output, size);
    if (result > 0) return result;
    const int error = SSL_get_error(impl_->connection.get(), result);
    if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE) retry = true;
    if (error == SSL_ERROR_SYSCALL && result < 0) retry = true;
    if (error == SSL_ERROR_ZERO_RETURN) return 0;
    ERR_clear_error();
    return -1;
#endif
}

bool TlsConnection::write_all(const char* data, std::size_t size) {
#if !defined(AINIUX_HAS_OPENSSL)
    (void)data;
    (void)size;
    return false;
#else
    std::size_t written = 0;
    while (written < size) {
        const std::size_t remaining = size - written;
        const int chunk = remaining > static_cast<std::size_t>(std::numeric_limits<int>::max())
                              ? std::numeric_limits<int>::max()
                              : static_cast<int>(remaining);
        ERR_clear_error();
        const int count = SSL_write(impl_->connection.get(), data + written, chunk);
        if (count <= 0) {
            ERR_clear_error();
            return false;
        }
        written += static_cast<std::size_t>(count);
    }
    return true;
#endif
}

}  // namespace ainiux::server
