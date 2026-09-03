#include "server/job_service.hpp"

#include <set>
#include <cmath>
#include <optional>
#include <utility>

#include "app/app.hpp"
#include "app/operations.hpp"
#include "editor/ai_continue.hpp"
#include "editor/editor_assist.hpp"
#include "provider/provider.hpp"
#include "security/redact.hpp"
#include "server/metrics.hpp"
#include "server/workspace_service.hpp"

namespace ainiux::server {
namespace {

Error field_error(const std::string& field, const std::string& correction) {
    return {ErrorCode::BadArgs, "field '" + field + "' " + correction};
}

Error reject_unknown(const json::Value& root, const std::set<std::string>& allowed) {
    for (const auto& entry : root.object) {
        if (allowed.count(entry.first) == 0) {
            return field_error(entry.first, "is not supported for this operation");
        }
    }
    return ok_error();
}

Error optional_string(const json::Value& root,
                      const std::string& field,
                      std::string& output,
                      std::size_t max_bytes = 4096U) {
    const json::Value* value = root.get(field);
    if (value == nullptr) return ok_error();
    if (!value->is_string()) return field_error(field, "must be a string");
    if (value->string.size() > max_bytes) return field_error(field, "is too long");
    output = value->string;
    return ok_error();
}

Error required_string(const json::Value& root,
                      const std::string& field,
                      std::string& output,
                      std::size_t max_bytes = Limits::json_body_bytes) {
    Error error = optional_string(root, field, output, max_bytes);
    if (!error.ok()) return error;
    output = ascii_trim(output);
    if (output.empty()) return field_error(field, "must be a non-empty string");
    return ok_error();
}

Error optional_index(const json::Value& root,
                     const std::string& field,
                     std::optional<std::size_t>& output) {
    const json::Value* value = root.get(field);
    if (value == nullptr) return ok_error();
    if (value->type != json::Value::Type::Number || value->number < 0.0 ||
        std::floor(value->number) != value->number ||
        value->number > static_cast<double>(Limits::json_body_bytes)) {
        return field_error(field, "must be a non-negative integer within the editing limit");
    }
    output = static_cast<std::size_t>(value->number);
    return ok_error();
}

std::string base64_encode(const std::string& bytes) {
    static constexpr char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string output;
    output.reserve(((bytes.size() + 2U) / 3U) * 4U);
    for (std::size_t i = 0; i < bytes.size(); i += 3U) {
        const unsigned int a = static_cast<unsigned char>(bytes[i]);
        const unsigned int b = i + 1U < bytes.size()
                                   ? static_cast<unsigned char>(bytes[i + 1U]) : 0U;
        const unsigned int c = i + 2U < bytes.size()
                                   ? static_cast<unsigned char>(bytes[i + 2U]) : 0U;
        const unsigned int value = (a << 16U) | (b << 8U) | c;
        output.push_back(alphabet[(value >> 18U) & 63U]);
        output.push_back(alphabet[(value >> 12U) & 63U]);
        output.push_back(i + 1U < bytes.size() ? alphabet[(value >> 6U) & 63U] : '=');
        output.push_back(i + 2U < bytes.size() ? alphabet[value & 63U] : '=');
    }
    return output;
}

std::string valid_json_or_null(const std::string& value) {
    const json::ParseResult parsed = json::parse(value);
    return parsed.error.ok() ? value : "null";
}

Error public_operation_error(Error error, const std::vector<std::string>& secrets = {}) {
    if (error.ok()) return error;
    error.message = redact_secrets(std::move(error.message), secrets);
    if (error.code == ErrorCode::FileRead) {
        error.message = "a required server-side file could not be read";
    } else if (error.code == ErrorCode::FileWrite) {
        error.message = "the operation could not persist server-side state";
    } else if (error.code == ErrorCode::FileLock) {
        error.message = "server-side workspace state is locked";
    }
    return error;
}

}  // namespace

JobService::JobService(cli::Options base_options, std::string workspace, std::size_t max_jobs)
    : base_options_(std::move(base_options)),
      workspace_(std::move(workspace)),
      registry_(max_jobs) {
    base_options_.server = false;
    base_options_.quiet = true;
    base_options_.prompt.clear();
    base_options_.prompt_file.clear();
    base_options_.system_file.clear();
    base_options_.attachment_paths.clear();
    base_options_.input_path.clear();
    base_options_.fetch_url.clear();
    base_options_.search_query.clear();
}

Error JobService::validate_common(const json::Value& root,
                                  const std::string& operation,
                                  cli::Options& options) const {
    if (!root.is_object()) return {ErrorCode::BadArgs, "request body must be a JSON object"};
    std::string selected_provider;
    Error error = optional_string(root, "provider", selected_provider, 128U);
    if (!error.ok()) return error;
    if (!selected_provider.empty()) {
        const std::string canonical = provider::canonical_profile_name(selected_provider);
        bool known = false;
        for (const provider::Profile& profile : provider::built_in_profiles()) {
            if (profile.name == canonical) {
                known = true;
                break;
            }
        }
        if (!known) {
            return field_error("provider", "names an unknown configured provider profile");
        }
        provider::apply_provider_target(options, selected_provider);
        options.provider_explicit = true;
    }
    error = optional_string(root, "model", options.model, 512U);
    if (!error.ok()) return error;
    if (root.get("model") != nullptr) options.model_explicit = true;
    std::string api;
    error = optional_string(root, "api", api, 32U);
    if (!error.ok()) return error;
    if (!api.empty()) {
        if (api != "chat" && api != "responses") {
            return field_error("api", "must be 'chat' or 'responses'");
        }
        options.api = api;
        options.api_explicit = true;
    }
    options.image = operation == "image";
    options.agent_run = operation == "run" || operation == "plan";
    options.agent_plan = operation == "plan";
    return ok_error();
}

JobOutcome JobService::run_chat_job(cli::Options options,
                                    std::vector<provider::Message> messages,
                                    runtime::CancellationToken cancellation,
                                    JobEvents events) const {
    options.prompt = messages.back().content;
    provider::ContextResult built = provider::build_context(options);
    if (!built.error.ok()) return {public_operation_error(built.error, {options.key}), {}};
    Error model_error = app::choose_default_model(built.context);
    if (!model_error.ok()) return {public_operation_error(model_error, {built.context.api_key}), {}};
    app::operation::ChatResult result = app::operation::run_chat(
        built.context, {messages}, cancellation, std::move(events));
    if (!result.error.ok()) return {public_operation_error(result.error, {built.context.api_key}), {}};
    const provider::ChatResult& response = result.response;
    const GenerationMetrics metrics =
        chat_generation_metrics(built.context, messages, response);
    return {ok_error(),
            "{\"model\":" + json::quote(response.model) +
                ",\"content\":" + json::quote(response.content) +
                ",\"usage\":" + valid_json_or_null(response.usage_json) +
                ",\"timing\":{\"ttft_ms\":" + std::to_string(response.ttft_ms) +
                ",\"total_ms\":" + std::to_string(response.total_ms) + "}" +
                ",\"metrics\":" + generation_metrics_json(metrics) + "}"};
}

JobOutcome JobService::run_models_job(cli::Options options,
                                      runtime::CancellationToken cancellation) const {
    options.list_models = true;
    provider::ContextResult built = provider::build_context(options);
    if (!built.error.ok()) return {public_operation_error(built.error, {options.key}), {}};
    provider::ModelsResult result;
    const Error error = provider::list_models(built.context, result, cancellation);
    if (!error.ok()) return {public_operation_error(error, {built.context.api_key}), {}};
    std::string models = "[";
    for (std::size_t index = 0; index < result.model_ids.size(); ++index) {
        if (index != 0) models += ',';
        models += json::quote(result.model_ids[index]);
    }
    models += ']';
    return {ok_error(),
            "{\"provider\":" + json::quote(built.context.profile.name) +
                ",\"models\":" + models + "}"};
}

JobOutcome JobService::run_agent_job(cli::Options options,
                                     std::string goal,
                                     bool plan,
                                     runtime::CancellationToken cancellation,
                                     JobEvents events) const {
    options.prompt = goal;
    options.agent_run = true;
    options.agent_plan = plan;
    provider::ContextResult built = provider::build_context(options);
    if (!built.error.ok()) return {public_operation_error(built.error, {options.key}), {}};
    Error model_error = app::choose_default_model(built.context);
    if (!model_error.ok()) return {public_operation_error(model_error, {built.context.api_key}), {}};
    auto progress = [events](const std::string& text) {
        if (events) (void)events({app::operation::EventType::Progress, text, 0, 0});
    };
    const std::string api_key = built.context.api_key;
    const long long context_window_tokens = built.context.options.context_tokens;
    app::AgentGoalResult result = app::run_agent_goal(
        std::move(built.context), goal, cancellation, {}, false, progress, workspace_);
    if (!result.error.ok()) return {public_operation_error(result.error, {api_key}), {}};
    const GenerationMetrics metrics = agent_generation_metrics(
        -1, context_window_tokens, result.token_usage, result.elapsed_ms);
    return {ok_error(),
            "{\"content\":" + json::quote(result.final_text) +
                ",\"turns\":" + std::to_string(result.turns) +
                ",\"tool_calls\":" + std::to_string(result.tool_calls) +
                ",\"failed_tool_calls\":" + std::to_string(result.failed_tool_calls) +
                ",\"elapsed_ms\":" + std::to_string(result.elapsed_ms) +
                ",\"metrics\":" + generation_metrics_json(metrics) + "}"};
}

JobOutcome JobService::run_image_job(cli::Options options,
                                     app::operation::ImageRequest request,
                                     runtime::CancellationToken cancellation,
                                     JobEvents events) const {
    options.prompt = request.prompt;
    options.image = true;
    provider::ContextResult built = provider::build_context(options);
    if (!built.error.ok()) return {public_operation_error(built.error, {options.key}), {}};
    const std::string api_key = built.context.api_key;
    app::operation::ImageResult result = app::operation::run_image(
        std::move(built.context), request, cancellation, std::move(events));
    if (!result.error.ok()) return {public_operation_error(result.error, {api_key}), {}};
    const std::string format = result.response.output_format.empty()
                                   ? result.request.output_format
                                   : result.response.output_format;
    return {ok_error(),
            "{\"model\":" + json::quote(result.selected_model) +
                ",\"format\":" + json::quote(format) +
                ",\"data_base64\":" + json::quote(base64_encode(result.response.bytes)) +
                ",\"size\":" + json::quote(result.response.size) +
                ",\"total_ms\":" + std::to_string(result.response.total_ms) + "}"};
}

JobOutcome JobService::run_editor_assist_job(
    cli::Options options,
    std::string path,
    std::string revision,
    std::string instruction,
    std::optional<std::size_t> selection_start,
    std::optional<std::size_t> selection_end,
    runtime::CancellationToken cancellation,
    JobEvents events) const {
    WorkspaceService workspace(workspace_);
    WorkspaceFileSnapshot snapshot;
    std::string current_revision;
    Error error = workspace.load_file(path, revision, snapshot, &current_revision);
    if (!error.ok()) return {error, {}};
    if (selection_start.has_value() != selection_end.has_value()) {
        return {field_error("selection_start", "and selection_end must be supplied together"), {}};
    }
    if (selection_start.has_value() &&
        (*selection_start >= *selection_end || *selection_end > snapshot.content.size())) {
        return {field_error("selection_end", "must follow selection_start within the reviewed file"), {}};
    }

    options.editor = true;
    provider::ContextResult built = provider::build_context(options);
    if (!built.error.ok()) return {public_operation_error(built.error, {options.key}), {}};
    Error model_error = app::choose_default_model(built.context);
    if (!model_error.ok()) {
        return {public_operation_error(model_error, {built.context.api_key}), {}};
    }
    const std::string api_key = built.context.api_key;
    editor::AiContinueContext assist;
    assist.request = std::move(built.context);
    assist.settings = editor::ai_continue_settings(assist.request.options);
    assist.assist_config = assist.request.options.editor_assist_config;
    editor::EditorState state = editor::EditorState::from_text(snapshot.content);
    state.set_path(path);
    std::optional<editor::AssistPromptMode> mode = editor::AssistPromptMode::All;
    if (selection_start.has_value()) {
        state.selection.anchor = *selection_start;
        state.selection.active = *selection_end;
        state.cursor = *selection_end;
        mode = editor::AssistPromptMode::Selection;
    }
    const editor::AssistExecution execution = editor::build_assist_execution(
        state, assist, editor::AssistCommandKind::Prompt, 0, std::nullopt,
        instruction, mode);
    if (!execution.ok) return {{ErrorCode::BadArgs, execution.error_message}, {}};

    if (events) (void)events({app::operation::EventType::Progress, "editor assist thinking", 0, 0});
    provider::ChatResult chat;
    auto on_delta = [&cancellation](const std::string&) -> Error {
        return cancellation.cancelled()
                   ? Error{ErrorCode::Cancelled, "editor assist cancelled"}
                   : ok_error();
    };
    provider::RequestContext request = editor::assist_request_context(assist, false);
    error = provider::send_chat_messages(request, execution.messages, on_delta, chat, cancellation);
    if (!error.ok()) return {public_operation_error(error, {api_key}), {}};
    const std::string replacement = editor::trim_assist_inplace_response(chat.content);
    return {ok_error(),
            "{\"path\":" + json::quote(snapshot.path) +
                ",\"revision\":" + json::quote(snapshot.revision) +
                ",\"edit\":{\"start\":" + std::to_string(execution.replace_start) +
                ",\"length\":" + std::to_string(execution.replace_count) +
                ",\"replacement\":" + json::quote(replacement) + "}" +
                ",\"model\":" + json::quote(chat.model) +
                ",\"usage\":" + valid_json_or_null(chat.usage_json) +
                ",\"timing\":{\"ttft_ms\":" + std::to_string(chat.ttft_ms) +
                ",\"total_ms\":" + std::to_string(chat.total_ms) + "}}"};
}

ServiceSubmitResult JobService::submit(const std::string& operation,
                                       const std::string& body,
                                       const std::string& idempotency_key) {
    const json::ParseResult parsed = json::parse(body);
    if (!parsed.error.ok()) {
        return {{}, {ErrorCode::JsonParse, "request body is not valid JSON: " + parsed.error.message}};
    }
    cli::Options options = base_options_;
    Error error = validate_common(parsed.value, operation, options);
    if (!error.ok()) return {{}, error};
    const std::string canonical = json::stringify(parsed.value);

    if (operation == "models") {
        error = reject_unknown(parsed.value, {"provider", "api"});
        if (!error.ok()) return {{}, error};
        JobWork work = [this, options](runtime::CancellationToken token, JobEvents) {
            return run_models_job(options, token);
        };
        return {registry_.submit(operation, canonical, idempotency_key,
                                 JobClass::Provider, std::move(work)), ok_error()};
    }

    if (operation == "chat") {
        error = reject_unknown(parsed.value, {"provider", "model", "api", "messages"});
        if (!error.ok()) return {{}, error};
        const json::Value* messages_value = parsed.value.get("messages");
        if (messages_value == nullptr || !messages_value->is_array() || messages_value->array.empty()) {
            return {{}, field_error("messages", "must be a non-empty array")};
        }
        std::vector<provider::Message> messages;
        for (const json::Value& item : messages_value->array) {
            if (!item.is_object()) return {{}, field_error("messages", "entries must be objects")};
            error = reject_unknown(item, {"role", "content"});
            if (!error.ok()) return {{}, error};
            std::string role;
            std::string content;
            error = required_string(item, "role", role, 32U);
            if (!error.ok()) return {{}, error};
            if (role != "system" && role != "user" && role != "assistant") {
                return {{}, field_error("role", "must be 'system', 'user', or 'assistant'")};
            }
            error = required_string(item, "content", content);
            if (!error.ok()) return {{}, error};
            messages.emplace_back(std::move(role), std::move(content));
        }
        JobWork work = [this, options, messages = std::move(messages)](
                           runtime::CancellationToken token, JobEvents events) mutable {
            return run_chat_job(options, std::move(messages), token, std::move(events));
        };
        return {registry_.submit(operation, canonical, idempotency_key,
                                 JobClass::Provider, std::move(work)), ok_error()};
    }

    if (operation == "run" || operation == "plan") {
        error = reject_unknown(parsed.value, {"provider", "model", "api", "goal"});
        if (!error.ok()) return {{}, error};
        std::string goal;
        error = required_string(parsed.value, "goal", goal);
        if (!error.ok()) return {{}, error};
        const bool plan = operation == "plan";
        JobWork work = [this, options, goal = std::move(goal), plan](
                           runtime::CancellationToken token, JobEvents events) mutable {
            return run_agent_job(options, std::move(goal), plan, token, std::move(events));
        };
        return {registry_.submit(operation, canonical, idempotency_key,
                                 JobClass::Agent, std::move(work)), ok_error()};
    }

    if (operation == "image") {
        error = reject_unknown(parsed.value,
                               {"provider", "model", "api", "prompt", "size", "aspect",
                                "quality", "format"});
        if (!error.ok()) return {{}, error};
        app::operation::ImageRequest request;
        error = required_string(parsed.value, "prompt", request.prompt);
        if (!error.ok()) return {{}, error};
        request.model = options.model;
        error = optional_string(parsed.value, "size", request.size, 64U);
        if (!error.ok()) return {{}, error};
        error = optional_string(parsed.value, "aspect", request.aspect, 64U);
        if (!error.ok()) return {{}, error};
        error = optional_string(parsed.value, "quality", request.quality, 64U);
        if (!error.ok()) return {{}, error};
        error = optional_string(parsed.value, "format", request.format, 32U);
        if (!error.ok()) return {{}, error};
        request.format_explicit = parsed.value.get("format") != nullptr;
        request.max_image_bytes = Limits::upload_body_bytes;
        JobWork work = [this, options, request = std::move(request)](
                           runtime::CancellationToken token, JobEvents events) mutable {
            return run_image_job(options, std::move(request), token, std::move(events));
        };
        return {registry_.submit(operation, canonical, idempotency_key,
                                 JobClass::Provider, std::move(work)), ok_error()};
    }

    if (operation == "editor-assist") {
        error = reject_unknown(parsed.value,
                               {"provider", "model", "api", "path", "revision",
                                "instruction", "selection_start", "selection_end"});
        if (!error.ok()) return {{}, error};
        std::string path;
        std::string revision;
        std::string instruction;
        error = required_string(parsed.value, "path", path, Limits::request_line_bytes);
        if (!error.ok()) return {{}, error};
        const json::Value* path_value = parsed.value.get("path");
        if (path_value == nullptr || path_value->string != path) {
            return {{}, field_error("path", "must not have leading or trailing whitespace")};
        }
        error = required_string(parsed.value, "revision", revision, 128U);
        if (!error.ok()) return {{}, error};
        error = required_string(parsed.value, "instruction", instruction);
        if (!error.ok()) return {{}, error};
        std::optional<std::size_t> selection_start;
        std::optional<std::size_t> selection_end;
        error = optional_index(parsed.value, "selection_start", selection_start);
        if (!error.ok()) return {{}, error};
        error = optional_index(parsed.value, "selection_end", selection_end);
        if (!error.ok()) return {{}, error};
        if (selection_start.has_value() != selection_end.has_value()) {
            return {{}, field_error("selection_start", "and selection_end must be supplied together")};
        }
        JobWork work = [this, options, path = std::move(path), revision = std::move(revision),
                        instruction = std::move(instruction), selection_start, selection_end](
                           runtime::CancellationToken token, JobEvents events) mutable {
            return run_editor_assist_job(options, std::move(path), std::move(revision),
                                         std::move(instruction), selection_start, selection_end,
                                         token, std::move(events));
        };
        return {registry_.submit(operation, canonical, idempotency_key,
                                 JobClass::Provider, std::move(work)), ok_error()};
    }
    return {{}, {ErrorCode::BadArgs, "unknown job operation"}};
}

}  // namespace ainiux::server
