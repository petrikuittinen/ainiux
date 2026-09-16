#include "pptx/pptx.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <limits>
#include <new>
#include <unordered_map>
#include <utility>

#include "markdown/blocks.hpp"
#include "ooxml/zip.hpp"
#include "pptx/detail.hpp"
#include "security/hash.hpp"

namespace ainiux::pptx {
namespace {

using detail::RelationshipMap;
using ooxml::XmlEvent;
using ooxml::ZipArchive;

struct ContentTypes {
    std::unordered_map<std::string, std::string> defaults;
    std::unordered_map<std::string, std::string> overrides;

    std::string for_part(const std::string& part) const {
        const auto exact = overrides.find("/" + part);
        if (exact != overrides.end()) return exact->second;
        const std::size_t dot = part.find_last_of('.');
        if (dot != std::string::npos) {
            const auto found = defaults.find(ascii_lower(part.substr(dot + 1)));
            if (found != defaults.end()) return found->second;
        }
        return {};
    }
};

struct Paragraph {
    std::vector<markdown::Run> runs;
    bool ordered = false;
    bool bullet = false;
    int level = 0;
    int start = 1;
};

struct Item {
    enum class Kind { Paragraph, Table, Image, Placeholder } kind = Kind::Paragraph;
    Paragraph paragraph;
    bool title = false;
    std::vector<std::vector<std::vector<Paragraph>>> table;
    std::string alt;
    std::string image_target;
    std::string placeholder;
};

struct Slide {
    std::vector<Item> items;
    std::vector<Paragraph> notes;
};

struct ShapeState {
    bool title = false;
    bool skip = false;
    std::vector<Paragraph> paragraphs;
};

struct PictureState {
    std::string alt;
    std::string rid;
};

bool true_value(const std::string* value) {
    if (value == nullptr) return true;
    const std::string lower = ascii_lower(*value);
    return lower != "0" && lower != "false" && lower != "off" && lower != "none";
}

int bounded_int(const std::string* value, int fallback, int maximum) {
    if (value == nullptr || value->empty()) return fallback;
    try {
        const long long parsed = std::stoll(*value);
        if (parsed < 0) return 0;
        if (parsed > maximum) return maximum;
        return static_cast<int>(parsed);
    } catch (...) {
        return fallback;
    }
}

Error parse_content_types(std::string_view xml,
                          runtime::CancellationToken cancellation,
                          ContentTypes& types) {
    types = {};
    return ooxml::scan_xml(xml, {}, cancellation, [&](const XmlEvent& event) -> Error {
        if (event.kind != XmlEvent::Kind::Start) return ok_error();
        if (event.name.local == "Default") {
            const std::string* extension = detail::attr_local(event, "Extension");
            const std::string* content = detail::attr_local(event, "ContentType");
            if (extension != nullptr && content != nullptr)
                types.defaults[ascii_lower(*extension)] = *content;
        } else if (event.name.local == "Override") {
            const std::string* part = detail::attr_local(event, "PartName");
            const std::string* content = detail::attr_local(event, "ContentType");
            if (part != nullptr && content != nullptr) types.overrides[*part] = *content;
        }
        return ok_error();
    });
}

std::string resolve_internal(const std::string& source,
                             const detail::Relationship& relationship,
                             Error& error) {
    if (relationship.external) return {};
    std::string part;
    error = ooxml::resolve_part_name(source, relationship.target, part);
    return part;
}

std::string format_run(const markdown::Run& run, bool table_cell) {
    std::string text = detail::markdown_escape(run.text, table_cell);
    if ((run.style & static_cast<unsigned>(markdown::RunStyle::Code)) != 0) {
        text = "`" + text + "`";
    } else {
        if ((run.style & static_cast<unsigned>(markdown::RunStyle::Underline)) != 0)
            text = "++" + text + "++";
        if ((run.style & static_cast<unsigned>(markdown::RunStyle::Strike)) != 0)
            text = "~~" + text + "~~";
        const bool bold = (run.style & static_cast<unsigned>(markdown::RunStyle::Bold)) != 0;
        const bool italic = (run.style & static_cast<unsigned>(markdown::RunStyle::Italic)) != 0;
        if (bold && italic) text = "***" + text + "***";
        else if (bold) text = "**" + text + "**";
        else if (italic) text = "*" + text + "*";
    }
    if (!run.url.empty()) text = "[" + text + "](" + run.url + ")";
    if (run.hard_break_after) text += table_cell ? "<br>" : "  \n";
    return text;
}

std::string format_paragraph(const Paragraph& paragraph, bool table_cell = false) {
    std::string text;
    for (const markdown::Run& run : paragraph.runs) text += format_run(run, table_cell);
    return text;
}

void append_placeholder(Slide& slide, std::string message, Diagnostics* diagnostics) {
    Item item;
    item.kind = Item::Kind::Placeholder;
    item.placeholder = "[" + std::move(message) + "]";
    slide.items.push_back(std::move(item));
    if (diagnostics != nullptr) ++diagnostics->unsupported_objects;
}

class SlideParser {
   public:
    SlideParser(const RelationshipMap& relationships,
                ZipArchive& archive,
                const ContentTypes& content_types,
                const ReadOptions& options,
                Diagnostics* diagnostics,
                std::vector<MediaAsset>* media,
                std::unordered_map<std::string, std::size_t>& media_by_digest,
                std::size_t& object_count,
                std::string slide_part,
                bool notes_only)
        : relationships_(relationships), archive_(archive), content_types_(content_types),
          options_(options), diagnostics_(diagnostics), media_(media),
          media_by_digest_(media_by_digest), object_count_(object_count),
          slide_part_(std::move(slide_part)), notes_only_(notes_only) {}

