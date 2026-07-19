#pragma once

#include <string>

#include "agent/index/index.hpp"

namespace ainiux::agent::index {

ScanResult scan_additional_source(const std::string& source, Language language);

}  // namespace ainiux::agent::index
