#include "pptx/detail.hpp"

#include <algorithm>
#include <cctype>

namespace ainiux::pptx::detail {
namespace {

constexpr std::size_t kMaxDiagnostics = 32;

bool namespace_is(std::string_view uri, const char* transitional, const char* strict) {
    return uri.empty() || uri == transitional || uri == strict;
}

std::string lower(std::string_view value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return result;
}

}  // namespace

bool is_p(const ooxml::XmlEvent& event, std::string_view local) {
    return event.name.local == local &&
           namespace_is(event.name.uri, kPresentation, kPresentationStrict);
}

bool is_a(const ooxml::XmlEvent& event, std::string_view local) {
    return event.name.local == local && namespace_is(event.name.uri, kDrawing, kDrawingStrict);
}

bool is_rel(const ooxml::XmlEvent& event, std::string_view local) {
    return event.name.local == local &&
           (event.name.uri.empty() || event.name.uri == kPackageRel);
}

bool type_suffix(std::string_view type, std::string_view suffix) {
    return type.size() >= suffix.size() &&
           type.compare(type.size() - suffix.size(), suffix.size(), suffix) == 0;
}

const std::string* attr_local(const ooxml::XmlEvent& event, std::string_view local) {
    return ooxml::attribute_local(event, local);
}

const std::string* attr_rid(const ooxml::XmlEvent& event, std::string_view local) {
    if (const std::string* value = ooxml::attribute(event, kOfficeRel, local)) return value;
    if (const std::string* value = ooxml::attribute(event, kOfficeRelStrict, local)) return value;
    return attr_local(event, local);
}

std::string relationships_part(std::string_view part) {
    const std::size_t slash = part.find_last_of('/');
    if (slash == std::string_view::npos) return "_rels/" + std::string(part) + ".rels";
    return std::string(part.substr(0, slash + 1)) + "_rels/" +
           std::string(part.substr(slash + 1)) + ".rels";
}

Error parse_relationships(std::string_view xml,
                          runtime::CancellationToken cancellation,
                          RelationshipMap& relationships) {
    relationships.clear();
    return ooxml::scan_xml(xml, {}, cancellation, [&](const ooxml::XmlEvent& event) -> Error {
        if (event.kind != ooxml::XmlEvent::Kind::Start || !is_rel(event, "Relationship"))
            return ok_error();
        const std::string* id = attr_local(event, "Id");
        const std::string* type = attr_local(event, "Type");
        const std::string* target = attr_local(event, "Target");
        if (id == nullptr || type == nullptr || target == nullptr || id->empty() || target->empty())
            return {ErrorCode::FileRead, "invalid PPTX relationship: Id, Type, or Target is missing"};
        Relationship relation;
        relation.type = *type;
        relation.target = *target;
        if (const std::string* mode = attr_local(event, "TargetMode"))
            relation.external = ascii_lower(*mode) == "external";
        if (!relationships.emplace(*id, std::move(relation)).second)
            return {ErrorCode::FileRead, "invalid PPTX relationships: duplicate Id " + *id};
        return ok_error();
    });
}

std::string xml_escape(std::string_view text, bool attribute) {
    std::string out;
    out.reserve(text.size());
    for (unsigned char ch : text) {
        switch (ch) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += attribute ? "&quot;" : "\""; break;
            case '\'': out += attribute ? "&apos;" : "'"; break;
            default:
                // XML 1.0 excludes these bytes even when the input Markdown
                // happens to contain them.
                if (ch == 0x09U || ch == 0x0aU || ch == 0x0dU || ch >= 0x20U)
                    out.push_back(static_cast<char>(ch));
                break;
        }
    }
    return out;
}

std::string markdown_escape(std::string_view text, bool table_cell) {
    std::string out;
    out.reserve(text.size() + 8);
    for (char ch : text) {
        if (ch == '\\' || ch == '*' || ch == '_' || ch == '`' || ch == '[' || ch == ']' ||
            ch == '<' || ch == '>' || ch == '~' || ch == '+') {
            out.push_back('\\');
        }
        if (table_cell && ch == '|') out.push_back('\\');
        if (ch == '\r') continue;
        if (ch == '\n' && table_cell) out += "<br>";
        else out.push_back(ch);
    }
    return out;
}

std::string normalize_extension(std::string_view part,
                                std::string_view content_type,
                                std::string_view bytes) {
    std::string type = lower(content_type);
    if (type == "image/png") return "png";
    if (type == "image/jpeg" || type == "image/jpg") return "jpg";
    if (type == "image/gif") return "gif";
    if (type == "image/bmp" || type == "image/x-ms-bmp") return "bmp";
    if (type == "image/tiff") return "tiff";
    if (type == "image/x-wmf" || type == "image/wmf") return "wmf";
    if (type == "image/x-emf" || type == "image/emf") return "emf";
    if (type == "image/svg+xml") return "svg";
    if (bytes.size() >= 8 && bytes.substr(0, 8) == "\x89PNG\r\n\x1a\n") return "png";
    if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xffU &&
        static_cast<unsigned char>(bytes[1]) == 0xd8U &&
        static_cast<unsigned char>(bytes[2]) == 0xffU) return "jpg";
    if (bytes.size() >= 6 && (bytes.substr(0, 6) == "GIF87a" || bytes.substr(0, 6) == "GIF89a"))
        return "gif";
    if (bytes.size() >= 2 && bytes.substr(0, 2) == "BM") return "bmp";
    const std::size_t dot = part.find_last_of('.');
    if (dot != std::string_view::npos && dot + 1 < part.size()) {
        const std::string ext = lower(part.substr(dot + 1));
        if (ext == "jpeg") return "jpg";
        if (ext == "tif") return "tiff";
        if (ext == "png" || ext == "jpg" || ext == "gif" || ext == "bmp" ||
            ext == "tiff" || ext == "wmf" || ext == "emf" || ext == "svg") return ext;
    }
    return {};
}

std::string content_type_for_extension(std::string_view extension) {
    const std::string ext = lower(extension);
    if (ext == "png") return "image/png";
    if (ext == "jpg" || ext == "jpeg") return "image/jpeg";
    if (ext == "gif") return "image/gif";
    if (ext == "bmp") return "image/bmp";
    if (ext == "tif" || ext == "tiff") return "image/tiff";
    if (ext == "wmf") return "image/x-wmf";
    if (ext == "emf") return "image/x-emf";
    if (ext == "svg") return "image/svg+xml";
    return {};
}

bool compressed_image_extension(std::string_view extension) {
    const std::string ext = lower(extension);
    return ext == "png" || ext == "jpg" || ext == "jpeg" || ext == "gif" ||
           ext == "tiff" || ext == "tif";
}

void diagnostic(Diagnostics* diagnostics, const std::string& message) {
    if (diagnostics == nullptr || diagnostics->messages.size() >= kMaxDiagnostics ||
        std::find(diagnostics->messages.begin(), diagnostics->messages.end(), message) !=
            diagnostics->messages.end()) return;
    diagnostics->messages.push_back(message);
}

}  // namespace ainiux::pptx::detail
