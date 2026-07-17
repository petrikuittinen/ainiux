#pragma once

#include <string>

namespace ainiux::editor {

class Clipboard {
   public:
    void set(std::string text);
    const std::string& text() const { return text_; }
    bool empty() const { return text_.empty(); }
    void clear();

   private:
    std::string text_;
};

Clipboard& shared_clipboard();

}  // namespace ainiux::editor