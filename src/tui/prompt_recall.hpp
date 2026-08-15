#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "provider/provider.hpp"

namespace ainiux::tui {

// Chronological user-prompt texts from a loaded thread (empty rows skipped).
std::vector<std::string> user_prompts_for_recall(
    const std::vector<provider::Message>& messages);

// In-memory bash-style prompt recall for one TUI surface (chat or agent).
// The list itself is not written to disk; chat/agent surfaces seed it from
// the currently loaded transcript so Up/Down can recall older user prompts.
class PromptRecall {
   public:
    void record(const std::string& text);
    // Replace the list (oldest first) and return to the live-draft slot.
    void replace(const std::vector<std::string>& texts);
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
