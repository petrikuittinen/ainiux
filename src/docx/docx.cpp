#include "docx/docx.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <new>
#include <set>
#include <sstream>
#include <unordered_map>
#include <utility>

#include "docx/xml.hpp"
#include "docx/zip.hpp"
#include "html/html.hpp"
#include "markdown/blocks.hpp"

namespace ainiux::docx {
namespace {

using detail::XmlEvent;

constexpr const char* kWordTransitional =
    "http://schemas.openxmlformats.org/wordprocessingml/2006/main";
constexpr const char* kWordStrict =
    "http://purl.oclc.org/ooxml/wordprocessingml/main";
constexpr const char* kOfficeRelationships =
    "http://schemas.openxmlformats.org/officeDocument/2006/relationships";
constexpr const char* kOfficeRelationshipsStrict =
    "http://purl.oclc.org/ooxml/officeDocument/relationships";
constexpr const char* kMarkupCompatibility =
    "http://schemas.openxmlformats.org/markup-compatibility/2006";
constexpr const char* kMainContentType =
    "application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml";

bool is_word(const XmlEvent& event, std::string_view local) {
    return event.name.local == local &&
           (event.name.uri == kWordTransitional || event.name.uri == kWordStrict);
}

const std::string* attr_local(const XmlEvent& event, std::string_view local) {
    return detail::attribute_local(event, local);
}

bool type_suffix(const std::string& type, std::string_view suffix) {
    return type.size() >= suffix.size() &&
           type.compare(type.size() - suffix.size(), suffix.size(), suffix) == 0;
}

int integer_value(const std::string* value, int fallback = 0) {
    if (value == nullptr || value->empty()) return fallback;
    bool negative = false;
    std::size_t pos = 0;
    if ((*value)[0] == '-') {
        negative = true;
        pos = 1;
    }
    if (pos == value->size()) return fallback;
    long long result = 0;
    for (; pos < value->size(); ++pos) {
        const unsigned char ch = static_cast<unsigned char>((*value)[pos]);
        if (!std::isdigit(ch)) return fallback;
        if (result > (std::numeric_limits<int>::max() - (ch - '0')) / 10LL)
            return negative ? std::numeric_limits<int>::min() : std::numeric_limits<int>::max();
        result = result * 10 + (ch - '0');
    }
    return negative ? -static_cast<int>(result) : static_cast<int>(result);
}

bool bool_value(const std::string* value) {
    if (value == nullptr) return true;
    const std::string lower = ascii_lower(*value);
    return lower != "0" && lower != "false" && lower != "off" && lower != "none";
}

void reset_diagnostics(Diagnostics* diagnostics) {
    if (diagnostics != nullptr) *diagnostics = Diagnostics{};
}

void diagnostic(Diagnostics* diagnostics, const std::string& message) {
    if (diagnostics == nullptr || diagnostics->messages.size() >= 16U ||
        std::find(diagnostics->messages.begin(), diagnostics->messages.end(), message) !=
            diagnostics->messages.end()) return;
    diagnostics->messages.push_back(message);
}

struct OptionalBool {
    bool set = false;
    bool value = false;
};

struct RunProperties {
    OptionalBool bold;
    OptionalBool italic;
    OptionalBool underline;
    OptionalBool strike;
    std::string style_id;
};

struct ParagraphProperties {
    bool style_set = false;
    std::string style_id;
    bool num_set = false;
    int num_id = 0;
    bool level_set = false;
    int level = 0;
    bool outline_set = false;
    int outline = -1;
    OptionalBool bidi;
};

void merge(OptionalBool& base, const OptionalBool& derived) {
    if (derived.set) base = derived;
}

void merge(RunProperties& base, const RunProperties& derived) {
    merge(base.bold, derived.bold);
    merge(base.italic, derived.italic);
    merge(base.underline, derived.underline);
    merge(base.strike, derived.strike);
    if (!derived.style_id.empty()) base.style_id = derived.style_id;
}

void merge(ParagraphProperties& base, const ParagraphProperties& derived) {
    if (derived.style_set) {
        base.style_set = true;
        base.style_id = derived.style_id;
    }
    if (derived.num_set) {
        base.num_set = true;
        base.num_id = derived.num_id;
    }
    if (derived.level_set) {
        base.level_set = true;
        base.level = derived.level;
    }
    if (derived.outline_set) {
        base.outline_set = true;
        base.outline = derived.outline;
    }
    merge(base.bidi, derived.bidi);
}

struct Style {
    std::string id;
    std::string name;
    std::string based_on;
    std::string type;
    ParagraphProperties paragraph;
    RunProperties run;
    bool is_default = false;
    bool flattened = false;
    bool flattening = false;
};

struct Styles {
    std::unordered_map<std::string, Style> values;
    RunProperties default_run;
    ParagraphProperties default_paragraph;
    std::string default_paragraph_style;
};

void set_run_property(const XmlEvent& event, RunProperties& properties) {
    OptionalBool* target = nullptr;
    if (is_word(event, "b") || is_word(event, "bCs")) target = &properties.bold;
    else if (is_word(event, "i") || is_word(event, "iCs")) target = &properties.italic;
    else if (is_word(event, "strike") || is_word(event, "dstrike")) target = &properties.strike;
    else if (is_word(event, "u")) target = &properties.underline;
    if (target != nullptr) {
        target->set = true;
        target->value = bool_value(attr_local(event, "val"));
    } else if (is_word(event, "rStyle")) {
        if (const std::string* value = attr_local(event, "val")) properties.style_id = *value;
    }
    // rFonts, sz, and color are intentionally recognized by the tokenizer but
    // normalized at the Markdown boundary.
}

void set_paragraph_property(const XmlEvent& event, ParagraphProperties& properties) {
    if (is_word(event, "pStyle")) {
        if (const std::string* value = attr_local(event, "val")) {
            properties.style_set = true;
            properties.style_id = *value;
        }
    } else if (is_word(event, "numId")) {
        properties.num_set = true;
        properties.num_id = integer_value(attr_local(event, "val"));
    } else if (is_word(event, "ilvl")) {
        properties.level_set = true;
        properties.level = integer_value(attr_local(event, "val"));
    } else if (is_word(event, "outlineLvl")) {
        properties.outline_set = true;
        properties.outline = integer_value(attr_local(event, "val"), -1);
    } else if (is_word(event, "bidi")) {
        properties.bidi.set = true;
        properties.bidi.value = bool_value(attr_local(event, "val"));
    }
}

Error parse_styles(std::string_view xml,
                   runtime::CancellationToken cancellation,
                   Styles& styles) {
    Style current;
    bool in_style = false;
    int paragraph_properties = 0;
    int run_properties = 0;
    int default_run_properties = 0;
    int default_paragraph_properties = 0;
    Error error = detail::scan_xml(xml, {}, cancellation, [&](const XmlEvent& event) -> Error {
        if (event.kind == XmlEvent::Kind::Start) {
            if (is_word(event, "style")) {
                current = Style{};
                in_style = true;
                if (const std::string* value = attr_local(event, "styleId")) current.id = *value;
                if (const std::string* value = attr_local(event, "type")) current.type = *value;
                if (const std::string* value = attr_local(event, "default"))
                    current.is_default = bool_value(value);
            } else if (is_word(event, "pPr")) {
                ++paragraph_properties;
                if (!in_style) ++default_paragraph_properties;
            } else if (is_word(event, "rPr")) {
                ++run_properties;
                if (!in_style) ++default_run_properties;
            } else if (in_style && is_word(event, "name")) {
                if (const std::string* value = attr_local(event, "val")) current.name = *value;
            } else if (in_style && is_word(event, "basedOn")) {
                if (const std::string* value = attr_local(event, "val")) current.based_on = *value;
            }
            if (run_properties > 0) {
                if (in_style) set_run_property(event, current.run);
                else if (default_run_properties > 0) set_run_property(event, styles.default_run);
            }
            if (paragraph_properties > 0) {
                if (in_style) set_paragraph_property(event, current.paragraph);
                else if (default_paragraph_properties > 0)
                    set_paragraph_property(event, styles.default_paragraph);
            }
        } else if (event.kind == XmlEvent::Kind::End) {
            if (is_word(event, "style")) {
                if (!current.id.empty()) {
                    if (current.is_default && current.type == "paragraph")
                        styles.default_paragraph_style = current.id;
                    styles.values[current.id] = std::move(current);
                }
                in_style = false;
            } else if (is_word(event, "pPr")) {
                if (!in_style && default_paragraph_properties > 0) --default_paragraph_properties;
                if (paragraph_properties > 0) --paragraph_properties;
            } else if (is_word(event, "rPr")) {
                if (!in_style && default_run_properties > 0) --default_run_properties;
                if (run_properties > 0) --run_properties;
            }
        }
        return ok_error();
    });
    if (!error.ok()) return error;

    std::function<Error(Style&)> flatten = [&](Style& style) -> Error {
        if (style.flattened) return ok_error();
        if (style.flattening) return {ErrorCode::FileRead, "invalid DOCX styles: basedOn cycle at " + style.id};
        style.flattening = true;
        if (!style.based_on.empty()) {
            const auto parent = styles.values.find(style.based_on);
            if (parent != styles.values.end()) {
                Error parent_error = flatten(parent->second);
                if (!parent_error.ok()) return parent_error;
                ParagraphProperties paragraph = parent->second.paragraph;
                RunProperties run = parent->second.run;
                merge(paragraph, style.paragraph);
                merge(run, style.run);
                style.paragraph = std::move(paragraph);
                style.run = std::move(run);
            }
        }
        style.flattening = false;
        style.flattened = true;
        return ok_error();
    };
    for (auto& item : styles.values) {
        error = flatten(item.second);
        if (!error.ok()) return error;
    }
    return ok_error();
}

struct NumberLevel {
    int start = 1;
    // -1 is the Word default (restart after the immediately higher level),
    // 0 means never restart, and positive values identify the one-based level
    // whose advance triggers a restart.
    int restart = -1;
    std::string format = "decimal";
    std::string paragraph_style;
};

struct AbstractNumbering {
    std::map<int, NumberLevel> levels;
};

struct Numbering {
    std::unordered_map<int, AbstractNumbering> abstracts;
    std::unordered_map<int, int> instances;
    std::map<std::pair<int, int>, int> start_overrides;
};

Error parse_numbering(std::string_view xml,
                      runtime::CancellationToken cancellation,
                      Numbering& numbering) {
    int abstract_id = -1;
    int level_id = -1;
    NumberLevel level;
    int num_id = -1;
    int override_level = -1;
    return detail::scan_xml(xml, {}, cancellation, [&](const XmlEvent& event) -> Error {
        if (event.kind == XmlEvent::Kind::Start) {
            if (is_word(event, "abstractNum")) {
                abstract_id = integer_value(attr_local(event, "abstractNumId"), -1);
            } else if (is_word(event, "lvl") && abstract_id >= 0) {
                level_id = integer_value(attr_local(event, "ilvl"), 0);
                level = NumberLevel{};
            } else if (level_id >= 0 && is_word(event, "start")) {
                level.start = std::max(0, integer_value(attr_local(event, "val"), 1));
            } else if (level_id >= 0 && is_word(event, "numFmt")) {
                if (const std::string* value = attr_local(event, "val")) level.format = *value;
            } else if (level_id >= 0 && is_word(event, "pStyle")) {
                if (const std::string* value = attr_local(event, "val")) level.paragraph_style = *value;
            } else if (level_id >= 0 && is_word(event, "lvlRestart")) {
                level.restart = integer_value(attr_local(event, "val"));
            } else if (is_word(event, "num")) {
                num_id = integer_value(attr_local(event, "numId"), -1);
            } else if (num_id >= 0 && is_word(event, "abstractNumId")) {
                numbering.instances[num_id] = integer_value(attr_local(event, "val"), -1);
            } else if (num_id >= 0 && is_word(event, "lvlOverride")) {
                override_level = integer_value(attr_local(event, "ilvl"), 0);
            } else if (num_id >= 0 && override_level >= 0 && is_word(event, "startOverride")) {
                numbering.start_overrides[{num_id, override_level}] =
                    std::max(0, integer_value(attr_local(event, "val"), 1));
            }
        } else if (event.kind == XmlEvent::Kind::End) {
            if (is_word(event, "lvl") && abstract_id >= 0 && level_id >= 0) {
                numbering.abstracts[abstract_id].levels[level_id] = std::move(level);
                level_id = -1;
            } else if (is_word(event, "abstractNum")) {
                abstract_id = -1;
            } else if (is_word(event, "lvlOverride")) {
                override_level = -1;
            } else if (is_word(event, "num")) {
                num_id = -1;
            }
        }
        return ok_error();
    });
}

struct Relationship {
    std::string type;
    std::string target;
    bool external = false;
};

using Relationships = std::unordered_map<std::string, Relationship>;

Error parse_relationships(std::string_view xml,
                          runtime::CancellationToken cancellation,
                          Relationships& relationships) {
    return detail::scan_xml(xml, {}, cancellation, [&](const XmlEvent& event) -> Error {
        if (event.kind != XmlEvent::Kind::Start || event.name.local != "Relationship")
            return ok_error();
        const std::string* id = attr_local(event, "Id");
        const std::string* type = attr_local(event, "Type");
        const std::string* target = attr_local(event, "Target");
        if (id == nullptr || id->empty() || type == nullptr || target == nullptr || target->empty())
            return {ErrorCode::FileRead, "invalid DOCX relationships: Relationship is missing Id, Type, or Target"};
        if (relationships.find(*id) != relationships.end())
            return {ErrorCode::FileRead, "invalid DOCX relationships: duplicate Id " + *id};
        const std::string* mode = attr_local(event, "TargetMode");
        relationships.emplace(*id, Relationship{*type, *target,
                                                mode != nullptr && ascii_lower(*mode) == "external"});
        return ok_error();
    });
}

std::string relationships_part(const std::string& part) {
    const std::size_t slash = part.find_last_of('/');
    const std::string directory = slash == std::string::npos ? std::string() : part.substr(0, slash + 1);
    const std::string basename = slash == std::string::npos ? part : part.substr(slash + 1);
    return directory + "_rels/" + basename + ".rels";
}

Error content_types(std::string_view xml,
                    runtime::CancellationToken cancellation,
                    std::unordered_map<std::string, std::string>& overrides) {
    return detail::scan_xml(xml, {}, cancellation, [&](const XmlEvent& event) -> Error {
        if (event.kind != XmlEvent::Kind::Start || event.name.local != "Override") return ok_error();
        const std::string* part = attr_local(event, "PartName");
        const std::string* type = attr_local(event, "ContentType");
        if (part == nullptr || type == nullptr || part->empty() || part->front() != '/')
            return {ErrorCode::FileRead, "invalid DOCX content types: malformed Override"};
        const std::string name = part->substr(1);
        if (!detail::safe_part_name(name))
            return {ErrorCode::FileRead, "invalid DOCX content types: unsafe part name"};
        if (!overrides.emplace(name, *type).second)
            return {ErrorCode::FileRead,
                    "invalid DOCX content types: duplicate Override for " + name};
        return ok_error();
    });
}

struct Paragraph {
    ParagraphProperties properties;
    std::vector<markdown::Run> runs;
};

struct Cell {
    std::vector<Paragraph> paragraphs;
    int column_span = 1;
    enum class VerticalMerge { None, Restart, Continue } vertical = VerticalMerge::None;
};

struct Table {
    std::vector<std::vector<Cell>> rows;
};

struct Block {
    enum class Kind { Paragraph, Table } kind = Kind::Paragraph;
    Paragraph paragraph;
    Table table;
};

unsigned effective_run_style(const RunProperties& properties) {
    unsigned style = 0;
    if (properties.bold.set && properties.bold.value) style |= static_cast<unsigned>(markdown::RunStyle::Bold);
    if (properties.italic.set && properties.italic.value) style |= static_cast<unsigned>(markdown::RunStyle::Italic);
    if (properties.underline.set && properties.underline.value)
        style |= static_cast<unsigned>(markdown::RunStyle::Underline);
    if (properties.strike.set && properties.strike.value)
        style |= static_cast<unsigned>(markdown::RunStyle::Strike);
    return style;
}

std::string hyperlink_instruction(std::string instruction) {
    const std::string lower = ascii_lower(instruction);
    const std::size_t marker = lower.find("hyperlink");
    if (marker == std::string::npos) return {};
    const std::size_t local_marker = lower.find("\\l", marker + 9);
    std::size_t quote = instruction.find('"', marker + 9);
    if (quote == std::string::npos) return {};
    const std::size_t end = instruction.find('"', quote + 1);
    if (end == std::string::npos) return {};
    std::string target = instruction.substr(quote + 1, end - quote - 1);
    if (local_marker != std::string::npos && local_marker < quote) target.insert(target.begin(), '#');
    return target;
}

std::string paragraph_plain_text(const Paragraph& paragraph) {
    std::string out;
    for (const markdown::Run& run : paragraph.runs) {
        out += run.text;
        if (run.hard_break_after) out.push_back('\n');
    }
    return out;
}

std::string normalized_style_name(const Style* style);

class DocumentReader {
   public:
    DocumentReader(const Styles& styles,
                   const Relationships& relationships,
                   Diagnostics* diagnostics)
        : styles_(styles), relationships_(relationships), diagnostics_(diagnostics) {}

