#pragma once

#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "cli/args.hpp"
#include "common.hpp"
#include "http/http.hpp"
#include "runtime/runtime.hpp"

namespace ainiux::provider {

enum class ApiKind { ChatCompletions, Responses };

struct Capabilities {
    bool chat_completions = false;
    bool responses_api = false;
    bool streaming = false;
    bool model_listing = false;
    bool usage_reporting = false;
    bool requires_bearer_key = false;
    bool optional_bearer_key = false;
    bool images = false;
    bool pdfs = false;
    bool file_uploads = false;
    bool file_urls = false;
    bool tool_calls = false;
    bool server_side_context_management = false;
    bool custom_headers = false;
    bool local_endpoint = false;
};

struct Profile {
    std::string name;
    std::vector<std::string> aliases;
    std::string base_url;
    std::string chat_path = "/chat/completions";
    std::string responses_path = "/responses";
    std::string models_path = "/models";
    bool requires_bearer_key = false;
    bool local_endpoint = false;
    bool offline = false;
    std::vector<std::string> key_envs;
    std::string dummy_api_key;
    std::string compatibility_warning;
    Capabilities capabilities;
};

struct RequestContext {
    cli::Options options;
    Profile profile;
    std::string base_url;
    std::string chat_url;
    std::string responses_url;
    std::string models_url;
    std::string api_key;
    std::vector<std::string> headers;
    ApiKind api_kind = ApiKind::ChatCompletions;
    bool suppress_streaming_reasoning = false;
};

struct ImageInput {
    std::string mime_type;
    std::string base64_data;
    // Managed chat media is persisted outside SQLite and hydrated into
    // base64_data only in the request worker. storage_ref is a lowercase
    // SHA-256 digest, never an arbitrary filesystem path.
    std::string storage_ref;
    std::string display_name;
    std::string source_ref;
    long long byte_size = 0;

    ImageInput() = default;
    ImageInput(std::string input_mime_type, std::string input_base64_data)
        : mime_type(std::move(input_mime_type)),
          base64_data(std::move(input_base64_data)) {}
};

struct TextAttachment {
    // Markdown is the canonical replay format for text-like attachments.
    // Small attachments keep markdown_content in SQLite; larger attachments
    // use a managed-media SHA-256 storage_ref and are hydrated in a worker.
    std::string markdown_content;
    std::string storage_ref;
    std::string display_name;
    std::string source_ref;
    long long byte_size = 0;
};

struct Message {
    std::string role;
    std::string content;
    std::vector<ImageInput> images;
    std::vector<TextAttachment> text_attachments;
    // Optional wall-clock timestamp in milliseconds since Unix epoch. Used for
    // UI elapsed-time display (agent transcript); never serialized to providers.
    long long created_at_ms = 0;

