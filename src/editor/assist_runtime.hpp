#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "editor/ai_continue.hpp"
#include "editor/editor.hpp"
#include "editor/editor_assist.hpp"
#include "editor/terminal_ui.hpp"
#include "provider/provider.hpp"
#include "runtime/runtime.hpp"
#include "tui/tui.hpp"

namespace pkchat::editor {

struct AssistSession {
    runtime::JobHandle job;
    runtime::EventQueue<ContinueEvent> events;
    bool active = false;
    bool streaming = false;
    bool saw_visible = false;
    AssistEditKind edit_kind = AssistEditKind::StreamInsert;
    EditorSnapshot undo_before;
    std::string provider_name;
    std::string model_name;
    std::vector<provider::Message> messages;
    std::vector<provider::Message> usage_messages;
    std::string status_suffix;
    tui::ActivityKind activity_kind = tui::ActivityKind::None;
    size_t replace_start = 0;
    size_t replace_count = 0;
    bool new_buffer_assist = false;
    size_t source_buffer_index = 0;
};

struct StoredAssistCommand {
    bool valid = false;
    AssistCommandKind kind = AssistCommandKind::Unknown;
    size_t command_index = 0;
    std::optional<AssistScope> scope;
    std::string custom_prompt;
    std::optional<AssistPromptMode> prompt_mode;
    bool has_revertable_output = false;
    EditorSnapshot revert_snapshot;
    size_t user_message_index = static_cast<size_t>(-1);
    size_t assistant_message_index = static_cast<size_t>(-1);
    std::string source_text;
};

void clear_assist_session(AssistSession& session);

std::string assist_request_text_from_messages(const std::vector<provider::Message>& messages);

struct AssistEventHandlers {
    std::function<void()> on_thinking;
    std::function<void()> on_writing;
    std::function<Error(const std::string&)> on_delta;
    std::function<void(const ContinueEvent& event, AssistSession& session)> on_done;
    std::function<void(const Error& error, bool cancelled, AssistSession& session)> on_error;
};

bool process_assist_events(AssistSession& session,
                           AssistEventHandlers& handlers,
                           bool& regenerate_after_cancel,
                           bool& pending_regenerate_restart);

bool is_configured_assist_slash_command(const std::string& line, const EditorAssistConfig& config);

std::vector<std::string> chat_assist_command_completions(const EditorAssistConfig& config);

}  // namespace pkchat::editor