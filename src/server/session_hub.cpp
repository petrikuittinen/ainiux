#include "server/session_hub.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <utility>

#include "app/app.hpp"
#include "agent/approval.hpp"
#include "agent/session_runtime.hpp"
#include "json/json.hpp"
#include "provider/provider.hpp"
#include "search/search.hpp"
#include "security/redact.hpp"

namespace ainiux::server {
namespace {

constexpr std::size_t kMaxSessionText = 256U * 1024U;
constexpr std::size_t kMaxReviewBytes = 1U * 1024U * 1024U;

Error field_error(const std::string& field, const std::string& message) {
    return {ErrorCode::BadArgs, "field '" + field + "' " + message};
}

Error optional_string(const json::Value& root,
                      const std::string& field,
                      std::string& out,
                      std::size_t max_bytes = 4096U) {
    const json::Value* value = root.get(field);
    if (value == nullptr) return ok_error();
    if (!value->is_string()) return field_error(field, "must be a string");
    if (value->string.size() > max_bytes) return field_error(field, "is too long");
    out = value->string;
    return ok_error();
}

Error required_string(const json::Value& root,
                      const std::string& field,
                      std::string& out,
                      std::size_t max_bytes = kMaxSessionText) {
    Error error = optional_string(root, field, out, max_bytes);
    if (!error.ok()) return error;
    out = ascii_trim(out);
    if (out.empty()) return field_error(field, "must be a non-empty string");
    return ok_error();
}

Error reject_unknown(const json::Value& root,
                     const std::set<std::string>& allowed) {
    for (const auto& entry : root.object) {
        if (allowed.count(entry.first) == 0)
            return field_error(entry.first, "is not supported for an interactive session");
    }
    return ok_error();
}

bool known_provider(const std::string& name) {
    const std::string canonical = provider::canonical_profile_name(name);
    for (const provider::Profile& profile : provider::built_in_profiles()) {
        if (profile.name == canonical) return true;
    }
    return false;
}

Error public_context_error(Error error) {
    if (error.code == ErrorCode::FileRead || error.code == ErrorCode::FileWrite)
        return {error.code, "server-side provider configuration could not be read"};
    return error;
}

bool path_within(const std::filesystem::path& root,
                 const std::filesystem::path& path) {
    auto root_it = root.begin();
    auto path_it = path.begin();
    for (; root_it != root.end(); ++root_it, ++path_it) {
        if (path_it == path.end() || *root_it != *path_it) return false;
    }
    return true;
}

std::string task_mode_name(agent::AgentTaskMode mode) {
    return mode == agent::AgentTaskMode::Plan ? "plan" : "act";
}

}  // namespace

InteractiveSession::InteractiveSession(std::string id,
                                       std::string workspace,
                                       provider::RequestContext context,
                                       agent::PermissionMode permission_mode,
                                       agent::AgentTaskMode task_mode,
                                       bool allow_yolo,
                                       std::size_t max_events)
    : id_(std::move(id)),
      workspace_(std::move(workspace)),
      created_at_(server_timestamp()),
      context_(std::move(context)),
      permission_mode_(permission_mode),
      task_mode_(task_mode),
      allow_yolo_(allow_yolo),
      updated_at_(created_at_),
      controller_(std::make_shared<agent::AgentController>()),
      events_(id_, max_events, Limits::event_bytes_per_job) {
    event_pump_.start([this](runtime::CancellationToken token) {
        while (!token.cancelled()) {
            agent::AgentSurfaceEvent event;
            if (controller_->events().wait_pop_for(event, std::chrono::milliseconds(100)))
                consume_event(event);
        }
    });
    publish("session_created", snapshot_json());
}

InteractiveSession::~InteractiveSession() { close(); }

std::string InteractiveSession::safe_error(const Error& error) const {
    if (error.code == ErrorCode::FileRead || error.code == ErrorCode::FileWrite ||
        error.code == ErrorCode::FileLock)
        return "server-side session state could not be accessed";
    return redact_secrets(error.message, {context_.api_key});
}

void InteractiveSession::publish(const std::string& type,
                                 const std::string& data_json,
                                 const std::string& turn_id) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        updated_at_ = server_timestamp();
    }
    events_.publish(type, data_json, id_, turn_id);
}

