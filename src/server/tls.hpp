#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "common.hpp"

namespace ainiux::server {

// OpenSSL stays behind these RAII wrappers so listener ownership and cleanup do
// not depend on C handles. Builds without OpenSSL retain plain loopback support.
class TlsConnection;

class TlsContext {
   public:
    TlsContext();
    ~TlsContext();
    TlsContext(const TlsContext&) = delete;
    TlsContext& operator=(const TlsContext&) = delete;

    Error initialize(const std::string& certificate_file,
                     const std::string& private_key_file);
    bool enabled() const;
    static bool available();

   private:
    friend class TlsConnection;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class TlsConnection {
   public:
    explicit TlsConnection(TlsContext& context);
    ~TlsConnection();
    TlsConnection(const TlsConnection&) = delete;
    TlsConnection& operator=(const TlsConnection&) = delete;

    Error attach(std::uintptr_t socket);
    Error accept();
    int read(char* output, int size, bool& retry);
    bool write_all(const char* data, std::size_t size);

   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ainiux::server
