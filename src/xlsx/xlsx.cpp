#include "xlsx/xlsx.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <new>
#include <unordered_map>
#include <utility>

#include "docx/xml.hpp"
#include "docx/zip.hpp"
#include "html/html.hpp"
#include "markdown/blocks.hpp"

namespace ainiux::xlsx {
namespace {

using docx::detail::XmlEvent;
using docx::detail::ZipArchive;
using docx::detail::ZipLimits;
using docx::detail::ZipWriteEntry;

constexpr const char* kMain =
    "http://schemas.openxmlformats.org/spreadsheetml/2006/main";
constexpr const char* kMainStrict = "http://purl.oclc.org/ooxml/spreadsheetml/main";
constexpr const char* kOfficeRel =
    "http://schemas.openxmlformats.org/officeDocument/2006/relationships";
constexpr const char* kOfficeRelStrict =
    "http://purl.oclc.org/ooxml/officeDocument/relationships";
constexpr const char* kPkgRel =
    "http://schemas.openxmlformats.org/package/2006/relationships";

constexpr std::uint32_t kMaxColumns = 16384;
constexpr std::uint32_t kMaxRows = 1048576;
constexpr std::size_t kMaxCells = 1000000;
constexpr std::size_t kMaxSheets = 256;
constexpr std::size_t kMaxStringChars = 32767;
constexpr std::size_t kMaxDiagnostics = 16;

bool is_ssml(const XmlEvent& event, std::string_view local) {
    return event.name.local == local &&
           (event.name.uri.empty() || event.name.uri == kMain || event.name.uri == kMainStrict);
}

bool is_rel(const XmlEvent& event, std::string_view local) {
    return event.name.local == local &&
           (event.name.uri == kPkgRel || event.name.uri.empty());
}

bool type_suffix(const std::string& type, std::string_view suffix) {
    return type.size() >= suffix.size() &&
           type.compare(type.size() - suffix.size(), suffix.size(), suffix) == 0;
}

const std::string* attr_local(const XmlEvent& event, std::string_view local) {
    return docx::detail::attribute_local(event, local);
}

const std::string* sheet_rid(const XmlEvent& event) {
    if (const std::string* value = docx::detail::attribute(event, kOfficeRel, "id")) return value;
    if (const std::string* value = docx::detail::attribute(event, kOfficeRelStrict, "id")) return value;
    return attr_local(event, "id");
}

void diagnostic(Diagnostics* diagnostics, const std::string& message) {
    if (diagnostics == nullptr || diagnostics->messages.size() >= kMaxDiagnostics ||
        std::find(diagnostics->messages.begin(), diagnostics->messages.end(), message) !=
            diagnostics->messages.end())
        return;
    diagnostics->messages.push_back(message);
}

std::string relationships_part(const std::string& part) {
    const std::size_t slash = part.find_last_of('/');
    if (slash == std::string::npos) return "_rels/" + part + ".rels";
    return part.substr(0, slash + 1) + "_rels/" + part.substr(slash + 1) + ".rels";
}

bool zip_magic(std::string_view body) {
    return body.size() >= 4 && body[0] == 'P' && body[1] == 'K' &&
           ((body[2] == '\x03' && body[3] == '\x04') ||
            (body[2] == '\x05' && body[3] == '\x06') ||
            (body[2] == '\x07' && body[3] == '\x08'));
}

std::string xml_escape(std::string_view text, bool attribute) {
    std::string out;
    out.reserve(text.size());
    for (unsigned char ch : text) {
        switch (ch) {
            case '&':
                out += "&amp;";
                break;
            case '<':
                out += "&lt;";
                break;
            case '>':
                out += "&gt;";
                break;
            case '"':
                if (attribute) out += "&quot;";
                else out += '"';
                break;
            case '\'':
                if (attribute) out += "&apos;";
                else out += '\'';
                break;
            default:
                out.push_back(static_cast<char>(ch));
                break;
        }
    }
    return out;
}

bool control_xml_char(unsigned char ch) {
    return ch <= 0x08U || ch == 0x0bU || ch == 0x0cU || (ch >= 0x0eU && ch <= 0x1fU);
}

int hex_value(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

void decode_xhhhh(std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size();) {
        if (text[i] == '_' && i + 6 < text.size() && text[i + 1] == 'x' && text[i + 6] == '_') {
            int a = hex_value(text[i + 2]);
            int b = hex_value(text[i + 3]);
            int c = hex_value(text[i + 4]);
            int d = hex_value(text[i + 5]);
            if (a >= 0 && b >= 0 && c >= 0 && d >= 0) {
                const int value = (a << 12) | (b << 8) | (c << 4) | d;
                if (value <= 0xff && (control_xml_char(static_cast<unsigned char>(value)) || value == 0x5f)) {
                    out.push_back(static_cast<char>(value));
                    i += 7;
                    continue;
                }
            }
        }
        out.push_back(text[i]);
        ++i;
    }
    text = std::move(out);
}

void encode_xhhhh(std::string& text) {
    std::string out;
    out.reserve(text.size());
    auto append_hex = [&](unsigned value) {
        static const char* kHex = "0123456789ABCDEF";
        out += "_x";
        out.push_back(kHex[(value >> 12) & 0xf]);
        out.push_back(kHex[(value >> 8) & 0xf]);
        out.push_back(kHex[(value >> 4) & 0xf]);
        out.push_back(kHex[value & 0xf]);
        out += '_';
    };
    for (std::size_t i = 0; i < text.size();) {
        if (text[i] == '_' && i + 6 < text.size() && text[i + 1] == 'x' && text[i + 6] == '_' &&
            hex_value(text[i + 2]) >= 0 && hex_value(text[i + 3]) >= 0 &&
            hex_value(text[i + 4]) >= 0 && hex_value(text[i + 5]) >= 0) {
            append_hex(0x5f);
            ++i;
            continue;
        }
        const unsigned char ch = static_cast<unsigned char>(text[i]);
        if (control_xml_char(ch)) {
            append_hex(ch);
            ++i;
            continue;
        }
        out.push_back(text[i]);
        ++i;
    }
    text = std::move(out);
}

bool parse_a1(std::string_view ref, std::uint32_t& col, std::uint32_t& row) {
    col = 0;
    row = 0;
    std::size_t i = 0;
    while (i < ref.size()) {
        const unsigned char ch = static_cast<unsigned char>(ref[i]);
        if (ch < 'A' || (ch > 'Z' && ch < 'a') || ch > 'z') break;
        const unsigned letter = ch >= 'a' ? ch - 'a' : ch - 'A';
        if (col > (kMaxColumns - (letter + 1U)) / 26U) return false;
        col = col * 26U + letter + 1U;
        ++i;
    }
    if (col == 0 || i == ref.size()) return false;
    while (i < ref.size()) {
        const unsigned char ch = static_cast<unsigned char>(ref[i]);
        if (ch < '0' || ch > '9') return false;
        if (row > (kMaxRows - (ch - '0')) / 10U) return false;
        row = row * 10U + (ch - '0');
        ++i;
    }
    return row >= 1 && col >= 1 && col <= kMaxColumns && row <= kMaxRows;
}

void append_column_name(std::string& out, std::uint32_t col) {
    char buf[8];
    int n = 0;
    while (col > 0 && n < 8) {
        const std::uint32_t v = (col - 1U) % 26U;
        buf[n++] = static_cast<char>('A' + v);
        col = (col - 1U) / 26U;
    }
    while (n > 0) out.push_back(buf[--n]);
}

int days_from_civil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<int>(doe) - 719468;
}

void civil_from_days(int z, int& y, unsigned& m, unsigned& d) {
    z += 719468;
    const int era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = static_cast<int>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp < 10 ? mp + 3 : mp - 9;
    y += (m <= 2);
}

bool builtin_date_format(int id) {
    return (id >= 14 && id <= 22) || (id >= 45 && id <= 47);
}

bool custom_date_format(std::string code) {
    bool quote = false;
    std::string filtered;
    filtered.reserve(code.size());
    for (std::size_t i = 0; i < code.size(); ++i) {
        if (code[i] == '"') {
            quote = !quote;
            continue;
        }
        if (quote) continue;
        if (code[i] == '[') {
            const std::size_t close = code.find(']', i);
            if (close == std::string::npos) break;
            i = close;
            continue;
        }
        if (code[i] == '\\' && i + 1 < code.size()) {
            ++i;
            continue;
        }
        filtered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(code[i]))));
    }
    if (filtered.find_first_of("ydhs") != std::string::npos) return true;
    return filtered.find('m') != std::string::npos &&
           (filtered.find('/') != std::string::npos || filtered.find('-') != std::string::npos ||
            filtered.find(':') != std::string::npos);
}

