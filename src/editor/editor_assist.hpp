#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "common.hpp"
#include "editor/ai_continue.hpp"
#include "editor/editor.hpp"
#include "provider/provider.hpp"
#include "runtime/runtime.hpp"

namespace pkchat::editor {

enum class AssistCommandKind {
    Unknown,
    Spell,
    Grammar,
    Continue,
    Fact,
    Prompt,
    Quit,
};

enum class AssistScope {
    Selection,
    All,
};

enum class AssistPromptMode {
    Continue,
    Selection,
    All,
};

enum class AssistEditKind {
    StreamInsert,
    ReplaceInPlace,
};

struct ParsedAssistCommand {
    AssistCommandKind kind = AssistCommandKind::Unknown;
    std::optional<AssistScope> scope;
    std::string custom_prompt;
    bool ok = false;
    std::string error_message;
};

struct AssistExecution {
    bool ok = false;
    std::string error_message;
    std::vector<provider::Message> messages;
    bool stream = false;
    AssistEditKind edit_kind = AssistEditKind::StreamInsert;
    size_t replace_start = 0;
    size_t replace_count = 0;
};

struct AssistCompletionResult {
    Error error;
    size_t match_count = 0;
    size_t choice_index = 0;
    std::string value;
    bool changed = false;
    bool cycling = false;
    bool handled = true;
};

struct AssistCompleterState {
    bool active = false;
    size_t next_choice = 0;
    std::string applied_value;
    std::vector<std::string> candidates;
};

const std::vector<std::string>& assist_command_completions();
std::string assist_completion_status(const AssistCompletionResult& result);
AssistCompletionResult complete_assist_command(std::string& input, AssistCompleterState& state);
ParsedAssistCommand parse_assist_command(const std::string& line);
std::string assist_scope_prompt(AssistCommandKind kind);
std::string assist_prompt_mode_message();
AssistExecution build_assist_execution(const EditorState& state,
                                       const AiContinueContext& context,
                                       AssistCommandKind kind,
                                       std::optional<AssistScope> scope,
                                       const std::string& custom_prompt,
                                       std::optional<AssistPromptMode> prompt_mode);
provider::RequestContext assist_request_context(const AiContinueContext& context, bool stream);
void start_assist_job(const AiContinueContext& context,
                      const std::vector<provider::Message>& messages,
                      bool stream,
                      runtime::EventQueue<ContinueEvent>& events,
                      runtime::JobHandle& job);
std::string trim_assist_inplace_response(std::string text);

class AssistStreamFilter {
   public:
    std::string feed(const std::string& chunk);
    std::string finish();

   private:
    std::string strip_trailing_close_tag(std::string text) const;
    std::string emit_with_holdback(std::string chunk);

    const std::string open_tag_ = "<content>";
    const std::string close_tag_ = "</content>";
    bool decided_ = false;
    bool stripping_ = false;
    bool done_ = false;
    std::string detect_buffer_;
    std::string holdback_;
};

}  // namespace pkchat::editor