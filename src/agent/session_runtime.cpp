#include "agent/session_runtime.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <limits>
#include <sstream>
#include <utility>
#include <unistd.h>

#include "agent/agent_loop.hpp"
#include "agent/compact.hpp"
#include "agent/project_root.hpp"
#include "agent/project_settings.hpp"
#include "agent/reasoning_preview.hpp"
#include "agent/tool_display.hpp"
#include "chat/settings.hpp"
#include "config/model_catalog.hpp"
#include "security/redact.hpp"

namespace ainiux::agent {
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

bool AgentSessionRuntime::is_interrupted(runtime::CancellationToken cancellation,
                                         const std::function<bool()>& interrupted) const {
    if (cancellation.cancelled()) return true;
    if (interrupted && interrupted()) return true;
    return false;
}

long long AgentSessionRuntime::estimated_request_tokens() const {
    return cached_request_tokens_.load(std::memory_order_relaxed);
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
    project.workspace = options_.workspace;
    return session_store_.update_project_meta(project);
}

void AgentSessionRuntime::publish_request_token_estimate() {
    // Must only run on the agent worker (or while no concurrent turn is active).
    long long total = 0;
    for (const provider::Message& message : conversation_.messages) {
        total += estimate_tokens_from_text(message.role);
        total += estimate_tokens_from_text(message.content);
        total += 4;
    }
    for (const std::string& item : conversation_.continuation_items_json) {
        total += estimate_tokens_from_text(item);
        total += 2;
    }
    cached_request_tokens_.store(total, std::memory_order_relaxed);
}

void AgentSessionRuntime::rebuild_compacted_conversation(
    const CompactionPartition& partition, const std::string& checkpoint) {
    seed_agent_conversation(conversation_, prompts_, task_mode_, state_.protocol, "",
                            agents_md_.injection_text);
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

    const long long window = context.options.context_tokens > 0
                                 ? static_cast<long long>(
                                       context.options.context_tokens)
                                 : 0LL;
    result.tokens_before = estimated_request_tokens();
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
        partition, window > 0 ? std::max<long long>(512, window * 10 / 100)
                              : 1000);
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