std::string format_iso_date(double serial, bool date1904) {
    if (!std::isfinite(serial) || serial < 0 || serial >= 3000000) return {};
    double whole = 0;
    const double frac = std::modf(serial + 1e-12, &whole);
    long seconds = static_cast<long>(std::llround(frac * 86400.0));
    if (seconds >= 86400) {
        seconds -= 86400;
        whole += 1;
    }
    if (seconds < 0) {
        seconds += 86400;
        whole -= 1;
    }
    const long day = static_cast<long>(whole);
    int y = 0;
    unsigned m = 0;
    unsigned d = 0;
    if (date1904) {
        civil_from_days(days_from_civil(1904, 1, 1) + static_cast<int>(day), y, m, d);
    } else if (day == 60) {
        y = 1900;
        m = 2;
        d = 29;
    } else if (day < 60) {
        civil_from_days(days_from_civil(1899, 12, 31) + static_cast<int>(day), y, m, d);
    } else {
        civil_from_days(days_from_civil(1899, 12, 30) + static_cast<int>(day), y, m, d);
    }
    if (y < 1 || y > 9999) return {};
    char buf[32];
    const int hh = static_cast<int>(seconds / 3600);
    const int mm = static_cast<int>((seconds % 3600) / 60);
    const int ss = static_cast<int>(seconds % 60);
    if (hh == 0 && mm == 0 && ss == 0) {
        std::snprintf(buf, sizeof(buf), "%04d-%02u-%02u", y, m, d);
    } else {
        std::snprintf(buf, sizeof(buf), "%04d-%02u-%02uT%02d:%02d:%02d", y, m, d, hh, mm, ss);
    }
    return buf;
}

void append_number(std::string& out, double value) {
    if (!std::isfinite(value)) {
        out += "0";
        return;
    }
    if (value == 0) {
        out += '0';
        return;
    }
    if (value >= static_cast<double>(std::numeric_limits<long long>::min()) &&
        value <= static_cast<double>(std::numeric_limits<long long>::max())) {
        const auto nearest = static_cast<long long>(value);
        if (static_cast<double>(nearest) == value) {
            char buf[32];
            const auto result = std::to_chars(buf, buf + sizeof(buf), nearest);
            if (result.ec == std::errc()) {
                out.append(buf, result.ptr);
                return;
            }
        }
    }
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.15g", value);
    for (char* p = buf; *p != '\0'; ++p) {
        if (*p == ',') *p = '.';
    }
    out += buf;
}

bool parse_number(std::string_view text, double& value) {
    if (text.empty()) return false;
    std::size_t i = 0;
    bool negative = false;
    if (text[i] == '+' || text[i] == '-') {
        negative = text[i] == '-';
        ++i;
        if (i == text.size()) return false;
    }
    double n = 0;
    bool digit = false;
    while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
        digit = true;
        n = n * 10.0 + (text[i] - '0');
        ++i;
    }
    if (i < text.size() && text[i] == '.') {
        ++i;
        double scale = 0.1;
        while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
            digit = true;
            n += (text[i] - '0') * scale;
            scale *= 0.1;
            ++i;
        }
    }
    if (!digit) return false;
    if (i < text.size() && (text[i] == 'e' || text[i] == 'E')) {
        ++i;
        bool exp_neg = false;
        if (i < text.size() && (text[i] == '+' || text[i] == '-')) {
            exp_neg = text[i] == '-';
            ++i;
        }
        if (i == text.size() || text[i] < '0' || text[i] > '9') return false;
        int exp = 0;
        while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
            if (exp > 1000) return false;
            exp = exp * 10 + (text[i] - '0');
            ++i;
        }
        double factor = 1;
        for (int e = 0; e < exp; ++e) factor *= 10.0;
        n = exp_neg ? n / factor : n * factor;
    }
    if (i != text.size() || !std::isfinite(n)) return false;
    value = negative ? -n : n;
    return true;
}

