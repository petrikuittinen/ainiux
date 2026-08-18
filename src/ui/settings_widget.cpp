#include "ui/settings_widget.hpp"

#include <string>

#include "editor/detail/unicode.hpp"
#include "editor/detail/wrap.hpp"
#include "ui/text_selector.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace ainiux::ui {
namespace {

size_t cells(const std::string& text) {
    return editor::detail::display_width_for_range(text, 0, text.size());
}

std::string pad_cells(const std::string& text, size_t width) {
    const size_t used = cells(text);
    if (used >= width) return text;
    return text + std::string(width - used, ' ');
}

std::string clip_cells(const std::string& text, size_t width) {
    if (width == 0) return "";
    if (cells(text) <= width) return text;
    std::string out;
    size_t pos = 0;
    size_t used = 0;
    while (pos < text.size()) {
        const size_t next = editor::detail::next_grapheme_offset(text, pos);
        const size_t width_here = editor::detail::display_width_for_range(text, pos, next);
        if (used + width_here > width) break;
        out.append(text, pos, next - pos);
        used += width_here;
        pos = next;
    }
    return out;
}

std::string fill_rule(int cols, const char* left, const char* right) {
    cols = std::max(2, cols);
    std::string line = left;
    for (int i = 0; i < cols - 2; ++i) line += u8"─";
    line += right;
    return line;
}

std::string titled_top(const std::string& title, int cols) {
    cols = std::max(2, cols);
    if (cols <= 3) return fill_rule(cols, u8"┌", u8"┐");
    std::string line = u8"┌─";
    if (!title.empty()) line += " " + title + " ";
    while (static_cast<int>(cells(line)) < cols - 1) line += u8"─";
    line += u8"┐";
    if (static_cast<int>(cells(line)) > cols) {
        return fill_rule(cols, u8"┌", u8"┐");
    }
    return line;
}

void append_box_line(std::vector<tui::StyledLine>& lines,
                     const std::string& inner,
                     tui::StyleRole role,
                     int cols) {
    const int inner_cols = std::max(1, cols - 2);
    tui::StyledLine line;
    line.segments.push_back({u8"│", tui::StyleRole::PanelBorder});
    line.segments.push_back({clip_cells(pad_cells(inner, static_cast<size_t>(inner_cols)),
                                        static_cast<size_t>(inner_cols)),
                             role});
    line.segments.push_back({u8"│", tui::StyleRole::PanelBorder});
    lines.push_back(std::move(line));
}

void append_box_segments(std::vector<tui::StyledLine>& lines,
                         const std::vector<tui::StyledSegment>& inner,
                         int cols) {
    const int inner_cols = std::max(1, cols - 2);
    tui::StyledLine line;
    line.segments.push_back({u8"│", tui::StyleRole::PanelBorder});
    size_t used = 0;
    for (const tui::StyledSegment& segment : inner) {
        if (used >= static_cast<size_t>(inner_cols)) break;
        const size_t remain = static_cast<size_t>(inner_cols) - used;
        const std::string clipped = clip_cells(segment.text, remain);
        if (clipped.empty()) continue;
        used += cells(clipped);
        line.segments.push_back({clipped, segment.role, segment.reverse, segment.attributes});
    }
    if (used < static_cast<size_t>(inner_cols)) {
        line.segments.push_back({std::string(static_cast<size_t>(inner_cols) - used, ' '),
                                 tui::StyleRole::PanelBody});
    }
    line.segments.push_back({u8"│", tui::StyleRole::PanelBorder});
    lines.push_back(std::move(line));
}

const chat::SettingsFieldSpec* field_at(const SettingsWidget& widget, size_t index) {
    const auto fields = chat::visible_settings_fields(widget.surface);
    if (index >= fields.size()) return nullptr;
    return fields[index];
}

void clamp_selected(SettingsWidget& widget) {
    const auto fields = chat::visible_settings_fields(widget.surface);
    if (fields.empty()) {
        widget.selected = 0;
        return;
    }
    if (widget.selected >= fields.size()) widget.selected = fields.size() - 1;
}

std::string current_stored(const SettingsWidget& widget, const chat::SettingsFieldSpec& field) {
    return chat::settings_field_stored_value(field, widget.draft, widget.editor_draft);
}

bool commit_row(SettingsWidget& widget) {
    const chat::SettingsFieldSpec* field = field_at(widget, widget.selected);
    if (field == nullptr) return true;
    if (!widget.row_editing) return true;
    const Error err =
        chat::apply_settings_field(*field, widget.draft, widget.editor_draft, widget.row_draft);
    if (!err.ok()) {
        widget.status = err.message;
        return false;
    }
    widget.row_editing = false;
    widget.row_draft.clear();
    widget.row_revert = current_stored(widget, *field);
    widget.status = std::string(field->id) + " updated";
    return true;
}

void begin_row_edit(SettingsWidget& widget, const chat::SettingsFieldSpec& field) {
    if (!widget.row_editing) {
        widget.row_revert = current_stored(widget, field);
        widget.row_draft = widget.row_revert;
        widget.row_editing = true;
    }
}

bool valid_numeric_syntax(const chat::SettingsFieldSpec& field,
                          const std::string& draft,
                          bool allow_negative) {
    if (draft.empty()) return chat::settings_field_allows_empty(field);
    size_t i = 0;
    if (allow_negative && draft[0] == '-') {
        if (draft.size() == 1) return true;
        i = 1;
    }
    bool saw_dot = false;
    bool saw_digit = false;
    for (; i < draft.size(); ++i) {
        const char ch = draft[i];
        if (ch == '.') {
            if (field.type == chat::SettingsFieldType::Integer || saw_dot) return false;
            saw_dot = true;
            continue;
        }
        if (ch < '0' || ch > '9') return false;
        saw_digit = true;
    }
    return saw_digit || draft == "." || draft == "-." || (allow_negative && draft == "-");
}

bool valid_numeric_draft(SettingsWidget& widget,
                         const chat::SettingsFieldSpec& field,
                         const std::string& draft) {
    if (!valid_numeric_syntax(field, draft, chat::settings_field_allows_negative(field))) {
        return false;
    }
    if (draft.empty() || draft == "." || draft == "-" || draft == "-.") {
        return true;
    }
    if (!draft.empty() && draft.back() == '.') {
        return true;
    }
    return chat::validate_settings_field(field, widget.draft, widget.editor_draft, draft).ok();
}

void cycle_choice(SettingsWidget& widget, int delta) {
    const chat::SettingsFieldSpec* field = field_at(widget, widget.selected);
    if (field == nullptr) return;
    const chat::SettingsFieldType type =
        chat::effective_settings_field_type(*field, widget.draft);
    if (type != chat::SettingsFieldType::Choice) {
        widget.status = std::string(field->id) + " is not a choice; type a value";
        return;
    }
    const std::vector<std::string> choices = chat::settings_field_choices(*field, widget.draft);
    if (choices.empty()) return;
    std::string current = widget.row_editing ? widget.row_draft : current_stored(widget, *field);
    if (current.empty() && field->optional) current = "default";
    size_t index = 0;
    bool found = false;
    for (size_t i = 0; i < choices.size(); ++i) {
        if (choices[i] == current) {
            index = i;
            found = true;
            break;
        }
    }
    if (!found && current == "auto" && !choices.empty() && choices.front() == "auto") {
        index = 0;
        found = true;
    }
    const int count = static_cast<int>(choices.size());
    int next = found ? static_cast<int>(index) + delta : (delta > 0 ? 0 : count - 1);
    next %= count;
    if (next < 0) next += count;
    const Error err =
        chat::apply_settings_field(*field, widget.draft, widget.editor_draft,
                                   choices[static_cast<size_t>(next)]);
    if (!err.ok()) {
        widget.status = err.message;
        return;
    }
    widget.row_editing = false;
    widget.row_draft.clear();
    widget.status = std::string(field->id) + " = " + choices[static_cast<size_t>(next)];
}

void cancel_row(SettingsWidget& widget) {
    const chat::SettingsFieldSpec* field = field_at(widget, widget.selected);
    if (field == nullptr) return;
    const std::string revert = widget.row_revert.empty()
                                   ? chat::settings_field_stored_value(
                                         *field, widget.snapshot, widget.editor_snapshot)
                                   : widget.row_revert;
    const std::string apply_value =
        (revert.empty() && field->optional) ? std::string("null") : revert;
    (void)chat::apply_settings_field(*field, widget.draft, widget.editor_draft, apply_value);
    widget.row_editing = false;
    widget.row_draft.clear();
    widget.status = "Change cancelled";
}

bool options_and_editor_equal(const cli::Options& lhs,
                              const chat::SettingsEditorLocals& lhs_editor,
                              const cli::Options& rhs,
                              const chat::SettingsEditorLocals& rhs_editor) {
    for (const chat::SettingsFieldSpec& field : chat::all_settings_fields()) {
        if (chat::settings_field_stored_value(field, lhs, lhs_editor) !=
            chat::settings_field_stored_value(field, rhs, rhs_editor)) {
            return false;
        }
    }
    return true;
}

}  // namespace

