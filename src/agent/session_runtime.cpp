#include "agent/session_runtime.hpp"

#include "agent/project_scripts.hpp"
#include "runtime/subprocess.hpp"
#include "mcp/registry.hpp"
#include "mcp/arg_rewrite.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <utility>

#include "agent/agent_loop.hpp"
#include "agent/compact.hpp"
#include "agent/goal.hpp"
#include "agent/project_root.hpp"
#include "agent/project_settings.hpp"
#include "agent/reasoning_preview.hpp"
#include "agent/tool_display.hpp"
#include "chat/settings.hpp"
#include "config/model_catalog.hpp"
#include "security/redact.hpp"
#include "platform/environment.hpp"

namespace ainiux::agent {

const char* preparation_phase_name(PreparationPhase phase) {
    switch (phase) {
        case PreparationPhase::IndexProbe:
            return "index probe";
        case PreparationPhase::ToolSetup:
            return "tool setup";
        case PreparationPhase::SessionDatabase:
            return "session DB";
        case PreparationPhase::History:
            return "history";
        case PreparationPhase::ProjectInstructions:
            return "project instructions";
    }
    return "preparation";
}
namespace {

json::Value log_object() {
    json::Value value;
    value.type = json::Value::Type::Object;
    return value;
}
json::Value log_string(const std::string& text) {
    json::Value value;
    value.type = json::Value::Type::String;
    value.string = text;
    return value;
}
json::Value log_number(double number) {
    json::Value value;
    value.type = json::Value::Type::Number;
    value.number = number;
    return value;
}
json::Value log_bool(bool boolean) {
    json::Value value;
    value.type = json::Value::Type::Bool;
    value.boolean = boolean;
    return value;
}

std::vector<std::string> configured_secrets(const provider::RequestContext& context) {
    std::vector<std::string> secrets;
    if (!context.api_key.empty()) secrets.push_back(context.api_key);
    if (!context.options.key.empty()) secrets.push_back(context.options.key);
    for (const std::string& header : context.headers) {
        const std::size_t colon = header.find(':');
        if (colon == std::string::npos) continue;
        if (is_sensitive_header_name(ascii_trim(header.substr(0, colon)))) {
            const std::string value = ascii_trim(header.substr(colon + 1));
            if (!value.empty()) secrets.push_back(value);
        }
    }
    std::sort(secrets.begin(), secrets.end());
    secrets.erase(std::unique(secrets.begin(), secrets.end()), secrets.end());
    return secrets;
}

std::vector<std::string> known_tool_names(const ReadToolRegistry& tools) {
    std::vector<std::string> names;
    for (const provider::FunctionDefinition& definition : tools.definitions())
        names.push_back(definition.name);
    return names;
}

// One-line history preface before the compact totals table after /index-code.
std::string format_index_completion_intro(long long elapsed_ms) {
    std::ostringstream out;
    out << "Code indexing completed in " << std::fixed << std::setprecision(2)
        << (static_cast<double>(std::max(0LL, elapsed_ms)) / 1000.0)
        << " seconds. Here is the summary:";
    return out.str();
}

Error default_compaction_summary_call(
    const provider::RequestContext& context,
    const std::vector<provider::Message>& messages,
    int max_output_tokens,
    runtime::CancellationToken cancellation,
    std::string& summary) {
    provider::RequestContext request = context;
    request.options.stream = false;
    request.options.max_output_tokens = max_output_tokens;
    request.options.has_max_output_tokens = true;
    request.suppress_streaming_reasoning = true;
    if (const ModelCapability* capability =
            provider::matched_model_capability(request);
        capability != nullptr) {
        request.options.reasoning =
            compaction_summary_reasoning(capability->reasoning_options);
    } else {
        request.options.reasoning = ReasoningSelection::automatic();
    }
    request.options.reasoning_explicit = true;
    provider::ChatResult result;
    Error error = provider::send_chat_messages(
        request, messages, [](const std::string&) { return ok_error(); }, result,
        cancellation);
    if (!error.ok()) return error;
    summary = result.content;
    return ok_error();
}

bool context_length_error(const Error& error) {
    if (error.ok()) return false;
    const std::string lower = ascii_lower(error.message);
    return (error.code == ErrorCode::HttpStatus &&
            lower.find("413") != std::string::npos) ||
           lower.find("context length") != std::string::npos ||
           lower.find("context_length") != std::string::npos ||
           lower.find("maximum context") != std::string::npos ||
           lower.find("too many tokens") != std::string::npos ||
           lower.find("request is too large") != std::string::npos;
}

std::vector<std::string> utf8_chunks(const std::string& source,
                                     long long token_budget) {
    const std::size_t byte_budget = static_cast<std::size_t>(
        std::max<long long>(256, token_budget * 4));
    std::vector<std::string> chunks;
    std::size_t begin = 0;
    while (begin < source.size()) {
        std::size_t end = std::min(source.size(), begin + byte_budget);
        while (end > begin && end < source.size() &&
               (static_cast<unsigned char>(source[end]) & 0xc0U) == 0x80U)
            --end;
        if (end == begin) end = std::min(source.size(), begin + byte_budget);
        chunks.push_back(source.substr(begin, end - begin));
        begin = end;
    }
    return chunks;
}

std::string utf8_bounded_extract(const std::string& text,
                                 long long token_budget) {
    const std::size_t byte_budget = static_cast<std::size_t>(
        std::max<long long>(64, token_budget * 4));
    if (text.size() <= byte_budget) return text;
    std::size_t end = byte_budget > 4 ? byte_budget - 4 : byte_budget;
    while (end > 0 &&
           (static_cast<unsigned char>(text[end]) & 0xc0U) == 0x80U)
        --end;
    return text.substr(0, end) + " ...";
}

}  // namespace

void accumulate_agent_token_usage(const provider::ChatResult& metrics,
                                  AgentTokenUsage& usage,
                                  long long estimated_input_tokens,
                                  long long estimated_output_tokens) {
    auto add = [](long long& total, long long value) {
        if (value < 0) return;
        total = total > std::numeric_limits<long long>::max() - value
                    ? std::numeric_limits<long long>::max()
                    : total + value;
    };
    const bool has_input = metrics.prompt_tokens >= 0 || estimated_input_tokens > 0;
    const bool has_output =
        (metrics.completion_tokens >= 0 &&
         (!metrics.completion_tokens_estimated &&
          !metrics.usage_json.empty() && metrics.usage_json != "null")) ||
        estimated_output_tokens > 0 || metrics.completion_tokens_estimated;
    if (!has_input && !has_output) return;

    ++usage.reported_rounds;
    if (metrics.prompt_tokens >= 0) {
        add(usage.input_tokens, metrics.prompt_tokens);
    } else {
        add(usage.input_tokens, estimated_input_tokens);
        usage.input_estimated = true;
    }
    add(usage.fresh_input_tokens, metrics.fresh_prompt_tokens);
    add(usage.cache_read_tokens, metrics.cache_read_tokens);
    add(usage.cache_write_tokens, metrics.cache_write_tokens);
    if (!metrics.completion_tokens_estimated &&
        !metrics.usage_json.empty() && metrics.usage_json != "null") {
        add(usage.output_tokens, metrics.completion_tokens);
    } else {
        add(usage.output_tokens, estimated_output_tokens);
        usage.output_estimated = true;
    }
}

bool accumulate_agent_stream_decode(const provider::ChatResult& metrics,
                                    long long estimated_output_tokens,
                                    bool stream,
                                    long long& tokens,
                                    long long& decode_ms,
                                    bool& estimated) {
    long long first_ms = metrics.ttft_ms;
    if (first_ms < 0 && stream && metrics.first_body_ms >= 0)
        first_ms = metrics.first_body_ms;
    if (!stream || first_ms < 0 || metrics.total_ms <= first_ms) {
        return false;
    }
    long long round_tokens = 0;
    bool round_estimated = false;
    if (!metrics.completion_tokens_estimated &&
        !metrics.usage_json.empty() && metrics.usage_json != "null" &&
        metrics.completion_tokens > 0) {
        round_tokens = metrics.completion_tokens;
    } else if (estimated_output_tokens > 0) {
        round_tokens = estimated_output_tokens;
        round_estimated = true;
    } else if (metrics.completion_tokens > 0) {
        round_tokens = metrics.completion_tokens;
        round_estimated = metrics.completion_tokens_estimated;
    } else {
        return false;
    }
    auto add = [](long long& total, long long value) {
        if (value <= 0) return;
        total = total > std::numeric_limits<long long>::max() - value
                    ? std::numeric_limits<long long>::max()
                    : total + value;
    };
    add(tokens, round_tokens);
    add(decode_ms, metrics.total_ms - first_ms);
    if (round_estimated) estimated = true;
    return true;
}

double agent_stream_tokens_per_second(long long tokens, long long decode_ms) {
    if (tokens <= 0 || decode_ms <= 0) return -1.0;
    return static_cast<double>(tokens) * 1000.0 / static_cast<double>(decode_ms);
}

bool AgentSessionRuntime::is_interrupted(runtime::CancellationToken cancellation,
                                         const std::function<bool()>& interrupted) const {
    if (cancellation.cancelled()) return true;
    if (interrupted && interrupted()) return true;
    return false;
}

long long AgentSessionRuntime::estimated_request_tokens() const {
    return cached_request_tokens_.load(std::memory_order_relaxed);
}

long long AgentSessionRuntime::last_nonzero_request_tokens() const {
    return last_nonzero_request_tokens_.load(std::memory_order_relaxed);
}

long long AgentSessionRuntime::in_flight_generation_tokens() const {
    return in_flight_generation_tokens_.load(std::memory_order_relaxed);
}

void AgentSessionRuntime::publish_in_flight_generation_tokens(long long tokens) {
    if (tokens < 0) tokens = 0;
    in_flight_generation_tokens_.store(tokens, std::memory_order_relaxed);
}

void AgentSessionRuntime::clear_in_flight_generation_tokens() {
    in_flight_generation_tokens_.store(0, std::memory_order_relaxed);
}

Error AgentSessionRuntime::update_project_settings(
    const provider::RequestContext& context) {
    if (!prepared_) return {ErrorCode::Internal, "agent runtime is not prepared"};
    if (!session_store_.is_open() || context.profile.offline) return ok_error();

    // Provider selection may change before the first turn. No conversation has
    // been encoded yet, so it is safe to select the matching tool protocol.
    if (!conversation_seeded_) {
        state_.protocol =
            default_tool_protocol(provider::capabilities_for(context).tool_calls);
    }

    AgentProjectRecord project;
    project.status = "idle";
    project.workspace = options_.workspace;
    Error error = session_store_.open_project(project);
    if (!error.ok()) return error;
    project.provider = context.profile.name;
    project.model = context.options.model;
    project.api = context.api_kind == provider::ApiKind::Responses ? "responses" : "chat";
    project.protocol = state_.protocol == ToolProtocol::Xml ? "xml" : "native";
    project.base_url = context.base_url;
    error = settings_json_with_permission_mode(
        chat::settings_json_from_options(context.options), permission_mode_,
        project.settings_json);
    if (!error.ok()) return error;
    error = write_session_settings(project);
    if (!error.ok()) return error;
    project.workspace = options_.workspace;
    return session_store_.update_project_meta(project);
}

long long AgentSessionRuntime::estimate_seed_overhead_tokens() const {
    long long total = 0;
    // system prompt (agent_prompt.md + protocol appendix)
    total += estimate_tokens_from_text("system");
    total += estimate_tokens_from_text(prompts_.agent_system_prompt(state_.protocol));
    total += 4;
    // optional AGENTS.md (user-role, untrusted project data)
    if (!agents_md_.injection_text.empty()) {
        total += estimate_tokens_from_text("user");
        total += estimate_tokens_from_text(agents_md_.injection_text);
        total += 4;
    }
    // Act/Plan mode control (user-role)
    {
        std::vector<std::string> script_names;
        (void)list_project_scripts(options_.workspace, script_names);
        const std::string mode_control = agent_task_mode_control(task_mode_, script_names);
        total += estimate_tokens_from_text("user");
        total += estimate_tokens_from_text(mode_control);
        total += 4;
    }
    // Native tool schemas are part of every tool request (not XML protocol).
    if (state_.protocol != ToolProtocol::Xml) {
        for (const provider::FunctionDefinition& definition : tools_.definitions()) {
            total += estimate_tokens_from_text(definition.name);
            total += estimate_tokens_from_text(definition.description);
            total += estimate_tokens_from_text(definition.parameters_json);
            total += 8;
        }
    }
    return total;
}

long long AgentSessionRuntime::estimate_compact_tokens_before(
    const std::vector<AgentMessageRecord>& stored) const {
    if (conversation_seeded_)
        return estimated_request_tokens();
    // Unseeded: compare compact projection against seed overhead plus the full
    // durable model-projection transcript. Idle chrome uses a smaller bounded
    // prior-session seed and must not drive this reduction check.
    return estimate_seed_overhead_tokens() +
           estimate_transcript_tokens(messages_after_seq(stored, context_reset_after_seq_));
}

std::vector<std::string> AgentSessionRuntime::context_load_notices() const {
    std::vector<std::string> lines;
    const long long prompt_tokens =
        estimate_tokens_from_text(prompts_.agent_system_prompt(state_.protocol));
    lines.push_back("agent_prompt.md loaded ~" + std::to_string(prompt_tokens) +
                    " tokens");
    if (!agents_md_.injection_text.empty()) {
        const long long agents_tokens =
            estimate_tokens_from_text(agents_md_.injection_text);
        lines.push_back("AGENTS.md loaded ~" + std::to_string(agents_tokens) +
                        " tokens");
    }
    long long tool_tokens = 0;
    std::size_t count = 0;
    for (const provider::FunctionDefinition& definition : tools_.definitions()) {
        tool_tokens += estimate_tokens_from_text(definition.name);
        tool_tokens += estimate_tokens_from_text(definition.description);
        tool_tokens += estimate_tokens_from_text(definition.parameters_json);
        tool_tokens += 8;
        ++count;
    }
    if (count > 0) {
        lines.push_back("tools loaded ~" + std::to_string(tool_tokens) +
                        " tokens");
    }
    return lines;
}

void AgentSessionRuntime::append_context_load_notices(
    std::vector<provider::Message>& history) const {
    if (visible_history_hidden()) return;
    const long long now = now_unix_ms();
    for (const std::string& line : context_load_notices()) {
        if (!history.empty() && history.back().role == "notice" &&
            history.back().content == line) {
            continue;
        }
        provider::Message notice{"notice", line};
        notice.created_at_ms = now;
        history.push_back(std::move(notice));
    }
}

void AgentSessionRuntime::apply_context_reset_filter(
    std::vector<AgentMessageRecord>& messages) const {
    messages = messages_after_seq(messages, context_reset_after_seq_);
}

void AgentSessionRuntime::apply_context_reset_filter(
    std::vector<AgentMessageRecord>& messages,
    std::vector<AgentToolEventRecord>& events) const {
    messages = messages_after_seq(messages, context_reset_after_seq_);
    events = tool_events_after_seq(events, context_reset_after_seq_);
}

Error AgentSessionRuntime::write_session_settings(AgentProjectRecord& project) const {
    Error error = settings_json_with_permission_mode(
        project.settings_json, permission_mode_, project.settings_json);
    if (!error.ok()) return error;
    error = settings_json_with_goal(project.settings_json, goal_, project.settings_json);
    if (!error.ok()) return error;
    return settings_json_with_context_reset_after_seq(
        project.settings_json, context_reset_after_seq_, project.settings_json);
}

Error AgentSessionRuntime::reset_model_context() {
    if (!prepared_)
        return {ErrorCode::Internal, "agent session runtime is not prepared"};
    bool expected = false;
    if (!operation_active_.compare_exchange_strong(expected, true))
        return {ErrorCode::BadArgs,
                "cannot reset context while an agent operation is active"};
    struct Release {
        std::atomic<bool>& active;
        ~Release() { active.store(false); }
    } release{operation_active_};

    long long cut = 0;
    if (session_store_.is_open()) {
        AgentMessageRecord last;
        bool found = false;
        Error error = session_store_.peek_last_message(last, found);
        if (!error.ok()) return error;
        if (found) cut = last.seq;
    }
    context_reset_after_seq_ = cut;
    display_min_seq_.store(0, std::memory_order_relaxed);

    const bool had_goal = goal_.status != GoalStatus::Cleared && !goal_.condition.empty();
    goal_.status = GoalStatus::Cleared;
    goal_.turns = 0;
    goal_.last_reason = "context reset";
    if (!had_goal) goal_.condition.clear();

    if (session_store_.is_open()) {
        AgentProjectRecord project;
        Error error = session_store_.open_project(project);
        if (!error.ok()) return error;
        error = write_session_settings(project);
        if (!error.ok()) return error;
        error = session_store_.update_project_meta(project);
        if (!error.ok()) return error;
        error = session_store_.append_message(
            "notice", "Context reset; transcript retained on disk");
        if (!error.ok()) return error;
    }

    conversation_ = provider::ToolConversation{};
    conversation_seeded_ = false;
    const long long baseline = estimate_seed_overhead_tokens();
    cached_request_tokens_.store(baseline, std::memory_order_relaxed);
    if (baseline > 0)
        last_nonzero_request_tokens_.store(baseline, std::memory_order_relaxed);
    return ok_error();
}

Error AgentSessionRuntime::hide_visible_history() {
    if (!prepared_)
        return {ErrorCode::Internal, "agent session runtime is not prepared"};
    long long cut = 0;
    if (session_store_.is_open()) {
        AgentMessageRecord last;
        bool found = false;
        Error error = session_store_.peek_last_message(last, found);
        if (!error.ok()) return error;
        if (found) cut = last.seq;
    }
    if (cut > 0) display_min_seq_.store(cut, std::memory_order_relaxed);
    return ok_error();
}

void AgentSessionRuntime::publish_request_token_estimate() {
    // Must only run on the agent worker (or while no concurrent turn is active).
    long long total = 0;
    for (const provider::Message& message : conversation_.messages) {
        total += estimate_tokens_from_text(message.role);
        total += estimate_tokens_from_text(message.content);
        for (const provider::ImageInput& image : message.images) {
            // Rough byte-based estimate; vision models often charge more per image.
            total += estimate_tokens_from_text(image.base64_data);
            total += estimate_tokens_from_text(image.mime_type);
            total += 16;
        }
        total += 4;
    }
    for (const std::string& item : conversation_.continuation_items_json) {
        total += estimate_tokens_from_text(item);
        total += 2;
    }
    // Tool schemas are part of every native tool request; include a coarse
    // estimate so idle chrome is not "system prompt only".
    if (state_.protocol != ToolProtocol::Xml) {
        for (const provider::FunctionDefinition& definition : tools_.definitions()) {
            total += estimate_tokens_from_text(definition.name);
            total += estimate_tokens_from_text(definition.description);
            total += estimate_tokens_from_text(definition.parameters_json);
            total += 8;
        }
    }
    cached_request_tokens_.store(total, std::memory_order_relaxed);
    if (total > 0)
        last_nonzero_request_tokens_.store(total, std::memory_order_relaxed);
}

void AgentSessionRuntime::rebuild_compacted_conversation(
    const CompactionPartition& partition, const std::string& checkpoint) {
    std::vector<std::string> script_names;
    (void)list_project_scripts(options_.workspace, script_names);
    seed_agent_conversation(conversation_, prompts_, task_mode_, state_.protocol, "",
                            agents_md_.injection_text, script_names);
    conversation_.messages.push_back(
        {"user", compaction_checkpoint_wrapper(checkpoint)});
    auto append_plain = [&](const CompactionLogicalItem& item) {
        if (item.role == "user" || item.role == "assistant") {
            conversation_.messages.push_back({item.role, item.content});
        } else if (item.role == "tool") {
            conversation_.messages.push_back(
                {"user", "[Retained agent tool activity]\n" + item.content});
        }
    };
    for (const CompactionLogicalItem& item : partition.head) append_plain(item);
    for (const CompactionLogicalItem& item : partition.tail) append_plain(item);
    // Opaque Responses/Chat continuation state is intentionally discarded at
    // the compaction boundary. Retained tool units are plain, protocol-safe context.
    conversation_.continuation_items_json.clear();
    conversation_seeded_ = true;
    publish_request_token_estimate();
}

SessionCompactionResult AgentSessionRuntime::compact_impl(
    const provider::RequestContext& context,
    CompactionReason reason,
    runtime::CancellationToken cancellation,
    std::optional<CompactionStrategy> strategy_override,
    bool forced_summary) {
    const auto started = std::chrono::steady_clock::now();
    SessionCompactionResult result;
    result.requested_strategy =
        strategy_override.value_or(options_.compact_strategy);
    result.applied_strategy = result.requested_strategy;
    if (!prepared_) {
        result.error = {ErrorCode::Internal, "agent session runtime is not prepared"};
        return result;
    }
    if (!session_store_.is_open()) {
        result.error = {ErrorCode::Internal,
                        "agent compaction requires an open project session DB"};
        return result;
    }
    if (cancellation.cancelled()) {
        result.error = {ErrorCode::Cancelled, "agent compaction cancelled"};
        return result;
    }

    AgentProjectRecord project;
    std::vector<AgentMessageRecord> stored;
    std::vector<AgentToolEventRecord> tool_events;
    result.error =
        session_store_.load_session(1, project, stored, tool_events);
    if (!result.error.ok()) return result;
    apply_context_reset_filter(stored, tool_events);

    const long long window = context.options.context_tokens > 0
                                 ? static_cast<long long>(
                                       context.options.context_tokens)
                                 : 0LL;
    result.tokens_before = estimate_compact_tokens_before(stored);
    const long long newest_seq =
        stored.empty() ? 0 : stored.back().seq;
    auto failed_result = [&]() {
        if (reason == CompactionReason::Automatic) {
            last_auto_compact_failure_ms_ = now_unix_ms();
            last_auto_compact_failure_seq_ = newest_seq;
        }
        return result;
    };
    if (reason == CompactionReason::Automatic && !forced_summary) {
        if (!options_.auto_compact) {
            result.error = ok_error();
            result.no_op = true;
            return result;
        }
        const long long now = now_unix_ms();
        if (last_auto_compact_failure_ms_ > 0 &&
            newest_seq <= last_auto_compact_failure_seq_ &&
            now - last_auto_compact_failure_ms_ < 60000) {
            result.error = ok_error();
            result.no_op = true;
            result.reason = "automatic compaction cooldown after a failed attempt";
            return result;
        }
        if (!should_auto_compact(options_.auto_compact, options_.compact_limit, window,
                                 estimated_request_tokens())) {
            result.error = ok_error();
            result.no_op = true;
            return result;
        }
    }

    const std::vector<CompactionLogicalItem> timeline =
        build_compaction_timeline(stored, tool_events);
    CompactionPartition partition =
        partition_compaction_timeline(timeline, window);
    if (partition.middle.empty()) {
        result.error = ok_error();
        result.no_op = true;
        result.tokens_after = result.tokens_before;
        result.notice =
            format_compaction_no_op_notice(result.tokens_after);
        return result;
    }
    // Deterministic pre-shrink of the compressible middle before fast/summary.
    pre_shrink_compaction_middle(partition.middle);
    if (partition.middle.empty()) {
        result.error = ok_error();
        result.no_op = true;
        result.tokens_after = result.tokens_before;
        result.notice =
            format_compaction_no_op_notice(result.tokens_after);
        return result;
    }
    const CompactionKeepList keep_list =
        harvest_compaction_keep_list(partition.middle);
    std::string first_head_carry;
    std::string summary_preamble;
    for (const CompactionLogicalItem& item : timeline) {
        if (item.role == "user") {
            summary_preamble = redact_secrets(item.content, secrets_);
            break;
        }
    }
    if (summary_preamble.empty() && !partition.head.empty())
        summary_preamble =
            redact_secrets(partition.head.front().content, secrets_);
    if (partition.prior_summary.empty() && !partition.head.empty()) {
        std::ostringstream carry;
        carry << "\n\nProtected Initial Context\n";
        for (const CompactionLogicalItem& item : partition.head) {
            carry << "[" << item.role << "] "
                  << (item.estimated_tokens > partition.tail_budget_tokens
                          ? utf8_bounded_extract(
                                item.content, partition.tail_budget_tokens)
                          : item.content)
                  << "\n";
        }
        first_head_carry = carry.str();
    }

    const long long trigger =
        window > 0
            ? (window *
                   effective_compact_limit_percent(options_.compact_limit, window) +
               99) /
                  100
            : 0;
    FastCompactionCandidate fast = build_fast_compaction_candidate(
        partition,
        window > 0 ? std::max<long long>(512, window * 10 / 100) : 1000,
        keep_list);
    auto has_oversized_protected = [&](const auto& items) {
        return std::any_of(
            items.begin(), items.end(), [&](const CompactionLogicalItem& item) {
                return item.estimated_tokens > partition.tail_budget_tokens;
            });
    };
    if (has_oversized_protected(partition.head) ||
        has_oversized_protected(partition.tail)) {
        fast.protected_content_truncated = true;
        if (result.requested_strategy == CompactionStrategy::Fast)
            result.reason =
                "oversized protected content used a bounded local extract";
    }
    std::string checkpoint = fast.checkpoint;
    CompactionStrategy applied = result.requested_strategy;
    bool use_model = result.requested_strategy == CompactionStrategy::Summary ||
                     forced_summary;
    if (result.requested_strategy == CompactionStrategy::Smart && !use_model) {
        use_model = smart_compaction_should_escalate(
            fast, window, trigger, partition.tail_budget_tokens, result.reason);
        if (use_model) applied = CompactionStrategy::Summary;
    }

    if (use_model) {
        auto move_oversized_to_middle = [&](auto& protected_items) {
            auto first = std::stable_partition(
                protected_items.begin(), protected_items.end(),
                [&](const CompactionLogicalItem& item) {
                    return item.estimated_tokens <=
                           partition.tail_budget_tokens;
                });
            partition.middle.insert(partition.middle.end(), first,
                                    protected_items.end());
            protected_items.erase(first, protected_items.end());
        };
        move_oversized_to_middle(partition.head);
        move_oversized_to_middle(partition.tail);
        std::stable_sort(
            partition.middle.begin(), partition.middle.end(),
            [](const CompactionLogicalItem& left,
               const CompactionLogicalItem& right) {
                return left.seq < right.seq;
            });
    } else {
        // Fast remains model-free: retain a bounded deterministic UTF-8 extract
        // in place of any protected item that alone exceeds the tail budget.
        auto bound_protected = [&](auto& items) {
            for (CompactionLogicalItem& item : items) {
                if (item.estimated_tokens <= partition.tail_budget_tokens)
                    continue;
                item.content = utf8_bounded_extract(
                    item.content, partition.tail_budget_tokens);
                item.estimated_tokens = estimate_tokens_from_text(item.content);
            }
        };
        bound_protected(partition.head);
        bound_protected(partition.tail);
    }

    bool model_fallback_to_fast = false;
    if (use_model) {
        applied = CompactionStrategy::Summary;
        const CompactionSummaryCall summary_call =
            options_.summary_call ? options_.summary_call
                                  : default_compaction_summary_call;
        std::string source =
            redact_secrets(render_compaction_source(partition), secrets_);
        const std::string system =
            compaction_summary_schema_prompt(summary_preamble);
        const std::string user_guidance =
            compaction_summary_user_guidance(keep_list);
        const long long input_budget = compaction_summary_input_budget(window);
        std::vector<std::string> chunks = utf8_chunks(source, input_budget);
        if (chunks.empty()) chunks.push_back(source);
        std::vector<std::string> summaries;
        bool model_ok = true;
        const auto model_deadline =
            started + std::chrono::milliseconds(
                          compaction_summary_model_timeout_ms());
        auto model_timed_out = [&]() {
            return std::chrono::steady_clock::now() >= model_deadline;
        };
        for (const std::string& chunk : chunks) {
            if (cancellation.cancelled()) {
                result.error = {ErrorCode::Cancelled,
                                "agent compaction cancelled"};
                return result;
            }
            if (model_timed_out()) {
                model_ok = false;
                result.reason = "summary model path exceeded wall-clock budget";
                break;
            }
            std::string summary;
            const int output_budget = static_cast<int>(
                compaction_summary_output_budget(
                    estimate_tokens_from_text(chunk), window));
            result.error = summary_call(
                context,
                {{"system", system},
                 {"user", user_guidance + "\nChronological history to summarize:\n" +
                              chunk}},
                output_budget, cancellation, summary);
            if (!result.error.ok()) {
                if (result.error.code == ErrorCode::Cancelled) return result;
                model_ok = false;
                result.reason = "summary model call failed; falling back to fast";
                break;
            }
            summary = ascii_trim(redact_secrets(summary, secrets_));
            if (summary.empty()) {
                model_ok = false;
                result.reason =
                    "summary model returned an empty checkpoint; falling back to fast";
                break;
            }
            summaries.push_back(std::move(summary));
        }
        // At most one consolidation pass; further growth falls back to fast.
        if (model_ok && summaries.size() > 1) {
            if (cancellation.cancelled()) {
                result.error = {ErrorCode::Cancelled,
                                "agent compaction cancelled"};
                return result;
            }
            if (model_timed_out()) {
                model_ok = false;
                result.reason = "summary model path exceeded wall-clock budget";
            } else {
                std::ostringstream combined;
                for (const std::string& summary : summaries)
                    combined << "[Chunk checkpoint]\n" << summary << "\n";
                std::vector<std::string> consolidation_chunks =
                    utf8_chunks(combined.str(), input_budget);
                std::vector<std::string> next;
                for (const std::string& chunk : consolidation_chunks) {
                    if (cancellation.cancelled()) {
                        result.error = {ErrorCode::Cancelled,
                                        "agent compaction cancelled"};
                        return result;
                    }
                    if (model_timed_out()) {
                        model_ok = false;
                        result.reason =
                            "summary model path exceeded wall-clock budget";
                        break;
                    }
                    std::string summary;
                    result.error = summary_call(
                        context,
                        {{"system", system},
                         {"user",
                          user_guidance +
                              "\nConsolidate these chunk checkpoints without losing "
                              "unfinished work or verified facts:\n" +
                              chunk}},
                        static_cast<int>(compaction_summary_output_budget(
                            estimate_tokens_from_text(chunk), window)),
                        cancellation, summary);
                    if (!result.error.ok()) {
                        if (result.error.code == ErrorCode::Cancelled)
                            return result;
                        model_ok = false;
                        result.reason =
                            "summary consolidation failed; falling back to fast";
                        break;
                    }
                    summary = ascii_trim(redact_secrets(summary, secrets_));
                    if (summary.empty()) {
                        model_ok = false;
                        result.reason =
                            "summary consolidation returned empty; falling back to fast";
                        break;
                    }
                    next.push_back(std::move(summary));
                }
                if (model_ok) {
                    if (next.size() != 1) {
                        model_ok = false;
                        result.reason =
                            "summary could not consolidate to one checkpoint; "
                            "falling back to fast";
                    } else {
                        summaries = std::move(next);
                    }
                }
            }
        }
        if (model_ok && !summaries.empty()) {
            checkpoint = summaries.front();
            result.error = ok_error();
        } else {
            model_fallback_to_fast = true;
            checkpoint = fast.checkpoint;
            applied = CompactionStrategy::Fast;
            if (result.reason.empty())
                result.reason = "summary fallback to fast checkpoint";
            result.error = ok_error();
        }
    }

    if (cancellation.cancelled()) {
        result.error = {ErrorCode::Cancelled, "agent compaction cancelled"};
        return result;
    }

    auto try_project = [&](const std::string& candidate_checkpoint,
                           CompactionStrategy candidate_strategy,
                           bool enforce_summary_fit) -> bool {
        const provider::ToolConversation old_conversation = conversation_;
        const long long old_tokens =
            cached_request_tokens_.load(std::memory_order_relaxed);
        rebuild_compacted_conversation(partition, candidate_checkpoint);
        result.tokens_after = estimated_request_tokens();
        // Restore the live conversation without republishing a dry-run estimate
        // that would thrash the TUI chrome during projection.
        conversation_ = old_conversation;
        cached_request_tokens_.store(old_tokens, std::memory_order_relaxed);
        if (result.tokens_before > 0 &&
            result.tokens_after >= result.tokens_before)
            return false;
        if (enforce_summary_fit && candidate_strategy == CompactionStrategy::Summary &&
            window > 0) {
            const long long fit_limit = std::min<long long>(
                window * 60 / 100, trigger > 0 ? trigger : window);
            if (result.tokens_after > fit_limit) return false;
        }
        return true;
    };

    // Project the replacement before committing; ineffective LLM output is not
    // allowed to alter SQLite or the live request. Prefer summary when it
    // reduces and fits; otherwise fall back to the deterministic fast checkpoint.
    result.applied_strategy = applied;
    bool projected_ok =
        try_project(checkpoint, applied, applied == CompactionStrategy::Summary);
    if (!projected_ok && applied == CompactionStrategy::Summary &&
        !fast.checkpoint.empty()) {
        checkpoint = fast.checkpoint;
        applied = CompactionStrategy::Fast;
        model_fallback_to_fast = true;
        if (result.reason.empty())
            result.reason =
                "summary projection did not reduce/fit; falling back to fast";
        result.applied_strategy = applied;
        projected_ok = try_project(checkpoint, applied, false);
    }
    if (!projected_ok) {
        result.error = {ErrorCode::ProviderSchema,
                        "agent compaction did not reduce the model-visible context"};
        return failed_result();
    }

    // Commit durable state first. Only after this succeeds may request context change.
    result.error = session_store_.compact_with_summary(
        redact_secrets(checkpoint + first_head_carry, secrets_),
        static_cast<int>(partition.tail.size()));
    if (!result.error.ok()) return result;

    rebuild_compacted_conversation(partition, checkpoint);
    result.tokens_after = estimated_request_tokens();
    result.error = ok_error();
    result.compacted = true;
    result.applied_strategy = applied;
    last_auto_compact_failure_ms_ = 0;
    last_auto_compact_failure_seq_ = 0;
    const long long elapsed_seconds =
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - started)
            .count();
    result.notice = format_compaction_success_notice(
        elapsed_seconds, result.tokens_before, result.tokens_after);
    if (model_fallback_to_fast && !result.reason.empty()) {
        result.notice += " (" + result.reason + ")";
    }
    return result;
}

