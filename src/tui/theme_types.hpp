#pragma once

namespace ainiux::tui {

struct Rgb {
    int r = 0;
    int g = 0;
    int b = 0;
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