void open_settings_widget(SettingsWidget& widget,
                          chat::SettingsSurface surface,
                          const cli::Options& options,
                          const chat::SettingsEditorLocals& editor) {
    widget = SettingsWidget{};
    widget.active = true;
    widget.surface = surface;
    widget.snapshot = options;
    widget.draft = options;
    widget.editor_snapshot = editor;
    widget.editor_draft = editor;
    widget.selected = 0;
    const chat::SettingsFieldSpec* field = field_at(widget, 0);
    if (field != nullptr) {
        widget.row_revert = current_stored(widget, *field);
    }
    widget.status = "Settings · Enter accept · Esc cancel row · s save · q quit";
}

void close_settings_widget(SettingsWidget& widget) {
    widget = SettingsWidget{};
}

void cancel_settings_widget_row(SettingsWidget& widget) {
    if (!widget.active) return;
    cancel_row(widget);
}

bool settings_widget_modified(const SettingsWidget& widget) {
    if (!widget.active) return false;
    if (widget.row_editing) return true;
    return !options_and_editor_equal(
        widget.snapshot, widget.editor_snapshot, widget.draft, widget.editor_draft);
}

const chat::SettingsFieldSpec* selected_settings_field(const SettingsWidget& widget) {
    return field_at(widget, widget.selected);
}