SessionCompactionResult AgentSessionRuntime::compact(
    const provider::RequestContext& context,
    CompactionReason reason,
    runtime::CancellationToken cancellation,
    std::optional<CompactionStrategy> strategy_override) {
    SessionCompactionResult result;
    bool expected = false;
    if (!operation_active_.compare_exchange_strong(expected, true)) {
        result.error = {ErrorCode::BadArgs, "an agent operation is already active"};
        return result;
    }
    struct Release {
        std::atomic<bool>& active;
        ~Release() { active.store(false); }
    } release{operation_active_};
    return compact_impl(context, reason, cancellation, strategy_override);
}

SessionProjectReplaceResult AgentSessionRuntime::replace_project(
    const provider::RequestContext& context,
    const NewProjectTarget& requested_target,
    runtime::CancellationToken cancellation,
    std::optional<bool> indexing_enabled) {
    namespace fs = std::filesystem;
    SessionProjectReplaceResult result;
    if (!prepared_) {
        result.error = {ErrorCode::Internal, "agent session runtime is not prepared"};
        return result;
    }

    const std::string old_workspace = options_.workspace;
    const SessionRuntimeOptions old_options = options_;
    NewProjectTarget target;
    result.error =
        resolve_new_project_target(old_workspace, requested_target.root, target);
    if (!result.error.ok()) return result;
    if (target.state_dir_exists && !requested_target.state_dir_exists) {
        result.error = {
            ErrorCode::BadArgs,
            "agent state appeared before initialization; run /new again to confirm removal: " +
                target.state_dir};
        return result;
    }

    SessionRuntimeOptions new_options = old_options;
    new_options.workspace = target.root;
    new_options.task_mode = AgentTaskMode::Act;
    new_options.permission_mode = PermissionMode::Smart;
    if (indexing_enabled.has_value())
        new_options.index_mode =
            *indexing_enabled
                ? SessionRuntimeOptions::IndexMode::UseExistingLazy
                : SessionRuntimeOptions::IndexMode::Disabled;
    provider::RequestContext quiet_context = context;
    quiet_context.options.quiet = true;

    (void)finish_session("cancelled", "", "Cancelled", "project replaced with /new");
    reset();  // release DB/index/tool handles before state moves

    std::error_code ec;
    bool created_root = false;
    bool state_moved = false;
    bool initialization_started = false;
    fs::path backup;
    auto reopen_old = [&](const Error& original) {
        reset();
        std::error_code cleanup_ec;
        std::string rollback_detail;
        if (initialization_started) {
            fs::remove_all(fs::u8path(target.state_dir), cleanup_ec);
            if (cleanup_ec)
                rollback_detail = "could not remove failed state " + target.state_dir + ": " +
                                  cleanup_ec.message();
        }
        bool restored_state = !state_moved;
        cleanup_ec.clear();
        if (state_moved && !backup.empty()) {
            const bool backup_exists = fs::exists(backup, cleanup_ec);
            if (backup_exists && !cleanup_ec) {
                cleanup_ec.clear();
                fs::rename(backup, fs::u8path(target.state_dir), cleanup_ec);
                restored_state = !cleanup_ec;
            }
            if (!restored_state) {
                if (!rollback_detail.empty()) rollback_detail += "; ";
                rollback_detail +=
                    "could not restore prior state from " + backup.u8string() + " to " +
                    target.state_dir +
                    (cleanup_ec ? ": " + cleanup_ec.message()
                                : std::string(": backup is unavailable"));
            }
        }
        if (created_root) {
            cleanup_ec.clear();
            if (fs::is_empty(fs::u8path(target.root), cleanup_ec) && !cleanup_ec)
                fs::remove(fs::u8path(target.root), cleanup_ec);
        }
        result.error = original;
        if (!rollback_detail.empty()) result.error.message += "; rollback: " + rollback_detail;
        if (old_workspace == target.root && !restored_state) {
            result.error.message += "; prior project could not be reopened safely";
            return;
        }
        Error reopen_error = prepare(quiet_context, {}, {}, old_options);
        if (!reopen_error.ok()) {
            result.error.message += "; additionally could not reopen prior project " +
                                    old_workspace + ": " + reopen_error.message;
        }
    };

    if (!target.root_exists) {
        created_root = fs::create_directory(fs::u8path(target.root), ec);
        if (ec || !created_root) {
            reopen_old({ErrorCode::FileWrite,
                        "could not create /new project directory " + target.root + ": " +
                            (ec ? ec.message() : std::string("creation failed"))});
            return result;
        }
    }

    if (target.state_dir_exists) {
        const fs::path root = fs::u8path(target.root);
        for (unsigned attempt = 0; attempt < 100; ++attempt) {
            backup = root / (std::string(".ainiux-pr.ainiux-new-backup-") +
                             std::to_string(platform::current_process_id()) + "-" +
                             std::to_string(attempt));
            if (!fs::exists(backup, ec)) break;
            ec.clear();
        }
        fs::rename(fs::u8path(target.state_dir), backup, ec);
        if (ec) {
            reopen_old({ErrorCode::FileWrite,
                        "could not release existing agent state " + target.state_dir + ": " +
                            ec.message()});
            return result;
        }
        state_moved = true;
    }

    initialization_started = true;
    result.error = prepare(quiet_context, cancellation, {}, new_options);
    if (!result.error.ok()) {
        const Error initialization_error = result.error;
        reopen_old({initialization_error.code,
                    "could not initialize fresh agent project " + target.root + ": " +
                        initialization_error.message});
        return result;
    }

    if (!backup.empty()) {
        ec.clear();
        fs::remove_all(backup, ec);
        if (ec)
            result.warning = "Fresh project initialized, but old state cleanup failed at " +
                             backup.u8string() + ": " + ec.message();
    }
    result.workspace = target.root;
    result.error = ok_error();
    return result;
}

