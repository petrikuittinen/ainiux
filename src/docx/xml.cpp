#include "docx/xml.hpp"

#include <cctype>
#include <cstdint>
#include <limits>
#include <new>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "html/html.hpp"

namespace ainiux::docx::detail {
namespace {

constexpr const char* kXmlNamespace = "http://www.w3.org/XML/1998/namespace";

Error xml_error(std::string message) {
    return {ErrorCode::FileRead, "invalid DOCX XML: " + std::move(message)};
}

void append_utf8(std::string& out, std::uint32_t cp) {
    if (cp <= 0x7fU) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7ffU) {
        out.push_back(static_cast<char>(0xc0U | (cp >> 6U)));
        out.push_back(static_cast<char>(0x80U | (cp & 0x3fU)));
    } else if (cp <= 0xffffU) {
        out.push_back(static_cast<char>(0xe0U | (cp >> 12U)));
        out.push_back(static_cast<char>(0x80U | ((cp >> 6U) & 0x3fU)));
        out.push_back(static_cast<char>(0x80U | (cp & 0x3fU)));
    } else {
        out.push_back(static_cast<char>(0xf0U | (cp >> 18U)));
        out.push_back(static_cast<char>(0x80U | ((cp >> 12U) & 0x3fU)));
        out.push_back(static_cast<char>(0x80U | ((cp >> 6U) & 0x3fU)));
        out.push_back(static_cast<char>(0x80U | (cp & 0x3fU)));
    }
}

bool valid_xml_codepoint(std::uint32_t cp) {
    return cp == 0x09U || cp == 0x0aU || cp == 0x0dU ||
           (cp >= 0x20U && cp <= 0xd7ffU) ||
           (cp >= 0xe000U && cp <= 0xfffdU) ||
           (cp >= 0x10000U && cp <= 0x10ffffU);
}

Error normalized_utf8(std::string_view input, std::string& utf8) {
    utf8.clear();
    try {
        if (input.size() >= 2 &&
            ((static_cast<unsigned char>(input[0]) == 0xffU &&
              static_cast<unsigned char>(input[1]) == 0xfeU) ||
             (static_cast<unsigned char>(input[0]) == 0xfeU &&
              static_cast<unsigned char>(input[1]) == 0xffU))) {
            const bool little = static_cast<unsigned char>(input[0]) == 0xffU;
            if ((input.size() - 2U) % 2U != 0) return xml_error("UTF-16 input has an odd byte count");
            utf8.reserve(input.size());
            auto unit = [&](std::size_t pos) {
                const std::uint16_t a = static_cast<unsigned char>(input[pos]);
                const std::uint16_t b = static_cast<unsigned char>(input[pos + 1]);
                return static_cast<std::uint16_t>(little ? a | (b << 8U) : (a << 8U) | b);
            };
            for (std::size_t pos = 2; pos < input.size(); pos += 2) {
                const std::uint16_t first = unit(pos);
                std::uint32_t cp = first;
                if (first >= 0xd800U && first <= 0xdbffU) {
                    if (pos + 3 >= input.size()) return xml_error("UTF-16 high surrogate is truncated");
                    const std::uint16_t second = unit(pos + 2);
                    if (second < 0xdc00U || second > 0xdfffU)
                        return xml_error("UTF-16 high surrogate is not paired");
                    cp = 0x10000U + ((static_cast<std::uint32_t>(first) - 0xd800U) << 10U) +
                         (static_cast<std::uint32_t>(second) - 0xdc00U);
                    pos += 2;
                } else if (first >= 0xdc00U && first <= 0xdfffU) {
                    return xml_error("UTF-16 low surrogate is unpaired");
                }
                if (!valid_xml_codepoint(cp)) return xml_error("UTF-16 contains an invalid XML character");
                append_utf8(utf8, cp);
            }
        } else {
            std::size_t start = input.size() >= 3 &&
                                        static_cast<unsigned char>(input[0]) == 0xefU &&
                                        static_cast<unsigned char>(input[1]) == 0xbbU &&
                                        static_cast<unsigned char>(input[2]) == 0xbfU
                                    ? 3U : 0U;
            utf8.assign(input.substr(start));
            std::size_t invalid = 0;
            if (!html::is_valid_utf8(utf8, &invalid))
                return xml_error("input is not valid UTF-8 at byte " + std::to_string(invalid));
            for (std::size_t pos = 0; pos < utf8.size();) {
                const unsigned char first = static_cast<unsigned char>(utf8[pos++]);
                std::uint32_t cp = first;
                if ((first & 0xe0U) == 0xc0U) {
                    cp = (first & 0x1fU) << 6U;
                    cp |= static_cast<unsigned char>(utf8[pos++]) & 0x3fU;
                } else if ((first & 0xf0U) == 0xe0U) {
                    cp = (first & 0x0fU) << 12U;
                    cp |= (static_cast<unsigned char>(utf8[pos++]) & 0x3fU) << 6U;
                    cp |= static_cast<unsigned char>(utf8[pos++]) & 0x3fU;
                } else if ((first & 0xf8U) == 0xf0U) {
                    cp = (first & 0x07U) << 18U;
                    cp |= (static_cast<unsigned char>(utf8[pos++]) & 0x3fU) << 12U;
                    cp |= (static_cast<unsigned char>(utf8[pos++]) & 0x3fU) << 6U;
                    cp |= static_cast<unsigned char>(utf8[pos++]) & 0x3fU;
                }
                if (!valid_xml_codepoint(cp))
                    return xml_error("input contains an invalid XML character");
            }
        }
    } catch (const std::bad_alloc&) {
        return {ErrorCode::Internal, "not enough memory to decode DOCX XML"};
    }
    return ok_error();
}

bool name_start(unsigned char ch) {
    return std::isalpha(ch) != 0 || ch == '_' || ch == ':' || ch >= 0x80U;
}

bool name_char(unsigned char ch) {
    return name_start(ch) || std::isdigit(ch) != 0 || ch == '-' || ch == '.';
}

bool parse_name(const std::string& xml, std::size_t& pos, std::string& name) {
    const std::size_t start = pos;
    if (pos >= xml.size() || !name_start(static_cast<unsigned char>(xml[pos]))) return false;
    ++pos;
    while (pos < xml.size() && name_char(static_cast<unsigned char>(xml[pos]))) ++pos;
    name = xml.substr(start, pos - start);
    return true;
}

void skip_space(const std::string& xml, std::size_t& pos) {
    while (pos < xml.size() && (xml[pos] == ' ' || xml[pos] == '\t' ||
                                xml[pos] == '\r' || xml[pos] == '\n')) ++pos;
}

Error decode_entities(std::string_view raw, std::string& decoded) {
    decoded.clear();
    try {
        decoded.reserve(raw.size());
        for (std::size_t pos = 0; pos < raw.size();) {
            if (raw[pos] != '&') {
                decoded.push_back(raw[pos++]);
                continue;
            }
            const std::size_t semi = raw.find(';', pos + 1);
            if (semi == std::string_view::npos || semi - pos > 16)
                return xml_error("entity reference is malformed");
            const std::string_view entity = raw.substr(pos + 1, semi - pos - 1);
            if (entity == "amp") decoded.push_back('&');
            else if (entity == "lt") decoded.push_back('<');
            else if (entity == "gt") decoded.push_back('>');
            else if (entity == "quot") decoded.push_back('"');
            else if (entity == "apos") decoded.push_back('\'');
            else if (!entity.empty() && entity.front() == '#') {
                bool hex = entity.size() > 1 && (entity[1] == 'x' || entity[1] == 'X');
                std::size_t digit = hex ? 2 : 1;
                if (digit == entity.size()) return xml_error("numeric entity has no digits");
                std::uint32_t cp = 0;
                for (; digit < entity.size(); ++digit) {
                    const unsigned char ch = static_cast<unsigned char>(entity[digit]);
                    unsigned value = 0;
                    if (ch >= '0' && ch <= '9') value = ch - '0';
                    else if (hex && ch >= 'a' && ch <= 'f') value = ch - 'a' + 10U;
                    else if (hex && ch >= 'A' && ch <= 'F') value = ch - 'A' + 10U;
                    else return xml_error("numeric entity has an invalid digit");
                    const unsigned base = hex ? 16U : 10U;
                    if (cp > (0x10ffffU - value) / base)
                        return xml_error("numeric entity is out of range");
                    cp = cp * base + value;
                }
                if (!valid_xml_codepoint(cp)) return xml_error("numeric entity is not a valid XML character");
                append_utf8(decoded, cp);
            } else {
                return xml_error("DTD-defined entity references are unsupported");
            }
            pos = semi + 1;
        }
    } catch (const std::bad_alloc&) {
        return {ErrorCode::Internal, "not enough memory to decode DOCX XML entities"};
    }
    return ok_error();
}

void split_qname(const std::string& qname, std::string& prefix, std::string& local) {
    const std::size_t colon = qname.find(':');
    if (colon == std::string::npos) {
        prefix.clear();
        local = qname;
    } else {
        prefix = qname.substr(0, colon);
        local = qname.substr(colon + 1);
    }
}

bool valid_qname(const std::string& qname) {
    const std::size_t first = qname.find(':');
    return first == std::string::npos ||
           (first != 0 && first + 1 < qname.size() &&
            qname.find(':', first + 1) == std::string::npos);
}

struct RawAttribute {
    std::string qname;
    std::string value;
};

}  // namespace

