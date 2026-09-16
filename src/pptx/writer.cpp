#include "pptx/pptx.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <new>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "ainiux/version.hpp"
#include "markdown/blocks.hpp"
#include "ooxml/zip.hpp"
#include "platform/filesystem.hpp"
#include "pptx/detail.hpp"
#include "security/hash.hpp"

namespace ainiux::pptx {
namespace {

constexpr std::int64_t kEmuPerInch = 914400;
constexpr std::int64_t kDefaultWidth = 12192000;
constexpr std::int64_t kDefaultHeight = 6858000;
constexpr std::size_t kMaxSlides = 2048;
constexpr std::size_t kMaxShapesAndCells = 1000000;

struct SourceSlide {
    std::string markdown;
    std::string notes;
};

struct ImageResource {
    std::string digest;
    std::string extension;
    std::string content_type;
    std::string bytes;
    std::string part_name;
    std::uint32_t pixel_width = 0;
    std::uint32_t pixel_height = 0;
};

struct Relationship {
    std::string id;
    std::string type;
    std::string target;
    bool external = false;
};

struct SlideBuild {
    std::string xml;
    std::vector<Relationship> relationships;
    std::string notes;
};

std::string trim(std::string text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t' ||
                             text.front() == '\r' || text.front() == '\n')) text.erase(text.begin());
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' ||
                             text.back() == '\r' || text.back() == '\n')) text.pop_back();
    return text;
}

bool starts_with_ci(std::string_view text, std::string_view prefix) {
    if (text.size() < prefix.size()) return false;
    return ascii_lower(std::string(text.substr(0, prefix.size()))) == ascii_lower(std::string(prefix));
}

void parse_dimensions(std::string& markdown, std::int64_t& width, std::int64_t& height) {
    width = kDefaultWidth;
    height = kDefaultHeight;
    const std::string marker = "<!-- ainiux-pptx ";
    if (markdown.rfind(marker, 0) != 0) return;
    const std::size_t end = markdown.find("-->");
    if (end == std::string::npos || end > 256) return;
    const std::string metadata = markdown.substr(marker.size(), end - marker.size());
    auto value = [&](const char* key, std::int64_t& output) {
        const std::string needle = std::string(key) + "=";
        const std::size_t position = metadata.find(needle);
        if (position == std::string::npos) return;
        std::size_t begin = position + needle.size();
        std::size_t finish = begin;
        while (finish < metadata.size() && std::isdigit(static_cast<unsigned char>(metadata[finish]))) ++finish;
        if (finish == begin) return;
        try {
            const unsigned long long parsed = std::stoull(metadata.substr(begin, finish - begin));
            if (parsed >= 914400ULL && parsed <= 100000000ULL)
                output = static_cast<std::int64_t>(parsed);
        } catch (...) {
        }
    };
    value("width", width);
    value("height", height);
    markdown.erase(0, end + 3);
    while (!markdown.empty() && (markdown.front() == '\r' || markdown.front() == '\n'))
        markdown.erase(markdown.begin());
}

bool fence_open(std::string_view line, char& marker, std::size_t& length) {
    std::size_t pos = 0;
    while (pos < line.size() && pos < 4 && line[pos] == ' ') ++pos;
    if (pos >= line.size() || (line[pos] != '`' && line[pos] != '~')) return false;
    marker = line[pos];
    length = 0;
    while (pos + length < line.size() && line[pos + length] == marker) ++length;
    return length >= 3;
}

std::vector<SourceSlide> split_slides(std::string_view input) {
    std::vector<SourceSlide> slides;
    std::string current;
    bool fenced = false;
    char fence_marker = 0;
    std::size_t fence_length = 0;
    std::size_t start = 0;
    while (start <= input.size()) {
        const std::size_t newline = input.find('\n', start);
        const std::size_t end = newline == std::string_view::npos ? input.size() : newline;
        std::string_view line = input.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        bool delimiter = !fenced && line == "---";
        if (delimiter) {
            slides.push_back({current, {}});
            current.clear();
        } else {
            char marker = 0;
            std::size_t length = 0;
            if (fence_open(line, marker, length)) {
                if (!fenced) {
                    fenced = true;
                    fence_marker = marker;
                    fence_length = length;
                } else if (marker == fence_marker && length >= fence_length) {
                    fenced = false;
                }
            }
            current.append(line.data(), line.size());
            if (newline != std::string_view::npos) current.push_back('\n');
        }
        if (newline == std::string_view::npos) break;
        start = newline + 1;
    }
    slides.push_back({current, {}});
    return slides;
}

void extract_notes(SourceSlide& slide) {
    std::vector<std::pair<std::size_t, std::string>> lines;
    std::size_t start = 0;
    while (start <= slide.markdown.size()) {
        const std::size_t newline = slide.markdown.find('\n', start);
        const std::size_t end = newline == std::string::npos ? slide.markdown.size() : newline;
        std::string line = slide.markdown.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.emplace_back(start, std::move(line));
        if (newline == std::string::npos) break;
        start = newline + 1;
    }
    for (std::size_t index = lines.size(); index-- > 0;) {
        if (trim(lines[index].second) != "> **Speaker notes:**") continue;
        bool valid = true;
        for (std::size_t rest = index + 1; rest < lines.size(); ++rest) {
            const std::string trimmed = trim(lines[rest].second);
            if (!trimmed.empty() && lines[rest].second.rfind('>', 0) != 0) {
                valid = false;
                break;
            }
        }
        if (!valid) continue;
        for (std::size_t rest = index + 1; rest < lines.size(); ++rest) {
            std::string line = lines[rest].second;
            if (!line.empty() && line[0] == '>') {
                line.erase(line.begin());
                if (!line.empty() && line.front() == ' ') line.erase(line.begin());
            }
            if (slide.notes.empty() && line.empty()) continue;
            slide.notes += line;
            slide.notes.push_back('\n');
        }
        while (!slide.notes.empty() && (slide.notes.back() == '\n' || slide.notes.back() == '\r'))
            slide.notes.pop_back();
        slide.markdown.erase(lines[index].first);
        while (!slide.markdown.empty() &&
               (slide.markdown.back() == '\n' || slide.markdown.back() == '\r'))
            slide.markdown.pop_back();
        return;
    }
}

std::string runs_text(const std::vector<markdown::Run>& runs, bool include_images = true) {
    std::string text;
    for (const markdown::Run& run : runs) {
        if (!include_images && run.image_placeholder && !run.image_destination.empty()) continue;
        text += run.text;
        if (run.hard_break_after) text.push_back('\n');
    }
    return text;
}

