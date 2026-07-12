#pragma once

#include <optional>
#include <string>
#include <vector>

#include "chat/session.hpp"
#include "editor/ai_continue.hpp"
#include "editor/editor.hpp"
#include "editor/editor_prompts.hpp"
#include "provider/provider.hpp"

namespace pkchat::app {

enum class InteractiveMode {
    Editor,
    Chat,
};

enum class InteractiveUiTarget {
    Quit,
    Editor,
    Chat,
};

struct PendingEditorAssistFromChat {
    bool active = false;
    size_t command_index = 0;
    std::string selection_text;
};

struct InteractiveSession {
    InteractiveMode start_mode = InteractiveMode::Editor;

    provider::RequestContext context;
    std::optional<editor::AiContinueContext> ai_continue;

    chat::Session chat_session;
    bool chat_session_initialized = false;

    std::vector<editor::EditorState> editor_buffers;
    size_t editor_active_buffer = 0;
    bool editor_buffers_initialized = false;

    std::string editor_path;
    std::string editor_save_as;
    editor::EditorSettings editor_settings;
    editor::EditorAssistConfig assist_config;
    PendingEditorAssistFromChat pending_editor_assist;
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

int run_interactive(InteractiveSession session);

}  // namespace pkchat::app