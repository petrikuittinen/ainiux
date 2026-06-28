#include "tui/tui.hpp"

#include <cmath>

namespace pkchat::tui {

const char* theme_name(ThemeName theme) {
    switch (theme) {
        case ThemeName::Dark:
            return "dark";
        case ThemeName::Light:
            return "light";
    }
    return "dark";
}

bool parse_theme_name(const std::string& text, ThemeName& out) {
    std::string lower;
    lower.reserve(text.size());
    for (char ch : text) {
        if (ch >= 'A' && ch <= 'Z') {
            lower.push_back(static_cast<char>(ch - 'A' + 'a'));
        } else {
            lower.push_back(ch);
        }
    }
    if (lower == "dark") {
        out = ThemeName::Dark;
        return true;
    }
    if (lower == "light") {
        out = ThemeName::Light;
        return true;
    }
    return false;
}

StylePair style_pair_for(ThemeName theme, StyleRole role) {
    const bool dark = theme == ThemeName::Dark;
    const Rgb background = dark ? Rgb{0x0B, 0x0F, 0x14} : Rgb{0xFA, 0xFA, 0xFA};
    const Rgb text = dark ? Rgb{0xE6, 0xED, 0xF3} : Rgb{0x11, 0x18, 0x27};
    const Rgb muted = dark ? Rgb{0x9B, 0xA7, 0xB4} : Rgb{0x4B, 0x55, 0x63};
    const Rgb thinking = dark ? Rgb{0xA7, 0xB8, 0xC9} : Rgb{0x52, 0x63, 0x7A};
    const Rgb user = dark ? Rgb{0x7D, 0xD3, 0xFC} : Rgb{0x07, 0x59, 0x85};
    const Rgb assistant = dark ? Rgb{0x86, 0xEF, 0xAC} : Rgb{0x16, 0x65, 0x34};
    const Rgb error = dark ? Rgb{0xFC, 0xA5, 0xA5} : Rgb{0xB9, 0x1C, 0x1C};
    const Rgb status_bg = dark ? Rgb{0x1F, 0x29, 0x37} : Rgb{0xE5, 0xE7, 0xEB};
    const Rgb status_fg = dark ? Rgb{0xFF, 0xFF, 0xFF} : Rgb{0x11, 0x18, 0x27};

    switch (role) {
        case StyleRole::Text:
            return {text, background};
        case StyleRole::Muted:
            return {muted, background};
        case StyleRole::ThinkingTrace:
            return {thinking, background};
        case StyleRole::UserLabel:
            return {user, background};
        case StyleRole::AssistantLabel:
            return {assistant, background};
        case StyleRole::Error:
            return {error, background};
        case StyleRole::Status:
            return {status_fg, status_bg};
        case StyleRole::InputLabel:
            return {status_fg, status_bg};
    }
    return {text, background};
}

double contrast_ratio(Rgb foreground, Rgb background) {
    auto linear = [](int channel) {
        const double value = static_cast<double>(channel) / 255.0;
        if (value <= 0.03928) {
            return value / 12.92;
        }
        return std::pow((value + 0.055) / 1.055, 2.4);
    };
    auto luminance = [&](Rgb color) {
        return 0.2126 * linear(color.r) + 0.7152 * linear(color.g) + 0.0722 * linear(color.b);
    };
    const double a = luminance(foreground);
    const double b = luminance(background);
    const double lighter = std::max(a, b);
    const double darker = std::min(a, b);
    return (lighter + 0.05) / (darker + 0.05);
}

}  // namespace pkchat::tui