void flatten_blocks(const std::vector<markdown::Block>& input,
                    std::vector<markdown::Block>& output) {
    for (const markdown::Block& block : input) {
        if (block.kind == markdown::BlockKind::Quote) flatten_blocks(block.children, output);
        else output.push_back(block);
    }
}

std::string protect_slide_delimiters(std::string text) {
    std::string out;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t newline = text.find('\n', start);
        const std::size_t end = newline == std::string::npos ? text.size() : newline;
        const std::string_view line(text.data() + start, end - start);
        if (line == "---") out += "    ";
        out.append(line.data(), line.size());
        if (newline == std::string::npos) break;
        out.push_back('\n');
        start = newline + 1;
    }
    return out;
}

bool has_rtl(std::string_view text) {
    for (std::size_t i = 0; i < text.size();) {
        const unsigned char first = static_cast<unsigned char>(text[i]);
        std::uint32_t cp = first;
        std::size_t length = 1;
        if ((first & 0xe0U) == 0xc0U && i + 1 < text.size()) {
            cp = ((first & 0x1fU) << 6U) | (static_cast<unsigned char>(text[i + 1]) & 0x3fU);
            length = 2;
        } else if ((first & 0xf0U) == 0xe0U && i + 2 < text.size()) {
            cp = ((first & 0x0fU) << 12U) |
                 ((static_cast<unsigned char>(text[i + 1]) & 0x3fU) << 6U) |
                 (static_cast<unsigned char>(text[i + 2]) & 0x3fU);
            length = 3;
        } else if ((first & 0xf8U) == 0xf0U && i + 3 < text.size()) {
            cp = ((first & 0x07U) << 18U) |
                 ((static_cast<unsigned char>(text[i + 1]) & 0x3fU) << 12U) |
                 ((static_cast<unsigned char>(text[i + 2]) & 0x3fU) << 6U) |
                 (static_cast<unsigned char>(text[i + 3]) & 0x3fU);
            length = 4;
        }
        if ((cp >= 0x0590U && cp <= 0x08ffU) || (cp >= 0xfb1dU && cp <= 0xfdffU) ||
            (cp >= 0xfe70U && cp <= 0xfeffU)) return true;
        i += length;
    }
    return false;
}

std::uint32_t be32(std::string_view bytes, std::size_t offset) {
    if (offset + 4 > bytes.size()) return 0;
    return (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset])) << 24U) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset + 1])) << 16U) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset + 2])) << 8U) |
           static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset + 3]));
}

std::uint16_t le16(std::string_view bytes, std::size_t offset) {
    if (offset + 2 > bytes.size()) return 0;
    return static_cast<std::uint16_t>(static_cast<unsigned char>(bytes[offset])) |
           static_cast<std::uint16_t>(static_cast<unsigned char>(bytes[offset + 1])) << 8U;
}

void image_dimensions(const std::string& extension, std::string_view bytes,
                      std::uint32_t& width, std::uint32_t& height) {
    width = height = 0;
    if (extension == "png" && bytes.size() >= 24) {
        width = be32(bytes, 16);
        height = be32(bytes, 20);
    } else if (extension == "gif" && bytes.size() >= 10) {
        width = le16(bytes, 6);
        height = le16(bytes, 8);
    } else if (extension == "bmp" && bytes.size() >= 26) {
        const auto le32 = [&](std::size_t offset) {
            return static_cast<std::uint32_t>(le16(bytes, offset)) |
                   static_cast<std::uint32_t>(le16(bytes, offset + 2)) << 16U;
        };
        width = le32(18);
        height = le32(22);
    } else if (extension == "jpg" && bytes.size() >= 4) {
        std::size_t pos = 2;
        while (pos + 4 <= bytes.size()) {
            if (static_cast<unsigned char>(bytes[pos]) != 0xffU) { ++pos; continue; }
            const unsigned char marker = static_cast<unsigned char>(bytes[pos + 1]);
            pos += 2;
            if (marker == 0xd8U || marker == 0xd9U) continue;
            if (pos + 2 > bytes.size()) break;
            const std::size_t length = (static_cast<unsigned char>(bytes[pos]) << 8U) |
                                       static_cast<unsigned char>(bytes[pos + 1]);
            if (length < 2 || pos + length > bytes.size()) break;
            if ((marker >= 0xc0U && marker <= 0xc3U) ||
                (marker >= 0xc5U && marker <= 0xc7U) ||
                (marker >= 0xc9U && marker <= 0xcbU) ||
                (marker >= 0xcdU && marker <= 0xcfU)) {
                if (length >= 7) {
                    height = (static_cast<unsigned char>(bytes[pos + 3]) << 8U) |
                             static_cast<unsigned char>(bytes[pos + 4]);
                    width = (static_cast<unsigned char>(bytes[pos + 5]) << 8U) |
                            static_cast<unsigned char>(bytes[pos + 6]);
                }
                break;
            }
            pos += length;
        }
    }
}