bool parse_bool_cell(std::string_view text, bool& value) {
    if (text.size() == 4 && (text[0] == 'T' || text[0] == 't') && (text[1] == 'R' || text[1] == 'r') &&
        (text[2] == 'U' || text[2] == 'u') && (text[3] == 'E' || text[3] == 'e')) {
        value = true;
        return true;
    }
    if (text.size() == 5 && (text[0] == 'F' || text[0] == 'f') && (text[1] == 'A' || text[1] == 'a') &&
        (text[2] == 'L' || text[2] == 'l') && (text[3] == 'S' || text[3] == 's') &&
        (text[4] == 'E' || text[4] == 'e')) {
        value = false;
        return true;
    }
    return false;
}

enum class CellKind { Empty, Number, String, Bool, Error };

struct Cell {
    CellKind kind = CellKind::Empty;
    std::string text;
    double number = 0;
};

struct Sheet {
    std::string name;
    std::vector<std::vector<Cell>> rows;
};

struct Relationship {
    std::string type;
    std::string target;
    bool external = false;
};

using RelationshipMap = std::unordered_map<std::string, Relationship>;

Error parse_relationships(const std::string& xml,
                          runtime::CancellationToken cancellation,
                          RelationshipMap& out) {
    out.clear();
    return docx::detail::scan_xml(xml, {}, cancellation, [&](const XmlEvent& event) -> Error {
        if (event.kind != XmlEvent::Kind::Start || !is_rel(event, "Relationship")) return ok_error();
        const std::string* id = attr_local(event, "Id");
        const std::string* type = attr_local(event, "Type");
        const std::string* target = attr_local(event, "Target");
        const std::string* mode = attr_local(event, "TargetMode");
        if (id == nullptr || type == nullptr || target == nullptr || id->empty() || type->empty() ||
            target->empty()) {
            return {ErrorCode::FileRead, "invalid XLSX relationships: Relationship is missing Id, Type, or Target"};
        }
        if (out.find(*id) != out.end())
            return {ErrorCode::FileRead, "invalid XLSX relationships: duplicate Id " + *id};
        Relationship item;
        item.type = *type;
        item.target = *target;
        item.external = mode != nullptr && ascii_lower(*mode) == "external";
        out.emplace(*id, std::move(item));
        return ok_error();
    });
}

Error parse_shared_strings(const std::string& xml,
                           runtime::CancellationToken cancellation,
                           std::vector<std::string>& strings) {
    strings.clear();
    std::string current;
    bool in_si = false;
    bool in_t = false;
    bool in_phonetic = false;
    return docx::detail::scan_xml(xml, {}, cancellation, [&](const XmlEvent& event) -> Error {
        if (event.kind == XmlEvent::Kind::Start) {
            if (is_ssml(event, "si")) {
                in_si = true;
                current.clear();
            } else if (is_ssml(event, "rPh") || is_ssml(event, "phoneticPr")) {
                in_phonetic = true;
            } else if (in_si && !in_phonetic && is_ssml(event, "t")) {
                in_t = true;
            }
        } else if (event.kind == XmlEvent::Kind::End) {
            if (is_ssml(event, "t")) {
                in_t = false;
            } else if (is_ssml(event, "rPh") || is_ssml(event, "phoneticPr")) {
                in_phonetic = false;
            } else if (is_ssml(event, "si")) {
                decode_xhhhh(current);
                if (current.size() > kMaxStringChars) current.resize(kMaxStringChars);
                try {
                    strings.push_back(std::move(current));
                } catch (const std::bad_alloc&) {
                    return {ErrorCode::Internal, "not enough memory to store XLSX shared strings"};
                }
                current.clear();
                in_si = false;
            }
        } else if (in_t && !in_phonetic) {
            current += event.text;
        }
        return ok_error();
    });
}

struct StyleTable {
    std::unordered_map<int, std::string> custom_formats;
    std::vector<int> cell_xfs;
};

Error parse_styles(const std::string& xml, runtime::CancellationToken cancellation, StyleTable& styles) {
    styles = {};
    bool in_num_fmts = false;
    bool in_cell_xfs = false;
    return docx::detail::scan_xml(xml, {}, cancellation, [&](const XmlEvent& event) -> Error {
        if (event.kind != XmlEvent::Kind::Start) {
            if (event.kind == XmlEvent::Kind::End) {
                if (is_ssml(event, "numFmts")) in_num_fmts = false;
                if (is_ssml(event, "cellXfs")) in_cell_xfs = false;
            }
            return ok_error();
        }
        if (is_ssml(event, "numFmts")) {
            in_num_fmts = true;
        } else if (is_ssml(event, "cellXfs")) {
            in_cell_xfs = true;
        } else if (in_num_fmts && is_ssml(event, "numFmt")) {
            const std::string* id = attr_local(event, "numFmtId");
            const std::string* code = attr_local(event, "formatCode");
            if (id != nullptr && code != nullptr) {
                int parsed = 0;
                const auto result = std::from_chars(id->data(), id->data() + id->size(), parsed);
                if (result.ec == std::errc()) styles.custom_formats[parsed] = *code;
            }
        } else if (in_cell_xfs && is_ssml(event, "xf")) {
            int id = 0;
            if (const std::string* value = attr_local(event, "numFmtId")) {
                const auto result = std::from_chars(value->data(), value->data() + value->size(), id);
                if (result.ec != std::errc()) id = 0;
            }
            styles.cell_xfs.push_back(id);
        }
        return ok_error();
    });
}

bool style_is_date(const StyleTable& styles, int style_index) {
    if (style_index < 0) return false;
    int num_fmt = 0;
    if (static_cast<std::size_t>(style_index) < styles.cell_xfs.size())
        num_fmt = styles.cell_xfs[static_cast<std::size_t>(style_index)];
    if (builtin_date_format(num_fmt)) return true;
    const auto found = styles.custom_formats.find(num_fmt);
    return found != styles.custom_formats.end() && custom_date_format(found->second);
}

std::string cell_markdown(const Cell& cell) {
    switch (cell.kind) {
        case CellKind::Empty:
            return {};
        case CellKind::Bool:
            return cell.number != 0 ? "TRUE" : "FALSE";
        case CellKind::Number:
        case CellKind::Error:
        case CellKind::String:
            return cell.text;
    }
    return {};
}

void escape_gfm_cell(std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (char ch : text) {
        if (ch == '|') {
            out += "\\|";
        } else if (ch == '\n' || ch == '\r') {
            out += "<br>";
        } else {
            out.push_back(ch);
        }
    }
    text = std::move(out);
}

