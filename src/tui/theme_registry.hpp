#pragma once

#include <string>
#include <vector>

#include "tui/theme_types.hpp"

namespace pkchat::tui {

struct ThemePalette {
    std::string name;
    Rgb background{0x0B, 0x0F, 0x14};
    Rgb text{0xE6, 0xED, 0xF3};
    Rgb muted{0x9B, 0xA7, 0xB4};
    Rgb thinking_trace{0xA7, 0xB8, 0xC9};
    Rgb user_label{0x7D, 0xD3, 0xFC};
    Rgb assistant_label{0x86, 0xEF, 0xAC};
    Rgb error{0xFC, 0xA5, 0xA5};
    Rgb status_foreground{0xFF, 0xFF, 0xFF};
    Rgb status_background{0x1F, 0x29, 0x37};
    Rgb thinking_activity{0x93, 0xC5, 0xFD};
    Rgb streaming_activity{0x4A, 0xDE, 0x80};
    Rgb panel_title{0x7D, 0xD3, 0xFC};
    Rgb panel_border{0xA8, 0xB8, 0xCA};
    Rgb panel_hint{0xB8, 0xC4, 0xD4};
    Rgb panel_highlight{0xFD, 0xE6, 0x8A};
    Rgb panel_body{0xE6, 0xED, 0xF3};
    Rgb panel_background{0x11, 0x17, 0x22};
};

class ThemeRegistry {
   public:
    void clear();
    void merge(ThemePalette palette);
    const ThemePalette* find(const std::string& name) const;
    bool has(const std::string& name) const;
    bool normalize_name(const std::string& input, std::string& canonical) const;
    std::vector<std::string> names() const;

   private:
    std::vector<ThemePalette> palettes_;
};

ThemeRegistry default_theme_registry();
bool parse_rgb_color(const std::string& text, Rgb& out);
std::string format_theme_list(const ThemeRegistry& registry);
std::string ansi_style_sequence(const StylePair& pair);
std::string ansi_foreground_sequence(const Rgb& color);

struct ThemeCommandResult {
    bool ok = false;
    std::string message;
    std::string selected_theme;
};

ThemeCommandResult handle_theme_command(const ThemeRegistry& registry,
                                        const std::string& current_theme,
                                        const std::string& requested,
                                        bool use_colors);

StylePair style_pair_for(const ThemePalette& palette, StyleRole role);
StylePair style_pair_for(const ThemeRegistry& registry, const std::string& theme_name, StyleRole role);
std::string style_sequence_for(const ThemeRegistry& registry, const std::string& theme_name, StyleRole role);

}  // namespace pkchat::tui