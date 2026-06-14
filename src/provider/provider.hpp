#pragma once

#include <functional>
#include <string>
#include <vector>

#include "cli/args.hpp"
#include "common.hpp"
#include "http/http.hpp"

namespace pkchat::provider {

struct Profile {
    std::string name;
    std::string base_url;
    bool requires_bearer_key = false;
    bool local_endpoint = false;
    std::vector<std::string> key_envs;
};

struct RequestContext {
    cli::Options options;
    Profile profile;
    std::string base_url;
    std::string chat_url;
    std::string models_url;
    std::string api_key;
    std::vector<std::string> headers;
};

struct ChatResult {
    std::string model;
    std::string content;
    std::string usage_json = "null";
    long long total_ms = 0;
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
Error list_models(const RequestContext& context, ModelsResult& result);
Error send_chat(const RequestContext& context, DeltaCallback on_delta, ChatResult& result);
std::string normalize_base_url(const std::string& url, bool* changed, Error& error);

}  // namespace pkchat::provider
