#include "editor/path_completion.hpp"

#include "common.hpp"
#include "editor/assist_runtime.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <system_error>

namespace ainiux::editor {
namespace {

bool is_token_separator(char ch) {
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
}

enum class ChatCompletionContextKind {
    None,
    Command,
    Path,
};

struct ChatCompletionContext {
    ChatCompletionContextKind kind = ChatCompletionContextKind::None;
};

const std::vector<std::string>& chat_command_completions() {
    // Align /width are editor-only (not chat history).
    static const std::vector<std::string> commands = {
        "/attach ",
        "/clear",
        "/cleanup",
        "/clone",
        "/context ",
        "/exit",
        "/fetch ",
        "/help",
        "/highlight ",
        "/insert ",
        "/list",
        "/load ",
        "/model ",
        "/models",
        "/mode ",
        "/new ",
        "/pop",
        "/provider ",
        "/quit",
        "/reasoning ",
        "/remove",
        "/remove-empty",
        "/response",
        "/save ",
        "/search ",
        "/scrollbar ",
        "/setting",
        "/setting ",
        "/shell ",
        "/shell-stdout ",
        "/system",
        "/theme ",
        "/thinking ",
    };
    return commands;
}

const std::vector<std::string>& agent_command_completions() {
    // Align /width are editor-only (not agent history).
    static const std::vector<std::string> commands = {
        "/act",
        "/goal",
        "/agent",
        "/attach ",
        "/chat",
        "/clear",
        "/cmd-out ",
        "/compact",
        "/compact fast",
        "/compact smart",
        "/compact summary",
        "/context ",
        "/cycle",
        "/edit",
        "/editor",
        "/exit",
        "/fetch ",
        "/help",
        "/highlight ",
        "/index-code",
        "/insert ",
        "/model ",
        "/models",
        "/mode ",
        "/new ",
        "/permissions ",
        "/plan",
        "/provider ",
        "/quit",
        "/reasoning ",
        "/search ",
        "/scrollbar ",
        "/setting",
        "/setting ",
        "/shell ",
        "/shell-stdout ",
        "/show-index",
        "/system",
        "/theme ",
        "/thinking ",
    };
    return commands;
}

bool is_path_command(const std::string& command) {
    return command == "/save" || command == "/load" || command == "/attach" ||
           command == "/insert";
}

std::string trim_completion_display(std::string value) {
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
        value.pop_back();
    }
    return value;
}

ChatCompletionContext chat_completion_context(const EditorState& state) {
    ChatCompletionContext context;
    if (state.mode != EditorMode::Chat) {
        return context;
    }

    const std::string buffer = state.text.str();
    const size_t cursor = std::min(state.cursor, buffer.size());
    if (buffer.empty() || cursor == 0 || buffer[0] != '/') {
        return context;
    }

    const size_t newline = buffer.find('\n');
    const size_t first_line_end = newline == std::string::npos ? buffer.size() : newline;
    if (cursor > first_line_end) {
        return context;
    }

    size_t command_end = 0;
    while (command_end < first_line_end && !is_token_separator(buffer[command_end])) {
        ++command_end;
    }

    if (cursor <= command_end) {
        context.kind = ChatCompletionContextKind::Command;
        return context;
    }

    const std::string command = buffer.substr(0, command_end);
    if (command_end < first_line_end && is_token_separator(buffer[command_end]) &&
        is_path_command(command)) {
        context.kind = ChatCompletionContextKind::Path;
    }
    return context;
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
    return std::filesystem::path(expand_user_path(path));
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

    return state.replace(start, length, value);
}

}  // namespace

