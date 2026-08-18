#include "tui/chat_assist.hpp"

#include "app/detail.hpp"
#include "editor/assist_runtime.hpp"
#include "provider/provider.hpp"
#include "search/search.hpp"

namespace ainiux::tui {

std::optional<std::string> chat_assist_turn_prompt(const editor::ParsedAssistCommand& parsed,
                                                   const editor::EditorAssistConfig& config) {
    if (!parsed.ok) {
        return std::nullopt;
    }
    switch (parsed.kind) {
        case editor::AssistCommandKind::Configured:
            if (parsed.command_index >= config.commands.size()) {
                return std::nullopt;
            }
            return config.commands[parsed.command_index].prompt;
        case editor::AssistCommandKind::Prompt:
            return parsed.custom_prompt;
        default:
            return std::nullopt;
    }
}

bool try_handle_chat_assist_command(const std::string& text,
                                    const editor::EditorState& input,
                                    const editor::EditorAssistConfig& assist_config,
                                    provider::RequestContext& context,
                                    chat::Session& session,
                                    std::string& status,
                                    int& history_scroll,
                                    const ChatAssistCallbacks& callbacks) {
    if (text.empty() || text[0] != '/') {
        return false;
    }

    const editor::ParsedAssistCommand parsed = editor::parse_assist_command(text, assist_config);
    if (parsed.kind == editor::AssistCommandKind::Unknown) {
        if (!parsed.ok && editor::is_configured_assist_slash_command(text, assist_config)) {
            status = parsed.error_message;
            return true;
        }
        return false;
    }

    if (parsed.kind == editor::AssistCommandKind::Quit) {
        return false;
    }

    if (parsed.kind == editor::AssistCommandKind::Regenerate) {
        callbacks.regenerate_last_turn();
        return true;
    }

    if (parsed.kind == editor::AssistCommandKind::WebSearch) {
        if (provider::hosted_web_search_enabled(context)) {
            session.messages.push_back(
                {"user", "Search the web for: " + parsed.custom_prompt});
            history_scroll = 0;
            status = "Using model-hosted web_search";
            callbacks.start_store_save();
            return true;
        }
        search::Options search_options = search::options_for(context.options);
        search::SearchResponse response;
        Error search_error = search::search(parsed.custom_prompt, search_options, response);
        if (!search_error.ok()) {
            status = search_error.message;
            return true;
        }
        session.messages.push_back(
            {"user", search::format_context_message(parsed.custom_prompt, response)});
        history_scroll = 0;
        status = "Inserted web search results from " + response.provider_used;
        callbacks.start_store_save();
        return true;
    }

    if (parsed.kind == editor::AssistCommandKind::Configured &&
        parsed.scope == editor::AssistScope::NewBuffer) {
        if (!input.selection.has_range()) {
            status = "AI new-buffer command requires selected text in chat input";
            return true;
        }
        if (!callbacks.switch_to_editor_new_buffer_assist) {
            status = "Editor mode is unavailable";
            return true;
        }
        ChatEditorNewBufferAssist pending;
        pending.command_index = parsed.command_index;
        pending.selection_text = input.selected_text();
        if (!callbacks.switch_to_editor_new_buffer_assist(pending)) {
            return true;
        }
        return true;
    }

    const std::optional<std::string> prompt = chat_assist_turn_prompt(parsed, assist_config);
    if (!prompt.has_value() || app::detail::trim_ascii(*prompt).empty()) {
        status = "AI command is missing a prompt";
        return true;
    }

    callbacks.start_turn(*prompt);
    return true;
}

bool handle_chat_assist_continue_key(const editor::EditorAssistConfig& assist_config,
                                     std::string& status,
                                     const ChatAssistCallbacks& callbacks) {
    const std::optional<size_t> command_index =
        editor::assist_command_index(assist_config, "/continue");
    if (!command_index.has_value()) {
        status = "Configured editor assist commands are missing /continue";
        return true;
    }
    if (*command_index >= assist_config.commands.size()) {
        status = "Configured /continue command index is out of range";
        return true;
    }
    const std::string& prompt = assist_config.commands[*command_index].prompt;
    if (app::detail::trim_ascii(prompt).empty()) {
        status = "Configured /continue command is missing a prompt";
        return true;
    }
    callbacks.start_turn(prompt);
    return true;
}

}  // namespace ainiux::tui