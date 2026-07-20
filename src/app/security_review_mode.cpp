#include "app/app.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <map>
#include <memory>
#include <thread>

#include "agent/index/index.hpp"
#include "agent/prompts.hpp"
#include "agent/review.hpp"
#include "agent/review_log.hpp"
#include "agent/tools.hpp"
#include "security/redact.hpp"
#include "json/json.hpp"

namespace ainiux::app {
namespace {

volatile std::sig_atomic_t g_review_interrupt = 0;

json::Value log_object() { json::Value value; value.type = json::Value::Type::Object; return value; }
json::Value log_array() { json::Value value; value.type = json::Value::Type::Array; return value; }
json::Value log_string(const std::string& text) { json::Value value; value.type = json::Value::Type::String; value.string = text; return value; }
json::Value log_number(double number) { json::Value value; value.type = json::Value::Type::Number; value.number = number; return value; }
json::Value log_bool(bool boolean) { json::Value value; value.type = json::Value::Type::Bool; value.boolean = boolean; return value; }

void review_signal_handler(int) { g_review_interrupt = 1; }

class ReviewSignalGuard {
   public:
    ReviewSignalGuard() { g_review_interrupt = 0; previous_ = std::signal(SIGINT, review_signal_handler); }
    ~ReviewSignalGuard() { if (previous_ != SIG_ERR) std::signal(SIGINT, previous_); }
    ReviewSignalGuard(const ReviewSignalGuard&) = delete;
    ReviewSignalGuard& operator=(const ReviewSignalGuard&) = delete;
   private:
    using Handler = void (*)(int);
    Handler previous_ = SIG_ERR;
};

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

int render_failure_report(const provider::RequestContext& context,
                          const Error& error,
                          long long reviewed_at = 0) {
    agent::ReviewReport report;
    report.workspace = ".";
    report.provider = context.profile.name;
    report.model = context.options.model;
    report.api = context.api_kind == provider::ApiKind::Responses ? "responses" : "chat";
    report.reasoning = "configured";
    report.reviewed_at = reviewed_at == 0
                             ? std::chrono::duration_cast<std::chrono::seconds>(
                                   std::chrono::system_clock::now().time_since_epoch()).count()
                             : reviewed_at;
    report.batch_size = context.options.security_review_batch_size;
    report.parallel_agents = static_cast<std::size_t>(context.options.max_parallel_agents);
    report.complete = false;
    report.errors.push_back(error.message);
    const Error render_error = agent::render_review_markdown(report, std::cout);
    if (!render_error.ok()) print_error(render_error);
    print_error(error);
    return exit_code_for(error.code);
}

}  // namespace

int run_security_review_mode(provider::RequestContext context) {
    ReviewSignalGuard signal_guard;
    runtime::CancellationSource cancellation;
    std::atomic<bool> finished{false};
    std::thread interrupt_monitor([&] {
        while (!finished.load(std::memory_order_acquire)) {
            if (g_review_interrupt != 0) { cancellation.cancel(); return; }
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
    });
    struct MonitorJoin {
        std::atomic<bool>& finished;
        std::thread& thread;
        ~MonitorJoin() { finished.store(true, std::memory_order_release); if (thread.joinable()) thread.join(); }
    } monitor_join{finished, interrupt_monitor};

    const std::vector<std::string> secrets = configured_secrets(context);
    const auto review_started = std::chrono::steady_clock::now();
    std::unique_ptr<agent::ReviewLogger> logger;
    if (context.options.security_review_log_enabled) {
        Error log_error;
        logger = agent::ReviewLogger::create(
            ".", context.options.security_review_log_keep_runs, secrets,
            [&](const std::string& warning) { std::cerr << warning << "\n"; }, log_error);
        if (!logger) {
            std::cerr << "SECURITY REVIEW LOGGING DISABLED: "
                      << redact_secrets(log_error.message, secrets)
                      << "; the review will continue\n";
        } else {
            if (!context.options.quiet)
                std::cerr << "Security review diagnostic log: " << logger->final_path() << "\n";
            json::Value fields = log_object();
            fields.object["workspace"] = log_string(".");
            fields.object["provider"] = log_string(context.profile.name);
            fields.object["model"] = log_string(context.options.model);
            fields.object["api"] = log_string(context.api_kind == provider::ApiKind::Responses ? "responses" : "chat");
            fields.object["streaming"] = log_bool(context.options.stream);
            fields.object["reasoning_kind"] = log_number(static_cast<int>(context.options.reasoning.kind));
            fields.object["reasoning_value"] = log_string(context.options.reasoning.value);
            fields.object["reasoning_tokens"] = log_number(context.options.reasoning.tokens);
            fields.object["batch_size"] = log_number(context.options.security_review_batch_size);
            fields.object["parallelism"] = log_number(context.options.max_parallel_agents);
            logger->event("run_start", {"run"}, std::move(fields), "success");
        }
    }
    auto finish_log = [&](const Error& final_error, const agent::ReviewReport* report) {
        if (!logger) return;
        json::Value fields = log_object();
        fields.object["duration_ms"] = log_number(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - review_started).count());
        fields.object["exit_code"] = log_number(exit_code_for(final_error.code));
        fields.object["error_code"] = log_string(error_code_name(final_error.code));
        fields.object["error_message"] = log_string(final_error.message);
        std::map<std::string, std::size_t> counts;
        if (report != nullptr) for (const agent::FileCoverage& coverage : report->coverage) ++counts[coverage.status];
        json::Value coverage = log_object();
        for (const auto& count : counts) coverage.object[count.first] = log_number(count.second);
        fields.object["coverage_counts"] = std::move(coverage);
        fields.object["finding_count"] = log_number(report == nullptr ? 0 : report->findings.size());
        logger->finish(std::move(fields), final_error.ok() ? "success" : "failure");
    };

    agent::index::Options index_options;
    index_options.workspace = ".";
    index_options.max_source_code_file_size = context.options.max_source_code_file_size;
    index_options.cancellation = cancellation.token();
    index_options.interrupted = [] { return g_review_interrupt != 0; };
    agent::index::RefreshStats index_stats;
    Error error = agent::index::refresh(index_options, index_stats);
    if (logger) {
        json::Value fields = log_object();
        fields.object["discovered"] = log_number(index_stats.discovered);
        fields.object["indexed"] = log_number(index_stats.indexed);
        fields.object["unchanged"] = log_number(index_stats.unchanged);
        fields.object["skipped"] = log_number(index_stats.skipped);
        if (!error.ok()) {
            fields.object["error_code"] = log_string(error_code_name(error.code));
            fields.object["error_message"] = log_string(error.message);
        }
        logger->event("index_result", {"index"}, std::move(fields), error.ok() ? "success" : "failure");
    }
    if (!error.ok()) { finish_log(error, nullptr); return render_failure_report(context, error); }
    if (!context.options.quiet) {
        for (const std::string& diagnostic : index_stats.diagnostics)
            std::cerr << "Index warning: " << redact_secrets(diagnostic, secrets) << "\n";
        std::cerr << "Code index refreshed: " << index_stats.discovered << " eligible, "
                  << index_stats.indexed << " indexed, " << index_stats.unchanged << " unchanged, "
                  << index_stats.skipped << " skipped.\n";
    }

    agent::index::Snapshot snapshot;
    error = agent::index::load_snapshot(index_options, snapshot);
    if (!error.ok()) { finish_log(error, nullptr); return render_failure_report(context, error); }
    std::size_t source_files = 0;
    std::uintmax_t source_bytes = 0;
    for (const agent::index::IndexedFile& file : snapshot.files) if (file.status == "indexed") {
        ++source_files; source_bytes += file.size;
    }
    if (!context.options.quiet)
        std::cerr << "Security review scope: " << source_files << " indexed file(s), " << source_bytes
                  << " raw source bytes from " << snapshot.workspace << "; source will be sent to "
                  << context.profile.name << "/" << context.options.model << ".\n";

    agent::ReadToolRegistry tools;
    error = agent::ReadToolRegistry::create(index_options, std::move(snapshot), secrets, tools);
    if (!error.ok()) { finish_log(error, nullptr); return render_failure_report(context, error); }
    agent::TrustedPrompts prompts;
    error = agent::load_trusted_prompts(context.options.trusted_prompt_dir, prompts);
    if (!error.ok()) { finish_log(error, nullptr); return render_failure_report(context, error); }

    agent::ReviewReport report;
    Error review_error = agent::run_review(
        context, prompts, tools, context.options.security_review_batch_size,
        static_cast<std::size_t>(context.options.max_parallel_agents), cancellation.token(),
        [&](const std::string& message) {
            if (!context.options.quiet) std::cerr << "Security review: " << redact_secrets(message, secrets) << "\n";
        }, report, logger.get());

    agent::index::Freshness freshness;
    const Error freshness_error = agent::index::check_freshness(index_options, freshness);
    if (logger) {
        json::Value fields = log_object();
        fields.object["fresh"] = log_bool(freshness.fresh);
        const auto add_paths = [&](const char* name, const std::vector<std::string>& paths) {
            json::Value values = log_array();
            for (const std::string& path : paths) values.array.push_back(log_string(path));
            fields.object[name] = std::move(values);
        };
        add_paths("changed", freshness.changed);
        add_paths("added", freshness.added);
        add_paths("removed", freshness.removed);
        if (!freshness_error.ok()) {
            fields.object["error_code"] = log_string(error_code_name(freshness_error.code));
            fields.object["error_message"] = log_string(freshness_error.message);
        }
        logger->event("freshness_result", {"freshness"}, std::move(fields),
                      freshness_error.ok() && freshness.fresh ? "success" : "failure");
    }
    if (!freshness_error.ok()) {
        report.complete = false;
        report.errors.push_back("post-review freshness check: " + freshness_error.message);
        if (review_error.ok()) review_error = freshness_error;
    } else if (!freshness.fresh) {
        report.complete = false;
        for (const std::string& path : freshness.changed) {
            bool found = false;
            for (agent::FileCoverage& coverage : report.coverage) if (coverage.path == path) {
                coverage.status = "stale"; coverage.detail = "file changed during review"; found = true; break;
            }
            if (!found) report.coverage.push_back({path, "stale", "file changed during review"});
        }
        for (const std::string& path : freshness.added) report.coverage.push_back({path, "uncovered", "file appeared during review"});
        for (const std::string& path : freshness.removed) {
            for (agent::FileCoverage& coverage : report.coverage) if (coverage.path == path) {
                coverage.status = "stale"; coverage.detail = "file was removed during review"; break;
            }
        }
        report.errors.push_back("workspace changed after the indexed review snapshot");
        if (review_error.ok()) review_error = {ErrorCode::FileRead, "workspace changed during security review"};
    }
    std::sort(report.coverage.begin(), report.coverage.end(), [](const agent::FileCoverage& left, const agent::FileCoverage& right) {
        return left.path < right.path;
    });
    const Error render_error = agent::render_review_markdown(report, std::cout);
    if (!render_error.ok()) {
        finish_log(render_error, &report);
        print_error(render_error);
        return exit_code_for(render_error.code);
    }
    if (!review_error.ok()) {
        finish_log(review_error, &report);
        print_error(review_error);
        return exit_code_for(review_error.code);
    }
    finish_log(ok_error(), &report);
    return 0;
}

}  // namespace ainiux::app