    Error parse(std::string_view xml,
                runtime::CancellationToken cancellation,
                std::vector<Block>& blocks) {
        blocks_ = &blocks;
        Error error = detail::scan_xml(xml, {}, cancellation, [&](const XmlEvent& event) {
            if (event.kind == XmlEvent::Kind::Start) {
                if (xml_depth_ == 0 && !is_word(event, "document"))
                    return Error{ErrorCode::FileRead,
                                 "invalid DOCX main part: root is not w:document"};
                ++xml_depth_;
                if (is_word(event, "body")) body_seen_ = true;
                return start(event);
            }
            if (event.kind == XmlEvent::Kind::End) {
                Error result = end(event);
                if (xml_depth_ > 0) --xml_depth_;
                return result;
            }
            return text(event.text);
        });
        if (!error.ok()) return error;
        if (!body_seen_)
            return {ErrorCode::FileRead, "invalid DOCX main part: w:body is missing"};
        return ok_error();
    }

   private:
    Error start(const XmlEvent& event) {
        if (skip_depth_ > 0) {
            ++skip_depth_;
            return ok_error();
        }
        if (is_word(event, "del") || is_word(event, "moveFrom") ||
            (event.name.uri == kMarkupCompatibility && event.name.local == "Fallback")) {
            skip_depth_ = 1;
            return ok_error();
        }
        if (image_depth_ > 0) {
            if (event.name.local == "docPr") {
                const std::string* alt = attr_local(event, "descr");
                if (alt == nullptr || alt->empty()) alt = attr_local(event, "title");
                if (alt == nullptr || alt->empty()) alt = attr_local(event, "name");
                if (alt != nullptr) image_alt_ = *alt;
            }
            ++image_depth_;
            return ok_error();
        }
        if (is_word(event, "drawing") || is_word(event, "pict")) {
            image_depth_ = 1;
            image_alt_.clear();
            return ok_error();
        }
        if (is_word(event, "tbl")) {
            tables_.push_back(Table{});
        } else if (is_word(event, "tr") && !tables_.empty()) {
            tables_.back().rows.emplace_back();
        } else if (is_word(event, "tc") && !tables_.empty()) {
            if (tables_.back().rows.empty()) tables_.back().rows.emplace_back();
            tables_.back().rows.back().emplace_back();
        } else if (is_word(event, "gridSpan") && !tables_.empty() && !paragraph_active_) {
            if (Cell* cell = current_cell()) cell->column_span = std::max(1, integer_value(attr_local(event, "val"), 1));
        } else if (is_word(event, "vMerge") && !tables_.empty() && !paragraph_active_) {
            if (Cell* cell = current_cell()) {
                const std::string* value = attr_local(event, "val");
                cell->vertical = value == nullptr || value->empty() || ascii_lower(*value) == "continue"
                                     ? Cell::VerticalMerge::Continue
                                     : Cell::VerticalMerge::Restart;
            }
        } else if (is_word(event, "p")) {
            paragraph_ = Paragraph{};
            paragraph_active_ = true;
        } else if (paragraph_active_ && is_word(event, "pPr")) {
            ++paragraph_properties_;
        } else if (paragraph_active_ && is_word(event, "r")) {
            run_ = markdown::Run{};
            direct_run_ = RunProperties{};
            run_active_ = true;
        } else if (run_active_ && is_word(event, "rPr")) {
            ++run_properties_;
        }
        if (paragraph_properties_ > 0) set_paragraph_property(event, paragraph_.properties);
        if (run_properties_ > 0) set_run_property(event, direct_run_);

        if (run_active_ && is_word(event, "t")) ++text_depth_;
        else if (run_active_ && is_word(event, "tab")) run_.text.push_back('\t');
        else if (run_active_ && (is_word(event, "br") || is_word(event, "cr"))) {
            if (!run_.text.empty()) {
                const RunProperties continued = direct_run_;
                run_.hard_break_after = true;
                finish_run();
                run_active_ = true;
                direct_run_ = continued;
            } else if (!paragraph_.runs.empty()) {
                paragraph_.runs.back().hard_break_after = true;
            }
        } else if (run_active_ && is_word(event, "noBreakHyphen")) {
            run_.text.append("\xE2\x80\x91", 3);  // U+2011 NON-BREAKING HYPHEN
        } else if (run_active_ && is_word(event, "instrText")) {
            ++instruction_depth_;
        } else if (is_word(event, "hyperlink")) {
            link_stack_.push_back(explicit_link_);
            explicit_link_.clear();
            if (const std::string* id = relationship_attribute(event, "id")) {
                const auto found = relationships_.find(*id);
                if (found != relationships_.end() && found->second.external)
                    explicit_link_ = found->second.target;
            }
            if (const std::string* anchor = attr_local(event, "anchor")) explicit_link_ = "#" + *anchor;
        } else if (is_word(event, "fldSimple")) {
            field_stack_.push_back(field_link_);
            field_link_ = hyperlink_instruction(attr_local(event, "instr") == nullptr
                                                    ? std::string() : *attr_local(event, "instr"));
        } else if (is_word(event, "fldChar")) {
            const std::string type = ascii_lower(attr_local(event, "fldCharType") == nullptr
                                                     ? std::string() : *attr_local(event, "fldCharType"));
            if (type == "begin") {
                complex_field_ = true;
                field_separated_ = false;
                field_instruction_.clear();
                field_link_.clear();
            } else if (type == "separate" && complex_field_) {
                field_separated_ = true;
                field_link_ = hyperlink_instruction(field_instruction_);
            } else if (type == "end") {
                complex_field_ = false;
                field_separated_ = false;
                field_instruction_.clear();
                field_link_.clear();
            }
        }
        return ok_error();
    }

