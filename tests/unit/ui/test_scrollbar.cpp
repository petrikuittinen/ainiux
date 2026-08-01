#include "ui/test_scrollbar.hpp"

#include "support/test_support.hpp"
#include "ui/scrollbar.hpp"

namespace ainiux::test::ui {
namespace {

void test_scrollbar_content_fits() {
    const ainiux::ui::ScrollbarMetrics m =
        ainiux::ui::compute_vertical_scrollbar(20, 10, 0);
    check(m.track_height == 20, "fits: track equals viewport");
    check(m.thumb_height == 20, "fits: thumb fills track");
    check(m.thumb_start == 0, "fits: thumb starts at top");
    check(m.content_fits, "fits: content_fits true");
    check(ainiux::ui::scrollbar_thumb_covers(m, 0), "fits: thumb covers first row");
    check(ainiux::ui::scrollbar_thumb_covers(m, 19), "fits: thumb covers last row");
}

void test_scrollbar_proportional_half() {
    // viewport 10, content 20 → thumb ≈ 5
    const ainiux::ui::ScrollbarMetrics m =
        ainiux::ui::compute_vertical_scrollbar(10, 20, 0);
    check(m.track_height == 10, "half: track height");
    check(m.thumb_height == 5, "half: thumb is half of track");
    check(m.thumb_start == 0, "half: at top scroll offset");
    check(!m.content_fits, "half: content does not fit");
}

void test_scrollbar_min_thumb_height() {
    // viewport 10, content 1000 → raw thumb = 0 → clamp to 1
    const ainiux::ui::ScrollbarMetrics m =
        ainiux::ui::compute_vertical_scrollbar(10, 1000, 0);
    check(m.thumb_height == 1, "min: thumb height at least 1");
    check(m.thumb_start == 0, "min: top offset");
}

void test_scrollbar_scroll_bottom() {
    const ainiux::ui::ScrollbarMetrics top =
        ainiux::ui::compute_vertical_scrollbar(10, 40, 0);
    check(top.thumb_start == 0, "scroll: top thumb_start is 0");

    const ainiux::ui::ScrollbarMetrics bottom =
        ainiux::ui::compute_vertical_scrollbar(10, 40, 30);  // max_scroll = 30
    check(bottom.thumb_height == top.thumb_height, "scroll: thumb height stable");
    check(bottom.thumb_start + bottom.thumb_height == bottom.track_height,
          "scroll: bottom thumb ends at track bottom");
}

void test_scrollbar_scroll_mid() {
    // viewport 10, content 40, mid scroll 15 → travel = 10-2.5... thumb=max(1,100/40)=2
    // thumb_height = max(1, 10*10/40) = 2
    // travel = 8, thumb_start = 15*8/30 = 4
    const ainiux::ui::ScrollbarMetrics m =
        ainiux::ui::compute_vertical_scrollbar(10, 40, 15);
    check(m.thumb_height == 2, "mid: thumb height");
    check(m.thumb_start == 4, "mid: thumb position");
}

void test_scrollbar_zero_viewport() {
    const ainiux::ui::ScrollbarMetrics m =
        ainiux::ui::compute_vertical_scrollbar(0, 100, 5);
    check(m.track_height == 0 && m.thumb_height == 0, "zero viewport: empty metrics");
    check(ainiux::ui::scrollbar_glyph_at(m, 0).empty(), "zero viewport: no glyph");
}

void test_scrollbar_zero_content() {
    const ainiux::ui::ScrollbarMetrics m =
        ainiux::ui::compute_vertical_scrollbar(12, 0, 0);
    check(m.thumb_height == 12 && m.thumb_start == 0 && m.content_fits,
          "zero content: full thumb");
}

void test_scrollbar_glyphs() {
    const ainiux::ui::ScrollbarMetrics m =
        ainiux::ui::compute_vertical_scrollbar(5, 20, 0);
    // thumb_height = max(1, 25/20) = 1
    check(ainiux::ui::scrollbar_glyph_at(m, 0) == ainiux::ui::kScrollbarThumbGlyph,
          "glyph: thumb at start");
    check(ainiux::ui::scrollbar_glyph_at(m, 1) == ainiux::ui::kScrollbarTrackGlyph,
          "glyph: track below thumb");
    check(ainiux::ui::scrollbar_glyph_at(m, 4) == ainiux::ui::kScrollbarTrackGlyph,
          "glyph: track at end");
    check(ainiux::ui::scrollbar_glyph_at(m, 5).empty(), "glyph: OOB empty");
    check(ainiux::ui::scrollbar_glyph_at(m, -1).empty(), "glyph: negative empty");
}

void test_scrollbar_clamps_overscroll() {
    const ainiux::ui::ScrollbarMetrics m =
        ainiux::ui::compute_vertical_scrollbar(10, 30, 9999);
    check(m.thumb_start + m.thumb_height == m.track_height,
          "overscroll: clamps to bottom");
}

void test_scrollbar_equal_viewports_equal_thumb_height() {
    const ainiux::ui::ScrollbarMetrics top =
        ainiux::ui::compute_vertical_scrollbar(12, 100, 0);
    const ainiux::ui::ScrollbarMetrics bottom =
        ainiux::ui::compute_vertical_scrollbar(12, 100, 40);
    check(top.thumb_height == bottom.thumb_height,
          "equal viewports: thumb height matches regardless of scroll offset");
    check(top.thumb_start != bottom.thumb_start,
          "equal viewports: thumb position tracks scroll offset");
}

void test_scrollbar_visibility_command() {
    ainiux::ui::ScrollbarVisibilityResult result =
        ainiux::ui::handle_scrollbar_visibility("", true);
    check(result.ok && result.visible && result.message == "Scrollbar: show",
          "visibility: bare command reports shown state");

    result = ainiux::ui::handle_scrollbar_visibility("hide", true);
    check(result.ok && !result.visible && result.message == "Scrollbar hidden",
          "visibility: hide disables the scrollbar");
    result = ainiux::ui::handle_scrollbar_visibility("show", false);
    check(result.ok && result.visible && result.message == "Scrollbar shown",
          "visibility: show enables the scrollbar");
    result = ainiux::ui::handle_scrollbar_visibility("off", true);
    check(!result.ok && result.visible && result.message == "Usage: /scrollbar show|hide",
          "visibility: noncanonical values are rejected without changing state");
}

}  // namespace

void run_scrollbar_tests() {
    test_scrollbar_content_fits();
    test_scrollbar_proportional_half();
    test_scrollbar_min_thumb_height();
    test_scrollbar_scroll_bottom();
    test_scrollbar_scroll_mid();
    test_scrollbar_zero_viewport();
    test_scrollbar_zero_content();
    test_scrollbar_glyphs();
    test_scrollbar_clamps_overscroll();
    test_scrollbar_equal_viewports_equal_thumb_height();
    test_scrollbar_visibility_command();
}

}  // namespace ainiux::test::ui
