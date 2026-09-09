#pragma once

#include <string>

#include "common.hpp"
#include "pdf/document.hpp"
#include "pdf/pdf.hpp"

namespace ainiux::pdf {

Error extract_markdown(Document& document, const Options& options, std::string& markdown);

}  // namespace ainiux::pdf