void AgentSessionRuntime::reset() {
    runtime::kill_all_background_processes();
    tools_.set_mcp_bridge(nullptr);
    mcp_bridge_.reset();
    if (mcp_manager_) mcp_manager_->close_all();
    mcp_manager_.reset();
    if (session_id_ > 0 && session_store_.is_open()) {
        // Best-effort close of a still-running session when the runtime is torn down.
        (void)session_store_.finish_session(session_id_, "cancelled", "", "Cancelled",
                                            "agent session closed",
                                            static_cast<long long>(session_turns_),
                                            static_cast<long long>(session_tool_calls_));
    }
    logger_.reset();
    session_store_.close();
    tools_ = ReadToolRegistry{};
    prompts_ = TrustedPrompts{};
    agents_md_ = AgentsMdBundle{};
    conversation_ = provider::ToolConversation{};
    state_ = AgentLoopState{};
    limits_ = AgentLoopLimits{};
    known_tools_.clear();
    secrets_.clear();
    session_id_ = 0;
    session_turns_ = 0;
    session_tool_calls_ = 0;
    session_failed_tool_calls_ = 0;
    conversation_seeded_ = false;
    prepared_ = false;
    options_ = SessionRuntimeOptions{};
    task_mode_ = AgentTaskMode::Act;
    permission_mode_ = PermissionMode::Smart;
    goal_ = SessionGoal{};
    context_reset_after_seq_ = 0;
    display_min_seq_.store(0, std::memory_order_relaxed);
    cached_request_tokens_.store(0, std::memory_order_relaxed);
    last_nonzero_request_tokens_.store(0, std::memory_order_relaxed);
    in_flight_generation_tokens_.store(0, std::memory_order_relaxed);
    guard_approval_wait_ms_.store(0, std::memory_order_relaxed);
    operation_active_.store(false, std::memory_order_relaxed);
}