    Error end(const XmlEvent& event) {
        if (skip_depth_ > 0) {
            --skip_depth_;
            return ok_error();
        }
        if (image_depth_ > 0) {
            --image_depth_;
            if (image_depth_ == 0 && run_active_) {
                const RunProperties continued = direct_run_;
                if (!run_.text.empty()) finish_run();
                markdown::Run image;
                image.image_placeholder = true;
                image.image_alt = image_alt_;
                image.text = image_alt_.empty() ? "[image omitted]" : "[image omitted: " + image_alt_ + "]";
                paragraph_.runs.push_back(std::move(image));
                // A drawing can occur between two text nodes in one w:r. Keep
                // accepting the remainder of that run after emitting the
                // placeholder for the drawing itself.
                run_active_ = true;
                direct_run_ = continued;
                if (diagnostics_ != nullptr) ++diagnostics_->omitted_images;
                diagnostic(diagnostics_, "embedded images were replaced with visible Markdown placeholders");
            }
            return ok_error();
        }
        if (run_active_ && is_word(event, "t") && text_depth_ > 0) --text_depth_;
        else if (run_active_ && is_word(event, "instrText") && instruction_depth_ > 0) --instruction_depth_;
        else if (run_active_ && is_word(event, "rPr") && run_properties_ > 0) --run_properties_;
        else if (paragraph_active_ && is_word(event, "pPr") && paragraph_properties_ > 0)
            --paragraph_properties_;
        else if (is_word(event, "r") && run_active_) finish_run();
        else if (is_word(event, "p") && paragraph_active_) finish_paragraph();
        else if (is_word(event, "tbl") && !tables_.empty()) finish_table();
        else if (is_word(event, "hyperlink")) {
            explicit_link_ = link_stack_.empty() ? std::string() : link_stack_.back();
            if (!link_stack_.empty()) link_stack_.pop_back();
        } else if (is_word(event, "fldSimple")) {
            field_link_ = field_stack_.empty() ? std::string() : field_stack_.back();
            if (!field_stack_.empty()) field_stack_.pop_back();
        }
        return ok_error();
    }

    Error text(const std::string& value) {
        if (skip_depth_ > 0 || image_depth_ > 0) return ok_error();
        if (instruction_depth_ > 0 && complex_field_) field_instruction_ += value;
        else if (text_depth_ > 0 && run_active_) {
            run_.text += value;
        }
        return ok_error();
    }

    const std::string* relationship_attribute(const XmlEvent& event, std::string_view local) const {
        const std::string* value = detail::attribute(event, kOfficeRelationships, local);
        if (value == nullptr) value = detail::attribute(event, kOfficeRelationshipsStrict, local);
        return value;
    }

    Cell* current_cell() {
        if (tables_.empty() || tables_.back().rows.empty() || tables_.back().rows.back().empty()) return nullptr;
        return &tables_.back().rows.back().back();
    }

