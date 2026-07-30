#include "ui/scrollbar.hpp"

#include <algorithm>

namespace ainiux::ui {

ScrollbarMetrics compute_vertical_scrollbar(size_t viewport_height,
                                            size_t content_height,
                                            size_t scroll_offset) {
    ScrollbarMetrics metrics;
    if (viewport_height == 0) {
        return metrics;
    }

    metrics.track_height = static_cast<int>(viewport_height);

    if (content_height == 0 || content_height <= viewport_height) {
        metrics.thumb_height = metrics.track_height;
        metrics.thumb_start = 0;
        metrics.content_fits = true;
        return metrics;
    }

    metrics.content_fits = false;
    // thumb_height = max(1, viewport * viewport / content)
    const size_t raw_thumb =
        (viewport_height * viewport_height) / content_height;
    metrics.thumb_height =
        static_cast<int>(std::max<size_t>(1, std::min(raw_thumb, viewport_height)));

    const size_t max_scroll = content_height - viewport_height;
    const size_t clamped_scroll = std::min(scroll_offset, max_scroll);
    const int travel = metrics.track_height - metrics.thumb_height;
    if (travel <= 0) {
        metrics.thumb_start = 0;
        metrics.thumb_height = metrics.track_height;
        return metrics;
    }
    metrics.thumb_start =
        static_cast<int>((clamped_scroll * static_cast<size_t>(travel)) / max_scroll);
    // Keep thumb fully inside the track under integer rounding.
    if (metrics.thumb_start + metrics.thumb_height > metrics.track_height) {
        metrics.thumb_start = metrics.track_height - metrics.thumb_height;
    }
    if (metrics.thumb_start < 0) {
        metrics.thumb_start = 0;
    }
    return metrics;
}

bool scrollbar_thumb_covers(const ScrollbarMetrics& metrics, int track_row) {
    if (metrics.track_height <= 0 || metrics.thumb_height <= 0) {
        return false;
    }
    return track_row >= metrics.thumb_start &&
           track_row < metrics.thumb_start + metrics.thumb_height;
}

std::string scrollbar_glyph_at(const ScrollbarMetrics& metrics, int track_row) {
    if (metrics.track_height <= 0 || track_row < 0 || track_row >= metrics.track_height) {
        return {};
    }
    return scrollbar_thumb_covers(metrics, track_row) ? std::string(kScrollbarThumbGlyph)
                                                      : std::string(kScrollbarTrackGlyph);
}

}  // namespace ainiux::ui
