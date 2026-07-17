#pragma once

#include "common.hpp"

#include <cstddef>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ainiux::editor {

struct EditorState;

class WordIndex {
   public:
    void ensure(const std::string& text) const;
    void invalidate();
    void apply_edit(const std::string& before,
                    size_t position,
                    size_t removed,
                    const std::string& inserted);
    void append_matches(const std::string& prefix,
                        bool case_sensitive,
                        std::map<std::string, size_t>& matches) const;
    size_t occurrence_count(const std::string& word) const;
    size_t unique_word_count() const;
    bool initialized() const;

   private:
    void add_range(const std::string& text, size_t start, size_t end) const;
    void remove_range(const std::string& text, size_t start, size_t end);

    struct Data;
    mutable std::shared_ptr<Data> data_;
};

struct WordCompletionResult {
    Error error;
    bool completed = false;
    bool cycling = false;
    size_t match_count = 0;
    std::string value;
};

class WordCompleter {
   public:
    WordCompletionResult complete(EditorState& active,
                                  const std::vector<EditorState>& buffers,
                                  size_t active_buffer);
    void reset();
    bool active() const { return active_; }

   private:
    bool session_is_valid(const EditorState& state, size_t active_buffer) const;
    WordCompletionResult cycle(EditorState& state);

    bool active_ = false;
    size_t active_buffer_ = 0;
    size_t start_ = 0;
    std::string current_value_;
    std::vector<std::string> candidates_;
    size_t next_candidate_ = 0;
};

std::string word_completion_status(const WordCompletionResult& result);

}  // namespace ainiux::editor
