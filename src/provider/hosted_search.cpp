#include "provider/hosted_search.hpp"

#include "config/model_catalog.hpp"
#include "json/json.hpp"

namespace ainiux::provider {
namespace {

json::Value json_string(const std::string& text) {
    json::Value value;
    value.type = json::Value::Type::String;
    value.string = text;
    return value;
}

json::Value json_bool(bool boolean) {
    json::Value value;
    value.type = json::Value::Type::Bool;
    value.boolean = boolean;
    return value;
}

json::Value json_object() {
    json::Value value;
    value.type = json::Value::Type::Object;
    return value;
}

json::Value json_array() {
    json::Value value;
    value.type = json::Value::Type::Array;
    return value;
}

HostedWebSearchKind kind_for(const ModelCapability& capability,
                             ApiKind api_kind,
                             const std::string& name) {
    if (!name.empty() && name.front() == '$') return HostedWebSearchKind::KimiBuiltin;
    if (capability.id == "zai-glm-5" || capability.reasoning_protocol == ReasoningProtocol::Zai)
        return HostedWebSearchKind::GlmChat;
    if (api_kind == ApiKind::Responses) return HostedWebSearchKind::ResponsesType;
    return HostedWebSearchKind::ChatType;
}

bool family_requires_responses(const ModelCapability& capability) {
    return capability.id == "openai-gpt-5" || capability.id == "xai-grok-4" ||
           capability.id == "deepseek-v4";
}

bool hosted_search_unsupported_on_adapter(const ModelCapability& capability,
                                          ApiKind api_kind) {
    if (family_requires_responses(capability) && api_kind != ApiKind::Responses)
        return true;
    // Official Gemini OpenAI-compat Chat rejects type=google_search (HTTP 400).
    // Native generateContent/Interactions grounding is not this adapter.
    const std::string name =
        capability.web_search_name.empty() ? "web_search" : capability.web_search_name;
    return capability.id == "google-gemini-3" || name == "google_search";
}

}  // namespace

HostedWebSearch resolve_hosted_web_search(const RequestContext& context) {
    HostedWebSearch search;
    if (!context.options.builtin_web_search) return search;
    const ModelCapability* capability = matched_model_capability(context);
    if (capability == nullptr || !capability->web_search) return search;
    search.catalog_id = capability->id;
    search.name = capability->web_search_name.empty() ? "web_search" : capability->web_search_name;
    if (hosted_search_unsupported_on_adapter(*capability, context.api_kind))
        return HostedWebSearch{};
    search.kind = kind_for(*capability, context.api_kind, search.name);
    return search;
}

bool hosted_web_search_requires_responses(const HostedWebSearch& search) {
    return search.catalog_id == "openai-gpt-5" || search.catalog_id == "xai-grok-4" ||
           search.catalog_id == "deepseek-v4";
}

json::Value hosted_web_search_tool_json(const HostedWebSearch& search) {
    json::Value item = json_object();
    if (search.kind == HostedWebSearchKind::KimiBuiltin) {
        item.object["type"] = json_string("builtin_function");
        json::Value function = json_object();
        function.object["name"] = json_string(search.name);
        item.object["function"] = std::move(function);
        return item;
    }
    item.object["type"] = json_string(search.name);
    if (search.kind == HostedWebSearchKind::GlmChat) {
        json::Value wrapper = json_object();
        wrapper.object["enable"] = json_bool(true);
        item.object["web_search"] = std::move(wrapper);
    }
    return item;
}

void append_hosted_web_search_tool(json::Value& tools, const HostedWebSearch& search) {
    if (!search.enabled() || !tools.is_array()) return;
    tools.array.push_back(hosted_web_search_tool_json(search));
}

void attach_hosted_web_search_tools(json::Value& root, const RequestContext& context) {
    const HostedWebSearch search = resolve_hosted_web_search(context);
    if (!search.enabled() || !root.is_object()) return;
    json::Value& tools = root.object["tools"];
    if (!tools.is_array()) tools = json_array();
    append_hosted_web_search_tool(tools, search);
    if (root.get("tool_choice") == nullptr)
        root.object["tool_choice"] = json_string("auto");
}

std::string attach_hosted_web_search_tools(const RequestContext& context, std::string request_json) {
    const HostedWebSearch search = resolve_hosted_web_search(context);
    if (!search.enabled()) return request_json;
    json::ParseResult parsed = json::parse(request_json);
    if (!parsed.error.ok() || !parsed.value.is_object()) return request_json;
    attach_hosted_web_search_tools(parsed.value, context);
    return json::stringify(parsed.value);
}

void collect_content_citations(const json::Value& content,
                               std::vector<std::pair<std::string, std::string>>& citations) {
    if (!content.is_array()) return;
    for (const json::Value& part : content.array) {
        const json::Value* annotations = part.get("annotations");
        if (annotations == nullptr || !annotations->is_array()) continue;
        for (const json::Value& annotation : annotations->array) {
            const json::Value* type = annotation.get("type");
            if (type == nullptr || !type->is_string() || type->string != "url_citation") continue;
            const json::Value* url = annotation.get("url");
            if (url == nullptr || !url->is_string() || url->string.empty()) continue;
            std::string title;
            if (const json::Value* title_value = annotation.get("title");
                title_value != nullptr && title_value->is_string())
                title = title_value->string;
            bool seen = false;
            for (const auto& existing : citations) {
                if (existing.first == url->string) {
                    seen = true;
                    break;
                }
            }
            if (!seen) citations.emplace_back(url->string, title);
        }
    }
}

void collect_hosted_search_metadata(const json::Value& item,
                                    std::vector<std::string>& queries,
                                    std::vector<std::pair<std::string, std::string>>& citations) {
    const json::Value* type = item.get("type");
    const std::string kind = type != nullptr && type->is_string() ? type->string : std::string();
    if (kind == "web_search_call") {
        const json::Value* action = item.get("action");
        if (action != nullptr && action->is_object()) {
            if (const json::Value* query = action->get("query");
                query != nullptr && query->is_string() && !query->string.empty())
                queries.push_back(query->string);
            if (const json::Value* query_list = action->get("queries");
                query_list != nullptr && query_list->is_array()) {
                for (const json::Value& query : query_list->array) {
                    if (query.is_string() && !query.string.empty()) queries.push_back(query.string);
                }
            }
        }
        return;
    }
    if (const json::Value* content = item.get("content")) collect_content_citations(*content, citations);
}

bool hosted_web_search_enabled(const RequestContext& context) {
    return resolve_hosted_web_search(context).enabled();
}

bool hosted_web_search_is_kimi_echo(const RequestContext& context) {
    return resolve_hosted_web_search(context).kind == HostedWebSearchKind::KimiBuiltin;
}

bool hosted_web_search_name_matches(const RequestContext& context, const std::string& name) {
    const HostedWebSearch search = resolve_hosted_web_search(context);
    return search.enabled() && (name == search.name || name == "$web_search");
}

std::string hosted_web_search_display_name(const RequestContext& context) {
    const HostedWebSearch search = resolve_hosted_web_search(context);
    return search.name.empty() ? "web_search" : search.name;
}

}  // namespace ainiux::provider
