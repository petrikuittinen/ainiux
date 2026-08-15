#include "tui/prompt_recall.hpp"

#include "app/detail.hpp"

namespace ainiux::tui {

std::vector<std::string> user_prompts_for_recall(
    const std::vector<provider::Message>& messages) {
    std::vector<std::string> prompts;
    for (const provider::Message& message : messages) {
        if (message.role != "user") continue;
        if (app::detail::trim_ascii(message.content).empty()) continue;
        prompts.push_back(message.content);
    }
    return prompts;
}

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

void PromptRecall::replace(const std::vector<std::string>& texts) {
    entries_.clear();
    draft_.clear();
    for (const std::string& text : texts) record(text);
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