    Error parse(std::string_view xml, Slide& slide) {
        slide_ = &slide;
        ooxml::XmlLimits limits;
        limits.max_depth = 256;
        limits.max_text_bytes = options_.max_xml_part_bytes;
        Error error = ooxml::scan_xml(xml, limits, options_.cancellation,
                                      [&](const XmlEvent& event) { return on_event(event); });
        if (!error.ok()) return error;
        return ok_error();
    }

   private:
    Error count_object() {
        if (object_count_ >= options_.max_shapes_and_cells)
            return {ErrorCode::UnsupportedFeature,
                    "PPTX exceeds the 1,000,000 shape/table-cell conversion limit"};
        ++object_count_;
        return ok_error();
    }

    void begin_paragraph() {
        paragraph_active_ = true;
        paragraph_ = {};
        run_ = {};
        run_active_ = false;
        text_depth_ = 0;
        hyperlink_rid_.clear();
    }

    void finish_run() {
        if (!run_active_) return;
        if (!hyperlink_rid_.empty()) {
            const auto found = relationships_.find(hyperlink_rid_);
            if (found != relationships_.end() && found->second.external &&
                detail::type_suffix(found->second.type, "/hyperlink")) {
                run_.url = found->second.target;
            } else {
                if (diagnostics_ != nullptr) ++diagnostics_->missing_relationships;
                detail::diagnostic(diagnostics_,
                    "a slide hyperlink relationship was missing or invalid and its target was omitted");
            }
        }
        if (!run_.text.empty() || run_.hard_break_after) paragraph_.runs.push_back(std::move(run_));
        run_ = {};
        run_active_ = false;
        hyperlink_rid_.clear();
    }

    Error finish_paragraph() {
        finish_run();
        paragraph_active_ = false;
        if (table_cell_active_) {
            table_cell_.push_back(std::move(paragraph_));
        } else if (shape_active_) {
            shape_.paragraphs.push_back(std::move(paragraph_));
        }
        paragraph_ = {};
        return ok_error();
    }

    Error finish_shape() {
        if (!shape_active_) return ok_error();
        if (!shape_.skip) {
            for (Paragraph& paragraph : shape_.paragraphs) {
                if (notes_only_) {
                    slide_->notes.push_back(std::move(paragraph));
                } else if (!paragraph.runs.empty() || paragraph.bullet || paragraph.ordered) {
                    Item item;
                    item.kind = Item::Kind::Paragraph;
                    item.paragraph = std::move(paragraph);
                    item.title = shape_.title;
                    slide_->items.push_back(std::move(item));
                }
            }
        }
        shape_ = {};
        shape_active_ = false;
        return ok_error();
    }

