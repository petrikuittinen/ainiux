#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace ainiux::tui {

// In-memory bash-style prompt recall for one TUI surface (chat or agent).
// Does not persist across process restarts.
class PromptRecall {
   public:
    void record(const std::string& text);
    bool recall_previous(std::string& current);
    bool recall_next(std::string& current);
    std::size_t size() const { return entries_.size(); }
    bool at_live_draft() const { return index_ >= entries_.size(); }

   private:
    std::vector<std::string> entries_;
    std::size_t index_ = 0;
    std::string draft_;
};

}  // namespace ainiux::tui
