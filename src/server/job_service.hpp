#pragma once

#include <memory>
#include <optional>
#include <string>

#include "cli/args.hpp"
#include "json/json.hpp"
#include "server/job_registry.hpp"
#include "server/chat_input_store.hpp"
#include "server/image_input_store.hpp"
#include "server/video_input_store.hpp"

namespace ainiux::server {

class ChatService;

struct ServiceSubmitResult {
    SubmitResult submission;
    Error validation_error;
};

// Internal server helper exposed for focused collision/cleanup coverage.
Error persist_generated_image(const std::string& workspace,
                              const std::string& extension,
                              const std::string& bytes,
                              std::string& relative_path);

class JobService {
   public:
    JobService(cli::Options base_options, std::string workspace, std::size_t max_jobs);
    JobService(const JobService&) = delete;
    JobService& operator=(const JobService&) = delete;

    ServiceSubmitResult submit(const std::string& operation,
                               const std::string& body,
                               const std::string& idempotency_key);
    JobRegistry& registry() { return registry_; }
    const JobRegistry& registry() const { return registry_; }
    std::string image_catalog_json() const;
    std::string video_catalog_json() const;
    Error add_image_input(std::string mime_type,
                          std::string bytes,
                          StoredImageInput& output);
    bool remove_image_input(const std::string& id);
    Error add_video_input(std::string mime_type, std::string bytes, StoredVideoInput& output);
    bool remove_video_input(const std::string& id);
    Error add_chat_input(std::string mime_type,
                         std::string filename,
                         std::string bytes,
                         StoredChatInput& output);
    bool remove_chat_input(const std::string& id);
    ChatInputStore& chat_inputs() { return chat_inputs_; }
    void set_chat_threads(ChatService* service) { chat_threads_ = service; }
    const std::string& workspace() const { return workspace_; }
    void shutdown() { registry_.shutdown(); }

   private:
    Error validate_common(const json::Value& root,
                          const std::string& operation,
                          cli::Options& options) const;
    JobOutcome run_chat_job(cli::Options options,
                            std::vector<provider::Message> messages,
                            runtime::CancellationToken cancellation,
                            JobEvents events) const;
    JobOutcome run_models_job(cli::Options options,
                              runtime::CancellationToken cancellation) const;
    JobOutcome run_agent_job(cli::Options options,
                             std::string goal,
                             bool plan,
                             runtime::CancellationToken cancellation,
                             JobEvents events) const;
    JobOutcome run_image_job(cli::Options options,
                             app::operation::ImageRequest request,
                             runtime::CancellationToken cancellation,
                             JobEvents events) const;
    JobOutcome run_video_job(cli::Options options,
                             app::operation::VideoRequest request,
                             runtime::CancellationToken cancellation,
                             JobEvents events) const;
    JobOutcome run_editor_assist_job(cli::Options options,
                                     std::string path,
                                     std::string revision,
                                     std::string instruction,
                                     std::optional<std::size_t> selection_start,
                                     std::optional<std::size_t> selection_end,
                                     runtime::CancellationToken cancellation,
                                     JobEvents events) const;

    cli::Options base_options_;
    std::string workspace_;
    JobRegistry registry_;
    ImageInputStore image_inputs_;
    VideoInputStore video_inputs_;
    ChatInputStore chat_inputs_;
    ChatService* chat_threads_ = nullptr;
};

}  // namespace ainiux::server
