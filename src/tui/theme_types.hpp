#pragma once

namespace pkchat::tui {

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
};

struct StylePair {
    Rgb foreground;
    Rgb background;
};

}  // namespace pkchat::tui