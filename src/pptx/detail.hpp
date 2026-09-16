#pragma once

#include <string>
#include <string_view>
#include <unordered_map>

#include "common.hpp"
#include "ooxml/xml.hpp"
#include "pptx/pptx.hpp"

namespace ainiux::pptx::detail {

inline constexpr const char* kPresentation =
    "http://schemas.openxmlformats.org/presentationml/2006/main";
inline constexpr const char* kPresentationStrict =
    "http://purl.oclc.org/ooxml/presentationml/main";
inline constexpr const char* kDrawing =
    "http://schemas.openxmlformats.org/drawingml/2006/main";
inline constexpr const char* kDrawingStrict =
    "http://purl.oclc.org/ooxml/drawingml/main";
inline constexpr const char* kOfficeRel =
    "http://schemas.openxmlformats.org/officeDocument/2006/relationships";
inline constexpr const char* kOfficeRelStrict =
    "http://purl.oclc.org/ooxml/officeDocument/relationships";
inline constexpr const char* kPackageRel =
    "http://schemas.openxmlformats.org/package/2006/relationships";

struct Relationship {
    std::string type;
    std::string target;
    bool external = false;
};
using RelationshipMap = std::unordered_map<std::string, Relationship>;

bool is_p(const ooxml::XmlEvent& event, std::string_view local);
bool is_a(const ooxml::XmlEvent& event, std::string_view local);
bool is_rel(const ooxml::XmlEvent& event, std::string_view local);
bool type_suffix(std::string_view type, std::string_view suffix);
const std::string* attr_local(const ooxml::XmlEvent& event, std::string_view local);
const std::string* attr_rid(const ooxml::XmlEvent& event, std::string_view local);
std::string relationships_part(std::string_view part);
Error parse_relationships(std::string_view xml,
                          runtime::CancellationToken cancellation,
                          RelationshipMap& relationships);
std::string xml_escape(std::string_view text, bool attribute = false);
std::string markdown_escape(std::string_view text, bool table_cell = false);
std::string normalize_extension(std::string_view part, std::string_view content_type,
                                std::string_view bytes = {});
std::string content_type_for_extension(std::string_view extension);
bool compressed_image_extension(std::string_view extension);
void diagnostic(Diagnostics* diagnostics, const std::string& message);

}  // namespace ainiux::pptx::detail