void InteractiveSession::start_preparation() {
    preparation_job_.start([this](runtime::CancellationToken token) {
        provider::RequestContext context;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            context = context_;
        }
        Error error = app::choose_default_model(context);
        if (error.ok()) {
            std::lock_guard<std::mutex> lock(mutex_);
            context_ = context;
        }
        if (error.ok()) {
            agent::SessionRuntimeOptions options;
            options.workspace = workspace_;
            options.task_mode = task_mode_;
            options.allow_network = true;
            options.interactive = true;
            options.enable_session_db = true;
            options.enable_agent_log = context.options.agent_log_enabled;
            options.security_review_log_keep_runs = context.options.security_review_log_keep_runs;
            options.trusted_prompt_dir = context.options.trusted_prompt_dir;
            options.max_source_code_file_size = context.options.max_source_code_file_size;
            options.history_backup.enabled = context.options.agent_history_backup_enabled;
            options.history_backup.max_bytes = context.options.agent_history_backup_max_bytes;
            options.history_backup.ttl_days = context.options.agent_history_backup_ttl_days;
            options.auto_compact = context.options.agent_auto_compact;
            options.compact_strategy = context.options.agent_compact_strategy;
            options.compact_limit = context.options.agent_compact_limit;
            options.max_agent_turns = context.options.agent_max_turns;
            options.index_mode = context.options.disable_indexing
                                     ? agent::SessionRuntimeOptions::IndexMode::Disabled
                                     : agent::SessionRuntimeOptions::IndexMode::UseExistingLazy;
            options.show_command_output = context.options.agent_show_command_output;
            options.fetch_options.connect_timeout_seconds = context.options.connect_timeout_seconds;
            options.fetch_options.timeout_seconds = context.options.timeout_seconds > 0
                                                       ? context.options.timeout_seconds : 30;
            options.fetch_options.max_bytes = context.options.max_fetch_bytes;
            options.fetch_options.proxy = context.options.proxy;
            options.fetch_options.insecure_tls = context.options.insecure_tls;
            options.fetch_options.trace_http = context.options.trace_http;
            options.fetch_options.allow_private = context.options.allow_private_url_fetch;
            options.search_options = search::options_for(context.options);
            options.permission_mode = permission_mode_;
            options.allow_yolo = allow_yolo_;
            options.on_prepare_progress = [this](const agent::PreparationProgress& progress) {
                publish("preparing", "{\"completed\":" +
                        std::string(progress.completed ? "true" : "false") + "}");
            };
            const std::shared_ptr<agent::ApprovalGate> gate = controller_->approval_gate();
            options.on_guard_ask = [gate](const agent::GuardApprovalRequest& request,
                                          runtime::CancellationToken cancellation) {
                return gate->request(request, cancellation);
            };
            error = controller_->runtime()->prepare(context, token, {}, std::move(options));
            if (error.ok()) {
                (void)controller_->runtime()->update_project_settings(context);
                controller_->runtime()->begin_background_index_freshness();
            }
        }
        bool closed = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            closed = closed_;
            if (!closed_) status_ = error.ok() ? "ready" : "error";
            updated_at_ = server_timestamp();
        }
        if (closed) return;
        if (error.ok()) {
            publish("ready", snapshot_json());
        } else {
            publish("session_error", "{\"message\":" + json::quote(safe_error(error)) + "}");
        }
    });
}