void append_gfm_row(std::string& markdown, const std::vector<std::string>& cells, std::size_t columns) {
    markdown += '|';
    for (std::size_t i = 0; i < columns; ++i) {
        markdown += ' ';
        if (i < cells.size()) markdown += cells[i];
        markdown += " |";
    }
    markdown += '\n';
}

Error emit_sheet_markdown(const Sheet& sheet, std::string& markdown) {
    std::size_t columns = 0;
    for (const auto& row : sheet.rows) columns = std::max(columns, row.size());
    if (columns == 0) return ok_error();
    if (columns == 1) columns = 2;
    markdown += "# ";
    markdown += sheet.name.empty() ? "Sheet" : sheet.name;
    markdown += "\n\n";
    std::vector<std::string> cells;
    cells.reserve(columns);
    auto fill = [&](const std::vector<Cell>& row) {
        cells.assign(columns, {});
        for (std::size_t i = 0; i < row.size() && i < columns; ++i) {
            cells[i] = cell_markdown(row[i]);
            escape_gfm_cell(cells[i]);
        }
    };
    if (sheet.rows.empty()) return ok_error();
    fill(sheet.rows.front());
    append_gfm_row(markdown, cells, columns);
    markdown += '|';
    for (std::size_t i = 0; i < columns; ++i) markdown += " --- |";
    markdown += '\n';
    for (std::size_t r = 1; r < sheet.rows.size(); ++r) {
        fill(sheet.rows[r]);
        append_gfm_row(markdown, cells, columns);
    }
    markdown += '\n';
    return ok_error();
}

struct ParsedCell {
    std::uint32_t row = 0;
    std::uint32_t col = 0;
    Cell cell;
};

Error parse_sheet_data(const std::string& xml,
                       const std::vector<std::string>& shared,
                       const StyleTable& styles,
                       bool date1904,
                       runtime::CancellationToken cancellation,
                       Diagnostics* diagnostics,
                       std::vector<ParsedCell>& cells) {
    cells.clear();
    bool in_sheet_data = false;
    bool in_row = false;
    bool in_c = false;
    bool in_v = false;
    bool in_is = false;
    bool in_t = false;
    bool in_f = false;
    std::uint32_t row_index = 0;
    std::uint32_t next_col = 1;
    std::uint32_t cell_row = 0;
    std::uint32_t cell_col = 0;
    std::string cell_type;
    int cell_style = -1;
    std::string value;
    std::size_t merge_count = 0;

    auto finish_cell = [&]() -> Error {
        if (!in_c) return ok_error();
        Cell cell;
        if (cell_type == "s") {
            int index = 0;
            const auto result = std::from_chars(value.data(), value.data() + value.size(), index);
            if (result.ec == std::errc() && index >= 0 &&
                static_cast<std::size_t>(index) < shared.size()) {
                cell.kind = CellKind::String;
                cell.text = shared[static_cast<std::size_t>(index)];
            } else if (!value.empty()) {
                cell.kind = CellKind::String;
                cell.text = value;
            }
        } else if (cell_type == "inlineStr" || cell_type == "str") {
            decode_xhhhh(value);
            if (!value.empty()) {
                cell.kind = CellKind::String;
                cell.text = std::move(value);
            }
        } else if (cell_type == "b") {
            cell.kind = CellKind::Bool;
            cell.number = (value == "1" || ascii_lower(value) == "true") ? 1 : 0;
            cell.text = cell.number != 0 ? "TRUE" : "FALSE";
        } else if (cell_type == "e") {
            if (!value.empty()) {
                cell.kind = CellKind::Error;
                cell.text = std::move(value);
            }
        } else if (cell_type == "d") {
            if (!value.empty()) {
                cell.kind = CellKind::String;
                cell.text = std::move(value);
            }
        } else if (!value.empty()) {
            double number = 0;
            if (parse_number(value, number)) {
                if (style_is_date(styles, cell_style)) {
                    std::string iso = format_iso_date(number, date1904);
                    cell.kind = CellKind::String;
                    cell.text = iso.empty() ? std::move(value) : std::move(iso);
                } else {
                    cell.kind = CellKind::Number;
                    cell.number = number;
                    append_number(cell.text, number);
                }
            } else {
                cell.kind = CellKind::String;
                cell.text = std::move(value);
            }
        }
        if (cell.kind != CellKind::Empty) {
            if (cells.size() >= kMaxCells)
                return {ErrorCode::UnsupportedFeature, "XLSX worksheet exceeds the 1,000,000 cell conversion limit"};
            try {
                cells.push_back({cell_row, cell_col, std::move(cell)});
            } catch (const std::bad_alloc&) {
                return {ErrorCode::Internal, "not enough memory to store XLSX cells"};
            }
        }
        in_c = false;
        in_v = false;
        in_is = false;
        in_t = false;
        in_f = false;
        value.clear();
        cell_type.clear();
        cell_style = -1;
        return ok_error();
    };

    Error error = docx::detail::scan_xml(xml, {}, cancellation, [&](const XmlEvent& event) -> Error {
        if (event.kind == XmlEvent::Kind::Start) {
            if (is_ssml(event, "sheetData")) {
                in_sheet_data = true;
            } else if (in_sheet_data && is_ssml(event, "row")) {
                in_row = true;
                next_col = 1;
                if (const std::string* r = attr_local(event, "r")) {
                    unsigned parsed = 0;
                    const auto result = std::from_chars(r->data(), r->data() + r->size(), parsed);
                    row_index = result.ec == std::errc() ? parsed : row_index + 1;
                } else {
                    ++row_index;
                }
                if (row_index == 0) row_index = 1;
            } else if (in_row && is_ssml(event, "c")) {
                in_c = true;
                value.clear();
                cell_type.clear();
                cell_style = -1;
                cell_row = row_index;
                cell_col = next_col;
                if (const std::string* r = attr_local(event, "r")) {
                    std::uint32_t col = 0;
                    std::uint32_t row = 0;
                    if (parse_a1(*r, col, row)) {
                        cell_col = col;
                        cell_row = row;
                    }
                }
                if (const std::string* t = attr_local(event, "t")) cell_type = *t;
                if (const std::string* s = attr_local(event, "s")) {
                    const auto result = std::from_chars(s->data(), s->data() + s->size(), cell_style);
                    if (result.ec != std::errc()) cell_style = -1;
                }
                next_col = cell_col + 1;
            } else if (in_c && is_ssml(event, "v")) {
                in_v = true;
            } else if (in_c && is_ssml(event, "is")) {
                in_is = true;
            } else if (in_c && is_ssml(event, "t")) {
                in_t = true;
            } else if (in_c && is_ssml(event, "f")) {
                in_f = true;
            } else if (is_ssml(event, "mergeCell")) {
                ++merge_count;
            }
        } else if (event.kind == XmlEvent::Kind::End) {
            if (is_ssml(event, "v")) {
                in_v = false;
            } else if (is_ssml(event, "t")) {
                in_t = false;
            } else if (is_ssml(event, "is")) {
                in_is = false;
            } else if (is_ssml(event, "f")) {
                in_f = false;
            } else if (is_ssml(event, "c")) {
                Error done = finish_cell();
                if (!done.ok()) return done;
            } else if (is_ssml(event, "row")) {
                in_row = false;
            } else if (is_ssml(event, "sheetData")) {
                in_sheet_data = false;
            }
        } else if (in_c && !in_f && (in_v || (in_is && in_t) || (in_t && cell_type == "inlineStr"))) {
            value += event.text;
        }
        return ok_error();
    });
    if (!error.ok()) return error;
    if (merge_count > 0 && diagnostics != nullptr) {
        diagnostics->flattened_merged_cells += merge_count;
        diagnostic(diagnostics, "merged cells were flattened into a rectangular Markdown table");
    }
    return ok_error();
}

