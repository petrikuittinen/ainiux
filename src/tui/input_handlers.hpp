#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "chat/session.hpp"
#include "chat/sqlite_store.hpp"
#include "editor/editor.hpp"
#include "tui/events.hpp"
#include "tui/tui.hpp"

namespace ainiux::tui {

std::vector<std::string> selectable_provider_ids();
std::string provider_picker_text(const std::vector<std::string>& provider_ids, size_t selected);
std::string model_picker_text(const std::vector<std::string>& models, size_t selected);
std::string thread_picker_text(const std::vector<chat::ThreadSummary>& threads, size_t selected);
std::string attachment_picker_text(const std::vector<ChatAttachment>& attachments, size_t selected);
std::string remove_confirm_text(const chat::Session& session);
std::string system_edit_text();
std::string history_edit_text();
std::string join_models_preview(const std::vector<std::string>& models);

void scroll_chat_history_page_up(const Layout& layout, int& history_scroll);
void scroll_chat_history_page_down(const Layout& layout, int& history_scroll);

bool apply_chat_history_scroll(const editor::MovementKeyEvent& movement,
                               const Layout& layout,
                               int& history_scroll);

EscapeResult handle_escape(editor::EditorState& input,
                           const Layout& layout,
                           int& history_scroll,
                           std::string& status,
                           bool input_only_movement = false);

enum class PickerEscapeResult { Navigated, Cancelled };

PickerEscapeResult handle_list_picker_escape(size_t item_count,
                                             size_t& selected,
                                             std::string& status,
                                             const std::string& selection_label);

PickerEscapeResult handle_thread_list_escape(std::vector<chat::ThreadSummary>& threads,
                                                size_t& selected,
                                                std::string& status,
                                                size_t& pending_delete,
                                                TuiMode& mode);

PickerEscapeResult handle_attachment_list_escape(size_t item_count,
                                                 size_t& selected,
                                                 std::string& status,
                                                 size_t& pending_delete,
                                                 TuiMode& mode);

}  // namespace ainiux::tui