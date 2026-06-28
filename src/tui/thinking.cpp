#include "tui/tui.hpp"

namespace pkchat::tui {

int history_scroll_for_thread_beginning() {
    return std::numeric_limits<int>::max();
}

int history_scroll_for_thread_end() {
    return 0;
}

ThinkingDisplay thinking_display_text(const std::string& content, bool show_traces) {
    ThinkingDisplay display;
    if (show_traces) {
        display.text = content;
        return display;
    }

    const std::string open_tag = "<think>";
    const std::string close_tag = "</think>";
    auto lower_ascii = [](char ch) {
        if (ch >= 'A' && ch <= 'Z') {
            return static_cast<char>(ch - 'A' + 'a');
        }
        return ch;
    };
    auto tag_at = [&](std::size_t pos, const std::string& tag) {
        if (pos + tag.size() > content.size()) {
            return false;
        }
        for (std::size_t i = 0; i < tag.size(); ++i) {
            if (lower_ascii(content[pos + i]) != tag[i]) {
                return false;
            }
        }
        return true;
    };
    auto find_tag = [&](const std::string& tag, std::size_t start) {
        for (std::size_t pos = start; pos + tag.size() <= content.size(); ++pos) {
            if (tag_at(pos, tag)) {
                return pos;
            }
        }
        return std::string::npos;
    };
    auto trim_outer_newlines = [](std::string& text) {
        while (!text.empty() && (text.front() == '\n' || text.front() == '\r')) {
            text.erase(text.begin());
        }
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
            text.pop_back();
        }
    };

    bool in_thinking = false;
    std::size_t pos = 0;
    while (pos < content.size()) {
        if (!in_thinking) {
            const std::size_t open = find_tag(open_tag, pos);
            if (open == std::string::npos) {
                display.text.append(content, pos, std::string::npos);
                break;
            }
            display.text.append(content, pos, open - pos);
            display.saw_thinking_tag = true;
            pos = open + open_tag.size();
            in_thinking = true;
            continue;
        }

        const std::size_t close = find_tag(close_tag, pos);
        if (close == std::string::npos) {
            display.open_thinking_tag = true;
            break;
        }
        pos = close + close_tag.size();
        in_thinking = false;
    }

    if (display.saw_thinking_tag) {
        trim_outer_newlines(display.text);
    }
    return display;
}

}  // namespace pkchat::tui