    void finish_run() {
        RunProperties effective = styles_.default_run;
        std::string paragraph_style = paragraph_.properties.style_set
                                          ? paragraph_.properties.style_id
                                          : styles_.default_paragraph_style;
        const auto paragraph_found = styles_.values.find(paragraph_style);
        const std::string paragraph_semantic = paragraph_found == styles_.values.end()
                                                   ? std::string()
                                                   : normalized_style_name(&paragraph_found->second);
        const bool semantic_paragraph = paragraph_semantic.rfind("heading", 0) == 0 ||
                                        paragraph_semantic == "quote" ||
                                        paragraph_semantic == "intensequote" ||
                                        paragraph_semantic == "code";
        if (paragraph_found != styles_.values.end() && !semantic_paragraph)
            merge(effective, paragraph_found->second.run);
        std::string run_semantic;
        if (!direct_run_.style_id.empty()) {
            const auto run_found = styles_.values.find(direct_run_.style_id);
            run_semantic = run_found == styles_.values.end()
                               ? ascii_lower(direct_run_.style_id)
                               : normalized_style_name(&run_found->second);
            if (run_found != styles_.values.end() && run_semantic != "hyperlink" &&
                run_semantic != "code" && run_semantic != "codechar")
                merge(effective, run_found->second.run);
        }
        merge(effective, direct_run_);
        run_.style = effective_run_style(effective);
        if (paragraph_semantic == "code" || run_semantic == "code" || run_semantic == "codechar")
            run_.style |= static_cast<unsigned>(markdown::RunStyle::Code);
        if (!explicit_link_.empty()) run_.url = explicit_link_;
        else if (!field_link_.empty() && (!complex_field_ || field_separated_)) run_.url = field_link_;
        const bool hidden_instruction = complex_field_ && !field_separated_;
        if (!hidden_instruction && (!run_.text.empty() || run_.hard_break_after))
            paragraph_.runs.push_back(std::move(run_));
        run_ = markdown::Run{};
        direct_run_ = RunProperties{};
        run_active_ = false;
        run_properties_ = 0;
    }

    void finish_paragraph() {
        if (run_active_) finish_run();
        if (Cell* cell = current_cell()) {
            cell->paragraphs.push_back(std::move(paragraph_));
        } else {
            Block block;
            block.kind = Block::Kind::Paragraph;
            block.paragraph = std::move(paragraph_);
            blocks_->push_back(std::move(block));
        }
        paragraph_ = Paragraph{};
        paragraph_active_ = false;
        paragraph_properties_ = 0;
    }

    void finish_table() {
        Table table = std::move(tables_.back());
        tables_.pop_back();
        if (!tables_.empty()) {
            Paragraph flattened;
            bool first = true;
            for (const auto& row : table.rows) {
                for (const Cell& cell : row) {
                    for (const Paragraph& paragraph : cell.paragraphs) {
                        if (!first) {
                            markdown::Run separator;
                            separator.text = " ";
                            flattened.runs.push_back(std::move(separator));
                        }
                        markdown::Run run;
                        run.text = paragraph_plain_text(paragraph);
                        flattened.runs.push_back(std::move(run));
                        first = false;
                    }
                }
            }
            if (Cell* cell = current_cell()) cell->paragraphs.push_back(std::move(flattened));
            return;
        }
        Block block;
        block.kind = Block::Kind::Table;
        block.table = std::move(table);
        blocks_->push_back(std::move(block));
    }