    Error finish_picture() {
        if (!picture_active_) return ok_error();
        if (notes_only_) {
            picture_ = {};
            picture_active_ = false;
            return ok_error();
        }
        Item item;
        item.kind = Item::Kind::Image;
        item.alt = picture_.alt;
        if (item.alt.empty()) item.alt = "image";
        const auto found = relationships_.find(picture_.rid);
        if (picture_.rid.empty() || found == relationships_.end() || found->second.external) {
            item.image_target.clear();
            if (diagnostics_ != nullptr) {
                ++diagnostics_->omitted_images;
                ++diagnostics_->missing_relationships;
            }
            detail::diagnostic(diagnostics_,
                "an image relationship was missing or invalid and was replaced with a visible placeholder");
        } else {
            Error error;
            const std::string part = resolve_internal(slide_part_, found->second, error);
            if (!error.ok()) return error;
            if (!archive_.contains(part)) {
                if (diagnostics_ != nullptr) {
                    ++diagnostics_->omitted_images;
                    ++diagnostics_->missing_relationships;
                }
                detail::diagnostic(diagnostics_,
                    "an image part was missing and was replaced with a visible placeholder");
            } else if (!options_.collect_media || media_ == nullptr) {
                if (diagnostics_ != nullptr) ++diagnostics_->omitted_images;
                detail::diagnostic(diagnostics_,
                    "embedded images were replaced with visible Markdown placeholders");
            } else {
                std::string image_bytes;
                error = archive_.read(part, image_bytes);
                if (!error.ok()) return error;
                const std::string content_type = content_types_.for_part(part);
                const std::string extension =
                    detail::normalize_extension(part, content_type, image_bytes);
                if (extension.empty()) {
                    if (diagnostics_ != nullptr) ++diagnostics_->omitted_images;
                    detail::diagnostic(diagnostics_,
                        "an image with an unsupported format was replaced with a visible placeholder");
                } else {
                    const std::string digest = security::sha256_hex(image_bytes);
                    auto known = media_by_digest_.find(digest);
                    if (known == media_by_digest_.end()) {
                        MediaAsset asset;
                        asset.source_part = part;
                        asset.extension = extension;
                        asset.content_type = content_type.empty()
                                                 ? detail::content_type_for_extension(extension)
                                                 : content_type;
                        asset.sha256 = digest;
                        asset.markdown_target = options_.media_link_prefix.empty()
                            ? "ainiux-media:" + digest + "." + extension
                            : options_.media_link_prefix + "/image-" + digest + "." + extension;
                        asset.bytes = std::move(image_bytes);
                        const std::size_t index = media_->size();
                        media_->push_back(std::move(asset));
                        media_by_digest_.emplace(digest, index);
                        known = media_by_digest_.find(digest);
                        if (diagnostics_ != nullptr) ++diagnostics_->collected_images;
                    }
                    item.image_target = (*media_)[known->second].markdown_target;
                }
            }
        }
        slide_->items.push_back(std::move(item));
        picture_ = {};
        picture_active_ = false;
        return ok_error();
    }

