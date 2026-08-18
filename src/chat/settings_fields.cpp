#include "chat/settings_fields.hpp"

#include "chat/generation_settings.hpp"
#include "chat/settings.hpp"
#include "config/model_catalog.hpp"

#include <cmath>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <utility>

namespace ainiux::chat {
namespace {

using generation::kMaxTokens;
using generation::kMinP;
using generation::kPresencePenalty;
using generation::kReasoning;
using generation::kRepeatPenalty;
using generation::kTemperature;
using generation::kTopK;
using generation::kTopP;

const SettingsFieldSpec kFields[] = {
    {kTemperature, SettingsGroup::Model, SettingsFieldType::Number, true},
    {kTopP, SettingsGroup::Model, SettingsFieldType::Number, true},
    {kTopK, SettingsGroup::Model, SettingsFieldType::Integer, true},
    {kMinP, SettingsGroup::Model, SettingsFieldType::Number, true},
    {kRepeatPenalty, SettingsGroup::Model, SettingsFieldType::Number, true},
    {kPresencePenalty, SettingsGroup::Model, SettingsFieldType::Number, true},
    {kMaxTokens, SettingsGroup::Model, SettingsFieldType::Integer, true},
    {kReasoning, SettingsGroup::Model, SettingsFieldType::Choice, false},
    {"stream", SettingsGroup::Model, SettingsFieldType::Choice, false},
    {"highlight", SettingsGroup::Display, SettingsFieldType::Choice, false},
    {"show_thinking_traces", SettingsGroup::Display, SettingsFieldType::Choice, true},
    {"thinking_preview_max_chars", SettingsGroup::Display, SettingsFieldType::Integer, true},
    {"cmd-out", SettingsGroup::Display, SettingsFieldType::Choice, false},
    {"auto_convert_html_to_md", SettingsGroup::General, SettingsFieldType::Choice, false},
    {"context_tokens", SettingsGroup::General, SettingsFieldType::String, true},
    {"tab-width", SettingsGroup::Editor, SettingsFieldType::Integer, false},
    {"tab-style", SettingsGroup::Editor, SettingsFieldType::Choice, false},
    {"linebreak", SettingsGroup::Editor, SettingsFieldType::Choice, false},
    {"alignment-width", SettingsGroup::Editor, SettingsFieldType::Integer, false},
};

bool field_visible(const SettingsFieldSpec& field, SettingsSurface surface) {
    const std::string id = field.id;
    if (id == "thinking_preview_max_chars" || id == "cmd-out") {
        return surface == SettingsSurface::Agent;
    }
    if (field.group == SettingsGroup::Editor) {
        return surface == SettingsSurface::Editor;
    }
    return true;
}

bool is_on_off(const std::string& id) {
    return id == "stream" || id == "highlight" || id == "show_thinking_traces" || id == "cmd-out" ||
           id == "auto_convert_html_to_md";
}

std::string on_off(bool value) {
    return value ? "on" : "off";
}

config::ReasoningSelectorData reasoning_data(const cli::Options& options) {
    return config::reasoning_selector_data(
        options.model_catalog, options.provider, options.api, options.model);
}

bool reasoning_is_choice(const cli::Options& options) {
    const config::ReasoningSelectorData data = reasoning_data(options);
    return data.values.size() > 1;
}

std::string reasoning_stored(const cli::Options& options) {
    return config::reasoning_selection_value(options.reasoning);
}

}  // namespace

const char* settings_group_title(SettingsGroup group) {
    switch (group) {
        case SettingsGroup::Model:
            return "Model";
        case SettingsGroup::Display:
            return "Display";
        case SettingsGroup::General:
            return "General";
        case SettingsGroup::Editor:
            return "Editor";
    }
    return "General";
}

const std::vector<SettingsFieldSpec>& all_settings_fields() {
    static const std::vector<SettingsFieldSpec> fields(std::begin(kFields), std::end(kFields));
    return fields;
}

std::vector<const SettingsFieldSpec*> visible_settings_fields(SettingsSurface surface) {
    std::vector<const SettingsFieldSpec*> out;
    for (const SettingsFieldSpec& field : all_settings_fields()) {
        if (field_visible(field, surface)) {
            out.push_back(&field);
        }
    }
    return out;
}

std::string format_settings_number(double value) {
    std::ostringstream out;
    out << std::setprecision(6) << std::defaultfloat << value;
    return out.str();
}

std::string settings_field_stored_value(const SettingsFieldSpec& field,
                                        const cli::Options& options,
                                        const SettingsEditorLocals& editor) {
    const std::string id = field.id;
    if (id == kTemperature) {
        return options.has_temperature ? format_settings_number(options.temperature) : "";
    }
    if (id == kTopP) {
        return options.has_top_p ? format_settings_number(options.top_p) : "";
    }
    if (id == kTopK) {
        return options.has_top_k ? std::to_string(options.top_k) : "";
    }
    if (id == kMinP) {
        return options.has_min_p ? format_settings_number(options.min_p) : "";
    }
    if (id == kRepeatPenalty) {
        return options.has_repeat_penalty ? format_settings_number(options.repeat_penalty) : "";
    }
    if (id == kPresencePenalty) {
        return options.has_presence_penalty ? format_settings_number(options.presence_penalty) : "";
    }
    if (id == kMaxTokens) {
        return options.has_max_output_tokens ? std::to_string(options.max_output_tokens) : "";
    }
    if (id == kReasoning) {
        return reasoning_stored(options);
    }
    if (id == "stream") {
        return on_off(options.stream);
    }
    if (id == "highlight") {
        return on_off(options.tui_highlight);
    }
    if (id == "show_thinking_traces") {
        return options.has_show_thinking_traces ? on_off(options.show_thinking_traces) : "";
    }
    if (id == "thinking_preview_max_chars") {
        return options.has_agent_thinking_preview_max_chars
                   ? std::to_string(options.agent_thinking_preview_max_chars)
                   : "";
    }
    if (id == "cmd-out") {
        return on_off(options.agent_show_command_output);
    }
    if (id == "auto_convert_html_to_md") {
        return on_off(options.auto_convert_html_to_markdown);
    }
    if (id == "context_tokens") {
        return options.has_context_tokens ? std::to_string(options.context_tokens) : "";
    }
    if (id == "tab-width") {
        return std::to_string(editor.tab_width);
    }
    if (id == "tab-style") {
        return editor::tab_style_name(editor.tab_style);
    }
    if (id == "linebreak") {
        return editor::linebreak_name(editor.linebreak);
    }
    if (id == "alignment-width") {
        return std::to_string(editor.alignment_width);
    }
    return {};
}

std::string settings_field_display(const SettingsFieldSpec& field,
                                   const cli::Options& options,
                                   const SettingsEditorLocals& editor) {
    const std::string stored = settings_field_stored_value(field, options, editor);
    if (stored.empty() && field.optional) {
        return "default";
    }
    return stored;
}

std::vector<std::string> settings_field_choices(const SettingsFieldSpec& field,
                                                const cli::Options& options) {
    const std::string id = field.id;
    if (id == kReasoning) {
        const config::ReasoningSelectorData data = reasoning_data(options);
        if (data.values.size() <= 1) {
            return {};
        }
        std::vector<std::string> choices;
        choices.reserve(data.values.size());
        for (const ReasoningSelection& selection : data.values) {
            choices.push_back(config::reasoning_selection_value(selection));
        }
        return choices;
    }
    if (is_on_off(id)) {
        return {"off", "on"};
    }
    if (id == "tab-style") {
        return {"spaces", "tab"};
    }
    if (id == "linebreak") {
        return {"lf", "cr", "crlf"};
    }
    return {};
}

std::string settings_field_constraint_hint(const SettingsFieldSpec& field,
                                           const cli::Options& options) {
    const std::string id = field.id;
    if (id == kTemperature) {
        const ModelCapability* capability = config::resolve_model_capability(
            options.model_catalog, options.provider, options.api, options.model);
        std::ostringstream out;
        out << "0.0–" << format_settings_number(config::temperature_max_for(capability));
        return out.str();
    }
    if (id == kTopP || id == kMinP) {
        return "0.0–1.0";
    }
    if (id == kTopK || id == kMaxTokens) {
        return "≥ 0";
    }
    if (id == kRepeatPenalty) {
        return "> 0";
    }
    if (id == "thinking_preview_max_chars") {
        return "0–1000";
    }
    if (id == "tab-width") {
        return "1–32";
    }
    if (id == "alignment-width") {
        return "21–1000";
    }
    if (id == "context_tokens") {
        return "auto or tokens";
    }
    return {};
}

bool settings_field_allows_negative(const SettingsFieldSpec& field) {
    return std::string(field.id) == kPresencePenalty;
}

bool settings_field_allows_empty(const SettingsFieldSpec& field) {
    return field.optional;
}

Error validate_settings_field(const SettingsFieldSpec& field,
                              const cli::Options& options,
                              const SettingsEditorLocals& editor,
                              const std::string& value) {
    cli::Options probe = options;
    SettingsEditorLocals locals = editor;
    return apply_settings_field(field, probe, locals, value);
}

Error apply_settings_field(const SettingsFieldSpec& field,
                           cli::Options& options,
                           SettingsEditorLocals& editor,
                           const std::string& value) {
    const std::string id = field.id;
    if (id == "tab-width") {
        int parsed = 0;
        try {
            parsed = std::stoi(value);
        } catch (const std::exception&) {
            return {ErrorCode::BadArgs, "invalid tab-width setting: expected an integer from 1 through 32"};
        }
        if (parsed < 1 || parsed > 32) {
            return {ErrorCode::BadArgs, "invalid tab-width setting: expected an integer from 1 through 32"};
        }
        editor.tab_width = static_cast<size_t>(parsed);
        return ok_error();
    }
    if (id == "tab-style") {
        editor::TabStyle style;
        if (!editor::parse_tab_style(value, style)) {
            return {ErrorCode::BadArgs, "invalid tab-style setting: expected spaces or tab"};
        }
        editor.tab_style = style;
        return ok_error();
    }
    if (id == "linebreak") {
        editor::LineBreak linebreak;
        if (!editor::parse_linebreak(value, linebreak)) {
            return {ErrorCode::BadArgs, "invalid linebreak setting: expected lf, cr, or crlf"};
        }
        editor.linebreak = linebreak;
        return ok_error();
    }
    if (id == "alignment-width") {
        int parsed = 0;
        try {
            parsed = std::stoi(value);
        } catch (const std::exception&) {
            return {ErrorCode::BadArgs,
                    "invalid alignment-width setting: expected an integer from 21 through 1000"};
        }
        if (parsed <= static_cast<int>(editor::kMinTextAlignWidthExclusive) ||
            parsed > static_cast<int>(editor::kMaxTextAlignWidth)) {
            return {ErrorCode::BadArgs,
                    "invalid alignment-width setting: expected an integer from 21 through 1000"};
        }
        editor.alignment_width = static_cast<size_t>(parsed);
        return ok_error();
    }
    const std::string apply_name = (id == "auto_convert_html_to_md") ? "auto-convert-html-to-md" : id;
    const std::string apply_value = (value.empty() && field.optional) ? "null" : value;
    return apply_chat_setting(options, apply_name, apply_value);
}

SettingsFieldType effective_settings_field_type(const SettingsFieldSpec& field,
                                                const cli::Options& options) {
    if (std::string(field.id) == kReasoning && !reasoning_is_choice(options)) {
        return SettingsFieldType::String;
    }
    return field.type;
}

SettingsEditorLocals make_editor_locals(const editor::EditorState& state, size_t alignment_width) {
    SettingsEditorLocals locals;
    locals.tab_width = state.tab_width;
    locals.tab_style = state.tab_style;
    locals.linebreak = state.linebreak;
    locals.alignment_width = alignment_width;
    return locals;
}

void apply_editor_locals_to_state(const SettingsEditorLocals& locals, editor::EditorState& state) {
    state.tab_width = locals.tab_width;
    state.tab_style = locals.tab_style;
    state.linebreak = locals.linebreak;
}

void copy_visible_settings(SettingsSurface surface,
                           const cli::Options& from,
                           const SettingsEditorLocals& from_editor,
                           cli::Options& to,
                           SettingsEditorLocals& to_editor) {
    for (const SettingsFieldSpec* field : visible_settings_fields(surface)) {
        const std::string stored = settings_field_stored_value(*field, from, from_editor);
        const std::string value = (stored.empty() && field->optional) ? "null" : stored;
        (void)apply_settings_field(*field, to, to_editor, value);
    }
}

}  // namespace ainiux::chat