const std::string* attribute(const XmlEvent& event,
                             std::string_view uri,
                             std::string_view local) {
    for (const XmlAttribute& item : event.attributes) {
        if (item.name.uri == uri && item.name.local == local) return &item.value;
    }
    return nullptr;
}

const std::string* attribute_local(const XmlEvent& event, std::string_view local) {
    for (const XmlAttribute& item : event.attributes) {
        if (item.name.local == local) return &item.value;
    }
    return nullptr;
}

Error scan_xml(std::string_view bytes,
               const XmlLimits& limits,
               runtime::CancellationToken cancellation,
               const XmlCallback& callback) {
    std::string xml;
    Error error = normalized_utf8(bytes, xml);
    if (!error.ok()) return error;
    using Namespaces = std::unordered_map<std::string, std::string>;
    struct Frame { std::string qname; XmlName name; Namespaces namespaces; };
    std::vector<Frame> stack;
    Namespaces root_namespaces;
    root_namespaces.emplace("xml", kXmlNamespace);
    bool seen_root = false;
    bool closed_root = false;
    std::size_t pos = 0;
    try {
        while (pos < xml.size()) {
            if ((pos & 0xffffU) == 0 && cancellation.cancelled())
                return {ErrorCode::Cancelled, "DOCX XML scan cancelled"};
            if (xml[pos] != '<') {
                const std::size_t end = xml.find('<', pos);
                const std::size_t stop = end == std::string::npos ? xml.size() : end;
                if (stop - pos > limits.max_text_bytes) return xml_error("text node exceeds the 16 MiB limit");
                std::string text;
                error = decode_entities(std::string_view(xml).substr(pos, stop - pos), text);
                if (!error.ok()) return error;
                if (text.find("]]>" ) != std::string::npos)
                    return xml_error("text contains the forbidden ']]>' sequence");
                if (stack.empty()) {
                    if (text.find_first_not_of(" \t\r\n") != std::string::npos)
                        return xml_error("text appears outside the document element");
                } else if (!text.empty()) {
                    XmlEvent event;
                    event.kind = XmlEvent::Kind::Text;
                    event.text = std::move(text);
                    error = callback(event);
                    if (!error.ok()) return error;
                }
                pos = stop;
                continue;
            }
            if (xml.compare(pos, 4, "<!--") == 0) {
                const std::size_t end = xml.find("-->", pos + 4);
                if (end == std::string::npos || xml.find("--", pos + 4) < end)
                    return xml_error("comment is malformed");
                pos = end + 3;
                continue;
            }
            if (xml.compare(pos, 9, "<![CDATA[") == 0) {
                if (stack.empty()) return xml_error("CDATA appears outside the document element");
                const std::size_t end = xml.find("]]>", pos + 9);
                if (end == std::string::npos) return xml_error("CDATA section is unterminated");
                if (end - pos - 9 > limits.max_text_bytes)
                    return xml_error("CDATA text exceeds the 16 MiB limit");
                XmlEvent event;
                event.kind = XmlEvent::Kind::Text;
                event.text = xml.substr(pos + 9, end - pos - 9);
                error = callback(event);
                if (!error.ok()) return error;
                pos = end + 3;
                continue;
            }
            if (xml.compare(pos, 2, "<?") == 0) {
                const std::size_t end = xml.find("?>", pos + 2);
                if (end == std::string::npos) return xml_error("processing instruction is unterminated");
                pos = end + 2;
                continue;
            }
            if (xml.compare(pos, 2, "<!") == 0)
                return xml_error("DTD, entity declarations, and other markup declarations are forbidden");
            if (xml.compare(pos, 2, "</") == 0) {
                pos += 2;
                std::string qname;
                if (!parse_name(xml, pos, qname)) return xml_error("end tag has no valid name");
                if (!valid_qname(qname)) return xml_error("end tag has an invalid qualified name");
                skip_space(xml, pos);
                if (pos >= xml.size() || xml[pos++] != '>') return xml_error("end tag is malformed");
                if (stack.empty() || stack.back().qname != qname)
                    return xml_error("element nesting is malformed at </" + qname + ">");
                XmlEvent event;
                event.kind = XmlEvent::Kind::End;
                event.name = stack.back().name;
                error = callback(event);
                if (!error.ok()) return error;
                stack.pop_back();
                if (stack.empty()) closed_root = true;
                continue;
            }

            ++pos;
            std::string qname;
            if (!parse_name(xml, pos, qname)) return xml_error("start tag has no valid name");
            if (!valid_qname(qname)) return xml_error("start tag has an invalid qualified name");
            std::vector<RawAttribute> raw_attributes;
            std::unordered_set<std::string> raw_names;
            bool empty = false;
            for (;;) {
                skip_space(xml, pos);
                if (pos >= xml.size()) return xml_error("start tag is unterminated");
                if (xml[pos] == '>') {
                    ++pos;
                    break;
                }
                if (xml[pos] == '/' && pos + 1 < xml.size() && xml[pos + 1] == '>') {
                    pos += 2;
                    empty = true;
                    break;
                }
                if (raw_attributes.size() >= limits.max_attributes)
                    return xml_error("element exceeds the 256-attribute limit");
                RawAttribute raw;
                if (!parse_name(xml, pos, raw.qname)) return xml_error("attribute has no valid name");
                if (!valid_qname(raw.qname) || !raw_names.insert(raw.qname).second)
                    return xml_error("attribute has an invalid or duplicate qualified name");
                skip_space(xml, pos);
                if (pos >= xml.size() || xml[pos++] != '=') return xml_error("attribute is missing '='");
                skip_space(xml, pos);
                if (pos >= xml.size() || (xml[pos] != '\'' && xml[pos] != '"'))
                    return xml_error("attribute value is not quoted");
                const char quote = xml[pos++];
                const std::size_t end = xml.find(quote, pos);
                if (end == std::string::npos) return xml_error("attribute value is unterminated");
                if (xml.find('<', pos) < end) return xml_error("attribute value contains '<'");
                error = decode_entities(std::string_view(xml).substr(pos, end - pos), raw.value);
                if (!error.ok()) return error;
                pos = end + 1;
                raw_attributes.push_back(std::move(raw));
            }
            if (closed_root) return xml_error("multiple document elements are present");
            if (stack.size() >= limits.max_depth) return xml_error("element nesting exceeds the depth limit of 256");
            Namespaces namespaces = stack.empty() ? root_namespaces : stack.back().namespaces;
            for (const RawAttribute& raw : raw_attributes) {
                if (raw.qname == "xmlns") namespaces[""] = raw.value;
                else if (raw.qname.rfind("xmlns:", 0) == 0) {
                    const std::string prefix = raw.qname.substr(6);
                    if (prefix.empty() || prefix == "xmlns") return xml_error("namespace declaration is invalid");
                    namespaces[prefix] = raw.value;
                }
            }
            std::string prefix, local;
            split_qname(qname, prefix, local);
            if (local.empty()) return xml_error("qualified element name is invalid");
            const auto ns = namespaces.find(prefix);
            if (!prefix.empty() && ns == namespaces.end()) return xml_error("element uses an undeclared prefix");
            XmlEvent event;
            event.kind = XmlEvent::Kind::Start;
            event.name = {ns == namespaces.end() ? std::string() : ns->second, local};
            std::unordered_set<std::string> seen_attributes;
            for (RawAttribute& raw : raw_attributes) {
                if (raw.qname == "xmlns" || raw.qname.rfind("xmlns:", 0) == 0) continue;
                split_qname(raw.qname, prefix, local);
                if (local.empty()) return xml_error("qualified attribute name is invalid");
                std::string uri;
                if (!prefix.empty()) {
                    const auto attr_ns = namespaces.find(prefix);
                    if (attr_ns == namespaces.end()) return xml_error("attribute uses an undeclared prefix");
                    uri = attr_ns->second;
                }
                const std::string key = uri + '\0' + local;
                if (!seen_attributes.insert(key).second) return xml_error("duplicate expanded attribute name");
                event.attributes.push_back({{std::move(uri), local}, std::move(raw.value)});
            }
            if (!seen_root) seen_root = true;
            stack.push_back({qname, event.name, std::move(namespaces)});
            error = callback(event);
            if (!error.ok()) return error;
            if (empty) {
                XmlEvent end;
                end.kind = XmlEvent::Kind::End;
                end.name = stack.back().name;
                error = callback(end);
                if (!error.ok()) return error;
                stack.pop_back();
                if (stack.empty()) closed_root = true;
            }
        }
    } catch (const std::bad_alloc&) {
        return {ErrorCode::Internal, "not enough memory while scanning DOCX XML"};
    } catch (const std::length_error&) {
        return xml_error("XML structure is too large");
    }
    if (!seen_root || !stack.empty()) return xml_error("document element is missing or unclosed");
    return ok_error();
}

}  // namespace ainiux::docx::detail
