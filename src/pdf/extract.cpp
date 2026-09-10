#include "pdf/extract.hpp"

#include "pdf/fonts.hpp"
#include "pdf/limits.hpp"
#include "pdf/token.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ainiux::pdf {
namespace {

struct Span {
    double x = 0;
    double y = 0;
    double end_x = 0;
    double size = 11;
    double space_width = 3;
    std::string text;
};

struct Matrix {
    double a = 1;
    double b = 0;
    double c = 0;
    double d = 1;
    double e = 0;
    double f = 0;
};

Matrix multiply(const Matrix& left, const Matrix& right) {
    Matrix out;
    out.a = left.a * right.a + left.b * right.c;
    out.b = left.a * right.b + left.b * right.d;
    out.c = left.c * right.a + left.d * right.c;
    out.d = left.c * right.b + left.d * right.d;
    out.e = left.e * right.a + left.f * right.c + right.e;
    out.f = left.e * right.b + left.f * right.d + right.f;
    return out;
}

void transform(const Matrix& m, double x, double y, double& ox, double& oy) {
    ox = m.a * x + m.c * y + m.e;
    oy = m.b * x + m.d * y + m.f;
}

void append_utf8(std::string& out, unsigned int cp) {
    if (cp == 0) {
        return;
    }
    if (cp <= 0x7F) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        if (cp >= 0xD800 && cp <= 0xDFFF) {
            return;
        }
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0x10FFFF) {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

std::string utf16be_to_utf8(const std::string& bytes) {
    std::string out;
    out.reserve(bytes.size());
    for (std::size_t i = 0; i + 1 < bytes.size(); i += 2) {
        unsigned int unit = (static_cast<unsigned char>(bytes[i]) << 8) |
                            static_cast<unsigned char>(bytes[i + 1]);
        if (unit == 0xFEFF) {
            continue;
        }
        if (unit >= 0xD800 && unit <= 0xDBFF && i + 3 < bytes.size()) {
            unsigned int low = (static_cast<unsigned char>(bytes[i + 2]) << 8) |
                               static_cast<unsigned char>(bytes[i + 3]);
            if (low >= 0xDC00 && low <= 0xDFFF) {
                unit = 0x10000 + (((unit - 0xD800) << 10) | (low - 0xDC00));
                i += 2;
            }
        }
        append_utf8(out, unit);
    }
    return out;
}

std::uint32_t code_from_bytes(const std::string& bytes) {
    std::uint32_t value = 0;
    for (unsigned char ch : bytes) {
        value = (value << 8) | ch;
    }
    return value;
}

int hex_digit(char ch) {
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    return -1;
}

unsigned int parse_uni_name(const std::string& name) {
    std::size_t start = 0;
    if (name.size() >= 5 && (name.compare(0, 3, "uni") == 0 || name.compare(0, 3, "UNI") == 0)) {
        start = 3;
    } else if (name.size() >= 3 && (name[0] == 'u' || name[0] == 'U')) {
        start = 1;
    } else {
        return 0;
    }
    if (name.size() - start < 4 || (name.size() - start) % 4 != 0) {
        return 0;
    }
    unsigned int cp = 0;
    for (std::size_t i = start; i < start + 4; ++i) {
        const int n = hex_digit(name[i]);
        if (n < 0) {
            return 0;
        }
        cp = (cp << 4) | static_cast<unsigned int>(n);
    }
    return cp;
}

int glyph_unicode(const std::string& name) {
    if (name.size() == 1) {
        const unsigned char ch = static_cast<unsigned char>(name[0]);
        if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')) {
            return ch;
        }
    }
    static const std::unordered_map<std::string, int> kNames = {
        {"space", 0x20},        {"exclam", 0x21},      {"quotedbl", 0x22},     {"numbersign", 0x23},
        {"dollar", 0x24},       {"percent", 0x25},     {"ampersand", 0x26},    {"quotesingle", 0x27},
        {"parenleft", 0x28},    {"parenright", 0x29},  {"asterisk", 0x2A},     {"plus", 0x2B},
        {"comma", 0x2C},        {"hyphen", 0x2D},      {"period", 0x2E},       {"slash", 0x2F},
        {"colon", 0x3A},        {"semicolon", 0x3B},   {"less", 0x3C},         {"equal", 0x3D},
        {"greater", 0x3E},      {"question", 0x3F},    {"at", 0x40},           {"bracketleft", 0x5B},
        {"backslash", 0x5C},    {"bracketright", 0x5D},{"asciicircum", 0x5E},  {"underscore", 0x5F},
        {"grave", 0x60},        {"braceleft", 0x7B},   {"bar", 0x7C},          {"braceright", 0x7D},
        {"asciitilde", 0x7E},   {"quotedblleft", 0x201C}, {"quotedblright", 0x201D},
        {"quoteleft", 0x2018},  {"quoteright", 0x2019}, {"endash", 0x2013},    {"emdash", 0x2014},
        {"minus", 0x2212},      {"fi", 0xFB01},        {"fl", 0xFB02},         {"ff", 0xFB00},
        {"ffi", 0xFB03},        {"ffl", 0xFB04},       {"bullet", 0x2022},     {"ellipsis", 0x2026},
        {"dagger", 0x2020},     {"daggerdbl", 0x2021}, {"trademark", 0x2122},  {"copyright", 0xA9},
        {"registered", 0xAE},   {"degree", 0xB0},      {"plusminus", 0xB1},    {"multiply", 0xD7},
        {"divide", 0xF7},       {"section", 0xA7},     {"paragraph", 0xB6},    {"periodcentered", 0xB7},
        {"commaaccent", 0xF6C3},{"hungarumlaut", 0x2DD},{"ogonek", 0x2DB},     {"caron", 0x2C7},
        {"breve", 0x2D8},       {"dotaccent", 0x2D9},  {"ring", 0x2DA},        {"cedilla", 0xB8},
        {"macron", 0xAF},       {"circumflex", 0x2C6}, {"tilde", 0x2DC},       {"dieresis", 0xA8},
        {"acute", 0xB4},        {"ae", 0xE6},          {"AE", 0xC6},           {"oe", 0x153},
        {"OE", 0x152},          {"ssharp", 0xDF},      {"lslash", 0x142},      {"Lslash", 0x141},
        {"oslash", 0xF8},       {"Oslash", 0xD8},      {"eth", 0xF0},          {"Eth", 0xD0},
        {"thorn", 0xFE},        {"Thorn", 0xDE},       {"dotlessi", 0x131},    {"space.nobreak", 0xA0},
        {"nbspace", 0xA0},      {"nonbreakingspace", 0xA0}, {"visiblespace", 0x20},
        {"uni2423", 0x20},
    };
    const auto it = kNames.find(name);
    if (it != kNames.end()) {
        return it->second;
    }
    const unsigned int uni = parse_uni_name(name);
    if (uni != 0) {
        return static_cast<int>(uni);
    }
    return 0;
}

struct FontDecoder {
    int encoding[256]{};
    std::unordered_map<std::uint32_t, std::string> to_unicode;
    std::unordered_map<std::uint32_t, double> widths;
    int code_width = 1;
    bool has_tounicode = false;
    double default_width = 500;
    double space_width = 250;
};

double operand_number(const Token& token) {
    return std::strtod(token.text.c_str(), nullptr);
}

const Value* resolve_value(Document& document, const Value* value) {
    if (value == nullptr) {
        return nullptr;
    }
    if (value->type == ValueType::Ref) {
        return document.object_value(value->ref.number);
    }
    return value;
}

const Value* dict_resolved(Document& document, const Value& dict, const char* key) {
    return resolve_value(document, dict_get(dict, key));
}

bool starts_with_punct(const std::string& text) {
    if (text.empty()) {
        return false;
    }
    switch (static_cast<unsigned char>(text[0])) {
        case '.':
        case ',':
        case ';':
        case ':':
        case '!':
        case '?':
        case ')':
        case ']':
        case '}':
            return true;
        default:
            return false;
    }
}

bool is_space_text(const std::string& text) {
    return text == " " || text == "\xC2\xA0";
}

std::string strip_subset_prefix(const std::string& name) {
    if (name.size() > 7 && name[6] == '+') {
        bool prefix = true;
        for (std::size_t i = 0; i < 6; ++i) {
            const unsigned char ch = static_cast<unsigned char>(name[i]);
            if (ch < 'A' || ch > 'Z') {
                prefix = false;
                break;
            }
        }
        if (prefix) {
            return name.substr(7);
        }
    }
    return name;
}

void apply_ascii_widths(FontDecoder& font, const short* table, int count, double space, double missing) {
    for (int i = 0; i < count; ++i) {
        font.widths[static_cast<std::uint32_t>(32 + i)] = table[i];
    }
    font.space_width = space;
    font.default_width = missing;
}

void apply_base_font_widths(const std::string& basefont, FontDecoder& font) {
    const std::string name = strip_subset_prefix(basefont);
    if (name.find("Courier") != std::string::npos) {
        font.default_width = 600;
        font.space_width = 600;
        return;
    }
    if (name.find("Helvetica") != std::string::npos || name.find("Arial") != std::string::npos) {
        apply_ascii_widths(font, kHelveticaAscii, 95, 278, 556);
        return;
    }
    if (name.find("Times") != std::string::npos) {
        apply_ascii_widths(font, kTimesAscii, 95, 250, 500);
    }
}

double width_of(const FontDecoder& font, std::uint32_t code) {
    const auto it = font.widths.find(code);
    if (it != font.widths.end() && it->second > 0) {
        return it->second;
    }
    return font.default_width > 0 ? font.default_width : 500.0;
}

void load_simple_widths(Document& document, const Value& font_dict, FontDecoder& font) {
    std::int64_t first = 0;
    std::int64_t last = 255;
    dict_int(font_dict, "FirstChar", first);
    dict_int(font_dict, "LastChar", last);
    const Value* widths = dict_resolved(document, font_dict, "Widths");
    if (widths != nullptr && widths->type == ValueType::Array) {
        for (std::size_t i = 0; i < widths->array.size(); ++i) {
            if (widths->array[i].type != ValueType::Number) {
                continue;
            }
            const std::int64_t code = first + static_cast<std::int64_t>(i);
            if (code >= 0 && (last < first || code <= last)) {
                font.widths[static_cast<std::uint32_t>(code)] = widths->array[i].number;
            }
        }
    }
    const Value* descriptor = dict_resolved(document, font_dict, "FontDescriptor");
    if (descriptor != nullptr && descriptor->type == ValueType::Dict) {
        double missing = 0;
        if (dict_number(*descriptor, "MissingWidth", missing) && missing > 0) {
            font.default_width = missing;
        }
    }
}

void load_cid_widths(Document& document, const Value& font_dict, FontDecoder& font) {
    font.default_width = 1000;
    const Value* descendants = dict_resolved(document, font_dict, "DescendantFonts");
    if (descendants == nullptr || descendants->type != ValueType::Array || descendants->array.empty()) {
        return;
    }
    const Value* cid = resolve_value(document, &descendants->array[0]);
    if (cid == nullptr || cid->type != ValueType::Dict) {
        return;
    }
    double dw = 0;
    if (dict_number(*cid, "DW", dw) && dw > 0) {
        font.default_width = dw;
    }
    const Value* w = dict_resolved(document, *cid, "W");
    if (w == nullptr || w->type != ValueType::Array) {
        return;
    }
    const std::vector<Value>& items = w->array;
    for (std::size_t i = 0; i < items.size();) {
        if (items[i].type != ValueType::Number) {
            ++i;
            continue;
        }
        const std::uint32_t first = static_cast<std::uint32_t>(items[i].number);
        ++i;
        if (i >= items.size()) {
            break;
        }
        if (items[i].type == ValueType::Array) {
            std::uint32_t code = first;
            for (const Value& width : items[i].array) {
                if (width.type == ValueType::Number) {
                    font.widths[code] = width.number;
                }
                ++code;
            }
            ++i;
        } else if (items[i].type == ValueType::Number && i + 1 < items.size() &&
                   items[i + 1].type == ValueType::Number) {
            const std::uint32_t last = static_cast<std::uint32_t>(items[i].number);
            const double width = items[i + 1].number;
            for (std::uint32_t code = first; code <= last; ++code) {
                font.widths[code] = width;
            }
            i += 2;
        } else {
            ++i;
        }
    }
}

void finish_font_metrics(const Value& font_dict, FontDecoder& font) {
    std::string basefont;
    dict_name(font_dict, "BaseFont", basefont);
    if (font.widths.empty() && !basefont.empty()) {
        apply_base_font_widths(basefont, font);
    }
    for (int code = 0; code < 256; ++code) {
        if (font.encoding[code] == 0x20) {
            const auto it = font.widths.find(static_cast<std::uint32_t>(code));
            if (it != font.widths.end() && it->second > 0) {
                font.space_width = it->second;
            }
            break;
        }
    }
    if (font.space_width <= 0) {
        font.space_width = font.default_width > 0 ? std::min(font.default_width, 333.0) : 250.0;
    }
}

Error parse_tounicode(const std::string& cmap, FontDecoder& font) {
    Tokenizer tokens(Cursor{reinterpret_cast<const std::uint8_t*>(cmap.data()), cmap.size(), 0});
    std::string dest_hex;
    auto dest_utf8 = [](const std::string& bytes) {
        if (bytes.size() == 1) {
            std::string out;
            append_utf8(out, static_cast<unsigned char>(bytes[0]));
            return out;
        }
        return utf16be_to_utf8(bytes);
    };
    Token token;
    while (true) {
        Error err = tokens.next(token);
        if (!err.ok()) {
            return err;
        }
        if (token.kind == TokenKind::Eof) {
            break;
        }
        if (token.kind != TokenKind::Keyword) {
            continue;
        }
        if (token.text == "beginbfchar") {
            while (true) {
                Token src;
                err = tokens.next(src);
                if (!err.ok()) {
                    return err;
                }
                if (src.kind == TokenKind::Keyword && src.text == "endbfchar") {
                    break;
                }
                Token dst;
                err = tokens.next(dst);
                if (!err.ok()) {
                    return err;
                }
                if (src.kind == TokenKind::Hex && dst.kind == TokenKind::Hex) {
                    font.to_unicode[code_from_bytes(src.text)] = dest_utf8(dst.text);
                    font.code_width = std::max(font.code_width, static_cast<int>(src.text.size()));
                }
            }
        } else if (token.text == "beginbfrange") {
            while (true) {
                Token src1;
                err = tokens.next(src1);
                if (!err.ok()) {
                    return err;
                }
                if (src1.kind == TokenKind::Keyword && src1.text == "endbfrange") {
                    break;
                }
                Token src2;
                Token dst;
                err = tokens.next(src2);
                if (!err.ok()) {
                    return err;
                }
                err = tokens.next(dst);
                if (!err.ok()) {
                    return err;
                }
                if (src1.kind != TokenKind::Hex || src2.kind != TokenKind::Hex) {
                    continue;
                }
                const std::uint32_t start = code_from_bytes(src1.text);
                const std::uint32_t end = code_from_bytes(src2.text);
                font.code_width = std::max(font.code_width, static_cast<int>(src1.text.size()));
                if (dst.kind == TokenKind::Hex) {
                    std::string current = dst.text;
                    for (std::uint32_t code = start; code <= end; ++code) {
                        font.to_unicode[code] = dest_utf8(current);
                        if (!current.empty()) {
                            auto last = static_cast<unsigned char>(current.back());
                            if (last < 255) {
                                current.back() = static_cast<char>(last + 1);
                            }
                        }
                    }
                } else if (dst.kind == TokenKind::ArrayStart) {
                    std::uint32_t code = start;
                    while (code <= end) {
                        Token item;
                        err = tokens.next(item);
                        if (!err.ok()) {
                            return err;
                        }
                        if (item.kind == TokenKind::ArrayEnd) {
                            break;
                        }
                        if (item.kind == TokenKind::Hex) {
                            font.to_unicode[code] = dest_utf8(item.text);
                        }
                        ++code;
                    }
                }
            }
        } else if (token.text == "begincodespacerange") {
            Token lo;
            Token hi;
            if (tokens.next(lo).ok() && tokens.next(hi).ok() && lo.kind == TokenKind::Hex) {
                font.code_width = std::max(font.code_width, static_cast<int>(lo.text.size()));
            }
        }
    }
    font.has_tounicode = !font.to_unicode.empty();
    if (font.has_tounicode && font.code_width < 1) {
        font.code_width = 1;
    }
    return ok_error();
}

void apply_differences(const Value& encoding_dict, int encoding[256]) {
    const Value* diffs = dict_array(encoding_dict, "Differences");
    if (diffs == nullptr) {
        return;
    }
    int next = 0;
    for (const Value& item : diffs->array) {
        if (item.type == ValueType::Number) {
            next = static_cast<int>(item.number);
        } else if (item.type == ValueType::Name && next >= 0 && next < 256) {
            encoding[next] = glyph_unicode(item.text);
            ++next;
        }
    }
}

Error load_font(Document& document, const Value& font_dict, FontDecoder& font) {
    fill_win_ansi(font.encoding);
    std::string subtype;
    dict_name(font_dict, "Subtype", subtype);
    if (subtype == "Type0") {
        font.code_width = 2;
    }
    std::string encoding_name;
    if (dict_name(font_dict, "Encoding", encoding_name)) {
        if (encoding_name == "Identity-H" || encoding_name == "Identity-V") {
            font.code_width = 2;
        }
    } else {
        const Value* encoding_dict = dict_dict(font_dict, "Encoding");
        Ref encoding_ref;
        if (encoding_dict == nullptr && dict_ref(font_dict, "Encoding", encoding_ref)) {
            encoding_dict = document.object_dict(encoding_ref.number);
        }
        if (encoding_dict != nullptr) {
            std::string base;
            dict_name(*encoding_dict, "BaseEncoding", base);
            apply_differences(*encoding_dict, font.encoding);
        }
    }
    Ref to_unicode;
    if (dict_ref(font_dict, "ToUnicode", to_unicode)) {
        std::string cmap;
        Error err = document.decode_stream(to_unicode.number, cmap);
        if (err.ok()) {
            err = parse_tounicode(cmap, font);
            if (!err.ok()) {
                return err;
            }
        }
    }
    if (subtype == "Type0") {
        load_cid_widths(document, font_dict, font);
    } else {
        load_simple_widths(document, font_dict, font);
    }
    finish_font_metrics(font_dict, font);
    return ok_error();
}

void tidy_extracted(std::string& text) {
    std::string mapped;
    mapped.reserve(text.size());
    for (std::size_t i = 0; i < text.size();) {
        const unsigned char c0 = static_cast<unsigned char>(text[i]);
        if (c0 == 0xE2 && i + 2 < text.size() && static_cast<unsigned char>(text[i + 1]) == 0x90 &&
            static_cast<unsigned char>(text[i + 2]) == 0xA3) {
            mapped.push_back(' ');
            i += 3;
            continue;
        }
        if (c0 == 0xC2 && i + 1 < text.size() && static_cast<unsigned char>(text[i + 1]) == 0xA0) {
            mapped.push_back(' ');
            i += 2;
            continue;
        }
        mapped.push_back(text[i]);
        ++i;
    }
    std::string out;
    out.reserve(mapped.size());
    for (char ch : mapped) {
        if (ch == ' ' || ch == '\t') {
            if (!out.empty() && out.back() != ' ') {
                out.push_back(' ');
            }
            continue;
        }
        if ((ch == '.' || ch == ',' || ch == ';' || ch == ':' || ch == '!' || ch == '?' || ch == ')' || ch == ']' ||
             ch == '}') &&
            !out.empty() && out.back() == ' ') {
            out.pop_back();
        }
        out.push_back(ch);
    }
    while (!out.empty() && out.back() == ' ') {
        out.pop_back();
    }
    text = std::move(out);
}

void nfkc_arabic_forms(std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size();) {
        const unsigned char c0 = static_cast<unsigned char>(text[i]);
        if (c0 == 0xEF && i + 2 < text.size()) {
            const unsigned char c1 = static_cast<unsigned char>(text[i + 1]);
            const unsigned char c2 = static_cast<unsigned char>(text[i + 2]);
            const unsigned int cp = 0xF000 | ((c1 & 0x3F) << 6) | (c2 & 0x3F);
            unsigned int mapped = 0;
            if (cp >= 0xFE80 && cp <= 0xFEFC) {
                static const unsigned int kFe80[] = {
                    0x0621, 0x0622, 0x0622, 0x0623, 0x0623, 0x0624, 0x0624, 0x0625, 0x0625, 0x0626, 0x0626, 0x0626,
                    0x0626, 0x0627, 0x0627, 0x0628, 0x0628, 0x0628, 0x0628, 0x0629, 0x0629, 0x062A, 0x062A, 0x062A,
                    0x062A, 0x062B, 0x062B, 0x062B, 0x062B, 0x062C, 0x062C, 0x062C, 0x062C, 0x062D, 0x062D, 0x062D,
                    0x062D, 0x062E, 0x062E, 0x062E, 0x062E, 0x062F, 0x062F, 0x0630, 0x0630, 0x0631, 0x0631, 0x0632,
                    0x0632, 0x0633, 0x0633, 0x0633, 0x0633, 0x0634, 0x0634, 0x0634, 0x0634, 0x0635, 0x0635, 0x0635,
                    0x0635, 0x0636, 0x0636, 0x0636, 0x0636, 0x0637, 0x0637, 0x0637, 0x0637, 0x0638, 0x0638, 0x0638,
                    0x0638, 0x0639, 0x0639, 0x0639, 0x0639, 0x063A, 0x063A, 0x063A, 0x063A, 0x0641, 0x0641, 0x0641,
                    0x0641, 0x0642, 0x0642, 0x0642, 0x0642, 0x0643, 0x0643, 0x0643, 0x0643, 0x0644, 0x0644, 0x0644,
                    0x0644, 0x0645, 0x0645, 0x0645, 0x0645, 0x0646, 0x0646, 0x0646, 0x0646, 0x0647, 0x0647, 0x0647,
                    0x0647, 0x0648, 0x0648, 0x0649, 0x0649, 0x064A, 0x064A, 0x064A, 0x064A};
                const std::size_t idx = cp - 0xFE80;
                if (idx < sizeof(kFe80) / sizeof(kFe80[0])) {
                    mapped = kFe80[idx];
                }
            }
            if (mapped != 0) {
                append_utf8(out, mapped);
                i += 3;
                continue;
            }
        }
        out.push_back(text[i]);
        ++i;
    }
    text = std::move(out);
}