void InteractiveSession::consume_event(const agent::AgentSurfaceEvent& event) {
    std::string turn_id;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_) return;
        turn_id = active_turn_id_;
    }
    if (event.type == agent::AgentSurfaceEvent::Type::GuardApproval) {
        std::string approval_id;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            approval_id = id_ + "_approval_" + std::to_string(next_approval_++);
            pending_approval_id_ = approval_id;
            pending_review_path_ = event.guard_review_path;
            pending_tool_name_ = event.guard_tool_name;
            pending_command_preview_ = event.guard_command_preview;
            pending_rule_id_ = event.guard_rule_id;
            pending_message_ = event.guard_message;
            status_ = "waiting_guard";
            updated_at_ = server_timestamp();
        }
        publish("approval_required",
                "{\"approval_id\":" + json::quote(approval_id) +
                    ",\"tool\":" + json::quote(event.guard_tool_name) +
                    ",\"command_preview\":" + json::quote(event.guard_command_preview) +
                    ",\"rule_id\":" + json::quote(event.guard_rule_id) +
                    ",\"message\":" + json::quote(event.guard_message) +
                    ",\"review_file\":" +
                    (event.guard_review_path.empty() ? std::string("null")
                                                      : json::quote(event.guard_review_path)) + "}",
                turn_id);
        return;
    }
    if (event.type == agent::AgentSurfaceEvent::Type::TurnDone ||
        event.type == agent::AgentSurfaceEvent::Type::TurnError) {
        const bool cancelled = event.error.code == ErrorCode::Cancelled;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            active_turn_id_.clear();
            pending_approval_id_.clear();
            pending_review_path_.clear();
            status_ = closed_ ? "closed" : "ready";
            updated_at_ = server_timestamp();
        }
        if (event.type == agent::AgentSurfaceEvent::Type::TurnDone) {
            publish("turn_completed",
                    "{\"content\":" + json::quote(event.agent_final_text) +
                        ",\"turns\":" + std::to_string(event.agent_turns) +
                        ",\"tool_calls\":" + std::to_string(event.agent_tool_calls) +
                        ",\"failed_tool_calls\":" + std::to_string(event.agent_failed_tool_calls) +
                        ",\"status\":" + json::quote(cancelled ? "cancelled" : "succeeded") + "}",
                    turn_id);
        } else {
            publish("turn_failed",
                    "{\"code\":" + json::quote(error_code_name(event.error.code)) +
                        ",\"message\":" + json::quote(safe_error(event.error)) + "}",
                    turn_id);
        }
        return;
    }
}

std::string InteractiveSession::snapshot_json() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string result = "{\"id\":" + json::quote(id_) +
                         ",\"kind\":\"agent\"" +
                         ",\"status\":" + json::quote(status_) +
                         ",\"created_at\":" + json::quote(created_at_) +
                         ",\"updated_at\":" + json::quote(updated_at_) +
                         ",\"provider\":" + json::quote(context_.profile.name) +
                         ",\"model\":" + json::quote(context_.options.model) +
                         ",\"task_mode\":" + json::quote(task_mode_name(task_mode_)) +
                         ",\"permission_mode\":" + json::quote(agent::permission_mode_name(permission_mode_));
    if (!active_turn_id_.empty()) result += ",\"turn_id\":" + json::quote(active_turn_id_);
    else result += ",\"turn_id\":null";
    if (!pending_approval_id_.empty()) {
        result += ",\"approval\":{\"id\":" + json::quote(pending_approval_id_) +
                  ",\"tool\":" + json::quote(pending_tool_name_) +
                  ",\"command_preview\":" + json::quote(pending_command_preview_) +
                  ",\"rule_id\":" + json::quote(pending_rule_id_) +
                  ",\"message\":" + json::quote(pending_message_) +
                  ",\"review_file\":" +
                  (pending_review_path_.empty() ? std::string("null")
                                                : json::quote(pending_review_path_)) + "}";
    } else {
        result += ",\"approval\":null";
    }
    return result + "}";
}

