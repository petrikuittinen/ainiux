#pragma once

#include <string>
#include <string_view>

#include "pdf/pdf.hpp"

namespace ainiux::pdf {

Error layout_markdown(std::string_view markdown, WriteOptions& options, std::string& pdf);

}  // namespace ainiux::pdf