    const Styles& styles_;
    const Relationships& relationships_;
    Diagnostics* diagnostics_ = nullptr;
    std::vector<Block>* blocks_ = nullptr;
    std::vector<Table> tables_;
    Paragraph paragraph_;
    markdown::Run run_;
    RunProperties direct_run_;
    bool paragraph_active_ = false;
    bool run_active_ = false;
    int paragraph_properties_ = 0;
    int run_properties_ = 0;
    int text_depth_ = 0;
    int instruction_depth_ = 0;
    int skip_depth_ = 0;
    int image_depth_ = 0;
    int xml_depth_ = 0;
    bool body_seen_ = false;
    std::string image_alt_;
    std::string explicit_link_;
    std::vector<std::string> link_stack_;
    std::string field_link_;
    std::vector<std::string> field_stack_;
    bool complex_field_ = false;
    bool field_separated_ = false;
    std::string field_instruction_;
};

std::string markdown_escape(std::string_view text, bool table_cell = false) {
    std::string out;
    out.reserve(text.size());
    for (char ch : text) {
        if (ch == '\\' || ch == '`' || ch == '*' || ch == '_' || ch == '[' || ch == ']' ||
            ch == '~' || ch == '+') out.push_back('\\');
        if (table_cell && ch == '|') out.push_back('\\');
        if (ch != '\r') out.push_back(ch);
    }
    return out;
}

std::string markdown_link_target(std::string target) {
    std::string out;
    out.reserve(target.size());
    for (unsigned char ch : target) {
        if (ch == ' ') out += "%20";
        else if (ch == '(') out += "%28";
        else if (ch == ')') out += "%29";
        else out.push_back(static_cast<char>(ch));
    }
    return out;
}

std::string markdown_runs(const std::vector<markdown::Run>& runs, bool table_cell = false) {
    std::string out;
    for (const markdown::Run& run : runs) {
        if (run.image_placeholder) {
            out += run.image_alt.empty() ? "[image omitted]"
                                         : "[image omitted: " + markdown_escape(run.image_alt, table_cell) + "]";
            if (run.hard_break_after) out += table_cell ? "<br>" : "  \n";
            continue;
        }
        if ((run.text == "[image omitted]" ||
             (run.text.rfind("[image omitted: ", 0) == 0 && run.text.back() == ']')) &&
            run.url.empty() && run.style == 0) {
            out += run.text;
            if (run.hard_break_after) out += table_cell ? "<br>" : "  \n";
            continue;
        }
        std::string body = markdown_escape(run.text, table_cell);
        const unsigned code = static_cast<unsigned>(markdown::RunStyle::Code);
        const unsigned bold = static_cast<unsigned>(markdown::RunStyle::Bold);
        const unsigned italic = static_cast<unsigned>(markdown::RunStyle::Italic);
        const unsigned strike = static_cast<unsigned>(markdown::RunStyle::Strike);
        const unsigned underline = static_cast<unsigned>(markdown::RunStyle::Underline);
        if ((run.style & code) != 0) {
            const std::string marker = body.find('`') == std::string::npos ? "`" : "``";
            body = marker + body + marker;
        } else {
            if ((run.style & underline) != 0) body = "++" + body + "++";
            if ((run.style & strike) != 0) body = "~~" + body + "~~";
            if ((run.style & italic) != 0) body = "*" + body + "*";
            if ((run.style & bold) != 0) body = "**" + body + "**";
        }
        if (!run.url.empty()) body = "[" + body + "](" + markdown_link_target(run.url) + ")";
        out += body;
        if (run.hard_break_after) out += table_cell ? "<br>" : "  \n";
    }
    return out;
}

ParagraphProperties effective_paragraph_properties(const Paragraph& paragraph, const Styles& styles) {
    ParagraphProperties effective = styles.default_paragraph;
    const std::string id = paragraph.properties.style_set
                               ? paragraph.properties.style_id : styles.default_paragraph_style;
    const auto style = styles.values.find(id);
    if (style != styles.values.end()) merge(effective, style->second.paragraph);
    merge(effective, paragraph.properties);
    return effective;
}

const Style* paragraph_style(const Paragraph& paragraph, const Styles& styles) {
    const std::string id = paragraph.properties.style_set
                               ? paragraph.properties.style_id : styles.default_paragraph_style;
    const auto found = styles.values.find(id);
    return found == styles.values.end() ? nullptr : &found->second;
}

std::string normalized_style_name(const Style* style) {
    if (style == nullptr) return {};
    std::string value = style->name.empty() ? style->id : style->name;
    value = ascii_lower(std::move(value));
    value.erase(std::remove_if(value.begin(), value.end(), [](char ch) {
                    return ch == ' ' || ch == '-' || ch == '_';
                }), value.end());
    return value;
}

int heading_level(const Paragraph& paragraph,
                  const ParagraphProperties& properties,
                  const Styles& styles) {
    const std::string style = normalized_style_name(paragraph_style(paragraph, styles));
    if (style.rfind("heading", 0) == 0 && style.size() == 8 &&
        style[7] >= '1' && style[7] <= '6') return style[7] - '0';
    if (properties.outline_set && properties.outline >= 0 && properties.outline < 6)
        return properties.outline + 1;
    return 0;
}

const NumberLevel* number_level(const Numbering& numbering, int num_id, int level) {
    const auto instance = numbering.instances.find(num_id);
    if (instance == numbering.instances.end()) return nullptr;
    const auto abstract = numbering.abstracts.find(instance->second);
    if (abstract == numbering.abstracts.end()) return nullptr;
    const auto found = abstract->second.levels.find(level);
    return found == abstract->second.levels.end() ? nullptr : &found->second;
}

bool unordered_format(std::string format) {
    format = ascii_lower(std::move(format));
    return format == "bullet" || format == "none";
}

std::string cell_markdown(const Cell& cell) {
    std::string out;
    for (std::size_t i = 0; i < cell.paragraphs.size(); ++i) {
        if (i != 0) out += "<br>";
        out += markdown_runs(cell.paragraphs[i].runs, true);
    }
    return out;
}

void emit_table(const Table& table, std::string& markdown, Diagnostics* diagnostics) {
    if (table.rows.empty()) return;
    std::size_t columns = 0;
    bool merged = false;
    for (const auto& row : table.rows) {
        std::size_t width = 0;
        for (const Cell& cell : row) {
            width += static_cast<std::size_t>(std::max(1, cell.column_span));
            if (cell.column_span > 1 || cell.vertical != Cell::VerticalMerge::None) merged = true;
        }
        columns = std::max(columns, width);
    }
    if (columns == 0) return;
    std::vector<std::vector<std::string>> rows;
    rows.reserve(table.rows.size());
    for (const auto& source : table.rows) {
        std::vector<std::string> row;
        for (const Cell& cell : source) {
            const bool covered = cell.vertical == Cell::VerticalMerge::Continue;
            row.push_back(covered ? std::string() : cell_markdown(cell));
            for (int span = 1; span < std::max(1, cell.column_span); ++span) row.emplace_back();
        }
        row.resize(columns);
        rows.push_back(std::move(row));
    }
    if (merged) {
        if (diagnostics != nullptr) ++diagnostics->flattened_merged_tables;
        diagnostic(diagnostics, "merged table cells were flattened into a rectangular Markdown table");
    }
    auto emit_row = [&](const std::vector<std::string>& row) {
        markdown += '|';
        for (const std::string& cell : row) markdown += " " + cell + " |";
        markdown.push_back('\n');
    };
    emit_row(rows.front());
    markdown += '|';
    for (std::size_t column = 0; column < columns; ++column) markdown += " --- |";
    markdown.push_back('\n');
    for (std::size_t row = 1; row < rows.size(); ++row) emit_row(rows[row]);
    markdown.push_back('\n');
}

Error emit_markdown(const std::vector<Block>& blocks,
                    const Styles& styles,
                    const Numbering& numbering,
                    runtime::CancellationToken cancellation,
                    Diagnostics* diagnostics,
                    std::string& markdown) {
    markdown.clear();
    struct CounterState {
        std::array<long long, 9> values{};
        std::array<bool, 9> initialized{};
    };
    std::unordered_map<int, CounterState> counters;
    bool previous_list = false;
    try {
        for (std::size_t block_index = 0; block_index < blocks.size(); ++block_index) {
            if ((block_index & 0xffU) == 0 && cancellation.cancelled())
                return {ErrorCode::Cancelled, "DOCX Markdown emission cancelled"};
            const Block& block = blocks[block_index];
            if (block.kind == Block::Kind::Table) {
                if (previous_list) markdown.push_back('\n');
                previous_list = false;
                emit_table(block.table, markdown, diagnostics);
                continue;
            }
            const Paragraph& paragraph = block.paragraph;
            const ParagraphProperties properties = effective_paragraph_properties(paragraph, styles);
            const Style* style_ptr = paragraph_style(paragraph, styles);
            const std::string style_name = normalized_style_name(style_ptr);
            std::string content = markdown_runs(paragraph.runs);
            const int heading = heading_level(paragraph, properties, styles);
            if (heading > 0) {
                if (previous_list) markdown.push_back('\n');
                previous_list = false;
                markdown += std::string(static_cast<std::size_t>(heading), '#') + " " + content + "\n\n";
                continue;
            }
            if (properties.num_set && properties.num_id > 0) {
                const int level = std::max(0, std::min(8, properties.level_set ? properties.level : 0));
                const NumberLevel* definition = number_level(numbering, properties.num_id, level);
                if (definition != nullptr) {
                    auto& state = counters[properties.num_id];
                    int start = definition->start;
                    const auto override = numbering.start_overrides.find({properties.num_id, level});
                    if (override != numbering.start_overrides.end()) start = override->second;
                    if (!state.initialized[static_cast<std::size_t>(level)]) {
                        state.values[static_cast<std::size_t>(level)] = start;
                        state.initialized[static_cast<std::size_t>(level)] = true;
                    }
                    else
                        ++state.values[static_cast<std::size_t>(level)];
                    for (int deeper = level + 1; deeper < 9; ++deeper) {
                        const NumberLevel* deeper_definition =
                            number_level(numbering, properties.num_id, deeper);
                        const int restart = deeper_definition == nullptr ? -1
                                                                         : deeper_definition->restart;
                        const bool reset = restart < 0 ? level == deeper - 1
                                                       : restart > 0 && level == restart - 1;
                        if (reset) state.initialized[static_cast<std::size_t>(deeper)] = false;
                    }
                    markdown += std::string(static_cast<std::size_t>(level) * 4U, ' ');
                    if (unordered_format(definition->format)) markdown += "- ";
                    else markdown += std::to_string(state.values[static_cast<std::size_t>(level)]) + ". ";
                    markdown += content + "\n";
                    previous_list = true;
                    continue;
                }
            }
            if (previous_list) markdown.push_back('\n');
            previous_list = false;
            if (style_name == "quote" || style_name == "intensequote") {
                std::vector<std::string> lines = split_lines_crlf(content);
                if (lines.empty()) lines.push_back({});
                for (const std::string& line : lines) markdown += "> " + line + "\n";
                markdown.push_back('\n');
            } else {
                markdown += content;
                markdown += "\n\n";
            }
        }
        while (!markdown.empty() && (markdown.back() == '\n' || markdown.back() == ' ')) markdown.pop_back();
        if (!markdown.empty()) markdown.push_back('\n');
    } catch (const std::bad_alloc&) {
        markdown.clear();
        return {ErrorCode::Internal, "not enough memory to emit DOCX Markdown"};
    } catch (const std::length_error&) {
        markdown.clear();
        return {ErrorCode::UnsupportedFeature, "converted DOCX Markdown is too large"};
    }
    return ok_error();
}

Error read_file_bounded(const std::string& path,
                        const ReadOptions& options,
                        std::string& bytes) {
    bytes.clear();
    const std::string resolved = expand_user_path(path);
    std::ifstream file(std::filesystem::u8path(resolved), std::ios::binary);
    if (!file) return {ErrorCode::FileRead, "could not open DOCX for reading: " + resolved};
    std::array<char, 65536> buffer{};
    try {
        while (file) {
            if (options.cancellation.cancelled())
                return {ErrorCode::Cancelled, "DOCX read cancelled: " + resolved};
            file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const std::streamsize count = file.gcount();
            if (count <= 0) break;
            const std::size_t chunk = static_cast<std::size_t>(count);
            if (bytes.size() > options.max_bytes || chunk > options.max_bytes - bytes.size()) {
                bytes.clear();
                return {ErrorCode::UnsupportedFeature, "DOCX exceeds input size limit of " +
                                                           std::to_string(options.max_bytes) + " bytes: " + resolved};
            }
            bytes.append(buffer.data(), chunk);
        }
    } catch (const std::bad_alloc&) {
        bytes.clear();
        return {ErrorCode::Internal, "not enough memory to read DOCX: " + resolved};
    }
    if (file.bad()) return {ErrorCode::FileRead, "could not read DOCX: " + resolved};
    return ok_error();
}

Error extract_docx(std::string_view bytes,
                   const ReadOptions& options,
                   std::string& markdown,
                   Diagnostics* diagnostics) {
    markdown.clear();
    detail::ZipArchive archive;
    detail::ZipLimits limits;
    limits.max_member_bytes = 64U * 1024U * 1024U;
    Error error = archive.open(bytes, limits, options.cancellation);
    if (!error.ok()) return error;
    if (!archive.contains("[Content_Types].xml"))
        return {ErrorCode::FileRead, "invalid DOCX package: [Content_Types].xml is missing"};
    if (!archive.contains("_rels/.rels"))
        return {ErrorCode::FileRead, "invalid DOCX package: package relationships are missing"};

    std::string xml;
    error = archive.read("[Content_Types].xml", xml);
    if (!error.ok()) return error;
    std::unordered_map<std::string, std::string> overrides;
    error = content_types(xml, options.cancellation, overrides);
    if (!error.ok()) return error;
    error = archive.read("_rels/.rels", xml);
    if (!error.ok()) return error;
    Relationships package_relationships;
    error = parse_relationships(xml, options.cancellation, package_relationships);
    if (!error.ok()) return error;
    std::string main_part;
    for (const auto& item : package_relationships) {
        if (type_suffix(item.second.type, "/officeDocument")) {
            if (!main_part.empty())
                return {ErrorCode::FileRead,
                        "invalid DOCX package: multiple officeDocument relationships"};
            if (item.second.external)
                return {ErrorCode::FileRead, "invalid DOCX package: main document relationship is external"};
            error = detail::resolve_part_name({}, item.second.target, main_part);
            if (!error.ok()) return error;
        }
    }
    if (main_part.empty())
        return {ErrorCode::FileRead, "invalid DOCX package: officeDocument relationship is missing"};
    const auto main_type = overrides.find(main_part);
    if (main_type == overrides.end() || main_type->second != kMainContentType)
        return {ErrorCode::FileRead, "invalid DOCX package: main document content type is missing or unsupported"};
    if (!archive.contains(main_part))
        return {ErrorCode::FileRead, "invalid DOCX package: main document part is missing: " + main_part};

    Relationships document_relationships;
    const std::string rels_part = relationships_part(main_part);
    if (archive.contains(rels_part)) {
        error = archive.read(rels_part, xml);
        if (!error.ok()) return error;
        error = parse_relationships(xml, options.cancellation, document_relationships);
        if (!error.ok()) return error;
    }
    std::string styles_part;
    std::string numbering_part;
    for (const auto& item : document_relationships) {
        const Relationship& relationship = item.second;
        if (!relationship.external && type_suffix(relationship.type, "/styles")) {
            if (!styles_part.empty())
                return {ErrorCode::FileRead,
                        "invalid DOCX package: multiple styles relationships"};
            error = detail::resolve_part_name(main_part, relationship.target, styles_part);
        } else if (!relationship.external && type_suffix(relationship.type, "/numbering")) {
            if (!numbering_part.empty())
                return {ErrorCode::FileRead,
                        "invalid DOCX package: multiple numbering relationships"};
            error = detail::resolve_part_name(main_part, relationship.target, numbering_part);
        } else if (type_suffix(relationship.type, "/header") || type_suffix(relationship.type, "/footer") ||
                 type_suffix(relationship.type, "/footnotes") || type_suffix(relationship.type, "/endnotes") ||
                 type_suffix(relationship.type, "/comments")) {
            if (diagnostics != nullptr) ++diagnostics->unsupported_story_parts;
            diagnostic(diagnostics, "headers, footers, notes, or comments were omitted from DOCX conversion");
        }
        if (!error.ok()) return error;
    }
    Styles styles;
    if (!styles_part.empty()) {
        if (!archive.contains(styles_part))
            return {ErrorCode::FileRead, "invalid DOCX package: related styles part is missing"};
        error = archive.read(styles_part, xml);
        if (!error.ok()) return error;
        error = parse_styles(xml, options.cancellation, styles);
        if (!error.ok()) return error;
    }
    Numbering numbering;
    if (!numbering_part.empty()) {
        if (!archive.contains(numbering_part))
            return {ErrorCode::FileRead, "invalid DOCX package: related numbering part is missing"};
        error = archive.read(numbering_part, xml);
        if (!error.ok()) return error;
        error = parse_numbering(xml, options.cancellation, numbering);
        if (!error.ok()) return error;
    }
    error = archive.read(main_part, xml);
    if (!error.ok()) return error;
    std::vector<Block> blocks;
    DocumentReader reader(styles, document_relationships, diagnostics);
    error = reader.parse(xml, options.cancellation, blocks);
    if (!error.ok()) return error;
    return emit_markdown(blocks, styles, numbering, options.cancellation, diagnostics, markdown);
}

std::string xml_escape(std::string_view text, bool attribute = false) {
    std::string out;
    out.reserve(text.size());
    for (char ch : text) {
        switch (ch) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += attribute ? "&quot;" : "\""; break;
            case '\'': out += attribute ? "&apos;" : "'"; break;
            default: out.push_back(ch); break;
        }
    }
    return out;
}

bool has_rtl(std::string_view text) {
    for (std::size_t pos = 0; pos < text.size();) {
        const unsigned char first = static_cast<unsigned char>(text[pos++]);
        std::uint32_t cp = first;
        if ((first & 0xe0U) == 0xc0U && pos < text.size()) {
            cp = (first & 0x1fU) << 6U;
            cp |= static_cast<unsigned char>(text[pos++]) & 0x3fU;
        } else if ((first & 0xf0U) == 0xe0U && pos + 1 < text.size()) {
            cp = (first & 0x0fU) << 12U;
            cp |= (static_cast<unsigned char>(text[pos++]) & 0x3fU) << 6U;
            cp |= static_cast<unsigned char>(text[pos++]) & 0x3fU;
        } else if ((first & 0xf8U) == 0xf0U && pos + 2 < text.size()) {
            cp = (first & 0x07U) << 18U;
            cp |= (static_cast<unsigned char>(text[pos++]) & 0x3fU) << 12U;
            cp |= (static_cast<unsigned char>(text[pos++]) & 0x3fU) << 6U;
            cp |= static_cast<unsigned char>(text[pos++]) & 0x3fU;
        }
        if ((cp >= 0x0590U && cp <= 0x08ffU) || (cp >= 0xfb1dU && cp <= 0xfdffU) ||
            (cp >= 0xfe70U && cp <= 0xfeffU)) return true;
    }
    return false;
}

struct ListInstance {
    int id = 0;
    bool ordered = false;
    int level = 0;
    int start = 1;
};

class DocumentWriter {
   public:
    DocumentWriter(const std::vector<markdown::Block>& blocks,
                   runtime::CancellationToken cancellation)
        : blocks_(blocks), cancellation_(cancellation), list_ids_(blocks.size(), 0) {
        assign_lists();
    }