Error InteractiveSession::start_turn(const std::string& body, std::string& turn_id) {
    const json::ParseResult parsed = json::parse(body);
    if (!parsed.error.ok()) return {ErrorCode::JsonParse, "turn body is not valid JSON"};
    if (!parsed.value.is_object()) return {ErrorCode::BadArgs, "turn body must be a JSON object"};
    Error error = reject_unknown(parsed.value, {"text"});
    if (!error.ok()) return error;
    std::string text;
    error = required_string(parsed.value, "text", text);
    if (!error.ok()) return error;

    provider::RequestContext context;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_) return {ErrorCode::Cancelled, "session is closed"};
        if (status_ == "preparing") return {ErrorCode::FileLock, "session is still preparing"};
        if (status_ == "error") return {ErrorCode::Internal, "session preparation failed"};
        if (!active_turn_id_.empty() || controller_->turn_running())
            return {ErrorCode::FileLock, "an interactive turn is already active"};
        if (!controller_->prepared()) return {ErrorCode::FileLock, "session is not ready"};
        turn_id = id_ + "_turn_" + std::to_string(next_turn_++);
        active_turn_id_ = turn_id;
        status_ = "running";
        context = context_;
        updated_at_ = server_timestamp();
    }
    publish("turn_started", "{\"text\":" + json::quote(text) + "}", turn_id);
    const bool started = controller_->start_turn(
        [this, context = std::move(context), text = std::move(text), turn_id](runtime::CancellationToken token) mutable {
            const auto progress = [this, turn_id](const std::string& line) {
                publish("progress", "{\"text\":" + json::quote(line) + "}", turn_id);
            };
            agent::SessionTurnResult result = controller_->runtime()->run_user_turn(
                context, text, token, {}, progress, {});
            agent::AgentSurfaceEvent event;
            event.type = result.error.ok() ? agent::AgentSurfaceEvent::Type::TurnDone
                                           : agent::AgentSurfaceEvent::Type::TurnError;
            event.error = result.error;
            event.agent_turn = true;
            event.agent_final_text = std::move(result.final_text);
            event.agent_turns = result.turns;
            event.agent_tool_calls = result.tool_calls;
            event.agent_failed_tool_calls = result.failed_tool_calls;
            event.agent_stream_output_tokens = result.stream_output_tokens;
            event.agent_stream_decode_ms = result.stream_decode_ms;
            event.agent_stream_tokens_estimated = result.stream_tokens_estimated;
            return event;
        });
    if (!started) {
        std::lock_guard<std::mutex> lock(mutex_);
        active_turn_id_.clear();
        status_ = "ready";
        return {ErrorCode::FileLock, "could not start the interactive turn"};
    }
    return ok_error();
}

Error InteractiveSession::cancel_turn(const std::string& turn_id) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (active_turn_id_ != turn_id || turn_id.empty())
            return {ErrorCode::FileRead, "interactive turn was not found"};
    }
    controller_->cancel_turn();
    publish("turn_cancel_requested", "{}", turn_id);
    return ok_error();
}

Error InteractiveSession::resolve_approval(const std::string& approval_id,
                                           const std::string& decision) {
    agent::GuardApprovalDecision parsed;
    if (decision == "allow") parsed = agent::GuardApprovalDecision::Allow;
    else if (decision == "deny") parsed = agent::GuardApprovalDecision::Deny;
    else if (decision == "cancelled" || decision == "cancel") parsed = agent::GuardApprovalDecision::Cancelled;
    else return {ErrorCode::BadArgs, "decision must be allow, deny, or cancelled"};
    std::string turn_id;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (pending_approval_id_ != approval_id || approval_id.empty())
            return {ErrorCode::FileRead, "approval was not found or is no longer pending"};
        turn_id = active_turn_id_;
        pending_approval_id_.clear();
        pending_review_path_.clear();
        status_ = "running";
    }
    controller_->approval_gate()->resolve(parsed);
    publish("approval_resolved", "{\"decision\":" + json::quote(decision) + "}", turn_id);
    return ok_error();
}

