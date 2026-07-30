#pragma once

#include <cstddef>
#include <string>

namespace ainiux::ui {

// Vertical scrollbar track cell (U+2591 LIGHT SHADE).
constexpr const char kScrollbarTrackGlyph[] = u8"\u2591";
// Vertical scrollbar thumb cell (U+2588 FULL BLOCK).
constexpr const char kScrollbarThumbGlyph[] = u8"\u2588";

struct ScrollbarMetrics {
    int track_height = 0;   // viewport rows (>= 0)
    int thumb_height = 0;   // 0 when track empty; else in [1, track_height]
    int thumb_start = 0;    // first track row of thumb
    bool content_fits = true;
};

// Windows-style vertical scrollbar geometry.
// content_height = total scrollable rows, viewport_height = visible rows,
// scroll_offset = top-most visible content row (0 = top).
ScrollbarMetrics compute_vertical_scrollbar(size_t viewport_height,
                                            size_t content_height,
                                            size_t scroll_offset);

// True when track_row is inside the thumb (inclusive start, exclusive end).
bool scrollbar_thumb_covers(const ScrollbarMetrics& metrics, int track_row);

// UTF-8 glyph for one track row (thumb or track). Empty when metrics empty or row OOB.
std::string scrollbar_glyph_at(const ScrollbarMetrics& metrics, int track_row);

}  // namespace ainiux::ui
