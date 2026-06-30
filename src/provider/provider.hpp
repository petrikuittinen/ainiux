#pragma once

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "cli/args.hpp"
#include "common.hpp"
#include "http/http.hpp"
#include "runtime/runtime.hpp"

namespace pkchat::provider {

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
};

struct Message {
    std::string role;
    std::string content;
    std::vector<ImageInput> images;

    Message() = default;
    Message(std::string message_role,
            std::string message_content,
            std::vector<ImageInput> message_images = {})
        : role(std::move(message_role)),
          content(std::move(message_content)),
          images(std::move(message_images)) {}
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

struct ModelsResult {
    std::vector<std::string> model_ids;
};

using DeltaCallback = std::function<Error(const std::string&)>;

struct ContextResult {
    RequestContext context;
    Error error;
};

ContextResult build_context(const cli::Options& options);
std::vector<Profile> built_in_profiles();
std::string display_name_for_profile(const std::string& profile_name);
Error validate_profile_name(const std::string& name);
const Capabilities& capabilities_for(const RequestContext& context);
Capabilities detected_capabilities_for(const RequestContext& context);
Error validate_image_input(const RequestContext& context);
std::string active_request_url(const RequestContext& context);
double tokens_per_second(const ChatResult& result, bool stream);
long long reported_total_tokens(const ChatResult& result);
std::string serialize_chat_request(const RequestContext& context, const std::vector<Message>& messages);
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

}  // namespace pkchat::provider