    Error on_event(const XmlEvent& event) {
        if (skip_depth_ > 0) {
            if (event.kind == XmlEvent::Kind::Start) ++skip_depth_;
            else if (event.kind == XmlEvent::Kind::End) --skip_depth_;
            return ok_error();
        }
        if (event.kind == XmlEvent::Kind::Start && event.name.local == "AlternateContent") {
            alternate_choices_.push_back(false);
            return ok_error();
        }
        if (event.kind == XmlEvent::Kind::Start && event.name.local == "Choice" &&
            !alternate_choices_.empty()) {
            alternate_choices_.back() = true;
            return ok_error();
        }
        if (event.kind == XmlEvent::Kind::Start && event.name.local == "Fallback" &&
            !alternate_choices_.empty() && alternate_choices_.back()) {
            skip_depth_ = 1;
            return ok_error();
        }
        if (event.kind == XmlEvent::Kind::End && event.name.local == "AlternateContent") {
            if (!alternate_choices_.empty()) alternate_choices_.pop_back();
            return ok_error();
        }
        if (event.name.local == "Choice" || event.name.local == "Fallback") return ok_error();
        if (event.kind == XmlEvent::Kind::Text) {
            if (text_depth_ > 0 && run_active_) run_.text += event.text;
            return ok_error();
        }

        if (event.kind == XmlEvent::Kind::Start) {
            if (detail::is_p(event, "sp")) {
                Error error = count_object();
                if (!error.ok()) return error;
                shape_active_ = true;
                shape_ = {};
            } else if (detail::is_p(event, "pic")) {
                Error error = count_object();
                if (!error.ok()) return error;
                picture_active_ = true;
                picture_ = {};
            } else if (detail::is_p(event, "cxnSp")) {
                Error error = count_object();
                if (!error.ok()) return error;
                if (!notes_only_) {
                    append_placeholder(*slide_, "unsupported PPTX connector omitted", diagnostics_);
                    detail::diagnostic(diagnostics_,
                        "unsupported connector shapes were replaced with visible placeholders");
                }
            } else if (detail::is_p(event, "oleObj") || detail::is_p(event, "video") ||
                       detail::is_p(event, "audio")) {
                if (!notes_only_) {
                    append_placeholder(*slide_, "unsupported PPTX embedded object omitted", diagnostics_);
                    detail::diagnostic(diagnostics_,
                        "OLE, audio, or video objects were replaced with visible placeholders");
                }
            }

            if (shape_active_ && detail::is_p(event, "ph")) {
                const std::string* type = detail::attr_local(event, "type");
                const std::string lower = type == nullptr ? std::string() : ascii_lower(*type);
                if (lower == "title" || lower == "ctrtitle") shape_.title = true;
                if (notes_only_ && (lower == "sldnum" || lower == "dt" || lower == "ftr" ||
                                    lower == "hdr")) shape_.skip = true;
            }
            if ((shape_active_ || table_cell_active_) && detail::is_a(event, "p")) begin_paragraph();
            if (paragraph_active_ && detail::is_a(event, "pPr")) {
                paragraph_.level = bounded_int(detail::attr_local(event, "lvl"), 0, 8);
            } else if (paragraph_active_ && detail::is_a(event, "buChar")) {
                paragraph_.bullet = true;
            } else if (paragraph_active_ && detail::is_a(event, "buAutoNum")) {
                paragraph_.ordered = true;
                paragraph_.start = bounded_int(detail::attr_local(event, "startAt"), 1, 1000000);
            } else if (paragraph_active_ && detail::is_a(event, "buNone")) {
                paragraph_.ordered = false;
                paragraph_.bullet = false;
            } else if (paragraph_active_ && (detail::is_a(event, "r") || detail::is_a(event, "fld"))) {
                finish_run();
                run_active_ = true;
                run_ = {};
            } else if (paragraph_active_ && detail::is_a(event, "rPr")) {
                if (!run_active_) run_active_ = true;
                if (true_value(detail::attr_local(event, "b"))) {
                    if (detail::attr_local(event, "b") != nullptr)
                        run_.style |= static_cast<unsigned>(markdown::RunStyle::Bold);
                }
                if (true_value(detail::attr_local(event, "i"))) {
                    if (detail::attr_local(event, "i") != nullptr)
                        run_.style |= static_cast<unsigned>(markdown::RunStyle::Italic);
                }
                const std::string* underline = detail::attr_local(event, "u");
                if (underline != nullptr && ascii_lower(*underline) != "none")
                    run_.style |= static_cast<unsigned>(markdown::RunStyle::Underline);
                const std::string* strike = detail::attr_local(event, "strike");
                if (strike != nullptr && ascii_lower(*strike) != "nostrike")
                    run_.style |= static_cast<unsigned>(markdown::RunStyle::Strike);
            } else if (paragraph_active_ && detail::is_a(event, "hlinkClick")) {
                if (const std::string* rid = detail::attr_rid(event, "id")) hyperlink_rid_ = *rid;
            } else if (paragraph_active_ && detail::is_a(event, "t")) {
                if (!run_active_) run_active_ = true;
                ++text_depth_;
            } else if (paragraph_active_ && detail::is_a(event, "br")) {
                if (!run_active_) run_active_ = true;
                run_.hard_break_after = true;
                finish_run();
            }

            if (picture_active_ && detail::is_p(event, "cNvPr")) {
                if (const std::string* alt = detail::attr_local(event, "descr")) picture_.alt = *alt;
                if (picture_.alt.empty()) {
                    if (const std::string* name = detail::attr_local(event, "name")) picture_.alt = *name;
                }
            } else if (picture_active_ && detail::is_a(event, "blip")) {
                if (const std::string* rid = detail::attr_rid(event, "embed")) picture_.rid = *rid;
            }

            if (detail::is_a(event, "tbl")) {
                Error error = count_object();
                if (!error.ok()) return error;
                table_active_ = true;
                table_.clear();
            } else if (table_active_ && detail::is_a(event, "tr")) {
                table_row_active_ = true;
                table_row_.clear();
            } else if (table_row_active_ && detail::is_a(event, "tc")) {
                Error error = count_object();
                if (!error.ok()) return error;
                table_cell_active_ = true;
                table_cell_.clear();
                table_cell_continuation_ = false;
                const std::string* h_merge = detail::attr_local(event, "hMerge");
                const std::string* v_merge = detail::attr_local(event, "vMerge");
                const std::string* grid_span = detail::attr_local(event, "gridSpan");
                const std::string* row_span = detail::attr_local(event, "rowSpan");
                table_cell_continuation_ = (h_merge != nullptr && true_value(h_merge)) ||
                                           (v_merge != nullptr && true_value(v_merge));
                if (h_merge != nullptr || v_merge != nullptr || grid_span != nullptr || row_span != nullptr) {
                    saw_table_merge_ = true;
                }
            }

            if (detail::is_a(event, "graphicData")) {
                const std::string uri = detail::attr_local(event, "uri") == nullptr
                                            ? std::string() : *detail::attr_local(event, "uri");
                if (uri.find("/chart") != std::string::npos) graphic_placeholder_ = "chart";
                else if (uri.find("/diagram") != std::string::npos) graphic_placeholder_ = "SmartArt";
            }
            if ((detail::is_a(event, "effectLst") || detail::is_a(event, "effectDag")) && !notes_only_)
                detail::diagnostic(diagnostics_,
                    "PPTX layout and visual effects were not preserved in Markdown");
            return ok_error();
        }

        if (paragraph_active_ && detail::is_a(event, "t")) {
            if (text_depth_ > 0) --text_depth_;
        } else if (paragraph_active_ && (detail::is_a(event, "r") || detail::is_a(event, "fld"))) {
            finish_run();
        } else if (paragraph_active_ && detail::is_a(event, "p")) {
            return finish_paragraph();
        } else if (table_cell_active_ && detail::is_a(event, "tc")) {
            if (table_cell_continuation_) table_cell_.clear();
            table_row_.push_back(std::move(table_cell_));
            table_cell_ = {};
            table_cell_active_ = false;
        } else if (table_row_active_ && detail::is_a(event, "tr")) {
            table_.push_back(std::move(table_row_));
            table_row_ = {};
            table_row_active_ = false;
        } else if (table_active_ && detail::is_a(event, "tbl")) {
            if (!notes_only_) {
                Item item;
                item.kind = Item::Kind::Table;
                item.table = std::move(table_);
                slide_->items.push_back(std::move(item));
            }
            table_ = {};
            table_active_ = false;
            if (saw_table_merge_) {
                if (diagnostics_ != nullptr) ++diagnostics_->flattened_merged_tables;
                detail::diagnostic(diagnostics_,
                    "merged PPTX table continuations were flattened to empty Markdown cells");
                saw_table_merge_ = false;
            }
        } else if (detail::is_p(event, "sp")) {
            return finish_shape();
        } else if (detail::is_p(event, "pic")) {
            return finish_picture();
        } else if (detail::is_p(event, "graphicFrame")) {
            if (!graphic_placeholder_.empty() && !notes_only_) {
                append_placeholder(*slide_, "unsupported PPTX " + graphic_placeholder_ + " omitted", diagnostics_);
                detail::diagnostic(diagnostics_,
                    "charts and SmartArt were replaced with visible Markdown placeholders");
            }
            graphic_placeholder_.clear();
        }
        return ok_error();
    }