    Message() = default;
    Message(std::string message_role,
            std::string message_content,
            std::vector<ImageInput> message_images = {},
            std::vector<TextAttachment> message_text_attachments = {})
        : role(std::move(message_role)),
          content(std::move(message_content)),
          images(std::move(message_images)),
          text_attachments(std::move(message_text_attachments)) {}
};

struct ChatResult {
    std::string model;
    std::string content;
    std::string usage_json = "null";
    long long ttft_ms = -1;
    long long total_ms = 0;
    long long completion_tokens = 0;
    bool completion_tokens_estimated = false;
    long long prompt_tokens = -1;
    long long total_tokens = -1;
    long http_status = 0;
    long long dns_ms = -1;
    long long connect_ms = -1;
    long long tls_ms = -1;
    long long time_to_first_byte_ms = -1;
    long long first_body_ms = -1;
};

struct FunctionDefinition {
    std::string name;
    std::string description;
    std::string parameters_json;
};

struct ToolCall {
    std::string id;
    std::string name;
    std::string arguments_json;
    std::size_t index = 0;
};

struct ToolConversation {
    std::vector<Message> messages;
    // Opaque protocol-native assistant/output and tool-result items from prior rounds.
    std::vector<std::string> continuation_items_json;
};

struct ToolRoundResult {
    std::string content;
    std::vector<ToolCall> tool_calls;
    std::vector<std::string> continuation_items_json;
    ChatResult metrics;
    bool truncated = false;
};

struct ToolSourceRange {
    std::string path;
    std::size_t byte_start = 0;
    std::size_t byte_end = 0;
    std::size_t line_start = 0;
    std::size_t line_end = 0;
};

struct ToolRoundContext {
    std::string stage;
    std::size_t worker_slot = 0;
    std::size_t task_number = 0;
    std::size_t segment_number = 0;
    std::size_t synthesis_group = 0;
    std::size_t round = 0;
    std::size_t retry_attempt = 0;
    std::size_t cumulative_tool_calls = 0;
    std::vector<ToolSourceRange> sources;
};

// Provider-neutral hooks used by bounded workflows that need an audit trail.
// Callbacks are synchronous and must be thread-safe.
struct ToolRoundObserver {
    std::function<void(const ToolRoundContext&, const std::string& endpoint,
                       const std::vector<std::string>& header_names,
                       const std::string& serialized_body,
                       const Error& serialization_error)> on_request;
    std::function<void(const ToolRoundContext&, const http::Response&,
                       const ToolRoundResult&, const Error& outcome)> on_response;
};

struct ModelInfo {
    std::string id;
    std::map<std::string, std::string> attributes;
};

struct ModelsResult {
    std::vector<ModelInfo> models;
    std::vector<std::string> model_ids;
};

using DeltaCallback = std::function<Error(const std::string&)>;

struct ContextResult {
    RequestContext context;
    Error error;
};

ContextResult build_context(const cli::Options& options);
bool tui_needs_startup_provider_selection(const cli::Options& options);
bool editor_needs_local_only_default(const cli::Options& options);
bool looks_like_api_url(const std::string& text);
void apply_provider_target(cli::Options& options, const std::string& target);
void apply_editor_startup_default(cli::Options& options);
void apply_editor_offline_default(cli::Options& options);
void apply_tui_startup_default(cli::Options& options);
std::string format_models_markdown(const std::string& provider_name,
                                   const std::string& models_url,
                                   const ModelsResult& result);
Error parse_models_response(const std::string& body, ModelsResult& result);
std::vector<Profile> built_in_profiles();
std::string normalize_provider_key(std::string text);
std::string canonical_profile_name(const std::string& name);
bool is_selectable_provider(const Profile& profile);
bool needs_interactive_model_selection(const RequestContext& context);
std::string display_name_for_profile(const std::string& profile_name);
Error validate_profile_name(const std::string& name);
const Capabilities& capabilities_for(const RequestContext& context);
const ModelCapability* matched_model_capability(const RequestContext& context);
std::string reasoning_temperature_advisory(const RequestContext& context);
Capabilities detected_capabilities_for(const RequestContext& context);
Error validate_image_input(const RequestContext& context);
std::string active_request_url(const RequestContext& context);
double tokens_per_second(const ChatResult& result, bool stream);
long long reported_total_tokens(const ChatResult& result);
long long model_context_window_tokens(const ModelInfo& model);
long long context_window_for_model(const ModelsResult& models, const std::string& model_selector);
void apply_context_window_from_models(RequestContext& context,
                                      const ModelsResult& models,
                                      const std::string& model_selector = "");
Error resolve_context_window(RequestContext& context, const std::string& model_selector = "");
std::string serialize_request(const RequestContext& context, const std::vector<Message>& messages);
std::string serialize_chat_request(const RequestContext& context, const std::vector<Message>& messages);
std::string serialize_tool_request(const RequestContext& context,
                                   const ToolConversation& conversation,
                                   const std::vector<FunctionDefinition>& tools);
Error parse_tool_response(const RequestContext& context,
                          const std::string& body,
                          ToolRoundResult& result,
                          bool streaming);
void append_tool_results(const RequestContext& context,
                         const std::vector<ToolCall>& calls,
                         const std::vector<std::string>& result_json,
                         ToolConversation& conversation);
Error send_tool_round(const RequestContext& context,
                      const ToolConversation& conversation,
                      const std::vector<FunctionDefinition>& tools,
                      ToolRoundResult& result,
                      runtime::CancellationToken cancellation = runtime::CancellationToken(),
                      const ToolRoundObserver* observer = nullptr,
                      const ToolRoundContext& observation_context = ToolRoundContext{});
Error list_models(const RequestContext& context,
                  ModelsResult& result,
                  runtime::CancellationToken cancellation = runtime::CancellationToken());
Error send_chat(const RequestContext& context,
                DeltaCallback on_delta,
                ChatResult& result,
                runtime::CancellationToken cancellation = runtime::CancellationToken());
Error send_chat_messages(const RequestContext& context,
                         const std::vector<Message>& messages,
                         DeltaCallback on_delta,
                         ChatResult& result,
                         runtime::CancellationToken cancellation = runtime::CancellationToken());
std::string normalize_base_url(const std::string& url, bool* changed, Error& error);

}  // namespace ainiux::provider
