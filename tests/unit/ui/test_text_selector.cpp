#include "ui/test_text_selector.hpp"

#include "editor/selection.hpp"
#include "support/test_support.hpp"
#include "ui/text_selector.hpp"

namespace pkchat::test::ui {
namespace {

void test_text_selector_rendering() {
    const std::vector<std::string> items = {"alpha", "beta", "gamma"};
    pkchat::ui::TextSelectorConfig config;
    config.header = "Pick one";
    const std::string rendered = pkchat::ui::render_text_selector(config, 1, items);
    check(rendered.find("Pick one\n") == 0, "text selector renders header line first");
    check(rendered.find(u8"  alpha\n") != std::string::npos, "text selector renders unselected prefix");
    check(rendered.find(u8"› beta\n") != std::string::npos, "text selector renders selected arrow prefix");
    check(rendered.find(u8"  gamma") != std::string::npos, "text selector omits trailing newline on last item");

    config.selected_prefix = pkchat::ui::kTextSelectorCaretPrefix;
    const std::string caret_rendered =
        pkchat::ui::render_text_selector(config, 0, 1, [](size_t) { return "only"; });
    check(caret_rendered.find("> only") != std::string::npos,
          "text selector label callback supports custom selected prefix");
}

void test_text_selector_movement() {
    constexpr size_t count = 12;
    check(pkchat::ui::move_text_selector_selection(0, count, editor::MovementKey::Down) == 1,
          "text selector moves selection down");
    check(pkchat::ui::move_text_selector_selection(1, count, editor::MovementKey::Up) == 0,
          "text selector moves selection up");
    check(pkchat::ui::move_text_selector_selection(5, count, editor::MovementKey::PageUp) == 0,
          "text selector page up clamps at top");
    check(pkchat::ui::move_text_selector_selection(5, count, editor::MovementKey::PageDown) == 11,
          "text selector page down advances by default step");
    check(pkchat::ui::move_text_selector_selection(3, count, editor::MovementKey::Home) == 0,
          "text selector home selects first item");
    check(pkchat::ui::move_text_selector_selection(3, count, editor::MovementKey::End) == 11,
          "text selector end selects last item");
    check(pkchat::ui::move_text_selector_selection(99, count, editor::MovementKey::Up) == 10,
          "text selector clamps out-of-range selection before moving");
    check(pkchat::ui::move_text_selector_selection(2, 0, editor::MovementKey::Down) == 0,
          "text selector movement on empty list stays at zero");
}

void test_text_selector_status() {
    check(pkchat::ui::text_selector_status("Selected model", 0, 3) == "Selected model 1/3",
          "text selector status formats one-based position");
    check(pkchat::ui::text_selector_status("Selected thread", 2, 3) == "Selected thread 3/3",
          "text selector status uses clamped selection");
    check(pkchat::ui::text_selector_status("Selected provider", 0, 0) == "Selected provider",
          "text selector status omits position for empty lists");
}

}  // namespace

void run_all() {
    test_text_selector_rendering();
    test_text_selector_movement();
    test_text_selector_status();
}

}  // namespace pkchat::test::ui