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

void rebind_editor_theme_settings(InteractiveSession& session) {
    // EditorSettings.themes is a non-owning pointer into Options::tui_themes.
    // Options always carries at least default_theme_registry() (and normally
    // themes.conf). Rebind after every context handoff so the pointer cannot
    // stay null or point at a destroyed temporary.
    session.editor_settings.themes = &session.context.options.tui_themes;
    if (!session.theme_name.empty()) {
        session.editor_settings.theme_name = session.theme_name;
    } else if (session.editor_settings.theme_name.empty()) {
        session.editor_settings.theme_name = session.context.options.tui_theme;
    }
    session.editor_settings.use_colors = session.use_colors;
    session.editor_settings.highlight_enabled = session.highlight_enabled;
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

InteractiveUiTarget editor_toggle_target(const InteractiveSession& session) {
    return session.editor_return_mode == InteractiveMode::Agent
               ? InteractiveUiTarget::Agent
               : InteractiveUiTarget::Chat;
}

int run_interactive(InteractiveSession session) {
    InteractiveMode mode = session.start_mode;
    // Keep options.agent aligned with the active product mode.
    session.context.options.agent = (mode == InteractiveMode::Agent);
    session.context.options.tui = (mode == InteractiveMode::Chat);

    while (true) {
        if (mode == InteractiveMode::Editor) {
            rebind_editor_theme_settings(session);
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
                // Process quit from editor: stop any background agent first.
                if (session.agent_controller) {
                    session.agent_controller->shutdown(true, "quit from editor");
                    session.agent_controller.reset();
                }
                return result.exit_code;
            }
            sync_editor_provider_to_shared(session, session.ai_continue);
            ensure_chat_session_initialized(session);
            // Chat is a separate product surface: tear down the agent controller.
            if (session.agent_controller) {
                session.agent_controller->shutdown(true, "left agent for chat");
                session.agent_controller.reset();
            }
            session.context.options.agent = false;
            session.context.options.tui = true;
            mode = InteractiveMode::Chat;
            continue;
        }

        // Chat and Agent share the TUI shell (input, pickers) but must NOT share
        // transcript state. Agent history lives in .ainiux-pr/agent.sqlite and is
        // loaded into a disposable view session; chat keeps chat_session only.
        // Mixing them made Chat show tool lines and triggered HTTP 400 when the
        // provider received unsupported "tool" roles on ordinary completions.
        session.context.options.agent = (mode == InteractiveMode::Agent);
        session.context.options.tui = (mode == InteractiveMode::Chat);

        TuiRunResult result;
        if (mode == InteractiveMode::Agent) {
            ensure_chat_session_initialized(session);
            agent::ensure_agent_controller(session.agent_controller);
            chat::Session agent_view = chat::new_session(session.context);
            // No chat system prompt / thread id — agent has its own prompts + DB.
            agent_view.messages.clear();
            result = tui::run(session.context, std::move(agent_view), &session);
        } else {
            ensure_chat_session_initialized(session);
            result = tui::run(session.context, session.chat_session, &session);
        }

        if (result.next == InteractiveUiTarget::Editor) {
            session.editor_return_mode = mode;
            sync_shared_provider_to_editor(session);
            rebind_editor_theme_settings(session);
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
            if (session.agent_controller) {
                session.agent_controller->shutdown(true, "left agent for chat");
                session.agent_controller.reset();
            }
            session.context.options.agent = false;
            session.context.options.tui = true;
            mode = InteractiveMode::Chat;
            continue;
        }
        // Quit from agent/chat TUI.
        if (session.agent_controller) {
            session.agent_controller->shutdown(true, "quit interactive session");
            session.agent_controller.reset();
        }
        return result.exit_code;
    }
}

}  // namespace ainiux::app
