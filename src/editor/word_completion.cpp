#include "editor/word_completion.hpp"

#include "editor/detail/unicode_word_data.hpp"
#include "editor/editor.hpp"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <iterator>
#include <limits>
#include <new>
#include <sstream>
#include <utility>

namespace pkchat::editor {
namespace {

struct Decoded {
    std::uint32_t codepoint = 0;
    size_t length = 1;
    bool valid = false;
};

Decoded decode_at(const std::string& text, size_t position) {
    if (position >= text.size()) {
        return {};
    }
    const unsigned char first = static_cast<unsigned char>(text[position]);
    if (first < 0x80U) {
        return {first, 1, true};
    }
    size_t length = 0;
    std::uint32_t value = 0;
    std::uint32_t minimum = 0;
    if ((first & 0xE0U) == 0xC0U) {
        length = 2;
        value = first & 0x1FU;
        minimum = 0x80U;
    } else if ((first & 0xF0U) == 0xE0U) {
        length = 3;
        value = first & 0x0FU;
        minimum = 0x800U;
    } else if ((first & 0xF8U) == 0xF0U) {
        length = 4;
        value = first & 0x07U;
        minimum = 0x10000U;
    } else {
        return {};
    }
    if (position + length > text.size()) {
        return {};
    }
    for (size_t index = 1; index < length; ++index) {
        const unsigned char byte = static_cast<unsigned char>(text[position + index]);
        if ((byte & 0xC0U) != 0x80U) {
            return {};
        }
        value = (value << 6U) | static_cast<std::uint32_t>(byte & 0x3FU);
    }
    if (value < minimum || value > 0x10FFFFU ||
        (value >= 0xD800U && value <= 0xDFFFU)) {
        return {};
    }
    return {value, length, true};
}

size_t previous_start(const std::string& text, size_t position) {
    position = std::min(position, text.size());
    if (position == 0) {
        return 0;
    }
    size_t start = position - 1;
    size_t continuation_count = 0;
    while (start > 0 && continuation_count < 3 &&
           (static_cast<unsigned char>(text[start]) & 0xC0U) == 0x80U) {
        --start;
        ++continuation_count;
    }
    const Decoded decoded = decode_at(text, start);
    return decoded.valid && start + decoded.length == position ? start : position - 1;
}

template <size_t N>
bool in_ranges(std::uint32_t value, const unicode_data::UnicodeRange (&ranges)[N]) {
    size_t first = 0;
    size_t last = N;
    while (first < last) {
        const size_t middle = first + (last - first) / 2;
        if (value < ranges[middle].first) {
            last = middle;
        } else if (value > ranges[middle].last) {
            first = middle + 1;
        } else {
            return true;
        }
    }
    return false;
}

bool is_word_codepoint(const Decoded& decoded) {
    return decoded.valid &&
           (decoded.codepoint == '_' ||
            in_ranges(decoded.codepoint, unicode_data::kWordRanges));
}

bool is_uppercase(std::uint32_t codepoint) {
    return in_ranges(codepoint, unicode_data::kUppercaseRanges);
}

void append_utf8(std::string& output, std::uint32_t codepoint) {
    if (codepoint <= 0x7FU) {
        output.push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7FFU) {
        output.push_back(static_cast<char>(0xC0U | (codepoint >> 6U)));
        output.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
    } else if (codepoint <= 0xFFFFU) {
        output.push_back(static_cast<char>(0xE0U | (codepoint >> 12U)));
        output.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU)));
        output.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
    } else {
        output.push_back(static_cast<char>(0xF0U | (codepoint >> 18U)));
        output.push_back(static_cast<char>(0x80U | ((codepoint >> 12U) & 0x3FU)));
        output.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU)));
        output.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
    }
}

const unicode_data::FoldMapping* fold_mapping(std::uint32_t codepoint) {
    const auto* first = std::begin(unicode_data::kCaseFolds);
    const auto* last = std::end(unicode_data::kCaseFolds);
    const auto* found = std::lower_bound(
        first, last, codepoint,
        [](const unicode_data::FoldMapping& mapping, std::uint32_t value) {
            return mapping.source < value;
        });
    return found != last && found->source == codepoint ? found : nullptr;
}

void append_folded_codepoint(std::string& output, std::uint32_t codepoint) {
    const unicode_data::FoldMapping* mapping = fold_mapping(codepoint);
    if (mapping == nullptr) {
        append_utf8(output, codepoint);
        return;
    }
    for (size_t index = 0; index < mapping->length; ++index) {
        append_utf8(output, mapping->values[index]);
    }
}