const Value* font_dict_from_resources(Document& document, const Value* resources, const std::string& name) {
    if (resources == nullptr) {
        return nullptr;
    }
    const Value* fonts = dict_dict(*resources, "Font");
    Ref fonts_ref;
    if (fonts == nullptr && dict_ref(*resources, "Font", fonts_ref)) {
        fonts = document.object_dict(fonts_ref.number);
    }
    if (fonts == nullptr) {
        return nullptr;
    }
    const Value* font = dict_get(*fonts, name.c_str());
    if (font == nullptr) {
        return nullptr;
    }
    if (font->type == ValueType::Ref) {
        return document.object_dict(font->ref.number);
    }
    if (font->type == ValueType::Dict) {
        return font;
    }
    return nullptr;
}

Error extract_page(Document& document, std::size_t index, std::string& text) {
    text.clear();
    std::string content;
    Error err = document.page_content(index, content);
    if (!err.ok()) {
        return err;
    }
    const Value* resources = document.page_inherited(index, "Resources");
    if (resources != nullptr && resources->type == ValueType::Ref) {
        resources = document.object_dict(resources->ref.number);
    }
    std::unordered_map<std::string, FontDecoder> fonts;
    FontDecoder* current = nullptr;
    FontDecoder fallback;
    fill_win_ansi(fallback.encoding);
    fallback.default_width = 500;
    fallback.space_width = 250;
    current = &fallback;
    double font_size = 11;
    double leading = 11;
    double tc = 0;
    double tw = 0;
    double tz = 100;
    Matrix ctm;
    Matrix text_matrix;
    Matrix text_line;
    std::vector<Matrix> graphics;
    std::vector<Span> spans;
    bool in_text = false;
    bool saw_image = false;

    auto ensure_font = [&](const std::string& name) -> Error {
        auto it = fonts.find(name);
        if (it != fonts.end()) {
            current = &it->second;
            return ok_error();
        }
        const Value* dict = font_dict_from_resources(document, resources, name);
        FontDecoder decoder;
        fill_win_ansi(decoder.encoding);
        if (dict != nullptr) {
            Error font_err = load_font(document, *dict, decoder);
            if (!font_err.ok()) {
                return font_err;
            }
        }
        current = &fonts.emplace(name, std::move(decoder)).first->second;
        return ok_error();
    };

    auto emit = [&](const std::string& raw) {
        if (raw.empty() || current == nullptr) {
            return;
        }
        const int width = current->code_width > 0 ? current->code_width : 1;
        const double scale = tz > 0 ? tz / 100.0 : 1.0;
        std::string decoded;
        decoded.reserve(raw.size());
        double dx = 0;
        for (std::size_t i = 0; i < raw.size();) {
            std::uint32_t code = 0;
            int used = 0;
            if (width >= 2 && i + 1 < raw.size()) {
                code = (static_cast<unsigned char>(raw[i]) << 8) | static_cast<unsigned char>(raw[i + 1]);
                used = 2;
            } else {
                code = static_cast<unsigned char>(raw[i]);
                used = 1;
            }
            i += static_cast<std::size_t>(used);
            std::string glyph;
            const auto it = current->to_unicode.find(code);
            if (it != current->to_unicode.end()) {
                glyph = it->second;
            } else if (used == 1 && code < 256 && current->encoding[code] > 0) {
                append_utf8(glyph, static_cast<unsigned int>(current->encoding[code]));
            }
            dx += (width_of(*current, code) / 1000.0) * font_size * scale + tc;
            if (is_space_text(glyph)) {
                dx += tw;
            }
            decoded += glyph;
        }
        const Matrix combined = multiply(ctm, text_matrix);
        double x = 0;
        double y = 0;
        transform(combined, 0, 0, x, y);
        text_matrix = multiply(Matrix{1, 0, 0, 1, dx, 0}, text_matrix);
        const Matrix combined_end = multiply(ctm, text_matrix);
        double x2 = 0;
        double y2 = 0;
        transform(combined_end, 0, 0, x2, y2);
        (void)y2;
        if (decoded.empty()) {
            return;
        }
        Span span;
        span.x = x;
        span.y = y;
        span.end_x = x2;
        span.size = font_size * std::hypot(combined.a, combined.b);
        if (span.size < 0.5) {
            span.size = std::max(font_size, 1.0);
        }
        span.space_width = (current->space_width / 1000.0) * span.size * scale;
        if (span.space_width < 0.5) {
            span.space_width = 0.25 * span.size;
        }
        span.text = std::move(decoded);
        spans.push_back(std::move(span));
    };

    Tokenizer tokens(Cursor{reinterpret_cast<const std::uint8_t*>(content.data()), content.size(), 0});
    std::vector<Token> stack;
    Token token;
    while (true) {
        err = tokens.next(token);
        if (!err.ok()) {
            return err;
        }
        if (token.kind == TokenKind::Eof) {
            break;
        }
        if (token.kind == TokenKind::Keyword && token.text != "true" && token.text != "false" &&
            token.text != "null" && token.text != "R") {
            const std::string& op = token.text;
            if (op == "BT") {
                in_text = true;
                text_matrix = Matrix{};
                text_line = text_matrix;
            } else if (op == "ET") {
                in_text = false;
            } else if (op == "q") {
                graphics.push_back(ctm);
            } else if (op == "Q") {
                if (!graphics.empty()) {
                    ctm = graphics.back();
                    graphics.pop_back();
                }
            } else if (op == "cm" && stack.size() >= 6) {
                Matrix m;
                m.a = operand_number(stack[stack.size() - 6]);
                m.b = operand_number(stack[stack.size() - 5]);
                m.c = operand_number(stack[stack.size() - 4]);
                m.d = operand_number(stack[stack.size() - 3]);
                m.e = operand_number(stack[stack.size() - 2]);
                m.f = operand_number(stack[stack.size() - 1]);
                ctm = multiply(m, ctm);
            } else if (op == "Tm" && stack.size() >= 6) {
                text_matrix.a = operand_number(stack[stack.size() - 6]);
                text_matrix.b = operand_number(stack[stack.size() - 5]);
                text_matrix.c = operand_number(stack[stack.size() - 4]);
                text_matrix.d = operand_number(stack[stack.size() - 3]);
                text_matrix.e = operand_number(stack[stack.size() - 2]);
                text_matrix.f = operand_number(stack[stack.size() - 1]);
                text_line = text_matrix;
            } else if ((op == "Td" || op == "TD") && stack.size() >= 2) {
                const double tx = operand_number(stack[stack.size() - 2]);
                const double ty = operand_number(stack[stack.size() - 1]);
                if (op == "TD") {
                    leading = -ty;
                }
                text_line = multiply(Matrix{1, 0, 0, 1, tx, ty}, text_line);
                text_matrix = text_line;
            } else if (op == "T*") {
                text_line = multiply(Matrix{1, 0, 0, 1, 0, -leading}, text_line);
                text_matrix = text_line;
            } else if (op == "TL" && !stack.empty()) {
                leading = operand_number(stack.back());
            } else if (op == "Tc" && !stack.empty()) {
                tc = operand_number(stack.back());
            } else if (op == "Tw" && !stack.empty()) {
                tw = operand_number(stack.back());
            } else if (op == "Tz" && !stack.empty()) {
                tz = operand_number(stack.back());
                if (tz <= 0) {
                    tz = 100;
                }
            } else if (op == "Tf" && stack.size() >= 2) {
                font_size = operand_number(stack.back());
                if (leading <= 0) {
                    leading = font_size;
                }
                std::string name;
                for (auto it = stack.rbegin(); it != stack.rend(); ++it) {
                    if (it->kind == TokenKind::Name) {
                        name = it->text;
                        break;
                    }
                }
                if (!name.empty()) {
                    err = ensure_font(name);
                    if (!err.ok()) {
                        return err;
                    }
                }
            } else if (op == "Tj" || op == "'" || op == "\"") {
                if (op == "\"" && stack.size() >= 3) {
                    tw = operand_number(stack[stack.size() - 3]);
                    tc = operand_number(stack[stack.size() - 2]);
                }
                if (op == "'" || op == "\"") {
                    text_line = multiply(Matrix{1, 0, 0, 1, 0, -leading}, text_line);
                    text_matrix = text_line;
                }
                if (!stack.empty() && (stack.back().kind == TokenKind::String || stack.back().kind == TokenKind::Hex)) {
                    emit(stack.back().text);
                }
            } else if (op == "TJ") {
                std::vector<Token> items;
                int depth = 0;
                for (const Token& item : stack) {
                    if (item.kind == TokenKind::ArrayStart) {
                        ++depth;
                        continue;
                    }
                    if (item.kind == TokenKind::ArrayEnd) {
                        --depth;
                        continue;
                    }
                    if (depth > 0) {
                        items.push_back(item);
                    }
                }
                if (items.empty()) {
                    items = stack;
                }
                for (const Token& item : items) {
                    if (item.kind == TokenKind::String || item.kind == TokenKind::Hex) {
                        emit(item.text);
                    } else if (item.kind == TokenKind::Number) {
                        const double kern = operand_number(item);
                        const double scale = tz > 0 ? tz / 100.0 : 1.0;
                        text_matrix =
                            multiply(Matrix{1, 0, 0, 1, -kern / 1000.0 * font_size * scale, 0}, text_matrix);
                    }
                }
            } else if (op == "Do") {
                saw_image = true;
            }
            stack.clear();
            continue;
        }
        stack.push_back(std::move(token));
        if (stack.size() > 8192) {
            stack.erase(stack.begin(), stack.begin() + static_cast<std::ptrdiff_t>(stack.size() - 4096));
        }
        (void)in_text;
    }

    if (spans.empty()) {
        if (saw_image) {
            text = "[scanned page " + std::to_string(index + 1) + ": no extractable text]";
        }
        return ok_error();
    }

    std::stable_sort(spans.begin(), spans.end(), [](const Span& a, const Span& b) {
        if (std::fabs(a.y - b.y) > 2.0) {
            return a.y > b.y;
        }
        return a.x < b.x;
    });

    std::string line;
    double line_y = spans.front().y;
    double line_size = spans.front().size;
    double last_end = spans.front().x;
    auto flush_line = [&]() {
        while (!line.empty() && (line.back() == ' ' || line.back() == '\t')) {
            line.pop_back();
        }
        if (!line.empty()) {
            if (!text.empty()) {
                text.push_back('\n');
            }
            nfkc_arabic_forms(line);
            tidy_extracted(line);
            text += line;
        }
        line.clear();
    };

    for (std::size_t i = 0; i < spans.size(); ++i) {
        const Span& span = spans[i];
        const double y_gap = std::fabs(span.y - line_y);
        const double line_thresh = std::max(2.0, 0.6 * std::max(line_size, span.size));
        if (i > 0 && y_gap > line_thresh) {
            flush_line();
            if (y_gap > 1.6 * std::max(line_size, span.size) && !text.empty()) {
                text += "\n";
            }
            line_y = span.y;
            last_end = span.x;
        }
        if (!line.empty()) {
            const double gap = span.x - last_end;
            const double user_size = std::max(span.size, line_size);
            // pdfio pdf2text treats |TJ| > 100 (0.1 em) as a word gap. Letter kerning in
            // TeX is typically < 0.08 em; justified word glue is often 0.20–0.35 em.
            // Do not raise this with the font's space glyph: some Type1 subsets store a
            // 0.5 em width at code 32 even though the file spaces words with ~0.24 em TJ.
            const double space_thresh = std::max(0.5, 0.12 * user_size);
            if (gap > space_thresh && line.back() != ' ' && !starts_with_punct(span.text) &&
                !is_space_text(span.text)) {
                line.push_back(' ');
            }
        }
        line += span.text;
        last_end = span.end_x;
        line_size = span.size;
        line_y = span.y;
    }
    flush_line();
    return ok_error();
}

}  // namespace