Error AgentSessionRuntime::prepare(const provider::RequestContext& context,
                                   runtime::CancellationToken cancellation,
                                   std::function<bool()> interrupted,
                                   SessionRuntimeOptions options) {
    const auto preparation_started = std::chrono::steady_clock::now();
    reset();
    options_ = std::move(options);
    task_mode_ = options_.task_mode;
    permission_mode_ = options_.interactive ? options_.permission_mode
                                            : PermissionMode::Smart;
    if (options_.workspace.empty()) options_.workspace = ".";
    {
        std::string absolute;
        Error root_error = resolve_agent_project_root(options_.workspace, absolute);
        if (!root_error.ok()) return root_error;
        options_.workspace = absolute;
    }
    secrets_ = configured_secrets(context);

    // Capture cancellation/interrupted by value. index_options_ lives inside tools for
    // the whole session; a [&] lambda here used to dangle after prepare() returned and
    // segfault during index_overview → check_freshness (stack-use-after-scope).
    const runtime::CancellationToken cancel_copy = cancellation;
    const std::function<bool()> interrupted_copy = std::move(interrupted);
    auto interrupted_fn = [this, cancel_copy, interrupted_copy]() {
        return is_interrupted(cancel_copy, interrupted_copy);
    };

    const bool quiet = context.options.quiet;
    if (options_.enable_agent_log) {
        Error log_error;
        logger_ = ReviewLogger::create(
            options_.workspace, options_.security_review_log_keep_runs, secrets_,
            [quiet](const std::string& warning) {
                if (!quiet) std::cerr << warning << "\n";
            },
            log_error, "agent");
        if (!logger_ && !quiet) {
            std::cerr << "AGENT LOGGING DISABLED: " << redact_secrets(log_error.message, secrets_)
                      << "; the agent will continue\n";
        } else if (logger_ && !quiet) {
            std::cerr << "Agent diagnostic log (live): " << logger_->partial_path() << "\n"
                      << "  tail -f that path while the agent runs; finalized as "
                      << logger_->final_path() << " on completion\n";
        }
    }
    auto publish_preparation =
        [&](PreparationPhase phase, bool completed,
            std::chrono::steady_clock::time_point phase_started) {
            const auto now = std::chrono::steady_clock::now();
            PreparationProgress progress;
            progress.phase = phase;
            progress.completed = completed;
            if (completed) {
                progress.phase_elapsed_ms =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        now - phase_started)
                        .count();
            }
            progress.total_elapsed_ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - preparation_started)
                    .count();
            if (options_.on_prepare_progress)
                options_.on_prepare_progress(progress);
            if (completed && logger_) {
                json::Value fields = log_object();
                fields.object["phase"] =
                    log_string(preparation_phase_name(phase));
                fields.object["phase_elapsed_ms"] =
                    log_number(progress.phase_elapsed_ms);
                fields.object["total_elapsed_ms"] =
                    log_number(progress.total_elapsed_ms);
                logger_->event("preparation_phase", {"prepare"},
                               std::move(fields), "success");
            }
        };

    index::Options index_options;
    index_options.workspace = options_.workspace;
    index_options.max_source_code_file_size = options_.max_source_code_file_size;
    index_options.cancellation = cancel_copy;
    index_options.interrupted = interrupted_fn;
    index_options.on_progress = options_.on_index_progress;
    Error error = ok_error();
    bool indexing_enabled = false;
    auto phase_started = std::chrono::steady_clock::now();
    publish_preparation(PreparationPhase::IndexProbe, false, phase_started);
    const bool indexing_requested =
        options_.index_mode != SessionRuntimeOptions::IndexMode::Disabled;
    if (indexing_requested) {
        index::ProbeResult probe;
        error = index::probe(index_options, probe);
        if (error.ok() && probe.state == index::ProbeState::Completed)
            indexing_enabled = true;
        else if (error.ok() && probe.state == index::ProbeState::Corrupt)
            error = probe.error;
        if (!error.ok()) {
            if (error.code == ErrorCode::Cancelled) {
                reset();
                return error;
            }
            if (!context.options.quiet)
                std::cerr << "Index warning: "
                          << redact_secrets(error.message, secrets_)
                          << "; continuing with live filesystem tools.\n";
            error = ok_error();
            indexing_enabled = false;
        } else if (!indexing_enabled && !context.options.quiet) {
            std::cerr
                << "Code index not found; Agent is ready with live filesystem "
                   "tools. Run /index-code to create it.\n";
        }
    }
    publish_preparation(PreparationPhase::IndexProbe, true, phase_started);
    if (logger_) {
        json::Value fields = log_object();
        fields.object["indexing_enabled"] =
            log_bool(indexing_enabled);
        logger_->event("index_result", {"index"}, std::move(fields),
                       "success");
    }

    phase_started = std::chrono::steady_clock::now();
    publish_preparation(PreparationPhase::ToolSetup, false, phase_started);
    ToolRegistryOptions tool_options;
    tool_options.mutation_policy = task_mode_ == AgentTaskMode::Plan
                                       ? MutationPolicy::PlanningDocuments
                                       : MutationPolicy::Full;
    tool_options.allow_network = options_.allow_network;
    tool_options.hosted_web_search = provider::hosted_web_search_enabled(context);
    tool_options.hosted_web_search_name = provider::hosted_web_search_display_name(context);
    tool_options.history_backup = options_.history_backup;
    tool_options.fetch_options = options_.fetch_options;
    tool_options.search_options = options_.search_options;
    tool_options.permission_mode = permission_mode_;
    tool_options.permission_controls = options_.interactive;
    tool_options.session_store = &session_store_;
    tool_options.indexing_enabled = indexing_enabled;
    tool_options.goal_hooks.has_active_goal = [this]() { return goal_is_active(goal_); };
    tool_options.goal_hooks.mark_complete = [this](const std::string& evidence) -> Error {
        return mark_goal_complete(evidence);
    };
    // Wrap Ask so every resolution is persisted and optionally surfaced.
    if (options_.on_guard_ask) {
        tool_options.on_guard_ask =
            [this](const GuardApprovalRequest& request,
                   runtime::CancellationToken cancellation) -> GuardApprovalDecision {
            const auto approval_started = std::chrono::steady_clock::now();
            GuardApprovalDecision decision = GuardApprovalDecision::Deny;
            if (options_.on_guard_ask) decision = options_.on_guard_ask(request, cancellation);
            const long long approval_ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - approval_started)
                    .count();
            guard_approval_wait_ms_.fetch_add(approval_ms, std::memory_order_relaxed);
            if (session_store_.is_open()) {
                AgentApprovalRecord row;
                row.tool_name = request.tool_name;
                row.command_preview = redact_secrets(request.command_preview, secrets_);
                row.rule_id = request.rule_id;
                row.decision = guard_approval_decision_name(decision);
                row.source = "interactive";
                row.message = redact_secrets(request.message, secrets_);
                (void)session_store_.record_approval(row);
                const std::string notice =
                    "Guard " + row.decision + ": " + row.command_preview +
                    (row.rule_id.empty() ? std::string() : " [" + row.rule_id + "]");
                (void)session_store_.append_message("notice", notice);
            }
            if (logger_) {
                json::Value fields = log_object();
                fields.object["tool"] = log_string(request.tool_name);
                fields.object["command"] = log_string(request.command_preview);
                fields.object["rule_id"] = log_string(request.rule_id);
                fields.object["decision"] = log_string(guard_approval_decision_name(decision));
                fields.object["message"] = ReviewLogger::payload(
                    redact_secrets(request.message, secrets_));
                logger_->event("guard_approval", {"guard"}, std::move(fields),
                               decision == GuardApprovalDecision::Allow ? "success" : "failure");
            }
            return decision;
        };
    }
    if (indexing_enabled) {
        error = ReadToolRegistry::create_lazy(
            index_options, secrets_, tools_, tool_options);
    } else {
        error = ReadToolRegistry::create_without_index(
            index_options, secrets_, tools_, tool_options);
    }
    if (!error.ok()) {
        reset();
        return error;
    }

    // Load enabled MCP servers and advertise their tools (agent/run/plan only).
    mcp_manager_ = std::make_shared<mcp::Manager>();
    mcp::ConnectOptions mcp_opts;
    mcp_opts.connect_timeout_seconds = context.options.connect_timeout_seconds > 0
                                           ? context.options.connect_timeout_seconds
                                           : 30;
    mcp_opts.tool_timeout_seconds =
        context.options.timeout_seconds > 0 ? context.options.timeout_seconds : 120;
    mcp_opts.block_private_addresses = !context.options.allow_private_url_fetch;
    mcp_opts.insecure_tls = context.options.insecure_tls;
    mcp_opts.trace = context.options.trace_http;
    mcp_opts.secrets_to_redact = secrets_;
    mcp_opts.cancellation = cancellation;
    mcp_manager_->set_connect_options(mcp_opts);
    {
        const Error mcp_load = mcp_manager_->reload_from_registry();
        if (!mcp_load.ok() && !context.options.quiet) {
            std::cerr << "MCP registry: " << mcp_load.message << "\n";
        }
        mcp_bridge_ = std::make_unique<mcp::ToolBridge>();
        mcp_bridge_->set_manager(mcp_manager_);
        const Error mcp_refresh = mcp_bridge_->refresh(cancellation);
        if (!mcp_refresh.ok() && !context.options.quiet) {
            std::cerr << "MCP tools: " << mcp_refresh.message << "\n";
        } else if (!context.options.quiet) {
            const auto defs = mcp_bridge_->definitions();
            if (!defs.empty()) {
                std::cerr << "MCP tools loaded: " << defs.size() << "\n";
            }
            for (const std::string& msg : mcp_bridge_->last_errors()) {
                std::cerr << "MCP: " << msg << "\n";
            }
        }
        tools_.set_mcp_bridge(mcp_bridge_.get());
    }

    error = load_trusted_prompts(options_.trusted_prompt_dir, prompts_);
    if (!error.ok()) {
        reset();
        return error;
    }

    const bool supports_tools = provider::capabilities_for(context).tool_calls;
    state_.protocol = default_tool_protocol(supports_tools);
    limits_.interactive = options_.interactive;
    limits_.max_scripted_turns = options_.max_agent_turns > 0
                                     ? static_cast<std::size_t>(options_.max_agent_turns)
                                     : 250U;
    known_tools_ = known_tool_names(tools_);
    publish_preparation(PreparationPhase::ToolSetup, true, phase_started);

    phase_started = std::chrono::steady_clock::now();
    publish_preparation(PreparationPhase::SessionDatabase, false, phase_started);
    if (options_.enable_session_db) {
        error = session_store_.open(options_.workspace);
        if (!error.ok()) {
            reset();
            return error;
        }
        if (!context.options.quiet)
            std::cerr << "Agent session DB: " << session_store_.path() << "\n";
        if (options_.interactive) {
            const PermissionMode requested_permission_mode = permission_mode_;
            AgentProjectRecord project;
            error = session_store_.open_project(project);
            if (!error.ok()) {
                reset();
                return error;
            }
            error = permission_mode_from_settings_json(project.settings_json,
                                                       permission_mode_);
            if (!error.ok()) {
                reset();
                return error;
            }
            if (!options_.allow_yolo && permission_mode_ == PermissionMode::Yolo) {
                permission_mode_ = requested_permission_mode;
            }
            options_.permission_mode = permission_mode_;
            tools_.set_permission_mode(permission_mode_);
            error = goal_from_settings_json(project.settings_json, goal_);
            if (!error.ok()) {
                reset();
                return error;
            }
            error = context_reset_after_seq_from_settings_json(project.settings_json,
                                                               context_reset_after_seq_);
            if (!error.ok()) {
                reset();
                return error;
            }
        }
    }
    publish_preparation(PreparationPhase::SessionDatabase, true, phase_started);

    phase_started = std::chrono::steady_clock::now();
    publish_preparation(PreparationPhase::History, false, phase_started);
    if (options_.interactive && session_store_.is_open()) {
        std::vector<AgentMessageRecord> history;
        error = session_store_.load_messages(history);
        if (!error.ok()) {
            reset();
            return error;
        }
    }
    publish_preparation(PreparationPhase::History, true, phase_started);

    phase_started = std::chrono::steady_clock::now();
    publish_preparation(PreparationPhase::ProjectInstructions, false,
                        phase_started);
    error = load_root_agents_md(options_.workspace, kDefaultAgentsMdMaxBytes, agents_md_);
    if (!error.ok()) {
        if (!context.options.quiet)
            std::cerr << "Agent warning: could not load AGENTS.md: "
                      << redact_secrets(error.message, secrets_) << "\n";
        if (logger_) {
            json::Value fields = log_object();
            fields.object["error_code"] = log_string(error_code_name(error.code));
            fields.object["error_message"] = log_string(error.message);
            logger_->event("agents_md", {"agents_md"}, std::move(fields), "failure");
        }
        agents_md_ = AgentsMdBundle{};
        error = ok_error();
    } else if (logger_) {
        json::Value fields = log_object();
        fields.object["documents"] = log_number(agents_md_.documents.size());
        fields.object["total_bytes"] = log_number(agents_md_.total_bytes);
        fields.object["truncated"] = log_bool(agents_md_.truncated);
        logger_->event("agents_md", {"agents_md"}, std::move(fields), "success");
    }
    publish_preparation(PreparationPhase::ProjectInstructions, true,
                        phase_started);

    prepared_ = true;
    // Do not seed conversation_ here: the first user turn still owns full seed
    // (including prior-transcript injection). Idle chrome must estimate what the
    // *next* model request will carry — not the full durable SQLite history.
    // Match seed_agent_conversation + interactive build_prior_session_context +
    // native tool schemas so the status line is not inflated by old tool rows.
    {
        long long baseline = estimate_seed_overhead_tokens();
        // Interactive reopen injects a bounded prior-session block, not the full
        // transcript. Headless --run/--plan starts with a fresh model conversation.
        if (options_.interactive && session_store_.is_open()) {
            std::vector<AgentMessageRecord> rows;
            if (session_store_.load_messages(rows).ok()) {
                apply_context_reset_filter(rows);
                const std::string prior = build_prior_session_context(rows);
                if (!prior.empty()) {
                    baseline += estimate_tokens_from_text("user");
                    baseline += estimate_tokens_from_text(prior);
                    baseline += 4;
                }
            }
        }
        cached_request_tokens_.store(baseline, std::memory_order_relaxed);
        if (baseline > 0)
            last_nonzero_request_tokens_.store(baseline, std::memory_order_relaxed);
    }
    return ok_error();
}

void AgentSessionRuntime::begin_background_index_freshness() {
    if (prepared_ && tools_.indexing_enabled())
        tools_.enqueue_background_freshness();
}

Error AgentSessionRuntime::load_display_messages(std::vector<provider::Message>& out) const {
    out.clear();
    if (!prepared_ || !session_store_.is_open()) return ok_error();
    std::vector<AgentMessageRecord> rows;
    Error error = session_store_.load_messages(rows);
    if (!error.ok()) return error;
    apply_context_reset_filter(rows);
    const long long hidden_through =
        display_min_seq_.load(std::memory_order_relaxed);
    out.reserve(rows.size());
    const std::size_t cols = terminal_column_count();
    for (const AgentMessageRecord& row : rows) {
        if (hidden_through > 0 && row.seq <= hidden_through) continue;
        // Summary rows contain the internal model checkpoint, potentially with
        // large structured tool results. The completion status reports the
        // compaction to the user; replaying this payload would flood the TUI.
        if (row.role == "summary") continue;
        provider::Message message;
        if (row.role == "user" || row.role == "assistant" || row.role == "system" ||
            row.role == "tool" || row.role == "notice" || row.role == "thinking" ||
            row.role == "summary" || row.role == "index") {
            message.role = row.role;
        } else {
            message.role = "notice";
        }
        // Tool/notice lines are single-row activity; clip to terminal width.
        if (row.role == "tool" || row.role == "notice" || row.role == "thinking") {
            message.content = clip_to_cells(row.content, cols);
        } else {
            message.content = row.content;
        }
        message.created_at_ms = normalize_timestamp_ms(row.created_at);
        // Collapse consecutive identical notices so repeated prepare runs (for
        // example "Code indexing is off…") do not stack in the Agent history.
        if (message.role == "notice" && !out.empty() &&
            out.back().role == "notice" && out.back().content == message.content) {
            continue;
        }
        out.push_back(std::move(message));
    }
    return ok_error();
}

Error AgentSessionRuntime::append_display_notice(const std::string& content) {
    if (!session_store_.is_open()) return ok_error();
    if (content.empty()) return ok_error();
    const std::string redacted = redact_secrets(content, secrets_);
    // Avoid stacking the same startup notice across agent restarts.
    AgentMessageRecord last;
    bool found = false;
    Error error = session_store_.peek_last_message(last, found);
    if (!error.ok()) return error;
    if (found && last.role == "notice" && last.content == redacted) {
        return ok_error();
    }
    return session_store_.append_message("notice", redacted);
}

SessionIndexReportResult AgentSessionRuntime::show_index(
    bool refresh,
    runtime::CancellationToken cancellation) {
    SessionIndexReportResult result;
    if (!prepared_) {
        result.error = {ErrorCode::Internal,
                        "agent session runtime is not prepared"};
        return result;
    }
    bool expected = false;
    if (!operation_active_.compare_exchange_strong(expected, true)) {
        result.error = {ErrorCode::BadArgs,
                        "an agent operation is already active"};
        return result;
    }
    struct Release {
        std::atomic<bool>& active;
        ~Release() { active.store(false); }
    } release{operation_active_};

    if (!tools_.indexing_enabled()) {
        result.error = {
            ErrorCode::UnsupportedFeature,
            "Code indexing is off for this Agent session; run /index-code to "
            "create and enable it. Code indexing can speed up certain lookup "
            "calls."};
        return result;
    }
    result.indexing_enabled = true;
    if (refresh) {
        result.error = tools_.refresh_persistent_index(true, cancellation);
        if (!result.error.ok()) return result;
    }
    index::Options totals_options;
    totals_options.workspace = options_.workspace;
    totals_options.max_source_code_file_size =
        options_.max_source_code_file_size;
    totals_options.cancellation = cancellation;
    index::QueryTotals totals;
    result.error = index::query_totals(totals_options, totals);
    if (!result.error.ok()) return result;
    result.markdown = index::compact_totals_markdown(totals);
    if (session_store_.is_open()) {
        result.error =
            session_store_.append_message("index", result.markdown);
        if (!result.error.ok()) return result;
    }
    result.error = ok_error();
    return result;
}