std::string fold_text(const std::string& text) {
    std::string folded;
    folded.reserve(text.size());
    size_t position = 0;
    while (position < text.size()) {
        const Decoded decoded = decode_at(text, position);
        if (!decoded.valid) {
            folded.push_back(text[position]);
            ++position;
            continue;
        }
        append_folded_codepoint(folded, decoded.codepoint);
        position += decoded.length;
    }
    return folded;
}

bool prefix_has_uppercase(const std::string& prefix) {
    size_t position = 0;
    while (position < prefix.size()) {
        const Decoded decoded = decode_at(prefix, position);
        if (!decoded.valid) {
            ++position;
            continue;
        }
        if (is_uppercase(decoded.codepoint)) {
            return true;
        }
        position += decoded.length;
    }
    return false;
}

size_t word_window_start(const std::string& text, size_t position) {
    position = std::min(position, text.size());
    while (position > 0) {
        const size_t previous = previous_start(text, position);
        if (!is_word_codepoint(decode_at(text, previous))) {
            break;
        }
        position = previous;
    }
    return position;
}

size_t word_window_end(const std::string& text, size_t position) {
    position = std::min(position, text.size());
    while (position < text.size()) {
        const Decoded decoded = decode_at(text, position);
        if (!is_word_codepoint(decoded)) {
            break;
        }
        position += decoded.length;
    }
    return position;
}

void for_each_word(const std::string& text,
                   size_t start,
                   size_t end,
                   const std::function<void(const std::string&)>& callback) {
    size_t position = std::min(start, text.size());
    end = std::min(end, text.size());
    while (position < end) {
        const Decoded decoded = decode_at(text, position);
        if (!is_word_codepoint(decoded)) {
            position += decoded.valid ? decoded.length : 1;
            continue;
        }
        const size_t word_start = position;
        position += decoded.length;
        while (position < end) {
            const Decoded next = decode_at(text, position);
            if (!is_word_codepoint(next)) {
                break;
            }
            position += next.length;
        }
        callback(text.substr(word_start, position - word_start));
    }
}

bool starts_with(const std::string& value, const std::string& prefix) {
    return value.size() >= prefix.size() && value.compare(0, prefix.size(), prefix) == 0;
}

bool candidate_extends(const std::string& candidate,
                       const std::string& prefix,
                       bool case_sensitive) {
    if (case_sensitive) {
        return candidate.size() > prefix.size() && starts_with(candidate, prefix);
    }
    const std::string folded_candidate = fold_text(candidate);
    const std::string folded_prefix = fold_text(prefix);
    return folded_candidate.size() > folded_prefix.size() &&
           starts_with(folded_candidate, folded_prefix);
}

size_t valid_utf8_prefix_length(const std::string& value, size_t length) {
    length = std::min(length, value.size());
    while (length > 0 && length < value.size() &&
           (static_cast<unsigned char>(value[length]) & 0xC0U) == 0x80U) {
        --length;
    }
    return length;
}

std::string common_prefix(const std::vector<std::string>& candidates, bool case_sensitive) {
    if (candidates.empty()) {
        return {};
    }
    if (case_sensitive) {
        size_t length = candidates.front().size();
        for (size_t index = 1; index < candidates.size(); ++index) {
            length = std::min(length, candidates[index].size());
            size_t shared = 0;
            while (shared < length && candidates.front()[shared] == candidates[index][shared]) {
                ++shared;
            }
            length = valid_utf8_prefix_length(candidates.front(), shared);
        }
        return candidates.front().substr(0, length);
    }

    std::vector<std::string> folded;
    folded.reserve(candidates.size());
    for (const std::string& candidate : candidates) {
        folded.push_back(fold_text(candidate));
    }
    size_t folded_length = folded.front().size();
    for (size_t index = 1; index < folded.size(); ++index) {
        folded_length = std::min(folded_length, folded[index].size());
        size_t shared = 0;
        while (shared < folded_length && folded.front()[shared] == folded[index][shared]) {
            ++shared;
        }
        folded_length = valid_utf8_prefix_length(folded.front(), shared);
    }

    std::string folded_first;
    size_t source_end = 0;
    while (source_end < candidates.front().size()) {
        const Decoded decoded = decode_at(candidates.front(), source_end);
        if (!decoded.valid) {
            break;
        }
        std::string next = folded_first;
        append_folded_codepoint(next, decoded.codepoint);
        if (next.size() > folded_length ||
            folded.front().compare(0, next.size(), next) != 0) {
            break;
        }
        folded_first = std::move(next);
        source_end += decoded.length;
    }
    return candidates.front().substr(0, source_end);
}

}  // namespace

