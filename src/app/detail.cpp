#include "app/detail.hpp"

#include <utility>

#include "common.hpp"

namespace ainiux::app::detail {

std::string trim_ascii(std::string text) { return ainiux::ascii_trim(std::move(text)); }

}  // namespace ainiux::app::detail
