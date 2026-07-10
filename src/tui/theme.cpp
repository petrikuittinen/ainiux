#include "tui/theme_registry.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace pkchat::tui {
namespace {

std::string lowercase_copy(const std::string& text) {
    std::string lower;
    lower.reserve(text.size());
    for (char ch : text) {
        lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    return lower;
}

ThemePalette dark_palette() {
    ThemePalette palette;
    palette.name = "dark";
    palette.background = {0x0B, 0x0F, 0x14};
    palette.text = {0xE6, 0xED, 0xF3};
    palette.muted = {0x9B, 0xA7, 0xB4};
    palette.thinking_trace = {0xA7, 0xB8, 0xC9};
    palette.user_label = {0x7D, 0xD3, 0xFC};
    palette.assistant_label = {0x86, 0xEF, 0xAC};
    palette.error = {0xFC, 0xA5, 0xA5};
    palette.status_foreground = {0xFF, 0xFF, 0xFF};
    palette.status_background = {0x1F, 0x29, 0x37};
    palette.thinking_activity = {0x93, 0xC5, 0xFD};
    palette.streaming_activity = {0x4A, 0xDE, 0x80};
    palette.panel_title = {0x7D, 0xD3, 0xFC};
    palette.panel_border = {0xA8, 0xB8, 0xCA};
    palette.panel_hint = {0xB8, 0xC4, 0xD4};
    palette.panel_highlight = {0xFD, 0xE6, 0x8A};
    palette.panel_body = {0xE6, 0xED, 0xF3};
    palette.panel_background = {0x11, 0x17, 0x22};
    return palette;
}

ThemePalette light_palette() {
    ThemePalette palette;
    palette.name = "light";
    palette.background = {0xFA, 0xFA, 0xFA};
    palette.text = {0x11, 0x18, 0x27};
    palette.muted = {0x4B, 0x55, 0x63};
    palette.thinking_trace = {0x52, 0x63, 0x7A};
    palette.user_label = {0x07, 0x59, 0x85};
    palette.assistant_label = {0x16, 0x65, 0x34};
    palette.error = {0xB9, 0x1C, 0x1C};
    palette.status_foreground = {0x11, 0x18, 0x27};
    palette.status_background = {0xE5, 0xE7, 0xEB};
    palette.thinking_activity = {0x1D, 0x4E, 0x8A};
    palette.streaming_activity = {0x05, 0x86, 0x30};
    palette.panel_title = {0x07, 0x59, 0x85};
    palette.panel_border = {0x4B, 0x55, 0x63};
    palette.panel_hint = {0x37, 0x47, 0x51};
    palette.panel_highlight = {0xB4, 0x53, 0x09};
    palette.panel_body = {0x1F, 0x29, 0x37};
    palette.panel_background = {0xF3, 0xF4, 0xF6};
    return palette;
}

bool parse_hex_byte(const std::string& text, size_t& pos, int& out) {
    if (pos + 1 >= text.size()) {
        return false;
    }
    auto hex_value = [](char ch) -> int {
        if (ch >= '0' && ch <= '9') {
            return ch - '0';
        }
        if (ch >= 'a' && ch <= 'f') {
            return 10 + (ch - 'a');
        }
        if (ch >= 'A' && ch <= 'F') {
            return 10 + (ch - 'A');
        }
        return -1;
    };
    const int high = hex_value(text[pos]);
    const int low = hex_value(text[pos + 1]);
    if (high < 0 || low < 0) {
        return false;
    }
    out = (high << 4) | low;
    pos += 2;
    return true;
}

}  // namespace

void ThemeRegistry::clear() {
    palettes_.clear();
}

void ThemeRegistry::merge(ThemePalette palette) {
    const std::string key = lowercase_copy(palette.name);
    for (auto& item : palettes_) {
        if (lowercase_copy(item.name) == key) {
            item = std::move(palette);
            return;
        }
    }
    palettes_.push_back(std::move(palette));
}

const ThemePalette* ThemeRegistry::find(const std::string& name) const {
    const std::string key = lowercase_copy(name);
    for (const ThemePalette& palette : palettes_) {
        if (lowercase_copy(palette.name) == key) {
            return &palette;
        }
    }
    return nullptr;
}

bool ThemeRegistry::has(const std::string& name) const {
    return find(name) != nullptr;
}

bool ThemeRegistry::normalize_name(const std::string& input, std::string& canonical) const {
    const ThemePalette* palette = find(input);
    if (palette == nullptr) {
        return false;
    }
    canonical = palette->name;
    return true;
}

std::vector<std::string> ThemeRegistry::names() const {
    std::vector<std::string> out;
    out.reserve(palettes_.size());
    for (const ThemePalette& palette : palettes_) {
        out.push_back(palette.name);
    }
    std::sort(out.begin(), out.end(), [](const std::string& left, const std::string& right) {
        return lowercase_copy(left) < lowercase_copy(right);
    });
    return out;
}

ThemeRegistry default_theme_registry() {
    ThemeRegistry registry;
    registry.merge(dark_palette());
    registry.merge(light_palette());
    return registry;
}

bool parse_rgb_color(const std::string& text, Rgb& out) {
    std::string trimmed;
    trimmed.reserve(text.size());
    for (char ch : text) {
        if (!std::isspace(static_cast<unsigned char>(ch))) {
            trimmed.push_back(ch);
        }
    }
    if (trimmed.empty()) {
        return false;
    }

    size_t pos = 0;
    if (trimmed[0] == '#') {
        pos = 1;
    } else if (trimmed.rfind("0x", 0) == 0 || trimmed.rfind("0X", 0) == 0) {
        pos = 2;
    }

    int r = 0;
    int g = 0;
    int b = 0;
    if (!parse_hex_byte(trimmed, pos, r) || !parse_hex_byte(trimmed, pos, g) ||
        !parse_hex_byte(trimmed, pos, b) || pos != trimmed.size()) {
        return false;
    }
    out = {r, g, b};
    return true;
}

std::string format_theme_list(const ThemeRegistry& registry) {
    const std::vector<std::string> theme_names = registry.names();
    if (theme_names.empty()) {
        return "none";
    }
    std::string out;
    for (size_t i = 0; i < theme_names.size(); ++i) {
        if (i > 0) {
            out += ", ";
        }
        out += theme_names[i];
    }
    return out;
}

std::string ansi_foreground_sequence(const Rgb& color) {
    return "\x1b[38;2;" + std::to_string(color.r) + ";" + std::to_string(color.g) + ";" +
           std::to_string(color.b) + "m";
}

std::string ansi_style_sequence(const StylePair& pair) {
    return ansi_foreground_sequence(pair.foreground) + "\x1b[48;2;" + std::to_string(pair.background.r) + ";" +
           std::to_string(pair.background.g) + ";" + std::to_string(pair.background.b) + "m";
}

ThemeCommandResult handle_theme_command(const ThemeRegistry& registry,
                                        const std::string& current_theme,
                                        const std::string& requested,
                                        bool use_colors) {
    ThemeCommandResult result;
    result.selected_theme = current_theme;
    if (requested.empty()) {
        result.ok = true;
        result.message = "Theme: " + current_theme + ". Available: " + format_theme_list(registry);
        if (!use_colors) {
            result.message += " (colors disabled by --nocolors)";
        }
        return result;
    }

    std::string normalized;
    if (!registry.normalize_name(requested, normalized)) {
        result.message = "Unknown theme: " + requested + ". Available: " + format_theme_list(registry);
        return result;
    }
    result.ok = true;
    result.selected_theme = normalized;
    result.message = "Theme set to " + normalized;
    if (!use_colors) {
        result.message += " (colors disabled by --nocolors)";
    }
    return result;
}

StylePair style_pair_for(const ThemePalette& palette, StyleRole role) {
    switch (role) {
        case StyleRole::Text:
            return {palette.text, palette.background};
        case StyleRole::Muted:
            return {palette.muted, palette.background};
        case StyleRole::ThinkingTrace:
            return {palette.thinking_trace, palette.background};
        case StyleRole::UserLabel:
            return {palette.user_label, palette.background};
        case StyleRole::AssistantLabel:
            return {palette.assistant_label, palette.background};
        case StyleRole::Error:
            return {palette.error, palette.background};
        case StyleRole::Status:
            return {palette.status_foreground, palette.status_background};
        case StyleRole::InputLabel:
            return {palette.status_foreground, palette.status_background};
        case StyleRole::ThinkingActivity:
            return {palette.thinking_activity, palette.background};
        case StyleRole::StreamingActivity:
            return {palette.streaming_activity, palette.background};
        case StyleRole::PanelTitle:
            return {palette.panel_title, palette.panel_background};
        case StyleRole::PanelBorder:
            return {palette.panel_border, palette.panel_background};
        case StyleRole::PanelHint:
            return {palette.panel_hint, palette.panel_background};
        case StyleRole::PanelHighlight:
            return {palette.panel_highlight, palette.panel_background};
        case StyleRole::PanelBody:
            return {palette.panel_body, palette.panel_background};
    }
    return {palette.text, palette.background};
}

StylePair style_pair_for(const ThemeRegistry& registry, const std::string& theme_name, StyleRole role) {
    const ThemePalette* palette = registry.find(theme_name);
    if (palette == nullptr) {
        palette = registry.find("dark");
    }
    if (palette == nullptr && !registry.names().empty()) {
        palette = registry.find(registry.names().front());
    }
    if (palette == nullptr) {
        return style_pair_for(dark_palette(), role);
    }
    return style_pair_for(*palette, role);
}

std::string style_sequence_for(const ThemeRegistry& registry, const std::string& theme_name, StyleRole role) {
    return ansi_style_sequence(style_pair_for(registry, theme_name, role));
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