struct WordIndex::Data {
    bool initialized = false;
    std::map<std::string, size_t> exact;
    std::map<std::string, std::map<std::string, size_t>> folded;
};

void WordIndex::ensure(const std::string& text) const {
    if (data_ != nullptr && data_->initialized) {
        return;
    }
    data_ = std::make_shared<Data>();
    add_range(text, 0, text.size());
    data_->initialized = true;
}

void WordIndex::invalidate() {
    data_.reset();
}

void WordIndex::add_range(const std::string& text, size_t start, size_t end) const {
    for_each_word(text, start, end, [&](const std::string& word) {
        ++data_->exact[word];
        ++data_->folded[fold_text(word)][word];
    });
}

void WordIndex::remove_range(const std::string& text, size_t start, size_t end) {
    for_each_word(text, start, end, [&](const std::string& word) {
        auto exact = data_->exact.find(word);
        if (exact != data_->exact.end() && --exact->second == 0) {
            data_->exact.erase(exact);
        }
        const std::string folded_word = fold_text(word);
        auto folded = data_->folded.find(folded_word);
        if (folded == data_->folded.end()) {
            return;
        }
        auto spelling = folded->second.find(word);
        if (spelling != folded->second.end() && --spelling->second == 0) {
            folded->second.erase(spelling);
        }
        if (folded->second.empty()) {
            data_->folded.erase(folded);
        }
    });
}

void WordIndex::apply_edit(const std::string& before,
                           size_t position,
                           size_t removed,
                           const std::string& inserted) {
    if (data_ == nullptr || !data_->initialized) {
        return;
    }
    if (!data_.unique()) {
        try {
            data_ = std::make_shared<Data>(*data_);
        } catch (...) {
            invalidate();
            return;
        }
    }
    position = std::min(position, before.size());
    removed = std::min(removed, before.size() - position);
    const size_t old_start = word_window_start(before, position);
    const size_t old_end = word_window_end(before, position + removed);
    try {
        std::string replacement_window;
        const size_t left_size = position - old_start;
        const size_t right_start = position + removed;
        const size_t right_size = old_end - right_start;
        if (inserted.size() > std::numeric_limits<size_t>::max() - left_size ||
            inserted.size() + left_size >
                std::numeric_limits<size_t>::max() - right_size) {
            invalidate();
            return;
        }
        replacement_window.reserve(left_size + inserted.size() + right_size);
        replacement_window.append(before, old_start, left_size);
        replacement_window += inserted;
        replacement_window.append(before, right_start, right_size);
        remove_range(before, old_start, old_end);
        add_range(replacement_window, 0, replacement_window.size());
    } catch (...) {
        invalidate();
    }
}

void WordIndex::append_matches(const std::string& prefix,
                               bool case_sensitive,
                               std::map<std::string, size_t>& matches) const {
    if (data_ == nullptr || !data_->initialized) {
        return;
    }
    if (case_sensitive) {
        for (auto found = data_->exact.lower_bound(prefix);
             found != data_->exact.end() && starts_with(found->first, prefix);
             ++found) {
            if (found->first.size() > prefix.size()) {
                matches[found->first] += found->second;
            }
        }
        return;
    }
    const std::string folded_prefix = fold_text(prefix);
    for (auto found = data_->folded.lower_bound(folded_prefix);
         found != data_->folded.end() && starts_with(found->first, folded_prefix);
         ++found) {
        if (found->first.size() <= folded_prefix.size()) {
            continue;
        }
        for (const auto& spelling : found->second) {
            matches[spelling.first] += spelling.second;
        }
    }
}

size_t WordIndex::occurrence_count(const std::string& word) const {
    if (data_ == nullptr) {
        return 0;
    }
    const auto found = data_->exact.find(word);
    return found == data_->exact.end() ? 0 : found->second;
}

size_t WordIndex::unique_word_count() const {
    return data_ == nullptr ? 0 : data_->exact.size();
}

bool WordIndex::initialized() const {
    return data_ != nullptr && data_->initialized;
}

bool WordCompleter::session_is_valid(const EditorState& state, size_t active_buffer) const {
    if (!active_ || active_buffer != active_buffer_ || state.selection.has_range() ||
        start_ > state.text.size() || current_value_.size() > state.text.size() - start_) {
        return false;
    }
    return state.cursor == start_ + current_value_.size() &&
           state.text.range_text(start_, current_value_.size()) == current_value_;
}