    if (use_model) {
        applied = CompactionStrategy::Summary;
        const CompactionSummaryCall summary_call =
            options_.summary_call ? options_.summary_call
                                  : default_compaction_summary_call;
        std::string source =
            redact_secrets(render_compaction_source(partition), secrets_);
        const std::string system =
            compaction_summary_schema_prompt(summary_preamble);
        const long long input_budget = compaction_summary_input_budget(window);
        std::vector<std::string> chunks = utf8_chunks(source, input_budget);
        if (chunks.empty()) chunks.push_back(source);
        std::vector<std::string> summaries;
        for (const std::string& chunk : chunks) {
            if (cancellation.cancelled()) {
                result.error = {ErrorCode::Cancelled,
                                "agent compaction cancelled"};
                return result;
            }
            std::string summary;
            const int output_budget = static_cast<int>(
                compaction_summary_output_budget(
                    estimate_tokens_from_text(chunk), window));
            result.error = summary_call(
                context,
                {{"system", system},
                 {"user", "Chronological history to summarize:\n" + chunk}},
                output_budget, cancellation, summary);
            if (!result.error.ok()) return failed_result();
            summary = ascii_trim(redact_secrets(summary, secrets_));
            if (summary.empty()) {
                result.error = {ErrorCode::ProviderSchema,
                                "agent compaction summarizer returned an empty checkpoint"};
                return failed_result();
            }
            summaries.push_back(std::move(summary));
        }
        while (summaries.size() > 1) {
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
                    return failed_result();
                }
                std::string summary;
                result.error = summary_call(
                    context,
                    {{"system", system},
                     {"user", "Consolidate these chunk checkpoints without losing "
                              "unfinished work:\n" +
                                  chunk}},
                    static_cast<int>(compaction_summary_output_budget(
                        estimate_tokens_from_text(chunk), window)),
                    cancellation, summary);
                if (!result.error.ok()) return failed_result();
                summary = ascii_trim(redact_secrets(summary, secrets_));
                if (summary.empty()) {
                    result.error = {
                        ErrorCode::ProviderSchema,
                        "agent compaction consolidation returned an empty checkpoint"};
                    return failed_result();
                }
                next.push_back(std::move(summary));
            }
            if (next.size() >= summaries.size() && next.size() > 1) {
                result.error = {
                    ErrorCode::ProviderSchema,
                    "agent compaction could not consolidate the oversized history"};
                return failed_result();
            }
            summaries = std::move(next);
        }
        checkpoint = summaries.front();
    }

    if (cancellation.cancelled()) {
        result.error = {ErrorCode::Cancelled, "agent compaction cancelled"};
        return result;
    }

    result.applied_strategy = applied;
    // Project the replacement before committing; ineffective LLM output is not
    // allowed to alter SQLite or the live request.
    const provider::ToolConversation old_conversation = conversation_;
    rebuild_compacted_conversation(partition, checkpoint);
    result.tokens_after = estimated_request_tokens();
    conversation_ = old_conversation;
    publish_request_token_estimate();
    if (result.tokens_before > 0 &&
        result.tokens_after >= result.tokens_before) {
        result.error = {ErrorCode::ProviderSchema,
                        "agent compaction did not reduce the model-visible context"};
        return failed_result();
    }
    if (applied == CompactionStrategy::Summary && window > 0) {
        const long long fit_limit =
            std::min<long long>(window * 60 / 100,
                                trigger > 0 ? trigger : window);
        if (result.tokens_after > fit_limit) {
            result.error = {
                ErrorCode::ProviderSchema,
                "agent summary compaction projection still exceeds its context budget"};
            return failed_result();
        }
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
    last_auto_compact_failure_ms_ = 0;
    last_auto_compact_failure_seq_ = 0;
    result.notice =
        format_compaction_success_notice(result.tokens_before,
                                         result.tokens_after);
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
                ? SessionRuntimeOptions::IndexMode::CreateOrRefresh
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
            fs::remove_all(fs::path(target.state_dir), cleanup_ec);
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
                fs::rename(backup, fs::path(target.state_dir), cleanup_ec);
                restored_state = !cleanup_ec;
            }
            if (!restored_state) {
                if (!rollback_detail.empty()) rollback_detail += "; ";
                rollback_detail +=
                    "could not restore prior state from " + backup.string() + " to " +
                    target.state_dir +
                    (cleanup_ec ? ": " + cleanup_ec.message()
                                : std::string(": backup is unavailable"));
            }
        }
        if (created_root) {
            cleanup_ec.clear();
            if (fs::is_empty(fs::path(target.root), cleanup_ec) && !cleanup_ec)
                fs::remove(fs::path(target.root), cleanup_ec);
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
        created_root = fs::create_directory(fs::path(target.root), ec);
        if (ec || !created_root) {
            reopen_old({ErrorCode::FileWrite,
                        "could not create /new project directory " + target.root + ": " +
                            (ec ? ec.message() : std::string("creation failed"))});
            return result;
        }
    }

    if (target.state_dir_exists) {
        const fs::path root(target.root);
        for (unsigned attempt = 0; attempt < 100; ++attempt) {
            backup = root / (std::string(".ainiux-pr.ainiux-new-backup-") +
                             std::to_string(static_cast<long long>(::getpid())) + "-" +
                             std::to_string(attempt));
            if (!fs::exists(backup, ec)) break;
            ec.clear();
        }
        fs::rename(fs::path(target.state_dir), backup, ec);
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
                             backup.string() + ": " + ec.message();
    }
    result.workspace = target.root;
    result.error = ok_error();
    return result;
}

void AgentSessionRuntime::reset() {
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
    cached_request_tokens_.store(0, std::memory_order_relaxed);
    guard_approval_wait_ms_.store(0, std::memory_order_relaxed);
    operation_active_.store(false, std::memory_order_relaxed);
}