SessionIndexReportResult AgentSessionRuntime::index_code(
    runtime::CancellationToken cancellation) {
    SessionIndexReportResult result;
    const auto started = std::chrono::steady_clock::now();
    auto stamp_elapsed = [&]() {
        result.elapsed_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started)
                .count();
    };
    if (!prepared_) {
        result.error = {ErrorCode::Internal,
                        "agent session runtime is not prepared"};
        stamp_elapsed();
        return result;
    }
    bool expected = false;
    if (!operation_active_.compare_exchange_strong(expected, true)) {
        result.error = {ErrorCode::BadArgs,
                        "an agent operation is already active"};
        stamp_elapsed();
        return result;
    }
    struct Release {
        std::atomic<bool>& active;
        ~Release() { active.store(false); }
    } release{operation_active_};

    const bool was_enabled = tools_.indexing_enabled();
    if (was_enabled) {
        result.error = tools_.refresh_persistent_index(true, cancellation);
        if (!result.error.ok()) {
            result.indexing_enabled = true;
            stamp_elapsed();
            return result;
        }
    } else {
        index::Options index_options;
        index_options.workspace = options_.workspace;
        index_options.max_source_code_file_size =
            options_.max_source_code_file_size;
        index_options.cancellation = cancellation;
        index_options.interrupted =
            [cancellation] { return cancellation.cancelled(); };
        index_options.on_progress = options_.on_index_progress;

        index::RefreshStats stats;
        result.error = index::refresh(index_options, stats);
        if (!result.error.ok()) {
            stamp_elapsed();
            return result;
        }
        result.error =
            tools_.enable_lazy_index(std::move(index_options));
        if (!result.error.ok()) {
            stamp_elapsed();
            return result;
        }

        options_.index_mode =
            SessionRuntimeOptions::IndexMode::UseExistingLazy;
        known_tools_ = known_tool_names(tools_);
        result.created = true;
    }

    result.indexing_enabled = true;
    index::Options totals_options;
    totals_options.workspace = options_.workspace;
    totals_options.max_source_code_file_size =
        options_.max_source_code_file_size;
    totals_options.cancellation = cancellation;
    index::QueryTotals totals;
    result.error = index::query_totals(totals_options, totals);
    if (!result.error.ok()) {
        stamp_elapsed();
        return result;
    }
    stamp_elapsed();
    // History row: timing line, then the compact language totals table.
    result.markdown = format_index_completion_intro(result.elapsed_ms) + "\n\n" +
                      index::compact_totals_markdown(totals);
    if (session_store_.is_open()) {
        result.error =
            session_store_.append_message("index", result.markdown);
        if (!result.error.ok()) return result;
    }
    result.error = ok_error();
    return result;
}

Error AgentSessionRuntime::switch_task_mode(AgentTaskMode mode) {
    if (!prepared_)
        return {ErrorCode::Internal, "agent session runtime is not prepared"};
    if (mode == task_mode_) return ok_error();
    bool expected = false;
    if (!operation_active_.compare_exchange_strong(expected, true))
        return {ErrorCode::BadArgs,
                "cannot switch agent task mode while an agent operation is active"};
    struct Release {
        std::atomic<bool>& active;
        ~Release() { active.store(false); }
    } release{operation_active_};

    AgentsMdBundle refreshed;
    Error error = load_root_agents_md(options_.workspace, kDefaultAgentsMdMaxBytes, refreshed);
    if (!error.ok()) return error;
    if (conversation_seeded_) {
        if (conversation_.messages.empty() || conversation_.messages.front().role != "system")
            return {ErrorCode::Internal,
                    "agent conversation has no trusted system prompt"};
        // Preserve the serialized prefix. Refreshed project instructions and
        // mode controls are appended for later rounds.
        if (refreshed.injection_text != agents_md_.injection_text) {
            const std::string refreshed_context =
                refreshed.injection_text.empty()
                    ? "[Ainiux refreshed project instructions]\n"
                      "No workspace-root AGENTS.md instructions are currently present."
                    : refreshed.injection_text;
            append_conversation_text(conversation_, "user", refreshed_context);
        }
        std::vector<std::string> script_names;
        (void)list_project_scripts(options_.workspace, script_names);
        append_conversation_text(conversation_, "user",
                                 agent_task_mode_control(mode, script_names));
    }
    agents_md_ = std::move(refreshed);
    task_mode_ = mode;
    options_.task_mode = mode;
    tools_.set_mutation_policy(mode == AgentTaskMode::Plan
                                   ? MutationPolicy::PlanningDocuments
                                   : MutationPolicy::Full);
    known_tools_ = known_tool_names(tools_);
    publish_request_token_estimate();
    return ok_error();
}

Error AgentSessionRuntime::switch_permission_mode(
    PermissionMode mode,
    const provider::RequestContext& context) {
    if (!prepared_)
        return {ErrorCode::Internal, "agent session runtime is not prepared"};
    if (!options_.interactive)
        return {ErrorCode::UnsupportedFeature,
                "permission modes are available only in interactive agent mode"};
    if (mode == permission_mode_) return ok_error();
    bool expected = false;
    if (!operation_active_.compare_exchange_strong(expected, true))
        return {ErrorCode::BadArgs,
                "cannot switch permissions while an agent operation or approval is active"};
    struct Release {
        std::atomic<bool>& active;
        ~Release() { active.store(false); }
    } release{operation_active_};

    // write_session_settings encodes permission_mode_, so apply the new mode
    // before persisting. Roll it back if the project row cannot be updated.
    const PermissionMode previous = permission_mode_;
    permission_mode_ = mode;
    if (session_store_.is_open()) {
        AgentProjectRecord project;
        Error error = session_store_.open_project(project);
        if (!error.ok()) {
            permission_mode_ = previous;
            return error;
        }
        project.provider = context.profile.name;
        project.model = context.options.model;
        project.api =
            context.api_kind == provider::ApiKind::Responses ? "responses" : "chat";
        project.protocol = state_.protocol == ToolProtocol::Xml ? "xml" : "native";
        project.base_url = context.base_url;
        project.workspace = options_.workspace;
        error = settings_json_with_permission_mode(
            chat::settings_json_from_options(context.options), mode,
            project.settings_json);
        if (!error.ok()) {
            permission_mode_ = previous;
            return error;
        }
        error = write_session_settings(project);
        if (!error.ok()) {
            permission_mode_ = previous;
            return error;
        }
        error = session_store_.update_project_meta(project);
        if (!error.ok()) {
            permission_mode_ = previous;
            return error;
        }
    }
    options_.permission_mode = mode;
    tools_.set_permission_mode(mode);
    return ok_error();
}

Error AgentSessionRuntime::persist_goal_settings() {
    if (!session_store_.is_open()) return ok_error();
    AgentProjectRecord project;
    Error error = session_store_.open_project(project);
    if (!error.ok()) return error;
    error = settings_json_with_goal(project.settings_json, goal_, project.settings_json);
    if (!error.ok()) return error;
    return session_store_.update_project_meta(project);
}

void AgentSessionRuntime::inject_active_goal_control(bool continue_nudge) {
    if (!goal_is_active(goal_) || !conversation_seeded_) return;
    const std::string control =
        continue_nudge ? agent_goal_continue_control(goal_) : agent_goal_control(goal_);
    if (control.empty()) return;
    append_conversation_text(conversation_, "user", control);
    publish_request_token_estimate();
}

Error AgentSessionRuntime::set_goal(const std::string& condition) {
    if (!prepared_)
        return {ErrorCode::Internal, "agent session runtime is not prepared"};
    const std::string text = ascii_trim(condition);
    if (text.empty())
        return {ErrorCode::BadArgs, "goal condition must not be empty"};
    bool expected = false;
    if (!operation_active_.compare_exchange_strong(expected, true))
        return {ErrorCode::BadArgs,
                "cannot set a goal while an agent operation is active"};
    struct Release {
        std::atomic<bool>& active;
        ~Release() { active.store(false); }
    } release{operation_active_};

    goal_.condition = text;
    goal_.status = GoalStatus::Active;
    goal_.turns = 0;
    goal_.last_reason.clear();
    Error error = persist_goal_settings();
    if (!error.ok()) return error;
    // Prompt injection happens on the next run_user_turn (avoids duplicate controls).
    if (session_store_.is_open()) {
        (void)session_store_.append_message(
            "notice", "Goal set: " + bound_goal_text(goal_.condition, 240));
    }
    return ok_error();
}

Error AgentSessionRuntime::clear_goal(const std::string& reason) {
    if (!prepared_)
        return {ErrorCode::Internal, "agent session runtime is not prepared"};
    bool expected = false;
    if (!operation_active_.compare_exchange_strong(expected, true))
        return {ErrorCode::BadArgs,
                "cannot clear the goal while an agent operation is active"};
    struct Release {
        std::atomic<bool>& active;
        ~Release() { active.store(false); }
    } release{operation_active_};

    const bool had = goal_.status != GoalStatus::Cleared && !goal_.condition.empty();
    goal_.status = GoalStatus::Cleared;
    goal_.turns = 0;
    goal_.last_reason = bound_goal_text(reason);
    if (!had) goal_.condition.clear();
    Error error = persist_goal_settings();
    if (!error.ok()) return error;
    if (session_store_.is_open() && had) {
        (void)session_store_.append_message("notice", "Goal cleared");
    }
    return ok_error();
}

Error AgentSessionRuntime::pause_goal(const std::string& reason) {
    if (!prepared_)
        return {ErrorCode::Internal, "agent session runtime is not prepared"};
    if (!goal_is_active(goal_) && goal_.status != GoalStatus::Paused)
        return {ErrorCode::BadArgs, "no active goal to pause"};
    bool expected = false;
    if (!operation_active_.compare_exchange_strong(expected, true))
        return {ErrorCode::BadArgs,
                "cannot pause the goal while an agent operation is active"};
    struct Release {
        std::atomic<bool>& active;
        ~Release() { active.store(false); }
    } release{operation_active_};

    goal_.status = GoalStatus::Paused;
    if (!reason.empty()) goal_.last_reason = bound_goal_text(reason);
    Error error = persist_goal_settings();
    if (!error.ok()) return error;
    if (session_store_.is_open()) {
        (void)session_store_.append_message(
            "notice", "Goal paused: " + bound_goal_text(goal_.condition, 160));
    }
    return ok_error();
}

Error AgentSessionRuntime::resume_goal() {
    if (!prepared_)
        return {ErrorCode::Internal, "agent session runtime is not prepared"};
    if (goal_.status != GoalStatus::Paused || ascii_trim(goal_.condition).empty())
        return {ErrorCode::BadArgs, "no paused goal to resume"};
    bool expected = false;
    if (!operation_active_.compare_exchange_strong(expected, true))
        return {ErrorCode::BadArgs,
                "cannot resume the goal while an agent operation is active"};
    struct Release {
        std::atomic<bool>& active;
        ~Release() { active.store(false); }
    } release{operation_active_};

    goal_.status = GoalStatus::Active;
    Error error = persist_goal_settings();
    if (!error.ok()) return error;
    // Prompt injection happens on the next run_user_turn.
    if (session_store_.is_open()) {
        (void)session_store_.append_message(
            "notice", "Goal resumed: " + bound_goal_text(goal_.condition, 160));
    }
    return ok_error();
}

Error AgentSessionRuntime::mark_goal_complete(const std::string& evidence) {
    if (!prepared_)
        return {ErrorCode::Internal, "agent session runtime is not prepared"};
    if (!goal_is_active(goal_))
        return {ErrorCode::UnsupportedFeature, "no active session goal"};
    const std::string text = bound_goal_text(evidence);
    if (ascii_trim(text).empty())
        return {ErrorCode::BadArgs, "goal_met requires non-empty evidence"};
    // Called from the tool executor while a turn is already active — do not
    // take operation_active_ (already held by run_user_turn).
    goal_.status = GoalStatus::Complete;
    goal_.last_reason = text;
    Error error = persist_goal_settings();
    if (!error.ok()) return error;
    if (session_store_.is_open()) {
        (void)session_store_.append_message(
            "notice", "Goal complete: " + bound_goal_text(text, 240));
    }
    return ok_error();
}

Error AgentSessionRuntime::finish_session(const std::string& status,
                                          const std::string& final_text,
                                          const std::string& error_code,
                                          const std::string& error_message) {
    runtime::kill_all_background_processes();
    if (!session_store_.is_open() || session_id_ <= 0) return ok_error();
    Error error = session_store_.finish_session(
        session_id_, status, redact_secrets(final_text, secrets_), error_code,
        redact_secrets(error_message, secrets_), static_cast<long long>(session_turns_),
        static_cast<long long>(session_tool_calls_));
    if (logger_) {
        json::Value fields = log_object();
        fields.object["session_id"] = log_number(session_id_);
        fields.object["status"] = log_string(status);
        fields.object["turns"] = log_number(session_turns_);
        fields.object["tool_calls"] = log_number(session_tool_calls_);
        fields.object["failed_tool_calls"] =
            log_number(session_failed_tool_calls_);
        logger_->event("session_finish", {"session"}, std::move(fields),
                       error.ok() ? "success" : "failure");
        logger_->finish(log_object(), status == "success" ? "success" : "failure");
        logger_.reset();
    }
    session_id_ = 0;
    return error;
}

