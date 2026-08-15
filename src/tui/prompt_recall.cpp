#include "tui/prompt_recall.hpp"

namespace ainiux::tui {

void PromptRecall::record(const std::string& text) {
    if (text.empty()) {
        index_ = entries_.size();
        draft_.clear();
        return;
    }
    if (entries_.empty() || entries_.back() != text) {
        entries_.push_back(text);
    }
    index_ = entries_.size();
    draft_.clear();
}

bool PromptRecall::recall_previous(std::string& current) {
    if (entries_.empty() || index_ == 0) return false;
    if (index_ >= entries_.size()) draft_ = current;
    --index_;
    current = entries_[index_];
    return true;
}

bool PromptRecall::recall_next(std::string& current) {
    if (index_ >= entries_.size()) return false;
    ++index_;
    current = index_ >= entries_.size() ? draft_ : entries_[index_];
    return true;
}

}  // namespace ainiux::tui