Error AgentSessionRuntime::prepare(const provider::RequestContext& context,
                                   runtime::CancellationToken cancellation,
                                   std::function<bool()> interrupted,
                                   SessionRuntimeOptions options) {
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
    // segfault during project_overview → check_freshness (stack-use-after-scope).
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

    index::Options index_options;
    index_options.workspace = options_.workspace;
    index_options.max_source_code_file_size = options_.max_source_code_file_size;
    index_options.cancellation = cancel_copy;
    index_options.interrupted = interrupted_fn;
    index_options.on_progress = options_.on_index_progress;
    index::RefreshStats index_stats;
    Error error = ok_error();
    bool indexing_enabled = false;
    const bool indexing_requested =
        options_.index_mode != SessionRuntimeOptions::IndexMode::Disabled;
    if (indexing_requested) {
        index::ProbeResult probe;
        error = index::probe(index_options, probe);
        if (error.ok() && probe.state == index::ProbeState::Corrupt)
            error = probe.error;
        if (error.ok() &&
            probe.state == index::ProbeState::MissingOrIncomplete &&
            options_.index_mode ==
                SessionRuntimeOptions::IndexMode::UseExisting) {
            if (!context.options.quiet)
                std::cerr << "Code index not found; continuing without indexed tools.\n";
        } else if (error.ok()) {
            error = index::refresh(index_options, index_stats);
            indexing_enabled = error.ok();
        }
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
        }
    }
    if (logger_) {
        json::Value fields = log_object();
        fields.object["indexing_enabled"] =
            log_bool(indexing_enabled);
        fields.object["discovered"] = log_number(index_stats.discovered);
        fields.object["indexed"] = log_number(index_stats.indexed);
        fields.object["unchanged"] = log_number(index_stats.unchanged);
        fields.object["skipped"] = log_number(index_stats.skipped);
        if (!error.ok()) {
            fields.object["error_code"] = log_string(error_code_name(error.code));
            fields.object["error_message"] = log_string(error.message);
        }
        logger_->event("index_result", {"index"}, std::move(fields),
                       error.ok() ? "success" : "failure");
    }
    if (!context.options.quiet && indexing_enabled) {
        for (const std::string& diagnostic : index_stats.diagnostics)
            std::cerr << "Index warning: " << redact_secrets(diagnostic, secrets_) << "\n";
        std::cerr << "Code index refreshed: " << index_stats.discovered << " eligible, "
                  << index_stats.indexed << " indexed, " << index_stats.unchanged
                  << " unchanged, " << index_stats.skipped << " skipped.\n";
    }

    index::Snapshot snapshot;
    if (indexing_enabled) {
        error = index::load_snapshot(index_options, snapshot);
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
        }
    }

    ToolRegistryOptions tool_options;
    tool_options.mutation_policy = task_mode_ == AgentTaskMode::Plan
                                       ? MutationPolicy::PlanningDocuments
                                       : MutationPolicy::Full;
    tool_options.allow_network = options_.allow_network;
    tool_options.history_backup = options_.history_backup;
    tool_options.fetch_options = options_.fetch_options;
    tool_options.search_options = options_.search_options;
    tool_options.permission_mode = permission_mode_;
    tool_options.permission_controls = options_.interactive;
    tool_options.indexing_enabled = indexing_enabled;
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
                logger_->event("guard_approval", {"guard"}, std::move(fields),
                               decision == GuardApprovalDecision::Allow ? "success" : "failure");
            }
            return decision;
        };
    }
    if (indexing_enabled) {
        error = ReadToolRegistry::create(index_options, std::move(snapshot),
                                         secrets_, tools_, tool_options);
    } else {
        error = ReadToolRegistry::create_without_index(
            index_options, secrets_, tools_, tool_options);
    }
    if (!error.ok()) {
        reset();
        return error;
    }

    error = load_trusted_prompts(options_.trusted_prompt_dir, prompts_);
    if (!error.ok()) {
        reset();
        return error;
    }

    const bool supports_tools = provider::capabilities_for(context).tool_calls;
    state_.protocol = default_tool_protocol(supports_tools);
    limits_.interactive = options_.interactive;
    limits_.max_scripted_turns = 50;
    known_tools_ = known_tool_names(tools_);

    if (options_.enable_session_db) {
        error = session_store_.open(options_.workspace);
        if (!error.ok()) {
            reset();
            return error;
        }
        if (!context.options.quiet)
            std::cerr << "Agent session DB: " << session_store_.path() << "\n";
        if (options_.interactive) {
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
            options_.permission_mode = permission_mode_;
            tools_.set_permission_mode(permission_mode_);
        }
    }

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

    prepared_ = true;
    publish_request_token_estimate();
    return ok_error();
}