    const RelationshipMap& relationships_;
    ZipArchive& archive_;
    const ContentTypes& content_types_;
    const ReadOptions& options_;
    Diagnostics* diagnostics_;
    std::vector<MediaAsset>* media_;
    std::unordered_map<std::string, std::size_t>& media_by_digest_;
    std::size_t& object_count_;
    std::string slide_part_;
    bool notes_only_ = false;
    Slide* slide_ = nullptr;
    bool shape_active_ = false;
    ShapeState shape_;
    bool picture_active_ = false;
    PictureState picture_;
    bool paragraph_active_ = false;
    Paragraph paragraph_;
    bool run_active_ = false;
    markdown::Run run_;
    int text_depth_ = 0;
    std::string hyperlink_rid_;
    bool table_active_ = false;
    bool table_row_active_ = false;
    bool table_cell_active_ = false;
    bool table_cell_continuation_ = false;
    bool saw_table_merge_ = false;
    std::vector<std::vector<std::vector<Paragraph>>> table_;
    std::vector<std::vector<Paragraph>> table_row_;
    std::vector<Paragraph> table_cell_;
    std::string graphic_placeholder_;
    int skip_depth_ = 0;
    std::vector<bool> alternate_choices_;
};

std::string table_cell_markdown(const std::vector<Paragraph>& paragraphs) {
    std::string out;
    for (std::size_t i = 0; i < paragraphs.size(); ++i) {
        if (i != 0) out += "<br>";
        out += format_paragraph(paragraphs[i], true);
    }
    return out;
}

void emit_slide(const Slide& slide, std::string& markdown) {
    bool first_title = true;
    for (const Item& item : slide.items) {
        if (item.kind == Item::Kind::Paragraph) {
            if (item.title && first_title) {
                markdown += "# ";
                first_title = false;
            } else if (item.paragraph.ordered || item.paragraph.bullet) {
                markdown.append(static_cast<std::size_t>(std::max(0, item.paragraph.level)) * 2U, ' ');
                markdown += item.paragraph.ordered ? std::to_string(item.paragraph.start) + ". " : "- ";
            }
            std::string paragraph_text = format_paragraph(item.paragraph);
            if (paragraph_text == "---") paragraph_text.insert(paragraph_text.begin(), '\\');
            markdown += paragraph_text;
            markdown += "\n\n";
        } else if (item.kind == Item::Kind::Image) {
            if (item.image_target.empty()) {
                markdown += item.alt.empty() ? "[image omitted]" :
                    "[image omitted: " + detail::markdown_escape(item.alt) + "]";
            } else {
                markdown += "![" + detail::markdown_escape(item.alt) + "](" + item.image_target + ")";
            }
            markdown += "\n\n";
        } else if (item.kind == Item::Kind::Placeholder) {
            markdown += item.placeholder + "\n\n";
        } else if (item.kind == Item::Kind::Table) {
            std::size_t columns = 0;
            for (const auto& row : item.table) columns = std::max(columns, row.size());
            if (columns == 0) continue;
            auto emit_row = [&](std::size_t row_index) {
                markdown += '|';
                for (std::size_t column = 0; column < columns; ++column) {
                    markdown += ' ';
                    if (row_index < item.table.size() && column < item.table[row_index].size())
                        markdown += table_cell_markdown(item.table[row_index][column]);
                    markdown += " |";
                }
                markdown += '\n';
            };
            emit_row(0);
            markdown += '|';
            for (std::size_t column = 0; column < columns; ++column) markdown += " --- |";
            markdown += '\n';
            for (std::size_t row = 1; row < item.table.size(); ++row) emit_row(row);
            markdown += '\n';
        }
    }
    if (!slide.notes.empty()) {
        markdown += "> **Speaker notes:**\n>\n";
        for (std::size_t i = 0; i < slide.notes.size(); ++i) {
            std::string text = format_paragraph(slide.notes[i]);
            std::size_t start = 0;
            while (start <= text.size()) {
                const std::size_t newline = text.find('\n', start);
                markdown += "> ";
                markdown += text.substr(start, newline == std::string::npos ? std::string::npos : newline - start);
                markdown += '\n';
                if (newline == std::string::npos) break;
                start = newline + 1;
            }
            if (i + 1 < slide.notes.size()) markdown += ">\n";
        }
        markdown += '\n';
    }
    while (!markdown.empty() && markdown.back() == '\n') markdown.pop_back();
}

Error read_part_relationships(ZipArchive& archive,
                              const std::string& source_part,
                              runtime::CancellationToken cancellation,
                              RelationshipMap& relationships) {
    relationships.clear();
    const std::string rels = detail::relationships_part(source_part);
    if (!archive.contains(rels)) return ok_error();
    std::string xml;
    Error error = archive.read(rels, xml);
    if (!error.ok()) return error;
    return detail::parse_relationships(xml, cancellation, relationships);
}

Error extract_pptx(std::string_view bytes,
                   const ReadOptions& options,
                   std::string& markdown,
                   Diagnostics* diagnostics,
                   std::vector<MediaAsset>* media) {
    markdown.clear();
    if (diagnostics != nullptr) *diagnostics = {};
    if (media != nullptr) media->clear();
    if (bytes.size() > options.max_bytes)
        return {ErrorCode::UnsupportedFeature, "PPTX exceeds input size limit of " +
                                                   std::to_string(options.max_bytes) + " bytes"};
    if (options.cancellation.cancelled()) return {ErrorCode::Cancelled, "PPTX conversion cancelled"};

    ooxml::ZipLimits zip_limits;
    zip_limits.max_members = options.max_members;
    zip_limits.max_member_bytes = options.max_xml_part_bytes;
    if (options.collect_media)
        zip_limits.max_member_bytes = std::max(zip_limits.max_member_bytes, options.max_bytes);
    zip_limits.max_total_selected_bytes = options.max_selected_xml_bytes;
    if (options.collect_media && options.max_bytes <= std::numeric_limits<std::size_t>::max() -
                                                       zip_limits.max_total_selected_bytes)
        zip_limits.max_total_selected_bytes += options.max_bytes;
    ZipArchive archive;
    Error error = archive.open(bytes, zip_limits, options.cancellation);
    if (!error.ok()) return error;
    if (!archive.contains("[Content_Types].xml"))
        return {ErrorCode::FileRead, "invalid PPTX package: [Content_Types].xml is missing"};
    if (!archive.contains("_rels/.rels"))
        return {ErrorCode::FileRead, "invalid PPTX package: package relationships are missing"};

    std::string xml;
    error = archive.read("[Content_Types].xml", xml);
    if (!error.ok()) return error;
    ContentTypes content_types;
    error = parse_content_types(xml, options.cancellation, content_types);
    if (!error.ok()) return error;

    error = archive.read("_rels/.rels", xml);
    if (!error.ok()) return error;
    RelationshipMap root_relationships;
    error = detail::parse_relationships(xml, options.cancellation, root_relationships);
    if (!error.ok()) return error;
    std::string presentation_part;
    for (const auto& entry : root_relationships) {
        if (!detail::type_suffix(entry.second.type, "/officeDocument")) continue;
        if (!presentation_part.empty())
            return {ErrorCode::FileRead, "invalid PPTX package: multiple officeDocument relationships"};
        if (entry.second.external)
            return {ErrorCode::FileRead, "invalid PPTX package: presentation relationship is external"};
        error = ooxml::resolve_part_name({}, entry.second.target, presentation_part);
        if (!error.ok()) return error;
    }
    if (presentation_part.empty())
        return {ErrorCode::FileRead, "invalid PPTX package: officeDocument relationship is missing"};
    if (!archive.contains(presentation_part))
        return {ErrorCode::FileRead, "invalid PPTX package: presentation part is missing: " +
                                        presentation_part};

    RelationshipMap presentation_relationships;
    error = read_part_relationships(archive, presentation_part, options.cancellation,
                                    presentation_relationships);
    if (!error.ok()) return error;
    error = archive.read(presentation_part, xml);
    if (!error.ok()) return error;
    std::vector<std::string> slide_rids;
    std::uint64_t width = 12192000;
    std::uint64_t height = 6858000;
    error = ooxml::scan_xml(xml, {}, options.cancellation, [&](const XmlEvent& event) -> Error {
        if (event.kind != XmlEvent::Kind::Start) return ok_error();
        if (detail::is_p(event, "sldId")) {
            const std::string* rid = detail::attr_rid(event, "id");
            if (rid == nullptr || rid->empty())
                return {ErrorCode::FileRead, "invalid PPTX presentation: slide is missing r:id"};
            if (slide_rids.size() >= options.max_slides)
                return {ErrorCode::UnsupportedFeature, "PPTX exceeds the 2,048 slide conversion limit"};
            slide_rids.push_back(*rid);
        } else if (detail::is_p(event, "sldSz")) {
            auto parse_dimension = [](const std::string* value, std::uint64_t& output) {
                if (value == nullptr) return;
                try {
                    const unsigned long long parsed = std::stoull(*value);
                    if (parsed >= 914400ULL && parsed <= 100000000ULL) output = parsed;
                } catch (...) {
                }
            };
            parse_dimension(detail::attr_local(event, "cx"), width);
            parse_dimension(detail::attr_local(event, "cy"), height);
        }
        return ok_error();
    });
    if (!error.ok()) return error;
    if (slide_rids.empty())
        return {ErrorCode::FileRead, "invalid PPTX presentation: no slides were found"};

    std::vector<Slide> slides;
    slides.reserve(slide_rids.size());
    std::unordered_map<std::string, std::size_t> media_by_digest;
    std::size_t object_count = 0;
    for (const std::string& rid : slide_rids) {
        if (options.cancellation.cancelled()) {
            markdown.clear();
            if (media != nullptr) media->clear();
            return {ErrorCode::Cancelled, "PPTX conversion cancelled"};
        }
        Slide slide;
        const auto relation = presentation_relationships.find(rid);
        if (relation == presentation_relationships.end() || relation->second.external ||
            !detail::type_suffix(relation->second.type, "/slide")) {
            append_placeholder(slide, "missing PPTX slide relationship", diagnostics);
            if (diagnostics != nullptr) ++diagnostics->missing_relationships;
            detail::diagnostic(diagnostics,
                "a slide relationship was missing or invalid and was replaced with a visible placeholder");
            slides.push_back(std::move(slide));
            continue;
        }
        std::string slide_part;
        error = ooxml::resolve_part_name(presentation_part, relation->second.target, slide_part);
        if (!error.ok()) return error;
        if (!archive.contains(slide_part)) {
            append_placeholder(slide, "missing PPTX slide part", diagnostics);
            if (diagnostics != nullptr) ++diagnostics->missing_relationships;
            detail::diagnostic(diagnostics,
                "a slide part was missing and was replaced with a visible placeholder");
            slides.push_back(std::move(slide));
            continue;
        }
        RelationshipMap slide_relationships;
        error = read_part_relationships(archive, slide_part, options.cancellation,
                                        slide_relationships);
        if (!error.ok()) return error;
        error = archive.read(slide_part, xml);
        if (!error.ok()) return error;
        SlideParser parser(slide_relationships, archive, content_types, options, diagnostics,
                           media, media_by_digest, object_count, slide_part, false);
        error = parser.parse(xml, slide);
        if (!error.ok()) {
            markdown.clear();
            if (media != nullptr) media->clear();
            return error;
        }

        for (const auto& entry : slide_relationships) {
            if (!detail::type_suffix(entry.second.type, "/notesSlide") || entry.second.external) continue;
            std::string notes_part;
            error = ooxml::resolve_part_name(slide_part, entry.second.target, notes_part);
            if (!error.ok()) return error;
            if (!archive.contains(notes_part)) {
                if (diagnostics != nullptr) ++diagnostics->missing_relationships;
                detail::diagnostic(diagnostics, "a speaker-notes part was missing and was omitted");
                break;
            }
            RelationshipMap notes_relationships;
            error = read_part_relationships(archive, notes_part, options.cancellation,
                                            notes_relationships);
            if (!error.ok()) return error;
            error = archive.read(notes_part, xml);
            if (!error.ok()) return error;
            SlideParser notes_parser(notes_relationships, archive, content_types, options,
                                     diagnostics, nullptr, media_by_digest, object_count,
                                     notes_part, true);
            error = notes_parser.parse(xml, slide);
            if (!error.ok()) return error;
            break;
        }
        slides.push_back(std::move(slide));
    }

    try {
        markdown = "<!-- ainiux-pptx width=" + std::to_string(width) +
                   " height=" + std::to_string(height) + " -->\n\n";
        for (std::size_t index = 0; index < slides.size(); ++index) {
            if (index != 0) markdown += "\n\n---\n\n";
            emit_slide(slides[index], markdown);
        }
        markdown.push_back('\n');
    } catch (const std::bad_alloc&) {
        markdown.clear();
        if (media != nullptr) media->clear();
        return {ErrorCode::Internal, "not enough memory to emit PPTX Markdown"};
    } catch (const std::length_error&) {
        markdown.clear();
        if (media != nullptr) media->clear();
        return {ErrorCode::UnsupportedFeature, "converted PPTX Markdown is too large"};
    }
    return ok_error();
}

Error read_file_bounded(const std::string& path,
                        const ReadOptions& options,
                        std::string& bytes) {
    bytes.clear();
    const std::string resolved = expand_user_path(path);
    std::ifstream file(std::filesystem::u8path(resolved), std::ios::binary);
    if (!file) return {ErrorCode::FileRead, "could not open PPTX for reading: " + resolved};
    std::array<char, 65536> buffer{};
    try {
        while (file) {
            if (options.cancellation.cancelled())
                return {ErrorCode::Cancelled, "PPTX read cancelled: " + resolved};
            file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const std::streamsize count = file.gcount();
            if (count <= 0) break;
            const std::size_t chunk = static_cast<std::size_t>(count);
            if (bytes.size() > options.max_bytes || chunk > options.max_bytes - bytes.size()) {
                bytes.clear();
                return {ErrorCode::UnsupportedFeature, "PPTX exceeds input size limit of " +
                    std::to_string(options.max_bytes) + " bytes: " + resolved};
            }
            bytes.append(buffer.data(), chunk);
        }
    } catch (const std::bad_alloc&) {
        bytes.clear();
        return {ErrorCode::Internal, "not enough memory to read PPTX: " + resolved};
    }
    if (file.bad()) return {ErrorCode::FileRead, "could not read PPTX: " + resolved};
    return ok_error();
}

}  // namespace

Error to_markdown_file(const std::string& path,
                       const ReadOptions& options,
                       std::string& markdown,
                       Diagnostics* diagnostics,
                       std::vector<MediaAsset>* media) {
    markdown.clear();
    if (media != nullptr) media->clear();
    if (diagnostics != nullptr) *diagnostics = {};
    std::string bytes;
    Error error = read_file_bounded(path, options, bytes);
    if (!error.ok()) return error;
    error = extract_pptx(bytes, options, markdown, diagnostics, media);
    if (!error.ok()) {
        markdown.clear();
        if (media != nullptr) media->clear();
    }
    return error;
}

Error to_markdown_bytes(std::string_view bytes,
                        const ReadOptions& options,
                        std::string& markdown,
                        Diagnostics* diagnostics,
                        std::vector<MediaAsset>* media) {
    Error error = extract_pptx(bytes, options, markdown, diagnostics, media);
    if (!error.ok()) {
        markdown.clear();
        if (media != nullptr) media->clear();
    }
    return error;
}

bool looks_like_pptx(std::string_view bytes) {
    const bool zip = bytes.size() >= 4 && bytes[0] == 'P' && bytes[1] == 'K' &&
        ((bytes[2] == '\x03' && bytes[3] == '\x04') ||
         (bytes[2] == '\x05' && bytes[3] == '\x06') ||
         (bytes[2] == '\x07' && bytes[3] == '\x08'));
    if (!zip) return false;
    ooxml::ZipLimits limits;
    limits.max_members = 10000;
    limits.max_member_bytes = std::max<std::size_t>(bytes.size(), 1024U * 1024U);
    limits.max_total_selected_bytes = 2U * 1024U * 1024U;
    ZipArchive archive;
    if (!archive.open(bytes, limits, {}).ok() ||
        !archive.contains("[Content_Types].xml") || !archive.contains("_rels/.rels")) return false;
    std::string relationships_xml;
    if (!archive.read("_rels/.rels", relationships_xml).ok()) return false;
    RelationshipMap relationships;
    if (!detail::parse_relationships(relationships_xml, {}, relationships).ok()) return false;
    for (const auto& item : relationships) {
        if (!detail::type_suffix(item.second.type, "/officeDocument") || item.second.external) continue;
        std::string main_part;
        if (!ooxml::resolve_part_name({}, item.second.target, main_part).ok()) return false;
        return main_part.rfind("ppt/", 0) == 0 && archive.contains(main_part) &&
               !archive.contains("word/document.xml") && !archive.contains("xl/workbook.xml");
    }
    return false;
}

}  // namespace ainiux::pptx