std::vector<const chat::SettingsFieldSpec*> settings_widget_fields(const SettingsWidget& widget) {
    return chat::visible_settings_fields(widget.surface);
}

SettingsWidgetAction handle_settings_widget_movement(SettingsWidget& widget,
                                                     editor::MovementKey key) {
    if (!widget.active) return SettingsWidgetAction::None;
    clamp_selected(widget);
    if (key == editor::MovementKey::Left) {
        cycle_choice(widget, -1);
        return SettingsWidgetAction::Handled;
    }
    if (key == editor::MovementKey::Right) {
        cycle_choice(widget, 1);
        return SettingsWidgetAction::Handled;
    }
    if (widget.row_editing && !commit_row(widget)) {
        return SettingsWidgetAction::Handled;
    }
    const auto fields = chat::visible_settings_fields(widget.surface);
    widget.selected =
        move_text_selector_selection(widget.selected, fields.size(), key);
    const chat::SettingsFieldSpec* field = field_at(widget, widget.selected);
    if (field != nullptr) {
        widget.row_revert = current_stored(widget, *field);
        widget.status = std::string(field->id);
        const std::string hint = chat::settings_field_constraint_hint(*field, widget.draft);
        if (!hint.empty()) widget.status += " · " + hint;
    }
    return SettingsWidgetAction::Handled;
}

SettingsWidgetAction handle_settings_widget_key(SettingsWidget& widget, unsigned char ch) {
    if (!widget.active) return SettingsWidgetAction::None;
    clamp_selected(widget);
    const chat::SettingsFieldSpec* field = field_at(widget, widget.selected);
    const bool string_editing =
        widget.row_editing && field != nullptr &&
        chat::effective_settings_field_type(*field, widget.draft) == chat::SettingsFieldType::String;

    if (!string_editing && (ch == 's' || ch == 'S')) {
        if (!commit_row(widget)) return SettingsWidgetAction::Handled;
        return SettingsWidgetAction::Saved;
    }
    if (!string_editing && (ch == 'q' || ch == 'Q')) {
        return SettingsWidgetAction::Quit;
    }
    if (ch == '\r' || ch == '\n') {
        if (field == nullptr) return SettingsWidgetAction::Handled;
        if (!widget.row_editing) {
            widget.status = std::string(field->id) + " = " +
                            chat::settings_field_display(*field, widget.draft, widget.editor_draft);
            return SettingsWidgetAction::Handled;
        }
        (void)commit_row(widget);
        return SettingsWidgetAction::Handled;
    }
    if (ch == 127 || ch == 8) {
        if (field == nullptr) return SettingsWidgetAction::Handled;
        const chat::SettingsFieldType type =
            chat::effective_settings_field_type(*field, widget.draft);
        if (type == chat::SettingsFieldType::Choice) {
            widget.status = "Use ← → to change " + std::string(field->id);
            return SettingsWidgetAction::Handled;
        }
        begin_row_edit(widget, *field);
        if (!widget.row_draft.empty()) widget.row_draft.pop_back();
        widget.status = std::string(field->id) + " = " +
                        (widget.row_draft.empty() ? std::string("default") : widget.row_draft);
        return SettingsWidgetAction::Handled;
    }
    if (ch < 32 || ch == 127) {
        return SettingsWidgetAction::None;
    }
    if (field == nullptr) return SettingsWidgetAction::Handled;
    const chat::SettingsFieldType type =
        chat::effective_settings_field_type(*field, widget.draft);
    if (type == chat::SettingsFieldType::Choice) {
        widget.status = "Use ← → to change " + std::string(field->id);
        return SettingsWidgetAction::Handled;
    }
    const char typed = static_cast<char>(ch);
    begin_row_edit(widget, *field);
    std::string next = widget.row_draft;
    next.push_back(typed);
    if (type == chat::SettingsFieldType::Number || type == chat::SettingsFieldType::Integer) {
        if (!valid_numeric_draft(widget, *field, next)) {
            widget.status = chat::settings_field_constraint_hint(*field, widget.draft);
            if (widget.status.empty()) {
                widget.status = "Only valid numbers are accepted";
            } else {
                widget.status = std::string(field->id) + " must be " + widget.status;
            }
            return SettingsWidgetAction::Handled;
        }
    }
    widget.row_draft = std::move(next);
    widget.status = std::string(field->id) + " = " +
                    (widget.row_draft.empty() ? std::string("default") : widget.row_draft);
    return SettingsWidgetAction::Handled;
}

