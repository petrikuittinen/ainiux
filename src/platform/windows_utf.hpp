#pragma once

#include <cstdint>
#include <string>

#include "common.hpp"

namespace ainiux::platform {

#if defined(_WIN32)
Error utf8_to_utf16(const std::string& input, std::wstring& output);
Error utf16_to_utf8(const std::wstring& input, std::string& output);
std::string windows_error_message(unsigned long code);
#endif

}  // namespace ainiux::platform