SessionTurnResult AgentSessionRuntime::run_user_turn(
    provider::RequestContext& context,
    UserTurnPayload payload,
    runtime::CancellationToken cancellation,
    std::function<bool()> interrupted,
    std::function<void(const std::string& status_line)> on_progress,
    std::function<void(const AgentProgressUpdate&)> on_structured_progress) {
    SessionTurnResult result;
    if (!prepared_) {
        result.error = {ErrorCode::Internal, "agent session runtime is not prepared"};
        return result;
    }
    bool expected_active = false;
    if (!operation_active_.compare_exchange_strong(expected_active, true)) {
        result.error = {ErrorCode::BadArgs, "an agent operation is already active"};
        return result;
    }
    struct ReleaseOperation {
        std::atomic<bool>& active;
        ~ReleaseOperation() { active.store(false); }
    } release_operation{operation_active_};
    // Always drop image payloads when leaving this turn (success, error, cancel).
    struct StripTurnImages {
        provider::ToolConversation& conversation;
        bool armed = true;
        ~StripTurnImages() {
            if (armed) strip_conversation_images(conversation);
        }
    } strip_images{conversation_};
    std::string text = ascii_trim(payload.text);
    if (text.empty() && !payload.images.empty()) {
        text = payload.images.size() == 1 ? "See attached image."
                                          : "See attached images.";
    }
    if (text.empty()) {
        result.error = {ErrorCode::BadArgs, "agent turn requires a non-empty user message"};
        return result;
    }
    // Turn-scoped attachment bag for MCP rewrite (and optional vision inject).
    attachment_bag_.clear();
    if (mcp_bridge_) {
        mcp_bridge_->set_attachment_bag(&attachment_bag_);
        mcp::ArgRewriteCaps caps;
        caps.max_image_bytes =
            context.options.max_image_bytes > 0
                ? static_cast<std::size_t>(context.options.max_image_bytes)
                : 20U * 1024U * 1024U;
        mcp_bridge_->set_arg_rewrite_caps(caps);
        tools_.set_mcp_bridge(mcp_bridge_.get());
    }

    const bool vision_model_ok = provider::validate_image_input(context).ok();
    if (!payload.images.empty()) {
        for (const provider::ImageInput& image : payload.images) {
            if (image.mime_type.empty() || image.base64_data.empty()) {
                result.error = {
                    ErrorCode::FileRead,
                    "image attachment data is unavailable" +
                        (image.display_name.empty() ? std::string()
                                                    : ": " + image.display_name)};
                return result;
            }
            const std::string abs =
                !image.source_ref.empty() ? image.source_ref : image.display_name;
            (void)attachment_bag_.add_image(
                abs, image.display_name, image.mime_type, image.base64_data,
                static_cast<std::size_t>(
                    image.byte_size > 0 ? image.byte_size
                                        : static_cast<long long>(image.base64_data.size())),
                AttachmentSource::CliAttach, vision_model_ok);
        }
    }
    // attach_image tool: queue request-local images and inject after each tool round.
    std::vector<provider::ImageInput> pending_tool_images;
    std::size_t tool_images_attached = 0;
    const std::size_t max_tool_images_per_turn = 4;
    const std::size_t max_image_bytes =
        context.options.max_image_bytes > 0
            ? static_cast<std::size_t>(context.options.max_image_bytes)
            : 20U * 1024U * 1024U;
    {
        VisionAttachHooks vision;
        vision.max_image_bytes = max_image_bytes;
        vision.max_images_per_turn = max_tool_images_per_turn;
        vision.vision_capable = vision_model_ok;
        vision.attachment_bag = &attachment_bag_;
        vision.validate_capability = [&context]() -> Error {
            return provider::validate_image_input(context);
        };
        vision.queue_image = [&](provider::ImageInput image) -> Error {
            if (tool_images_attached >= max_tool_images_per_turn) {
                return {ErrorCode::UnsupportedFeature,
                        "attach_image limit reached for this turn (max " +
                            std::to_string(max_tool_images_per_turn) +
                            "); finish the turn or continue without more images"};
            }
            if (image.mime_type.empty() || image.base64_data.empty()) {
                return {ErrorCode::FileRead, "attach_image produced empty image data"};
            }
            Error capability = provider::validate_image_input(context);
            if (!capability.ok()) return capability;
            pending_tool_images.push_back(std::move(image));
            ++tool_images_attached;
            return ok_error();
        };
        tools_.set_vision_hooks(std::move(vision));
    }
    // Only inject pixels into the model when vision is supported.
    std::vector<provider::ImageInput> model_images;
    if (vision_model_ok) {
        model_images = payload.images;
    } else if (!payload.images.empty() && !context.options.quiet) {
        std::cerr << "Agent notice: " << payload.images.size()
                  << " image(s) attached for tools/MCP only (model is text-only; "
                     "pixels not sent to the model).\n";
    }
    // Lambdas above capture stack state; clear hooks on every exit path.
    struct ClearVisionHooks {
        ReadToolRegistry& tools;
        AttachmentBag& bag;
        mcp::ToolBridge* bridge;
        ~ClearVisionHooks() {
            tools.set_vision_hooks({});
            if (bridge) bridge->set_attachment_bag(nullptr);
            bag.clear();
        }
    } clear_vision_hooks{tools_, attachment_bag_, mcp_bridge_.get()};
    auto flush_pending_tool_images = [&]() {
        if (pending_tool_images.empty()) return;
        std::string note = "[Vision attachment for subsequent model rounds of this turn]";
        if (pending_tool_images.size() == 1 &&
            !pending_tool_images.front().display_name.empty()) {
            note += "\n" + pending_tool_images.front().display_name;
        } else {
            for (const provider::ImageInput& image : pending_tool_images) {
                if (image.display_name.empty()) continue;
                note += "\n- " + image.display_name;
            }
        }
        append_conversation_user_with_images(conversation_, note, pending_tool_images);
        pending_tool_images.clear();
        publish_request_token_estimate();
    };
    reset_agent_loop_for_user_turn(state_);
    result.turn_started_ms = now_unix_ms();
    // Prefer the per-turn callback (TUI streaming); fall back to prepare-time options.
    auto progress = [&](const std::string& line) {
        if (on_progress) {
            on_progress(line);
            return;
        }
        if (options_.on_progress) options_.on_progress(line);
    };
    auto publish_phase = [&](AgentActivityPhase phase) {
        if (options_.on_phase) options_.on_phase(phase);
    };
    auto structured_progress = [&](const AgentProgressUpdate& update) {
        if (on_structured_progress) {
            on_structured_progress(update);
            return;
        }
        if (options_.on_structured_progress) options_.on_structured_progress(update);
    };
    bool token_usage_logged = false;
    auto log_token_usage = [&]() {
        if (!logger_ || token_usage_logged || result.token_usage.reported_rounds == 0)
            return;
        token_usage_logged = true;
        json::Value fields = log_object();
        fields.object["model_rounds"] =
            log_number(result.token_usage.reported_rounds);
        fields.object["input_tokens"] =
            log_number(result.token_usage.input_tokens);
        fields.object["fresh_input_tokens"] =
            log_number(result.token_usage.fresh_input_tokens);
        fields.object["cache_read_tokens"] =
            log_number(result.token_usage.cache_read_tokens);
        fields.object["cache_write_tokens"] =
            log_number(result.token_usage.cache_write_tokens);
        fields.object["output_tokens"] =
            log_number(result.token_usage.output_tokens);
        logger_->event("agent_turn_usage", {"agent"}, std::move(fields), "success");
    };

    // First turn: open singleton project row and seed provider conversation.
    if (!conversation_seeded_) {
        if (session_store_.is_open()) {
            AgentProjectRecord project;
            project.status = "running";
            project.provider = context.profile.name;
            project.model = context.options.model;
            project.api =
                context.api_kind == provider::ApiKind::Responses ? "responses" : "chat";
            project.protocol = state_.protocol == ToolProtocol::Xml ? "xml" : "native";
            project.base_url = context.base_url;
            project.workspace = options_.workspace;
            Error error = session_store_.open_project(project);
            if (!error.ok()) {
                result.error = error;
                return result;
            }
            project.status = "running";
            project.provider = context.profile.name;
            project.model = context.options.model;
            project.api =
                context.api_kind == provider::ApiKind::Responses ? "responses" : "chat";
            project.protocol = state_.protocol == ToolProtocol::Xml ? "xml" : "native";
            project.base_url = context.base_url;
            Error settings_error = settings_json_with_permission_mode(
                chat::settings_json_from_options(context.options), permission_mode_,
                project.settings_json);
            if (!settings_error.ok()) {
                result.error = settings_error;
                return result;
            }
            settings_error = write_session_settings(project);
            if (!settings_error.ok()) {
                result.error = settings_error;
                return result;
            }
            project.workspace = options_.workspace;
            (void)session_store_.update_project_meta(project);
            session_id_ = 1;
            if (!context.options.quiet)
                std::cerr << "Agent project thread · " << session_store_.path() << "\n";
            if (logger_) {
                json::Value fields = log_object();
                fields.object["session_id"] = log_number(session_id_);
                fields.object["path"] = log_string(session_store_.path());
                logger_->event("agent_session", {"session"}, std::move(fields), "success");
            }
        }
        // Seed system (+ optional AGENTS.md). Interactive agent sessions resume
        // the project transcript as model context. Headless --run/--plan always
        // start with a fresh model conversation, while still using the durable
        // project DB for logging/display and the persistent code index.
        std::vector<AgentMessageRecord> prior;
        if (options_.interactive && session_store_.is_open())
            (void)session_store_.load_messages(prior);
        apply_context_reset_filter(prior);
        const std::string prior_context = build_prior_session_context(prior);
        std::vector<std::string> script_names;
        (void)list_project_scripts(options_.workspace, script_names);
        if (prior_context.empty()) {
            seed_agent_conversation(conversation_, prompts_, task_mode_, state_.protocol, text,
                                    agents_md_.injection_text, script_names);
        } else {
            seed_agent_conversation(conversation_, prompts_, task_mode_, state_.protocol, "",
                                    agents_md_.injection_text, script_names);
            conversation_.messages.push_back({"user", prior_context});
            conversation_.messages.push_back({"user", text});
            if (!context.options.quiet)
                std::cerr << "Injected prior agent transcript (" << prior.size()
                          << " stored messages) into model context.\n";
        }
        attach_images_to_last_user_message(conversation_, model_images);
        conversation_seeded_ = true;
        if (goal_is_active(goal_)) inject_active_goal_control(false);
        publish_request_token_estimate();
        if (session_store_.is_open() && session_id_ > 0) {
            // Durable transcript is text-only (image bytes never leave RAM).
            Error message_error =
                session_store_.append_message("user", redact_secrets(text, secrets_));
            if (!message_error.ok() && !context.options.quiet)
                std::cerr << "Agent warning: could not store goal message: "
                          << redact_secrets(message_error.message, secrets_) << "\n";
        }
        if (!context.options.quiet) {
            std::cerr << "Agent goal: " << redact_secrets(text, secrets_) << "\n"
                      << "Using " << context.profile.name << "/" << context.options.model
                      << " with protocol "
                      << (state_.protocol == ToolProtocol::Xml ? "xml" : "native")
                      << " (" << agent_task_mode_name(task_mode_) << " tools).\n";
            if (!payload.images.empty()) {
                std::cerr << "Attached " << payload.images.size()
                          << " image(s) for this turn only (not stored).\n";
            }
            if (!agents_md_.documents.empty()) {
                std::cerr << "Loaded project AGENTS.md (" << agents_md_.total_bytes << " bytes";
                if (agents_md_.truncated) std::cerr << ", truncated";
                std::cerr << ").\n";
            }
        }
    } else {
        // Tool/assistant history lives in continuation_items_json. Follow-up
        // user turns must append there so serialize_tool_request places them
        // after prior tool results (not between the seed goal and tools).
        if (goal_is_active(goal_)) inject_active_goal_control(false);
        append_conversation_user_with_images(conversation_, text, model_images);
        publish_request_token_estimate();
        if (session_store_.is_open() && session_id_ > 0) {
            Error message_error =
                session_store_.append_message("user", redact_secrets(text, secrets_));
            if (!message_error.ok() && !context.options.quiet)
                std::cerr << "Agent warning: could not store follow-up: "
                          << redact_secrets(message_error.message, secrets_) << "\n";
        }
    }

    // Auto and manual commands share one atomic transcript-preserving pipeline.
    if (options_.auto_compact && session_store_.is_open()) {
        SessionCompactionResult compact_result =
            compact_impl(context, CompactionReason::Automatic, cancellation);
        if (!compact_result.error.ok()) {
            result.error = compact_result.error;
            return result;
        }
        if (compact_result.compacted) {
            progress(compact_result.notice);
            if (!context.options.quiet)
                std::cerr << "Agent notice: " << compact_result.notice << "\n";
            result.notice = compact_result.notice;
        }
    }

    const std::size_t log_width = terminal_column_count();
    const std::size_t tool_line_width = log_width > 8 ? log_width : 8;
    std::size_t turn_tool_index = 0;
    std::size_t active_round_id = 0;
    constexpr std::size_t kWorkingNoticeId =
        std::numeric_limits<std::size_t>::max() - 1;
    constexpr std::size_t kResponseId =
        std::numeric_limits<std::size_t>::max() - 2;
    bool working_row_started = false;
    auto executor = [&](const std::string& name, const std::string& arguments_json,
                        runtime::CancellationToken token) {
        ++turn_tool_index;
        if (working_row_started) {
            structured_progress({AgentProgressAction::Discard, AgentProgressKind::Notice,
                                 active_round_id, kWorkingNoticeId, {}, 0});
            working_row_started = false;
        }
        const auto execution_started = std::chrono::steady_clock::now();
        const long long approval_before =
            guard_approval_wait_ms_.load(std::memory_order_relaxed);
        // Surface the call immediately so interactive UIs are not stuck on a blank
        // "streaming..." placeholder while the tool (or the next model round) runs.
        {
            std::ostringstream running;
            running << turn_tool_index << ": " << (name.empty() ? "tool" : name) << '('
                    << compact_tool_args_preview(arguments_json,
                                                 tool_line_width > 24 ? tool_line_width - 24 : 8)
                    << ") …";
            const std::string running_line = clip_to_cells(running.str(), tool_line_width);
            progress(running_line);
            structured_progress({AgentProgressAction::Upsert, AgentProgressKind::Tool,
                                 active_round_id, turn_tool_index, running_line, 0});
        }
        std::string body = tools_.execute(name, arguments_json, token);
        const long long approval_after =
            guard_approval_wait_ms_.load(std::memory_order_relaxed);
        const long long execution_ms = execution_only_elapsed_ms(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - execution_started)
                .count(),
            approval_before, approval_after);
        const long long completed_ms = now_unix_ms();
        const std::string line =
            format_compact_tool_line(turn_tool_index, name, arguments_json, body,
                                     execution_ms, tool_line_width);
        result.compact_tool_lines.push_back(line);
        result.compact_tool_line_ms.push_back(completed_ms);
        progress(line);
        structured_progress({AgentProgressAction::Commit, AgentProgressKind::Tool,
                             active_round_id, turn_tool_index, line, completed_ms});
        if (!options_.interactive && !context.options.quiet) {
            std::cerr << line << "\n";
        }
        if (session_store_.is_open()) {
            (void)session_store_.append_message(
                "tool", line, name, compact_tool_status(body) == "ok",
                compact_tool_args_preview(arguments_json));
        }
        if (options_.show_command_output && name == "run_command") {
            // Best-effort: surface truncated stdout when enabled.
            if (body.find("\"stdout\"") != std::string::npos) {
                // Keep short; full body stays in tool_events via append_tool_event.
            }
        }
        return body;
    };

    std::size_t turn_tool_calls = 0;
    std::size_t turn_failed_tool_calls = 0;
    std::string final_text;
    const std::size_t turns_before = state_.turn;
    bool counts_committed = false;
    auto commit_turn_counts = [&]() {
        if (!counts_committed) {
            session_turns_ = state_.turn;
            session_tool_calls_ += turn_tool_calls;
            session_failed_tool_calls_ += turn_failed_tool_calls;
            counts_committed = true;
        }
        result.turns = state_.turn - turns_before;
        result.tool_calls = turn_tool_calls;
        result.failed_tool_calls = turn_failed_tool_calls;
        result.session_turns = session_turns_;
        result.session_tool_calls = session_tool_calls_;
        result.session_failed_tool_calls = session_failed_tool_calls_;
    };
    auto log_turn_summary = [&](const std::string& status) {
        if (!logger_) return;
        json::Value fields = log_object();
        fields.object["turns"] = log_number(state_.turn - turns_before);
        fields.object["tool_calls"] = log_number(turn_tool_calls);
        fields.object["failed_tool_calls"] =
            log_number(turn_failed_tool_calls);
        logger_->event("agent_turn", {"agent"}, std::move(fields), status);
    };
    bool context_recovery_used = false;
    // Tool-less FinalText rounds while a session goal is Active. After one
    // auto-continue nudge, a second tool-less FinalText stalls (blocked/wait).
    int consecutive_tool_less_finals = 0;
    bool goal_completed_this_turn = false;
    // The scripted-round cap is per user-approved task segment. Keep
    // state_.turn cumulative for logs/session statistics.
    for (;;) {
        if (is_interrupted(cancellation, interrupted)) {
            pair_dangling_tool_calls(context, conversation_, state_);
            publish_request_token_estimate();
            result.error = {ErrorCode::Cancelled, "agent run cancelled"};
            result.final_text = final_text;
            commit_turn_counts();
            result.finished_at_ms = now_unix_ms();
            log_turn_summary("failure");
            log_token_usage();
            return result;
        }

        // Recheck before every provider request, including the round following
        // completed tools. This keeps automatic compaction aligned with actual
        // request growth rather than only user-message boundaries.
        if (options_.auto_compact && session_store_.is_open()) {
            SessionCompactionResult compact_result =
                compact_impl(context, CompactionReason::Automatic, cancellation);
            if (!compact_result.error.ok()) {
                result.error = compact_result.error;
                commit_turn_counts();
                result.finished_at_ms = now_unix_ms();
                log_turn_summary("failure");
                log_token_usage();
                return result;
            }
            if (compact_result.compacted) {
                progress(compact_result.notice);
                result.notice = compact_result.notice;
            }
        }

        // Native tools when protocol allows; empty definitions on pure XML channel.
        // Re-check hosted search here: the model/API may have changed since prepare.
        tools_.set_hosted_web_search(provider::hosted_web_search_enabled(context),
                                     provider::hosted_web_search_display_name(context));
        std::vector<provider::FunctionDefinition> definitions =
            state_.protocol == ToolProtocol::Xml ? std::vector<provider::FunctionDefinition>{}
                                                 : tools_.definitions();

        provider::ToolRoundResult round;
        active_round_id = state_.turn + 1;
        std::string round_reasoning;
        bool response_row_started = false;
        std::string round_preview;
        bool reasoning_row_started = false;
        const std::size_t thinking_preview_max_chars = static_cast<std::size_t>(
            std::max(0, context.options.agent_thinking_preview_max_chars));
        const int thinking_idle_seconds =
            std::max(0, context.options.agent_thinking_idle_preview_seconds);
        constexpr std::size_t kOpeningThinkingId = 0;
        constexpr std::size_t kFinishedThinkingId = 1;
        bool opening_thinking_frozen = false;
        bool finished_thinking_started = false;
        bool thinking_previews_finalized = false;
        std::string last_live_opening_preview;
        std::string last_live_finished_preview;
        auto last_thinking_idle_emit = std::chrono::steady_clock::now();
        const int thinking_token_refresh_seconds =
            std::max(0, context.options.agent_thinking_token_refresh_seconds);
        auto last_thinking_token_publish = std::chrono::steady_clock::time_point::min();
        clear_in_flight_generation_tokens();
        auto maybe_publish_in_flight_tokens = [&](const std::string& reasoning_so_far) {
            if (!options_.interactive || thinking_token_refresh_seconds <= 0) return;
            const auto now = std::chrono::steady_clock::now();
            if (last_thinking_token_publish !=
                std::chrono::steady_clock::time_point::min()) {
                const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                                         now - last_thinking_token_publish)
                                         .count();
                if (elapsed < thinking_token_refresh_seconds) return;
            }
            publish_in_flight_generation_tokens(
                estimate_tokens_from_text(reasoning_so_far));
            last_thinking_token_publish = now;
        };
        if (working_row_started) {
            structured_progress({AgentProgressAction::Discard, AgentProgressKind::Notice,
                                 active_round_id, kWorkingNoticeId, {}, 0});
            working_row_started = false;
        }
        publish_phase(AgentActivityPhase::Thinking);
        ReviewLogContext log_context("agent");
        log_context.round = state_.turn + 1;
        log_context.cumulative_tool_calls = session_tool_calls_ + turn_tool_calls;
        provider::ToolRoundObserver observer;
        const provider::ToolRoundObserver* observer_pointer = nullptr;
        if (logger_) {
            observer = logger_->tool_round_observer();
            observer_pointer = &observer;
        }

        const provider::ToolRoundContext observation_context = [&]() {
                provider::ToolRoundContext ctx;
                ctx.stage = log_context.stage;
                ctx.round = log_context.round;
                ctx.cumulative_tool_calls = log_context.cumulative_tool_calls;
                return ctx;
            }();
        bool retry_notice_active = false;
        auto on_retry = [&](const Error& retry_error, int attempt,
                            int backoff_seconds) {
                if (response_row_started) {
                    structured_progress({AgentProgressAction::Discard,
                                         AgentProgressKind::Response, active_round_id,
                                         kResponseId, {}, 0});
                    response_row_started = false;
                }
                round_reasoning.clear();
                round_preview.clear();
                reasoning_row_started = false;
                opening_thinking_frozen = false;
                finished_thinking_started = false;
                thinking_previews_finalized = false;
                last_live_opening_preview.clear();
                last_live_finished_preview.clear();
                last_thinking_idle_emit = std::chrono::steady_clock::now();
                last_thinking_token_publish =
                    std::chrono::steady_clock::time_point::min();
                clear_in_flight_generation_tokens();
                if (working_row_started) {
                    structured_progress({AgentProgressAction::Discard,
                                         AgentProgressKind::Notice, active_round_id,
                                         kWorkingNoticeId, {}, 0});
                    working_row_started = false;
                }
                retry_notice_active = true;
                std::string retry_notice =
                    "Waiting for provider · retry " +
                    std::to_string(attempt) + " in " +
                    std::to_string(backoff_seconds) + "s";
                if (!retry_error.message.empty())
                    retry_notice += " · " +
                                    redact_secrets(retry_error.message, secrets_);
                structured_progress(
                    {AgentProgressAction::Upsert,
                     AgentProgressKind::Notice, active_round_id,
                     std::numeric_limits<std::size_t>::max(),
                     retry_notice, now_unix_ms()});
                if (!logger_) return;
                json::Value fields = log_object();
                fields.object["error_code"] = log_string(error_code_name(retry_error.code));
                fields.object["error_message"] = log_string(retry_error.message);
                fields.object["attempt"] = log_number(attempt);
                fields.object["backoff_ms"] = log_number(backoff_seconds * 1000);
                logger_->event("retry_scheduled", log_context, std::move(fields), "failure");
            };
        auto clear_retry_notice = [&]() {
            if (!retry_notice_active) return;
            structured_progress(
                {AgentProgressAction::Discard,
                 AgentProgressKind::Notice, active_round_id,
                 std::numeric_limits<std::size_t>::max(), {}, 0});
            retry_notice_active = false;
        };
        auto clip_thinking_line = [&](const std::string& line) {
            return clip_thinking_preview_line(line, tool_line_width);
        };
        auto persist_thinking_line = [&](const std::string& line) {
            if (session_store_.is_open() && session_id_ > 0)
                (void)session_store_.append_message("thinking", line);
        };
        auto publish_thinking = [&](AgentProgressAction action, std::size_t tool_id,
                                    const std::string& text, long long created_at_ms = 0) {
            structured_progress({action, AgentProgressKind::Thinking, active_round_id,
                                 tool_id, text, created_at_ms});
        };
        auto commit_thinking_line = [&](std::size_t tool_id, const std::string& line) {
            if (line.empty()) return;
            const long long frozen_ms = now_unix_ms();
            publish_thinking(AgentProgressAction::Commit, tool_id, line, frozen_ms);
            persist_thinking_line(line);
        };
        auto discard_thinking_line = [&](std::size_t tool_id) {
            publish_thinking(AgentProgressAction::Discard, tool_id, {});
        };
        auto hide_working_row = [&]() {
            if (!working_row_started) return;
            structured_progress({AgentProgressAction::Discard, AgentProgressKind::Notice,
                                 active_round_id, kWorkingNoticeId, {}, 0});
            working_row_started = false;
        };
        auto finalize_thinking_previews = [&]() {
            if (thinking_previews_finalized) return;
            thinking_previews_finalized = true;
            if (!options_.interactive || thinking_preview_max_chars == 0) {
                if (finished_thinking_started)
                    discard_thinking_line(kFinishedThinkingId);
                if (reasoning_row_started && !opening_thinking_frozen)
                    discard_thinking_line(kOpeningThinkingId);
                return;
            }
            const std::string opening = clip_thinking_line(format_thinking_opening_preview(
                round_reasoning, thinking_preview_max_chars, secrets_));
            if (!opening.empty() && !opening_thinking_frozen)
                commit_thinking_line(kOpeningThinkingId, opening);
            else if (opening.empty() && reasoning_row_started &&
                     !opening_thinking_frozen)
                discard_thinking_line(kOpeningThinkingId);
            if (!round_reasoning.empty() &&
                !skip_finished_thinking_preview(round_reasoning, thinking_preview_max_chars,
                                                secrets_)) {
                const std::string finished = clip_thinking_line(
                    format_finished_thinking_preview(round_reasoning,
                                                     thinking_preview_max_chars,
                                                     secrets_));
                if (!finished.empty())
                    commit_thinking_line(kFinishedThinkingId, finished);
                else if (finished_thinking_started)
                    discard_thinking_line(kFinishedThinkingId);
            } else if (finished_thinking_started) {
                discard_thinking_line(kFinishedThinkingId);
            }
        };
        auto on_working = [&]() -> Error {
            if (!options_.interactive) return ok_error();
            finalize_thinking_previews();
            publish_phase(AgentActivityPhase::Working);
            if (!working_row_started) {
                structured_progress({AgentProgressAction::Upsert,
                                     AgentProgressKind::Notice, active_round_id,
                                     kWorkingNoticeId, "Working: ", 0});
                working_row_started = true;
            }
            return cancellation.cancelled()
                       ? Error{ErrorCode::Cancelled, "agent working preview cancelled"}
                       : ok_error();
        };
        auto upsert_live_thinking_tail = [&]() {
            const std::string opening_body = thinking_opening_body(
                round_reasoning, thinking_preview_max_chars, secrets_);
            const std::string tail = format_live_thinking_tail(
                round_reasoning, thinking_preview_max_chars, secrets_);
            if (tail.empty() || tail == opening_body) {
                if (finished_thinking_started) {
                    discard_thinking_line(kFinishedThinkingId);
                    finished_thinking_started = false;
                    last_live_finished_preview.clear();
                }
                return;
            }
            const std::string live = clip_thinking_line(tail);
            if (live.empty()) return;
            if (live != last_live_finished_preview) {
                publish_thinking(AgentProgressAction::Upsert, kFinishedThinkingId, live);
                last_live_finished_preview = live;
                finished_thinking_started = true;
            }
        };
        auto on_reasoning = [&](const std::string& delta) -> Error {
                if (!options_.interactive) return ok_error();
                round_reasoning += delta;
                // Throttled chrome meter: request size + local in-flight
                // reasoning estimate. Display-only; not compaction input.
                maybe_publish_in_flight_tokens(round_reasoning);
                if (thinking_preview_max_chars == 0) {
                    return cancellation.cancelled()
                               ? Error{ErrorCode::Cancelled,
                                       "agent reasoning preview cancelled"}
                               : ok_error();
                }

                const std::string opening = clip_thinking_line(format_thinking_opening_preview(
                    round_reasoning, thinking_preview_max_chars, secrets_));
                if (opening.empty()) {
                    return cancellation.cancelled()
                               ? Error{ErrorCode::Cancelled,
                                       "agent reasoning preview cancelled"}
                               : ok_error();
                }

                const auto now = std::chrono::steady_clock::now();
                if (!opening_thinking_frozen) {
                    if (!reasoning_row_started) last_thinking_idle_emit = now;
                    if (opening != last_live_opening_preview) {
                        publish_thinking(AgentProgressAction::Upsert, kOpeningThinkingId,
                                         opening);
                        last_live_opening_preview = opening;
                        round_preview = opening;
                        reasoning_row_started = true;
                    }
                    const bool more_after_opening = opening_preview_has_more(
                        round_reasoning, thinking_preview_max_chars, secrets_);
                    const bool idle_due =
                        thinking_idle_seconds > 0 &&
                        std::chrono::duration_cast<std::chrono::seconds>(
                            now - last_thinking_idle_emit)
                                .count() >= thinking_idle_seconds;
                    const bool freeze_now = thinking_idle_seconds <= 0
                                                ? more_after_opening
                                                : (idle_due || more_after_opening);
                    if (freeze_now) {
                        commit_thinking_line(kOpeningThinkingId, opening);
                        opening_thinking_frozen = true;
                        last_thinking_idle_emit = now;
                    }
                }

                if (opening_thinking_frozen) upsert_live_thinking_tail();

                return cancellation.cancelled()
                           ? Error{ErrorCode::Cancelled,
                                   "agent reasoning preview cancelled"}
                           : ok_error();
            };
        auto on_content = [&](const std::string& delta) -> Error {
                if (!options_.interactive || delta.empty()) return ok_error();
                structured_progress({AgentProgressAction::Append,
                                     AgentProgressKind::Response, active_round_id,
                                     kResponseId, delta, 0});
                response_row_started = true;
                return cancellation.cancelled()
                           ? Error{ErrorCode::Cancelled,
                                   "agent response preview cancelled"}
                           : ok_error();
            };
        Error error = send_tool_round_with_transport_retries(
            context, conversation_, definitions, round, cancellation,
            limits_.transport_attempts, observer_pointer, observation_context,
            on_retry, on_reasoning, on_working, on_content);
        clear_retry_notice();
        if (!error.ok() && options_.auto_compact &&
            options_.compact_strategy != CompactionStrategy::Fast &&
            !context_recovery_used && context_length_error(error)) {
            context_recovery_used = true;
            SessionCompactionResult recovered = compact_impl(
                context, CompactionReason::Automatic, cancellation,
                options_.compact_strategy, true);
            if (recovered.error.ok() && recovered.compacted) {
                progress(recovered.notice + "; retrying rejected model round once");
                round = provider::ToolRoundResult{};
                round_reasoning.clear();
                if (response_row_started) {
                    structured_progress({AgentProgressAction::Discard,
                                         AgentProgressKind::Response, active_round_id,
                                         kResponseId, {}, 0});
                    response_row_started = false;
                }
                round_preview.clear();
                reasoning_row_started = false;
                opening_thinking_frozen = false;
                finished_thinking_started = false;
                thinking_previews_finalized = false;
                last_live_opening_preview.clear();
                last_live_finished_preview.clear();
                last_thinking_idle_emit = std::chrono::steady_clock::now();
                last_thinking_token_publish =
                    std::chrono::steady_clock::time_point::min();
                clear_in_flight_generation_tokens();
                hide_working_row();
                error = send_tool_round_with_transport_retries(
                    context, conversation_, definitions, round, cancellation,
                    limits_.transport_attempts, observer_pointer,
                    observation_context, on_retry, on_reasoning, on_working,
                    on_content);
                clear_retry_notice();
            } else if (!recovered.error.ok()) {
                error = recovered.error;
            }
        }
        if (!error.ok()) {
            if (response_row_started) {
                structured_progress({AgentProgressAction::Discard,
                                     AgentProgressKind::Response, active_round_id,
                                     kResponseId, {}, 0});
                response_row_started = false;
            }
            hide_working_row();
            if (finished_thinking_started)
                discard_thinking_line(kFinishedThinkingId);
            if (reasoning_row_started && !opening_thinking_frozen)
                discard_thinking_line(kOpeningThinkingId);
            clear_in_flight_generation_tokens();
            pair_dangling_tool_calls(context, conversation_, state_);
            publish_request_token_estimate();
            result.error = error;
            result.final_text = final_text;
            commit_turn_counts();
            result.finished_at_ms = now_unix_ms();
            log_turn_summary("failure");
            log_token_usage();
            return result;
        }
        if (options_.interactive && round_reasoning.empty())
            round_reasoning = round.reasoning_text;
        finalize_thinking_previews();
        if (!round.hosted_search_queries.empty()) {
            std::string notice = "web_search";
            for (const std::string& query : round.hosted_search_queries) {
                notice += " · " + query;
            }
            structured_progress({AgentProgressAction::Commit, AgentProgressKind::Notice,
                                 active_round_id, kWorkingNoticeId + 1, notice,
                                 now_unix_ms()});
        }
        if (round.tool_calls.empty()) hide_working_row();

        long long estimated_round_input_tokens = 0;
        if (round.metrics.prompt_tokens < 0) {
            const std::string serialized_request =
                provider::serialize_tool_request(context, conversation_, definitions);
            estimated_round_input_tokens =
                serialized_request == "{}"
                    ? estimated_request_tokens()
                    : estimate_tokens_from_text(serialized_request);
        }
        long long estimated_round_output_tokens = round.metrics.completion_tokens;
        if (estimated_round_output_tokens <= 0) {
            for (const std::string& item : round.continuation_items_json)
                estimated_round_output_tokens += estimate_tokens_from_text(item);
            if (estimated_round_output_tokens <= 0)
                estimated_round_output_tokens = estimate_tokens_from_text(round.content);
        }

        if (!round.tool_calls.empty()) publish_phase(AgentActivityPhase::Working);
        accumulate_agent_token_usage(round.metrics, result.token_usage,
                                     estimated_round_input_tokens,
                                     estimated_round_output_tokens);
        accumulate_agent_stream_decode(round.metrics, estimated_round_output_tokens,
                                       context.options.stream, result.stream_output_tokens,
                                       result.stream_decode_ms,
                                       result.stream_tokens_estimated);
        // In-flight reasoning is not retained in the request projection; drop
        // the live meter before republishing the true next-request estimate.
        clear_in_flight_generation_tokens();
        const provider::ChatResult round_metrics = round.metrics;
        AgentRoundOutcome outcome = handle_agent_tool_round(
            state_, limits_, context, conversation_, std::move(round), known_tools_, executor,
            cancellation);
        if (response_row_started) {
            structured_progress({AgentProgressAction::Discard,
                                 AgentProgressKind::Response, active_round_id,
                                 kResponseId, {}, 0});
            response_row_started = false;
        }
        turn_tool_calls += outcome.tool_calls;
        turn_failed_tool_calls += outcome.failed_tool_calls;
        // Conversation grew (assistant/tool items); publish for TUI chrome.
        publish_request_token_estimate();

        if (logger_) {
            json::Value fields = log_object();
            fields.object["outcome"] = log_string(
                outcome.kind == AgentRoundOutcome::Kind::Continue           ? "continue"
                : outcome.kind == AgentRoundOutcome::Kind::FinalText       ? "final_text"
                : outcome.kind == AgentRoundOutcome::Kind::Aborted         ? "aborted"
                : outcome.kind == AgentRoundOutcome::Kind::NeedsUserContinue ? "needs_user_continue"
                                                                            : "error");
            fields.object["tool_result_count"] = log_number(outcome.tool_results.size());
            fields.object["tool_calls"] = log_number(outcome.tool_calls);
            fields.object["failed_tool_calls"] =
                log_number(outcome.failed_tool_calls);
            fields.object["protocol"] =
                log_string(state_.protocol == ToolProtocol::Xml ? "xml" : "native");
            fields.object["input_tokens"] = log_number(round_metrics.prompt_tokens);
            fields.object["fresh_input_tokens"] =
                log_number(round_metrics.fresh_prompt_tokens);
            fields.object["cache_read_tokens"] =
                log_number(round_metrics.cache_read_tokens);
            fields.object["cache_write_tokens"] =
                log_number(round_metrics.cache_write_tokens);
            fields.object["output_tokens"] =
                log_number(round_metrics.completion_tokens);
            if (!outcome.notice.empty())
                fields.object["notice"] = ReviewLogger::payload(outcome.notice);
            if (!outcome.final_text.empty())
                fields.object["final_text"] = ReviewLogger::payload(outcome.final_text);
            if (!outcome.error.ok()) {
                fields.object["error_code"] = log_string(error_code_name(outcome.error.code));
                fields.object["error_message"] = log_string(outcome.error.message);
            }
            logger_->event("agent_round", log_context, std::move(fields),
                           outcome.error.ok() ||
                                   outcome.kind == AgentRoundOutcome::Kind::Continue ||
                                   outcome.kind == AgentRoundOutcome::Kind::FinalText ||
                                   outcome.kind == AgentRoundOutcome::Kind::NeedsUserContinue
                               ? "success"
                               : "failure");
            for (std::size_t i = 0; i < outcome.prepared_calls.size(); ++i) {
                json::Value tool_fields = log_object();
                tool_fields.object["call_id"] = log_string(outcome.prepared_calls[i].id);
                tool_fields.object["tool_name"] = log_string(outcome.prepared_calls[i].name);
                tool_fields.object["arguments"] =
                    ReviewLogger::payload(outcome.prepared_calls[i].original_arguments);
                if (i < outcome.tool_results.size())
                    tool_fields.object["result"] =
                        ReviewLogger::payload(outcome.tool_results[i]);
                const bool tool_ok =
                    i < outcome.tool_results.size() &&
                    normalized_tool_result_ok(outcome.tool_results[i]);
                logger_->event("tool_result", log_context,
                               std::move(tool_fields),
                               tool_ok ? "success" : "failure");
            }
        }

        if (session_store_.is_open() && session_id_ > 0) {
            if (!outcome.notice.empty()) {
                Error notice_error =
                    session_store_.append_message("notice", redact_secrets(outcome.notice, secrets_));
                if (!notice_error.ok() && !context.options.quiet)
                    std::cerr << "Agent warning: could not store notice: "
                              << redact_secrets(notice_error.message, secrets_) << "\n";
            }
            for (std::size_t i = 0; i < outcome.prepared_calls.size(); ++i) {
                const std::string& result_body =
                    i < outcome.tool_results.size() ? outcome.tool_results[i] : std::string{};
                const bool tool_ok =
                    normalized_tool_result_ok(result_body);
                Error tool_error = session_store_.append_tool_event(
                    session_id_, static_cast<long long>(state_.turn),
                    outcome.prepared_calls[i].id, outcome.prepared_calls[i].name,
                    redact_secrets(outcome.prepared_calls[i].original_arguments, secrets_),
                    redact_secrets(result_body, secrets_), tool_ok);
                if (!tool_error.ok() && !context.options.quiet)
                    std::cerr << "Agent warning: could not store tool event: "
                              << redact_secrets(tool_error.message, secrets_) << "\n";
            }
        }

        {
            std::ostringstream progress_line;
            progress_line << "Agent turn " << state_.turn << " ("
                          << (state_.protocol == ToolProtocol::Xml ? "xml" : "native") << "): ";
            if (outcome.kind == AgentRoundOutcome::Kind::FinalText) {
                progress_line << "final answer (" << outcome.final_text.size() << " bytes)";
            } else if (outcome.kind == AgentRoundOutcome::Kind::Continue) {
                if (outcome.prepared_calls.empty()) {
                    progress_line << "continue";
                } else {
                    progress_line << outcome.prepared_calls.size() << " tool call(s):";
                    for (std::size_t i = 0; i < outcome.prepared_calls.size(); ++i) {
                        progress_line << " " << outcome.prepared_calls[i].name;
                        if (i < outcome.tool_results.size()) {
                            const std::string& body = outcome.tool_results[i];
                            const bool ok =
                                normalized_tool_result_ok(body);
                            progress_line << (ok ? "[ok]" : "[err]");
                        }
                    }
                }
            } else if (outcome.kind == AgentRoundOutcome::Kind::NeedsUserContinue) {
                progress_line << "needs user continue";
            } else {
                progress_line << "stop ("
                              << (outcome.kind == AgentRoundOutcome::Kind::Aborted ? "aborted"
                                                                                  : "error")
                              << ")";
            }
            const std::string line = clip_to_cells(progress_line.str(), log_width);
            progress(line);
            if (!context.options.quiet && !options_.interactive) std::cerr << line << "\n";
            if (!outcome.notice.empty() && !context.options.quiet && !options_.interactive) {
                const std::string notice =
                    clip_to_cells("Agent notice: " + redact_secrets(outcome.notice, secrets_),
                                  log_width);
                std::cerr << notice << "\n";
            }
        }

        if (outcome.kind == AgentRoundOutcome::Kind::Continue) {
            // Any tool round (including failed) resets the tool-less stall counter.
            consecutive_tool_less_finals = 0;
            if (goal_.status == GoalStatus::Complete) goal_completed_this_turn = true;
            // Inject attach_image payloads after tool results so the next model
            // round sees multimodal content (still stripped at end of turn).
            flush_pending_tool_images();
            continue;
        }

        if (outcome.kind == AgentRoundOutcome::Kind::FinalText) {
            final_text = outcome.final_text;
            if (goal_.status == GoalStatus::Complete) goal_completed_this_turn = true;

            // Active goal: auto-continue after tool-less FinalText until goal_met,
            // stall (2 tool-less finals), max_goal_turns, or cancel.
            if (goal_is_active(goal_) && !goal_completed_this_turn) {
                ++consecutive_tool_less_finals;
                const int max_goal_turns =
                    options_.max_goal_turns > 0 ? options_.max_goal_turns : 20;
                if (consecutive_tool_less_finals >= 2) {
                    result.goal_stalled = true;
                    const std::string stall_notice =
                        "Goal still active (agent stopped; blocked or waiting for user): " +
                        bound_goal_text(goal_.condition, 160);
                    result.notice = result.notice.empty()
                                        ? stall_notice
                                        : result.notice + "\n" + stall_notice;
                    if (session_store_.is_open())
                        (void)session_store_.append_message("notice", stall_notice);
                    // Fall through to normal FinalText completion (no more auto-continue).
                } else if (goal_.turns >= max_goal_turns) {
                    const std::string cap_notice =
                        "Goal still active (auto-continue cap of " +
                        std::to_string(max_goal_turns) + " reached)";
                    result.notice = result.notice.empty()
                                        ? cap_notice
                                        : result.notice + "\n" + cap_notice;
                    if (session_store_.is_open())
                        (void)session_store_.append_message("notice", cap_notice);
                } else {
                    ++goal_.turns;
                    (void)persist_goal_settings();
                    if (session_store_.is_open() && session_id_ > 0 &&
                        !final_text.empty()) {
                        (void)session_store_.append_message(
                            "assistant", redact_secrets(final_text, secrets_));
                    }
                    if (session_store_.is_open()) {
                        (void)session_store_.append_message(
                            "notice", "Continuing active goal…");
                    }
                    inject_active_goal_control(true);
                    progress("Continuing active goal…");
                    continue;  // next model round without returning
                }
            }

            commit_turn_counts();
            result.error = ok_error();
            result.final_text = final_text;
            result.goal_completed = goal_completed_this_turn ||
                                    goal_.status == GoalStatus::Complete;
            const Error final_index_error =
                tools_.refresh_persistent_index(true, cancellation);
            if (!final_index_error.ok() &&
                final_index_error.code != ErrorCode::Cancelled) {
                const std::string index_notice =
                    "final code-index refresh failed: " +
                    redact_secrets(final_index_error.message, secrets_);
                result.notice = result.notice.empty()
                                    ? index_notice
                                    : result.notice + "\n" + index_notice;
                if (!context.options.quiet)
                    std::cerr << "Agent warning: " << index_notice << "\n";
                if (logger_) {
                    json::Value fields = log_object();
                    fields.object["error_code"] =
                        log_string(error_code_name(final_index_error.code));
                    fields.object["error_message"] =
                        log_string(final_index_error.message);
                    logger_->event("final_index_refresh", {"index"},
                                   std::move(fields), "failure");
                }
            }
            result.finished_at_ms = now_unix_ms();
            if (session_store_.is_open() && session_id_ > 0 && !final_text.empty()) {
                Error assistant_error =
                    session_store_.append_message("assistant", redact_secrets(final_text, secrets_));
                if (!assistant_error.ok() && !context.options.quiet)
                    std::cerr << "Agent warning: could not store assistant message: "
                              << redact_secrets(assistant_error.message, secrets_) << "\n";
            }
            publish_request_token_estimate();
            log_turn_summary("success");
            log_token_usage();
            return result;
        }

        commit_turn_counts();

        if (outcome.kind == AgentRoundOutcome::Kind::NeedsUserContinue) {
            result.error = ok_error();
            result.needs_user_continue = true;
            result.notice = outcome.notice.empty()
                                ? "Agent turn limit reached; send another message to continue."
                                : outcome.notice;
            result.final_text = result.notice;
            result.finished_at_ms = now_unix_ms();
            if (session_store_.is_open() && session_id_ > 0) {
                (void)session_store_.append_message("notice",
                                                    redact_secrets(result.notice, secrets_));
            }
            publish_request_token_estimate();
            log_turn_summary("success");
            log_token_usage();
            return result;
        }

        pair_dangling_tool_calls(context, conversation_, state_);
        publish_request_token_estimate();
        result.error = outcome.error.ok()
                           ? Error{ErrorCode::Cancelled,
                                   outcome.notice.empty() ? "agent run aborted" : outcome.notice}
                           : outcome.error;
        if (result.error.message.empty() && !outcome.notice.empty())
            result.error.message = outcome.notice;
        // Prefer a concrete failure explanation over an empty final answer.
        if (final_text.empty() && !result.error.message.empty())
            final_text = result.error.message;
        result.final_text = final_text;
        result.notice = outcome.notice.empty() ? result.error.message : outcome.notice;
        result.finished_at_ms = now_unix_ms();
        if (session_store_.is_open() && session_id_ > 0 && !result.notice.empty()) {
            (void)session_store_.append_message("notice",
                                                redact_secrets(result.notice, secrets_));
        }
        log_turn_summary("failure");
        log_token_usage();
        return result;
    }
}

}  // namespace ainiux::agent
