#include "tui/commands.hpp"

#include "app/app.hpp"
#include "app/detail.hpp"
#include "chat/generation_settings.hpp"
#include "chat/settings.hpp"
#include "cli/args.hpp"
#include "ainiux/model_setting.hpp"
#include "tui/detail/render.hpp"
#include "tui/theme_registry.hpp"

namespace ainiux::tui {

namespace {

bool allowed_for_read_only_thread(const std::string& text) {
    return text == "/help" || text == "/quit" || text == "/exit" ||
           text == "/list" || text == "/models" || text == "/remove" ||
           text == "/remove-empty" || text == "/cleanup" ||
           text == "/new" || text.rfind("/new ", 0) == 0 ||
           text == "/index-code" || text == "/show-index" ||
           text == "/save" || text.rfind("/save ", 0) == 0 ||
           text.rfind("/load ", 0) == 0 || text == "/theme" ||
           text.rfind("/theme ", 0) == 0 || text == "/highlight" ||
           text.rfind("/highlight ", 0) == 0 ||
           text == "/shell" || text.rfind("/shell ", 0) == 0 ||
           text == "/shell-stdout" || text.rfind("/shell-stdout ", 0) == 0;
}

bool reasoning_change_needs_confirmation(const std::string& requested,
                                         TuiCommandContext& ctx,
                                         TuiCommandHandlers& handlers) {
    ReasoningSelection selection;
    Error err = config::parse_reasoning_selection(requested, selection);
    if (!err.ok()) {
        ctx.status = err.message;
        return true;
    }
    err = config::resolve_reasoning_off(
        ctx.context.options.model_catalog,
        ctx.context.profile.name,
        ctx.context.api_kind == provider::ApiKind::Responses ? "responses" : "chat",
        ctx.context.options.model,
        selection);
    if (!err.ok()) {
        ctx.status = err.message;
        return true;
    }
    const std::string warning = config::reasoning_catalog_warning(
        ctx.context.options.model_catalog,
        ctx.context.profile.name,
        ctx.context.api_kind == provider::ApiKind::Responses ? "responses" : "chat",
        ctx.context.options.model,
        selection);
    if (warning.empty()) {
        return false;
    }
    handlers.request_reasoning_confirmation(requested, warning);
    return true;
}

}  // namespace

AgentSlashCommand parse_agent_slash_command(const std::string& text) {
    AgentSlashCommand command;
    if (text == "/new" || text.rfind("/new ", 0) == 0) {
        command.action = AgentSlashAction::NewProject;
        command.argument =
            text.size() <= 4 ? std::string() : app::detail::trim_ascii(text.substr(4));
        return command;
    }
    if (text == "/compact" || text.rfind("/compact ", 0) == 0) {
        command.action = AgentSlashAction::Compact;
        command.argument =
            text.size() <= 8 ? std::string()
                             : app::detail::trim_ascii(text.substr(8));
        if (!command.argument.empty()) {
            CompactionStrategy ignored;
            if (!agent::parse_compaction_strategy(command.argument, ignored)) {
                command.action = AgentSlashAction::Invalid;
                command.error = "Usage: /compact [fast|smart|summary]";
            }
        }
        return command;
    }
    if (text == "/show-index") {
        command.action = AgentSlashAction::ShowIndex;
        return command;
    }
    if (text.rfind("/show-index ", 0) == 0) {
        command.action = AgentSlashAction::Invalid;
        command.error = "Usage: /show-index";
        return command;
    }
    if (text == "/index-code") {
        command.action = AgentSlashAction::IndexCode;
        return command;
    }
    if (text.rfind("/index-code ", 0) == 0) {
        command.action = AgentSlashAction::Invalid;
        command.error = "Usage: /index-code";
        return command;
    }
    if (text == "/plan") {
        command.action = AgentSlashAction::Plan;
        return command;
    }
    if (text == "/act") {
        command.action = AgentSlashAction::Act;
        return command;
    }
    if (text == "/permissions" || text.rfind("/permissions ", 0) == 0) {
        command.action = AgentSlashAction::Permissions;
        command.argument =
            text.size() <= 12 ? std::string()
                              : app::detail::trim_ascii(text.substr(12));
        if (!command.argument.empty()) {
            agent::PermissionMode ignored;
            if (!agent::parse_permission_mode(command.argument, ignored)) {
                command.action = AgentSlashAction::Invalid;
                command.error = "Usage: /permissions [confirm|smart|yolo]";
            }
        }
        return command;
    }
    if (text == "/goal" || text.rfind("/goal ", 0) == 0) {
        const std::string argument =
            text.size() <= 5 ? std::string()
                             : app::detail::trim_ascii(text.substr(5));
        if (argument.empty()) {
            command.action = AgentSlashAction::GoalStatus;
            return command;
        }
        // First token is a subcommand only for exact clear/stop/off/cancel/pause/resume.
        std::string token = argument;
        std::string rest;
        const std::size_t space = argument.find(' ');
        if (space != std::string::npos) {
            token = argument.substr(0, space);
            rest = app::detail::trim_ascii(argument.substr(space + 1));
        }
        std::string lower = token;
        for (char& ch : lower) {
            if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
        }
        if (lower == "clear" || lower == "stop" || lower == "off" ||
            lower == "cancel") {
            if (!rest.empty()) {
                command.action = AgentSlashAction::Invalid;
                command.error = "Usage: /goal clear";
                return command;
            }
            command.action = AgentSlashAction::GoalClear;
            return command;
        }
        if (lower == "pause") {
            if (!rest.empty()) {
                command.action = AgentSlashAction::Invalid;
                command.error = "Usage: /goal pause";
                return command;
            }
            command.action = AgentSlashAction::GoalPause;
            return command;
        }
        if (lower == "resume") {
            if (!rest.empty()) {
                command.action = AgentSlashAction::Invalid;
                command.error = "Usage: /goal resume";
                return command;
            }
            command.action = AgentSlashAction::GoalResume;
            return command;
        }
        command.action = AgentSlashAction::GoalSet;
        command.argument = argument;
        return command;
    }
    return command;
}

void handle_tui_command(const std::string& text, TuiCommandContext& ctx, TuiCommandHandlers& handlers) {
    if (text == "/quit" || text == "/exit") {
        handlers.quit();
        return;
    }
    if (text == "/help") {
        if (ctx.help_text.empty()) {
            ctx.settings_text.clear();
            ctx.help_text =
                "/help or Ctrl+H (hide/show this panel)\n"
                "/quit or /exit\n"
                "/clear\n"
                "/edit\n"
                "/list (Ctrl+L; also shown on chat startup; Tab/Insert new thread)\n"
                + std::string(ctx.context.options.agent
                                  ? "/new [PATH] (fresh agent project)\n"
                                    "/compact [fast|smart|summary] (preserve transcript)\n"
                                    "/index-code (create/enable code index)\n"
                                    "/show-index (refresh compact index report)\n"
                                    "/plan (planning task mode)\n"
                                    "/act (full coding task mode)\n"
                                    "/goal [condition|clear|pause|resume]\n"
                                    "/permissions [confirm|smart|yolo]\n"
                                    "/setting thinking_preview_max_chars=N (0 disables)\n"
                                  : "/new [NAME]\n") +
                "/provider [PROVIDER]\n"
                "/models\n"
                "/model [MODEL]\n"
                "/context [auto|TOKENS]\n"
                "/reasoning [auto|VALUE|TOKENS]\n"
                "/system [TEXT]\n"
                "/setting (hide/show current settings)\n"
                "/setting NAME=VALUE\n"
                "/setting general|coding|instruct|creative\n"
                + std::string(ctx.context.options.agent
                                  ? ""
                                  : "/clone\n"
                                    "/save [PATH]\n"
                                    "/load PATH\n"
                                    "/remove\n"
                                    "/remove-empty\n"
                                    "/cleanup (expire inactive managed media)\n"
                                    "/pop\n"
                                    "/response\n") +
                "/insert FILE_OR_URL (UTF-8 text at cursor)\n"
                "/attach [PATH|URL] (queue text or image for next prompt; bare shows list, DEL deletes;\n"
                "  agent: images request-local for that turn only, not stored in project/media)\n"
                "/fetch URL\n"
                "/search QUERY\n"
                "/shell COMMAND  or  !COMMAND (user shell; display-only notice)\n"
                "/shell-stdout COMMAND  or  !!COMMAND (stdout → editable input draft)\n"
                "/theme [THEME]\n"
                "/highlight [on|off]\n"
                "/thinking [show|hide]\n"
                "/editor (switch to editor mode)\n"
                "/chat (switch to ordinary chat mode)\n"
                "/agent (switch to interactive agent mode)\n"
                "/mode [chat|editor|agent] (show or jump modes)\n"
                "/cycle or Ctrl+G (toggle current chat/agent mode ↔ editor)\n"
                "/cmd-out [on|off] (agent: show run_command stdout)\n"
                + std::string(ctx.context.options.agent
                                  ? "Agent commands are isolated from Chat/editor AI commands"
                                  : "AI commands from editor-commands.conf (/spell, /grammar, "
                                    "/continue,\n/Chinese, /German, /Japanese, /prompt, "
                                    "/regenerate, and custom commands)");
            ctx.status = "Help shown; Ctrl+H or /help hides it";
        } else {
            ctx.help_text.clear();
            ctx.status = "Help hidden";
        }
        ctx.history_scroll = 0;
        return;
    }
    if (ctx.session.read_only && !allowed_for_read_only_thread(text)) {
        ctx.status = "Thread is read-only: " +
                     (ctx.session.read_only_reason.empty()
                          ? std::string("managed attachment media is unavailable")
                          : ctx.session.read_only_reason);
        return;
    }
    if (text == "/reasoning" || text.rfind("/reasoning ", 0) == 0) {
        const std::string requested = app::detail::trim_ascii(text.substr(10));
        if (requested.empty()) {
            handlers.open_reasoning_picker();
            return;
        }
        if (reasoning_change_needs_confirmation(requested, ctx, handlers)) {
            return;
        }
        Error err = chat::apply_chat_setting(ctx.context.options, "reasoning", requested);
        if (!err.ok()) {
            ctx.status = err.message;
            return;
        }
        std::string message = "Reasoning set to " +
            config::reasoning_selection_value(ctx.context.options.reasoning);
        const std::string advisory = provider::reasoning_temperature_advisory(ctx.context);
        if (!advisory.empty()) message += ". Warning: " + advisory;
        handlers.persist_settings_change(message);
        handlers.refresh_settings_panel_if_visible();
        return;
    }
    if (text.rfind("/thinking", 0) == 0) {
        const std::string requested = app::detail::trim_ascii(text.substr(9));
        if (requested.empty()) {
            ctx.status = std::string("Thinking traces: ") + (ctx.show_thinking_traces ? "show" : "hide") +
                           ". Use /thinking show or /thinking hide";
            return;
        }
        if (requested == "show") {
            handlers.set_thinking_trace_mode(true);
            return;
        }
        if (requested == "hide") {
            handlers.set_thinking_trace_mode(false);
            return;
        }
        ctx.status = "Usage: /thinking show|hide";
        return;
    }
    // Editor-only text layout commands (not chat/agent history).
    if (text == "/width" || text.rfind("/width ", 0) == 0 || text == "/alignment-width" ||
        text.rfind("/alignment-width ", 0) == 0 || text == "/left-align" ||
        text.rfind("/left-align ", 0) == 0 || text == "/right-align" ||
        text.rfind("/right-align ", 0) == 0 || text == "/center-align" ||
        text.rfind("/center-align ", 0) == 0 || text == "/justify" ||
        text.rfind("/justify ", 0) == 0 || text == "/justify-align" ||
        text.rfind("/justify-align ", 0) == 0) {
        ctx.status =
            "Text alignment and /width are editor-only; open /editor to use them";
        return;
    }
    if (text == "/highlight" || text.rfind("/highlight ", 0) == 0) {
        const std::string requested = app::detail::trim_ascii(text.substr(10));
        if (requested.empty()) {
            ctx.status = std::string("Syntax highlighting: ") +
                         (ctx.syntax_highlight ? "on" : "off");
            return;
        }
        if (requested == "on") {
            ctx.syntax_highlight = true;
            ctx.context.options.tui_highlight = true;
            ctx.input.highlight_enabled = true;
            ctx.status = "Syntax highlighting enabled";
            return;
        }
        if (requested == "off") {
            ctx.syntax_highlight = false;
            ctx.context.options.tui_highlight = false;
            ctx.input.highlight_enabled = false;
            ctx.status = "Syntax highlighting disabled";
            return;
        }
        ctx.status = "Usage: /highlight on|off";
        return;
    }
    if (text.rfind("/theme", 0) == 0) {
        const std::string requested = app::detail::trim_ascii(text.substr(6));
        const ThemeCommandResult theme_result =
            handle_theme_command(ctx.themes, ctx.theme, requested, ctx.use_colors);
        ctx.status = theme_result.message;
        if (theme_result.ok && !theme_result.selected_theme.empty()) {
            ctx.theme = theme_result.selected_theme;
        }
        if (theme_result.ok) ctx.use_colors = theme_result.colors_enabled;
        return;
    }
    if (text == "/editor") {
        if (ctx.active_job != ActiveJob::None) {
            ctx.status = "Cannot switch to editor while a model job is running";
            return;
        }
        handlers.switch_to_editor();
        return;
    }
    if (text == "/chat") {
        if (ctx.active_job != ActiveJob::None) {
            ctx.status = "Cannot switch mode while a model job is running";
            return;
        }
        if (handlers.switch_to_chat) {
            handlers.switch_to_chat();
        } else {
            ctx.status = "Chat mode switch is unavailable";
        }
        return;
    }
    if (text == "/agent") {
        if (ctx.active_job != ActiveJob::None) {
            ctx.status = "Cannot switch mode while a model job is running";
            return;
        }
        if (handlers.switch_to_agent) {
            handlers.switch_to_agent();
        } else {
            ctx.status = "Agent mode switch is unavailable";
        }
        return;
    }
    if (text == "/cycle") {
        if (ctx.active_job != ActiveJob::None) {
            ctx.status = "Cannot switch mode while a model job is running";
            return;
        }
        if (handlers.cycle_mode) {
            handlers.cycle_mode();
        } else {
            ctx.status = "Mode cycle is unavailable";
        }
        return;
    }
    if (text == "/mode" || text.rfind("/mode ", 0) == 0) {
        if (ctx.active_job != ActiveJob::None) {
            ctx.status = "Cannot switch mode while a model job is running";
            return;
        }
        const std::string arg = app::detail::trim_ascii(text.size() > 5 ? text.substr(5) : "");
        if (arg.empty()) {
            if (ctx.context.options.agent) {
                ctx.status = "Mode: agent · /mode chat|editor|agent · /cycle";
            } else {
                ctx.status = "Mode: chat · /mode chat|editor|agent · /cycle";
            }
            return;
        }
        if (arg == "chat") {
            if (handlers.switch_to_chat) handlers.switch_to_chat();
            else ctx.status = "Chat mode switch is unavailable";
            return;
        }
        if (arg == "editor") {
            handlers.switch_to_editor();
            return;
        }
        if (arg == "agent") {
            if (handlers.switch_to_agent) handlers.switch_to_agent();
            else ctx.status = "Agent mode switch is unavailable";
            return;
        }
        ctx.status = "Usage: /mode [chat|editor|agent]";
        return;
    }
    if (text == "/cmd-out" || text.rfind("/cmd-out ", 0) == 0) {
        if (!ctx.context.options.agent) {
            ctx.status = "/cmd-out is for agent mode only";
            return;
        }
        const std::string arg = app::detail::trim_ascii(text.size() > 8 ? text.substr(8) : "");
        if (arg.empty()) {
            ctx.status = ctx.context.options.agent_show_command_output ? "cmd-out on" : "cmd-out off";
            return;
        }
        if (arg == "on") {
            ctx.context.options.agent_show_command_output = true;
            ctx.status = "cmd-out on";
            return;
        }
        if (arg == "off") {
            ctx.context.options.agent_show_command_output = false;
            ctx.status = "cmd-out off";
            return;
        }
        ctx.status = "Usage: /cmd-out [on|off]";
        return;
    }
    if (text == "/edit") {
        handlers.start_history_edit();
        return;
    }
    if (text == "/clear") {
        ctx.session.messages.clear();
        ctx.pending_images.clear();
        ctx.inflight_image_count = 0;
        ctx.chat_attachments.clear();
        ctx.attachments_committed_for_turn = 0;
        app::apply_system_prompt(ctx.session, ctx.context.options.system);
        ctx.history_scroll = 0;
        ctx.status = "Chat history cleared";
        return;
    }
    if (text == "/list") {
        handlers.start_thread_list();
        return;
    }
    const AgentSlashCommand agent_command =
        ctx.context.options.agent ? parse_agent_slash_command(text) : AgentSlashCommand{};
    if (agent_command.action == AgentSlashAction::NewProject) {
        if (ctx.active_job != ActiveJob::None) {
            ctx.status = "Cannot create a project while an agent job is running; wait or cancel it first";
            return;
        }
        handlers.start_new_agent_project(agent_command.argument);
        return;
    }
    if (agent_command.action == AgentSlashAction::Invalid) {
        ctx.status = agent_command.error;
        return;
    }
    if (agent_command.action == AgentSlashAction::Compact) {
        if (ctx.active_job != ActiveJob::None) {
            ctx.status = "Cannot compact while an agent job is running; wait or cancel it first";
            return;
        }
        std::optional<CompactionStrategy> strategy;
        if (!agent_command.argument.empty()) {
            CompactionStrategy parsed;
            if (agent::parse_compaction_strategy(agent_command.argument, parsed))
                strategy = parsed;
        }
        handlers.start_agent_compaction(strategy);
        return;
    }
    if (agent_command.action == AgentSlashAction::ShowIndex) {
        if (ctx.active_job != ActiveJob::None) {
            ctx.status =
                "Cannot show the index while an agent job is running; wait or cancel it first";
            return;
        }
        handlers.start_agent_show_index();
        return;
    }
    if (agent_command.action == AgentSlashAction::IndexCode) {
        if (ctx.active_job != ActiveJob::None) {
            ctx.status =
                "Cannot index code while an agent job is running; wait or cancel it first";
            return;
        }
        handlers.start_agent_index_code();
        return;
    }
    if (agent_command.action == AgentSlashAction::Plan ||
        agent_command.action == AgentSlashAction::Act) {
        if (ctx.active_job != ActiveJob::None) {
            ctx.status =
                "Cannot switch task mode while an agent job is running; wait or cancel it first";
            return;
        }
        handlers.switch_agent_task_mode(
            agent_command.action == AgentSlashAction::Plan
                ? agent::AgentTaskMode::Plan
                : agent::AgentTaskMode::Act);
        return;
    }
    if (agent_command.action == AgentSlashAction::Permissions) {
        if (ctx.active_job != ActiveJob::None ||
            ctx.mode == TuiMode::GuardApprovalConfirm) {
            ctx.status =
                "Cannot switch permissions while an agent operation or approval is active";
            return;
        }
        if (agent_command.argument.empty()) {
            handlers.open_agent_permission_picker();
            return;
        }
        handlers.switch_agent_permission_mode(agent_command.argument);
        return;
    }
    if (agent_command.action == AgentSlashAction::GoalStatus) {
        if (handlers.show_agent_goal_status) handlers.show_agent_goal_status();
        return;
    }
    if (agent_command.action == AgentSlashAction::GoalSet) {
        if (ctx.active_job != ActiveJob::None) {
            ctx.status =
                "Cannot set a goal while an agent job is running; wait or cancel it first";
            return;
        }
        if (handlers.set_agent_goal) handlers.set_agent_goal(agent_command.argument);
        return;
    }
    if (agent_command.action == AgentSlashAction::GoalClear) {
        if (ctx.active_job != ActiveJob::None) {
            ctx.status =
                "Cannot clear the goal while an agent job is running; wait or cancel it first";
            return;
        }
        if (handlers.clear_agent_goal) handlers.clear_agent_goal();
        return;
    }
    if (agent_command.action == AgentSlashAction::GoalPause) {
        if (ctx.active_job != ActiveJob::None) {
            ctx.status =
                "Cannot pause the goal while an agent job is running; wait or cancel it first";
            return;
        }
        if (handlers.pause_agent_goal) handlers.pause_agent_goal();
        return;
    }
    if (agent_command.action == AgentSlashAction::GoalResume) {
        if (ctx.active_job != ActiveJob::None) {
            ctx.status =
                "Cannot resume the goal while an agent job is running; wait or cancel it first";
            return;
        }
        if (handlers.resume_agent_goal) handlers.resume_agent_goal();
        return;
    }
    if (text == "/compact" || text.rfind("/compact ", 0) == 0) {
        ctx.status = "/compact is for interactive agent mode only";
        return;
    }
    if (text == "/show-index" || text.rfind("/show-index ", 0) == 0) {
        ctx.status = "/show-index is for interactive agent mode only";
        return;
    }
    if (text == "/index-code" || text.rfind("/index-code ", 0) == 0) {
        ctx.status = "/index-code is for interactive agent mode only";
        return;
    }
    if (text == "/new" || text.rfind("/new ", 0) == 0) {
        handlers.start_new_chat_thread(text.size() <= 4 ? "" : text.substr(4));
        return;
    }
    if (text == "/provider" || text.rfind("/provider ", 0) == 0) {
        const std::string provider_name = app::detail::trim_ascii(text.substr(9));
        if (provider_name.empty()) {
            handlers.open_provider_picker(false);
            return;
        }
        if (!handlers.apply_selected_provider(provider_name)) {
            return;
        }
        handlers.start_store_save();
        handlers.start_models(ModelsRequestPurpose::Picker);
        return;
    }
    if (text == "/model" || text.rfind("/model ", 0) == 0) {
        const std::string model = app::detail::trim_ascii(text.substr(6));
        if (model.empty()) {
            if (ctx.context.profile.offline) {
                ctx.status = "Select a provider with /provider first";
                return;
            }
            handlers.start_models(ModelsRequestPurpose::Picker);
            return;
        }
        const bool changed = ctx.context.options.model != model;
        ctx.context.options.model = model;
        ctx.session.model = model;
        if (changed) {
            if (!ctx.context.options.has_context_tokens) {
                ctx.context.options.context_tokens = 0;
            }
            ctx.context.options.reasoning = ReasoningSelection::automatic();
            ctx.context.options.reasoning_explicit = true;
        }
        ctx.status = provider_model_status_message(ctx.context, "ready");
        handlers.start_store_save();
        if (changed && !ctx.context.options.has_context_tokens) {
            if (!ctx.context.profile.offline && !ctx.context.options.model.empty()) {
                handlers.refresh_model_context();
            }
        }
        return;
    }
    if (text == "/context" || text.rfind("/context ", 0) == 0) {
        const std::string requested = app::detail::trim_ascii(text.substr(8));
        if (requested.empty()) {
            if (ctx.context.options.context_tokens > 0) {
                ctx.status =
                    "Context window: " +
                    std::to_string(ctx.context.options.context_tokens) + " tokens (" +
                    (ctx.context.options.has_context_tokens ? "override"
                                                           : "catalog/model picker") +
                    ")";
            } else {
                ctx.status =
                    "Context window: unknown; usage will show tokens without a percentage";
            }
            return;
        }
        if (ascii_lower(requested) == "auto") {
            ctx.context.options.has_context_tokens = false;
            ctx.context.options.context_tokens = 0;
            handlers.persist_settings_change("Context window set to automatic");
            if (!ctx.context.profile.offline && !ctx.context.options.model.empty()) {
                handlers.refresh_model_context();
            }
            handlers.refresh_settings_panel_if_visible();
            return;
        }
        long long tokens = 0;
        const Error parse_error = cli::parse_context_tokens(requested, tokens);
        if (!parse_error.ok()) {
            ctx.status = "Usage: /context [auto|TOKENS] (examples: 64k, 131072, 1M)";
            return;
        }
        ctx.context.options.context_tokens = tokens;
        ctx.context.options.has_context_tokens = true;
        handlers.persist_settings_change(
            "Context window set to " + std::to_string(tokens) + " tokens");
        handlers.refresh_settings_panel_if_visible();
        return;
    }
    if (text == "/system" || text.rfind("/system ", 0) == 0) {
        const std::string system_text = text.size() <= 7 ? "" : app::detail::trim_ascii(text.substr(7));
        if (system_text.empty()) {
            if (ctx.active_job != ActiveJob::None) {
                ctx.status = "Cannot edit system prompt while a model job is running";
                return;
            }
            ctx.input = editor::EditorState::from_text(chat::current_system_prompt(ctx.session));
            ctx.input.set_undo_limit(ctx.input_undo_limit);
            ctx.mode = TuiMode::SystemEdit;
            ctx.status = "Editing system prompt";
            return;
        }
        app::replace_system_prompt(ctx.session, system_text);
        ctx.context.options.system = system_text;
        handlers.persist_settings_change("System prompt updated");
        return;
    }
    if (text == "/clone") {
        if (ctx.active_job != ActiveJob::None) {
            ctx.status = "Cannot clone a thread while a model job is running";
            return;
        }
        chat::Session cloned = ctx.session;
        cloned.thread_id = 0;
        cloned.created_at = chat::current_timestamp_utc();
        cloned.updated_at = cloned.created_at;
        if (cloned.name.empty()) {
            cloned.name = "Copy";
        } else {
            cloned.name += " (copy)";
        }
        ctx.session = std::move(cloned);
        ctx.history_scroll = 0;
        handlers.persist_settings_change("Cloned chat thread");
        return;
    }
    if (text == "/setting" || text.rfind("/setting ", 0) == 0) {
        const std::string requested = text.size() <= 8 ? "" : app::detail::trim_ascii(text.substr(8));
        if (requested.empty()) {
            if (ctx.settings_text.empty()) {
                ctx.help_text.clear();
                ctx.settings_text = chat::format_settings_panel(
                    ctx.context.options,
                    provider::reasoning_temperature_advisory(ctx.context));
                ctx.status = "Settings shown; /setting hides them";
            } else {
                ctx.settings_text.clear();
                ctx.status = "Settings hidden";
            }
            ctx.history_scroll = 0;
            return;
        }
        if (chat::generation::is_chat_purpose(requested)) {
            if (ctx.context.options.model.empty()) {
                ctx.status = "Set a model with /model before applying a purpose preset";
                return;
            }
            const ModelCapability* capability = provider::matched_model_capability(ctx.context);
            const ModelSetting* preset = capability == nullptr
                ? nullptr
                : config::find_model_preset(ctx.context.options.model_catalog,
                                            *capability,
                                            requested);
            if (preset == nullptr) {
                ctx.status = "No [preset] in models.conf for model " + ctx.context.options.model +
                             " purpose " + requested;
                return;
            }
            Error preset_error = chat::apply_model_setting_preset(ctx.context.options, *preset, capability);
            if (!preset_error.ok()) {
                ctx.status = detail::error_line(preset_error);
                return;
            }
            if (preset->default_system_prompt.has_value() && !preset->default_system_prompt->empty()) {
                app::replace_system_prompt(ctx.session, *preset->default_system_prompt);
                ctx.context.options.system = *preset->default_system_prompt;
            }
            std::string message = "Applied " + requested + " settings for " +
                                  ctx.context.options.model;
            const std::string advisory =
                provider::reasoning_temperature_advisory(ctx.context);
            if (!advisory.empty()) message += ". Warning: " + advisory;
            handlers.persist_settings_change(message);
            handlers.refresh_settings_panel_if_visible();
            return;
        }
        const size_t equals = requested.find('=');
        if (equals == std::string::npos) {
            ctx.status = "Usage: /setting NAME=VALUE or /setting " + chat::generation::chat_purpose_description();
            return;
        }
        const std::string name = app::detail::trim_ascii(requested.substr(0, equals));
        const std::string value = app::detail::trim_ascii(requested.substr(equals + 1));
        if (name.empty()) {
            ctx.status = "Usage: /setting NAME=VALUE";
            return;
        }
        if (ascii_lower(name) == "thinking_preview_max_chars" &&
            !ctx.context.options.agent) {
            ctx.status =
                "thinking_preview_max_chars is available only in interactive agent mode";
            return;
        }
        if (ascii_lower(name) == "cmd-out" && !ctx.context.options.agent) {
            ctx.status = "cmd-out is available only in interactive agent mode";
            return;
        }
        if (ascii_lower(name) == "reasoning" &&
            reasoning_change_needs_confirmation(value, ctx, handlers)) {
            return;
        }
        Error setting_error = chat::apply_chat_setting(ctx.context.options, name, value);
        if (!setting_error.ok()) {
            ctx.status = setting_error.message;
            return;
        }
        ctx.show_thinking_traces = ctx.context.options.show_thinking_traces;
        ctx.syntax_highlight = ctx.context.options.tui_highlight;
        ctx.input.highlight_enabled = ctx.syntax_highlight;
        std::string message = "Updated " + name;
        const std::string advisory =
            provider::reasoning_temperature_advisory(ctx.context);
        if (!advisory.empty()) message += ". Warning: " + advisory;
        handlers.persist_settings_change(message);
        handlers.refresh_settings_panel_if_visible();
        return;
    }
    if (text == "/models") {
        handlers.start_models(ModelsRequestPurpose::Preview);
        return;
    }
    if (text.rfind("/save", 0) == 0) {
        std::string path = app::detail::trim_ascii(text.substr(5));
        if (path.empty()) {
            path = ctx.context.options.save_chat_path;
        }
        if (path.empty()) {
            ctx.status = "Usage: /save PATH";
            return;
        }
        handlers.start_save(path);
        ctx.status = "Saving " + path;
        return;
    }
    if (text.rfind("/load", 0) == 0) {
        const std::string path = app::detail::trim_ascii(text.substr(5));
        if (path.empty()) {
            ctx.status = "Usage: /load PATH";
            return;
        }
        handlers.start_load(path);
        return;
    }
    if (text == "/remove") {
        if (!ctx.sqlite_available) {
            ctx.status = ctx.sqlite_unavailable_message();
            return;
        }
        if (ctx.session.thread_id <= 0) {
            ctx.status = "No saved thread to remove";
            return;
        }
        ctx.mode = TuiMode::RemoveConfirm;
        ctx.status = "Confirm removal with y or cancel with n/Esc";
        return;
    }
    if (text == "/remove-empty") {
        if (!ctx.sqlite_available) {
            ctx.status = ctx.sqlite_unavailable_message();
            return;
        }
        if (ctx.active_job != ActiveJob::None) {
            ctx.status = "Cannot remove empty threads while a model job is running";
            return;
        }
        long long deleted_count = 0;
        bool current_removed = false;
        Error remove_error =
            ctx.sqlite_store.soft_delete_empty_threads(deleted_count, ctx.session.thread_id, current_removed);
        if (!remove_error.ok()) {
            ctx.status = detail::error_line(remove_error);
            return;
        }
        if (current_removed) {
            ctx.sqlite_store.set_last_thread_id(0);
            handlers.start_new_chat_thread("");
        }
        if (deleted_count == 0) {
            ctx.status = "No empty threads to remove";
        } else if (deleted_count == 1) {
            ctx.status = current_removed ? "Removed 1 empty thread and started a new chat"
                                         : "Removed 1 empty thread";
        } else {
            ctx.status = current_removed
                             ? "Removed " + std::to_string(deleted_count) +
                                   " empty threads and started a new chat"
                             : "Removed " + std::to_string(deleted_count) + " empty threads";
        }
        return;
    }
    if (text == "/pop") {
        handlers.pop_last_message();
        return;
    }
    if (text == "/response") {
        handlers.start_response_to_unanswered_user();
        return;
    }
    if (text == "/cleanup") {
        handlers.start_media_cleanup();
        return;
    }
    if (text == "/insert" || text.rfind("/insert ", 0) == 0) {
        handlers.start_insert(app::detail::trim_ascii(text.substr(7)));
        return;
    }
    if (text == "/attach" || text.rfind("/attach ", 0) == 0) {
        handlers.start_attach(app::detail::trim_ascii(text.substr(7)));
        return;
    }
    if (text == "/fetch" || text.rfind("/fetch ", 0) == 0) {
        handlers.start_fetch(app::detail::trim_ascii(text.substr(6)));
        return;
    }
    if (text == "/search" || text.rfind("/search ", 0) == 0) {
        handlers.start_search(app::detail::trim_ascii(text.substr(7)));
        return;
    }
    if (text == "/shell-stdout" || text.rfind("/shell-stdout ", 0) == 0) {
        if (handlers.start_shell) {
            handlers.start_shell(
                app::detail::trim_ascii(text.size() > 13 ? text.substr(13) : ""), true);
        } else {
            ctx.status = "Shell command is unavailable";
        }
        return;
    }
    if (text == "/shell" || text.rfind("/shell ", 0) == 0) {
        if (handlers.start_shell) {
            handlers.start_shell(
                app::detail::trim_ascii(text.size() > 6 ? text.substr(6) : ""), false);
        } else {
            ctx.status = "Shell command is unavailable";
        }
        return;
    }
    ctx.status = "Unknown command: " + text;
}

}  // namespace ainiux::tui
