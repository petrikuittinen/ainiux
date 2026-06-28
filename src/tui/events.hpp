#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "chat/session.hpp"
#include "common.hpp"
#include "context/context.hpp"
#include "editor/editor.hpp"
#include "editor/path_completion.hpp"
#include "provider/provider.hpp"

namespace pkchat::tui {

enum class TuiEventType {
    Delta,
    Done,
    Error,
    SaveDone,
    LoadDone,
    StoreSaveDone,
    StoreLoadDone,
    InsertDone,
    FetchDone,
    ModelsDone,
    CompletionDone,
};

enum class ActiveJob { None, Chat, Models };
enum class TuiMode { Chat, ThreadList, RemoveConfirm };

struct TuiEvent {
    TuiEventType type = TuiEventType::Delta;
    std::string text;
    Error error;
    provider::ChatResult chat;
    chat::Session session;
    std::vector<std::string> models;
    provider::Message inserted_message;
    provider::ImageInput image;
    bool image_attachment = false;
    context::CompactionEvent compaction;
    bool compacted = false;
    editor::EditorState completed_input;
    editor::ContextualCompleter path_completer;
    editor::PathCompletionResult completion;
    size_t completion_generation = 0;
    bool quiet_success = false;
};

enum class EscapeResult {
    Unhandled,
    Handled,
    Regenerate,
};

}  // namespace pkchat::tui