#include "tui/theme_registry.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>

namespace ainiux::tui {
namespace {

std::string lowercase_copy(const std::string& text) {
    std::string lower;
    lower.reserve(text.size());
    for (char ch : text) {
        lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    return lower;
}

int clamp_byte(int value) {
    if (value < 0) {
        return 0;
    }
    if (value > 255) {
        return 255;
    }
    return value;
}

// Distance in simple RGB space (good enough for palette quantization).
long long rgb_distance2(const Rgb& left, const Rgb& right) {
    const long long dr = static_cast<long long>(left.r) - right.r;
    const long long dg = static_cast<long long>(left.g) - right.g;
    const long long db = static_cast<long long>(left.b) - right.b;
    return dr * dr + dg * dg + db * db;
}

// xterm 256 levels for the 6x6x6 color cube.
int cube_level(int channel) {
    if (channel < 48) {
        return 0;
    }
    if (channel < 115) {
        return 1;
    }
    return (channel - 35) / 40;
}

int cube_value(int level) {
    return level == 0 ? 0 : 55 + level * 40;
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
    palette.background = {0xFF, 0xFF, 0xFF};
    palette.text = {0x00, 0x00, 0x00};
    palette.muted = {0x76, 0x76, 0x76};
    palette.thinking_trace = {0x6F, 0x6F, 0x6F};
    palette.user_label = {0x04, 0x51, 0xA5};
    palette.assistant_label = {0x00, 0x80, 0x00};
    palette.error = {0xCD, 0x31, 0x31};
    palette.status_foreground = {0xFF, 0xFF, 0xFF};
    palette.status_background = {0x00, 0x7A, 0xCC};
    palette.thinking_activity = {0x04, 0x51, 0xA5};
    palette.streaming_activity = {0x00, 0x80, 0x00};
    palette.panel_title = {0x04, 0x51, 0xA5};
    palette.panel_border = {0x61, 0x61, 0x61};
    palette.panel_hint = {0x6F, 0x6F, 0x6F};
    palette.panel_highlight = {0x79, 0x5E, 0x26};
    palette.panel_body = {0x00, 0x00, 0x00};
    palette.panel_background = {0xF3, 0xF3, 0xF3};
    // VS Code Light+ token colors, mapped to Ainiux semantic roles.
    palette.syntax_comment = {0x00, 0x80, 0x00};
    palette.syntax_keyword = {0x00, 0x00, 0xFF};
    palette.syntax_type = {0x26, 0x7F, 0x99};
    palette.syntax_string = {0xA3, 0x15, 0x15};
    palette.syntax_number = {0x09, 0x86, 0x58};
    palette.syntax_literal = {0x00, 0x00, 0xFF};
    palette.syntax_function = {0x79, 0x5E, 0x26};
    palette.syntax_variable = {0x00, 0x10, 0x80};
    palette.syntax_operator = {0x00, 0x00, 0x00};
    palette.syntax_preprocessor = {0x00, 0x00, 0xFF};
    palette.syntax_tag = {0x80, 0x00, 0x00};
    palette.syntax_attribute = {0xE5, 0x00, 0x00};
    palette.syntax_property = {0x04, 0x51, 0xA5};
    palette.syntax_heading = {0x80, 0x00, 0x00};
    palette.syntax_emphasis = {0x80, 0x00, 0x80};
    palette.syntax_link = {0x04, 0x51, 0xA5};
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

bool parse_color_mode_preference(const std::string& text, ColorModePreference& out) {
    const std::string lower = lowercase_copy(text);
    if (lower == "auto") {
        out = ColorModePreference::Auto;
        return true;
    }
    if (lower == "truecolor" || lower == "24bit" || lower == "24-bit" || lower == "true") {
        out = ColorModePreference::Truecolor;
        return true;
    }
    if (lower == "256" || lower == "256color" || lower == "ansi256") {
        out = ColorModePreference::Ansi256;
        return true;
    }
    if (lower == "16" || lower == "16color" || lower == "ansi16" || lower == "ansi") {
        out = ColorModePreference::Ansi16;
        return true;
    }
    return false;
}

const char* color_mode_preference_name(ColorModePreference preference) {
    switch (preference) {
        case ColorModePreference::Auto:
            return "auto";
        case ColorModePreference::Truecolor:
            return "truecolor";
        case ColorModePreference::Ansi256:
            return "256";
        case ColorModePreference::Ansi16:
            return "16";
    }
    return "auto";
}

namespace {

bool colorterm_is_truecolor(const char* colorterm) {
    if (colorterm == nullptr || colorterm[0] == '\0') {
        return false;
    }
    const std::string lower = lowercase_copy(colorterm);
    return lower == "truecolor" || lower == "24bit" || lower.find("truecolor") != std::string::npos ||
           lower.find("24bit") != std::string::npos;
}

bool term_suggests_256(const char* term) {
    if (term == nullptr || term[0] == '\0') {
        return false;
    }
    const std::string lower = lowercase_copy(term);
    if (lower == "dumb") {
        return false;
    }
    return lower.find("256color") != std::string::npos || lower.find("-direct") != std::string::npos ||
           lower.rfind("xterm", 0) == 0 || lower.rfind("tmux", 0) == 0 ||
           lower.rfind("screen", 0) == 0 || lower.find("rxvt") != std::string::npos ||
           lower.find("alacritty") != std::string::npos || lower.find("kitty") != std::string::npos ||
           lower.find("wezterm") != std::string::npos || lower.find("st-") == 0 || lower == "st";
}

}  // namespace

ColorMode resolve_color_mode(bool use_colors,
                             ColorModePreference preference,
                             const char* colorterm,
                             const char* term) {
    if (!use_colors) {
        return ColorMode::Off;
    }
    switch (preference) {
        case ColorModePreference::Truecolor:
            return ColorMode::Truecolor;
        case ColorModePreference::Ansi256:
            return ColorMode::Ansi256;
        case ColorModePreference::Ansi16:
            return ColorMode::Ansi16;
        case ColorModePreference::Auto:
            break;
    }
    // Prefer truecolor when the terminal advertises it (local WT, Kitty, etc.).
    // OpenSSH often does not forward COLORTERM; fall back to 256 when TERM looks
    // capable so Windows Terminal + SSH avoids semicolon-truecolor mis-parses.
    if (colorterm_is_truecolor(colorterm)) {
        return ColorMode::Truecolor;
    }
    if (term != nullptr && lowercase_copy(term) == "dumb") {
        return ColorMode::Off;
    }
    if (term_suggests_256(term)) {
        return ColorMode::Ansi256;
    }
    // Modern default: colon truecolor. Naive parsers ignore colon subparams
    // instead of applying classic SGR 31/34/41 from RGB channel values.
    return ColorMode::Truecolor;
}

ColorMode resolve_color_mode(bool use_colors, ColorModePreference preference) {
    return resolve_color_mode(use_colors, preference, std::getenv("COLORTERM"), std::getenv("TERM"));
}

int rgb_to_xterm256(const Rgb& color) {
    const int r = clamp_byte(color.r);
    const int g = clamp_byte(color.g);
    const int b = clamp_byte(color.b);

    const int ri = cube_level(r);
    const int gi = cube_level(g);
    const int bi = cube_level(b);
    const Rgb cube{cube_value(ri), cube_value(gi), cube_value(bi)};
    const int cube_index = 16 + 36 * ri + 6 * gi + bi;
    const long long cube_dist = rgb_distance2({r, g, b}, cube);

    // Grayscale ramp 232-255: 8 + 10*n for n in 0..23.
    int gray_index = 0;
    if (r + g + b != 0) {
        gray_index = (r + g + b) / 3;
    }
    int gray_level = (gray_index - 8 + 5) / 10;
    if (gray_level < 0) {
        gray_level = 0;
    }
    if (gray_level > 23) {
        gray_level = 23;
    }
    const int gray_value = 8 + gray_level * 10;
    const Rgb gray{gray_value, gray_value, gray_value};
    const int gray_code = 232 + gray_level;
    const long long gray_dist = rgb_distance2({r, g, b}, gray);

    // Also consider the classic 16 colors (0-15) for pure primaries.
    static const Rgb kAnsi16[16] = {
        {0, 0, 0},       {205, 0, 0},     {0, 205, 0},     {205, 205, 0},
        {0, 0, 238},     {205, 0, 205},   {0, 205, 205},   {229, 229, 229},
        {127, 127, 127}, {255, 0, 0},     {0, 255, 0},     {255, 255, 0},
        {92, 92, 255},   {255, 0, 255},   {0, 255, 255},   {255, 255, 255},
    };
    int best_index = cube_index;
    long long best_dist = cube_dist;
    if (gray_dist < best_dist) {
        best_dist = gray_dist;
        best_index = gray_code;
    }
    for (int i = 0; i < 16; ++i) {
        const long long dist = rgb_distance2({r, g, b}, kAnsi16[i]);
        if (dist < best_dist) {
            best_dist = dist;
            best_index = i;
        }
    }
    return best_index;
}

int rgb_to_ansi16(const Rgb& color) {
    static const Rgb kAnsi16[16] = {
        {0, 0, 0},       {205, 0, 0},     {0, 205, 0},     {205, 205, 0},
        {0, 0, 238},     {205, 0, 205},   {0, 205, 205},   {229, 229, 229},
        {127, 127, 127}, {255, 0, 0},     {0, 255, 0},     {255, 255, 0},
        {92, 92, 255},   {255, 0, 255},   {0, 255, 255},   {255, 255, 255},
    };
    const Rgb clamped{clamp_byte(color.r), clamp_byte(color.g), clamp_byte(color.b)};
    int best = 7;
    long long best_dist = rgb_distance2(clamped, kAnsi16[7]);
    for (int i = 0; i < 16; ++i) {
        const long long dist = rgb_distance2(clamped, kAnsi16[i]);
        if (dist < best_dist) {
            best_dist = dist;
            best = i;
        }
    }
    return best;
}

std::string ansi_foreground_sequence(const Rgb& color, ColorMode mode) {
    switch (mode) {
        case ColorMode::Off:
            return {};
        case ColorMode::Truecolor:
            // Colon subparameters: naive classic-SGR parsers do not split on ':'
            // and will not apply RGB channel values as codes 31/34/41.
            return "\x1b[38:2:" + std::to_string(clamp_byte(color.r)) + ":" +
                   std::to_string(clamp_byte(color.g)) + ":" + std::to_string(clamp_byte(color.b)) +
                   "m";
        case ColorMode::Ansi256:
            return "\x1b[38;5;" + std::to_string(rgb_to_xterm256(color)) + "m";
        case ColorMode::Ansi16: {
            const int index = rgb_to_ansi16(color);
            if (index < 8) {
                return "\x1b[" + std::to_string(30 + index) + "m";
            }
            return "\x1b[" + std::to_string(90 + (index - 8)) + "m";
        }
    }
    return {};
}

std::string ansi_background_sequence(const Rgb& color, ColorMode mode) {
    switch (mode) {
        case ColorMode::Off:
            return {};
        case ColorMode::Truecolor:
            return "\x1b[48:2:" + std::to_string(clamp_byte(color.r)) + ":" +
                   std::to_string(clamp_byte(color.g)) + ":" + std::to_string(clamp_byte(color.b)) +
                   "m";
        case ColorMode::Ansi256:
            return "\x1b[48;5;" + std::to_string(rgb_to_xterm256(color)) + "m";
        case ColorMode::Ansi16: {
            const int index = rgb_to_ansi16(color);
            if (index < 8) {
                return "\x1b[" + std::to_string(40 + index) + "m";
            }
            return "\x1b[" + std::to_string(100 + (index - 8)) + "m";
        }
    }
    return {};
}

std::string ansi_style_sequence(const StylePair& pair, ColorMode mode) {
    if (mode == ColorMode::Off) {
        return {};
    }
    return ansi_foreground_sequence(pair.foreground, mode) +
           ansi_background_sequence(pair.background, mode);
}

StyleRole style_role_for_token(highlight::TokenRole role) {
    switch (role) {
        case highlight::TokenRole::Comment:
            return StyleRole::SyntaxComment;
        case highlight::TokenRole::Keyword:
            return StyleRole::SyntaxKeyword;
        case highlight::TokenRole::Type:
            return StyleRole::SyntaxType;
        case highlight::TokenRole::String:
            return StyleRole::SyntaxString;
        case highlight::TokenRole::Number:
            return StyleRole::SyntaxNumber;
        case highlight::TokenRole::Literal:
            return StyleRole::SyntaxLiteral;
        case highlight::TokenRole::Function:
            return StyleRole::SyntaxFunction;
        case highlight::TokenRole::Variable:
            return StyleRole::SyntaxVariable;
        case highlight::TokenRole::Operator:
            return StyleRole::SyntaxOperator;
        case highlight::TokenRole::Preprocessor:
            return StyleRole::SyntaxPreprocessor;
        case highlight::TokenRole::Tag:
            return StyleRole::SyntaxTag;
        case highlight::TokenRole::Attribute:
            return StyleRole::SyntaxAttribute;
        case highlight::TokenRole::Property:
            return StyleRole::SyntaxProperty;
        case highlight::TokenRole::Heading:
            return StyleRole::SyntaxHeading;
        case highlight::TokenRole::Emphasis:
        case highlight::TokenRole::Strong:
        case highlight::TokenRole::StrongEmphasis:
        case highlight::TokenRole::Strikethrough:
            return StyleRole::SyntaxEmphasis;
        case highlight::TokenRole::Link:
            return StyleRole::SyntaxLink;
        case highlight::TokenRole::LinkDestination:
            return StyleRole::SyntaxAttribute;
    }
    return StyleRole::Text;
}

TextAttributes text_attributes_for_token(highlight::TokenRole role) {
    TextAttributes attributes;
    attributes.bold = role == highlight::TokenRole::Heading ||
                      role == highlight::TokenRole::Strong ||
                      role == highlight::TokenRole::StrongEmphasis;
    attributes.italic = role == highlight::TokenRole::Emphasis ||
                        role == highlight::TokenRole::StrongEmphasis;
    attributes.underline = role == highlight::TokenRole::Link ||
                           role == highlight::TokenRole::LinkDestination;
    return attributes;
}

std::string ansi_text_attributes_sequence(const TextAttributes& attributes) {
    std::string sequence;
    if (attributes.bold) {
        sequence += "\x1b[1m";
    }
    if (attributes.italic) {
        sequence += "\x1b[3m";
    }
    if (attributes.underline) {
        sequence += "\x1b[4m";
    }
    return sequence;
}

bool set_syntax_theme_color(ThemePalette& palette, const std::string& key, const Rgb& color) {
    if (key == "syntax_comment") palette.syntax_comment = color;
    else if (key == "syntax_keyword") palette.syntax_keyword = color;
    else if (key == "syntax_type") palette.syntax_type = color;
    else if (key == "syntax_string") palette.syntax_string = color;
    else if (key == "syntax_number") palette.syntax_number = color;
    else if (key == "syntax_literal") palette.syntax_literal = color;
    else if (key == "syntax_function") palette.syntax_function = color;
    else if (key == "syntax_variable") palette.syntax_variable = color;
    else if (key == "syntax_operator") palette.syntax_operator = color;
    else if (key == "syntax_preprocessor") palette.syntax_preprocessor = color;
    else if (key == "syntax_tag") palette.syntax_tag = color;
    else if (key == "syntax_attribute") palette.syntax_attribute = color;
    else if (key == "syntax_property") palette.syntax_property = color;
    else if (key == "syntax_heading") palette.syntax_heading = color;
    else if (key == "syntax_emphasis") palette.syntax_emphasis = color;
    else if (key == "syntax_link") palette.syntax_link = color;
    else return false;
    return true;
}

void derive_syntax_theme_colors(ThemePalette& palette) {
    auto accessible = [&](Rgb candidate) {
        if (contrast_ratio(candidate, palette.background) >= 4.5) {
            return candidate;
        }
        const Rgb black{0, 0, 0};
        const Rgb white{255, 255, 255};
        return contrast_ratio(black, palette.background) >= contrast_ratio(white, palette.background)
                   ? black
                   : white;
    };
    palette.syntax_comment = accessible(palette.muted);
    palette.syntax_keyword = accessible(palette.panel_title);
    palette.syntax_type = accessible(palette.user_label);
    palette.syntax_string = accessible(palette.assistant_label);
    palette.syntax_number = accessible(palette.panel_highlight);
    palette.syntax_literal = accessible(palette.error);
    palette.syntax_function = accessible(palette.thinking_activity);
    palette.syntax_variable = accessible(palette.user_label);
    palette.syntax_operator = accessible(palette.text);
    palette.syntax_preprocessor = accessible(palette.panel_title);
    palette.syntax_tag = accessible(palette.user_label);
    palette.syntax_attribute = accessible(palette.panel_highlight);
    palette.syntax_property = accessible(palette.thinking_activity);
    palette.syntax_heading = accessible(palette.user_label);
    palette.syntax_emphasis = accessible(palette.panel_highlight);
    palette.syntax_link = accessible(palette.user_label);
}

ThemeCommandResult handle_theme_command(const ThemeRegistry& registry,
                                        const std::string& current_theme,
                                        const std::string& requested,
                                        bool use_colors) {
    ThemeCommandResult result;
    result.selected_theme = current_theme;
    result.colors_enabled = use_colors;
    if (requested.empty()) {
        result.ok = true;
        result.message = "Theme: " + std::string(use_colors ? current_theme : "off") +
                         ". Available: off, " + format_theme_list(registry);
        return result;
    }

    std::string requested_lower = requested;
    for (char& ch : requested_lower) {
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    }
    if (requested_lower == "off") {
        result.ok = true;
        result.colors_enabled = false;
        result.message = "Theme set to off";
        return result;
    }

    std::string normalized;
    if (!registry.normalize_name(requested, normalized)) {
        result.message = "Unknown theme: " + requested + ". Available: " + format_theme_list(registry);
        return result;
    }
    result.ok = true;
    result.selected_theme = normalized;
    result.colors_enabled = true;
    result.message = "Theme set to " + normalized;
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
        case StyleRole::SyntaxComment:
            return {palette.syntax_comment, palette.background};
        case StyleRole::SyntaxKeyword:
            return {palette.syntax_keyword, palette.background};
        case StyleRole::SyntaxType:
            return {palette.syntax_type, palette.background};
        case StyleRole::SyntaxString:
            return {palette.syntax_string, palette.background};
        case StyleRole::SyntaxNumber:
            return {palette.syntax_number, palette.background};
        case StyleRole::SyntaxLiteral:
            return {palette.syntax_literal, palette.background};
        case StyleRole::SyntaxFunction:
            return {palette.syntax_function, palette.background};
        case StyleRole::SyntaxVariable:
            return {palette.syntax_variable, palette.background};
        case StyleRole::SyntaxOperator:
            return {palette.syntax_operator, palette.background};
        case StyleRole::SyntaxPreprocessor:
            return {palette.syntax_preprocessor, palette.background};
        case StyleRole::SyntaxTag:
            return {palette.syntax_tag, palette.background};
        case StyleRole::SyntaxAttribute:
            return {palette.syntax_attribute, palette.background};
        case StyleRole::SyntaxProperty:
            return {palette.syntax_property, palette.background};
        case StyleRole::SyntaxHeading:
            return {palette.syntax_heading, palette.background};
        case StyleRole::SyntaxEmphasis:
            return {palette.syntax_emphasis, palette.background};
        case StyleRole::SyntaxLink:
            return {palette.syntax_link, palette.background};
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

std::string style_sequence_for(const ThemeRegistry& registry,
                               const std::string& theme_name,
                               StyleRole role,
                               ColorMode mode) {
    return ansi_style_sequence(style_pair_for(registry, theme_name, role), mode);
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

}  // namespace ainiux::tui
