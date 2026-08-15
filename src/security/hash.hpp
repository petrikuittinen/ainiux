#pragma once

#include <string>

namespace ainiux::security {

// Lowercase SHA-256 digest for content-addressed storage and approval identity.
std::string sha256_hex(const std::string& bytes);

}  // namespace ainiux::security