Error build_sheet(const std::string& name,
                  std::vector<ParsedCell> cells,
                  Sheet& sheet) {
    sheet = {};
    sheet.name = name;
    if (cells.empty()) return ok_error();
    std::uint32_t min_row = kMaxRows;
    std::uint32_t max_row = 1;
    std::uint32_t min_col = kMaxColumns;
    std::uint32_t max_col = 1;
    for (const ParsedCell& item : cells) {
        min_row = std::min(min_row, item.row);
        max_row = std::max(max_row, item.row);
        min_col = std::min(min_col, item.col);
        max_col = std::max(max_col, item.col);
    }
    const std::uint64_t rows = static_cast<std::uint64_t>(max_row - min_row) + 1U;
    const std::uint64_t cols = static_cast<std::uint64_t>(max_col - min_col) + 1U;
    if (rows * cols > kMaxCells)
        return {ErrorCode::UnsupportedFeature, "XLSX worksheet used range exceeds the 1,000,000 cell conversion limit"};
    try {
        sheet.rows.assign(static_cast<std::size_t>(rows),
                          std::vector<Cell>(static_cast<std::size_t>(cols)));
        for (ParsedCell& item : cells) {
            sheet.rows[item.row - min_row][item.col - min_col] = std::move(item.cell);
        }
    } catch (const std::bad_alloc&) {
        return {ErrorCode::Internal, "not enough memory to materialize an XLSX worksheet"};
    }
    return ok_error();
}

Error read_file_bounded(const std::string& path, const ReadOptions& options, std::string& bytes) {
    bytes.clear();
    const std::string resolved = expand_user_path(path);
    std::ifstream file(std::filesystem::u8path(resolved), std::ios::binary);
    if (!file) return {ErrorCode::FileRead, "could not open XLSX for reading: " + resolved};
    std::array<char, 65536> buffer{};
    try {
        while (file) {
            if (options.cancellation.cancelled())
                return {ErrorCode::Cancelled, "XLSX read cancelled: " + resolved};
            file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const std::streamsize count = file.gcount();
            if (count <= 0) break;
            const std::size_t chunk = static_cast<std::size_t>(count);
            if (bytes.size() > options.max_bytes || chunk > options.max_bytes - bytes.size()) {
                bytes.clear();
                return {ErrorCode::UnsupportedFeature,
                        "XLSX exceeds input size limit of " + std::to_string(options.max_bytes) +
                            " bytes: " + resolved};
            }
            bytes.append(buffer.data(), chunk);
        }
    } catch (const std::bad_alloc&) {
        bytes.clear();
        return {ErrorCode::Internal, "not enough memory to read XLSX: " + resolved};
    }
    if (file.bad()) return {ErrorCode::FileRead, "could not read XLSX: " + resolved};
    return ok_error();
}