Error local_image(const std::string& destination,
                  const WriteOptions& options,
                  ImageResource& image,
                  Diagnostics* diagnostics) {
    for (const MediaAsset& asset : options.collected_media) {
        if (asset.markdown_target != destination) continue;
        image.digest = asset.sha256.empty() ? security::sha256_hex(asset.bytes) : asset.sha256;
        image.extension = asset.extension.empty()
                              ? detail::normalize_extension(asset.source_part, asset.content_type, asset.bytes)
                              : ascii_lower(asset.extension);
        image.content_type = asset.content_type.empty()
                                 ? detail::content_type_for_extension(image.extension)
                                 : asset.content_type;
        image.bytes = asset.bytes;
        if (image.extension == "svg") {
            detail::diagnostic(diagnostics,
                "SVG images require a raster fallback for PPTX output and were replaced with alt text");
            return {ErrorCode::UnsupportedFeature, "SVG image has no raster fallback"};
        }
        if (image.extension.empty())
            return {ErrorCode::UnsupportedFeature, "image format is unsupported for PPTX output"};
        image_dimensions(image.extension, image.bytes, image.pixel_width, image.pixel_height);
        return ok_error();
    }

    if (starts_with_ci(destination, "http://") || starts_with_ci(destination, "https://") ||
        starts_with_ci(destination, "data:") || starts_with_ci(destination, "ainiux-media:")) {
        detail::diagnostic(diagnostics,
            "non-local Markdown images were not fetched and were replaced with alt text");
        return {ErrorCode::UnsupportedFeature, "PPTX output only embeds local Markdown images"};
    }
    std::string path_text = destination;
    if (path_text.size() >= 2 && path_text.front() == '<' && path_text.back() == '>')
        path_text = path_text.substr(1, path_text.size() - 2);
    std::filesystem::path path = std::filesystem::u8path(expand_user_path(path_text));
    if (path.is_relative()) {
        const std::filesystem::path base = options.source_directory.empty()
            ? std::filesystem::current_path() : std::filesystem::u8path(options.source_directory);
        path = base / path;
    }
    std::error_code ec;
    const std::filesystem::path canonical = std::filesystem::canonical(path, ec);
    if (ec) return {ErrorCode::FileRead, "could not resolve Markdown image: " + path.u8string()};
    if (!options.workspace_root.empty()) {
        bool within = false;
        Error contained = platform::path_is_within(options.workspace_root, canonical.u8string(), within);
        if (!contained.ok() || !within)
            return {ErrorCode::BadArgs, "Markdown image escapes the configured workspace: " + path_text};
    }
    Error error = platform::read_file_bounded(canonical.u8string(), 64U * 1024U * 1024U, image.bytes);
    if (!error.ok()) return error;
    image.extension = detail::normalize_extension(canonical.u8string(), {}, image.bytes);
    if (image.extension == "svg") {
        detail::diagnostic(diagnostics,
            "SVG images require a raster fallback for PPTX output and were replaced with alt text");
        return {ErrorCode::UnsupportedFeature, "SVG image has no raster fallback"};
    }
    image.content_type = detail::content_type_for_extension(image.extension);
    if (image.extension.empty() || image.content_type.empty())
        return {ErrorCode::UnsupportedFeature, "image format is unsupported for PPTX output: " + path_text};
    image.digest = security::sha256_hex(image.bytes);
    image_dimensions(image.extension, image.bytes, image.pixel_width, image.pixel_height);
    return ok_error();
}

std::string relationship_xml(const std::vector<Relationship>& relationships) {
    std::string xml = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
                      "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">";
    for (const Relationship& relationship : relationships) {
        xml += "<Relationship Id=\"" + detail::xml_escape(relationship.id, true) +
               "\" Type=\"" + detail::xml_escape(relationship.type, true) +
               "\" Target=\"" + detail::xml_escape(relationship.target, true) + '"';
        if (relationship.external) xml += " TargetMode=\"External\"";
        xml += "/>";
    }
    xml += "</Relationships>";
    return xml;
}

std::string paragraph_xml(const std::vector<markdown::Run>& source_runs,
                          bool ordered,
                          bool bullet,
                          int level,
                          int start,
                          int font_size,
                          std::vector<Relationship>& relationships,
                          bool omit_embedded_images = true) {
    std::vector<markdown::Run> runs;
    runs.reserve(source_runs.size());
    for (const markdown::Run& run : source_runs) {
        if (omit_embedded_images && run.image_placeholder && !run.image_destination.empty()) continue;
        runs.push_back(run);
    }
    const bool rtl = has_rtl(runs_text(runs));
    std::string xml = "<a:p><a:pPr";
    if (level > 0) xml += " lvl=\"" + std::to_string(std::min(level, 8)) + "\"";
    if (rtl) xml += " rtl=\"1\" algn=\"r\"";
    xml += '>';
    if (ordered) {
        xml += "<a:buAutoNum type=\"arabicPeriod\" startAt=\"" +
               std::to_string(std::max(1, start)) + "\"/>";
    } else if (bullet) {
        xml += "<a:buChar char=\"&#x2022;\"/>";
    } else {
        xml += "<a:buNone/>";
    }
    xml += "</a:pPr>";
    for (const markdown::Run& run : runs) {
        if (run.text.empty() && !run.hard_break_after) continue;
        xml += "<a:r><a:rPr lang=\"en-US\" sz=\"" + std::to_string(font_size) + '"';
        if ((run.style & static_cast<unsigned>(markdown::RunStyle::Bold)) != 0) xml += " b=\"1\"";
        if ((run.style & static_cast<unsigned>(markdown::RunStyle::Italic)) != 0) xml += " i=\"1\"";
        if ((run.style & static_cast<unsigned>(markdown::RunStyle::Underline)) != 0) xml += " u=\"sng\"";
        if ((run.style & static_cast<unsigned>(markdown::RunStyle::Strike)) != 0)
            xml += " strike=\"sngStrike\"";
        xml += '>';
        if ((run.style & static_cast<unsigned>(markdown::RunStyle::Code)) != 0)
            xml += "<a:latin typeface=\"Consolas\"/>";
        if (!run.url.empty()) {
            const std::string rid = "rId" + std::to_string(relationships.size() + 1);
            relationships.push_back({rid,
                "http://schemas.openxmlformats.org/officeDocument/2006/relationships/hyperlink",
                run.url, true});
            xml += "<a:hlinkClick r:id=\"" + rid + "\"/>";
        }
        xml += "</a:rPr><a:t";
        if (!run.text.empty() && (run.text.front() == ' ' || run.text.back() == ' ' ||
                                  run.text.find('\n') != std::string::npos))
            xml += " xml:space=\"preserve\"";
        xml += '>' + detail::xml_escape(run.text) + "</a:t></a:r>";
        if (run.hard_break_after) xml += "<a:br/>";
    }
    xml += "<a:endParaRPr lang=\"en-US\" sz=\"" + std::to_string(font_size) + "\"/></a:p>";
    return xml;
}

std::string shape_xml(std::uint32_t id,
                      std::string name,
                      std::int64_t x,
                      std::int64_t y,
                      std::int64_t cx,
                      std::int64_t cy,
                      const std::vector<std::string>& paragraphs,
                      bool title) {
    std::string xml = "<p:sp><p:nvSpPr><p:cNvPr id=\"" + std::to_string(id) +
        "\" name=\"" + detail::xml_escape(name, true) +
        "\"/><p:cNvSpPr txBox=\"1\"/><p:nvPr>";
    if (title) xml += "<p:ph type=\"title\"/>";
    xml += "</p:nvPr></p:nvSpPr><p:spPr><a:xfrm><a:off x=\"" + std::to_string(x) +
        "\" y=\"" + std::to_string(y) + "\"/><a:ext cx=\"" + std::to_string(cx) +
        "\" cy=\"" + std::to_string(cy) +
        "\"/></a:xfrm><a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom>"
        "<a:noFill/><a:ln><a:noFill/></a:ln></p:spPr><p:txBody>"
        "<a:bodyPr wrap=\"square\" lIns=\"91440\" rIns=\"91440\" tIns=\"45720\" bIns=\"45720\">"
        "<a:normAutofit/></a:bodyPr><a:lstStyle/>";
    if (paragraphs.empty()) xml += "<a:p><a:endParaRPr lang=\"en-US\"/></a:p>";
    else for (const std::string& paragraph : paragraphs) xml += paragraph;
    xml += "</p:txBody></p:sp>";
    return xml;
}

