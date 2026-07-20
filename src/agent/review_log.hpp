#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "common.hpp"
#include "json/json.hpp"
#include "provider/provider.hpp"

namespace ainiux::agent {

struct ReviewLogContext {
    ReviewLogContext() = default;
    ReviewLogContext(std::string stage_name) : stage(std::move(stage_name)) {}
    std::string stage;
    std::size_t worker_slot = 0;
    std::size_t task_number = 0;
    std::size_t segment_number = 0;
    std::size_t synthesis_group = 0;
    std::size_t round = 0;
    std::size_t retry_attempt = 0;
    std::size_t cumulative_tool_calls = 0;
    std::vector<provider::ToolSourceRange> sources;
};

class ReviewLogger {
   public:
    using WarningCallback = std::function<void(const std::string&)>;

    ~ReviewLogger();
    ReviewLogger(const ReviewLogger&) = delete;
    ReviewLogger& operator=(const ReviewLogger&) = delete;

    static std::unique_ptr<ReviewLogger> create(const std::string& workspace,
                                                int keep_runs,
                                                std::vector<std::string> secrets,
                                                WarningCallback warning,
                                                Error& error);

    const std::string& final_path() const { return final_path_; }
    const std::string& partial_path() const { return partial_path_; }
    const std::string& run_id() const { return run_id_; }
    bool enabled() const;

    void event(const std::string& event_type,
               const ReviewLogContext& context,
               json::Value fields = json::Value{},
               const std::string& status = "");
    provider::ToolRoundObserver tool_round_observer();
    void finish(json::Value fields, const std::string& status);

    // Payloads are never truncated. Invalid UTF-8 is represented as base64.
    static json::Value payload(const std::string& bytes);

   private:
    ReviewLogger() = default;
    Error initialize(const std::string& workspace, int keep_runs,
                     std::vector<std::string> secrets, WarningCallback warning);
    void fail_locked(const std::string& detail);
    bool write_record_locked(const std::string& record);
    void prune_completed();

    mutable std::mutex mutex_;
    int fd_ = -1;
    int directory_fd_ = -1;
    int keep_runs_ = 3;
    unsigned long long sequence_ = 0;
    bool active_ = false;
    bool warned_ = false;
    std::string directory_;
    std::string final_path_;
    std::string partial_path_;
    std::string final_name_;
    std::string partial_name_;
    std::string run_id_;
    std::vector<std::string> secrets_;
    WarningCallback warning_;
};

}  // namespace ainiux::agent