Error AgentSessionRuntime::load_display_messages(std::vector<provider::Message>& out) const {
    out.clear();
    if (!prepared_ || !session_store_.is_open()) return ok_error();
    std::vector<AgentMessageRecord> rows;
    Error error = session_store_.load_messages(rows);
    if (!error.ok()) return error;
    out.reserve(rows.size());
    const std::size_t cols = terminal_column_count();
    for (const AgentMessageRecord& row : rows) {
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
        out.push_back(std::move(message));
    }
    return ok_error();
}

Error AgentSessionRuntime::append_display_notice(const std::string& content) {
    if (!session_store_.is_open()) return ok_error();
    if (content.empty()) return ok_error();
    return session_store_.append_message("notice", redact_secrets(content, secrets_));
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
            "create and enable it."};
        return result;
    }
    result.indexing_enabled = true;
    if (refresh) {
        result.error = tools_.refresh_persistent_index(true, cancellation);
        if (!result.error.ok()) return result;
    }
    result.markdown = index::compact_totals_markdown(tools_.snapshot());
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

    const bool was_enabled = tools_.indexing_enabled();
    if (was_enabled) {
        result.error = tools_.refresh_persistent_index(true, cancellation);
        if (!result.error.ok()) {
            result.indexing_enabled = true;
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
        if (!result.error.ok()) return result;
        index::Snapshot snapshot;
        result.error = index::load_snapshot(index_options, snapshot);
        if (!result.error.ok()) return result;
        result.error = tools_.enable_persistent_index(
            std::move(index_options), std::move(snapshot));
        if (!result.error.ok()) return result;

        options_.index_mode =
            SessionRuntimeOptions::IndexMode::CreateOrRefresh;
        known_tools_ = known_tool_names(tools_);
        result.created = true;
    }

    result.indexing_enabled = true;
    result.markdown = index::compact_totals_markdown(tools_.snapshot());
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
        append_conversation_text(conversation_, "user",
                                 agent_task_mode_control(mode));
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

    if (session_store_.is_open()) {
        AgentProjectRecord project;
        Error error = session_store_.open_project(project);
        if (!error.ok()) return error;
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
        if (!error.ok()) return error;
        error = session_store_.update_project_meta(project);
        if (!error.ok()) return error;
    }
    permission_mode_ = mode;
    options_.permission_mode = mode;
    tools_.set_permission_mode(mode);
    return ok_error();
}

