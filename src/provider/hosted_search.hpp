#pragma once

#include <string>
#include <vector>

#include "json/json.hpp"
#include "provider/provider.hpp"

namespace ainiux::provider {

enum class HostedWebSearchKind {
    None,
    ResponsesType,
    ChatType,
    KimiBuiltin,
    GlmChat,
};

struct HostedWebSearch {
    HostedWebSearchKind kind = HostedWebSearchKind::None;
    std::string name = "web_search";
    std::string catalog_id;

    bool enabled() const { return kind != HostedWebSearchKind::None; }
};

HostedWebSearch resolve_hosted_web_search(const RequestContext& context);
bool hosted_web_search_requires_responses(const HostedWebSearch& search);
json::Value hosted_web_search_tool_json(const HostedWebSearch& search);
void append_hosted_web_search_tool(json::Value& tools, const HostedWebSearch& search);
void attach_hosted_web_search_tools(json::Value& root, const RequestContext& context);
std::string attach_hosted_web_search_tools(const RequestContext& context, std::string request_json);
void collect_hosted_search_metadata(const json::Value& item,
                                    std::vector<std::string>& queries,
                                    std::vector<std::pair<std::string, std::string>>& citations);
void collect_content_citations(const json::Value& content,
                               std::vector<std::pair<std::string, std::string>>& citations);

}  // namespace ainiux::provider