std::vector<tui::StyledLine> render_settings_widget(const SettingsWidget& widget, int cols) {
    std::vector<tui::StyledLine> lines;
    cols = std::max(8, cols);
    const std::string title =
        settings_widget_modified(widget) ? "Settings · modified" : "Settings";
    lines.push_back({{{titled_top(title, cols), tui::StyleRole::PanelBorder}}});
    append_box_line(lines, "", tui::StyleRole::PanelBody, cols);

    const auto fields = chat::visible_settings_fields(widget.surface);
    chat::SettingsGroup last_group = static_cast<chat::SettingsGroup>(-1);
    for (size_t i = 0; i < fields.size(); ++i) {
        const chat::SettingsFieldSpec& field = *fields[i];
        if (field.group != last_group) {
            if (last_group != static_cast<chat::SettingsGroup>(-1)) {
                append_box_line(lines, "", tui::StyleRole::PanelBody, cols);
            }
            append_box_line(lines, std::string(" ") + chat::settings_group_title(field.group),
                            tui::StyleRole::Muted, cols);
            last_group = field.group;
        }
        const bool selected = (i == widget.selected);
        const std::string prefix = selected ? kTextSelectorArrowPrefix : kTextSelectorUnselectedPrefix;
        std::string value;
        if (selected && widget.row_editing) {
            value = widget.row_draft.empty() ? "default" : widget.row_draft;
        } else {
            value = chat::settings_field_display(field, widget.draft, widget.editor_draft);
        }
        const chat::SettingsFieldType type =
            chat::effective_settings_field_type(field, widget.draft);
        std::vector<tui::StyledSegment> inner;
        const tui::StyleRole row_role =
            selected ? tui::StyleRole::PanelHighlight : tui::StyleRole::PanelBody;
        inner.push_back({prefix, row_role});
        inner.push_back({pad_cells(field.id, 26), row_role});
        if (type == chat::SettingsFieldType::Choice && selected) {
            const std::vector<std::string> choices =
                chat::settings_field_choices(field, widget.draft);
            const std::string current =
                (selected && widget.row_editing) ? widget.row_draft : current_stored(widget, field);
            for (size_t c = 0; c < choices.size(); ++c) {
                if (c != 0) inner.push_back({"|", tui::StyleRole::Muted});
                const bool current_choice = choices[c] == current ||
                    (current.empty() && choices[c] == "auto");
                tui::StyledSegment token;
                token.text = choices[c];
                token.role = current_choice ? tui::StyleRole::PanelHighlight : tui::StyleRole::Muted;
                token.reverse = current_choice;
                inner.push_back(std::move(token));
            }
            if (choices.empty()) {
                inner.push_back({value, row_role});
            }
        } else {
            const bool unset = value == "default";
            inner.push_back({value, unset ? tui::StyleRole::Muted : row_role});
        }
        const std::string hint = chat::settings_field_constraint_hint(field, widget.draft);
        if (!hint.empty() && type != chat::SettingsFieldType::Choice) {
            inner.push_back({"  ", tui::StyleRole::Muted});
            inner.push_back({hint, tui::StyleRole::Muted});
        }
        append_box_segments(lines, inner, cols);
    }

    append_box_line(lines, "", tui::StyleRole::PanelBody, cols);
    append_box_line(lines, " ↑↓ move · ←→ choice · type to edit · Enter accept",
                    tui::StyleRole::PanelHint, cols);
    append_box_line(lines, " Esc cancel row · s save · q quit", tui::StyleRole::PanelHint, cols);
    lines.push_back({{{fill_rule(cols, u8"└", u8"┘"), tui::StyleRole::PanelBorder}}});
    return lines;
}

}  // namespace ainiux::ui
