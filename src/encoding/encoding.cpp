#include "encoding/encoding.hpp"

#include <algorithm>
#include <new>
#include <utility>

namespace ainiux::encoding {
namespace {

std::string ascii_lower_copy(std::string text) {
    for (char& ch : text) {
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
    }
    return text;
}

std::string normalize_encoding_token(std::string text) {
    text = ascii_lower_copy(std::move(text));
    std::string out;
    out.reserve(text.size());
    for (char ch : text) {
        if (ch == '_' || ch == ' ' || ch == '.') {
            if (!out.empty() && out.back() != '-') {
                out.push_back('-');
            }
        } else {
            out.push_back(ch);
        }
    }
    while (!out.empty() && out.back() == '-') {
        out.pop_back();
    }
    return out;
}

bool has_nul(const std::string& bytes) {
    return bytes.find('\0') != std::string::npos;
}

std::uint16_t read_unit_le(const std::string& bytes, size_t offset) {
    const unsigned char lo = static_cast<unsigned char>(bytes[offset]);
    const unsigned char hi = static_cast<unsigned char>(bytes[offset + 1]);
    return static_cast<std::uint16_t>(lo | (static_cast<unsigned>(hi) << 8));
}

std::uint16_t read_unit_be(const std::string& bytes, size_t offset) {
    const unsigned char hi = static_cast<unsigned char>(bytes[offset]);
    const unsigned char lo = static_cast<unsigned char>(bytes[offset + 1]);
    return static_cast<std::uint16_t>(lo | (static_cast<unsigned>(hi) << 8));
}

bool is_high_surrogate(std::uint16_t unit) {
    return unit >= 0xD800 && unit <= 0xDBFF;
}

bool is_low_surrogate(std::uint16_t unit) {
    return unit >= 0xDC00 && unit <= 0xDFFF;
}

Error convert_utf16(const std::string& bytes, bool little_endian, std::string& utf8) {
    utf8.clear();
    if (bytes.size() % 2 != 0) {
        return {ErrorCode::UnsupportedFeature, "UTF-16 input has an odd number of bytes"};
    }
    try {
        utf8.reserve(bytes.size());
        size_t offset = 0;
        if (bytes.size() >= 2) {
            const std::uint16_t bom =
                little_endian ? read_unit_le(bytes, 0) : read_unit_be(bytes, 0);
            if (bom == 0xFEFF) {
                offset = 2;
            }
        }
        while (offset + 1 < bytes.size()) {
            const std::uint16_t unit =
                little_endian ? read_unit_le(bytes, offset) : read_unit_be(bytes, offset);
            offset += 2;
            if (is_high_surrogate(unit)) {
                if (offset + 1 >= bytes.size()) {
                    append_utf8_codepoint(utf8, 0xFFFD);
                    break;
                }
                const std::uint16_t next =
                    little_endian ? read_unit_le(bytes, offset) : read_unit_be(bytes, offset);
                if (!is_low_surrogate(next)) {
                    append_utf8_codepoint(utf8, 0xFFFD);
                    continue;
                }
                offset += 2;
                const std::uint32_t cp = 0x10000 +
                                         ((static_cast<std::uint32_t>(unit) - 0xD800) << 10) +
                                         (static_cast<std::uint32_t>(next) - 0xDC00);
                append_utf8_codepoint(utf8, cp);
                continue;
            }
            if (is_low_surrogate(unit)) {
                append_utf8_codepoint(utf8, 0xFFFD);
                continue;
            }
            append_utf8_codepoint(utf8, unit);
        }
    } catch (const std::bad_alloc&) {
        utf8.clear();
        return {ErrorCode::Internal, "not enough memory to convert UTF-16 to UTF-8"};
    }
    return ok_error();
}

bool looks_like_utf16(const std::string& bytes, bool little_endian) {
    if (bytes.size() < 8 || bytes.size() % 2 != 0) {
        return false;
    }
    const size_t units = bytes.size() / 2;
    size_t nul_on_high = 0;
    size_t unpaired = 0;
    for (size_t i = 0; i < bytes.size(); i += 2) {
        const std::uint16_t unit =
            little_endian ? read_unit_le(bytes, i) : read_unit_be(bytes, i);
        const unsigned char high = little_endian ? static_cast<unsigned char>(bytes[i + 1])
                                                 : static_cast<unsigned char>(bytes[i]);
        if (high == 0) {
            ++nul_on_high;
        }
        if (is_high_surrogate(unit)) {
            if (i + 3 >= bytes.size()) {
                ++unpaired;
            } else {
                const std::uint16_t next =
                    little_endian ? read_unit_le(bytes, i + 2) : read_unit_be(bytes, i + 2);
                if (!is_low_surrogate(next)) {
                    ++unpaired;
                }
            }
        } else if (is_low_surrogate(unit)) {
            ++unpaired;
        }
    }
    if (unpaired * 8 > units) {
        return false;
    }
    return nul_on_high * 10 >= units * 7;
}

std::string extract_charset_token(const std::string& lower_source) {
    const size_t pos = lower_source.find("charset");
    if (pos == std::string::npos) {
        return {};
    }
    size_t i = pos + 7;
    while (i < lower_source.size() &&
           (lower_source[i] == ' ' || lower_source[i] == '\t')) {
        ++i;
    }
    if (i >= lower_source.size() || lower_source[i] != '=') {
        return {};
    }
    ++i;
    while (i < lower_source.size() &&
           (lower_source[i] == ' ' || lower_source[i] == '\t')) {
        ++i;
    }
    if (i >= lower_source.size()) {
        return {};
    }
    char quote = 0;
    if (lower_source[i] == '"' || lower_source[i] == '\'') {
        quote = lower_source[i];
        ++i;
    }
    const size_t start = i;
    while (i < lower_source.size()) {
        const char ch = lower_source[i];
        if (quote != 0) {
            if (ch == quote) {
                break;
            }
        } else if (ch == ' ' || ch == '\t' || ch == ';' || ch == '"' || ch == '\'' ||
                   ch == '>') {
            break;
        }
        ++i;
    }
    if (start >= i) {
        return {};
    }
    std::string value = lower_source.substr(start, i - start);
    while (!value.empty() &&
           (value.back() == '"' || value.back() == '\'' || value.back() == ';')) {
        value.pop_back();
    }
    return value;
}

bool is_utf8_charset_name(const std::string& canonical) {
    return canonical == "utf-8" || canonical == "utf8" || canonical == "us-ascii" ||
           canonical == "ascii" || canonical == "ansi-x3-4-1968";
}

bool is_web_latin1_name(const std::string& canonical) {
    return canonical == "iso-8859-1" || canonical == "iso8859-1" || canonical == "latin1" ||
           canonical == "latin-1" || canonical == "iso-latin-1" ||
           canonical == "windows-1252" || canonical == "cp1252" ||
           canonical == "windows1252" || canonical == "ansi-x3-4-1968";
}

Error convert_utf8_maybe_bom(const std::string& bytes, std::string& utf8) {
    size_t start = 0;
    if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF &&
        static_cast<unsigned char>(bytes[1]) == 0xBB &&
        static_cast<unsigned char>(bytes[2]) == 0xBF) {
        start = 3;
    }
    const std::string slice = start == 0 ? bytes : bytes.substr(start);
    size_t offset = 0;
    if (!is_valid_utf8(slice, &offset)) {
        return {ErrorCode::UnsupportedFeature,
                "input is not valid UTF-8 (invalid byte at offset " +
                    std::to_string(offset + start) + ")"};
    }
    utf8 = slice;
    return ok_error();
}

}  // namespace

