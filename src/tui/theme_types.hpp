#pragma once

#include <string>

namespace ainiux::tui {

struct Rgb {
    int r = 0;
    int g = 0;
    int b = 0;
};

// Preferred wire format for theme RGB values (config/CLI). Auto is resolved
// from COLORTERM/TERM at TUI/editor start; palette data stays full RGB.
enum class ColorModePreference {
    Auto,
    Truecolor,
    Ansi256,
    Ansi16,
};

// Resolved emission mode used by sequence builders.
enum class ColorMode {
    Off,
    Truecolor,
    Ansi256,
    Ansi16,
};

enum class StyleRole {
    Text,
    Muted,
    ThinkingTrace,
    UserLabel,
    AssistantLabel,
    Error,
    Status,
    InputLabel,
    ThinkingActivity,
    StreamingActivity,
    PanelTitle,
    PanelBorder,
    PanelHint,
    PanelHighlight,
    PanelBody,
    SyntaxComment,
    SyntaxKeyword,
    SyntaxType,
    SyntaxString,
    SyntaxNumber,
    SyntaxLiteral,
    SyntaxFunction,
    SyntaxVariable,
    SyntaxOperator,
    SyntaxPreprocessor,
    SyntaxTag,
    SyntaxAttribute,
    SyntaxProperty,
    SyntaxHeading,
    SyntaxEmphasis,
    SyntaxLink,
};

struct StylePair {
    Rgb foreground;
    Rgb background;
};

struct TextAttributes {
    bool bold = false;
    bool italic = false;
    bool underline = false;
};

}  // namespace ainiux::tui