std::string picture_xml(std::uint32_t id,
                        const std::string& name,
                        const std::string& alt,
                        const std::string& rid,
                        std::int64_t x,
                        std::int64_t y,
                        std::int64_t cx,
                        std::int64_t cy) {
    return "<p:pic><p:nvPicPr><p:cNvPr id=\"" + std::to_string(id) + "\" name=\"" +
        detail::xml_escape(name, true) + "\" descr=\"" + detail::xml_escape(alt, true) +
        "\"/><p:cNvPicPr><a:picLocks noChangeAspect=\"1\"/></p:cNvPicPr><p:nvPr/></p:nvPicPr>"
        "<p:blipFill><a:blip r:embed=\"" + rid +
        "\"/><a:stretch><a:fillRect/></a:stretch></p:blipFill><p:spPr><a:xfrm><a:off x=\"" +
        std::to_string(x) + "\" y=\"" + std::to_string(y) + "\"/><a:ext cx=\"" +
        std::to_string(cx) + "\" cy=\"" + std::to_string(cy) +
        "\"/></a:xfrm><a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom></p:spPr></p:pic>";
}

std::string table_xml(std::uint32_t id,
                      std::int64_t x,
                      std::int64_t y,
                      std::int64_t cx,
                      std::int64_t cy,
                      const markdown::Block& block,
                      std::vector<Relationship>& relationships) {
    std::size_t columns = 0;
    for (const auto& row : block.table_cells) columns = std::max(columns, row.size());
    if (columns == 0) return {};
    const std::int64_t column_width = cx / static_cast<std::int64_t>(columns);
    const std::int64_t row_height = std::max<std::int64_t>(250000,
        cy / static_cast<std::int64_t>(std::max<std::size_t>(1, block.table_cells.size())));
    std::string xml = "<p:graphicFrame><p:nvGraphicFramePr><p:cNvPr id=\"" +
        std::to_string(id) + "\" name=\"Table " + std::to_string(id) +
        "\"/><p:cNvGraphicFramePr/><p:nvPr/></p:nvGraphicFramePr><p:xfrm><a:off x=\"" +
        std::to_string(x) + "\" y=\"" + std::to_string(y) + "\"/><a:ext cx=\"" +
        std::to_string(cx) + "\" cy=\"" + std::to_string(cy) +
        "\"/></p:xfrm><a:graphic><a:graphicData uri=\"http://schemas.openxmlformats.org/drawingml/2006/table\">"
        "<a:tbl><a:tblPr firstRow=\"1\" bandRow=\"1\"/><a:tblGrid>";
    for (std::size_t column = 0; column < columns; ++column)
        xml += "<a:gridCol w=\"" + std::to_string(column_width) + "\"/>";
    xml += "</a:tblGrid>";
    for (std::size_t row = 0; row < block.table_cells.size(); ++row) {
        xml += "<a:tr h=\"" + std::to_string(row_height) + "\">";
        for (std::size_t column = 0; column < columns; ++column) {
            xml += "<a:tc><a:txBody><a:bodyPr/><a:lstStyle/>";
            if (column < block.table_cells[row].size()) {
                std::vector<markdown::Run> runs = block.table_cells[row][column];
                if (row == 0) for (markdown::Run& run : runs)
                    run.style |= static_cast<unsigned>(markdown::RunStyle::Bold);
                xml += paragraph_xml(runs, false, false, 0, 1, 1600, relationships);
            } else {
                xml += "<a:p><a:endParaRPr lang=\"en-US\" sz=\"1600\"/></a:p>";
            }
            xml += "</a:txBody><a:tcPr marL=\"45720\" marR=\"45720\" marT=\"22860\" marB=\"22860\">";
            if (row == 0) xml += "<a:solidFill><a:srgbClr val=\"D9EAF7\"/></a:solidFill>";
            xml += "</a:tcPr></a:tc>";
        }
        xml += "</a:tr>";
    }
    xml += "</a:tbl></a:graphicData></a:graphic></p:graphicFrame>";
    return xml;
}

class PackageBuilder {
   public:
    PackageBuilder(const WriteOptions& options, Diagnostics* diagnostics,
                   std::int64_t width, std::int64_t height)
        : options_(options), diagnostics_(diagnostics), width_(width), height_(height) {}

    Error build(const std::vector<SourceSlide>& source, std::string& bytes) {
        std::vector<SlideBuild> slides;
        slides.reserve(source.size());
        for (std::size_t index = 0; index < source.size(); ++index) {
            if (options_.cancellation.cancelled())
                return {ErrorCode::Cancelled, "PPTX serialization cancelled"};
            SlideBuild slide;
            slide.notes = source[index].notes;
            Error error = build_slide(source[index], index, slide);
            if (!error.ok()) return error;
            slides.push_back(std::move(slide));
        }
        return write_package(slides, bytes);
    }

   private:
    Error resolve_image(const markdown::Run& run, std::size_t& image_index) {
        ImageResource candidate;
        Error error = local_image(run.image_destination, options_, candidate, diagnostics_);
        if (!error.ok()) return error;
        const auto known = image_by_digest_.find(candidate.digest);
        if (known != image_by_digest_.end()) {
            image_index = known->second;
            return ok_error();
        }
        candidate.part_name = "ppt/media/image" + std::to_string(images_.size() + 1) + "." +
                              candidate.extension;
        image_index = images_.size();
        images_.push_back(std::move(candidate));
        image_by_digest_.emplace(images_.back().digest, image_index);
        return ok_error();
    }

