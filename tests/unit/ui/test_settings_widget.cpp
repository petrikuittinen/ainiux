#include "ui/test_settings_widget.hpp"

#include <string>

#include "support/test_support.hpp"
#include "ui/settings_widget.hpp"

#include "ainiux/model_setting.hpp"
#include "chat/settings.hpp"
#include "chat/settings_fields.hpp"
#include "editor/selection.hpp"

namespace ainiux::test::ui_settings {
namespace {

using ainiux::test::check;

void test_visible_fields_filter_by_surface() {
    const auto chat = ainiux::chat::visible_settings_fields(ainiux::chat::SettingsSurface::Chat);
    const auto agent = ainiux::chat::visible_settings_fields(ainiux::chat::SettingsSurface::Agent);
    const auto editor = ainiux::chat::visible_settings_fields(ainiux::chat::SettingsSurface::Editor);
    auto has = [](const std::vector<const ainiux::chat::SettingsFieldSpec*>& fields, const char* id) {
        for (const ainiux::chat::SettingsFieldSpec* field : fields) {
            if (std::string(field->id) == id) return true;
        }
        return false;
    };
    check(has(chat, "temperature") && has(chat, "highlight") &&
              !has(chat, "cmd-out") && !has(chat, "tab-width") && !has(chat, "purpose"),
          "chat settings omit agent, editor, and purpose fields");
    check(has(agent, "cmd-out") && has(agent, "thinking_preview_max_chars") &&
              !has(agent, "tab-width"),
          "agent settings include agent-only rows");
    check(has(editor, "tab-width") && has(editor, "temperature") && !has(editor, "cmd-out"),
          "editor settings include buffer rows and shared generation rows");
}

void test_widget_numeric_and_choice_edit() {
    ainiux::cli::Options options;
    ainiux::ui::SettingsWidget widget;
    ainiux::ui::open_settings_widget(widget, ainiux::chat::SettingsSurface::Chat, options);
    check(widget.active && widget.selected == 0, "settings widget opens on the first field");

    ainiux::ui::handle_settings_widget_key(widget, 'a');
    check(widget.row_draft.empty(), "letters are rejected for numeric temperature");

    ainiux::ui::handle_settings_widget_key(widget, '0');
    ainiux::ui::handle_settings_widget_key(widget, '.');
    ainiux::ui::handle_settings_widget_key(widget, '7');
    check(widget.row_editing && widget.row_draft == "0.7", "numeric draft accepts 0.7");
    ainiux::ui::handle_settings_widget_key(widget, '\r');
    check(!widget.row_editing && widget.draft.has_temperature && widget.draft.temperature == 0.7,
          "Enter commits the temperature draft");
    check(!options.has_temperature, "live options stay unchanged until save");

    ainiux::ui::handle_settings_widget_key(widget, '2');
    check(widget.row_draft == "0.7" || !widget.row_editing || widget.row_draft != "2",
          "digit 2 is rejected when the catalog max is 1.0 during incremental typing");

    ainiux::ui::cancel_settings_widget_row(widget);
    check(widget.draft.temperature == 0.7, "Esc leaves the last committed draft value");

    ainiux::ui::handle_settings_widget_movement(widget, ainiux::editor::MovementKey::Down);
    ainiux::ui::handle_settings_widget_movement(widget, ainiux::editor::MovementKey::Down);
    ainiux::ui::handle_settings_widget_movement(widget, ainiux::editor::MovementKey::Down);
    ainiux::ui::handle_settings_widget_movement(widget, ainiux::editor::MovementKey::Down);
    ainiux::ui::handle_settings_widget_movement(widget, ainiux::editor::MovementKey::Down);
    ainiux::ui::handle_settings_widget_movement(widget, ainiux::editor::MovementKey::Down);
    ainiux::ui::handle_settings_widget_movement(widget, ainiux::editor::MovementKey::Down);
    // stream is the last model-group choice after reasoning
    const ainiux::chat::SettingsFieldSpec* field = ainiux::ui::selected_settings_field(widget);
    check(field != nullptr && std::string(field->id) == "reasoning",
          "seven downs land on reasoning");
    ainiux::ui::handle_settings_widget_movement(widget, ainiux::editor::MovementKey::Down);
    field = ainiux::ui::selected_settings_field(widget);
    check(field != nullptr && std::string(field->id) == "stream", "next row is stream");
    ainiux::ui::handle_settings_widget_movement(widget, ainiux::editor::MovementKey::Right);
    check(widget.draft.stream == false, "right arrow cycles stream off");
    ainiux::ui::handle_settings_widget_movement(widget, ainiux::editor::MovementKey::Left);
    check(widget.draft.stream, "left arrow cycles stream back on");
}

void test_widget_save_and_quit() {
    ainiux::cli::Options options;
    ainiux::ui::SettingsWidget widget;
    ainiux::ui::open_settings_widget(widget, ainiux::chat::SettingsSurface::Chat, options);
    ainiux::ui::handle_settings_widget_key(widget, '0');
    ainiux::ui::handle_settings_widget_key(widget, '.');
    ainiux::ui::handle_settings_widget_key(widget, '4');
    const ainiux::ui::SettingsWidgetAction saved =
        ainiux::ui::handle_settings_widget_key(widget, 's');
    check(saved == ainiux::ui::SettingsWidgetAction::Saved && widget.draft.has_temperature &&
              widget.draft.temperature == 0.4,
          "s commits the row and reports Saved");

    ainiux::ui::open_settings_widget(widget, ainiux::chat::SettingsSurface::Chat, options);
    ainiux::ui::handle_settings_widget_key(widget, '0');
    ainiux::ui::handle_settings_widget_key(widget, '.');
    ainiux::ui::handle_settings_widget_key(widget, '3');
    const ainiux::ui::SettingsWidgetAction quit =
        ainiux::ui::handle_settings_widget_key(widget, 'q');
    check(quit == ainiux::ui::SettingsWidgetAction::Quit && !options.has_temperature,
          "q reports Quit without mutating the original options");
}

void test_widget_empty_clears_optional() {
    ainiux::cli::Options options;
    options.has_temperature = true;
    options.temperature = 0.5;
    ainiux::ui::SettingsWidget widget;
    ainiux::ui::open_settings_widget(widget, ainiux::chat::SettingsSurface::Chat, options);
    ainiux::ui::handle_settings_widget_key(widget, 127);
    ainiux::ui::handle_settings_widget_key(widget, 127);
    ainiux::ui::handle_settings_widget_key(widget, 127);
    ainiux::ui::handle_settings_widget_key(widget, '\r');
    check(!widget.draft.has_temperature, "empty Enter clears an optional numeric override");
}

void test_widget_gemini_temperature() {
    ainiux::cli::Options options;
    ainiux::ModelCapability gemini;
    gemini.id = "google-gemini-3";
    gemini.model_regex = "^gemini-3(?:[.][0-9]+)?(?:-[a-z0-9]+)*$";
    gemini.temperature_max = 2.0;
    options.model_catalog.models.push_back(gemini);
    options.model = "gemini-3-pro";
    ainiux::ui::SettingsWidget widget;
    ainiux::ui::open_settings_widget(widget, ainiux::chat::SettingsSurface::Chat, options);
    ainiux::ui::handle_settings_widget_key(widget, '1');
    ainiux::ui::handle_settings_widget_key(widget, '.');
    ainiux::ui::handle_settings_widget_key(widget, '5');
    ainiux::ui::handle_settings_widget_key(widget, '\r');
    check(widget.draft.has_temperature && widget.draft.temperature == 1.5,
          "Gemini catalog max allows typing 1.5");
}

void test_widget_render_frame() {
    ainiux::cli::Options options;
    ainiux::ui::SettingsWidget widget;
    ainiux::ui::open_settings_widget(widget, ainiux::chat::SettingsSurface::Chat, options);
    ainiux::ui::handle_settings_widget_key(widget, '0');
    const std::vector<ainiux::tui::StyledLine> lines =
        ainiux::ui::render_settings_widget(widget, 72);
    check(!lines.empty(), "settings widget renders lines");
    bool saw_border = false;
    bool saw_title = false;
    bool saw_group = false;
    bool saw_highlight = false;
    bool saw_hint = false;
    for (const ainiux::tui::StyledLine& line : lines) {
        for (const ainiux::tui::StyledSegment& segment : line.segments) {
            if (segment.text.find(u8"┌") != std::string::npos ||
                segment.text.find(u8"└") != std::string::npos) {
                saw_border = true;
            }
            if (segment.text.find("Settings") != std::string::npos) saw_title = true;
            if (segment.text.find("Model") != std::string::npos) saw_group = true;
            if (segment.role == ainiux::tui::StyleRole::PanelHighlight) saw_highlight = true;
            if (segment.role == ainiux::tui::StyleRole::PanelHint) saw_hint = true;
        }
    }
    check(saw_border && saw_title && saw_group && saw_highlight && saw_hint,
          "settings widget paints a framed, grouped, highlighted list with hints");
    check(ainiux::ui::settings_widget_modified(widget), "typed draft marks the widget modified");
}

}  // namespace

void run_all() {
    test_visible_fields_filter_by_surface();
    test_widget_numeric_and_choice_edit();
    test_widget_save_and_quit();
    test_widget_empty_clears_optional();
    test_widget_gemini_temperature();
    test_widget_render_frame();
}

}  // namespace ainiux::test::ui_settings
