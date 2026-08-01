#pragma once

#include <string>
#include <vector>

#include "highlight/highlight.hpp"
#include "tui/theme_types.hpp"

namespace ainiux::tui {

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
    Rgb syntax_comment{0x9B, 0xA7, 0xB4};
    Rgb syntax_keyword{0xC4, 0xB5, 0xFD};
    Rgb syntax_type{0x7D, 0xD3, 0xFC};
    Rgb syntax_string{0x86, 0xEF, 0xAC};
    Rgb syntax_number{0xFD, 0xE6, 0x8A};
    Rgb syntax_literal{0xFD, 0xA4, 0xAF};
    Rgb syntax_function{0x93, 0xC5, 0xFD};
    Rgb syntax_variable{0xF9, 0xA8, 0xD4};
    Rgb syntax_operator{0xE6, 0xED, 0xF3};
    Rgb syntax_preprocessor{0xF0, 0xAB, 0xFC};
    Rgb syntax_tag{0x67, 0xE8, 0xF9};
    Rgb syntax_attribute{0xFC, 0xD3, 0x4D};
    Rgb syntax_property{0xA5, 0xB4, 0xFC};
    Rgb syntax_heading{0xF9, 0xA8, 0xD4};
    Rgb syntax_emphasis{0xFD, 0xBA, 0x74};
    Rgb syntax_link{0x7D, 0xD3, 0xFC};
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
double contrast_ratio(Rgb foreground, Rgb background);
StyleRole style_role_for_token(highlight::TokenRole role);
TextAttributes text_attributes_for_token(highlight::TokenRole role);
std::string ansi_text_attributes_sequence(const TextAttributes& attributes);
bool set_syntax_theme_color(ThemePalette& palette, const std::string& key, const Rgb& color);
void derive_syntax_theme_colors(ThemePalette& palette);

struct ThemeCommandResult {
    bool ok = false;
    std::string message;
    std::string selected_theme;
    bool colors_enabled = true;
};

ThemeCommandResult handle_theme_command(const ThemeRegistry& registry,
                                        const std::string& current_theme,
                                        const std::string& requested,
                                        bool use_colors);

StylePair style_pair_for(const ThemePalette& palette, StyleRole role);
StylePair style_pair_for(const ThemeRegistry& registry, const std::string& theme_name, StyleRole role);
std::string style_sequence_for(const ThemeRegistry& registry, const std::string& theme_name, StyleRole role);

}  // namespace ainiux::tui
