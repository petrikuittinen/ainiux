#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "chat/session.hpp"
#include "chat/sqlite_store.hpp"
#include "editor/editor.hpp"
#include "editor/terminal_input.hpp"
#include "tui/events.hpp"
#include "tui/prompt_recall.hpp"
#include "tui/tui.hpp"

namespace ainiux::tui {

std::string thread_picker_text(const std::vector<chat::ThreadSummary>& threads, size_t selected);
std::string thread_picker_label(const chat::ThreadSummary& thread);
std::string attachment_picker_text(const std::vector<ChatAttachment>& attachments, size_t selected);
std::string remove_confirm_text(const chat::Session& session);
std::string system_edit_text();
std::string history_edit_text();
std::string join_models_preview(const std::vector<std::string>& models);

void scroll_chat_history_page_up(const Layout& layout, int& history_scroll);
void scroll_chat_history_page_down(const Layout& layout, int& history_scroll);
bool apply_chat_mouse_scroll(const editor::MouseInputEvent& mouse,
                             const Layout& layout,
                             TuiMode mode,
                             int& history_scroll);

bool apply_chat_history_scroll(const editor::MovementKeyEvent& movement,
                               const Layout& layout,
                               int& history_scroll);

EscapeResult handle_escape(editor::EditorState& input,
                           const Layout& layout,
                           int& history_scroll,
                           std::string& status,
                           bool input_only_movement = false,
                           PromptRecall* prompt_recall = nullptr,
                           size_t input_undo_limit = 0);

enum class PickerEscapeResult { Navigated, Cancelled, CreateNew };

PickerEscapeResult handle_list_picker_escape(size_t item_count,
                                             size_t& selected,
                                             std::string& status,
                                             const std::string& selection_label);

PickerEscapeResult handle_thread_list_escape(std::vector<chat::ThreadSummary>& threads,
                                                size_t& selected,
                                                std::string& status,
                                                size_t& pending_delete,
                                                TuiMode& mode,
                                                bool allow_create_new = true);

PickerEscapeResult handle_attachment_list_escape(size_t item_count,
                                                 size_t& selected,
                                                 std::string& status,
                                                 size_t& pending_delete,
                                                 TuiMode& mode);

}  // namespace ainiux::tui
