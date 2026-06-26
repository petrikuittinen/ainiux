#include "editor/clipboard.hpp"

namespace pkchat::editor {

void publish_terminal_clipboard(const std::string& text);

void Clipboard::set(std::string text) {
    text_ = std::move(text);
    publish_terminal_clipboard(text_);
}

void Clipboard::clear() {
    text_.clear();
}

Clipboard& shared_clipboard() {
    static Clipboard clipboard;
    return clipboard;
}

}  // namespace pkchat::editor