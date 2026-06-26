#include "editor/editor_assist.hpp"

#include "output/thinking.hpp"

#include <algorithm>
#include <cctype>
#include <utility>

namespace pkchat::editor {
namespace {

constexpr const char* kSpellTaskPrompt = "Fix the spelling, but don't change anything else.";
constexpr const char* kGrammarTaskPrompt = "Fix the grammar, but don't change anything else.";
constexpr const char* kContinueTaskPrompt = "Continue the text without any preamble...";
constexpr const char* kFactTaskPrompt = "Fact check the text and comment";

std::string lower_ascii_copy(std::string text) {
    for (char& ch : text) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return text;
}

std::string trim_ascii_copy(std::string text) {
    auto is_ws = [](unsigned char ch) {
        return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
    };
    while (!text.empty() && is_ws(static_cast<unsigned char>(text.front()))) {
        text.erase(text.begin());
    }
    while (!text.empty() && is_ws(static_cast<unsigned char>(text.back()))) {
        text.pop_back();
    }
    return text;
}

bool is_token_separator(char ch) {
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
}

std::string longest_common_prefix(const std::vector<std::string>& values) {
    if (values.empty()) {
        return "";
    }
    std::string prefix = values.front();
    for (size_t i = 1; i < values.size(); ++i) {
        size_t length = 0;
        const size_t limit = std::min(prefix.size(), values[i].size());
        while (length < limit && prefix[length] == values[i][length]) {
            ++length;
        }
        prefix.resize(length);
    }
    return prefix;
}

std::optional<AssistScope> parse_scope_token(const std::string& token) {
    const std::string lower = lower_ascii_copy(token);
    if (lower == "all" || lower == "a") {
        return AssistScope::All;
    }
    if (lower == "selection" || lower == "s") {
        return AssistScope::Selection;
    }
    return std::nullopt;
}

std::string task_prompt_for(AssistCommandKind kind) {
    switch (kind) {
        case AssistCommandKind::Spell:
            return kSpellTaskPrompt;
        case AssistCommandKind::Grammar:
            return kGrammarTaskPrompt;
        case AssistCommandKind::Continue:
            return kContinueTaskPrompt;
        case AssistCommandKind::Fact:
            return kFactTaskPrompt;
        default:
            return "";
    }
}

std::string command_name_for(AssistCommandKind kind) {
    switch (kind) {
        case AssistCommandKind::Spell:
            return "/spell";
        case AssistCommandKind::Grammar:
            return "/grammar";
        case AssistCommandKind::Continue:
            return "/continue";
        case AssistCommandKind::Fact:
            return "/fact";
        case AssistCommandKind::Prompt:
            return "/prompt";
        case AssistCommandKind::Quit:
            return "/quit";
        default:
            return "command";
    }
}

std::string build_user_message(const std::string& task_prompt, const std::string& text) {
    if (text.empty()) {
        return task_prompt;
    }
    return task_prompt + "\n\n" + text;
}

std::vector<provider::Message> build_messages(const AiContinueContext& context,
                                              const std::string& user_content) {
    return {{"system", effective_editor_system_prompt(context)}, {"user", user_content}};
}

void push_visible_delta(runtime::EventQueue<ContinueEvent>& events, const std::string& visible) {
    if (visible.empty()) {
        return;
    }
    ContinueEvent event;
    event.type = ContinueEventType::Delta;
    event.text = visible;
    events.push(std::move(event));
}

}  // namespace

const std::vector<std::string>& assist_command_completions() {
    static const std::vector<std::string> commands = {
        "/spell",
        "/spell all",
        "/spell selection",
        "/grammar",
        "/grammar all",
        "/grammar selection",
        "/continue",
        "/fact",
        "/prompt ",
        "/quit",
    };
    return commands;
}

std::string effective_editor_system_prompt(const AiContinueContext& context) {
    if (!context.request.options.system.empty()) {
        return context.request.options.system;
    }
    return kDefaultEditorSystemPrompt;
}

std::string assist_completion_status(const AssistCompletionResult& result) {
    if (!result.handled) {
        return "Tab completion is not active here";
    }
    if (!result.error.ok()) {
        return result.error.message;
    }
    const std::string value = trim_ascii_copy(result.value);
    if (result.match_count == 0) {
        return value.empty() ? "No commands match" : "No commands match " + value;
    }
    if (result.match_count == 1) {
        return "Completed command: " + value;
    }
    if (result.cycling) {
        return "Command " + std::to_string(result.choice_index + 1) + "/" +
               std::to_string(result.match_count) + ": " + value + " (Tab for next)";
    }
    return std::to_string(result.match_count) + " commands match; Tab again to cycle";
}

AssistCompletionResult complete_assist_command(std::string& input, AssistCompleterState& state) {
    AssistCompletionResult result;
    result.handled = true;

    if (state.active) {
        const size_t selected = state.next_choice;
        if (selected < state.candidates.size()) {
            input = state.candidates[selected];
            state.applied_value = input;
            state.next_choice = (selected + 1) % state.candidates.size();
            result.match_count = state.candidates.size();
            result.choice_index = selected;
            result.value = input;
            result.changed = true;
            result.cycling = true;
        }
        return result;
    }

    state.active = false;
    state.next_choice = 0;
    state.applied_value.clear();
    state.candidates.clear();

    const std::string token = input;
    for (const std::string& command : assist_command_completions()) {
        if (command.compare(0, token.size(), token) == 0) {
            state.candidates.push_back(command);
        }
    }

    result.match_count = state.candidates.size();
    result.value = token;
    if (state.candidates.empty()) {
        return result;
    }

    const std::string completion = state.candidates.size() == 1
                                       ? state.candidates.front()
                                       : longest_common_prefix(state.candidates);
    input = completion;
    result.value = completion;
    result.changed = completion != token;
    if (state.candidates.size() > 1) {
        state.active = true;
        state.applied_value = completion;
        state.next_choice = 0;
    } else {
        state.candidates.clear();
    }
    return result;
}

ParsedAssistCommand parse_assist_command(const std::string& line) {
    ParsedAssistCommand parsed;
    const std::string trimmed = trim_ascii_copy(line);
    if (trimmed.empty() || trimmed[0] != '/') {
        parsed.error_message = "Command must start with /";
        return parsed;
    }

    size_t index = 1;
    const size_t command_start = index;
    while (index < trimmed.size() && !is_token_separator(trimmed[index])) {
        ++index;
    }
    std::string command = lower_ascii_copy(trimmed.substr(command_start, index - command_start));
    while (!command.empty() && command.front() == '/') {
        command.erase(command.begin());
    }
    if (command.empty()) {
        parsed.error_message = "Command name is required after /";
        return parsed;
    }
    while (index < trimmed.size() && is_token_separator(trimmed[index])) {
        ++index;
    }
    const std::string remainder = trimmed.substr(index);

    if (command == "spell") {
        parsed.kind = AssistCommandKind::Spell;
    } else if (command == "grammar") {
        parsed.kind = AssistCommandKind::Grammar;
    } else if (command == "continue") {
        parsed.kind = AssistCommandKind::Continue;
        if (!remainder.empty()) {
            parsed.error_message = "/continue does not take arguments";
            return parsed;
        }
        parsed.ok = true;
        return parsed;
    } else if (command == "fact") {
        parsed.kind = AssistCommandKind::Fact;
        if (!remainder.empty()) {
            parsed.error_message = "/fact does not take arguments";
            return parsed;
        }
        parsed.ok = true;
        return parsed;
    } else if (command == "quit") {
        parsed.kind = AssistCommandKind::Quit;
        if (!remainder.empty()) {
            parsed.error_message = "/quit does not take arguments";
            return parsed;
        }
        parsed.ok = true;
        return parsed;
    } else if (command == "prompt") {
        parsed.kind = AssistCommandKind::Prompt;
        parsed.custom_prompt = remainder;
        if (parsed.custom_prompt.empty()) {
            parsed.error_message = "/prompt requires a custom prompt";
            return parsed;
        }
        parsed.ok = true;
        return parsed;
    } else {
        parsed.error_message = "Unknown command: /" + command;
        return parsed;
    }

    if (remainder.empty()) {
        parsed.ok = true;
        return parsed;
    }

    const size_t arg_end = remainder.find_first_of(" \t");
    const std::string arg = arg_end == std::string::npos ? remainder : remainder.substr(0, arg_end);
    const std::string trailing =
        arg_end == std::string::npos ? "" : trim_ascii_copy(remainder.substr(arg_end + 1));
    if (!trailing.empty()) {
        parsed.error_message = command_name_for(parsed.kind) + " has too many arguments";
        return parsed;
    }

    const std::optional<AssistScope> scope = parse_scope_token(arg);
    if (!scope.has_value()) {
        parsed.error_message = command_name_for(parsed.kind) + " scope must be all or selection";
        return parsed;
    }
    parsed.scope = scope;
    parsed.ok = true;
    return parsed;
}

std::string assist_scope_prompt(AssistCommandKind kind) {
    return command_name_for(kind) + " for selection (s), all (a)";
}

std::string assist_prompt_mode_message() {
    return "/prompt: continue from cursor (c), edit selection (s), edit all (a)";
}

AssistExecution build_assist_execution(const EditorState& state,
                                       const AiContinueContext& context,
                                       AssistCommandKind kind,
                                       std::optional<AssistScope> scope,
                                       const std::string& custom_prompt,
                                       std::optional<AssistPromptMode> prompt_mode) {
    AssistExecution execution;
    const size_t read_len = std::min(state.cursor, context.settings.max_read_chars);
    const std::string prefix = state.text.range_text(state.cursor - read_len, read_len);

    auto fail = [&](std::string message) {
        execution.error_message = std::move(message);
        return execution;
    };

    if (kind == AssistCommandKind::Continue) {
        execution.stream = true;
        execution.edit_kind = AssistEditKind::StreamInsert;
        execution.messages =
            build_messages(context, build_user_message(kContinueTaskPrompt, prefix));
        execution.ok = true;
        return execution;
    }

    if (kind == AssistCommandKind::Fact) {
        execution.stream = true;
        execution.edit_kind = AssistEditKind::StreamInsert;
        const std::string source =
            state.selection.has_range() ? state.selected_text() : prefix;
        execution.messages =
            build_messages(context, build_user_message(kFactTaskPrompt, source));
        execution.ok = true;
        return execution;
    }

    if (kind == AssistCommandKind::Spell || kind == AssistCommandKind::Grammar) {
        if (!scope.has_value()) {
            return fail("Missing scope for " + command_name_for(kind));
        }
        const std::string task = task_prompt_for(kind);
        if (*scope == AssistScope::Selection) {
            if (!state.selection.has_range()) {
                return fail(command_name_for(kind) + " selection requires an active selection");
            }
            execution.replace_start = state.selection.start();
            execution.replace_count = state.selection.end() - state.selection.start();
            execution.messages = build_messages(
                context, build_user_message(task, state.selected_text()));
        } else {
            execution.replace_start = 0;
            execution.replace_count = state.text.size();
            execution.messages =
                build_messages(context, build_user_message(task, state.text.str()));
        }
        execution.stream = false;
        execution.edit_kind = AssistEditKind::ReplaceInPlace;
        execution.ok = true;
        return execution;
    }

    if (kind == AssistCommandKind::Prompt) {
        if (custom_prompt.empty()) {
            return fail("/prompt requires a custom prompt");
        }
        if (!prompt_mode.has_value()) {
            return fail("Missing /prompt mode");
        }
        switch (*prompt_mode) {
            case AssistPromptMode::Continue:
                execution.stream = true;
                execution.edit_kind = AssistEditKind::StreamInsert;
                execution.messages =
                    build_messages(context, build_user_message(custom_prompt, prefix));
                break;
            case AssistPromptMode::Selection:
                if (!state.selection.has_range()) {
                    return fail("/prompt selection requires an active selection");
                }
                execution.replace_start = state.selection.start();
                execution.replace_count = state.selection.end() - state.selection.start();
                execution.messages = build_messages(
                    context, build_user_message(custom_prompt, state.selected_text()));
                execution.stream = false;
                execution.edit_kind = AssistEditKind::ReplaceInPlace;
                break;
            case AssistPromptMode::All:
                execution.replace_start = 0;
                execution.replace_count = state.text.size();
                execution.messages =
                    build_messages(context, build_user_message(custom_prompt, state.text.str()));
                execution.stream = false;
                execution.edit_kind = AssistEditKind::ReplaceInPlace;
                break;
        }
        execution.ok = true;
        return execution;
    }

    return fail("Unknown assist command");
}

provider::RequestContext assist_request_context(const AiContinueContext& context, bool stream) {
    provider::RequestContext job_context = context.request;
    job_context.options.stream = stream;
    job_context.options.has_max_output_tokens = true;
    job_context.options.max_output_tokens = context.settings.max_output_tokens;
    return job_context;
}

void start_assist_job(const AiContinueContext& context,
                      const std::vector<provider::Message>& messages,
                      bool stream,
                      runtime::EventQueue<ContinueEvent>& events,
                      runtime::JobHandle& job) {
    provider::RequestContext job_context = assist_request_context(context, stream);
    job.start([job_context, messages, stream, &events](runtime::CancellationToken token) mutable {
        provider::ChatResult chat;
        pkchat::output::ThinkingTraceSplitter splitter;
        bool pushed_thinking = false;
        bool pushed_writing = false;
        auto push_thinking = [&]() {
            if (pushed_thinking) {
                return;
            }
            pushed_thinking = true;
            ContinueEvent event;
            event.type = ContinueEventType::Thinking;
            events.push(std::move(event));
        };
        auto push_writing = [&]() {
            if (pushed_writing) {
                return;
            }
            pushed_writing = true;
            ContinueEvent event;
            event.type = ContinueEventType::Writing;
            events.push(std::move(event));
        };
        auto on_delta = [&](const std::string& delta) -> Error {
            if (token.cancelled()) {
                return {ErrorCode::Cancelled, "AI assist cancelled while streaming"};
            }
            if (!stream) {
                return ok_error();
            }
            pkchat::output::ThinkingChunk chunk = splitter.feed(delta);
            if (!chunk.trace.empty() && chunk.visible.empty()) {
                push_thinking();
            }
            if (!chunk.visible.empty()) {
                push_writing();
            }
            push_visible_delta(events, chunk.visible);
            return ok_error();
        };

        Error send_error = provider::send_chat_messages(job_context, messages, on_delta, chat, token);
        if (send_error.ok()) {
            if (stream) {
                pkchat::output::ThinkingChunk final = splitter.finish();
                if (!final.trace.empty() && final.visible.empty()) {
                    push_thinking();
                }
                if (!final.visible.empty()) {
                    push_writing();
                }
                push_visible_delta(events, final.visible);
            } else if (!chat.content.empty()) {
                push_writing();
            }
            ContinueEvent event;
            event.type = ContinueEventType::Done;
            event.chat = std::move(chat);
            events.push(std::move(event));
            return;
        }

        ContinueEvent event;
        event.type = ContinueEventType::Error;
        event.error = std::move(send_error);
        events.push(std::move(event));
    });
}

std::string trim_assist_inplace_response(std::string text) {
    return trim_ascii_copy(std::move(text));
}

}  // namespace pkchat::editor