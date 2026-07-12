#include "tui/commands.hpp"

#include "app/app.hpp"
#include "app/detail.hpp"
#include "chat/generation_settings.hpp"
#include "chat/settings.hpp"
#include "pkchat/model_setting.hpp"
#include "tui/detail/render.hpp"
#include "tui/theme_registry.hpp"

namespace pkchat::tui {

void handle_tui_command(const std::string& text, TuiCommandContext& ctx, TuiCommandHandlers& handlers) {
    if (text == "/quit" || text == "/exit") {
        handlers.quit();
        return;
    }
    if (text == "/help") {
        if (ctx.help_text.empty()) {
            ctx.settings_text.clear();
            ctx.help_text =
                "/help (hide/show this panel)\n"
                "/quit or /exit\n"
                "/clear\n"
                "/edit\n"
                "/list (Ctrl+L; N new thread)\n"
                "/new [NAME]\n"
                "/provider [PROVIDER]\n"
                "/models\n"
                "/model [MODEL]\n"
                "/system [TEXT]\n"
                "/setting (hide/show current settings)\n"
                "/setting NAME=VALUE\n"
                "/setting general|coding|instruct|creative\n"
                "/clone\n"
                "/save [PATH]\n"
                "/load PATH\n"
                "/remove\n"
                "/remove-empty\n"
                "/pop\n"
                "/response\n"
                "/insert PATH or /attach PATH (text or image)\n"
                "/fetch URL\n"
                "/search QUERY\n"
                "/theme [THEME]\n"
                "/thinking [trace|notrace]\n"
                "/editor (Ctrl+P; switch to editor mode)\n"
                "AI commands from editor-commands.conf (/spell, /grammar, /continue,\n"
                "/Chinese, /prompt, /regenerate, and custom commands)";
            ctx.status = "Help shown; /help hides it";
        } else {
            ctx.help_text.clear();
            ctx.status = "Help hidden";
        }
        ctx.history_scroll = 0;
        return;
    }
    if (text.rfind("/thinking", 0) == 0) {
        const std::string requested = app::detail::trim_ascii(text.substr(9));
        if (requested.empty()) {
            ctx.status = std::string("Thinking traces: ") + (ctx.show_thinking_traces ? "trace" : "notrace") +
                           ". Use /thinking trace or /thinking notrace";
            return;
        }
        if (requested == "trace") {
            handlers.set_thinking_trace_mode(true);
            return;
        }
        if (requested == "notrace") {
            handlers.set_thinking_trace_mode(false);
            return;
        }
        ctx.status = "Usage: /thinking trace|notrace";
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
    if (text == "/edit") {
        handlers.start_history_edit();
        return;
    }
    if (text == "/clear") {
        ctx.session.messages.clear();
        ctx.pending_images.clear();
        ctx.inflight_image_count = 0;
        app::apply_system_prompt(ctx.session, ctx.context.options.system);
        ctx.history_scroll = 0;
        ctx.status = "Chat history cleared";
        return;
    }
    if (text == "/list") {
        handlers.start_thread_list();
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
        ctx.context.options.model = model;
        ctx.session.model = model;
        ctx.status = provider_model_status_message(ctx.context, "ready");
        handlers.start_store_save();
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
                ctx.settings_text = chat::format_settings_panel(ctx.context.options);
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
            const ModelSetting* preset =
                chat::find_model_setting(ctx.context.options.model,
                                         requested,
                                         ctx.context.options.model_settings);
            if (preset == nullptr) {
                ctx.status = "No [Model-setting] preset for model " + ctx.context.options.model +
                             " purpose " + requested;
                return;
            }
            Error preset_error = chat::apply_model_setting_preset(ctx.context.options, *preset);
            if (!preset_error.ok()) {
                ctx.status = detail::error_line(preset_error);
                return;
            }
            if (!preset->default_system_prompt.empty()) {
                app::replace_system_prompt(ctx.session, preset->default_system_prompt);
                ctx.context.options.system = preset->default_system_prompt;
            }
            handlers.persist_settings_change("Applied " + requested + " settings for " + ctx.context.options.model);
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
        Error setting_error = chat::apply_chat_setting(ctx.context.options, name, value);
        if (!setting_error.ok()) {
            ctx.status = setting_error.message;
            return;
        }
        handlers.persist_settings_change("Updated " + name);
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
    if (text == "/insert" || text.rfind("/insert ", 0) == 0 || text == "/attach" ||
        text.rfind("/attach ", 0) == 0) {
        handlers.start_insert(app::detail::trim_ascii(text.substr(7)));
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
    ctx.status = "Unknown command: " + text;
}

}  // namespace pkchat::tui