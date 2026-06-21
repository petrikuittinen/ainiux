#pragma once

#include <string>

namespace pkchat::output {

struct ThinkingChunk {
    std::string visible;
    std::string trace;
};

class ThinkingTraceSplitter {
   public:
    ThinkingChunk feed(const std::string& text);
    ThinkingChunk finish();

   private:
    std::string pending_;
    bool in_trace_ = false;
    bool saw_visible_ = false;
    bool suppress_leading_newlines_ = false;
};

ThinkingChunk split_thinking_traces(const std::string& text);

}  // namespace pkchat::output