    Error document(std::string& xml) {
        xml.clear();
        try {
            xml = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
                  "<w:document xmlns:w=\"" + std::string(kWordTransitional) +
                  "\" xmlns:r=\"" + std::string(kOfficeRelationships) + "\"><w:body>";
            for (std::size_t i = 0; i < blocks_.size(); ++i) {
                if ((i & 0xffU) == 0 && cancellation_.cancelled())
                    return {ErrorCode::Cancelled, "DOCX serialization cancelled"};
                write_block(blocks_[i], i, xml, {});
            }
            xml += "<w:sectPr><w:pgSz w:w=\"12240\" w:h=\"15840\"/>"
                   "<w:pgMar w:top=\"1440\" w:right=\"1440\" w:bottom=\"1440\" w:left=\"1440\" "
                   "w:header=\"720\" w:footer=\"720\" w:gutter=\"0\"/></w:sectPr>"
                   "</w:body></w:document>";
        } catch (const std::bad_alloc&) {
            return {ErrorCode::Internal, "not enough memory to serialize DOCX document XML"};
        } catch (const std::length_error&) {
            return {ErrorCode::UnsupportedFeature, "DOCX document XML is too large"};
        }
        return ok_error();
    }

    bool has_lists() const { return !lists_.empty(); }
    const std::vector<ListInstance>& lists() const { return lists_; }
    const std::map<std::string, std::string>& hyperlinks() const { return hyperlinks_; }

   private:
    void assign_lists() {
        bool active = false;
        bool active_ordered = false;
        int active_id = 0;
        std::array<int, 9> expected{};
        for (std::size_t i = 0; i < blocks_.size(); ++i) {
            const markdown::Block& block = blocks_[i];
            if (block.kind != markdown::BlockKind::ListItem) {
                active = false;
                expected.fill(0);
                continue;
            }
            const int level = std::max(0, std::min(8, block.indent / 4));
            bool restart = !active || active_ordered != block.ordered;
            if (!restart && block.ordered && expected[static_cast<std::size_t>(level)] != 0 &&
                block.list_index != expected[static_cast<std::size_t>(level)]) restart = true;
            if (restart) {
                active_id = static_cast<int>(lists_.size()) + 1;
                lists_.push_back({active_id, block.ordered, level,
                                  block.ordered ? std::max(0, block.list_index) : 1});
                active = true;
                active_ordered = block.ordered;
                expected.fill(0);
            }
            list_ids_[i] = active_id;
            if (block.ordered) expected[static_cast<std::size_t>(level)] = block.list_index + 1;
            for (int deeper = level + 1; deeper < 9; ++deeper)
                expected[static_cast<std::size_t>(deeper)] = 0;
        }
    }

    std::string relationship_id(const std::string& url) {
        const auto found = hyperlinks_.find(url);
        if (found != hyperlinks_.end()) return found->second;
        const int base = has_lists() ? 4 : 3;
        const std::string id = "rId" + std::to_string(base + hyperlinks_.size());
        hyperlinks_.emplace(url, id);
        return id;
    }

    void write_run_text(const std::string& text, std::string& xml) {
        std::size_t start = 0;
        for (std::size_t pos = 0; pos <= text.size(); ++pos) {
            if (pos != text.size() && text[pos] != '\t' && text[pos] != '\n') continue;
            if (pos > start) {
                xml += "<w:t xml:space=\"preserve\">" + xml_escape(
                    std::string_view(text).substr(start, pos - start)) + "</w:t>";
            }
            if (pos != text.size()) xml += text[pos] == '\t' ? "<w:tab/>" : "<w:br/>";
            start = pos + 1;
        }
    }

    void write_run(const markdown::Run& run, std::string& xml) {
        const bool internal = !run.url.empty() && run.url.front() == '#';
        if (!run.url.empty()) {
            if (internal) xml += "<w:hyperlink w:anchor=\"" + xml_escape(run.url.substr(1), true) + "\">";
            else xml += "<w:hyperlink r:id=\"" + relationship_id(run.url) + "\">";
        }
        xml += "<w:r><w:rPr>";
        if (!run.url.empty()) xml += "<w:rStyle w:val=\"Hyperlink\"/>";
        if ((run.style & static_cast<unsigned>(markdown::RunStyle::Code)) != 0)
            xml += "<w:rStyle w:val=\"CodeChar\"/>";
        if ((run.style & static_cast<unsigned>(markdown::RunStyle::Bold)) != 0) xml += "<w:b/>";
        if ((run.style & static_cast<unsigned>(markdown::RunStyle::Italic)) != 0) xml += "<w:i/>";
        if ((run.style & static_cast<unsigned>(markdown::RunStyle::Strike)) != 0) xml += "<w:strike/>";
        if ((run.style & static_cast<unsigned>(markdown::RunStyle::Underline)) != 0) xml += "<w:u w:val=\"single\"/>";
        if (has_rtl(run.text)) xml += "<w:rtl/>";
        xml += "</w:rPr>";
        write_run_text(run.text, xml);
        if (run.hard_break_after) xml += "<w:br/>";
        xml += "</w:r>";
        if (!run.url.empty()) xml += "</w:hyperlink>";
    }

