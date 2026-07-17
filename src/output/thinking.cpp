#include "output/thinking.hpp"

#include <algorithm>

namespace ainiux::output {
namespace {

constexpr const char* kOpenTag = "<think>";
constexpr const char* kCloseTag = "</think>";

char lower_ascii(char ch) {
    if (ch >= 'A' && ch <= 'Z') {
        return static_cast<char>(ch - 'A' + 'a');
    }
    return ch;
}

bool matches_at(const std::string& text, size_t position, const std::string& tag) {
    if (position + tag.size() > text.size()) {
        return false;
    }
    for (size_t i = 0; i < tag.size(); ++i) {
        if (lower_ascii(text[position + i]) != tag[i]) {
            return false;
        }
    }
    return true;
}

size_t find_tag(const std::string& text, const std::string& tag) {
    for (size_t position = 0; position + tag.size() <= text.size(); ++position) {
        if (matches_at(text, position, tag)) {
            return position;
        }
    }
    return std::string::npos;
}

size_t possible_tag_prefix_length(const std::string& text, const std::string& tag) {
    const size_t maximum = std::min(text.size(), tag.size() - 1);
    for (size_t length = maximum; length > 0; --length) {
        bool matches = true;
        for (size_t i = 0; i < length; ++i) {
            if (lower_ascii(text[text.size() - length + i]) != tag[i]) {
                matches = false;
                break;
            }
        }
        if (matches) {
            return length;
        }
    }
    return 0;
}

}  // namespace

ThinkingChunk ThinkingTraceSplitter::feed(const std::string& text) {
    pending_ += text;
    ThinkingChunk chunk;

    while (!pending_.empty()) {
        const std::string tag = in_trace_ ? kCloseTag : kOpenTag;
        const size_t position = find_tag(pending_, tag);
        if (position != std::string::npos) {
            std::string prefix = pending_.substr(0, position);
            if (in_trace_) {
                chunk.trace += prefix;
                chunk.trace += pending_.substr(position, tag.size());
                in_trace_ = false;
                suppress_leading_newlines_ = !saw_visible_;
            } else {
                if (suppress_leading_newlines_) {
                    const size_t first = prefix.find_first_not_of("\r\n");
                    prefix = first == std::string::npos ? std::string() : prefix.substr(first);
                }
                if (!prefix.empty()) {
                    chunk.visible += prefix;
                    saw_visible_ = true;
                    suppress_leading_newlines_ = false;
                }
                chunk.trace += pending_.substr(position, tag.size());
                in_trace_ = true;
            }
            pending_.erase(0, position + tag.size());
            continue;
        }

        const size_t retained = possible_tag_prefix_length(pending_, tag);
        std::string ready = pending_.substr(0, pending_.size() - retained);
        pending_.erase(0, pending_.size() - retained);
        if (in_trace_) {
            chunk.trace += ready;
        } else {
            if (suppress_leading_newlines_) {
                const size_t first = ready.find_first_not_of("\r\n");
                ready = first == std::string::npos ? std::string() : ready.substr(first);
            }
            if (!ready.empty()) {
                chunk.visible += ready;
                saw_visible_ = true;
                suppress_leading_newlines_ = false;
            }
        }
        break;
    }
    return chunk;
}

ThinkingChunk ThinkingTraceSplitter::finish() {
    ThinkingChunk chunk;
    if (in_trace_) {
        chunk.trace = std::move(pending_);
    } else {
        if (suppress_leading_newlines_) {
            const size_t first = pending_.find_first_not_of("\r\n");
            pending_ = first == std::string::npos ? std::string() : pending_.substr(first);
        }
        chunk.visible = std::move(pending_);
    }
    pending_.clear();
    return chunk;
}

ThinkingChunk split_thinking_traces(const std::string& text) {
    ThinkingTraceSplitter splitter;
    ThinkingChunk result = splitter.feed(text);
    ThinkingChunk final = splitter.finish();
    result.visible += final.visible;
    result.trace += final.trace;
    return result;
}

}  // namespace ainiux::output
