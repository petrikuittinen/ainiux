#pragma once

#include <optional>
#include <string>
#include <vector>

#include "chat/session.hpp"
#include "editor/ai_continue.hpp"
#include "editor/editor.hpp"
#include "editor/editor_prompts.hpp"
#include "provider/provider.hpp"

namespace ainiux::app {

// Distinct interactive product surfaces. Chat and Agent share the full-screen
// TUI shell and provider/model/reasoning selectors, but they are not the same
// mode: Chat is ordinary conversation; Agent runs the tool-using agent loop.
enum class InteractiveMode {
    Editor,
    Chat,
    Agent,
};

enum class InteractiveUiTarget {
    Quit,
    Editor,
    Chat,
    Agent,
};

struct PendingEditorAssistFromChat {
    bool active = false;
    size_t command_index = 0;
    std::string selection_text;
};

struct InteractiveSession {
    InteractiveMode start_mode = InteractiveMode::Editor;
    // Ctrl+G toggles Editor with the conversational mode that opened it.
    // A standalone editor defaults to Chat.
    InteractiveMode editor_return_mode = InteractiveMode::Chat;

    provider::RequestContext context;
    std::optional<editor::AiContinueContext> ai_continue;

    chat::Session chat_session;
    bool chat_session_initialized = false;
    // Chat and Agent are distinct OpenRouter conversations even though they
    // share provider settings and can cycle through the editor.
    std::string chat_routing_session_id;
    std::string agent_routing_session_id;

    std::vector<editor::EditorState> editor_buffers;
    size_t editor_active_buffer = 0;
    bool editor_buffers_initialized = false;

    std::string editor_path;
    std::string editor_save_as;
    editor::EditorSettings editor_settings;
    editor::EditorAssistConfig assist_config;
    PendingEditorAssistFromChat pending_editor_assist;
    bool highlight_enabled = true;
    std::string theme_name = "dark";
    bool use_colors = true;
    bool show_scrollbars = true;
};

struct EditorRunResult {
    int exit_code = 0;
    InteractiveUiTarget next = InteractiveUiTarget::Quit;
};

struct TuiRunResult {
    int exit_code = 0;
    InteractiveUiTarget next = InteractiveUiTarget::Quit;
};

void sync_editor_provider_to_shared(InteractiveSession& session,
                                  const std::optional<editor::AiContinueContext>& ai_continue);
void sync_shared_provider_to_editor(InteractiveSession& session);
void ensure_chat_session_initialized(InteractiveSession& session);
InteractiveUiTarget editor_toggle_target(const InteractiveSession& session);

int run_interactive(InteractiveSession session);

}  // namespace ainiux::app
