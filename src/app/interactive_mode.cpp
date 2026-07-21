#include "app/interactive_mode.hpp"

#include "app/app.hpp"
#include "chat/session.hpp"
#include "editor/ai_continue.hpp"
#include "editor/editor.hpp"
#include "tui/tui.hpp"

namespace ainiux::app {

void sync_editor_provider_to_shared(InteractiveSession& session,
                                    const std::optional<editor::AiContinueContext>& ai_continue) {
    if (ai_continue.has_value()) {
        session.context = ai_continue->request;
        session.ai_continue = ai_continue;
    }
}

void sync_shared_provider_to_editor(InteractiveSession& session) {
    if (!session.ai_continue.has_value()) {
        editor::AiContinueContext created;
        created.assist_config = session.assist_config;
        created.settings = editor::ai_continue_settings(session.context.options);
        created.request = session.context;
        session.ai_continue = std::move(created);
        return;
    }
    session.ai_continue->request = session.context;
}

void ensure_chat_session_initialized(InteractiveSession& session) {
    if (session.chat_session_initialized) {
        refresh_session_metadata(session.chat_session, session.context);
        return;
    }
    session.chat_session = chat::new_session(session.context);
    apply_system_prompt(session.chat_session, session.context.options.system);
    session.chat_session_initialized = true;
}

int run_interactive(InteractiveSession session) {
    InteractiveMode mode = session.start_mode;
    // Keep options.agent aligned with the active product mode.
    session.context.options.agent = (mode == InteractiveMode::Agent);
    session.context.options.tui = (mode == InteractiveMode::Chat);

    while (true) {
        if (mode == InteractiveMode::Editor) {
            const EditorRunResult result = editor::run_editor(session.editor_path,
                                                                      session.editor_save_as,
                                                                      session.editor_settings,
                                                                      session.ai_continue,
                                                                      session.assist_config,
                                                                      &session);
            if (result.next == InteractiveUiTarget::Agent) {
                sync_editor_provider_to_shared(session, session.ai_continue);
                ensure_chat_session_initialized(session);
                session.context.options.agent = true;
                session.context.options.tui = false;
                mode = InteractiveMode::Agent;
                continue;
            }
            if (result.next != InteractiveUiTarget::Chat) {
                return result.exit_code;
            }
            sync_editor_provider_to_shared(session, session.ai_continue);
            ensure_chat_session_initialized(session);
            session.context.options.agent = false;
            session.context.options.tui = true;
            mode = InteractiveMode::Chat;
            continue;
        }

        // Chat and Agent both use the shared full-screen TUI shell (history,
        // input editor, /provider /model /reasoning pickers). Generation differs
        // by session.context.options.agent (set only for InteractiveMode::Agent).
        session.context.options.agent = (mode == InteractiveMode::Agent);
        session.context.options.tui = (mode == InteractiveMode::Chat);
        const TuiRunResult result = tui::run(session.context, session.chat_session, &session);
        if (result.next == InteractiveUiTarget::Editor) {
            sync_shared_provider_to_editor(session);
            session.context.options.agent = false;
            session.context.options.tui = false;
            mode = InteractiveMode::Editor;
            continue;
        }
        if (result.next == InteractiveUiTarget::Agent) {
            session.context.options.agent = true;
            session.context.options.tui = false;
            mode = InteractiveMode::Agent;
            continue;
        }
        if (result.next == InteractiveUiTarget::Chat) {
            session.context.options.agent = false;
            session.context.options.tui = true;
            mode = InteractiveMode::Chat;
            continue;
        }
        return result.exit_code;
    }
}

}  // namespace ainiux::app