Error InteractiveSession::review_file(const std::string& approval_id,
                                       std::string& body) const {
    std::string review_path;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (pending_approval_id_ != approval_id || approval_id.empty())
            return {ErrorCode::FileRead, "approval was not found or is no longer pending"};
        review_path = pending_review_path_;
    }
    if (review_path.empty()) return {ErrorCode::FileRead, "approval has no review file"};
    const std::filesystem::path root = std::filesystem::u8path(workspace_);
    const std::filesystem::path requested = std::filesystem::u8path(review_path);
    if (requested.is_absolute()) return {ErrorCode::FileRead, "review file is outside the workspace"};
    std::error_code fs_error;
    const std::filesystem::path canonical_root = std::filesystem::canonical(root, fs_error);
    if (fs_error) return {ErrorCode::FileRead, "workspace could not be inspected"};
    const std::filesystem::path canonical_file = std::filesystem::canonical(root / requested, fs_error);
    if (fs_error || !path_within(canonical_root, canonical_file) ||
        !std::filesystem::is_regular_file(canonical_file, fs_error) || fs_error)
        return {ErrorCode::FileRead, "review file is outside the workspace"};
    const std::uintmax_t size = std::filesystem::file_size(canonical_file, fs_error);
    if (fs_error || size > kMaxReviewBytes) return {ErrorCode::FileRead, "review file is too large"};
    std::ifstream input(canonical_file, std::ios::binary);
    if (!input) return {ErrorCode::FileRead, "review file could not be read"};
    const std::string content((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    body = "{\"path\":" + json::quote(review_path) + ",\"content\":" + json::quote(content) + "}";
    return ok_error();
}

void InteractiveSession::close() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_) return;
        closed_ = true;
        status_ = "closed";
        updated_at_ = server_timestamp();
    }
    controller_->shutdown(true, "interactive session closed");
    preparation_job_.cancel();
    preparation_job_.join();
    event_pump_.cancel();
    event_pump_.join();
    events_.publish("session_closed", snapshot_json(), id_);
    events_.close();
}