    Error build_slide(const SourceSlide& source, std::size_t slide_index, SlideBuild& slide) {
        std::vector<markdown::Block> blocks;
        flatten_blocks(markdown::parse_blocks(source.markdown), blocks);
        slide.relationships.push_back({"rId1",
            "http://schemas.openxmlformats.org/officeDocument/2006/relationships/slideLayout",
            "../slideLayouts/slideLayout1.xml", false});
        std::string content;
        std::uint32_t shape_id = 2;
        std::int64_t y = 1180000;
        const std::int64_t margin = 550000;
        const std::int64_t content_width = std::max<std::int64_t>(1000000, width_ - margin * 2);
        const std::int64_t bottom = height_ - 350000;
        bool title_written = false;
        std::size_t counted = 0;

        for (const markdown::Block& block : blocks) {
            if (options_.cancellation.cancelled())
                return {ErrorCode::Cancelled, "PPTX serialization cancelled"};
            if (++counted > kMaxShapesAndCells)
                return {ErrorCode::UnsupportedFeature,
                        "Markdown exceeds the 1,000,000 shape/table-cell PPTX output limit"};
            if (block.kind == markdown::BlockKind::Rule) continue;
            if (block.kind == markdown::BlockKind::Heading && !title_written) {
                std::vector<std::string> paragraphs;
                paragraphs.push_back(paragraph_xml(block.runs, false, false, 0, 1, 2800,
                                                   slide.relationships));
                content += shape_xml(shape_id++, "Title", margin, 250000, content_width, 800000,
                                     paragraphs, true);
                title_written = true;
                continue;
            }
            if (block.kind == markdown::BlockKind::Table) {
                std::size_t columns = 0;
                for (const auto& row : block.table_cells) columns = std::max(columns, row.size());
                const std::size_t cells = columns * block.table_cells.size();
                if (cells > kMaxShapesAndCells - counted)
                    return {ErrorCode::UnsupportedFeature,
                            "Markdown exceeds the 1,000,000 shape/table-cell PPTX output limit"};
                counted += cells;
                const std::int64_t height = std::min<std::int64_t>(std::max<std::int64_t>(700000,
                    static_cast<std::int64_t>(block.table_cells.size()) * 330000),
                    std::max<std::int64_t>(300000, bottom - y));
                content += table_xml(shape_id++, margin, y, content_width, height, block,
                                     slide.relationships);
                y += height + 100000;
                continue;
            }

            bool wrote_image = false;
            for (const markdown::Run& run : block.runs) {
                if (!run.image_placeholder || run.image_destination.empty()) continue;
                std::size_t image_index = 0;
                Error image_error = resolve_image(run, image_index);
                if (!image_error.ok()) {
                    detail::diagnostic(diagnostics_,
                        "a Markdown image could not be embedded and was replaced with alt text");
                    continue;
                }
                const ImageResource& image = images_[image_index];
                const std::string rid = "rId" + std::to_string(slide.relationships.size() + 1);
                slide.relationships.push_back({rid,
                    "http://schemas.openxmlformats.org/officeDocument/2006/relationships/image",
                    "../media/" + image.part_name.substr(image.part_name.find_last_of('/') + 1), false});
                std::int64_t box_width = content_width;
                std::int64_t box_height = 2200000;
                if (image.pixel_width > 0 && image.pixel_height > 0) {
                    const long double aspect = static_cast<long double>(image.pixel_width) /
                                               static_cast<long double>(image.pixel_height);
                    box_height = static_cast<std::int64_t>(box_width / aspect);
                    if (box_height > 2600000) {
                        box_height = 2600000;
                        box_width = static_cast<std::int64_t>(box_height * aspect);
                    }
                }
                box_height = std::min(box_height, std::max<std::int64_t>(250000, bottom - y));
                box_width = std::min(box_width, content_width);
                const std::int64_t x = margin + (content_width - box_width) / 2;
                const std::uint32_t current_shape_id = shape_id++;
                content += picture_xml(current_shape_id,
                                       "Image " + std::to_string(current_shape_id),
                                       run.image_alt, rid, x, y, box_width, box_height);
                y += box_height + 100000;
                wrote_image = true;
            }

            std::vector<markdown::Run> runs = block.runs;
            if (block.kind == markdown::BlockKind::Code) {
                runs.clear();
                markdown::Run run;
                // A literal top-level "---" would become a slide delimiter if
                // the generated deck were read back. Keep it visibly identical
                // while retaining code indentation in canonical Markdown.
                run.text = protect_slide_delimiters(block.text);
                run.style = static_cast<unsigned>(markdown::RunStyle::Code);
                runs.push_back(std::move(run));
            } else if (block.kind == markdown::BlockKind::Html) {
                runs.clear();
                markdown::Run run;
                run.text = block.text;
                runs.push_back(std::move(run));
            }
            const bool has_text = !trim(runs_text(runs, false)).empty();
            if (!has_text && wrote_image) continue;
            if (!has_text && block.kind != markdown::BlockKind::Paragraph) continue;
            const int font_size = block.kind == markdown::BlockKind::Heading ? 2200 :
                                  block.kind == markdown::BlockKind::Code ? 1500 : 1800;
            const std::string visible_text = runs_text(runs);
            const std::int64_t height = std::min<std::int64_t>(
                std::max<std::int64_t>(420000, 360000 +
                    static_cast<std::int64_t>(std::count(visible_text.begin(),
                                                        visible_text.end(), '\n')) * 220000),
                std::max<std::int64_t>(250000, bottom - y));
            std::vector<std::string> paragraphs;
            paragraphs.push_back(paragraph_xml(runs,
                block.kind == markdown::BlockKind::ListItem && block.ordered,
                block.kind == markdown::BlockKind::ListItem && !block.ordered,
                block.indent / 2, block.list_index, font_size, slide.relationships));
            const std::uint32_t current_shape_id = shape_id++;
            content += shape_xml(current_shape_id,
                                 "Content " + std::to_string(current_shape_id),
                                 margin, y, content_width, height, paragraphs, false);
            y += height + 60000;
        }
        if (!slide.notes.empty()) {
            const std::string rid = "rId" + std::to_string(slide.relationships.size() + 1);
            slide.relationships.push_back({rid,
                "http://schemas.openxmlformats.org/officeDocument/2006/relationships/notesSlide",
                "../notesSlides/notesSlide" + std::to_string(slide_index + 1) + ".xml", false});
        }
        slide.xml = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
            "<p:sld xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\" "
            "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\" "
            "xmlns:p=\"http://schemas.openxmlformats.org/presentationml/2006/main\"><p:cSld><p:spTree>"
            "<p:nvGrpSpPr><p:cNvPr id=\"1\" name=\"\"/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr>"
            "<p:grpSpPr><a:xfrm><a:off x=\"0\" y=\"0\"/><a:ext cx=\"0\" cy=\"0\"/>"
            "<a:chOff x=\"0\" y=\"0\"/><a:chExt cx=\"0\" cy=\"0\"/></a:xfrm></p:grpSpPr>" +
            content + "</p:spTree></p:cSld><p:clrMapOvr><a:masterClrMapping/></p:clrMapOvr></p:sld>";
        return ok_error();
    }

