#pragma once

#include <functional>
#include <optional>
#include <string>

#include "chat/session.hpp"
#include "editor/editor.hpp"
#include "editor/editor_assist.hpp"
#include "editor/editor_prompts.hpp"
#include "provider/provider.hpp"

namespace pkchat::tui {

struct ChatEditorNewBufferAssist {
    size_t command_index = 0;
    std::string selection_text;
};

struct ChatAssistCallbacks {
    std::function<void(const std::string&)> start_turn;
    std::function<void()> regenerate_last_turn;
    std::function<void()> start_store_save;
    std::function<bool(const ChatEditorNewBufferAssist&)> switch_to_editor_new_buffer_assist;
};

std::optional<std::string> chat_assist_turn_prompt(const editor::ParsedAssistCommand& parsed,
                                                   const editor::EditorAssistConfig& config);

bool try_handle_chat_assist_command(const std::string& text,
                                    const editor::EditorState& input,
                                    const editor::EditorAssistConfig& assist_config,
                                    provider::RequestContext& context,
                                    chat::Session& session,
                                    std::string& status,
                                    int& history_scroll,
                                    const ChatAssistCallbacks& callbacks);

bool handle_chat_assist_continue_key(const editor::EditorAssistConfig& assist_config,
                                     std::string& status,
                                     const ChatAssistCallbacks& callbacks);

}  // namespace pkchat::tui