SessionHub::SessionHub(cli::Options base_options,
                       std::string workspace,
                       std::size_t max_sessions,
                       bool allow_yolo)
    : base_options_(std::move(base_options)),
      workspace_(std::move(workspace)),
      max_sessions_(max_sessions == 0 ? 1U : max_sessions),
      allow_yolo_(allow_yolo) {
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

SessionHub::~SessionHub() { shutdown(); }

SessionCreateResult SessionHub::create(const std::string& body) {
    const json::ParseResult parsed = json::parse(body);
    if (!parsed.error.ok()) return {{}, {ErrorCode::JsonParse, "session body is not valid JSON"}};
    if (!parsed.value.is_object()) return {{}, {ErrorCode::BadArgs, "session body must be a JSON object"}};
    Error error = reject_unknown(parsed.value, {"kind", "provider", "model", "api", "permission_mode", "task_mode"});
    if (!error.ok()) return {{}, error};
    std::string kind = "agent";
    std::string provider_name;
    std::string model;
    std::string api;
    std::string permission_text = "smart";
    std::string task_text = "act";
    error = optional_string(parsed.value, "kind", kind, 32U);
    if (!error.ok()) return {{}, error};
    error = optional_string(parsed.value, "provider", provider_name, 128U);
    if (!error.ok()) return {{}, error};
    error = optional_string(parsed.value, "model", model, 512U);
    if (!error.ok()) return {{}, error};
    error = optional_string(parsed.value, "api", api, 32U);
    if (!error.ok()) return {{}, error};
    error = optional_string(parsed.value, "permission_mode", permission_text, 32U);
    if (!error.ok()) return {{}, error};
    error = optional_string(parsed.value, "task_mode", task_text, 32U);
    if (!error.ok()) return {{}, error};
    if (ascii_lower(ascii_trim(kind)) != "agent")
        return {{}, {ErrorCode::UnsupportedFeature, "only kind 'agent' sessions are supported"}};
    agent::PermissionMode permission_mode;
    if (!agent::parse_permission_mode(ascii_lower(ascii_trim(permission_text)), permission_mode))
        return {{}, field_error("permission_mode", "must be confirm, smart, or yolo")};
    if (permission_mode == agent::PermissionMode::Yolo && !allow_yolo_)
        return {{}, {ErrorCode::UnsupportedFeature,
                     "remote Yolo requires the server startup option --allow-remote-yolo"}};
    agent::AgentTaskMode task_mode = agent::AgentTaskMode::Act;
    const std::string normalized_task = ascii_lower(ascii_trim(task_text));
    if (normalized_task == "plan") task_mode = agent::AgentTaskMode::Plan;
    else if (normalized_task != "act") return {{}, field_error("task_mode", "must be act or plan")};
    if (!provider_name.empty() && !known_provider(provider_name))
        return {{}, field_error("provider", "names an unknown configured provider profile")};
    if (!api.empty() && api != "chat" && api != "responses")
        return {{}, field_error("api", "must be chat or responses")};

    cli::Options options = base_options_;
    if (!provider_name.empty()) {
        provider::apply_provider_target(options, provider_name);
        options.provider_explicit = true;
    }
    if (!model.empty()) {
        options.model = model;
        options.model_explicit = true;
    }
    if (!api.empty()) {
        options.api = api;
        options.api_explicit = true;
    }
    options.agent = true;
    options.agent_run = false;
    options.agent_plan = task_mode == agent::AgentTaskMode::Plan;
    options.image = false;
    provider::ContextResult built = provider::build_context(options);
    if (!built.error.ok()) return {{}, public_context_error(std::move(built.error))};
    built.context.routing_session_id = provider::new_routing_session_id();

    std::shared_ptr<InteractiveSession> session;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) return {{}, {ErrorCode::Cancelled, "server is stopping"}};
        if (sessions_.size() >= max_sessions_)
            return {{}, {ErrorCode::RateLimit, "the interactive session limit is full"}};
        const std::string id = "session_" + std::to_string(next_session_id_++);
        session = std::shared_ptr<InteractiveSession>(new InteractiveSession(
            id, workspace_, std::move(built.context), permission_mode, task_mode,
            allow_yolo_,
            Limits::events_per_job));
        sessions_.emplace(id, session);
    }
    session->start_preparation();
    return {std::move(session), ok_error()};
}

std::shared_ptr<InteractiveSession> SessionHub::find(const std::string& id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = sessions_.find(id);
    return it == sessions_.end() ? std::shared_ptr<InteractiveSession>() : it->second;
}

std::string SessionHub::list_json() const {
    std::vector<std::shared_ptr<InteractiveSession>> sessions;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& entry : sessions_) sessions.push_back(entry.second);
    }
    std::string body = "[";
    for (std::size_t i = 0; i < sessions.size(); ++i) {
        if (i) body += ',';
        body += sessions[i]->snapshot_json();
    }
    return body + "]";
}

bool SessionHub::erase(const std::string& id) {
    std::shared_ptr<InteractiveSession> session;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = sessions_.find(id);
        if (it == sessions_.end()) return false;
        session = std::move(it->second);
        sessions_.erase(it);
    }
    session->close();
    return true;
}

void SessionHub::shutdown() {
    std::vector<std::shared_ptr<InteractiveSession>> sessions;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) return;
        stopping_ = true;
        for (auto& entry : sessions_) sessions.push_back(std::move(entry.second));
        sessions_.clear();
    }
    for (const auto& session : sessions) session->close();
}

std::size_t SessionHub::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return sessions_.size();
}

}  // namespace ainiux::server