PathCompletionResult PathCompleter::complete(EditorState& state,
                                             const std::function<bool()>& cancelled) {
    PathCompletionResult result;
    result.kind = CompletionKind::Path;
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

PathCompletionResult ContextualCompleter::complete(EditorState& state,
                                                   const std::function<bool()>& cancelled) {
    PathCompletionResult result;
    result.kind = CompletionKind::None;
    result.handled = false;

    if (state.mode != EditorMode::Chat) {
        reset();
        return result;
    }
    if (can_cycle_command(state)) {
        return complete_command(state);
    }
    if (path_completer_.can_cycle(state)) {
        command_active_ = false;
        command_start_ = 0;
        command_next_choice_ = 0;
        command_applied_value_.clear();
        command_candidates_.clear();
        return path_completer_.complete(state, cancelled);
    }

    const ChatCompletionContext context = chat_completion_context(state);
    if (context.kind == ChatCompletionContextKind::Command) {
        return complete_command(state);
    }
    if (context.kind == ChatCompletionContextKind::Path) {
        command_active_ = false;
        command_start_ = 0;
        command_next_choice_ = 0;
        command_applied_value_.clear();
        command_candidates_.clear();
        return path_completer_.complete(state, cancelled);
    }

    reset();
    return result;
}

bool ContextualCompleter::can_complete(const EditorState& state) const {
    if (state.mode != EditorMode::Chat) {
        return false;
    }
    if (can_cycle(state)) {
        return true;
    }
    return chat_completion_context(state).kind != ChatCompletionContextKind::None;
}

bool ContextualCompleter::can_cycle(const EditorState& state) const {
    if (state.mode != EditorMode::Chat) {
        return false;
    }
    return can_cycle_command(state) || path_completer_.can_cycle(state);
}

void ContextualCompleter::reset() {
    path_completer_.reset();
    command_active_ = false;
    command_start_ = 0;
    command_next_choice_ = 0;
    command_applied_value_.clear();
    command_candidates_.clear();
}

bool ContextualCompleter::can_cycle_command(const EditorState& state) const {
    if (!command_active_) {
        return false;
    }
    const std::string buffer = state.text.str();
    return state.cursor == command_start_ + command_applied_value_.size() &&
           command_start_ + command_applied_value_.size() <= buffer.size() &&
           buffer.compare(command_start_, command_applied_value_.size(),
                          command_applied_value_) == 0;
}

PathCompletionResult ContextualCompleter::complete_command(EditorState& state) {
    path_completer_.reset();

    PathCompletionResult result;
    result.kind = CompletionKind::Command;
    result.handled = true;

    const std::string buffer = state.text.str();
    if (can_cycle_command(state)) {
        const size_t selected = command_next_choice_;
        result.error = replace_token(state,
                                     command_start_,
                                     command_applied_value_.size(),
                                     command_candidates_[selected],
                                     result.changed);
        if (!result.error.ok()) {
            reset();
            return result;
        }
        command_applied_value_ = command_candidates_[selected];
        command_next_choice_ = (selected + 1) % command_candidates_.size();
        result.match_count = command_candidates_.size();
        result.choice_index = selected;
        result.value = command_applied_value_;
        result.cycling = true;
        return result;
    }

    command_active_ = false;
    command_start_ = 0;
    command_next_choice_ = 0;
    command_applied_value_.clear();
    command_candidates_.clear();

    const size_t cursor = std::min(state.cursor, buffer.size());
    size_t command_end = 0;
    const size_t newline = buffer.find('\n');
    const size_t first_line_end = newline == std::string::npos ? buffer.size() : newline;
    while (command_end < first_line_end && !is_token_separator(buffer[command_end])) {
        ++command_end;
    }

    const std::string token = buffer.substr(0, cursor);
    const std::string normalized_token = ascii_lower(token);
    const std::vector<std::string> commands =
        agent_mode_ ? agent_command_completions()
                    : (assist_config_ != nullptr
                           ? chat_assist_command_completions(*assist_config_)
                           : chat_command_completions());
    for (const std::string& command : commands) {
        const std::string normalized_command = ascii_lower(command);
        if (normalized_command.compare(0, normalized_token.size(), normalized_token) == 0) {
            command_candidates_.push_back(command);
        }
    }

    result.match_count = command_candidates_.size();
    result.value = token;
    if (command_candidates_.empty()) {
        command_candidates_.clear();
        return result;
    }

    const std::string completion = command_candidates_.size() == 1
                                       ? command_candidates_.front()
                                       : longest_common_prefix(command_candidates_);
    result.error = replace_token(state, 0, command_end, completion, result.changed);
    if (!result.error.ok()) {
        reset();
        return result;
    }

    result.value = completion;
    if (command_candidates_.size() > 1) {
        command_active_ = true;
        command_start_ = 0;
        command_applied_value_ = completion;
        command_next_choice_ = 0;
    } else {
        command_candidates_.clear();
    }
    return result;
}

bool is_chat_slash_command_tab_completion(const EditorState& state) {
    return chat_completion_context(state).kind == ChatCompletionContextKind::Command;
}

PathCompletionResult complete_path_input(std::string& input,
                                         PathCompleter& completer,
                                         const std::function<bool()>& cancelled) {
    EditorState state = EditorState::from_text(input);
    state.cursor = input.size();
    PathCompletionResult result = completer.complete(state, cancelled);
    input = state.text.str();
    return result;
}

std::string path_completion_status(const PathCompletionResult& result) {
    if (!result.handled || result.kind == CompletionKind::None) {
        return "Tab completion is not active here";
    }
    if (!result.error.ok()) {
        return result.error.message;
    }
    if (result.kind == CompletionKind::Command) {
        const std::string value = trim_completion_display(result.value);
        if (result.match_count == 0) {
            return value.empty() ? "No commands match" : "No commands match " + value;
        }
        if (result.match_count == 1) {
            return "Completed command: " + value;
        }
        if (result.cycling) {
            return "Command " + std::to_string(result.choice_index + 1) + "/" +
                   std::to_string(result.match_count) + ": " + value + " (Tab for next)";
        }
        return std::to_string(result.match_count) + " commands match; Tab again to cycle";
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

}  // namespace ainiux::editor
