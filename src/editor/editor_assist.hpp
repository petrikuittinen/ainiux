#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "common.hpp"
#include "editor/ai_continue.hpp"
#include "editor/editor.hpp"
#include "editor/editor_prompts.hpp"
#include "editor/path_completion.hpp"
#include "provider/provider.hpp"
#include "runtime/runtime.hpp"

namespace ainiux::editor {

enum class AssistCommandKind {
    Unknown,
    Configured,
    Prompt,
    Quit,
    Regenerate,
    WebSearch,
};

enum class AssistScope {
    Selection,
    All,
    Continue,
    Insert,
    NewBuffer,
    NewBufferVSplit,
    NewBufferHSplit,
};

enum class AssistPromptMode {
    Selection,
    All,
    Insert,
    NewBuffer,
    NewBufferVSplit,
    NewBufferHSplit,
};

enum class AssistEditKind {
    StreamInsert,
    ReplaceInPlace,
    NewBuffer,
};

// How a NewBuffer assist places the empty target buffer.
enum class AssistNewBufferLayout {
    Alone,
    VSplit,
    HSplit,
};

struct ParsedAssistCommand {
    AssistCommandKind kind = AssistCommandKind::Unknown;
    size_t command_index = 0;
    std::optional<AssistScope> scope;
    std::string custom_prompt;
    bool ok = false;
    std::string error_message;
};

struct AssistExecution {
    bool ok = false;
    std::string error_message;
    std::vector<provider::Message> messages;
    std::vector<provider::Message> usage_messages;
    bool stream = false;
    AssistEditKind edit_kind = AssistEditKind::StreamInsert;
    AssistNewBufferLayout new_buffer_layout = AssistNewBufferLayout::Alone;
    size_t replace_start = 0;
    size_t replace_count = 0;
    bool code_completion = false;
    bool prose_completion = false;
    highlight::Language completion_language = highlight::Language::Text;
};

struct AssistCompletionResult {
    Error error;
    CompletionKind kind = CompletionKind::Command;
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
    PathCompleter path_completer;
};

std::vector<std::string> assist_command_completions(const EditorAssistConfig& config);
std::string assist_completion_status(const AssistCompletionResult& result);
AssistCompletionResult complete_assist_command(std::string& input,
                                               AssistCompleterState& state,
                                               const EditorAssistConfig& config);
ParsedAssistCommand parse_assist_command(const std::string& line, const EditorAssistConfig& config);
std::string assist_scope_prompt(const EditorAssistCommand& command);
std::string assist_prompt_mode_message();
std::optional<AssistPromptMode> assist_prompt_mode_for_key(unsigned char ch);
AssistExecution build_assist_execution(const EditorState& state,
                                       const AiContinueContext& context,
                                       AssistCommandKind kind,
                                       size_t command_index,
                                       std::optional<AssistScope> scope,
                                       const std::string& custom_prompt,
                                       std::optional<AssistPromptMode> prompt_mode);
provider::RequestContext assist_request_context(const AiContinueContext& context, bool stream);
void start_assist_job(const AiContinueContext& context,
                      const std::vector<provider::Message>& messages,
                      bool stream,
                      bool code_completion,
                      bool prose_completion,
                      highlight::Language completion_language,
                      runtime::EventQueue<ContinueEvent>& events,
                      runtime::JobHandle& job);
std::string trim_assist_inplace_response(std::string text);
void strip_trailing_assist_close_tag_without_undo(EditorState& state);

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

class ProseAssistStreamFilter {
   public:
    std::string feed(const std::string& chunk);
    std::string finish();

   private:
    std::string feed_body(const std::string& chunk);

    const std::string open_tag_ = "<content>";
    const std::string close_tag_ = "</content>";
    bool decided_ = false;
    bool wrapped_ = false;
    bool done_ = false;
    std::string leading_;
    std::string trailing_;
};

class CodeAssistStreamFilter {
   public:
    explicit CodeAssistStreamFilter(highlight::Language language);

    Error feed(const std::string& chunk, std::string& output);
    Error finish(std::string& output);

   private:
    Error decide_leading(bool finishing, std::string& output);
    void feed_fenced(const std::string& chunk, std::string& output);
    void process_fenced_line(std::string line, std::string& output);

    std::string language_;
    bool decided_ = false;
    bool fenced_ = false;
    bool done_ = false;
    std::string leading_;
    std::string line_buffer_;
    std::string pending_close_;
    std::string pending_after_close_;
};

}  // namespace ainiux::editor