    void write_paragraph(const std::vector<markdown::Run>& runs,
                         std::string& xml,
                         const std::string& style,
                         int list_id = 0,
                         int list_level = 0,
                         bool rule = false) {
        xml += "<w:p>";
        if (!style.empty() || list_id > 0 || rule || has_rtl_runs(runs)) {
            xml += "<w:pPr>";
            if (!style.empty()) xml += "<w:pStyle w:val=\"" + style + "\"/>";
            if (list_id > 0) {
                xml += "<w:numPr><w:ilvl w:val=\"" + std::to_string(list_level) +
                       "\"/><w:numId w:val=\"" + std::to_string(list_id) + "\"/></w:numPr>";
            }
            if (rule) xml += "<w:pBdr><w:bottom w:val=\"single\" w:sz=\"4\" w:space=\"1\" w:color=\"808080\"/></w:pBdr>";
            if (has_rtl_runs(runs)) xml += "<w:bidi/>";
            xml += "</w:pPr>";
        }
        for (const markdown::Run& run : runs) write_run(run, xml);
        xml += "</w:p>";
    }

    bool has_rtl_runs(const std::vector<markdown::Run>& runs) const {
        for (const markdown::Run& run : runs) if (has_rtl(run.text)) return true;
        return false;
    }

    void write_table(const markdown::Block& block, std::string& xml) {
        std::size_t columns = 0;
        for (const auto& row : block.table_cells) columns = std::max(columns, row.size());
        if (columns == 0) return;
        xml += "<w:tbl><w:tblPr><w:tblStyle w:val=\"TableGrid\"/>"
               "<w:tblW w:w=\"0\" w:type=\"auto\"/></w:tblPr><w:tblGrid>";
        for (std::size_t column = 0; column < columns; ++column) xml += "<w:gridCol w:w=\"2400\"/>";
        xml += "</w:tblGrid>";
        for (const auto& row : block.table_cells) {
            xml += "<w:tr>";
            for (std::size_t column = 0; column < columns; ++column) {
                xml += "<w:tc><w:tcPr><w:tcW w:w=\"2400\" w:type=\"dxa\"/></w:tcPr>";
                static const std::vector<markdown::Run> empty;
                write_paragraph(column < row.size() ? row[column] : empty, xml, {});
                xml += "</w:tc>";
            }
            xml += "</w:tr>";
        }
        xml += "</w:tbl>";
    }

    void write_block(const markdown::Block& block,
                     std::size_t index,
                     std::string& xml,
                     const std::string& forced_style) {
        switch (block.kind) {
            case markdown::BlockKind::Heading:
                write_paragraph(block.runs, xml, "Heading" + std::to_string(
                    std::max(1, std::min(6, block.heading_level))));
                break;
            case markdown::BlockKind::ListItem:
                write_paragraph(block.runs, xml, "ListParagraph", list_ids_[index],
                                std::max(0, std::min(8, block.indent / 4)));
                break;
            case markdown::BlockKind::Quote:
                for (const markdown::Block& child : block.children)
                    write_block(child, 0, xml, "Quote");
                break;
            case markdown::BlockKind::Code: {
                markdown::Run run;
                run.text = block.text;
                run.style = static_cast<unsigned>(markdown::RunStyle::Code);
                write_paragraph({run}, xml, "Code");
                break;
            }
            case markdown::BlockKind::Table:
                write_table(block, xml);
                break;
            case markdown::BlockKind::Rule:
                write_paragraph({}, xml, {}, 0, 0, true);
                break;
            case markdown::BlockKind::Html:
            case markdown::BlockKind::Paragraph:
                write_paragraph(block.runs, xml, forced_style);
                break;
        }
    }