Error extract_xlsx(std::string_view bytes,
                   const ReadOptions& options,
                   std::string& markdown,
                   Diagnostics* diagnostics) {
    markdown.clear();
    ZipArchive archive;
    ZipLimits limits;
    limits.max_member_bytes = 64U * 1024U * 1024U;
    Error error = archive.open(bytes, limits, options.cancellation);
    if (!error.ok()) return error;
    if (!archive.contains("[Content_Types].xml"))
        return {ErrorCode::FileRead, "invalid XLSX package: [Content_Types].xml is missing"};
    if (!archive.contains("_rels/.rels"))
        return {ErrorCode::FileRead, "invalid XLSX package: package relationships are missing"};

    std::string xml;
    error = archive.read("_rels/.rels", xml);
    if (!error.ok()) return error;
    RelationshipMap package_rels;
    error = parse_relationships(xml, options.cancellation, package_rels);
    if (!error.ok()) return error;
    std::string workbook_part;
    for (const auto& item : package_rels) {
        if (type_suffix(item.second.type, "/officeDocument")) {
            if (!workbook_part.empty())
                return {ErrorCode::FileRead, "invalid XLSX package: multiple officeDocument relationships"};
            if (item.second.external)
                return {ErrorCode::FileRead, "invalid XLSX package: workbook relationship is external"};
            error = docx::detail::resolve_part_name({}, item.second.target, workbook_part);
            if (!error.ok()) return error;
        }
    }
    if (workbook_part.empty())
        return {ErrorCode::FileRead, "invalid XLSX package: officeDocument relationship is missing"};
    if (!archive.contains(workbook_part))
        return {ErrorCode::FileRead, "invalid XLSX package: workbook part is missing: " + workbook_part};

    RelationshipMap workbook_rels;
    const std::string rels_part = relationships_part(workbook_part);
    if (archive.contains(rels_part)) {
        error = archive.read(rels_part, xml);
        if (!error.ok()) return error;
        error = parse_relationships(xml, options.cancellation, workbook_rels);
        if (!error.ok()) return error;
    }

    std::string shared_part;
    std::string styles_part;
    std::unordered_map<std::string, std::string> sheet_parts;
    for (const auto& item : workbook_rels) {
        const Relationship& rel = item.second;
        if (rel.external) continue;
        std::string target;
        error = docx::detail::resolve_part_name(workbook_part, rel.target, target);
        if (!error.ok()) return error;
        if (type_suffix(rel.type, "/sharedStrings")) shared_part = std::move(target);
        else if (type_suffix(rel.type, "/styles")) styles_part = std::move(target);
        else if (type_suffix(rel.type, "/worksheet")) sheet_parts.emplace(item.first, std::move(target));
        else if (type_suffix(rel.type, "/chartsheet")) {
            if (diagnostics != nullptr) ++diagnostics->omitted_sheets;
            diagnostic(diagnostics, "chart sheets were omitted from XLSX conversion");
        }
    }

    error = archive.read(workbook_part, xml);
    if (!error.ok()) return error;
    struct SheetRef {
        std::string name;
        std::string rid;
    };
    std::vector<SheetRef> sheets;
    bool date1904 = false;
    error = docx::detail::scan_xml(xml, {}, options.cancellation, [&](const XmlEvent& event) -> Error {
        if (event.kind != XmlEvent::Kind::Start) return ok_error();
        if (is_ssml(event, "workbookPr")) {
            if (const std::string* value = attr_local(event, "date1904")) {
                const std::string lower = ascii_lower(*value);
                date1904 = lower == "1" || lower == "true";
            }
        } else if (is_ssml(event, "sheet")) {
            if (sheets.size() >= kMaxSheets)
                return {ErrorCode::UnsupportedFeature, "XLSX workbook has too many worksheets"};
            SheetRef ref;
            if (const std::string* name = attr_local(event, "name")) ref.name = *name;
            if (const std::string* rid = sheet_rid(event)) ref.rid = *rid;
            if (ref.rid.empty())
                return {ErrorCode::FileRead, "invalid XLSX workbook: sheet is missing r:id"};
            if (ref.name.empty()) ref.name = "Sheet" + std::to_string(sheets.size() + 1);
            for (char& ch : ref.name) {
                if (ch == '\n' || ch == '\r') ch = ' ';
            }
            sheets.push_back(std::move(ref));
        }
        return ok_error();
    });
    if (!error.ok()) return error;
    if (sheets.empty()) return {ErrorCode::FileRead, "invalid XLSX workbook: no worksheets were found"};

    std::vector<std::string> shared;
    if (!shared_part.empty()) {
        if (!archive.contains(shared_part))
            return {ErrorCode::FileRead, "invalid XLSX package: sharedStrings part is missing"};
        error = archive.read(shared_part, xml);
        if (!error.ok()) return error;
        error = parse_shared_strings(xml, options.cancellation, shared);
        if (!error.ok()) return error;
    }
    StyleTable styles;
    if (!styles_part.empty() && archive.contains(styles_part)) {
        error = archive.read(styles_part, xml);
        if (!error.ok()) return error;
        error = parse_styles(xml, options.cancellation, styles);
        if (!error.ok()) return error;
    }

    try {
        markdown.reserve(4096);
        bool any = false;
        for (const SheetRef& ref : sheets) {
            if (options.cancellation.cancelled())
                return {ErrorCode::Cancelled, "XLSX conversion cancelled"};
            const auto found = sheet_parts.find(ref.rid);
            if (found == sheet_parts.end()) {
                if (diagnostics != nullptr) ++diagnostics->omitted_sheets;
                diagnostic(diagnostics, "a workbook sheet could not be resolved and was omitted");
                continue;
            }
            if (!archive.contains(found->second))
                return {ErrorCode::FileRead, "invalid XLSX package: worksheet part is missing: " + found->second};
            error = archive.read(found->second, xml);
            if (!error.ok()) return error;
            std::vector<ParsedCell> parsed;
            error = parse_sheet_data(xml, shared, styles, date1904, options.cancellation, diagnostics, parsed);
            if (!error.ok()) return error;
            Sheet sheet;
            error = build_sheet(ref.name, std::move(parsed), sheet);
            if (!error.ok()) return error;
            if (sheet.rows.empty()) {
                if (diagnostics != nullptr) ++diagnostics->omitted_empty_sheets;
                diagnostic(diagnostics, "empty worksheets were omitted from XLSX conversion");
                continue;
            }
            error = emit_sheet_markdown(sheet, markdown);
            if (!error.ok()) return error;
            any = true;
        }
        if (!any) return {ErrorCode::FileRead, "XLSX workbook did not contain any convertible cell data"};
        while (!markdown.empty() && markdown.back() == '\n') markdown.pop_back();
        markdown.push_back('\n');
    } catch (const std::bad_alloc&) {
        markdown.clear();
        return {ErrorCode::Internal, "not enough memory to emit XLSX Markdown"};
    } catch (const std::length_error&) {
        markdown.clear();
        return {ErrorCode::UnsupportedFeature, "converted XLSX Markdown is too large"};
    }
    return ok_error();
}

std::string runs_text(const std::vector<markdown::Run>& runs) {
    std::string text;
    for (const markdown::Run& run : runs) {
        text += run.text;
        if (run.hard_break_after) text += '\n';
    }
    return text;
}

std::string unique_sheet_name(std::string name, std::size_t index, const std::vector<Sheet>& sheets) {
    for (char& ch : name) {
        if (ch == ':' || ch == '\\' || ch == '/' || ch == '?' || ch == '*' || ch == '[' || ch == ']' ||
            ch == '\n' || ch == '\r')
            ch = ' ';
    }
    while (!name.empty() && name.front() == '\'') name.erase(name.begin());
    while (!name.empty() && name.back() == '\'') name.pop_back();
    if (name.size() > 31) name.resize(31);
    if (name.empty()) name = "Sheet" + std::to_string(index + 1);
    std::string candidate = name;
    int suffix = 1;
    auto used = [&](const std::string& value) {
        for (const Sheet& sheet : sheets) {
            if (ascii_lower(sheet.name) == ascii_lower(value)) return true;
        }
        return false;
    };
    while (used(candidate)) {
        const std::string tail = "_" + std::to_string(++suffix);
        candidate = name;
        if (candidate.size() + tail.size() > 31)
            candidate.resize(31 > tail.size() ? 31 - tail.size() : 0);
        candidate += tail;
    }
    return candidate;
}

Cell infer_cell(std::string text) {
    Cell cell;
    if (text.empty()) return cell;
    bool flag = false;
    if (parse_bool_cell(text, flag)) {
        cell.kind = CellKind::Bool;
        cell.number = flag ? 1 : 0;
        cell.text = flag ? "TRUE" : "FALSE";
        return cell;
    }
    double number = 0;
    if (parse_number(text, number)) {
        cell.kind = CellKind::Number;
        cell.number = number;
        append_number(cell.text, number);
        return cell;
    }
    cell.kind = CellKind::String;
    if (text.size() > kMaxStringChars) text.resize(kMaxStringChars);
    cell.text = std::move(text);
    return cell;
}