WordCompletionResult WordCompleter::cycle(EditorState& state) {
    WordCompletionResult result;
    if (candidates_.empty()) {
        reset();
        return result;
    }
    const std::string& candidate = candidates_[next_candidate_];
    Error error = state.replace_completion(start_, current_value_.size(), candidate, false);
    if (!error.ok()) {
        reset();
        result.error = std::move(error);
        return result;
    }
    current_value_ = candidate;
    next_candidate_ = (next_candidate_ + 1) % candidates_.size();
    result.completed = true;
    result.cycling = true;
    result.match_count = candidates_.size();
    result.value = candidate;
    return result;
}

WordCompletionResult WordCompleter::complete(EditorState& active,
                                             const std::vector<EditorState>& buffers,
                                             size_t active_buffer) {
    try {
        if (session_is_valid(active, active_buffer)) {
            return cycle(active);
        }
        reset();
        WordCompletionResult result;
        if (active.selection.has_range() || active.cursor == 0 || active_buffer >= buffers.size()) {
            return result;
        }

        const std::string active_text = active.text.str();
        const size_t prefix_start = word_window_start(active_text, active.cursor);
        if (prefix_start == active.cursor) {
            return result;
        }
        const std::string prefix = active_text.substr(prefix_start, active.cursor - prefix_start);
        const bool case_sensitive = prefix_has_uppercase(prefix);
        const size_t current_end = word_window_end(active_text, active.cursor);
        const std::string current_word = active_text.substr(prefix_start, current_end - prefix_start);

        std::map<std::string, size_t> matches;
        for (size_t index = 0; index < buffers.size(); ++index) {
            const EditorState& buffer = index == active_buffer ? active : buffers[index];
            const WordIndex& word_index = index == active_buffer
                                              ? buffer.completion_word_index(active_text)
                                              : buffer.completion_word_index();
            word_index.append_matches(prefix, case_sensitive, matches);
        }
        auto current = matches.find(current_word);
        if (current != matches.end()) {
            if (current->second <= 1) {
                matches.erase(current);
            } else {
                --current->second;
            }
        }

        std::vector<std::string> candidates;
        candidates.reserve(matches.size());
        for (const auto& match : matches) {
            if (match.second > 0 && candidate_extends(match.first, prefix, case_sensitive)) {
                candidates.push_back(match.first);
            }
        }
        if (candidates.empty()) {
            return result;
        }

        std::string replacement;
        bool begin_cycle = candidates.size() > 1;
        size_t next_candidate = 0;
        if (candidates.size() == 1) {
            replacement = candidates.front();
        } else {
            replacement = common_prefix(candidates, case_sensitive);
            if (!candidate_extends(replacement, prefix, case_sensitive)) {
                replacement = candidates.front();
                next_candidate = 1 % candidates.size();
            }
        }

        Error error = active.replace_completion(
            prefix_start, prefix.size(), replacement, true);
        if (!error.ok()) {
            result.error = std::move(error);
            return result;
        }
        result.completed = true;
        result.match_count = candidates.size();
        result.value = replacement;
        if (begin_cycle) {
            active_ = true;
            active_buffer_ = active_buffer;
            start_ = prefix_start;
            current_value_ = replacement;
            candidates_ = std::move(candidates);
            next_candidate_ = next_candidate;
        }
        return result;
    } catch (const std::bad_alloc&) {
        reset();
        WordCompletionResult result;
        result.error = {ErrorCode::Internal, "not enough memory to complete editor words"};
        return result;
    } catch (const std::length_error&) {
        reset();
        WordCompletionResult result;
        result.error = {ErrorCode::Internal, "editor word completion result is too large"};
        return result;
    }
}

void WordCompleter::reset() {
    active_ = false;
    active_buffer_ = 0;
    start_ = 0;
    current_value_.clear();
    candidates_.clear();
    next_candidate_ = 0;
}

std::string word_completion_status(const WordCompletionResult& result) {
    if (!result.error.ok()) {
        return result.error.message;
    }
    if (!result.completed) {
        return {};
    }
    if (result.match_count <= 1) {
        return "Completed: " + result.value;
    }
    std::ostringstream status;
    status << result.match_count << " matches: " << result.value << " (Tab for next)";
    return status.str();
}

}  // namespace pkchat::editor
