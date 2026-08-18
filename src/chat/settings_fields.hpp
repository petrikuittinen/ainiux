#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "cli/args.hpp"
#include "common.hpp"
#include "editor/editor.hpp"

namespace ainiux::chat {

enum class SettingsSurface { Chat, Agent, Editor };
enum class SettingsGroup { Model, Display, General, Editor };
enum class SettingsFieldType { Number, Integer, String, Choice };

struct SettingsEditorLocals {
    size_t tab_width = editor::kDefaultTabWidth;
    editor::TabStyle tab_style = editor::TabStyle::Spaces;
    editor::LineBreak linebreak = editor::LineBreak::Lf;
    size_t alignment_width = editor::kDefaultTextAlignWidth;
};

struct SettingsFieldSpec {
    const char* id = "";
    SettingsGroup group = SettingsGroup::General;
    SettingsFieldType type = SettingsFieldType::String;
    bool optional = true;
};

const char* settings_group_title(SettingsGroup group);
const std::vector<SettingsFieldSpec>& all_settings_fields();
std::vector<const SettingsFieldSpec*> visible_settings_fields(SettingsSurface surface);

std::string format_settings_number(double value);
std::string settings_field_stored_value(const SettingsFieldSpec& field,
                                        const cli::Options& options,
                                        const SettingsEditorLocals& editor);
std::string settings_field_display(const SettingsFieldSpec& field,
                                   const cli::Options& options,
                                   const SettingsEditorLocals& editor);
std::vector<std::string> settings_field_choices(const SettingsFieldSpec& field,
                                                const cli::Options& options);
SettingsFieldType effective_settings_field_type(const SettingsFieldSpec& field,
                                                const cli::Options& options);
std::string settings_field_constraint_hint(const SettingsFieldSpec& field,
                                           const cli::Options& options);
bool settings_field_allows_negative(const SettingsFieldSpec& field);
bool settings_field_allows_empty(const SettingsFieldSpec& field);

Error validate_settings_field(const SettingsFieldSpec& field,
                              const cli::Options& options,
                              const SettingsEditorLocals& editor,
                              const std::string& value);
Error apply_settings_field(const SettingsFieldSpec& field,
                           cli::Options& options,
                           SettingsEditorLocals& editor,
                           const std::string& value);

SettingsEditorLocals make_editor_locals(const editor::EditorState& state, size_t alignment_width);
void apply_editor_locals_to_state(const SettingsEditorLocals& locals, editor::EditorState& state);
void copy_visible_settings(SettingsSurface surface,
                           const cli::Options& from,
                           const SettingsEditorLocals& from_editor,
                           cli::Options& to,
                           SettingsEditorLocals& to_editor);

}  // namespace ainiux::chat
