#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include "common.hpp"
#include "editor/editor.hpp"
#include "editor/editor_prompts.hpp"

namespace ainiux::editor {

enum class CompletionKind {
    None,
    Path,
    Command,
};

struct PathCompletionResult {
    Error error;
    CompletionKind kind = CompletionKind::Path;
    size_t match_count = 0;
    size_t choice_index = 0;
    std::string value;
    bool changed = false;
    bool cycling = false;
    bool handled = true;
};

class PathCompleter {
   public:
    PathCompletionResult complete(EditorState& state,
                                  const std::function<bool()>& cancelled = {});
    bool can_cycle(const EditorState& state) const;
    void reset();

   private:
    bool active_ = false;
    size_t token_start_ = 0;
    size_t next_choice_ = 0;
    std::string applied_value_;
    std::vector<std::string> candidates_;
};

class ContextualCompleter {
   public:
    void set_assist_config(const EditorAssistConfig* config) { assist_config_ = config; }

    PathCompletionResult complete(EditorState& state,
                                  const std::function<bool()>& cancelled = {});
    bool can_complete(const EditorState& state) const;
    bool can_cycle(const EditorState& state) const;
    void reset();

   private:
    bool can_cycle_command(const EditorState& state) const;
    PathCompletionResult complete_command(EditorState& state);

    PathCompleter path_completer_;
    const EditorAssistConfig* assist_config_ = nullptr;
    bool command_active_ = false;
    size_t command_start_ = 0;
    size_t command_next_choice_ = 0;
    std::string command_applied_value_;
    std::vector<std::string> command_candidates_;
};

bool is_chat_slash_command_tab_completion(const EditorState& state);

std::string path_completion_status(const PathCompletionResult& result);
PathCompletionResult complete_path_input(std::string& input,
                                         PathCompleter& completer,
                                         const std::function<bool()>& cancelled = {});

}  // namespace ainiux::editor