Error tables_from_markdown(std::string_view markdown,
                           runtime::CancellationToken cancellation,
                           std::vector<Sheet>& sheets) {
    sheets.clear();
    const std::vector<markdown::Block> blocks = markdown::parse_blocks(std::string(markdown));
    std::string pending_name;
    for (const markdown::Block& block : blocks) {
        if (cancellation.cancelled()) return {ErrorCode::Cancelled, "XLSX conversion cancelled"};
        if (block.kind == markdown::BlockKind::Heading) {
            pending_name = runs_text(block.runs);
            continue;
        }
        if (block.kind != markdown::BlockKind::Table || block.table_cells.empty()) continue;
        Sheet sheet;
        sheet.name = unique_sheet_name(pending_name, sheets.size(), sheets);
        pending_name.clear();
        std::size_t columns = 0;
        for (const auto& row : block.table_cells) columns = std::max(columns, row.size());
        if (columns == 0) continue;
        try {
            sheet.rows.reserve(block.table_cells.size());
            for (const auto& row : block.table_cells) {
                std::vector<Cell> cells(columns);
                for (std::size_t i = 0; i < row.size(); ++i) cells[i] = infer_cell(runs_text(row[i]));
                sheet.rows.push_back(std::move(cells));
            }
        } catch (const std::bad_alloc&) {
            return {ErrorCode::Internal, "not enough memory to parse Markdown tables"};
        }
        sheets.push_back(std::move(sheet));
        if (sheets.size() > kMaxSheets)
            return {ErrorCode::UnsupportedFeature, "Markdown contains too many tables for XLSX output"};
    }
    if (sheets.empty())
        return {ErrorCode::BadArgs, "Markdown does not contain a valid table for XLSX output"};
    return ok_error();
}

std::string shared_strings_xml(const std::vector<std::string>& unique, std::size_t refs) {
    std::string xml = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
                      "<sst xmlns=\"";
    xml += kMain;
    xml += "\" count=\"";
    xml += std::to_string(refs);
    xml += "\" uniqueCount=\"";
    xml += std::to_string(unique.size());
    xml += "\">";
    for (const std::string& item : unique) {
        std::string text = item;
        encode_xhhhh(text);
        xml += "<si><t";
        if (!text.empty() && (text.front() == ' ' || text.back() == ' ' || text.find('\n') != std::string::npos))
            xml += " xml:space=\"preserve\"";
        xml += '>';
        xml += xml_escape(text, false);
        xml += "</t></si>";
    }
    xml += "</sst>";
    return xml;
}

std::string styles_xml() {
    return "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
           "<styleSheet xmlns=\""
           "http://schemas.openxmlformats.org/spreadsheetml/2006/main\">"
           "<fonts count=\"1\"><font><sz val=\"11\"/><name val=\"Calibri\"/></font></fonts>"
           "<fills count=\"2\"><fill><patternFill patternType=\"none\"/></fill>"
           "<fill><patternFill patternType=\"gray125\"/></fill></fills>"
           "<borders count=\"1\"><border><left/><right/><top/><bottom/><diagonal/></border></borders>"
           "<cellStyleXfs count=\"1\"><xf numFmtId=\"0\" fontId=\"0\" fillId=\"0\" borderId=\"0\"/></cellStyleXfs>"
           "<cellXfs count=\"1\"><xf numFmtId=\"0\" fontId=\"0\" fillId=\"0\" borderId=\"0\" xfId=\"0\"/></cellXfs>"
           "<cellStyles count=\"1\"><cellStyle name=\"Normal\" xfId=\"0\" builtinId=\"0\"/></cellStyles>"
           "</styleSheet>";
}

std::string worksheet_xml(const Sheet& sheet, std::unordered_map<std::string, std::uint32_t>& intern,
                          std::vector<std::string>& unique, std::size_t& refs) {
    std::size_t columns = 0;
    std::size_t last_row = 0;
    for (std::size_t r = 0; r < sheet.rows.size(); ++r) {
        if (sheet.rows[r].size() > columns) columns = sheet.rows[r].size();
        for (const Cell& cell : sheet.rows[r]) {
            if (cell.kind != CellKind::Empty) last_row = r + 1;
        }
    }
    if (last_row == 0) last_row = 1;
    if (columns == 0) columns = 1;
    std::string xml = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
                      "<worksheet xmlns=\"";
    xml += kMain;
    xml += "\" xmlns:r=\"";
    xml += kOfficeRel;
    xml += "\"><dimension ref=\"A1:";
    append_column_name(xml, static_cast<std::uint32_t>(columns));
    xml += std::to_string(last_row);
    xml += "\"/><sheetViews><sheetView workbookViewId=\"0\"/></sheetViews>"
           "<sheetFormatPr defaultRowHeight=\"15\"/><sheetData>";
    for (std::size_t r = 0; r < last_row; ++r) {
        xml += "<row r=\"";
        xml += std::to_string(r + 1);
        xml += "\" spans=\"1:";
        xml += std::to_string(columns);
        xml += "\">";
        const std::vector<Cell>& row = r < sheet.rows.size() ? sheet.rows[r] : std::vector<Cell>{};
        for (std::size_t c = 0; c < columns; ++c) {
            const Cell cell = c < row.size() ? row[c] : Cell{};
            if (cell.kind == CellKind::Empty) continue;
            xml += "<c r=\"";
            append_column_name(xml, static_cast<std::uint32_t>(c + 1));
            xml += std::to_string(r + 1);
            xml += '"';
            if (cell.kind == CellKind::String) {
                auto found = intern.find(cell.text);
                std::uint32_t index = 0;
                if (found == intern.end()) {
                    index = static_cast<std::uint32_t>(unique.size());
                    intern.emplace(cell.text, index);
                    unique.push_back(cell.text);
                } else {
                    index = found->second;
                }
                ++refs;
                xml += " t=\"s\"><v>";
                xml += std::to_string(index);
                xml += "</v></c>";
            } else if (cell.kind == CellKind::Bool) {
                xml += " t=\"b\"><v>";
                xml += cell.number != 0 ? '1' : '0';
                xml += "</v></c>";
            } else if (cell.kind == CellKind::Error) {
                xml += " t=\"e\"><v>";
                xml += xml_escape(cell.text, false);
                xml += "</v></c>";
            } else {
                xml += "><v>";
                xml += cell.text;
                xml += "</v></c>";
            }
        }
        xml += "</row>";
    }
    xml += "</sheetData></worksheet>";
    return xml;
}

