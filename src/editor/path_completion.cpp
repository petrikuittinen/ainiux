#include "editor/path_completion.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <system_error>

namespace pkchat::editor {
namespace {

bool is_token_separator(char ch) {
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
}

std::string longest_common_prefix(const std::vector<std::string>& values) {
    if (values.empty()) {
        return "";
    }
    std::string prefix = values.front();
    for (size_t i = 1; i < values.size(); ++i) {
        size_t length = 0;
        const size_t limit = std::min(prefix.size(), values[i].size());
        while (length < limit && prefix[length] == values[i][length]) {
            ++length;
        }
        prefix.resize(length);
    }
    return prefix;
}

std::filesystem::path expanded_scan_path(const std::string& path) {
    if (path == "~" || path.rfind("~/", 0) == 0) {
        const char* home = std::getenv("HOME");
        if (home != nullptr && *home != '\0') {
            if (path.size() <= 2) {
                return std::filesystem::path(home);
            }
            return std::filesystem::path(home) / path.substr(2);
        }
    }
    return std::filesystem::path(path);
}

Error find_candidates(const std::string& token,
                      std::vector<std::string>& candidates,
                      const std::function<bool()>& cancelled) {
    const size_t slash = token.find_last_of('/');
    const std::string display_directory =
        slash == std::string::npos ? "" : token.substr(0, slash + 1);
    const std::string name_prefix =
        slash == std::string::npos ? token : token.substr(slash + 1);

    std::string scan_directory = ".";
    if (slash != std::string::npos) {
        scan_directory = token.substr(0, slash);
        if (scan_directory.empty()) {
            scan_directory = "/";
        }
    }

    if (cancelled && cancelled()) {
        return {ErrorCode::Cancelled, "path completion cancelled"};
    }

    std::error_code ec;
    std::filesystem::directory_iterator entry(expanded_scan_path(scan_directory), ec);
    const std::filesystem::directory_iterator end;
    if (ec) {
        if (ec == std::errc::no_such_file_or_directory || ec == std::errc::not_a_directory) {
            return ok_error();
        }
        return {ErrorCode::FileRead,
                "could not list path-completion directory " + scan_directory + ": " + ec.message()};
    }

    while (entry != end) {
        if (cancelled && cancelled()) {
            return {ErrorCode::Cancelled, "path completion cancelled"};
        }
        const std::string name = entry->path().filename().string();
        if (name.compare(0, name_prefix.size(), name_prefix) == 0 &&
            (!name_prefix.empty() || name.empty() || name[0] != '.')) {
            std::string candidate = display_directory + name;
            std::error_code type_error;
            if (entry->is_directory(type_error) && !type_error) {
                candidate.push_back('/');
            }
            candidates.push_back(std::move(candidate));
        }
        entry.increment(ec);
        if (ec) {
            return {ErrorCode::FileRead,
                    "could not continue listing path-completion directory " + scan_directory + ": " +
                        ec.message()};
        }
    }

    std::sort(candidates.begin(), candidates.end());
    return ok_error();
}

Error replace_token(EditorState& state, size_t start, size_t length, const std::string& value, bool& changed) {
    const std::string current = state.text.str().substr(start, length);
    changed = current != value;
    if (!changed) {
        return ok_error();
    }

    PieceTable replacement = state.text;
    Error err = replacement.erase(start, length);
    if (!err.ok()) {
        return err;
    }
    err = replacement.insert(start, value);
    if (!err.ok()) {
        return err;
    }
    state.text = std::move(replacement);
    state.cursor = start + value.size();
    state.preferred_column = state.text.display_column_for_offset(state.cursor);
    state.dirty = true;
    return ok_error();
}

}  // namespace

PathCompletionResult PathCompleter::complete(EditorState& state,
                                             const std::function<bool()>& cancelled) {
    PathCompletionResult result;
    const std::string buffer = state.text.str();

    if (active_ && state.cursor == token_start_ + applied_value_.size() &&
        token_start_ + applied_value_.size() <= buffer.size() &&
        buffer.compare(token_start_, applied_value_.size(), applied_value_) == 0) {
        const size_t selected = next_choice_;
        result.error = replace_token(
            state, token_start_, applied_value_.size(), candidates_[selected], result.changed);
        if (!result.error.ok()) {
            reset();
            return result;
        }
        applied_value_ = candidates_[selected];
        next_choice_ = (selected + 1) % candidates_.size();
        result.match_count = candidates_.size();
        result.choice_index = selected;
        result.value = applied_value_;
        result.cycling = true;
        return result;
    }

    reset();
    const size_t cursor = std::min(state.cursor, buffer.size());
    size_t start = cursor;
    while (start > 0 && !is_token_separator(buffer[start - 1])) {
        --start;
    }
    size_t end = cursor;
    while (end < buffer.size() && !is_token_separator(buffer[end])) {
        ++end;
    }
    const std::string token = buffer.substr(start, cursor - start);

    result.error = find_candidates(token, candidates_, cancelled);
    if (!result.error.ok()) {
        reset();
        return result;
    }
    result.match_count = candidates_.size();
    result.value = token;
    if (candidates_.empty()) {
        reset();
        return result;
    }

    const std::string completion = candidates_.size() == 1 ? candidates_.front()
                                                            : longest_common_prefix(candidates_);
    result.error = replace_token(state, start, end - start, completion, result.changed);
    if (!result.error.ok()) {
        reset();
        return result;
    }
    result.value = completion;
    if (candidates_.size() > 1) {
        active_ = true;
        token_start_ = start;
        applied_value_ = completion;
        next_choice_ = 0;
    } else {
        candidates_.clear();
    }
    return result;
}

bool PathCompleter::can_cycle(const EditorState& state) const {
    if (!active_) {
        return false;
    }
    const std::string buffer = state.text.str();
    return state.cursor == token_start_ + applied_value_.size() &&
           token_start_ + applied_value_.size() <= buffer.size() &&
           buffer.compare(token_start_, applied_value_.size(), applied_value_) == 0;
}

void PathCompleter::reset() {
    active_ = false;
    token_start_ = 0;
    next_choice_ = 0;
    applied_value_.clear();
    candidates_.clear();
}

std::string path_completion_status(const PathCompletionResult& result) {
    if (!result.error.ok()) {
        return result.error.message;
    }
    if (result.match_count == 0) {
        return result.value.empty() ? "No file paths match" : "No file paths match " + result.value;
    }
    if (result.match_count == 1) {
        return "Completed path: " + result.value;
    }
    if (result.cycling) {
        return "Path " + std::to_string(result.choice_index + 1) + "/" +
               std::to_string(result.match_count) + ": " + result.value + " (Tab for next)";
    }
    return std::to_string(result.match_count) + " paths match; Tab again to cycle";
}

}  // namespace pkchat::editor
