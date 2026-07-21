#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "chat/session.hpp"
#include "chat/sqlite_store.hpp"
#include "common.hpp"
#include "context/context.hpp"
#include "editor/editor.hpp"
#include "editor/path_completion.hpp"
#include "provider/provider.hpp"

namespace ainiux::tui {

enum class TuiEventType {
    Delta,
    Done,
    Error,
    SaveDone,
    LoadDone,
    StoreSaveDone,
    StoreLoadDone,
    MediaCleanupDone,
    InsertDone,
    AttachDone,
    FetchDone,
    SearchDone,
    ModelsDone,
    CompletionDone,
};

enum class ActiveJob { None, Chat, Models };
enum class TuiMode {
    Chat,
    ThreadList,
    ProviderList,
    ModelList,
    ReasoningList,
    ReasoningConfirm,
    RemoveConfirm,
    ModelConfirm,
    SystemEdit,
    HistoryEdit,
    AttachmentList,
    AttachmentDeleteConfirm,
    ThreadDeleteConfirm,
};

enum class ModelsRequestPurpose { Preview, Picker };

struct TuiEvent {
    TuiEventType type = TuiEventType::Delta;
    std::string text;
    Error error;
    provider::ChatResult chat;
    chat::Session session;
    std::vector<std::string> models;
    provider::ModelsResult models_result;
    provider::Message inserted_message;
    std::string inserted_text;
    provider::ImageInput image;
    bool image_attachment = false;
    context::CompactionEvent compaction;
    bool compacted = false;
    editor::EditorState completed_input;
    editor::ContextualCompleter path_completer;
    editor::PathCompletionResult completion;
    size_t completion_generation = 0;
    bool quiet_success = false;
    chat::MediaCleanupResult media_cleanup;
    bool automatic_cleanup = false;
    // Canonical Markdown prepared by chat /attach.
    std::string attached_source;
    provider::TextAttachment text_attachment;
    bool text_attachment_ready = false;
    // Agent turn completion: structured tool lines + timestamps for elapsed UI.
    bool agent_turn = false;
    std::vector<std::string> agent_tool_lines;
    std::vector<long long> agent_tool_line_ms;
    std::string agent_final_text;
    long long agent_turn_started_ms = 0;
    long long agent_finished_at_ms = 0;
};

enum class EscapeResult {
    Unhandled,
    Handled,
};

}  // namespace ainiux::tui
