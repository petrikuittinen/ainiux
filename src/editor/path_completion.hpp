#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include "common.hpp"
#include "editor/editor.hpp"

namespace pkchat::editor {

struct PathCompletionResult {
    Error error;
    size_t match_count = 0;
    size_t choice_index = 0;
    std::string value;
    bool changed = false;
    bool cycling = false;
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

std::string path_completion_status(const PathCompletionResult& result);

}  // namespace pkchat::editor