void append_utf8_codepoint(std::string& out, std::uint32_t cp) {
    if (cp <= 0x7F) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | ((cp >> 6) & 0x1F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | ((cp >> 12) & 0x0F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        if (cp > 0x10FFFF) {
            cp = 0xFFFD;
        }
        out.push_back(static_cast<char>(0xF0 | ((cp >> 18) & 0x07)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

bool is_valid_utf8(const std::string& input, size_t* error_offset) {
    auto fail = [&](size_t offset) {
        if (error_offset != nullptr) {
            *error_offset = offset;
        }
        return false;
    };
    auto is_continuation = [&](size_t offset) {
        if (offset >= input.size()) {
            return false;
        }
        const unsigned char ch = static_cast<unsigned char>(input[offset]);
        return ch >= 0x80 && ch <= 0xBF;
    };

    size_t i = 0;
    while (i < input.size()) {
        const unsigned char ch = static_cast<unsigned char>(input[i]);
        if (ch <= 0x7F) {
            ++i;
            continue;
        }
        if (ch >= 0xC2 && ch <= 0xDF) {
            if (!is_continuation(i + 1)) {
                return fail(i);
            }
            i += 2;
            continue;
        }
        if (ch == 0xE0) {
            if (i + 2 >= input.size()) {
                return fail(i);
            }
            const unsigned char b1 = static_cast<unsigned char>(input[i + 1]);
            if (b1 < 0xA0 || b1 > 0xBF || !is_continuation(i + 2)) {
                return fail(i);
            }
            i += 3;
            continue;
        }
        if ((ch >= 0xE1 && ch <= 0xEC) || (ch >= 0xEE && ch <= 0xEF)) {
            if (!is_continuation(i + 1) || !is_continuation(i + 2)) {
                return fail(i);
            }
            i += 3;
            continue;
        }
        if (ch == 0xED) {
            if (i + 2 >= input.size()) {
                return fail(i);
            }
            const unsigned char b1 = static_cast<unsigned char>(input[i + 1]);
            if (b1 < 0x80 || b1 > 0x9F || !is_continuation(i + 2)) {
                return fail(i);
            }
            i += 3;
            continue;
        }
        if (ch == 0xF0) {
            if (i + 3 >= input.size()) {
                return fail(i);
            }
            const unsigned char b1 = static_cast<unsigned char>(input[i + 1]);
            if (b1 < 0x90 || b1 > 0xBF || !is_continuation(i + 2) || !is_continuation(i + 3)) {
                return fail(i);
            }
            i += 4;
            continue;
        }
        if (ch >= 0xF1 && ch <= 0xF3) {
            if (!is_continuation(i + 1) || !is_continuation(i + 2) || !is_continuation(i + 3)) {
                return fail(i);
            }
            i += 4;
            continue;
        }
        if (ch == 0xF4) {
            if (i + 3 >= input.size()) {
                return fail(i);
            }
            const unsigned char b1 = static_cast<unsigned char>(input[i + 1]);
            if (b1 < 0x80 || b1 > 0x8F || !is_continuation(i + 2) || !is_continuation(i + 3)) {
                return fail(i);
            }
            i += 4;
            continue;
        }
        return fail(i);
    }
    if (error_offset != nullptr) {
        *error_offset = input.size();
    }
    return true;
}

const char* encoding_display_name(Encoding encoding) {
    switch (encoding) {
        case Encoding::Utf8:
            return "UTF-8";
        case Encoding::Utf16Le:
            return "UTF-16 LE";
        case Encoding::Utf16Be:
            return "UTF-16 BE";
        case Encoding::Windows1250:
            return "Windows-1250";
        case Encoding::Windows1251:
            return "Windows-1251";
        case Encoding::Windows1252:
            return "Windows-1252";
        case Encoding::Iso88591:
            return "ISO-8859-1";
        case Encoding::Iso88592:
            return "ISO-8859-2";
        case Encoding::Koi8r:
            return "KOI8-R";
        case Encoding::Koi8u:
            return "KOI8-U";
        case Encoding::External:
            return "external";
        case Encoding::Unknown:
            return "unknown";
    }
    return "unknown";
}

const char* encoding_canonical_name(Encoding encoding) {
    switch (encoding) {
        case Encoding::Utf8:
            return "utf-8";
        case Encoding::Utf16Le:
            return "utf-16le";
        case Encoding::Utf16Be:
            return "utf-16be";
        case Encoding::Windows1250:
            return "windows-1250";
        case Encoding::Windows1251:
            return "windows-1251";
        case Encoding::Windows1252:
            return "windows-1252";
        case Encoding::Iso88591:
            return "iso-8859-1";
        case Encoding::Iso88592:
            return "iso-8859-2";
        case Encoding::Koi8r:
            return "koi8-r";
        case Encoding::Koi8u:
            return "koi8-u";
        case Encoding::External:
            return "";
        case Encoding::Unknown:
            return "";
    }
    return "";
}

Error parse_encoding_name(const std::string& name, Encoding& out, std::string& canonical) {
    out = Encoding::Unknown;
    canonical.clear();
    const std::string token = normalize_encoding_token(name);
    if (token.empty()) {
        return {ErrorCode::BadArgs, "encoding name is empty"};
    }
    struct Alias {
        const char* token;
        Encoding encoding;
        const char* canonical;
    };
    static const Alias kAliases[] = {
        {"utf-8", Encoding::Utf8, "utf-8"},
        {"utf8", Encoding::Utf8, "utf-8"},
        {"us-ascii", Encoding::Utf8, "utf-8"},
        {"ascii", Encoding::Utf8, "utf-8"},
        {"utf-16", Encoding::Utf16Le, "utf-16le"},
        {"utf16", Encoding::Utf16Le, "utf-16le"},
        {"utf-16le", Encoding::Utf16Le, "utf-16le"},
        {"utf16le", Encoding::Utf16Le, "utf-16le"},
        {"utf-16be", Encoding::Utf16Be, "utf-16be"},
        {"utf16be", Encoding::Utf16Be, "utf-16be"},
        {"windows-1250", Encoding::Windows1250, "windows-1250"},
        {"cp1250", Encoding::Windows1250, "windows-1250"},
        {"windows1250", Encoding::Windows1250, "windows-1250"},
        {"windows-1251", Encoding::Windows1251, "windows-1251"},
        {"cp1251", Encoding::Windows1251, "windows-1251"},
        {"windows1251", Encoding::Windows1251, "windows-1251"},
        {"windows-1252", Encoding::Windows1252, "windows-1252"},
        {"cp1252", Encoding::Windows1252, "windows-1252"},
        {"windows1252", Encoding::Windows1252, "windows-1252"},
        {"iso-8859-1", Encoding::Iso88591, "iso-8859-1"},
        {"iso8859-1", Encoding::Iso88591, "iso-8859-1"},
        {"latin-1", Encoding::Iso88591, "iso-8859-1"},
        {"latin1", Encoding::Iso88591, "iso-8859-1"},
        {"iso-latin-1", Encoding::Iso88591, "iso-8859-1"},
        {"iso-8859-2", Encoding::Iso88592, "iso-8859-2"},
        {"iso8859-2", Encoding::Iso88592, "iso-8859-2"},
        {"latin-2", Encoding::Iso88592, "iso-8859-2"},
        {"latin2", Encoding::Iso88592, "iso-8859-2"},
        {"iso-latin-2", Encoding::Iso88592, "iso-8859-2"},
        {"koi8-r", Encoding::Koi8r, "koi8-r"},
        {"koi8r", Encoding::Koi8r, "koi8-r"},
        {"koi8-u", Encoding::Koi8u, "koi8-u"},
        {"koi8u", Encoding::Koi8u, "koi8-u"},
        {"gbk", Encoding::External, "gbk"},
        {"gb2312", Encoding::External, "gb2312"},
        {"gb18030", Encoding::External, "gb18030"},
        {"big5", Encoding::External, "big5"},
        {"shift-jis", Encoding::External, "shift_jis"},
        {"shiftjis", Encoding::External, "shift_jis"},
        {"sjis", Encoding::External, "shift_jis"},
        {"euc-jp", Encoding::External, "euc-jp"},
        {"eucjp", Encoding::External, "euc-jp"},
        {"euc-kr", Encoding::External, "euc-kr"},
        {"euckr", Encoding::External, "euc-kr"},
        {"cp949", Encoding::External, "euc-kr"},
    };
    for (const Alias& alias : kAliases) {
        if (token == alias.token) {
            out = alias.encoding;
            canonical = alias.canonical;
            return ok_error();
        }
    }
    return {ErrorCode::UnsupportedFeature,
            "unknown character encoding '" + name +
                "'; use utf-8, utf-16, windows-1250/1251/1252, iso-8859-1/2, koi8-r/u, "
                "or a CJK name such as gbk or big5"};
}

bool is_external_encoding_name(const std::string& canonical) {
    return canonical == "gbk" || canonical == "gb2312" || canonical == "gb18030" ||
           canonical == "big5" || canonical == "shift_jis" || canonical == "euc-jp" ||
           canonical == "euc-kr";
}

DetectedEncoding detect(const std::string& bytes) {
    DetectedEncoding found;
    if (bytes.empty()) {
        found.encoding = Encoding::Utf8;
        found.name = "utf-8";
        found.confident = true;
        return found;
    }
    if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF &&
        static_cast<unsigned char>(bytes[1]) == 0xBB &&
        static_cast<unsigned char>(bytes[2]) == 0xBF) {
        found.stripped_bom = true;
        found.encoding = Encoding::Utf8;
        found.name = "utf-8";
        found.confident = is_valid_utf8(bytes.substr(3));
        return found;
    }
    if (bytes.size() >= 4 && static_cast<unsigned char>(bytes[0]) == 0xFF &&
        static_cast<unsigned char>(bytes[1]) == 0xFE &&
        static_cast<unsigned char>(bytes[2]) == 0x00 &&
        static_cast<unsigned char>(bytes[3]) == 0x00) {
        return found;
    }
    if (bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xFF &&
        static_cast<unsigned char>(bytes[1]) == 0xFE) {
        found.encoding = Encoding::Utf16Le;
        found.name = "utf-16le";
        found.confident = true;
        found.stripped_bom = true;
        return found;
    }
    if (bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xFE &&
        static_cast<unsigned char>(bytes[1]) == 0xFF) {
        found.encoding = Encoding::Utf16Be;
        found.name = "utf-16be";
        found.confident = true;
        found.stripped_bom = true;
        return found;
    }
    if (is_valid_utf8(bytes) && !has_nul(bytes)) {
        found.encoding = Encoding::Utf8;
        found.name = "utf-8";
        found.confident = true;
        return found;
    }
    if (looks_like_utf16(bytes, true)) {
        found.encoding = Encoding::Utf16Le;
        found.name = "utf-16le";
        found.confident = true;
        return found;
    }
    if (looks_like_utf16(bytes, false)) {
        found.encoding = Encoding::Utf16Be;
        found.name = "utf-16be";
        found.confident = true;
        return found;
    }
    return found;
}

Error to_utf8(const std::string& bytes,
              Encoding encoding,
              std::string& utf8,
              const std::string& external_name,
              runtime::CancellationToken cancellation) {
    utf8.clear();
    if (cancellation.cancelled()) {
        return {ErrorCode::Cancelled, "character encoding conversion cancelled"};
    }
    switch (encoding) {
        case Encoding::Utf8:
            return convert_utf8_maybe_bom(bytes, utf8);
        case Encoding::Utf16Le:
            return convert_utf16(bytes, true, utf8);
        case Encoding::Utf16Be:
            return convert_utf16(bytes, false, utf8);
        case Encoding::Windows1250:
        case Encoding::Windows1251:
        case Encoding::Windows1252:
        case Encoding::Iso88591:
        case Encoding::Iso88592:
        case Encoding::Koi8r:
        case Encoding::Koi8u:
            return convert_sbcs(bytes, encoding, utf8);
        case Encoding::External:
            return convert_external(bytes, external_name, utf8, cancellation);
        case Encoding::Unknown:
            break;
    }
    return {ErrorCode::UnsupportedFeature, "unknown character encoding"};
}

Error to_utf8_named(const std::string& bytes,
                    const std::string& name,
                    std::string& utf8,
                    runtime::CancellationToken cancellation) {
    Encoding encoding = Encoding::Unknown;
    std::string canonical;
    Error err = parse_encoding_name(name, encoding, canonical);
    if (!err.ok()) {
        return err;
    }
    return to_utf8(bytes, encoding, utf8, canonical, cancellation);
}

std::string charset_from_content_type(const std::string& content_type) {
    return extract_charset_token(ascii_lower_copy(content_type));
}

std::string charset_from_html_meta(const std::string& body) {
    const size_t scan = std::min(body.size(), static_cast<size_t>(8192));
    const std::string head = ascii_lower_copy(body.substr(0, scan));
    size_t pos = 0;
    while (pos < head.size()) {
        const size_t meta = head.find("<meta", pos);
        if (meta == std::string::npos) {
            break;
        }
        const size_t end = head.find('>', meta);
        if (end == std::string::npos) {
            break;
        }
        const std::string tag = head.substr(meta, end - meta + 1);
        std::string found = extract_charset_token(tag);
        if (!found.empty()) {
            return found;
        }
        if (tag.find("content-type") != std::string::npos) {
            const size_t content = tag.find("content=");
            if (content != std::string::npos) {
                found = extract_charset_token(tag.substr(content));
                if (!found.empty()) {
                    return found;
                }
            }
        }
        pos = end + 1;
    }
    return {};
}

Error decode_incoming_text(const std::string& bytes,
                           const DecodeOptions& options,
                           std::string& utf8,
                           DetectedEncoding& used,
                           runtime::CancellationToken cancellation) {
    utf8.clear();
    used = {};
    if (cancellation.cancelled()) {
        return {ErrorCode::Cancelled, "character encoding conversion cancelled"};
    }
    if (!options.encoding_name.empty()) {
        Error err = to_utf8_named(bytes, options.encoding_name, utf8, cancellation);
        if (!err.ok()) {
            return err;
        }
        Encoding parsed = Encoding::Unknown;
        parse_encoding_name(options.encoding_name, parsed, used.name);
        used.encoding = parsed;
        used.confident = true;
        return ok_error();
    }

    used = detect(bytes);
    if (used.confident) {
        return to_utf8(bytes, used.encoding, utf8, used.name, cancellation);
    }

    std::string declared;
    if (!options.content_type.empty()) {
        declared = charset_from_content_type(options.content_type);
    }
    if (declared.empty() && options.html_hints) {
        declared = charset_from_html_meta(bytes);
    }
    if (!declared.empty()) {
        Encoding parsed = Encoding::Unknown;
        std::string canonical;
        Error parse_err = parse_encoding_name(declared, parsed, canonical);
        if (parse_err.ok()) {
            if (options.html_hints && is_web_latin1_name(canonical)) {
                parsed = Encoding::Windows1252;
                canonical = "windows-1252";
            }
            if (is_utf8_charset_name(canonical) && options.allow_unlabeled_legacy) {
                used.encoding = Encoding::Windows1252;
                used.name = "windows-1252";
                return to_utf8(bytes, Encoding::Windows1252, utf8, {}, cancellation);
            }
            used.encoding = parsed;
            used.name = canonical;
            used.confident = true;
            return to_utf8(bytes, parsed, utf8, canonical, cancellation);
        }
        if (is_external_encoding_name(normalize_encoding_token(declared))) {
            return parse_err;
        }
        if (!options.allow_unlabeled_legacy) {
            return {ErrorCode::UnsupportedFeature,
                    "unknown character encoding '" + declared +
                        "'; convert the document to UTF-8 or pass --encoding NAME"};
        }
    }

    if (is_valid_utf8(bytes) && !has_nul(bytes)) {
        utf8 = bytes;
        used.encoding = Encoding::Utf8;
        used.name = "utf-8";
        used.confident = true;
        return ok_error();
    }

    if (options.allow_unlabeled_legacy) {
        used.encoding = Encoding::Windows1252;
        used.name = "windows-1252";
        return to_utf8(bytes, Encoding::Windows1252, utf8, {}, cancellation);
    }

    size_t offset = 0;
    is_valid_utf8(bytes, &offset);
    if (has_nul(bytes) && !used.confident) {
        const size_t nul = bytes.find('\0');
        return {ErrorCode::UnsupportedFeature,
                "input appears to be binary: contains a NUL byte at offset " +
                    std::to_string(nul) +
                    ". If this is UTF-16 or another encoding, pass --encoding NAME"};
    }
    return {ErrorCode::UnsupportedFeature,
            "input is not valid UTF-8 (invalid byte at offset " + std::to_string(offset) +
                "). Convert the document to UTF-8 or pass --encoding NAME "
                "(windows-1252, koi8-r, utf-16, …)"};
}

Error decode_web_bytes(const std::string& bytes,
                       const std::string& content_type,
                       std::string& utf8,
                       runtime::CancellationToken cancellation) {
    DecodeOptions options;
    options.content_type = content_type;
    options.html_hints = true;
    options.allow_unlabeled_legacy = true;
    DetectedEncoding used;
    return decode_incoming_text(bytes, options, utf8, used, cancellation);
}

std::vector<EncodingChoice> encoding_picker_choices() {
    return {
        {"UTF-8", "utf-8"},
        {"UTF-16 LE", "utf-16le"},
        {"UTF-16 BE", "utf-16be"},
        {"Windows-1250 (Central European)", "windows-1250"},
        {"Windows-1251 (Cyrillic)", "windows-1251"},
        {"Windows-1252 (Western)", "windows-1252"},
        {"ISO-8859-1 (Latin-1)", "iso-8859-1"},
        {"ISO-8859-2 (Latin-2)", "iso-8859-2"},
        {"KOI8-R", "koi8-r"},
        {"KOI8-U", "koi8-u"},
        {"GBK", "gbk"},
        {"GB2312", "gb2312"},
        {"GB18030", "gb18030"},
        {"Big5", "big5"},
        {"Shift_JIS", "shift_jis"},
        {"EUC-JP", "euc-jp"},
        {"EUC-KR", "euc-kr"},
        {"Open as-is", ""},
    };
}

std::vector<std::string> builtin_encoding_labels() {
    std::vector<std::string> labels;
    for (const EncodingChoice& choice : encoding_picker_choices()) {
        if (choice.name.empty() || is_external_encoding_name(choice.name)) {
            if (choice.name.empty()) {
                labels.push_back(choice.label);
            }
            continue;
        }
        labels.push_back(choice.label);
    }
    return labels;
}

std::vector<std::string> all_encoding_labels() {
    std::vector<std::string> labels;
    for (const EncodingChoice& choice : encoding_picker_choices()) {
        labels.push_back(choice.label);
    }
    return labels;
}

}  // namespace ainiux::encoding