Error AgentSessionRuntime::finish_session(const std::string& status,
                                          const std::string& final_text,
                                          const std::string& error_code,
                                          const std::string& error_message) {
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
    const std::string& user_text,
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
    const std::string text = ascii_trim(user_text);
    if (text.empty()) {
        result.error = {ErrorCode::BadArgs, "agent turn requires a non-empty user message"};
        return result;
    }
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
        const std::string prior_context = build_prior_session_context(prior);
        if (prior_context.empty()) {
            seed_agent_conversation(conversation_, prompts_, task_mode_, state_.protocol, text,
                                    agents_md_.injection_text);
        } else {
            seed_agent_conversation(conversation_, prompts_, task_mode_, state_.protocol, "",
                                    agents_md_.injection_text);
            conversation_.messages.push_back({"user", prior_context});
            conversation_.messages.push_back({"user", text});
            if (!context.options.quiet)
                std::cerr << "Injected prior agent transcript (" << prior.size()
                          << " stored messages) into model context.\n";
        }
        conversation_seeded_ = true;
        publish_request_token_estimate();
        if (session_store_.is_open() && session_id_ > 0) {
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
        append_conversation_text(conversation_, "user", text);
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
    auto executor = [&](const std::string& name, const std::string& arguments_json,
                        runtime::CancellationToken token) {
        ++turn_tool_index;
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
        const std::vector<provider::FunctionDefinition> definitions =
            state_.protocol == ToolProtocol::Xml ? std::vector<provider::FunctionDefinition>{}
                                                 : tools_.definitions();

        provider::ToolRoundResult round;
        active_round_id = state_.turn + 1;
        std::string round_reasoning;
        std::string round_preview;
        bool reasoning_row_started = false;
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
        auto on_retry = [&](const Error& retry_error, int attempt,
                            int backoff_seconds) {
                round_reasoning.clear();
                round_preview.clear();
                if (!logger_) return;
                json::Value fields = log_object();
                fields.object["error_code"] = log_string(error_code_name(retry_error.code));
                fields.object["error_message"] = log_string(retry_error.message);
                fields.object["attempt"] = log_number(attempt);
                fields.object["backoff_ms"] = log_number(backoff_seconds * 1000);
                logger_->event("retry_scheduled", log_context, std::move(fields), "failure");
            };
        auto on_reasoning = [&](const std::string& delta) -> Error {
                if (!options_.interactive) return ok_error();
                round_reasoning += delta;
                const std::string preview = clip_to_cells(format_reasoning_preview(
                    round_reasoning,
                    static_cast<std::size_t>(
                        std::max(0, context.options.agent_thinking_preview_max_chars)),
                    secrets_), tool_line_width);
                if (!preview.empty()) {
                    round_preview = preview;
                    reasoning_row_started = true;
                    structured_progress({AgentProgressAction::Upsert,
                                         AgentProgressKind::Thinking, active_round_id, 0,
                                         preview, 0});
                }
                return cancellation.cancelled()
                           ? Error{ErrorCode::Cancelled,
                                   "agent reasoning preview cancelled"}
                           : ok_error();
            };
        Error error = send_tool_round_with_transport_retries(
            context, conversation_, definitions, round, cancellation,
            limits_.transport_attempts, observer_pointer, observation_context,
            on_retry, on_reasoning);
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
                round_preview.clear();
                error = send_tool_round_with_transport_retries(
                    context, conversation_, definitions, round, cancellation,
                    limits_.transport_attempts, observer_pointer,
                    observation_context, on_retry, on_reasoning);
            } else if (!recovered.error.ok()) {
                error = recovered.error;
            }
        }
        if (!error.ok()) {
            if (reasoning_row_started)
                structured_progress({AgentProgressAction::Discard,
                                     AgentProgressKind::Thinking, active_round_id, 0, {}, 0});
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
        if (options_.interactive && !round_reasoning.empty()) {
            round_preview = clip_to_cells(format_reasoning_preview(
                round_reasoning,
                static_cast<std::size_t>(
                    std::max(0, context.options.agent_thinking_preview_max_chars)),
                secrets_), tool_line_width);
        }
        if (!round_preview.empty()) {
            const long long preview_ms = now_unix_ms();
            structured_progress({AgentProgressAction::Commit,
                                 AgentProgressKind::Thinking, active_round_id, 0,
                                 round_preview, preview_ms});
            if (session_store_.is_open() && session_id_ > 0)
                (void)session_store_.append_message("thinking", round_preview);
        } else if (reasoning_row_started) {
            structured_progress({AgentProgressAction::Discard,
                                 AgentProgressKind::Thinking, active_round_id, 0, {}, 0});
        }

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
        const provider::ChatResult round_metrics = round.metrics;
        AgentRoundOutcome outcome = handle_agent_tool_round(
            state_, limits_, context, conversation_, std::move(round), known_tools_, executor,
            cancellation);
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

        if (outcome.kind == AgentRoundOutcome::Kind::Continue) continue;

        commit_turn_counts();

        if (outcome.kind == AgentRoundOutcome::Kind::FinalText) {
            final_text = outcome.final_text;
            result.error = ok_error();
            result.final_text = final_text;
            const Error final_index_error =
                tools_.refresh_persistent_index(true, cancellation);
            if (!final_index_error.ok() &&
                final_index_error.code != ErrorCode::Cancelled) {
                result.notice =
                    "final code-index refresh failed: " +
                    redact_secrets(final_index_error.message, secrets_);
                if (!context.options.quiet)
                    std::cerr << "Agent warning: " << result.notice << "\n";
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
