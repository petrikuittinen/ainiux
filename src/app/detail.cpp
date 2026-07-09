#include "app/detail.hpp"

#include <utility>

#include "common.hpp"

namespace pkchat::app::detail {

std::string trim_ascii(std::string text) { return pkchat::ascii_trim(std::move(text)); }

}  // namespace pkchat::app::detail