    std::string notes_slide_xml(const std::string& notes) {
        const std::vector<markdown::Block> blocks = markdown::parse_blocks(notes);
        std::vector<std::string> paragraphs;
        std::vector<Relationship> unused;
        for (const markdown::Block& block : blocks) {
            if (block.kind == markdown::BlockKind::Rule || block.kind == markdown::BlockKind::Table) continue;
            std::vector<markdown::Run> runs = block.runs;
            if (runs.empty() && !block.text.empty()) {
                markdown::Run run;
                run.text = block.text;
                runs.push_back(std::move(run));
            }
            paragraphs.push_back(paragraph_xml(runs, false, false, 0, 1, 1200, unused));
        }
        return "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
            "<p:notes xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\" "
            "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\" "
            "xmlns:p=\"http://schemas.openxmlformats.org/presentationml/2006/main\"><p:cSld><p:spTree>"
            "<p:nvGrpSpPr><p:cNvPr id=\"1\" name=\"\"/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr>"
            "<p:grpSpPr><a:xfrm><a:off x=\"0\" y=\"0\"/><a:ext cx=\"0\" cy=\"0\"/>"
            "<a:chOff x=\"0\" y=\"0\"/><a:chExt cx=\"0\" cy=\"0\"/></a:xfrm></p:grpSpPr>" +
            shape_xml(2, "Notes Placeholder 1", 685800, 4400000, 7772400, 2000000,
                      paragraphs, false) +
            "</p:spTree></p:cSld><p:clrMapOvr><a:masterClrMapping/></p:clrMapOvr></p:notes>";
    }

    Error write_package(const std::vector<SlideBuild>& slides, std::string& bytes) {
        std::vector<ooxml::ZipWriteEntry> entries;
        const bool has_notes = std::any_of(slides.begin(), slides.end(),
            [](const SlideBuild& slide) { return !slide.notes.empty(); });
        entries.push_back({"[Content_Types].xml", content_types_xml(slides, has_notes), true});
        entries.push_back({"_rels/.rels", root_relationships_xml(), true});
        entries.push_back({"docProps/core.xml", core_properties_xml(), true});
        entries.push_back({"docProps/app.xml", app_properties_xml(slides.size()), true});
        entries.push_back({"ppt/presentation.xml", presentation_xml(slides.size()), true});
        entries.push_back({"ppt/_rels/presentation.xml.rels",
                           presentation_relationships_xml(slides.size()), true});
        entries.push_back({"ppt/slideMasters/slideMaster1.xml", slide_master_xml(), true});
        entries.push_back({"ppt/slideMasters/_rels/slideMaster1.xml.rels",
                           slide_master_relationships_xml(), true});
        entries.push_back({"ppt/slideLayouts/slideLayout1.xml", slide_layout_xml(), true});
        entries.push_back({"ppt/slideLayouts/_rels/slideLayout1.xml.rels",
                           slide_layout_relationships_xml(), true});
        entries.push_back({"ppt/theme/theme1.xml", theme_xml(), true});
        if (has_notes) {
            entries.push_back({"ppt/notesMasters/notesMaster1.xml", notes_master_xml(), true});
            entries.push_back({"ppt/notesMasters/_rels/notesMaster1.xml.rels",
                               notes_master_relationships_xml(), true});
        }
        for (std::size_t index = 0; index < slides.size(); ++index) {
            entries.push_back({"ppt/slides/slide" + std::to_string(index + 1) + ".xml",
                               slides[index].xml, true});
            entries.push_back({"ppt/slides/_rels/slide" + std::to_string(index + 1) + ".xml.rels",
                               relationship_xml(slides[index].relationships), true});
            if (!slides[index].notes.empty()) {
                entries.push_back({"ppt/notesSlides/notesSlide" + std::to_string(index + 1) + ".xml",
                                   notes_slide_xml(slides[index].notes), true});
                std::vector<Relationship> rels{
                    {"rId1", "http://schemas.openxmlformats.org/officeDocument/2006/relationships/notesMaster",
                     "../notesMasters/notesMaster1.xml", false},
                    {"rId2", "http://schemas.openxmlformats.org/officeDocument/2006/relationships/slide",
                     "../slides/slide" + std::to_string(index + 1) + ".xml", false}};
                entries.push_back({"ppt/notesSlides/_rels/notesSlide" + std::to_string(index + 1) +
                                   ".xml.rels", relationship_xml(rels), true});
            }
        }
        for (const ImageResource& image : images_)
            entries.push_back({image.part_name, image.bytes,
                               !detail::compressed_image_extension(image.extension)});
        return ooxml::write_zip(entries, options_.max_bytes, options_.cancellation, bytes);
    }

    std::string content_types_xml(const std::vector<SlideBuild>& slides, bool has_notes) const {
        std::string xml = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
            "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
            "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
            "<Default Extension=\"xml\" ContentType=\"application/xml\"/>";
        std::unordered_set<std::string> extensions;
        for (const ImageResource& image : images_) {
            if (extensions.insert(image.extension).second)
                xml += "<Default Extension=\"" + detail::xml_escape(image.extension, true) +
                       "\" ContentType=\"" + detail::xml_escape(image.content_type, true) + "\"/>";
        }
        xml += "<Override PartName=\"/ppt/presentation.xml\" ContentType=\"" +
               std::string(kMimeType) + ".main+xml\"/>"
            "<Override PartName=\"/ppt/slideMasters/slideMaster1.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.presentationml.slideMaster+xml\"/>"
            "<Override PartName=\"/ppt/slideLayouts/slideLayout1.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.presentationml.slideLayout+xml\"/>"
            "<Override PartName=\"/ppt/theme/theme1.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.theme+xml\"/>"
            "<Override PartName=\"/docProps/core.xml\" ContentType=\"application/vnd.openxmlformats-package.core-properties+xml\"/>"
            "<Override PartName=\"/docProps/app.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.extended-properties+xml\"/>";
        for (std::size_t index = 0; index < slides.size(); ++index) {
            xml += "<Override PartName=\"/ppt/slides/slide" + std::to_string(index + 1) +
                   ".xml\" ContentType=\"application/vnd.openxmlformats-officedocument.presentationml.slide+xml\"/>";
            if (!slides[index].notes.empty())
                xml += "<Override PartName=\"/ppt/notesSlides/notesSlide" +
                       std::to_string(index + 1) +
                       ".xml\" ContentType=\"application/vnd.openxmlformats-officedocument.presentationml.notesSlide+xml\"/>";
        }
        if (has_notes)
            xml += "<Override PartName=\"/ppt/notesMasters/notesMaster1.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.presentationml.notesMaster+xml\"/>";
        xml += "</Types>";
        return xml;
    }

