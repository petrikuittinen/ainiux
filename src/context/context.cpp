#include "context/context.hpp"

#include <algorithm>
#include <cctype>
#include <limits>

namespace pkchat::context {
namespace {

size_t message_bytes(const provider::Message& message) {
    return message.role.size() + message.content.size() + 16;
}

long long saturating_add(long long left, long long right) {
    if (right > std::numeric_limits<long long>::max() - left) {
        return std::numeric_limits<long long>::max();
    }
    return left + right;
}

long long estimated_content_tokens(const std::string& text) {
    long long ascii_bytes = 0;
    long long non_ascii_codepoints = 0;
    for (size_t i = 0; i < text.size();) {
        const unsigned char ch = static_cast<unsigned char>(text[i]);
        if (ch < 0x80U) {
            ++ascii_bytes;
            ++i;
            continue;
        }
        ++non_ascii_codepoints;
        ++i;
        size_t continuations = 0;
        while (i < text.size() && continuations < 3 &&
               (static_cast<unsigned char>(text[i]) & 0xC0U) == 0x80U) {
            ++i;
            ++continuations;
        }
    }
    const long long ascii_tokens = ascii_bytes / 4 + (ascii_bytes % 4 == 0 ? 0 : 1);
    return saturating_add(ascii_tokens, non_ascii_codepoints);
}

std::string compact_whitespace(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    bool pending_space = false;
    for (unsigned char ch : text) {
        if (std::isspace(ch)) {
            pending_space = !out.empty();
        } else {
            if (pending_space) {
                out.push_back(' ');
            }
            out.push_back(static_cast<char>(ch));
            pending_space = false;
        }
    }
    return out;
}

std::string make_summary(const std::vector<provider::Message>& removed, size_t max_content_bytes) {
    std::string summary = "Context summary of " + std::to_string(removed.size()) + " compacted messages:";
    for (const provider::Message& message : removed) {
        std::string line = "\n" + message.role + ": " + compact_whitespace(message.content);
        if (summary.size() + line.size() <= max_content_bytes) {
            summary += line;
            continue;
        }
        if (summary.size() + message.role.size() + 8 < max_content_bytes) {
            const size_t remaining = max_content_bytes - summary.size();
            if (line.size() > remaining) {
                size_t valid = remaining;
                while (valid > 0 && valid < line.size() &&
                       (static_cast<unsigned char>(line[valid]) & 0xc0U) == 0x80U) {
                    --valid;
                }
                line.resize(valid);
            }
            summary += line;
        }
        break;
    }
    if (summary.size() > max_content_bytes) {
        summary.resize(max_content_bytes);
    }
    return summary;
}

PreparedMessages too_large_error(const std::vector<provider::Message>& messages,
                                 const std::string& policy,
                                 size_t max_bytes,
                                 size_t actual) {
    PreparedMessages result;
    result.messages = messages;
    result.error = {ErrorCode::UnsupportedFeature,
                    "request context is approximately " + std::to_string(actual) +
                        " text bytes, exceeding --max-context-bytes " + std::to_string(max_bytes) +
                        " under policy " + policy +
                        ". Increase the limit or choose a compaction policy."};
    return result;
}

}  // namespace

size_t estimated_text_bytes(const std::vector<provider::Message>& messages) {
    size_t total = 0;
    for (const provider::Message& message : messages) {
        total += message_bytes(message);
    }
    return total;
}

long long estimated_text_tokens(const std::vector<provider::Message>& messages) {
    long long total = 3;
    for (const provider::Message& message : messages) {
        total = saturating_add(total, 4);
        total = saturating_add(total, estimated_content_tokens(message.content));
    }
    return total;
}

PreparedMessages prepare(const std::vector<provider::Message>& messages,
                         const std::string& policy,
                         size_t max_bytes) {
    PreparedMessages result;
    result.messages = messages;
    const size_t original_bytes = estimated_text_bytes(messages);
    if (max_bytes == 0 || original_bytes <= max_bytes || policy == "provider-auto") {
        return result;
    }
    if (policy != "error" && policy != "truncate-oldest" && policy != "summarize-oldest" &&
        policy != "summarize-middle") {
        result.error = {ErrorCode::BadArgs, "unknown context policy: " + policy};
        return result;
    }
    if (policy == "error") {
        return too_large_error(messages, policy, max_bytes, original_bytes);
    }

    std::vector<bool> keep(messages.size(), true);
    size_t kept_bytes = original_bytes;
    size_t last_non_system = messages.size();
    for (size_t i = messages.size(); i > 0; --i) {
        if (messages[i - 1].role != "system") {
            last_non_system = i - 1;
            break;
        }
    }

    std::vector<size_t> candidates;
    for (size_t i = 0; i < messages.size(); ++i) {
        if (messages[i].role != "system" && i != last_non_system) {
            candidates.push_back(i);
        }
    }
    if (policy == "summarize-middle" && !candidates.empty()) {
        candidates.erase(candidates.begin());
        const size_t center = messages.size() / 2;
        std::stable_sort(candidates.begin(), candidates.end(), [center](size_t a, size_t b) {
            const size_t da = a > center ? a - center : center - a;
            const size_t db = b > center ? b - center : center - b;
            return da < db;
        });
    }

    const bool summarize = policy == "summarize-oldest" || policy == "summarize-middle";
    const size_t minimum_summary_bytes = summarize ? 96 : 0;
    std::vector<size_t> removed_indices;
    for (size_t index : candidates) {
        if (kept_bytes + minimum_summary_bytes <= max_bytes) {
            break;
        }
        keep[index] = false;
        kept_bytes -= message_bytes(messages[index]);
        removed_indices.push_back(index);
    }
    if (removed_indices.empty() || kept_bytes + minimum_summary_bytes > max_bytes) {
        return too_large_error(messages, policy, max_bytes, original_bytes);
    }
    std::sort(removed_indices.begin(), removed_indices.end());

    std::vector<provider::Message> prepared;
    prepared.reserve(messages.size() - removed_indices.size() + (summarize ? 1 : 0));
    size_t summary_position = messages.size();
    if (summarize) {
        summary_position = removed_indices.front();
    }
    std::vector<provider::Message> removed;
    for (size_t i = 0; i < messages.size(); ++i) {
        if (!keep[i]) {
            removed.push_back(messages[i]);
            continue;
        }
        prepared.push_back(messages[i]);
    }

    if (summarize) {
        size_t insert_at = 0;
        for (size_t i = 0; i < summary_position; ++i) {
            if (keep[i]) {
                ++insert_at;
            }
        }
        const size_t available = max_bytes - kept_bytes;
        const size_t overhead = message_bytes(provider::Message{"system", ""});
        if (available <= overhead) {
            return too_large_error(messages, policy, max_bytes, original_bytes);
        }
        provider::Message summary{"system", make_summary(removed, available - overhead)};
        prepared.insert(prepared.begin() + static_cast<long>(insert_at), std::move(summary));
    }

    result.messages = std::move(prepared);
    result.compacted = true;
    result.event.policy = policy;
    result.event.messages_compacted = removed_indices.size();
    result.event.original_bytes = original_bytes;
    result.event.request_bytes = estimated_text_bytes(result.messages);
    result.event.notice = "Context compacted: " + std::to_string(removed_indices.size()) +
                          " earlier messages " + (summarize ? "summarized" : "removed") +
                          " for the provider request. Full transcript preserved on disk.";
    return result;
}

}  // namespace pkchat::context
