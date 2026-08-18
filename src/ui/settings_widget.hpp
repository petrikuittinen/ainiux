#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "chat/settings_fields.hpp"
#include "cli/args.hpp"
#include "editor/selection.hpp"
#include "tui/tui.hpp"

namespace ainiux::ui {

enum class SettingsWidgetAction {
    None,
    Handled,
    Saved,
    Quit,
};

struct SettingsWidget {
    bool active = false;
    chat::SettingsSurface surface = chat::SettingsSurface::Chat;
    cli::Options snapshot;
    cli::Options draft;
    chat::SettingsEditorLocals editor_snapshot;
    chat::SettingsEditorLocals editor_draft;
    size_t selected = 0;
    bool row_editing = false;
    std::string row_draft;
    std::string row_revert;
    std::string status;
};

void open_settings_widget(SettingsWidget& widget,
                          chat::SettingsSurface surface,
                          const cli::Options& options,
                          const chat::SettingsEditorLocals& editor = {});
void close_settings_widget(SettingsWidget& widget);
bool settings_widget_modified(const SettingsWidget& widget);

SettingsWidgetAction handle_settings_widget_key(SettingsWidget& widget, unsigned char ch);
SettingsWidgetAction handle_settings_widget_movement(SettingsWidget& widget,
                                                     editor::MovementKey key);
void cancel_settings_widget_row(SettingsWidget& widget);

std::vector<tui::StyledLine> render_settings_widget(const SettingsWidget& widget, int cols);

const chat::SettingsFieldSpec* selected_settings_field(const SettingsWidget& widget);
std::vector<const chat::SettingsFieldSpec*> settings_widget_fields(const SettingsWidget& widget);

}  // namespace ainiux::ui