Error extract_markdown(Document& document, const Options& options, std::string& markdown) {
    markdown.clear();
    const std::size_t pages = document.page_count();
    if (pages == 0) {
        return {ErrorCode::FileRead, "PDF has no pages"};
    }
    std::size_t limit = pages;
    if (options.max_pages > 0 && options.max_pages < pages) {
        limit = options.max_pages;
    }
    for (std::size_t i = 0; i < limit; ++i) {
        if (options.cancellation.cancelled()) {
            return {ErrorCode::Cancelled, "PDF text extraction cancelled"};
        }
        std::string page;
        Error err = extract_page(document, i, page);
        if (!err.ok()) {
            if (!markdown.empty()) {
                while (!markdown.empty() && markdown.back() == '\n') {
                    markdown.pop_back();
                }
                markdown += "\n\n";
            }
            markdown += "[page " + std::to_string(i + 1) + ": " + err.message + "]";
            continue;
        }
        if (page.empty()) {
            page = "[scanned page " + std::to_string(i + 1) + ": no extractable text]";
        }
        if (!markdown.empty()) {
            while (!markdown.empty() && markdown.back() == '\n') {
                markdown.pop_back();
            }
            markdown += "\n\n";
        }
        markdown += page;
    }
    if (!markdown.empty() && markdown.back() != '\n') {
        markdown.push_back('\n');
    }
    return ok_error();
}

}  // namespace ainiux::pdf
