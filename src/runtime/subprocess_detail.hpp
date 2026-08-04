#pragma once

#include <algorithm>
#include <cstddef>
#include <string>

namespace ainiux::runtime::detail {

inline void append_bounded(std::string& output,
                           const char* data,
                           std::size_t count,
                           std::size_t limit,
                           bool& truncated) {
    const std::size_t remaining = output.size() < limit ? limit - output.size() : 0;
    const std::size_t accepted = std::min(remaining, count);
    output.append(data, accepted);
    if (accepted != count) truncated = true;
}

inline void normalize_newlines(std::string& text) {
    std::size_t write = 0;
    for (std::size_t read = 0; read < text.size(); ++read) {
        if (text[read] == '\r') {
            if (read + 1 < text.size() && text[read + 1] == '\n') ++read;
            text[write++] = '\n';
        } else {
            text[write++] = text[read];
        }
    }
    text.resize(write);
}

// Child programs sit outside Ainiux's encoding boundary. Preserve valid UTF-8
// exactly and replace malformed sequences, including a capture cut mid-codepoint.
inline void normalize_utf8(std::string& text,
                           std::size_t limit,
                           bool& truncated,
                           bool& repaired) {
    std::string output;
    output.reserve(text.size());
    const auto continuation = [](unsigned char value) {
        return (value & 0xC0U) == 0x80U;
    };
    for (std::size_t offset = 0; offset < text.size();) {
        const unsigned char first = static_cast<unsigned char>(text[offset]);
        std::size_t length = 0;
        bool valid = false;
        if (first <= 0x7FU) {
            length = 1;
            valid = true;
        } else if (first >= 0xC2U && first <= 0xDFU && offset + 1 < text.size()) {
            length = 2;
            valid = continuation(static_cast<unsigned char>(text[offset + 1]));
        } else if (first >= 0xE0U && first <= 0xEFU && offset + 2 < text.size()) {
            const unsigned char second = static_cast<unsigned char>(text[offset + 1]);
            const unsigned char third = static_cast<unsigned char>(text[offset + 2]);
            length = 3;
            valid = continuation(second) && continuation(third) &&
                    (first != 0xE0U || second >= 0xA0U) &&
                    (first != 0xEDU || second <= 0x9FU);
        } else if (first >= 0xF0U && first <= 0xF4U && offset + 3 < text.size()) {
            const unsigned char second = static_cast<unsigned char>(text[offset + 1]);
            length = 4;
            valid = continuation(second) &&
                    continuation(static_cast<unsigned char>(text[offset + 2])) &&
                    continuation(static_cast<unsigned char>(text[offset + 3])) &&
                    (first != 0xF0U || second >= 0x90U) &&
                    (first != 0xF4U || second <= 0x8FU);
        }
        if (valid) {
            if (output.size() + length > limit) {
                truncated = true;
                break;
            }
            output.append(text, offset, length);
            offset += length;
        } else {
            repaired = true;
            if (output.size() + 3U > limit) {
                truncated = true;
                break;
            }
            output.append("\xEF\xBF\xBD", 3);
            ++offset;
        }
    }
    text.swap(output);
}

inline void normalize_output(std::string& text,
                             std::size_t limit,
                             bool& truncated,
                             bool& repaired) {
    normalize_newlines(text);
    normalize_utf8(text, limit, truncated, repaired);
}

}  // namespace ainiux::runtime::detail
