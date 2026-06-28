#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "chat/session.hpp"
#include "chat/sqlite_store.hpp"
#include "editor/editor.hpp"
#include "tui/events.hpp"
#include "tui/tui.hpp"

namespace pkchat::tui {

std::string thread_picker_text(const std::vector<chat::ThreadSummary>& threads, size_t selected);
std::string remove_confirm_text(const chat::Session& session);
std::string join_models_preview(const std::vector<std::string>& models);

EscapeResult handle_escape(editor::EditorState& input, const Layout& layout, int& history_scroll, std::string& status);

bool handle_thread_picker_escape(std::vector<chat::ThreadSummary>& threads,
                                 size_t& selected,
                                 TuiMode& mode,
                                 std::string& status);

}  // namespace pkchat::tui