    const std::vector<markdown::Block>& blocks_;
    runtime::CancellationToken cancellation_;
    std::vector<int> list_ids_;
    std::vector<ListInstance> lists_;
    std::map<std::string, std::string> hyperlinks_;
};

std::string styles_xml() {
    std::string xml =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
        "<w:styles xmlns:w=\"" + std::string(kWordTransitional) + "\">"
        "<w:docDefaults><w:rPrDefault><w:rPr><w:rFonts w:ascii=\"Arial\" w:hAnsi=\"Arial\" "
        "w:eastAsia=\"Arial\" w:cs=\"Arial\"/><w:color w:val=\"000000\"/><w:sz w:val=\"22\"/>"
        "<w:szCs w:val=\"22\"/></w:rPr></w:rPrDefault><w:pPrDefault><w:pPr/></w:pPrDefault></w:docDefaults>"
        "<w:style w:type=\"paragraph\" w:default=\"1\" w:styleId=\"Normal\"><w:name w:val=\"Normal\"/>"
        "<w:qFormat/><w:rPr><w:rFonts w:ascii=\"Arial\" w:hAnsi=\"Arial\"/><w:color w:val=\"000000\"/>"
        "<w:sz w:val=\"22\"/></w:rPr></w:style>";
    const int sizes[] = {32, 28, 26, 24, 22, 22};
    for (int level = 1; level <= 6; ++level) {
        xml += "<w:style w:type=\"paragraph\" w:styleId=\"Heading" + std::to_string(level) +
               "\"><w:name w:val=\"heading " + std::to_string(level) +
               "\"/><w:basedOn w:val=\"Normal\"/><w:next w:val=\"Normal\"/><w:qFormat/>"
               "<w:pPr><w:keepNext/><w:keepLines/><w:outlineLvl w:val=\"" +
               std::to_string(level - 1) + "\"/></w:pPr><w:rPr><w:b/><w:sz w:val=\"" +
               std::to_string(sizes[level - 1]) + "\"/></w:rPr></w:style>";
    }
    xml +=
        "<w:style w:type=\"paragraph\" w:styleId=\"Quote\"><w:name w:val=\"Quote\"/>"
        "<w:basedOn w:val=\"Normal\"/><w:pPr><w:ind w:left=\"720\" w:right=\"720\"/></w:pPr>"
        "<w:rPr><w:i/></w:rPr></w:style>"
        "<w:style w:type=\"paragraph\" w:styleId=\"ListParagraph\"><w:name w:val=\"List Paragraph\"/>"
        "<w:basedOn w:val=\"Normal\"/></w:style>"
        "<w:style w:type=\"paragraph\" w:styleId=\"Code\"><w:name w:val=\"Code\"/>"
        "<w:basedOn w:val=\"Normal\"/><w:rPr><w:rFonts w:ascii=\"Courier New\" w:hAnsi=\"Courier New\"/>"
        "<w:sz w:val=\"20\"/></w:rPr></w:style>"
        "<w:style w:type=\"character\" w:styleId=\"CodeChar\"><w:name w:val=\"Code Char\"/>"
        "<w:rPr><w:rFonts w:ascii=\"Courier New\" w:hAnsi=\"Courier New\"/><w:sz w:val=\"20\"/></w:rPr></w:style>"
        "<w:style w:type=\"character\" w:styleId=\"Hyperlink\"><w:name w:val=\"Hyperlink\"/>"
        "<w:rPr><w:color w:val=\"0563C1\"/><w:u w:val=\"single\"/></w:rPr></w:style>"
        "<w:style w:type=\"table\" w:default=\"1\" w:styleId=\"TableGrid\"><w:name w:val=\"Table Grid\"/>"
        "<w:tblPr><w:tblBorders><w:top w:val=\"single\" w:sz=\"4\" w:color=\"auto\"/>"
        "<w:left w:val=\"single\" w:sz=\"4\" w:color=\"auto\"/><w:bottom w:val=\"single\" w:sz=\"4\" w:color=\"auto\"/>"
        "<w:right w:val=\"single\" w:sz=\"4\" w:color=\"auto\"/><w:insideH w:val=\"single\" w:sz=\"4\" w:color=\"auto\"/>"
        "<w:insideV w:val=\"single\" w:sz=\"4\" w:color=\"auto\"/></w:tblBorders></w:tblPr></w:style>"
        "</w:styles>";
    return xml;
}

std::string numbering_xml(const std::vector<ListInstance>& instances) {
    std::string xml = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
                      "<w:numbering xmlns:w=\"" + std::string(kWordTransitional) + "\">";
    auto abstract = [&](int id, bool ordered) {
        xml += "<w:abstractNum w:abstractNumId=\"" + std::to_string(id) +
               "\"><w:multiLevelType w:val=\"multilevel\"/>";
        for (int level = 0; level < 9; ++level) {
            xml += "<w:lvl w:ilvl=\"" + std::to_string(level) + "\"><w:start w:val=\"1\"/>";
            if (ordered) {
                xml += "<w:numFmt w:val=\"decimal\"/><w:lvlText w:val=\"%" +
                       std::to_string(level + 1) + ".\"/>";
            } else {
                xml += "<w:numFmt w:val=\"bullet\"/><w:lvlText w:val=\"•\"/>";
            }
            xml += "<w:lvlJc w:val=\"left\"/><w:pPr><w:tabs><w:tab w:val=\"num\" w:pos=\"" +
                   std::to_string(720 + level * 720) + "\"/></w:tabs><w:ind w:left=\"" +
                   std::to_string(720 + level * 720) + "\" w:hanging=\"360\"/></w:pPr></w:lvl>";
        }
        xml += "</w:abstractNum>";
    };
    abstract(0, false);
    abstract(1, true);
    for (const ListInstance& instance : instances) {
        xml += "<w:num w:numId=\"" + std::to_string(instance.id) + "\"><w:abstractNumId w:val=\"" +
               std::string(instance.ordered ? "1" : "0") + "\"/>";
        if (instance.ordered && instance.start != 1)
            xml += "<w:lvlOverride w:ilvl=\"" + std::to_string(instance.level) +
                   "\"><w:startOverride w:val=\"" + std::to_string(instance.start) +
                   "\"/></w:lvlOverride>";
        xml += "</w:num>";
    }
    xml += "</w:numbering>";
    return xml;
}

std::string document_relationships_xml(const DocumentWriter& writer) {
    std::string xml = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
                      "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
                      "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles\" Target=\"styles.xml\"/>"
                      "<Relationship Id=\"rId2\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/settings\" Target=\"settings.xml\"/>";
    if (writer.has_lists())
        xml += "<Relationship Id=\"rId3\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/numbering\" Target=\"numbering.xml\"/>";
    for (const auto& item : writer.hyperlinks()) {
        xml += "<Relationship Id=\"" + item.second +
               "\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/hyperlink\" Target=\"" +
               xml_escape(item.first, true) + "\" TargetMode=\"External\"/>";
    }
    xml += "</Relationships>";
    return xml;
}

Error create_docx(std::string_view markdown_text,
                  const WriteOptions& options,
                  std::string& bytes) {
    bytes.clear();
    if (options.cancellation.cancelled()) return {ErrorCode::Cancelled, "DOCX serialization cancelled"};
    std::string markdown(markdown_text);
    std::size_t invalid = 0;
    if (!html::is_valid_utf8(markdown, &invalid))
        return {ErrorCode::UnsupportedFeature, "cannot write DOCX: Markdown is not valid UTF-8 at byte " +
                                                   std::to_string(invalid)};
    for (std::size_t i = 0; i < markdown.size(); ++i) {
        const unsigned char ch = static_cast<unsigned char>(markdown[i]);
        if (ch < 0x20U && ch != '\t' && ch != '\n' && ch != '\r') {
            return {ErrorCode::UnsupportedFeature,
                    "cannot write DOCX: Markdown contains an XML-forbidden control byte at byte " +
                        std::to_string(i)};
        }
    }
    const std::vector<markdown::Block> blocks = markdown::parse_blocks(markdown);
    if (options.cancellation.cancelled()) return {ErrorCode::Cancelled, "DOCX serialization cancelled"};
    DocumentWriter writer(blocks, options.cancellation);
    std::string document;
    Error error = writer.document(document);
    if (!error.ok()) return error;
    const std::string content_types_xml =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
        "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
        "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
        "<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
        "<Override PartName=\"/word/document.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml\"/>"
        "<Override PartName=\"/word/styles.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.styles+xml\"/>"
        "<Override PartName=\"/word/settings.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.settings+xml\"/>" +
        std::string(writer.has_lists()
                        ? "<Override PartName=\"/word/numbering.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.numbering+xml\"/>"
                        : "") +
        "<Override PartName=\"/docProps/core.xml\" ContentType=\"application/vnd.openxmlformats-package.core-properties+xml\"/>"
        "<Override PartName=\"/docProps/app.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.extended-properties+xml\"/>"
        "</Types>";
    const std::string package_rels =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
        "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
        "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" Target=\"word/document.xml\"/>"
        "<Relationship Id=\"rId2\" Type=\"http://schemas.openxmlformats.org/package/2006/relationships/metadata/core-properties\" Target=\"docProps/core.xml\"/>"
        "<Relationship Id=\"rId3\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/extended-properties\" Target=\"docProps/app.xml\"/>"
        "</Relationships>";
    const std::string core =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
        "<cp:coreProperties xmlns:cp=\"http://schemas.openxmlformats.org/package/2006/metadata/core-properties\" "
        "xmlns:dc=\"http://purl.org/dc/elements/1.1/\" xmlns:dcterms=\"http://purl.org/dc/terms/\" "
        "xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\"><dc:creator>Ainiux</dc:creator>"
        "<cp:lastModifiedBy>Ainiux</cp:lastModifiedBy><dcterms:created xsi:type=\"dcterms:W3CDTF\">"
        "2000-01-01T00:00:00Z</dcterms:created><dcterms:modified xsi:type=\"dcterms:W3CDTF\">"
        "2000-01-01T00:00:00Z</dcterms:modified></cp:coreProperties>";
    const std::string app =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
        "<Properties xmlns=\"http://schemas.openxmlformats.org/officeDocument/2006/extended-properties\" "
        "xmlns:vt=\"http://schemas.openxmlformats.org/officeDocument/2006/docPropsVTypes\">"
        "<Application>Ainiux</Application><AppVersion>1.0</AppVersion></Properties>";
    const std::string settings =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
        "<w:settings xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">"
        "<w:compat><w:compatSetting w:name=\"compatibilityMode\" w:uri=\"http://schemas.microsoft.com/office/word\" w:val=\"15\"/>"
        "</w:compat></w:settings>";
    std::vector<detail::ZipWriteEntry> entries;
    entries.push_back({"[Content_Types].xml", content_types_xml, true});
    entries.push_back({"_rels/.rels", package_rels, true});
    entries.push_back({"docProps/core.xml", core, true});
    entries.push_back({"docProps/app.xml", app, true});
    entries.push_back({"word/document.xml", std::move(document), true});
    entries.push_back({"word/_rels/document.xml.rels", document_relationships_xml(writer), true});
    entries.push_back({"word/styles.xml", styles_xml(), true});
    entries.push_back({"word/settings.xml", settings, true});
    if (writer.has_lists()) entries.push_back({"word/numbering.xml", numbering_xml(writer.lists()), true});
    const std::size_t max_bytes = options.max_bytes == 0 ? 64U * 1024U * 1024U : options.max_bytes;
    return detail::write_zip(entries, max_bytes, options.cancellation, bytes);
}

}  // namespace

Error to_markdown_file(const std::string& path,
                       const ReadOptions& options,
                       std::string& markdown,
                       Diagnostics* diagnostics) {
    reset_diagnostics(diagnostics);
    markdown.clear();
    ReadOptions effective = options;
    if (effective.max_bytes == 0) effective.max_bytes = 64U * 1024U * 1024U;
    std::string bytes;
    Error error = read_file_bounded(path, effective, bytes);
    if (!error.ok()) return error;
    return extract_docx(bytes, effective, markdown, diagnostics);
}

Error to_markdown_bytes(std::string_view bytes,
                        const ReadOptions& options,
                        std::string& markdown,
                        Diagnostics* diagnostics) {
    reset_diagnostics(diagnostics);
    markdown.clear();
    const std::size_t maximum = options.max_bytes == 0 ? 64U * 1024U * 1024U : options.max_bytes;
    if (bytes.size() > maximum)
        return {ErrorCode::UnsupportedFeature, "DOCX exceeds input size limit of " +
                                                   std::to_string(maximum) + " bytes"};
    try {
        return extract_docx(bytes, options, markdown, diagnostics);
    } catch (const std::bad_alloc&) {
        markdown.clear();
        return {ErrorCode::Internal, "not enough memory to convert DOCX"};
    } catch (const std::length_error&) {
        markdown.clear();
        return {ErrorCode::UnsupportedFeature, "DOCX structures are too large to convert"};
    }
}

Error from_markdown(std::string_view markdown,
                    const WriteOptions& options,
                    std::string& bytes,
                    Diagnostics* diagnostics) {
    reset_diagnostics(diagnostics);
    try {
        return create_docx(markdown, options, bytes);
    } catch (const std::bad_alloc&) {
        bytes.clear();
        return {ErrorCode::Internal, "not enough memory to write DOCX"};
    } catch (const std::length_error&) {
        bytes.clear();
        return {ErrorCode::UnsupportedFeature, "DOCX output is too large"};
    }
}

}  // namespace ainiux::docx
