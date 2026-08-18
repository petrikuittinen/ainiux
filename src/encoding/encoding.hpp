#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "common.hpp"
#include "runtime/runtime.hpp"

namespace ainiux::encoding {

enum class Encoding {
    Unknown,
    Utf8,
    Utf16Le,
    Utf16Be,
    Windows1250,
    Windows1251,
    Windows1252,
    Iso88591,
    Iso88592,
    Koi8r,
    Koi8u,
    External,
};

struct DetectedEncoding {
    Encoding encoding = Encoding::Unknown;
    std::string name;
    bool confident = false;
    bool stripped_bom = false;
};

struct DecodeOptions {
    std::string encoding_name;
    std::string content_type;
    bool html_hints = false;
    bool allow_unlabeled_legacy = false;
};

struct EncodingChoice {
    std::string label;
    std::string name;
};

bool is_valid_utf8(const std::string& input, size_t* error_offset = nullptr);
void append_utf8_codepoint(std::string& out, std::uint32_t cp);

const char* encoding_display_name(Encoding encoding);
const char* encoding_canonical_name(Encoding encoding);

Error parse_encoding_name(const std::string& name, Encoding& out, std::string& canonical);
bool is_external_encoding_name(const std::string& canonical);

DetectedEncoding detect(const std::string& bytes);

Error to_utf8(const std::string& bytes,
              Encoding encoding,
              std::string& utf8,
              const std::string& external_name = {},
              runtime::CancellationToken cancellation = runtime::CancellationToken());

Error to_utf8_named(const std::string& bytes,
                    const std::string& name,
                    std::string& utf8,
                    runtime::CancellationToken cancellation = runtime::CancellationToken());

// Content-Type / HTML meta, then conversion. Valid UTF-8 is unchanged.
// Declared ISO-8859-1 on HTML uses Windows-1252 (WHATWG). Unlabeled or
// mislabeled invalid UTF-8 falls back to Windows-1252 when
// allow_unlabeled_legacy is true (fetch JSON safety).
Error decode_incoming_text(const std::string& bytes,
                           const DecodeOptions& options,
                           std::string& utf8,
                           DetectedEncoding& used,
                           runtime::CancellationToken cancellation = runtime::CancellationToken());

Error decode_web_bytes(const std::string& bytes,
                       const std::string& content_type,
                       std::string& utf8,
                       runtime::CancellationToken cancellation = runtime::CancellationToken());

std::string charset_from_content_type(const std::string& content_type);
std::string charset_from_html_meta(const std::string& body);

std::vector<EncodingChoice> encoding_picker_choices();
std::vector<std::string> builtin_encoding_labels();
std::vector<std::string> all_encoding_labels();

// Built-in SBCS and optional CJK helpers. Callers should use to_utf8.
Error convert_sbcs(const std::string& bytes, Encoding encoding, std::string& utf8);
Error convert_external(const std::string& bytes,
                       const std::string& canonical,
                       std::string& utf8,
                       runtime::CancellationToken cancellation);

}  // namespace ainiux::encoding