    static std::string root_relationships_xml() {
        return relationship_xml({
            {"rId1", "http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument",
             "ppt/presentation.xml", false},
            {"rId2", "http://schemas.openxmlformats.org/package/2006/relationships/metadata/core-properties",
             "docProps/core.xml", false},
            {"rId3", "http://schemas.openxmlformats.org/officeDocument/2006/relationships/extended-properties",
             "docProps/app.xml", false}});
    }

    static std::string core_properties_xml() {
        return "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
            "<cp:coreProperties xmlns:cp=\"http://schemas.openxmlformats.org/package/2006/metadata/core-properties\" "
            "xmlns:dc=\"http://purl.org/dc/elements/1.1/\" xmlns:dcterms=\"http://purl.org/dc/terms/\" "
            "xmlns:dcmitype=\"http://purl.org/dc/dcmitype/\" xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\">"
            "<dc:creator>Ainiux</dc:creator><cp:lastModifiedBy>Ainiux</cp:lastModifiedBy>"
            "<dcterms:created xsi:type=\"dcterms:W3CDTF\">1980-01-01T00:00:00Z</dcterms:created>"
            "<dcterms:modified xsi:type=\"dcterms:W3CDTF\">1980-01-01T00:00:00Z</dcterms:modified>"
            "</cp:coreProperties>";
    }

    static std::string app_properties_xml(std::size_t slides) {
        return "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
            "<Properties xmlns=\"http://schemas.openxmlformats.org/officeDocument/2006/extended-properties\" "
            "xmlns:vt=\"http://schemas.openxmlformats.org/officeDocument/2006/docPropsVTypes\">"
            "<Application>Ainiux</Application><PresentationFormat>Widescreen</PresentationFormat>"
            "<Slides>" + std::to_string(slides) + "</Slides><Notes>0</Notes><HiddenSlides>0</HiddenSlides>"
            "<Company></Company><AppVersion>" + std::string(versionNumber) +
            "</AppVersion></Properties>";
    }

    std::string presentation_xml(std::size_t slides) const {
        std::string xml = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
            "<p:presentation xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\" "
            "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\" "
            "xmlns:p=\"http://schemas.openxmlformats.org/presentationml/2006/main\">"
            "<p:sldMasterIdLst><p:sldMasterId id=\"2147483648\" r:id=\"rId1\"/></p:sldMasterIdLst>"
            "<p:sldIdLst>";
        for (std::size_t index = 0; index < slides; ++index)
            xml += "<p:sldId id=\"" + std::to_string(256 + index) + "\" r:id=\"rId" +
                   std::to_string(index + 2) + "\"/>";
        xml += "</p:sldIdLst><p:sldSz cx=\"" + std::to_string(width_) + "\" cy=\"" +
               std::to_string(height_) + "\"/><p:notesSz cx=\"6858000\" cy=\"9144000\"/>"
               "<p:defaultTextStyle/></p:presentation>";
        return xml;
    }

    static std::string presentation_relationships_xml(std::size_t slides) {
        std::vector<Relationship> relationships;
        relationships.push_back({"rId1",
            "http://schemas.openxmlformats.org/officeDocument/2006/relationships/slideMaster",
            "slideMasters/slideMaster1.xml", false});
        for (std::size_t index = 0; index < slides; ++index)
            relationships.push_back({"rId" + std::to_string(index + 2),
                "http://schemas.openxmlformats.org/officeDocument/2006/relationships/slide",
                "slides/slide" + std::to_string(index + 1) + ".xml", false});
        return relationship_xml(relationships);
    }

    static std::string slide_master_xml() {
        return "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
            "<p:sldMaster xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\" "
            "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\" "
            "xmlns:p=\"http://schemas.openxmlformats.org/presentationml/2006/main\"><p:cSld name=\"Ainiux Master\"><p:spTree>"
            "<p:nvGrpSpPr><p:cNvPr id=\"1\" name=\"\"/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr>"
            "<p:grpSpPr><a:xfrm><a:off x=\"0\" y=\"0\"/><a:ext cx=\"0\" cy=\"0\"/>"
            "<a:chOff x=\"0\" y=\"0\"/><a:chExt cx=\"0\" cy=\"0\"/></a:xfrm></p:grpSpPr>"
            "</p:spTree></p:cSld><p:clrMap accent1=\"accent1\" accent2=\"accent2\" accent3=\"accent3\" "
            "accent4=\"accent4\" accent5=\"accent5\" accent6=\"accent6\" bg1=\"lt1\" bg2=\"lt2\" "
            "folHlink=\"folHlink\" hlink=\"hlink\" tx1=\"dk1\" tx2=\"dk2\"/>"
            "<p:sldLayoutIdLst><p:sldLayoutId id=\"1\" r:id=\"rId1\"/></p:sldLayoutIdLst>"
            "<p:txStyles><p:titleStyle><a:lvl1pPr algn=\"l\"><a:defRPr sz=\"2800\" b=\"1\"/></a:lvl1pPr>"
            "</p:titleStyle><p:bodyStyle><a:lvl1pPr marL=\"0\" indent=\"0\"><a:defRPr sz=\"1800\"/>"
            "</a:lvl1pPr></p:bodyStyle><p:otherStyle><a:defPPr><a:defRPr sz=\"1800\"/></a:defPPr>"
            "</p:otherStyle></p:txStyles></p:sldMaster>";
    }

    static std::string slide_master_relationships_xml() {
        return relationship_xml({
            {"rId1", "http://schemas.openxmlformats.org/officeDocument/2006/relationships/slideLayout",
             "../slideLayouts/slideLayout1.xml", false},
            {"rId2", "http://schemas.openxmlformats.org/officeDocument/2006/relationships/theme",
             "../theme/theme1.xml", false}});
    }

