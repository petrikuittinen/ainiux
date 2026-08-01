#include "ui/test_text_selector.hpp"

#include "editor/selection.hpp"
#include "support/test_support.hpp"
#include "ui/confirmation.hpp"
#include "ui/provider_model_display.hpp"
#include "ui/provider_model_selector.hpp"
#include "ui/test_scrollbar.hpp"
#include "ui/text_selector.hpp"

#include <algorithm>

namespace ainiux::test::ui {
namespace {

void test_text_selector_rendering() {
    const std::vector<std::string> items = {"alpha", "beta", "gamma"};
    ainiux::ui::TextSelectorConfig config;
    config.header = "Pick one";
    const std::string rendered = ainiux::ui::render_text_selector(config, 1, items);
    check(rendered.find("Pick one\n") == 0, "text selector renders header line first");
    check(rendered.find(u8"  alpha\n") != std::string::npos, "text selector renders unselected prefix");
    check(rendered.find(u8"› beta\n") != std::string::npos, "text selector renders selected arrow prefix");
    check(rendered.find(u8"  gamma") != std::string::npos, "text selector omits trailing newline on last item");

    config.selected_prefix = ainiux::ui::kTextSelectorCaretPrefix;
    const std::string caret_rendered =
        ainiux::ui::render_text_selector(config, 0, 1, [](size_t) { return "only"; });
    check(caret_rendered.find("> only") != std::string::npos,
          "text selector label callback supports custom selected prefix");
}

void test_text_selector_movement() {
    constexpr size_t count = 12;
    check(ainiux::ui::move_text_selector_selection(0, count, editor::MovementKey::Down) == 1,
          "text selector moves selection down");
    check(ainiux::ui::move_text_selector_selection(1, count, editor::MovementKey::Up) == 0,
          "text selector moves selection up");
    check(ainiux::ui::move_text_selector_selection(5, count, editor::MovementKey::PageUp) == 0,
          "text selector page up clamps at top");
    check(ainiux::ui::move_text_selector_selection(5, count, editor::MovementKey::PageDown) == 11,
          "text selector page down advances by default step");
    check(ainiux::ui::move_text_selector_selection(3, count, editor::MovementKey::Home) == 0,
          "text selector home selects first item");
    check(ainiux::ui::move_text_selector_selection(3, count, editor::MovementKey::End) == 11,
          "text selector end selects last item");
    check(ainiux::ui::move_text_selector_selection(99, count, editor::MovementKey::Up) == 10,
          "text selector clamps out-of-range selection before moving");
    check(ainiux::ui::move_text_selector_selection(2, 0, editor::MovementKey::Down) == 0,
          "text selector movement on empty list stays at zero");
}

void test_text_selector_type_ahead_jump() {
    const std::vector<std::string> items = {
        "openrouter", "openai", "deepseek", "llama", "lmstudio"};
    auto label_at = [&](size_t index) { return items[index]; };

    size_t selected = 0;
    check(ainiux::ui::jump_text_selector_by_char(selected, items.size(), label_at, 'a') &&
              selected == 1,
          "type-ahead A from openrouter jumps to openai (contains a)");
    check(ainiux::ui::jump_text_selector_by_char(selected, items.size(), label_at, 'A') &&
              selected == 3,
          "type-ahead A again jumps to llama and is case-insensitive");
    check(ainiux::ui::jump_text_selector_by_char(selected, items.size(), label_at, 'a') &&
              selected == 1,
          "type-ahead A wraps from llama back to openai");

    selected = 0;
    check(!ainiux::ui::jump_text_selector_by_char(selected, items.size(), label_at, 'z') &&
              selected == 0,
          "type-ahead with no matches leaves selection unchanged");
    check(!ainiux::ui::jump_text_selector_by_char(selected, items.size(), label_at, '\t') &&
              selected == 0,
          "type-ahead ignores Tab (control character)");
    check(!ainiux::ui::jump_text_selector_by_char(selected, 0, label_at, 'a') && selected == 0,
          "type-ahead on empty list is a no-op");
}

void test_confirmation_helpers() {
    check(ainiux::ui::yes_answer("y"), "confirmation accepts lowercase y");
    check(ainiux::ui::no_answer("n"), "confirmation accepts lowercase n");
    check(ainiux::ui::parse_confirmation_key('y') == ainiux::ui::ConfirmationKeyResult::Accepted,
          "confirmation key parser accepts y");
    check(ainiux::ui::parse_confirmation_key(27) == ainiux::ui::ConfirmationKeyResult::Rejected,
          "confirmation key parser treats Esc as reject");
}

void test_text_selector_escape_sequence() {
    size_t selected = 0;
    std::string status;
    const ainiux::ui::SelectorMovementResult cancelled =
        ainiux::ui::handle_selector_escape_sequence("", 3, selected, status, "Selected model");
    check(cancelled == ainiux::ui::SelectorMovementResult::Cancelled,
          "selector escape sequence cancels on empty suffix");
    check(selected == 0, "selector escape sequence leaves selection unchanged on cancel");

    const ainiux::ui::SelectorMovementResult navigated =
        ainiux::ui::handle_selector_escape_sequence("[B", 3, selected, status, "Selected model");
    check(navigated == ainiux::ui::SelectorMovementResult::Navigated,
          "selector escape sequence navigates on movement suffix");
    check(selected == 1, "selector escape sequence moves selection down");
    check(status == "Selected model 2/3", "selector escape sequence updates status line");
}

void test_text_selector_status() {
    check(ainiux::ui::text_selector_status("Selected model", 0, 3) == "Selected model 1/3",
          "text selector status formats one-based position");
    check(ainiux::ui::text_selector_status("Selected thread", 2, 3) == "Selected thread 3/3",
          "text selector status uses clamped selection");
    check(ainiux::ui::text_selector_status("Selected provider", 0, 0) == "Selected provider",
          "text selector status omits position for empty lists");
}

void test_provider_model_selectors() {
    const std::vector<std::string> providers = ainiux::ui::selectable_provider_ids();
    check(!providers.empty(), "provider selector includes at least one provider");
    check(std::find(providers.begin(), providers.end(), "none") == providers.end(),
          "provider selector excludes the offline none profile");
    check(std::find(providers.begin(), providers.end(), "custom_openai_chat") == providers.end(),
          "provider selector excludes custom_openai_chat without a base URL");
    check(std::find(providers.begin(), providers.end(), "lm_studio") != providers.end(),
          "provider selector includes lm_studio");

    const std::string provider_text =
        ainiux::ui::provider_selector_text({"lm_studio", "openai"}, 1);
    check(provider_text.find("Enter select") != std::string::npos,
          "provider selector documents Enter selection");
    check(provider_text.find(u8"› openai") != std::string::npos,
          "provider selector highlights the selected provider");

    const std::vector<std::string> models = {"alpha", "beta"};
    const std::string model_text = ainiux::ui::model_selector_text(models, 0);
    check(model_text.find(u8"› alpha") != std::string::npos,
          "model selector highlights the selected model");
    check(ainiux::ui::should_auto_select_only_model({"only"}),
          "one returned model is eligible for automatic selection");
    check(!ainiux::ui::should_auto_select_only_model(models),
          "multiple returned models require the selector");
}

void test_provider_model_display() {
    check(ainiux::ui::compact_model_name_for_display(
              "models/gemini-3.1-flash-lite-preview") ==
              u8"gemini-3.1-flash-lite-pre…",
          "model display strips a provider path prefix and truncates to 26 characters");
    check(ainiux::ui::compact_model_name_for_display(
              "accounts/example/models/gemini-flash") == "gemini-flash",
          "model display strips through the final slash");
    check(ainiux::ui::compact_model_name_for_display("short-model") == "short-model",
          "model display preserves a short unprefixed model name");

    const std::string family = u8"👨‍👩‍👧‍👦";
    const std::string limit_graphemes =
        std::string(ainiux::ui::kModelDisplayCharacterLimit - 1, 'a') + family;
    check(ainiux::ui::compact_model_name_for_display(limit_graphemes) == limit_graphemes,
          "model display counts a joined Unicode sequence as one character");
    check(ainiux::ui::compact_model_name_for_display(limit_graphemes + "x") ==
              std::string(ainiux::ui::kModelDisplayCharacterLimit - 1, 'a') + u8"…",
          "model display truncation does not split a Unicode grapheme");
    check(ainiux::ui::provider_model_display_label(
              "gemini", "models/gemini-3.1-flash-lite-preview") ==
              u8"[gemini/gemini-3.1-flash-lite-pre…]",
          "shared provider/model label uses the compact model display name");
    check(ainiux::ui::provider_model_display_label(
              "lm_studio", "publisher/family/qwen-local") ==
              "[lmstudio/qwen-local]",
          "shared label resolves canonical providers to the preferred alias");
    check(ainiux::ui::provider_model_display_label(
              "lmstudio", "qwen-local") == "[lmstudio/qwen-local]",
          "shared label resolves registered aliases identically");
    check(ainiux::ui::provider_model_display_label(
              "https://localhost:1234/v1", "qwen-local") == "[custom/qwen-local]" &&
              ainiux::ui::provider_model_display_label(
                  "custom_openai_chat", "qwen-local") == "[custom/qwen-local]",
          "shared label maps URL and custom profiles to custom");
    check(ainiux::ui::provider_model_display_label(
              "openrouter", "google/gemini-3.1-pro", "auto") ==
              "[openrouter/gemini-3.1-pro auto]",
          "shared label includes reasoning without slash whitespace");
    check(ainiux::ui::provider_model_display_label("", "model") == "[model]" &&
              ainiux::ui::provider_model_display_label("openai", "") ==
                  "[openai/model unknown]" &&
              ainiux::ui::provider_model_display_label("", "").empty(),
          "shared label preserves missing-value fallbacks");
}

}  // namespace

void run_all() {
    test_confirmation_helpers();
    test_text_selector_rendering();
    test_text_selector_movement();
    test_text_selector_type_ahead_jump();
    test_text_selector_escape_sequence();
    test_text_selector_status();
    test_provider_model_selectors();
    test_provider_model_display();
    run_scrollbar_tests();
}

}  // namespace ainiux::test::ui
