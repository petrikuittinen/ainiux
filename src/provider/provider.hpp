#pragma once

#include <functional>
#include <string>
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
};

struct Message {
    std::string role;
    std::string content;
};

struct ChatResult {
    std::string model;
    std::string content;
    std::string usage_json = "null";
    long long ttft_ms = -1;
    long long total_ms = 0;
    long long completion_tokens = 0;
    bool completion_tokens_estimated = false;
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
const Capabilities& capabilities_for(const RequestContext& context);
std::string active_request_url(const RequestContext& context);
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
