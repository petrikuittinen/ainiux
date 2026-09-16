#pragma once

// Shared namespace facade for the bounded namespace-aware OOXML scanner.
#include "docx/xml.hpp"

namespace ainiux::ooxml {

using XmlName = docx::detail::XmlName;
using XmlAttribute = docx::detail::XmlAttribute;
using XmlEvent = docx::detail::XmlEvent;
using XmlLimits = docx::detail::XmlLimits;
using XmlCallback = docx::detail::XmlCallback;
using docx::detail::attribute;
using docx::detail::attribute_local;
using docx::detail::scan_xml;

}  // namespace ainiux::ooxml
