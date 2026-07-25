#include "editor/assist_runtime.hpp"

#include "common.hpp"

#include <algorithm>

namespace ainiux::editor {
namespace {

constexpr const char* kContentOpenTag = "<content>";
constexpr const char* kContentCloseTag = "</content>";

bool command_token_matches_config(const std::string& line, const EditorAssistConfig& config) {
    const std::string trimmed = ascii_trim(line);
    if (trimmed.empty() || trimmed[0] != '/') {
        return false;
    }
    size_t index = 1;
    while (index < trimmed.size() && trimmed[index] != ' ' && trimmed[index] != '\t' &&
           trimmed[index] != '\r' && trimmed[index] != '\n') {
        ++index;
    }
    const std::string token = trimmed.substr(1, index - 1);
    if (token.empty()) {
        return false;
    }
    const std::optional<size_t> command_index = assist_command_index(config, "/" + token);
    return command_index.has_value();
}

}  // namespace

void clear_assist_session(AssistSession& session) {
    session.job.join();
    ContinueEvent event;
    while (session.events.try_pop(event)) {
    }
    session.active = false;
    session.streaming = false;
    session.saw_visible = false;
    session.code_completion = false;
    session.prose_completion = false;
    session.edit_kind = AssistEditKind::StreamInsert;
    session.undo_before = EditorSnapshot{};
    session.provider_name.clear();
    session.model_name.clear();
    session.messages.clear();
    session.usage_messages.clear();
    session.status_suffix.clear();
    session.activity_kind = tui::ActivityKind::None;
    session.replace_start = 0;
    session.replace_count = 0;
    session.new_buffer_assist = false;
    session.opened_split_for_assist = false;
    session.source_buffer_index = 0;
}

std::string assist_request_text_from_messages(const std::vector<provider::Message>& messages) {
    for (const provider::Message& message : messages) {
        if (message.role != "user") {
            continue;
        }
        std::string text = ascii_trim(message.content);
        const std::string open = kContentOpenTag;
        if (text.rfind(open, 0) == 0) {
            text.erase(0, open.size());
            text = ascii_trim(std::move(text));
        }
        const std::string close = kContentCloseTag;
        if (text.size() >= close.size() &&
            text.compare(text.size() - close.size(), close.size(), close) == 0) {
            text.erase(text.size() - close.size());
            text = ascii_trim(std::move(text));
        }
        return text;
    }
    return "";
}

bool process_assist_events(AssistSession& session,
                           AssistEventHandlers& handlers,
                           bool& regenerate_after_cancel,
                           bool& pending_regenerate_restart) {
    bool updated = false;
    ContinueEvent event;
    while (session.events.try_pop(event)) {
        updated = true;
        switch (event.type) {
            case ContinueEventType::Thinking:
                if (handlers.on_thinking) {
                    handlers.on_thinking();
                }
                break;
            case ContinueEventType::Writing:
                if (handlers.on_writing) {
                    handlers.on_writing();
                }
                break;
            case ContinueEventType::Delta:
                session.saw_visible = true;
                if (handlers.on_writing) {
                    handlers.on_writing();
                }
                if (handlers.on_delta) {
                    const Error delta_error = handlers.on_delta(event.text);
                    if (!delta_error.ok()) {
                        session.job.cancel();
                        session.job.join();
                        if (handlers.on_error) {
                            handlers.on_error(delta_error, false, session);
                        }
                        clear_assist_session(session);
                        return true;
                    }
                }
                break;
            case ContinueEventType::Done:
                if (handlers.on_done) {
                    handlers.on_done(event, session);
                }
                return true;
            case ContinueEventType::Error:
                if (event.error.code == ErrorCode::Cancelled) {
                    if (regenerate_after_cancel) {
                        regenerate_after_cancel = false;
                        session.job.join();
                        if (handlers.on_error) {
                            handlers.on_error(event.error, true, session);
                        }
                        clear_assist_session(session);
                        pending_regenerate_restart = true;
                    } else if (handlers.on_error) {
                        session.job.join();
                        handlers.on_error(event.error, true, session);
                        clear_assist_session(session);
                    }
                } else if (handlers.on_error) {
                    session.job.join();
                    handlers.on_error(event.error, false, session);
                    clear_assist_session(session);
                }
                return true;
        }
    }
    return updated;
}

bool is_configured_assist_slash_command(const std::string& line, const EditorAssistConfig& config) {
    return command_token_matches_config(line, config);
}

std::string chat_assist_command_name(const EditorAssistCommand& command) {
    if (!command.command.empty() && command.command.front() == '/') {
        return command.command;
    }
    return "/" + command.command;
}

std::vector<std::string> chat_assist_command_completions(const EditorAssistConfig& config) {
    std::vector<std::string> commands;
    commands.reserve(config.commands.size() + 32);
    for (const EditorAssistCommand& command : config.commands) {
        commands.push_back(chat_assist_command_name(command));
    }
    static const char* kChatOnly[] = {
        "/attach ",
        "/clear",
        "/clone",
        "/cleanup",
        "/context ",
        "/edit",
        "/editor",
        "/exit",
        "/fetch ",
        "/help",
        "/highlight ",
        "/insert ",
        "/list",
        "/load ",
        "/model ",
        "/models",
        "/mode ",
        "/new ",
        "/pop",
        "/prompt ",
        "/provider ",
        "/regenerate",
        "/remove",
        "/remove-empty",
        "/response",
        "/save ",
        "/search ",
        "/setting",
        "/setting ",
        "/system",
        "/theme ",
        "/thinking ",
        "/reasoning ",
    };
    for (const char* command : kChatOnly) {
        commands.push_back(command);
    }
    std::sort(commands.begin(), commands.end());
    commands.erase(std::unique(commands.begin(), commands.end()), commands.end());
    return commands;
}

}  // namespace ainiux::editor