Error write_package(const std::vector<Sheet>& sheets,
                    const WriteOptions& options,
                    std::string& bytes) {
    std::unordered_map<std::string, std::uint32_t> intern;
    intern.reserve(256);
    std::vector<std::string> unique;
    unique.reserve(256);
    std::size_t refs = 0;
    std::vector<std::string> sheet_xml;
    sheet_xml.reserve(sheets.size());
    for (const Sheet& sheet : sheets) {
        if (options.cancellation.cancelled()) return {ErrorCode::Cancelled, "XLSX serialization cancelled"};
        sheet_xml.push_back(worksheet_xml(sheet, intern, unique, refs));
    }
    std::string workbook =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
        "<workbook xmlns=\"";
    workbook += kMain;
    workbook += "\" xmlns:r=\"";
    workbook += kOfficeRel;
    workbook += "\"><sheets>";
    std::string workbook_rels =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
        "<Relationships xmlns=\"";
    workbook_rels += kPkgRel;
    workbook_rels += "\">";
    std::string types =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
        "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
        "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
        "<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
        "<Override PartName=\"/xl/workbook.xml\" "
        "ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml\"/>"
        "<Override PartName=\"/xl/styles.xml\" "
        "ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.styles+xml\"/>"
        "<Override PartName=\"/xl/sharedStrings.xml\" "
        "ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.sharedStrings+xml\"/>";
    std::vector<ZipWriteEntry> entries;
    entries.push_back({"[Content_Types].xml", {}, true});
    entries.push_back({"_rels/.rels",
                       "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
                       "<Relationships xmlns=\""
                           + std::string(kPkgRel) +
                           "\"><Relationship Id=\"rId1\" Type=\""
                           "http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument"
                           "\" Target=\"xl/workbook.xml\"/></Relationships>",
                       false});
    for (std::size_t i = 0; i < sheets.size(); ++i) {
        const std::string rid = "rId" + std::to_string(i + 1);
        const std::string part = "worksheets/sheet" + std::to_string(i + 1) + ".xml";
        workbook += "<sheet name=\"";
        workbook += xml_escape(sheets[i].name, true);
        workbook += "\" sheetId=\"";
        workbook += std::to_string(i + 1);
        workbook += "\" r:id=\"";
        workbook += rid;
        workbook += "\"/>";
        workbook_rels += "<Relationship Id=\"";
        workbook_rels += rid;
        workbook_rels += "\" Type=\"";
        workbook_rels += "http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet";
        workbook_rels += "\" Target=\"";
        workbook_rels += part;
        workbook_rels += "\"/>";
        types += "<Override PartName=\"/xl/";
        types += part;
        types += "\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml\"/>";
        entries.push_back({"xl/" + part, std::move(sheet_xml[i]), true});
    }
    workbook += "</sheets></workbook>";
    const std::string styles_rid = "rId" + std::to_string(sheets.size() + 1);
    const std::string shared_rid = "rId" + std::to_string(sheets.size() + 2);
    workbook_rels += "<Relationship Id=\"";
    workbook_rels += styles_rid;
    workbook_rels += "\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles"
                     "\" Target=\"styles.xml\"/>";
    workbook_rels += "<Relationship Id=\"";
    workbook_rels += shared_rid;
    workbook_rels +=
        "\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/sharedStrings"
        "\" Target=\"sharedStrings.xml\"/></Relationships>";
    types += "</Types>";
    entries[0].bytes = std::move(types);
    entries.push_back({"xl/workbook.xml", std::move(workbook), true});
    entries.push_back({"xl/_rels/workbook.xml.rels", std::move(workbook_rels), false});
    entries.push_back({"xl/styles.xml", styles_xml(), true});
    entries.push_back({"xl/sharedStrings.xml", shared_strings_xml(unique, refs), true});
    return docx::detail::write_zip(entries, options.max_bytes, options.cancellation, bytes);
}

}  // namespace

bool looks_like_xlsx(std::string_view bytes) {
    if (!zip_magic(bytes)) return false;
    if (bytes.find("[Content_Types].xml") == std::string_view::npos ||
        bytes.find("_rels/.rels") == std::string_view::npos)
        return false;
    return bytes.find("xl/") != std::string_view::npos || bytes.find("xl\\") != std::string_view::npos;
}

Error to_markdown_bytes(std::string_view bytes,
                        const ReadOptions& options,
                        std::string& markdown,
                        Diagnostics* diagnostics) {
    markdown.clear();
    if (diagnostics != nullptr) *diagnostics = Diagnostics{};
    if (options.cancellation.cancelled()) return {ErrorCode::Cancelled, "XLSX conversion cancelled"};
    if (bytes.size() > options.max_bytes)
        return {ErrorCode::UnsupportedFeature,
                "XLSX exceeds input size limit of " + std::to_string(options.max_bytes) + " bytes"};
    if (bytes.size() >= 8 && bytes.compare(0, 8, "\xd0\xcf\x11\xe0\xa1\xb1\x1a\xe1") == 0)
        return {ErrorCode::UnsupportedFeature, "legacy .xls (BIFF) workbooks are not supported; use .xlsx"};
    return extract_xlsx(bytes, options, markdown, diagnostics);
}

Error to_markdown_file(const std::string& path,
                       const ReadOptions& options,
                       std::string& markdown,
                       Diagnostics* diagnostics) {
    markdown.clear();
    std::string bytes;
    Error error = read_file_bounded(path, options, bytes);
    if (!error.ok()) return error;
    return to_markdown_bytes(bytes, options, markdown, diagnostics);
}

Error from_markdown(std::string_view markdown,
                    const WriteOptions& options,
                    std::string& bytes,
                    Diagnostics* diagnostics) {
    bytes.clear();
    if (diagnostics != nullptr) *diagnostics = Diagnostics{};
    if (options.cancellation.cancelled()) return {ErrorCode::Cancelled, "XLSX conversion cancelled"};
    std::vector<Sheet> sheets;
    Error error = tables_from_markdown(markdown, options.cancellation, sheets);
    if (!error.ok()) return error;
    return write_package(sheets, options, bytes);
}

}  // namespace ainiux::xlsx
