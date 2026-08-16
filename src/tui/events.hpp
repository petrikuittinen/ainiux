#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "ainiux/compaction_strategy.hpp"
#include "chat/session.hpp"
#include "agent/activity.hpp"
#include "agent/index/index.hpp"
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
    ShellDone,
    ModelsDone,
    CreditBalanceDone,
    CompletionDone,
    GuardApproval,  // agent Guard Ask pending (worker blocked)
    AgentProjectNewDone,
    AgentCompactDone,
    AgentIndexReportDone,
    AgentPhase,
    AgentProgress,
    AgentPrepareDone,
    AgentPrepareProgress,
    AgentIndexProgress,
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
    GuardApprovalConfirm,
    AgentPermissionSelect,
    AgentContinueConfirm,
    AgentNewConfirm,
    AgentIndexBuildConfirm,
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
    provider::CreditBalanceResult credit_balance;
    std::uint64_t background_generation = 0;
    std::string requested_provider;
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
    // User shell (/shell vs /shell-stdout): draft mode fills the input buffer.
    bool shell_to_draft = false;
    bool shell_failed = false;
    std::int64_t shell_exit_status = -1;
    bool shell_stdout_truncated = false;
    chat::MediaCleanupResult media_cleanup;
    bool automatic_cleanup = false;
    // Canonical Markdown prepared by chat /attach.
    std::string attached_source;
    provider::TextAttachment text_attachment;
    bool text_attachment_ready = false;
    // Agent turn completion: already-timed tool rows + wall-clock ordering stamps.
    bool agent_turn = false;
    std::vector<std::string> agent_tool_lines;
    std::vector<long long> agent_tool_line_ms;
    std::string agent_final_text;
    bool agent_needs_user_continue = false;
    long long agent_turn_started_ms = 0;
    long long agent_finished_at_ms = 0;
    long long agent_stream_output_tokens = 0;
    long long agent_stream_decode_ms = 0;
    bool agent_stream_tokens_estimated = false;
    // Guard Ask prompt (paired with ApprovalGate on the agent worker).
    std::string guard_tool_name;
    std::string guard_command_preview;
    std::string guard_rule_id;
    std::string guard_message;
    std::string guard_review_path;
    std::string agent_workspace;
    std::vector<provider::Message> agent_history;
    bool agent_history_loaded = false;
    bool agent_compacted = false;
    bool agent_compact_no_op = false;
    bool agent_index_enabled = false;
    CompactionStrategy agent_compact_requested = CompactionStrategy::Smart;
    CompactionStrategy agent_compact_applied = CompactionStrategy::Smart;
    agent::AgentActivityPhase agent_phase = agent::AgentActivityPhase::Thinking;
    agent::AgentProgressUpdate agent_progress;
    agent::PreparationProgress agent_prepare_progress;
    agent::index::Progress agent_index_progress;
};

enum class EscapeResult {
    Unhandled,
    Handled,
    // Physical F4 (same sequences as editor dired). Agent mode hops to dired.
    OpenDired,
};

}  // namespace ainiux::tui
