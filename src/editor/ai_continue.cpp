#include "editor/ai_continue.hpp"

#include "output/thinking.hpp"

#include <cstdlib>
#include <utility>

namespace pkchat::editor {
namespace {

int parse_positive_int_env(const char* name, int default_value) {
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0') {
        return default_value;
    }
    char* end = nullptr;
    const long parsed = std::strtol(value, &end, 10);
    if (end == value || (end != nullptr && *end != '\0') || parsed <= 0) {
        return default_value;
    }
    return static_cast<int>(parsed);
}

size_t parse_positive_size_env(const char* name, size_t default_value) {
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0') {
        return default_value;
    }
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(value, &end, 10);
    if (end == value || (end != nullptr && *end != '\0') || parsed == 0) {
        return default_value;
    }
    return static_cast<size_t>(parsed);
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

AiContinueSettings ai_continue_settings_from_env() {
    AiContinueSettings settings;
    settings.max_read_chars = parse_positive_size_env("MAX_AI_CONTINUE_READ", kDefaultAiContinueReadChars);
    settings.max_output_tokens =
        parse_positive_int_env("MAX_AI_CONTINUE_TOKENS", kDefaultAiContinueMaxTokens);
    return settings;
}

std::string continue_status_message(const std::string& model_name, const std::string& suffix) {
    if (model_name.empty()) {
        return suffix;
    }
    return "[" + model_name + "] " + suffix;
}

Error validate_continue_request(const AiContinueContext& context) {
    if (context.request.profile.offline) {
        return {ErrorCode::UnsupportedFeature,
                "AI continue requires a provider; use --provider lmstudio or similar"};
    }
    if (context.request.options.model.empty()) {
        return {ErrorCode::BadArgs, "AI continue requires --model"};
    }
    return ok_error();
}

provider::RequestContext continue_request_context(const AiContinueContext& context) {
    provider::RequestContext job_context = context.request;
    job_context.options.stream = true;
    job_context.options.has_max_output_tokens = true;
    job_context.options.max_output_tokens = context.settings.max_output_tokens;
    return job_context;
}

void start_continue_job(const AiContinueContext& context,
                        const std::string& prefix,
                        runtime::EventQueue<ContinueEvent>& events,
                        runtime::JobHandle& job) {
    provider::RequestContext job_context = continue_request_context(context);
    std::vector<provider::Message> messages = {{"user", prefix}};
    job.start([job_context, messages = std::move(messages), &events](runtime::CancellationToken token) mutable {
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
                return {ErrorCode::Cancelled, "AI continue cancelled while streaming"};
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
            pkchat::output::ThinkingChunk final = splitter.finish();
            if (!final.trace.empty() && final.visible.empty()) {
                push_thinking();
            }
            if (!final.visible.empty()) {
                push_writing();
            }
            push_visible_delta(events, final.visible);
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

}  // namespace pkchat::editor