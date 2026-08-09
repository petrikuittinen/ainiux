#include "editor/autosave.hpp"
#include "editor/terminal_ui.hpp"

#include "common.hpp"
#include "editor/ai_continue.hpp"
#include "editor/detail/editor_common.hpp"
#include "editor/detail/unicode.hpp"
#include "editor/detail/wrap.hpp"
#include "editor/clipboard.hpp"
#include "editor/editor_assist.hpp"
#include "editor/selection.hpp"
#include "editor/terminal_input.hpp"
#include "editor/text_layout.hpp"
#include "tui/activity.hpp"
#include "tui/detail/render.hpp"
#include "tui/theme_registry.hpp"
#include "ui/confirmation.hpp"
#include "ui/provider_model_display.hpp"
#include "ui/scrollbar.hpp"

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <iostream>
#include <new>
#include <sstream>
#include <stdexcept>

namespace ainiux::editor {
namespace {

bool minibuffer_supports_path_completion(MinibufferAction action) {
    return action == MinibufferAction::SaveFile || action == MinibufferAction::SaveAsFile ||
           action == MinibufferAction::LoadFile || action == MinibufferAction::DiredPath ||
           action == MinibufferAction::DiredRename || action == MinibufferAction::DiredCopy ||
           action == MinibufferAction::DiredNewFile || action == MinibufferAction::DiredNewDir;
}

std::string terminal_position(int row, int col) {
    return "\x1b[" + std::to_string(std::max(1, row)) + ";" +
           std::to_string(std::max(1, col)) + "H";
}

}  // namespace

using detail::pad_or_clip_ascii;

TerminalSize terminal_size() {
    TerminalSize size;
    const tui::TerminalDimensions dimensions = tui::terminal_dimensions();
    size.rows = dimensions.rows;
    size.cols = dimensions.cols;
    return size;
}

std::string editor_status_line(const EditorState& state,
                               bool help_view,
                               size_t split_pane_count,
                               const EditorStatusChrome& status_chrome) {
    std::ostringstream out;
    if (help_view) {
        out << "Help (read-only)";
    } else {
        out << (state.path.empty() ? "[scratch]" : state.path);
        if (state.read_only) {
            out << " [RO]";
        }
        if (state.dirty) {
            out << " *";
        }
        const char* linebreak = state.linebreak == LineBreak::Crlf
                                    ? "CRLF"
                                    : state.linebreak == LineBreak::Cr ? "CR" : "LF";
        out << "  (" << highlight::language_name(state.language) << ' ' << linebreak << ')';
        if (split_pane_count > 1) {
            out << "  [" << split_pane_count << " panes]";
        }
    }
    const size_t line = state.text.line_for_offset(state.cursor) + 1;
    const size_t column = state.text.display_column_for_offset(state.cursor, state.tab_width) + 1;
    out << "  Ln " << line << ", Col " << column;
    if (help_view) {
        out << "  Ctrl+H / Esc /help / Ctrl+Q to return";
    } else {
        const std::string model_bracket =
            ui::model_reasoning_bracket(status_chrome.model, status_chrome.reasoning);
        if (!model_bracket.empty()) {
            if (split_pane_count > 1) {
                out << "  Ctrl+X o other";
            }
            out << "  " << model_bracket;
        } else if (split_pane_count > 1) {
            out << "  Ctrl+X o other  Ctrl+Q quit";
        } else {
            out << "  Ctrl+Q quit  Ctrl+H help";
        }
    }
    return out.str();
}

Rect editor_main_area() {
    const TerminalSize size = terminal_size();
    const int rows = std::max(3, size.rows);
    const int cols = std::max(20, size.cols);
    return Rect{1, 1, std::max(1, rows - 2), std::max(1, cols - 1)};
}

Rect editor_content_rect(const Rect& pane_rect) {
    if (pane_rect.width <= 1) {
        return pane_rect;
    }
    return Rect{pane_rect.row, pane_rect.col, pane_rect.height, pane_rect.width - 1};
}

bool apply_editor_mouse_scroll(EditorState& state,
                               const Rect& focused_content_rect,
                               const MouseInputEvent& mouse) {
    if (mouse.row < focused_content_rect.row ||
        mouse.row >= focused_content_rect.row + focused_content_rect.height ||
        mouse.col < focused_content_rect.col ||
        mouse.col >= focused_content_rect.col + focused_content_rect.width) {
        return false;
    }
    if (mouse.button == MouseButton::WheelUp) {
        (void)state.scroll_view_rows(focused_content_rect, -1);
        return true;
    }
    if (mouse.button == MouseButton::WheelDown) {
        (void)state.scroll_view_rows(focused_content_rect, 1);
        return true;
    }
    return false;
}

namespace {

size_t editor_total_visual_rows(const EditorState& state, size_t width) {
    width = std::max<size_t>(1, width);
    size_t rows = 0;
    const size_t line_count = state.text.line_count();
    for (size_t line = 0; line < line_count; ++line) {
        rows += detail::wrapped_row_count(state.text.line_text(line), width, state.tab_width);
    }
    return rows;
}

// Ensure an unfocused pane's scroll still makes sense if its stored view is stale.
void clamp_pane_scroll(size_t& scroll_line, size_t content_rows, size_t viewport_rows) {
    if (viewport_rows == 0) {
        scroll_line = 0;
        return;
    }
    if (content_rows <= viewport_rows) {
        scroll_line = 0;
        return;
    }
    const size_t max_scroll = content_rows - viewport_rows;
    if (scroll_line > max_scroll) {
        scroll_line = max_scroll;
    }
}

void append_scrollbar_cell(std::string& output,
                           const TerminalThemeStyle& theme_style,
                           const std::string& glyph) {
    if (glyph.empty()) {
        return;
    }
    if (theme_style.use_colors && theme_style.themes != nullptr) {
        output += tui::style_sequence_for(
            *theme_style.themes, theme_style.theme_name, tui::StyleRole::Muted, theme_style.color_mode);
    }
    output += glyph;
    if (theme_style.use_colors && theme_style.themes != nullptr) {
        output += "\x1b[0m";
    }
}

// Pane chrome separators must use theme colors: unstyled box-drawing inherits the
// terminal default (often near-black), which disappears on dark backgrounds.
void append_pane_separator_glyph(std::string& output,
                                 const TerminalThemeStyle& theme_style,
                                 const char* glyph) {
    if (theme_style.use_colors && theme_style.themes != nullptr) {
        output += tui::style_sequence_for(
            *theme_style.themes, theme_style.theme_name, tui::StyleRole::PanelBorder, theme_style.color_mode);
    }
    output += glyph;
    if (theme_style.use_colors && theme_style.themes != nullptr) {
        output += "\x1b[0m";
    }
}

}  // namespace

std::string minibuffer_text(const MinibufferState& minibuffer) {
    if (minibuffer.active) {
        return minibuffer.prompt + minibuffer.input;
    }
    return minibuffer.message;
}

namespace {

std::string activity_color_sequence(const TerminalThemeStyle& theme_style, tui::ActivityKind kind) {
    if (!theme_style.use_colors || theme_style.themes == nullptr) {
        return "";
    }
    const tui::StyleRole role = tui::activity_indicator_role(kind);
    return tui::ansi_foreground_sequence(
        tui::style_pair_for(*theme_style.themes, theme_style.theme_name, role).foreground,
        theme_style.color_mode);
}

tui::Rgb blend_rgb(tui::Rgb from, tui::Rgb toward, double amount) {
    if (amount <= 0.0) {
        return from;
    }
    if (amount >= 1.0) {
        return toward;
    }
    auto mix = [amount](int a, int b) {
        return static_cast<int>(a + (b - a) * amount + 0.5);
    };
    return {mix(from.r, toward.r), mix(from.g, toward.g), mix(from.b, toward.b)};
}

// Mild tint of panel_highlight on the normal background so changed lines are
// obvious without a separate syntax scheme. panel_background alone is nearly
// identical to background on built-in dark/light themes.
tui::Rgb changed_line_background(const tui::ThemePalette& palette) {
    return blend_rgb(palette.background, palette.panel_highlight, 0.28);
}

void append_editor_rendered_line(std::string& output,
                                 const std::string& line,
                                 const std::vector<RenderedPanel::Span>& spans,
                                 const TerminalThemeStyle& theme_style,
                                 bool changed_line_bg = false) {
    size_t pos = 0;
    const bool colors = theme_style.use_colors && theme_style.themes != nullptr;
    const tui::ThemePalette* palette =
        colors ? theme_style.themes->find(theme_style.theme_name) : nullptr;
    if (colors && palette == nullptr) {
        palette = theme_style.themes->find("dark");
    }
    const bool use_changed_bg = changed_line_bg && colors && palette != nullptr;
    const tui::Rgb changed_bg =
        use_changed_bg ? changed_line_background(*palette) : tui::Rgb{};
    auto append_role_style = [&](tui::StyleRole role) {
        if (!colors) {
            return;
        }
        tui::StylePair pair =
            tui::style_pair_for(*theme_style.themes, theme_style.theme_name, role);
        if (use_changed_bg) {
            pair.background = changed_bg;
        }
        output += tui::ansi_style_sequence(pair, theme_style.color_mode);
    };
    auto append_base_style = [&]() { append_role_style(tui::StyleRole::Text); };
    append_base_style();
    for (const RenderedPanel::Span& span : spans) {
        const size_t start = std::min(span.start, line.size());
        const size_t end = std::min(span.end, line.size());
        if (start > pos) {
            // Gap text must keep the changed-line background (style is already active).
            output.append(line, pos, start - pos);
        }
        if (span.syntax && colors) {
            append_role_style(tui::style_role_for_token(span.role));
            output += tui::ansi_text_attributes_sequence(
                tui::text_attributes_for_token(span.role));
        } else {
            append_base_style();
        }
        if (span.selected) {
            output += "\x1b[7m";
        }
        if (end > start) {
            output.append(line, start, end - start);
        }
        output += "\x1b[0m";
        append_base_style();
        pos = std::max(pos, end);
    }
    if (pos < line.size()) {
        output.append(line, pos, line.size() - pos);
    }
    // Changed lines always emit a styled full-width row (including padding spaces).
    if (!spans.empty() || colors || use_changed_bg) {
        output += "\x1b[0m";
    }
}

int append_utf8_cells(std::string& out, const std::string& text, size_t& pos, int max_cells) {
    int used = 0;
    while (pos < text.size() && used < max_cells) {
        const size_t width = detail::display_width_at(text, pos, 0);
        if (width == 0) {
            break;
        }
        size_t len = 1;
        if ((static_cast<unsigned char>(text[pos]) & 0xF8U) == 0xF0U && pos + 3 < text.size()) {
            len = 4;
        } else if ((static_cast<unsigned char>(text[pos]) & 0xF0U) == 0xE0U && pos + 2 < text.size()) {
            len = 3;
        } else if ((static_cast<unsigned char>(text[pos]) & 0xE0U) == 0xC0U && pos + 1 < text.size()) {
            len = 2;
        }
        out.append(text, pos, len);
        pos += len;
        used += static_cast<int>(width);
    }
    return used;
}

std::string editor_assist_minibuffer_line(const EditorAssistDisplay& assist,
                                          const TerminalThemeStyle& theme_style,
                                          int width) {
    if (width <= 0) {
        return "";
    }
    const std::string label = continue_status_label(assist.provider_name, assist.model_name);
    const std::string indicator = tui::activity_indicator_text(assist.kind, assist.frame);
    const std::string color = activity_color_sequence(theme_style, assist.kind);
    const std::string reset = "\x1b[0m";

    std::string out;
    out.reserve(static_cast<size_t>(width) * 4);
    int cells = 0;

    auto append_plain = [&](const std::string& text) {
        size_t pos = 0;
        cells += append_utf8_cells(out, text, pos, width - cells);
    };
    auto append_indicator = [&]() {
        if (cells >= width || indicator.empty()) {
            return;
        }
        size_t pos = 0;
        const int indicator_cells = static_cast<int>(tui::activity_indicator_width(assist.kind));
        if (cells + indicator_cells > width) {
            return;
        }
        if (!color.empty()) {
            out += color;
        }
        cells += append_utf8_cells(out, indicator, pos, width - cells);
        if (!color.empty()) {
            out += reset;
        }
    };

    append_plain(label);
    if (!label.empty() && cells < width) {
        append_plain(" ");
    }
    append_indicator();
    if (!assist.suffix.empty() && cells < width) {
        append_plain(" ");
        append_plain(assist.suffix);
    }
    while (cells < width) {
        out.push_back(' ');
        ++cells;
    }
    return out;
}

}  // namespace

void minibuffer_message(MinibufferState& minibuffer, std::string message) {
    minibuffer.active = false;
    minibuffer.action = MinibufferAction::None;
    minibuffer.prompt.clear();
    minibuffer.input.clear();
    minibuffer.message = std::move(message);
}

Error paste_into_minibuffer(MinibufferState& minibuffer, const std::string& text) {
    if (!minibuffer.active) {
        return {ErrorCode::BadArgs, "no active minibuffer to paste into"};
    }
    switch (minibuffer.action) {
        case MinibufferAction::SaveFile:
        case MinibufferAction::SaveAsFile:
        case MinibufferAction::LoadFile:
        case MinibufferAction::Search:
        case MinibufferAction::ReplaceSearch:
        case MinibufferAction::ReplaceWith:
        case MinibufferAction::AssistCommand:
        case MinibufferAction::TextAlignWidth:
        case MinibufferAction::GotoLine:
        case MinibufferAction::DiredPath:
        case MinibufferAction::DiredRename:
        case MinibufferAction::DiredCopy:
        case MinibufferAction::DiredNewFile:
        case MinibufferAction::DiredNewDir:
            break;
        case MinibufferAction::None:
        case MinibufferAction::ConfirmLoad:
        case MinibufferAction::ConfirmAutosaveRecovery:
        case MinibufferAction::ConfirmQuit:
        case MinibufferAction::ConfirmSaveOnQuit:
        case MinibufferAction::ConfirmOverwrite:
        case MinibufferAction::ConfirmReloadAfterLock:
        case MinibufferAction::ConfirmReasoning:
        case MinibufferAction::AssistScopeChoice:
        case MinibufferAction::AssistPromptMode:
        case MinibufferAction::DiredConfirmDelete:
        case MinibufferAction::DiredConfirmOverwrite:
            return {ErrorCode::BadArgs, "paste is not accepted by the active prompt"};
    }

    size_t end = text.size();
    while (end > 0 && (text[end - 1] == '\r' || text[end - 1] == '\n')) {
        --end;
    }
    for (size_t index = 0; index < end; ++index) {
        const unsigned char ch = static_cast<unsigned char>(text[index]);
        if (ch == '\r' || ch == '\n' || ch == '\0') {
            return {ErrorCode::BadArgs, "minibuffer paste must contain exactly one line"};
        }
        if (ch < 0x20U && ch != '\t') {
            return {ErrorCode::BadArgs, "minibuffer paste contains an unsupported control character"};
        }
    }
    try {
        minibuffer.input.append(text, 0, end);
    } catch (const std::bad_alloc&) {
        return {ErrorCode::Internal, "not enough memory to paste into the minibuffer"};
    } catch (const std::length_error&) {
        return {ErrorCode::BadArgs, "pasted minibuffer text is too large"};
    }
    return ok_error();
}

void start_minibuffer(MinibufferState& minibuffer,
                      MinibufferAction action,
                      std::string prompt,
                      std::string initial,
                      PathCompleter* path_completer) {
    if (path_completer != nullptr && minibuffer_supports_path_completion(action)) {
        path_completer->reset();
    }
    minibuffer.active = true;
    minibuffer.action = action;
    minibuffer.prompt = std::move(prompt);
    minibuffer.input = std::move(initial);
}

bool is_assist_minibuffer_action(MinibufferAction action) {
    return action == MinibufferAction::AssistCommand || action == MinibufferAction::AssistScopeChoice ||
           action == MinibufferAction::AssistPromptMode;
}

void exit_assist_command_mode(MinibufferState& minibuffer, AssistCompleterState& completer) {
    completer = AssistCompleterState{};
    minibuffer_message(minibuffer, "Ready");
}

void start_assist_command_mode(MinibufferState& minibuffer, AssistCompleterState& completer) {
    completer = AssistCompleterState{};
    start_minibuffer(minibuffer, MinibufferAction::AssistCommand, "Command: ", "");
}

void reset_editor_buffer(EditorState& state,
                         LoadedFile loaded,
                         std::string path) {
    state.text = std::move(loaded.text);
    state.invalidate_word_index();
    state.cursor = 0;
    state.preferred_column = 0;
    state.scroll_line = 0;
    state.scroll_column = 0;
    state.set_path(std::move(path));
    state.tab_width = loaded.tab_width;
    state.tab_style = loaded.tab_style;
    state.linebreak = loaded.linebreak;
    state.dirty = false;
    state.reset_autosave_pending();
    state.clear_undo_history();
    state.clear_selection();
}

bool editor_target_exists(const std::string& path) {
    if (path.empty()) {
        return false;
    }
    std::error_code filesystem_error;
    return std::filesystem::exists(path, filesystem_error) &&
           std::filesystem::is_regular_file(path, filesystem_error);
}

bool needs_overwrite_confirm(const std::string& target, const std::string& current_path) {
    if (!editor_target_exists(target)) {
        return false;
    }
    if (!current_path.empty() && target == current_path) {
        return false;
    }
    return true;
}

std::string overwrite_prompt_message(const std::string& path) {
    return path + " exists. Press y to overwrite or any other key to cancel: ";
}

bool save_editor_to_path(EditorState& state,
                         PendingSaveRequest request,
                         PendingSaveRequest& pending_save,
                         MinibufferState& minibuffer,
                         const EditorSettings& settings) {
    FileFingerprint current;
    Error inspect_error = fingerprint_file(request.canonical_path, current);
    if (!inspect_error.ok()) {
        minibuffer_message(minibuffer, inspect_error.message);
        return false;
    }
    if (current != request.observed_disk) {
        request.observed_disk = current;
        request.external_change = true;
        pending_save = std::move(request);
        start_minibuffer(minibuffer,
                         MinibufferAction::ConfirmOverwrite,
                         pending_save.path +
                             " changed again while overwrite confirmation was pending. Press y "
                             "to overwrite this version or any other key to cancel: ");
        return false;
    }

    Error save_error = save_file(request.path, state.text, state.linebreak);
    if (save_error.ok()) {
        state.dirty = false;
        state.reset_autosave_pending();
        remove_autosave_file(request.path, settings);
        if (request.update_path) {
            const bool retargeting = state.canonical_path != request.canonical_path;
            state.set_path(request.path);
            state.canonical_path = request.canonical_path;
            if (retargeting) {
                state.file_lock = std::move(request.destination_lock);
            }
            state.read_only = false;
            state.reload_required = false;
        }
        const Error fingerprint_error = state.refresh_disk_fingerprint();
        minibuffer_message(minibuffer,
                           fingerprint_error.ok() ? "Saved " + request.path
                                                  : "Saved, but " + fingerprint_error.message);
        return true;
    } else {
        minibuffer_message(minibuffer, save_error.message);
        return false;
    }
}

void request_save_editor_to_path(EditorState& state,
                                 const std::string& path,
                                 MinibufferState& minibuffer,
                                 bool update_path,
                                 bool quit_after_save,
                                 bool& quit,
                                 PendingSaveRequest& pending_save,
                                 const EditorSettings& settings) {
    PendingSaveRequest request;
    request.path = path;
    request.update_path = update_path;
    request.quit_after_save = quit_after_save;
    Error canonical_error = canonicalize_editor_target(path, request.canonical_path);
    if (!canonical_error.ok()) {
        minibuffer_message(minibuffer, canonical_error.message);
        return;
    }
    const bool current_target = !state.canonical_path.empty() &&
                                request.canonical_path == state.canonical_path;
    if (current_target) {
        if (state.read_only || !state.file_lock) {
            minibuffer_message(minibuffer,
                               "current buffer is read-only; use Save As to a different path");
            return;
        }
        request.destination_lock = state.file_lock;
    } else {
        EditorLockAttempt attempt = acquire_editor_file_lock(request.canonical_path);
        if (!attempt.lock) {
            minibuffer_message(minibuffer, attempt.error.message);
            return;
        }
        request.destination_lock = std::move(attempt.lock);
    }
    Error inspect_error = fingerprint_file(request.canonical_path, request.observed_disk);
    if (!inspect_error.ok()) {
        minibuffer_message(minibuffer, inspect_error.message);
        return;
    }
    request.external_change = current_target && state.has_disk_fingerprint &&
                              request.observed_disk != state.disk_fingerprint;
    if ((!current_target && request.observed_disk.exists) || request.external_change) {
        pending_save = std::move(request);
        start_minibuffer(minibuffer,
                         MinibufferAction::ConfirmOverwrite,
                         pending_save.external_change
                             ? path +
                                   " changed, was replaced, or was deleted since it was loaded. "
                                   "Press y to overwrite or any other key to cancel: "
                             : overwrite_prompt_message(path));
        return;
    }
    const bool saved = save_editor_to_path(state,
                                           std::move(request),
                                           pending_save,
                                           minibuffer,
                                           settings);
    if (quit_after_save && saved) {
        quit = true;
    }
}

std::string file_size_warning_message(const std::string& path, std::uintmax_t size) {
    return "Warning: " + path + " is " + std::to_string(size) + " bytes";
}

void load_editor_from_path(EditorState& state,
                           const std::string& path,
                           const EditorSettings& settings,
                           MinibufferState& minibuffer) {
    LoadedFile loaded;
    Error load_error = load_file(path, settings, loaded);
    if (load_error.ok()) {
        const bool mixed = loaded.mixed_linebreaks;
        reset_editor_buffer(state, std::move(loaded), path);
        minibuffer_message(minibuffer,
                           mixed ? "Warning: mixed line endings in " + path +
                                       "; normalized and using " +
                                       linebreak_name(state.linebreak) + " for saves"
                                 : "Loaded " + path);
    } else {
        minibuffer_message(minibuffer, load_error.message);
    }
}

void recover_editor_from_autosave(EditorState& state,
                                  const std::string& path,
                                  const std::string& autosave_path,
                                  const EditorSettings& settings,
                                  MinibufferState& minibuffer) {
    LoadedFile loaded;
    Error load_error = load_file(autosave_path, settings, loaded);
    if (load_error.ok()) {
        const bool mixed = loaded.mixed_linebreaks;
        reset_editor_buffer(state, std::move(loaded), path);
        state.dirty = true;
        minibuffer_message(minibuffer,
                           mixed ? "Warning: mixed line endings in recovered auto-save " +
                                       autosave_path + "; using " +
                                       linebreak_name(state.linebreak) + " for saves"
                                 : "Recovered auto-save from " + autosave_path);
    } else {
        minibuffer_message(minibuffer, load_error.message);
    }
}

bool offer_autosave_recovery_before_load(const std::string& path,
                                         const EditorSettings& settings,
                                         MinibufferState& minibuffer,
                                         PendingAutosaveRecovery& pending) {
    const AutosaveRecoveryOffer offer = check_autosave_recovery_offer(path, settings);
    if (!offer.should_offer) {
        return false;
    }
    pending.path = path;
    pending.autosave_path = offer.autosave_path;
    start_minibuffer(minibuffer,
                     MinibufferAction::ConfirmAutosaveRecovery,
                     autosave_recovery_prompt_message(path, offer.autosave_path) + " (y/n) ");
    return true;
}

void request_load_editor_from_path(EditorState& state,
                                   const std::string& path,
                                   const EditorSettings& settings,
                                   MinibufferState& minibuffer,
                                   std::string& pending_load_path,
                                   PendingAutosaveRecovery& pending_autosave_recovery) {
    FileLoadCheck check;
    Error err = check_load_file_size(path, settings, check);
    if (!err.ok()) {
        minibuffer_message(minibuffer, err.message);
        return;
    }
    if (check.should_warn) {
        pending_load_path = path;
        start_minibuffer(minibuffer,
                         MinibufferAction::ConfirmLoad,
                         file_size_warning_message(path, check.size) + "; load anyway? (y/n) ");
        return;
    }
    if (offer_autosave_recovery_before_load(path, settings, minibuffer, pending_autosave_recovery)) {
        return;
    }
    load_editor_from_path(state, path, settings, minibuffer);
}

bool confirm_huge_load_before_terminal(const std::string& path, const FileLoadCheck& check) {
    std::cerr << file_size_warning_message(path, check.size)
              << ". Load anyway? [y/N] ";
    std::cerr.flush();
    std::string response;
    if (!std::getline(std::cin, response)) {
        return false;
    }
    return ui::yes_answer(trim_ascii_copy(response));
}

std::string search_found_message(const std::string& needle) {
    return "Found: " + needle;
}

std::string search_not_found_message(const std::string& needle) {
    return "Search not found: " + needle;
}

std::string replacement_count_message(size_t replacements) {
    std::ostringstream out;
    out << "Replaced " << replacements << " occurrence";
    if (replacements != 1) {
        out << "s";
    }
    return out.str();
}

std::string replace_prompt_message(const std::string& needle) {
    return "Replace '" + needle + "': Space replace | s skip | a all | Esc done";
}

void finish_replace_session(ReplaceSession& replace, MinibufferState& minibuffer, std::string message) {
    replace = ReplaceSession{};
    minibuffer_message(minibuffer, std::move(message));
}

bool find_replace_match(EditorState& state, ReplaceSession& replace, size_t start) {
    if (replace.needle.empty()) {
        return false;
    }
    const std::string haystack = state.text.str();
    if (haystack.empty()) {
        return false;
    }
    const size_t found = haystack.find(replace.needle, std::min(start, haystack.size()));
    if (found == std::string::npos) {
        replace.match_valid = false;
        return false;
    }
    replace.active = true;
    replace.match_valid = true;
    replace.match_start = found;
    state.cursor = found;
    update_preferred_column(state);
    return true;
}

void begin_replace_choices(EditorState& state, MinibufferState& minibuffer, ReplaceSession& replace) {
    minibuffer.active = false;
    minibuffer.action = MinibufferAction::None;
    minibuffer.prompt.clear();
    minibuffer.input.clear();
    if (find_replace_match(state, replace, state.cursor)) {
        minibuffer.message = replace_prompt_message(replace.needle);
    } else {
        finish_replace_session(replace, minibuffer, search_not_found_message(replace.needle));
    }
}

void submit_minibuffer(EditorState& state,
                       MinibufferState& minibuffer,
                       bool& quit,
                       std::string& last_search,
                       ReplaceSession& replace,
                       const EditorSettings& settings,
                       std::string& pending_load_path,
                       bool& pending_quit_after_save,
                       PendingSaveRequest& pending_save,
                       PendingAutosaveRecovery& pending_autosave_recovery,
                       PathCompleter& path_completer) {
    const MinibufferAction action = minibuffer.action;
    const std::string value = trim_ascii_copy(minibuffer.input);
    const std::string raw_value = minibuffer.input;
    if (action == MinibufferAction::SaveFile) {
        if (value.empty()) {
            minibuffer.prompt = pending_quit_after_save ? "Save file before quit (path required): "
                                                        : "Save file (path required): ";
            return;
        }
        request_save_editor_to_path(state,
                                    expand_user_path(value),
                                    minibuffer,
                                    true,
                                    pending_quit_after_save,
                                    quit,
                                    pending_save,
                                    settings);
        if (!pending_save.path.empty()) {
            return;
        }
        pending_quit_after_save = false;
        return;
    }
    if (action == MinibufferAction::SaveAsFile) {
        if (value.empty()) {
            minibuffer.prompt = "Save as (path required): ";
            return;
        }
        request_save_editor_to_path(state,
                                    expand_user_path(value),
                                    minibuffer,
                                    true,
                                    false,
                                    quit,
                                    pending_save,
                                    settings);
        return;
    }
    if (action == MinibufferAction::LoadFile) {
        if (value.empty()) {
            minibuffer.prompt = "Load file (path required): ";
            return;
        }
        request_load_editor_from_path(state,
                                      expand_user_path(value),
                                      settings,
                                      minibuffer,
                                      pending_load_path,
                                      pending_autosave_recovery);
        return;
    }
    if (action == MinibufferAction::Search) {
        if (value.empty()) {
            minibuffer.prompt = "Search (substring required): ";
            return;
        }
        last_search = value;
        minibuffer_message(minibuffer,
                           state.search(value) ? search_found_message(value)
                                               : search_not_found_message(value));
        return;
    }
    if (action == MinibufferAction::ReplaceSearch) {
        if (raw_value.empty()) {
            minibuffer.prompt = "Replace search (substring required): ";
            return;
        }
        replace = ReplaceSession{};
        replace.active = true;
        replace.needle = raw_value;
        last_search = raw_value;
        start_minibuffer(minibuffer, MinibufferAction::ReplaceWith, "Replace with: ");
        return;
    }
    if (action == MinibufferAction::ReplaceWith) {
        replace.replacement = raw_value;
        begin_replace_choices(state, minibuffer, replace);
        return;
    }
    if (action == MinibufferAction::GotoLine) {
        std::string message;
        if (value.empty()) {
            minibuffer.prompt = "Goto line: ";
            minibuffer.input.clear();
            return;
        }
        if (state.goto_line(value, message)) {
            minibuffer_message(minibuffer, message);
        } else {
            // Keep the prompt open so the user can correct a bad line number.
            minibuffer.prompt = message + " — Goto line: ";
            minibuffer.input.clear();
        }
        return;
    }
    if (action == MinibufferAction::TextAlignWidth) {
        size_t width = minibuffer.text_align_default_width;
        if (!value.empty()) {
            size_t parsed = 0;
            const char* begin = value.data();
            const char* end = begin + value.size();
            const std::from_chars_result fc = std::from_chars(begin, end, parsed);
            if (fc.ec != std::errc{} || fc.ptr != end || !valid_text_align_width(parsed)) {
                minibuffer.prompt =
                    "Enter width for the text-alignment (" +
                    std::to_string(minibuffer.text_align_default_width) +
                    " default); width must be 21–1000: ";
                minibuffer.input.clear();
                return;
            }
            width = parsed;
        } else if (!valid_text_align_width(width)) {
            minibuffer_message(minibuffer,
                               "Invalid default alignment width; set [editor] alignment-width "
                               "greater than 20");
            return;
        }
        TextAlignMode mode = static_cast<TextAlignMode>(minibuffer.text_align_mode);
        Error writable = state.mutation_allowed();
        if (!writable.ok()) {
            minibuffer_message(minibuffer, writable.message);
            return;
        }
        size_t start = 0;
        size_t end = 0;
        bool had_selection = false;
        text_layout_scope(state, start, end, had_selection);
        const std::string scope = state.text.range_text(start, end - start);
        TextLayoutResult layout = reflow_align(scope, mode, width);
        if (!layout.error.ok()) {
            minibuffer_message(minibuffer, layout.error.message);
            return;
        }
        Error apply_error =
            apply_text_layout_result(state, start, end, had_selection, layout);
        if (!apply_error.ok()) {
            minibuffer_message(minibuffer, apply_error.message);
            return;
        }
        if (!layout.changed) {
            minibuffer_message(minibuffer, "No changes");
            return;
        }
        minibuffer_message(minibuffer,
                           std::string(text_align_mode_name(mode)) + " to " +
                               std::to_string(width) + " columns");
        return;
    }
    if (action == MinibufferAction::ConfirmLoad) {
        if (ui::yes_answer(value)) {
            const std::string path = pending_load_path;
            pending_load_path.clear();
            if (offer_autosave_recovery_before_load(path, settings, minibuffer, pending_autosave_recovery)) {
                return;
            }
            load_editor_from_path(state, path, settings, minibuffer);
        } else if (ui::no_answer(value) || value.empty()) {
            pending_load_path.clear();
            minibuffer_message(minibuffer, "Load cancelled");
        } else {
            minibuffer.prompt = ui::kConfirmationRetryPrompt;
            minibuffer.input.clear();
        }
        return;
    }
    if (action == MinibufferAction::ConfirmAutosaveRecovery) {
        const PendingAutosaveRecovery recovery = pending_autosave_recovery;
        pending_autosave_recovery = PendingAutosaveRecovery{};
        if (ui::yes_answer(value)) {
            recover_editor_from_autosave(state,
                                         recovery.path,
                                         recovery.autosave_path,
                                         settings,
                                         minibuffer);
        } else if (ui::no_answer(value) || value.empty()) {
            load_editor_from_path(state, recovery.path, settings, minibuffer);
        } else {
            pending_autosave_recovery = recovery;
            minibuffer.prompt = ui::kConfirmationRetryPrompt;
            minibuffer.input.clear();
        }
        return;
    }
    if (action == MinibufferAction::ConfirmQuit) {
        if (ui::yes_answer(value)) {
            quit = true;
        } else if (ui::no_answer(value) || value.empty()) {
            minibuffer_message(minibuffer, "Quit cancelled");
        } else {
            minibuffer.prompt = ui::kConfirmationRetryPrompt;
            minibuffer.input.clear();
        }
        return;
    }
    if (action == MinibufferAction::ConfirmSaveOnQuit) {
        if (ui::yes_answer(value)) {
            pending_quit_after_save = true;
            start_minibuffer(minibuffer,
                             MinibufferAction::SaveFile,
                             "Save file: ",
                             "",
                             &path_completer);
        } else if (ui::no_answer(value) || value.empty()) {
            quit = true;
        } else {
            minibuffer.prompt = ui::kConfirmationRetryPrompt;
            minibuffer.input.clear();
        }
        return;
    }
    if (action == MinibufferAction::ConfirmOverwrite) {
        if (ui::yes_answer(value)) {
            PendingSaveRequest request = pending_save;
            pending_save = PendingSaveRequest{};
            const bool quit_after_save = request.quit_after_save;
            const bool saved = save_editor_to_path(state,
                                                   std::move(request),
                                                   pending_save,
                                                   minibuffer,
                                                   settings);
            if (quit_after_save && saved) {
                quit = true;
            }
            if (pending_save.path.empty()) pending_quit_after_save = false;
        } else {
            pending_save = PendingSaveRequest{};
            pending_quit_after_save = false;
            minibuffer_message(minibuffer, "Save cancelled");
        }
        return;
    }
    if (action == MinibufferAction::ConfirmReloadAfterLock) {
        if (ui::yes_answer(value)) {
            FileFingerprint loaded_fingerprint;
            Error inspect_error = fingerprint_file(state.canonical_path, loaded_fingerprint);
            if (!inspect_error.ok()) {
                minibuffer_message(minibuffer, inspect_error.message);
                return;
            }
            LoadedFile loaded;
            Error load_error = load_file(state.path, settings, loaded);
            if (!load_error.ok()) {
                minibuffer_message(minibuffer, load_error.message);
                return;
            }
            reset_editor_buffer(state, std::move(loaded), state.path);
            state.read_only = false;
            state.reload_required = false;
            state.disk_fingerprint = loaded_fingerprint;
            state.has_disk_fingerprint = true;
            minibuffer_message(minibuffer,
                               "Reloaded changed file; buffer is writable. Repeat the edit");
        } else if (ui::no_answer(value) || value.empty()) {
            state.file_lock.reset();
            state.reload_required = false;
            state.read_only = true;
            minibuffer_message(minibuffer, "Reload declined; buffer remains read-only");
        } else {
            minibuffer.prompt = ui::kConfirmationRetryPrompt;
            minibuffer.input.clear();
        }
        return;
    }
    minibuffer_message(minibuffer, "");
}

bool handle_minibuffer_key(EditorState& state,
                           MinibufferState& minibuffer,
                           unsigned char ch,
                           bool& quit,
                           std::string& last_search,
                           ReplaceSession& replace,
                           const EditorSettings& settings,
                           std::string& pending_load_path,
                           bool& pending_quit_after_save,
                           PendingSaveRequest& pending_save,
                           PendingAutosaveRecovery& pending_autosave_recovery,
                           PathCompleter& path_completer) {
    if (!minibuffer.active) {
        return false;
    }
    if (is_assist_minibuffer_action(minibuffer.action)) {
        return false;
    }
    if (ch == 27) {
        replace = ReplaceSession{};
        pending_load_path.clear();
        pending_autosave_recovery = PendingAutosaveRecovery{};
        pending_quit_after_save = false;
        pending_save = PendingSaveRequest{};
        path_completer.reset();
        minibuffer_message(minibuffer, "Minibuffer cancelled");
        return true;
    }
    if (ch == '\r' || ch == '\n') {
        submit_minibuffer(state,
                          minibuffer,
                          quit,
                          last_search,
                          replace,
                          settings,
                          pending_load_path,
                          pending_quit_after_save,
                          pending_save,
                          pending_autosave_recovery,
                          path_completer);
        return true;
    }
    if (ch == 127 || ch == 8) {
        if (!minibuffer.input.empty()) {
            minibuffer.input.pop_back();
            if (minibuffer_supports_path_completion(minibuffer.action)) {
                path_completer.reset();
            }
        }
        return true;
    }
    if (ch == 19 || ch == editor_key_save_as()) {
        return false;
    }
    if (minibuffer.action == MinibufferAction::ConfirmOverwrite) {
        if (ch == 'y' || ch == 'Y' || ch == 19) {
            PendingSaveRequest request = pending_save;
            pending_save = PendingSaveRequest{};
            const bool quit_after_save = request.quit_after_save;
            const bool saved = save_editor_to_path(state,
                                                   std::move(request),
                                                   pending_save,
                                                   minibuffer,
                                                   settings);
            if (quit_after_save && saved) {
                quit = true;
            }
            if (pending_save.path.empty()) pending_quit_after_save = false;
        } else {
            pending_save = PendingSaveRequest{};
            pending_quit_after_save = false;
            minibuffer_message(minibuffer, "Save cancelled");
        }
        return true;
    }
    if (minibuffer.action == MinibufferAction::ConfirmReloadAfterLock) {
        if (ch == 'y' || ch == 'Y') {
            FileFingerprint loaded_fingerprint;
            Error inspect_error = fingerprint_file(state.canonical_path, loaded_fingerprint);
            if (!inspect_error.ok()) {
                minibuffer_message(minibuffer, inspect_error.message);
                return true;
            }
            LoadedFile loaded;
            Error load_error = load_file(state.path, settings, loaded);
            if (!load_error.ok()) {
                minibuffer_message(minibuffer, load_error.message);
                return true;
            }
            reset_editor_buffer(state, std::move(loaded), state.path);
            state.read_only = false;
            state.reload_required = false;
            state.disk_fingerprint = loaded_fingerprint;
            state.has_disk_fingerprint = true;
            minibuffer_message(minibuffer,
                               "Reloaded changed file; buffer is writable. Repeat the edit");
        } else if (ch == 'n' || ch == 'N') {
            state.file_lock.reset();
            state.reload_required = false;
            state.read_only = true;
            minibuffer_message(minibuffer, "Reload declined; buffer remains read-only");
        } else {
            minibuffer.prompt = ui::kConfirmationRetryPrompt;
        }
        return true;
    }
    if (minibuffer.action == MinibufferAction::ConfirmAutosaveRecovery) {
        const PendingAutosaveRecovery recovery = pending_autosave_recovery;
        pending_autosave_recovery = PendingAutosaveRecovery{};
        if (ch == 'y' || ch == 'Y') {
            recover_editor_from_autosave(state,
                                         recovery.path,
                                         recovery.autosave_path,
                                         settings,
                                         minibuffer);
        } else if (ch == 'n' || ch == 'N') {
            load_editor_from_path(state, recovery.path, settings, minibuffer);
        } else {
            pending_autosave_recovery = recovery;
            minibuffer.prompt = ui::kConfirmationRetryPrompt;
        }
        return true;
    }
    if (minibuffer.action == MinibufferAction::ConfirmQuit ||
        minibuffer.action == MinibufferAction::ConfirmLoad ||
        minibuffer.action == MinibufferAction::ConfirmSaveOnQuit) {
        if (ch == 'y' || ch == 'Y') {
            if (minibuffer.action == MinibufferAction::ConfirmQuit) {
                quit = true;
            } else if (minibuffer.action == MinibufferAction::ConfirmLoad) {
                const std::string path = pending_load_path;
                pending_load_path.clear();
                if (offer_autosave_recovery_before_load(path, settings, minibuffer, pending_autosave_recovery)) {
                    return true;
                }
                load_editor_from_path(state, path, settings, minibuffer);
            } else {
                pending_quit_after_save = true;
                start_minibuffer(minibuffer,
                                 MinibufferAction::SaveFile,
                                 "Save file: ",
                                 "",
                                 &path_completer);
            }
        } else if (ch == 'n' || ch == 'N') {
            if (minibuffer.action == MinibufferAction::ConfirmLoad) {
                pending_load_path.clear();
                minibuffer_message(minibuffer, "Load cancelled");
            } else if (minibuffer.action == MinibufferAction::ConfirmSaveOnQuit) {
                quit = true;
            } else {
                minibuffer_message(minibuffer, "Quit cancelled");
            }
        } else {
            minibuffer.prompt = ui::kConfirmationRetryPrompt;
        }
        return true;
    }
    if (ch == '\t') {
        if (minibuffer_supports_path_completion(minibuffer.action)) {
            const PathCompletionResult result = complete_path_input(minibuffer.input, path_completer);
            minibuffer.message = path_completion_status(result);
        } else {
            minibuffer.message = "Tab completion is not active here";
        }
        return true;
    }
    if (ch >= 0x20U) {
        minibuffer.input.push_back(static_cast<char>(ch));
        if (minibuffer_supports_path_completion(minibuffer.action)) {
            EditorState temp = EditorState::from_text(minibuffer.input);
            temp.cursor = minibuffer.input.size();
            if (!path_completer.can_cycle(temp)) {
                path_completer.reset();
            }
        }
        return true;
    }
    return true;
}

void discard_escape_sequence_tail() {
    unsigned char ch = 0;
    std::string sequence;
    while (sequence.size() < 16 && read_terminal_byte(ch, 1)) {
        sequence.push_back(static_cast<char>(ch));
        if (sequence.size() == 1 && ch == 'O') {
            continue;
        }
        if ((ch >= 'A' && ch <= 'Z') || ch == '~') {
            break;
        }
    }
}

void replace_current_match(EditorState& state, MinibufferState& minibuffer, ReplaceSession& replace) {
    if (!replace.match_valid && !find_replace_match(state, replace, state.cursor)) {
        finish_replace_session(replace, minibuffer, "Replace complete");
        return;
    }

    const size_t replaced_at = replace.match_start;
    Error err = state.replace(replaced_at, replace.needle.size(), replace.replacement);
    if (!err.ok()) {
        finish_replace_session(replace, minibuffer, err.message);
        return;
    }

    const size_t next_start = replaced_at + replace.replacement.size();
    if (find_replace_match(state, replace, next_start)) {
        minibuffer.message = replacement_count_message(1) + ". " + replace_prompt_message(replace.needle);
    } else {
        finish_replace_session(replace, minibuffer, replacement_count_message(1) + "; no more matches");
    }
}

void skip_current_match(EditorState& state, MinibufferState& minibuffer, ReplaceSession& replace) {
    if (!replace.match_valid && !find_replace_match(state, replace, state.cursor)) {
        finish_replace_session(replace, minibuffer, "Replace complete");
        return;
    }

    const size_t next_start = replace.match_start + replace.needle.size();
    if (find_replace_match(state, replace, next_start)) {
        minibuffer.message = replace_prompt_message(replace.needle);
    } else {
        finish_replace_session(replace, minibuffer, "Replace complete");
    }
}

void replace_all_remaining(EditorState& state, MinibufferState& minibuffer, ReplaceSession& replace) {
    const size_t start = replace.match_valid ? replace.match_start : state.cursor;
    size_t replacements = 0;
    Error err = state.replace_all_from(start, replace.needle, replace.replacement, replacements);
    if (!err.ok()) {
        finish_replace_session(replace, minibuffer, err.message);
        return;
    }
    finish_replace_session(replace, minibuffer, replacement_count_message(replacements));
}

bool handle_replace_key(EditorState& state,
                        MinibufferState& minibuffer,
                        ReplaceSession& replace,
                        unsigned char ch) {
    if (!replace.active || minibuffer.active) {
        return false;
    }
    if (ch == 27) {
        discard_escape_sequence_tail();
        finish_replace_session(replace, minibuffer, "Replace ended");
        return true;
    }
    if (ch == ' ') {
        replace_current_match(state, minibuffer, replace);
        return true;
    }
    if (ch == 's' || ch == 'S') {
        skip_current_match(state, minibuffer, replace);
        return true;
    }
    if (ch == 'a' || ch == 'A') {
        replace_all_remaining(state, minibuffer, replace);
        return true;
    }
    minibuffer.message = replace_prompt_message(replace.needle);
    return true;
}

void render_terminal(EditorState& state,
                     const MinibufferState& minibuffer,
                     tui::detail::TerminalFrameRenderer& frame_renderer,
                     const TerminalThemeStyle& theme_style,
                     bool help_view,
                     const EditorAssistDisplay* assist_display,
                     bool show_scrollbars,
                     bool follow_cursor,
                     const EditorStatusChrome& status_chrome,
                     const char* status_text_override,
                     const std::vector<bool>* changed_source_lines) {
    SplitPaneRect single;
    single.buffer_index = 0;
    single.leaf_index = 0;
    single.rect = editor_main_area();
    single.focused = true;
    render_terminal_splits(
        {single},
        [&](size_t) -> const EditorState& { return state; },
        state,
        minibuffer,
        frame_renderer,
        theme_style,
        help_view,
        assist_display,
        1,
        show_scrollbars,
        follow_cursor,
        status_chrome,
        status_text_override,
        changed_source_lines);
}

void render_terminal_splits(
    const std::vector<SplitPaneRect>& panes,
    const std::function<const EditorState&(size_t buffer_index)>& buffer_at,
    EditorState& focused_state,
    const MinibufferState& minibuffer,
    tui::detail::TerminalFrameRenderer& frame_renderer,
    const TerminalThemeStyle& theme_style,
    bool help_view,
    const EditorAssistDisplay* assist_display,
    size_t pane_count_hint,
    bool show_scrollbars,
    bool follow_cursor,
    const EditorStatusChrome& status_chrome,
    const char* status_text_override,
    const std::vector<bool>* changed_source_lines) {
    const TerminalSize size = terminal_size();
    const int rows = std::max(3, size.rows);
    const int cols = std::max(20, size.cols);
    const int width = std::max(1, cols - 1);
    const Rect main_area = editor_main_area();
    const size_t pane_count = pane_count_hint > 0 ? pane_count_hint : panes.size();
    tui::detail::TerminalFrame frame(rows, cols);

    int cursor_row = main_area.row;
    int cursor_col = main_area.col;
    for (const SplitPaneRect& pane : panes) {
        if (pane.rect.width <= 0 || pane.rect.height <= 0) {
            continue;
        }
        const Rect content_rect = editor_content_rect(pane.rect);
        const bool show_scrollbar = show_scrollbars && pane.rect.width >= 2;
        const EditorState* source = &buffer_at(pane.buffer_index);
        if (pane.focused) {
            source = &focused_state;
            if (follow_cursor) focused_state.ensure_cursor_visible(content_rect);
        }
        // Per-pane view: focused uses live state; unfocused uses leaf-stored view.
        PaneViewState view =
            pane.focused ? pane_view_from_state(focused_state) : pane.view;
        view.cursor = std::min(view.cursor, source->text.size());
        const size_t content_rows = editor_total_visual_rows(
            *source, static_cast<size_t>(std::max(1, content_rect.width)));
        clamp_pane_scroll(view.scroll_line,
                          content_rows,
                          static_cast<size_t>(std::max(0, content_rect.height)));
        // History-diff marks apply only to the focused pane (dired single-view path).
        const std::vector<bool>* pane_changed =
            pane.focused ? changed_source_lines : nullptr;
        const RenderedPanel panel = source->render(
            content_rect, view.cursor, view.scroll_line, view.scroll_column, pane_changed);
        ui::ScrollbarMetrics bar_metrics;
        if (show_scrollbar) {
            bar_metrics = ui::compute_vertical_scrollbar(
                static_cast<size_t>(std::max(0, pane.rect.height)),
                content_rows,
                view.scroll_line);
        }
        for (int row = 0; row < pane.rect.height; ++row) {
            const int terminal_row = pane.rect.row + row;
            std::string command = terminal_position(terminal_row, pane.rect.col);
            if (row < static_cast<int>(panel.lines.size())) {
                const size_t index = static_cast<size_t>(row);
                const std::vector<RenderedPanel::Span> empty_spans;
                const std::vector<RenderedPanel::Span>& spans =
                    index < panel.line_spans.size() ? panel.line_spans[index] : empty_spans;
                const bool row_changed =
                    index < panel.line_changed_bg.size() && panel.line_changed_bg[index];
                // Clip to content width; render() already sizes lines to content_rect.width.
                append_editor_rendered_line(
                    command, panel.lines[index], spans, theme_style, row_changed);
            }
            if (show_scrollbar) {
                // Position the scrollbar on the pane's rightmost column.
                command += terminal_position(terminal_row,
                                             pane.rect.col + pane.rect.width - 1);
                append_scrollbar_cell(
                    command, theme_style, ui::scrollbar_glyph_at(bar_metrics, row));
            }
            frame.append_to_row(terminal_row, std::move(command));
        }
        // Dim vertical/horizontal edge marker on focused pane border via reverse on last
        // column of non-focused is intentionally skipped; empty cleared gaps act as dividers.
        if (pane.focused && panel.cursor.visible) {
            cursor_row = pane.rect.row + panel.cursor.row;
            cursor_col = pane.rect.col + panel.cursor.col;
            frame.cursor_visible = true;
        } else if (pane.focused) {
            frame.cursor_visible = false;
        }
    }

    // Draw themed separator glyphs between adjacent panes when multiple exist.
    if (panes.size() > 1) {
        for (size_t i = 0; i < panes.size(); ++i) {
            for (size_t j = i + 1; j < panes.size(); ++j) {
                const Rect& a = panes[i].rect;
                const Rect& b = panes[j].rect;
                // Vertical separator: b starts immediately to the right of a (+1 gap).
                if (a.row == b.row && a.height == b.height && b.col == a.col + a.width + 1) {
                    const int sep_col = a.col + a.width;
                    for (int row = 0; row < a.height; ++row) {
                        const int terminal_row = a.row + row;
                        std::string sep = terminal_position(terminal_row, sep_col);
                        append_pane_separator_glyph(sep, theme_style, u8"│");
                        frame.append_to_row(terminal_row, std::move(sep));
                    }
                }
                if (b.row == a.row && b.height == a.height && a.col == b.col + b.width + 1) {
                    const int sep_col = b.col + b.width;
                    for (int row = 0; row < b.height; ++row) {
                        const int terminal_row = b.row + row;
                        std::string sep = terminal_position(terminal_row, sep_col);
                        append_pane_separator_glyph(sep, theme_style, u8"│");
                        frame.append_to_row(terminal_row, std::move(sep));
                    }
                }
                // Horizontal separator.
                if (a.col == b.col && a.width == b.width && b.row == a.row + a.height + 1) {
                    const int sep_row = a.row + a.height;
                    std::string separator = terminal_position(sep_row, a.col);
                    if (theme_style.use_colors && theme_style.themes != nullptr) {
                        separator += tui::style_sequence_for(*theme_style.themes,
                                                             theme_style.theme_name,
                                                             tui::StyleRole::PanelBorder, theme_style.color_mode);
                    }
                    for (int col = 0; col < a.width; ++col) {
                        separator += u8"─";
                    }
                    if (theme_style.use_colors && theme_style.themes != nullptr) {
                        separator += "\x1b[0m";
                    }
                    frame.append_to_row(sep_row, std::move(separator));
                }
                if (b.col == a.col && b.width == a.width && a.row == b.row + b.height + 1) {
                    const int sep_row = b.row + b.height;
                    std::string separator = terminal_position(sep_row, b.col);
                    if (theme_style.use_colors && theme_style.themes != nullptr) {
                        separator += tui::style_sequence_for(*theme_style.themes,
                                                             theme_style.theme_name,
                                                             tui::StyleRole::PanelBorder, theme_style.color_mode);
                    }
                    for (int col = 0; col < b.width; ++col) {
                        separator += u8"─";
                    }
                    if (theme_style.use_colors && theme_style.themes != nullptr) {
                        separator += "\x1b[0m";
                    }
                    frame.append_to_row(sep_row, std::move(separator));
                }
            }
        }
    }

    const int status_row = rows - 1;
    const int minibuffer_row = rows;
    const std::string status_source =
        status_text_override != nullptr
            ? std::string(status_text_override)
            : editor_status_line(focused_state, help_view, pane_count, status_chrome);
    const std::string status_text = pad_or_clip_ascii(status_source, width);
    std::string status_command = terminal_position(status_row, 1);
    if (theme_style.use_colors && theme_style.themes != nullptr) {
        status_command += tui::style_sequence_for(
            *theme_style.themes, theme_style.theme_name, tui::StyleRole::Status, theme_style.color_mode);
    } else {
        status_command += "\x1b[7m";
    }
    status_command += status_text + "\x1b[0m\x1b[K";
    frame.set_row(status_row, std::move(status_command));
    const bool show_assist_activity = assist_display != nullptr && assist_display->active &&
                                        assist_display->kind != tui::ActivityKind::None &&
                                        !minibuffer.active;
    const std::string minibuffer_line =
        show_assist_activity ? editor_assist_minibuffer_line(*assist_display, theme_style, width)
                             : pad_or_clip_ascii(minibuffer_text(minibuffer), width);
    std::string minibuffer_command = terminal_position(minibuffer_row, 1);
    if (theme_style.use_colors && theme_style.themes != nullptr && !show_assist_activity) {
        minibuffer_command += tui::style_sequence_for(
            *theme_style.themes, theme_style.theme_name, tui::StyleRole::Text, theme_style.color_mode);
    }
    minibuffer_command += minibuffer_line;
    if (theme_style.use_colors && theme_style.themes != nullptr && !show_assist_activity) {
        minibuffer_command += "\x1b[0m";
    }
    minibuffer_command += "\x1b[K";
    frame.set_row(minibuffer_row, std::move(minibuffer_command));

    if (minibuffer.active) {
        cursor_row = minibuffer_row;
        const size_t prompt_width = minibuffer.prompt.size();
        const size_t input_width = minibuffer.input.size();
        cursor_col = 1 + static_cast<int>(std::min<size_t>(
            static_cast<size_t>(std::max(0, width - 1)), prompt_width + input_width));
        frame.cursor_visible = true;
    }
    frame.cursor_row = cursor_row;
    frame.cursor_col = cursor_col;
    frame_renderer.present(frame, std::cout);
}

void render_terminal_panel(EditorState& state,
                           const MinibufferState& minibuffer,
                           tui::detail::TerminalFrameRenderer& frame_renderer,
                           const TerminalThemeStyle& theme_style,
                           tui::TuiMode mode,
                           int& panel_scroll,
                           const char* panel_title_override,
                           const char* status_text_override,
                           const std::vector<tui::StyledLine>* body_lines_override) {
    const TerminalSize size = terminal_size();
    const int rows = std::max(3, size.rows);
    const int cols = std::max(20, size.cols);
    const int width = std::max(1, cols - 1);
    const int panel_height = std::max(1, rows - 2);
    tui::detail::TerminalFrame frame(rows, cols);
    const tui::detail::RenderStyle render_style{theme_style.themes,
                                                 theme_style.theme_name,
                                                 theme_style.use_colors,
                                                 theme_style.color_mode};
    std::vector<tui::StyledLine> lines;
    if (body_lines_override != nullptr) {
        // Dired (and similar): custom multi-role body rows, same panel chrome as list pickers.
        const char* title =
            panel_title_override != nullptr ? panel_title_override : "Panel";
        {
            tui::StyledLine rule;
            rule.segments.push_back({u8"── ", tui::StyleRole::PanelBorder});
            rule.segments.push_back({title, tui::StyleRole::PanelTitle});
            std::string tail = " ";
            int used = 3 + static_cast<int>(std::string(title).size()) + 1;
            while (used < width) {
                tail += u8"─";
                ++used;
            }
            rule.segments.push_back({std::move(tail), tui::StyleRole::PanelBorder});
            lines.push_back(std::move(rule));
        }
        if (!body_lines_override->empty()) {
            {
                std::string fill;
                for (int i = 0; i < width; ++i) {
                    fill += u8"─";
                }
                lines.push_back({{{std::move(fill), tui::StyleRole::PanelBorder}}});
            }
            for (const tui::StyledLine& body : *body_lines_override) {
                lines.push_back(body);
            }
            {
                std::string fill;
                for (int i = 0; i < width; ++i) {
                    fill += u8"─";
                }
                lines.push_back({{{std::move(fill), tui::StyleRole::PanelBorder}}});
            }
        }
    } else {
        lines = tui::detail::panel_lines_for_text(state.text.str(), mode, width, panel_title_override);
    }

    int highlighted_line = -1;
    for (size_t index = 0; index < lines.size(); ++index) {
        if (lines[index].segments.empty()) {
            continue;
        }
        const tui::StyledSegment& first = lines[index].segments.front();
        if (first.role == tui::StyleRole::PanelHighlight || first.reverse) {
            highlighted_line = static_cast<int>(index);
            break;
        }
    }

    const int max_scroll = std::max(0, static_cast<int>(lines.size()) - panel_height);
    panel_scroll = std::min(std::max(0, panel_scroll), max_scroll);
    if (highlighted_line >= 0) {
        if (highlighted_line < panel_scroll) {
            panel_scroll = highlighted_line;
        } else if (highlighted_line >= panel_scroll + panel_height) {
            panel_scroll = highlighted_line - panel_height + 1;
        }
        panel_scroll = std::min(std::max(0, panel_scroll), max_scroll);
    }

    int printed = 0;
    for (int index = panel_scroll;
         index < static_cast<int>(lines.size()) && printed < panel_height;
         ++index, ++printed) {
        const int terminal_row = 1 + printed;
        frame.set_row(
            terminal_row,
            tui::detail::format_line(
                terminal_row, width,
                lines[static_cast<size_t>(index)].segments,
                tui::StyleRole::PanelBorder, render_style));
    }
    while (printed < panel_height) {
        const int terminal_row = 1 + printed;
        frame.set_row(
            terminal_row,
            tui::detail::format_line(
                terminal_row, width, "", tui::StyleRole::PanelBorder,
                render_style));
        ++printed;
    }

    int cursor_row = 1;
    if (highlighted_line >= 0) {
        cursor_row = 1 + highlighted_line - panel_scroll;
    }
    int cursor_col = 1;
    const int status_row = rows - 1;
    const int minibuffer_row = rows;
    const std::string status_source =
        status_text_override != nullptr ? std::string(status_text_override)
                                        : editor_status_line(state, false);
    const std::string status_text = pad_or_clip_ascii(status_source, width);
    std::string status_command = terminal_position(status_row, 1);
    if (theme_style.use_colors && theme_style.themes != nullptr) {
        status_command += tui::style_sequence_for(*theme_style.themes,
                                                  theme_style.theme_name,
                                                  tui::StyleRole::Status, theme_style.color_mode);
    } else {
        status_command += "\x1b[7m";
    }
    status_command += status_text + "\x1b[0m\x1b[K";
    frame.set_row(status_row, std::move(status_command));

    const std::string minibuffer_line = pad_or_clip_ascii(minibuffer_text(minibuffer), width);
    std::string minibuffer_command = terminal_position(minibuffer_row, 1);
    if (theme_style.use_colors && theme_style.themes != nullptr) {
        minibuffer_command += tui::style_sequence_for(*theme_style.themes,
                                                      theme_style.theme_name,
                                                      tui::StyleRole::Text, theme_style.color_mode);
    }
    minibuffer_command += minibuffer_line;
    if (theme_style.use_colors && theme_style.themes != nullptr) {
        minibuffer_command += "\x1b[0m";
    }
    minibuffer_command += "\x1b[K";
    frame.set_row(minibuffer_row, std::move(minibuffer_command));

    if (minibuffer.active) {
        cursor_row = minibuffer_row;
        const size_t prompt_width = minibuffer.prompt.size();
        const size_t input_width = minibuffer.input.size();
        cursor_col = 1 + static_cast<int>(std::min<size_t>(
            static_cast<size_t>(std::max(0, width - 1)), prompt_width + input_width));
    }
    frame.cursor_row = cursor_row;
    frame.cursor_col = cursor_col;
    frame_renderer.present(frame, std::cout);
}

std::string read_escape_suffix() {
    std::string sequence;
    unsigned char ch = 0;
    while (sequence.size() < 16 && read_terminal_byte(ch, 25)) {
        sequence.push_back(static_cast<char>(ch));
        if (sequence.size() == 1 && ch == 'O') {
            continue;
        }
        if ((ch >= 'A' && ch <= 'Z') || ch == '~') {
            break;
        }
    }
    if (!sequence.empty() &&
        std::all_of(sequence.begin(), sequence.end(), [](char byte) { return byte == '\x1b'; })) {
        return {};
    }
    return sequence;
}

void dispatch_escape_sequence(EditorState& state,
                            const std::string& sequence,
                            std::string& status,
                            const std::string& last_search,
                            const Rect* panel_rect) {
    if (sequence.empty()) {
        return;
    }

    MovementKeyEvent movement;
    if (parse_movement_sequence(sequence, movement)) {
        const Rect rect =
            panel_rect != nullptr ? *panel_rect : editor_content_rect(editor_main_area());
        state.apply_movement(movement.key, rect, movement.shift, movement.alt, movement.ctrl);
        return;
    }

    if (sequence == "[3~") {
        const bool had_selection = state.selection.has_range();
        Error err = state.erase_at_cursor();
        if (!err.ok()) {
            status = err.message;
        } else if (had_selection) {
            // Selection delete acts as cut; publish for terminal clipboard clients.
            publish_terminal_clipboard(shared_clipboard().text());
            status = "Cut selection";
        }
    } else if (sequence == "OR" || sequence == "[13~" || sequence == "[[C") {
        if (last_search.empty()) {
            status = "No search string; use Ctrl+F";
        } else {
            status = state.search_next(last_search) ? search_found_message(last_search)
                                                    : search_not_found_message(last_search);
        }
    } else if (sequence == "[1;2R" || sequence == "O1;2R" ||
               sequence == "[13;2~" || sequence == "[25~") {
        if (last_search.empty()) {
            status = "No search string; use Ctrl+F";
        } else {
            status = state.search_previous(last_search) ? search_found_message(last_search)
                                                        : search_not_found_message(last_search);
        }
    }
}

void handle_escape(EditorState& state,
                   std::string& status,
                   const std::string& last_search,
                   const Rect* panel_rect) {
    dispatch_escape_sequence(state, read_escape_suffix(), status, last_search, panel_rect);
}


}  // namespace ainiux::editor
