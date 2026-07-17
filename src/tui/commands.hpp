#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include "chat/session.hpp"
#include "chat/sqlite_store.hpp"
#include "editor/editor.hpp"
#include "provider/provider.hpp"
#include "tui/events.hpp"
#include "tui/theme_registry.hpp"
#include "tui/tui.hpp"

namespace ainiux::tui {

struct TuiCommandHandlers {
    std::function<void()> quit;
    std::function<void()> start_history_edit;
    std::function<void()> start_thread_list;
    std::function<void(const std::string&)> start_new_chat_thread;
    std::function<void(bool)> open_provider_picker;
    std::function<bool(const std::string&)> apply_selected_provider;
    std::function<void()> start_store_save;
    std::function<void(ModelsRequestPurpose)> start_models;
    std::function<void(const std::string&)> persist_settings_change;
    std::function<void()> refresh_settings_panel_if_visible;
    std::function<void(const std::string&)> start_save;
    std::function<void(const std::string&)> start_load;
    std::function<void()> pop_last_message;
    std::function<void()> start_response_to_unanswered_user;
    std::function<void(const std::string&)> start_insert;
    std::function<void(const std::string&)> start_attach;
    std::function<void(const std::string&)> start_fetch;
    std::function<void(const std::string&)> start_search;
    std::function<void()> start_media_cleanup;
    std::function<void(bool)> set_thinking_trace_mode;
    std::function<void()> switch_to_editor;
};

struct TuiCommandContext {
    provider::RequestContext& context;
    chat::Session& session;
    editor::EditorState& input;
    std::string& status;
    std::string& help_text;
    std::string& settings_text;
    int& history_scroll;
    bool& show_thinking_traces;
    bool& syntax_highlight;
    const ThemeRegistry& themes;
    std::string& theme;
    bool use_colors = true;
    ActiveJob& active_job;
    TuiMode& mode;
    size_t input_undo_limit = 0;
    bool sqlite_available = false;
    chat::SqliteStore& sqlite_store;
    std::function<std::string()> sqlite_unavailable_message;
    std::vector<provider::ImageInput>& pending_images;
    size_t& inflight_image_count;
    std::vector<ChatAttachment>& chat_attachments;
    size_t& attachments_committed_for_turn;
};

void handle_tui_command(const std::string& text, TuiCommandContext& ctx, TuiCommandHandlers& handlers);

}  // namespace ainiux::tui