    static std::string slide_layout_xml() {
        return "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
            "<p:sldLayout xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\" "
            "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\" "
            "xmlns:p=\"http://schemas.openxmlformats.org/presentationml/2006/main\" type=\"blank\" preserve=\"1\">"
            "<p:cSld name=\"Blank\"><p:spTree><p:nvGrpSpPr><p:cNvPr id=\"1\" name=\"\"/>"
            "<p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr><p:grpSpPr><a:xfrm><a:off x=\"0\" y=\"0\"/>"
            "<a:ext cx=\"0\" cy=\"0\"/><a:chOff x=\"0\" y=\"0\"/><a:chExt cx=\"0\" cy=\"0\"/>"
            "</a:xfrm></p:grpSpPr></p:spTree></p:cSld><p:clrMapOvr><a:masterClrMapping/>"
            "</p:clrMapOvr></p:sldLayout>";
    }

    static std::string slide_layout_relationships_xml() {
        return relationship_xml({{"rId1",
            "http://schemas.openxmlformats.org/officeDocument/2006/relationships/slideMaster",
            "../slideMasters/slideMaster1.xml", false}});
    }

    static std::string theme_xml() {
        return "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
            "<a:theme xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\" name=\"Ainiux\">"
            "<a:themeElements><a:clrScheme name=\"Ainiux\"><a:dk1><a:sysClr val=\"windowText\" lastClr=\"000000\"/>"
            "</a:dk1><a:lt1><a:sysClr val=\"window\" lastClr=\"FFFFFF\"/></a:lt1>"
            "<a:dk2><a:srgbClr val=\"1F2937\"/></a:dk2><a:lt2><a:srgbClr val=\"F3F4F6\"/></a:lt2>"
            "<a:accent1><a:srgbClr val=\"2563EB\"/></a:accent1><a:accent2><a:srgbClr val=\"059669\"/></a:accent2>"
            "<a:accent3><a:srgbClr val=\"D97706\"/></a:accent3><a:accent4><a:srgbClr val=\"7C3AED\"/></a:accent4>"
            "<a:accent5><a:srgbClr val=\"DB2777\"/></a:accent5><a:accent6><a:srgbClr val=\"0891B2\"/></a:accent6>"
            "<a:hlink><a:srgbClr val=\"0000FF\"/></a:hlink><a:folHlink><a:srgbClr val=\"800080\"/></a:folHlink>"
            "</a:clrScheme><a:fontScheme name=\"Ainiux\"><a:majorFont><a:latin typeface=\"Aptos Display\"/>"
            "<a:ea typeface=\"\"/><a:cs typeface=\"\"/></a:majorFont><a:minorFont><a:latin typeface=\"Aptos\"/>"
            "<a:ea typeface=\"\"/><a:cs typeface=\"\"/></a:minorFont></a:fontScheme>"
            "<a:fmtScheme name=\"Ainiux\"><a:fillStyleLst><a:solidFill><a:schemeClr val=\"phClr\"/>"
            "</a:solidFill></a:fillStyleLst><a:lnStyleLst><a:ln w=\"9525\"><a:solidFill><a:schemeClr val=\"phClr\"/>"
            "</a:solidFill></a:ln></a:lnStyleLst><a:effectStyleLst><a:effectStyle><a:effectLst/>"
            "</a:effectStyle></a:effectStyleLst><a:bgFillStyleLst><a:solidFill><a:schemeClr val=\"phClr\"/>"
            "</a:solidFill></a:bgFillStyleLst></a:fmtScheme></a:themeElements></a:theme>";
    }

    static std::string notes_master_xml() {
        return "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
            "<p:notesMaster xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\" "
            "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\" "
            "xmlns:p=\"http://schemas.openxmlformats.org/presentationml/2006/main\"><p:cSld><p:spTree>"
            "<p:nvGrpSpPr><p:cNvPr id=\"1\" name=\"\"/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr>"
            "<p:grpSpPr><a:xfrm><a:off x=\"0\" y=\"0\"/><a:ext cx=\"0\" cy=\"0\"/>"
            "<a:chOff x=\"0\" y=\"0\"/><a:chExt cx=\"0\" cy=\"0\"/></a:xfrm></p:grpSpPr>"
            "</p:spTree></p:cSld><p:clrMap accent1=\"accent1\" accent2=\"accent2\" accent3=\"accent3\" "
            "accent4=\"accent4\" accent5=\"accent5\" accent6=\"accent6\" bg1=\"lt1\" bg2=\"lt2\" "
            "folHlink=\"folHlink\" hlink=\"hlink\" tx1=\"dk1\" tx2=\"dk2\"/>"
            "<p:notesStyle><a:lvl1pPr><a:defRPr sz=\"1200\"/></a:lvl1pPr></p:notesStyle></p:notesMaster>";
    }

    static std::string notes_master_relationships_xml() {
        return relationship_xml({{"rId1",
            "http://schemas.openxmlformats.org/officeDocument/2006/relationships/theme",
            "../theme/theme1.xml", false}});
    }

    const WriteOptions& options_;
    Diagnostics* diagnostics_;
    std::int64_t width_;
    std::int64_t height_;
    std::vector<ImageResource> images_;
    std::unordered_map<std::string, std::size_t> image_by_digest_;
};

}  // namespace

Error from_markdown(std::string_view markdown,
                    const WriteOptions& options,
                    std::string& bytes,
                    Diagnostics* diagnostics) {
    bytes.clear();
    if (diagnostics != nullptr) *diagnostics = {};
    if (options.cancellation.cancelled()) return {ErrorCode::Cancelled, "PPTX serialization cancelled"};
    try {
        std::string normalized(markdown);
        std::int64_t width = kDefaultWidth;
        std::int64_t height = kDefaultHeight;
        parse_dimensions(normalized, width, height);
        std::vector<SourceSlide> slides = split_slides(normalized);
        if (slides.empty()) slides.push_back({});
        if (slides.size() > kMaxSlides)
            return {ErrorCode::UnsupportedFeature, "Markdown exceeds the 2,048 slide PPTX output limit"};
        for (SourceSlide& slide : slides) extract_notes(slide);
        PackageBuilder builder(options, diagnostics, width, height);
        Error error = builder.build(slides, bytes);
        if (!error.ok()) bytes.clear();
        return error;
    } catch (const std::bad_alloc&) {
        bytes.clear();
        return {ErrorCode::Internal, "not enough memory to serialize PPTX"};
    } catch (const std::length_error&) {
        bytes.clear();
        return {ErrorCode::UnsupportedFeature, "PPTX output is too large to serialize"};
    }
}

}  // namespace ainiux::pptx
