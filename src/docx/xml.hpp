#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "common.hpp"
#include "runtime/runtime.hpp"

namespace ainiux::docx::detail {

struct XmlName {
    std::string uri;
    std::string local;
};

struct XmlAttribute {
    XmlName name;
    std::string value;
};

struct XmlEvent {
    enum class Kind { Start, End, Text } kind = Kind::Text;
    XmlName name;
    std::vector<XmlAttribute> attributes;
    std::string text;
};

struct XmlLimits {
    std::size_t max_depth = 256;
    std::size_t max_attributes = 256;
    std::size_t max_text_bytes = 16U * 1024U * 1024U;
};

using XmlCallback = std::function<Error(const XmlEvent&)>;

Error scan_xml(std::string_view bytes,
               const XmlLimits& limits,
               runtime::CancellationToken cancellation,
               const XmlCallback& callback);

const std::string* attribute(const XmlEvent& event,
                             std::string_view uri,
                             std::string_view local);
const std::string* attribute_local(const XmlEvent& event, std::string_view local);

}  // namespace ainiux::docx::detail
