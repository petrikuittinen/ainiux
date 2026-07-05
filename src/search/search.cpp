#include "search/search.hpp"

#include "html/html.hpp"
#include "http/http.hpp"
#include "json/json.hpp"
#include "security/redact.hpp"

#include <cctype>
#include <cstdlib>
#include <sstream>

namespace pkchat::search {
namespace {

constexpr const char* kUserAgent =
    "Mozilla/5.0 (X11; Linux x86_64; rv:128.0) Gecko/20100101 Firefox/128.0";

std::string trim_ascii(std::string text) {
    auto is_ws = [](unsigned char ch) {
        return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
    };
    while (!text.empty() && is_ws(static_cast<unsigned char>(text.front()))) {
        text.erase(text.begin());
    }
    while (!text.empty() && is_ws(static_cast<unsigned char>(text.back()))) {
        text.pop_back();
    }
    return text;
}

std::string lower_ascii(std::string text) {
    for (char& ch : text) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return text;
}

int parse_positive_int_env(const char* name, int default_value) {
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0') {
        return default_value;
    }
    char* end = nullptr;
    const long parsed = std::strtol(value, &end, 10);
    if (end == value || (end != nullptr && *end != '\0') || parsed <= 0) {
        return default_value;
    }
    return static_cast<int>(parsed);
}

std::string getenv_string(const char* name) {
    const char* value = std::getenv(name);
    return value == nullptr ? std::string() : std::string(value);
}

std::string resolve_key_env(const std::string& configured_env, const char* fallback_env) {
    if (!configured_env.empty()) {
        const std::string value = getenv_string(configured_env.c_str());
        if (!value.empty()) {
            return value;
        }
    }
    if (fallback_env != nullptr) {
        return getenv_string(fallback_env);
    }
    return {};
}

bool is_unreserved(unsigned char ch) {
    return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
           ch == '-' || ch == '_' || ch == '.' || ch == '~';
}

std::string url_encode(const std::string& input) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(input.size() * 3);
    for (unsigned char ch : input) {
        if (is_unreserved(ch)) {
            out.push_back(static_cast<char>(ch));
        } else if (ch == ' ') {
            out.push_back('+');
        } else {
            out.push_back('%');
            out.push_back(hex[ch >> 4]);
            out.push_back(hex[ch & 0x0F]);
        }
    }
    return out;
}

std::string strip_tags(const std::string& html) {
    std::string out;
    out.reserve(html.size());
    bool in_tag = false;
    for (char ch : html) {
        if (ch == '<') {
            in_tag = true;
            continue;
        }
        if (ch == '>') {
            in_tag = false;
            continue;
        }
        if (!in_tag) {
            out.push_back(ch);
        }
    }
    return trim_ascii(out);
}

std::string decode_html_entities(const std::string& input) {
    std::string out;
    out.reserve(input.size());
    for (size_t i = 0; i < input.size(); ++i) {
        if (input[i] != '&') {
            out.push_back(input[i]);
            continue;
        }
        const size_t semi = input.find(';', i + 1);
        if (semi == std::string::npos || semi - i > 16) {
            out.push_back(input[i]);
            continue;
        }
        const std::string entity = input.substr(i + 1, semi - i - 1);
        if (entity == "amp") {
            out.push_back('&');
        } else if (entity == "lt") {
            out.push_back('<');
        } else if (entity == "gt") {
            out.push_back('>');
        } else if (entity == "quot") {
            out.push_back('"');
        } else if (entity == "apos") {
            out.push_back('\'');
        } else if (entity == "nbsp") {
            out.push_back(' ');
        } else {
            out.append(input, i, semi - i + 1);
        }
        i = semi;
    }
    return out;
}

void append_unique_result(std::vector<SearchResult>& results, SearchResult item, int max_results) {
    if (results.size() >= static_cast<size_t>(max_results)) {
        return;
    }
    item.title = trim_ascii(item.title);
    item.url = trim_ascii(item.url);
    item.snippet = trim_ascii(item.snippet);
    if (item.title.empty() && item.snippet.empty()) {
        return;
    }
    if (item.title.empty()) {
        item.title = item.url.empty() ? "Untitled result" : item.url;
    }
    for (const SearchResult& existing : results) {
        if (!item.url.empty() && existing.url == item.url) {
            return;
        }
    }
    results.push_back(std::move(item));
}

http::Request base_request(const Options& options) {
    http::Request request;
    request.headers.push_back(std::string("User-Agent: ") + kUserAgent);
    request.connect_timeout_seconds = options.connect_timeout_seconds;
    request.timeout_seconds = options.timeout_seconds > 0 ? options.timeout_seconds : 30;
    request.proxy = options.proxy;
    request.insecure_tls = options.insecure_tls;
    request.trace = options.trace_http;
    request.block_private_addresses = !options.allow_private;
    request.max_body_bytes = options.max_response_bytes;
    return request;
}

Error perform_http(http::Request request, const std::vector<std::string>& secrets, http::Response& response) {
    http::Result result = http::perform(request, secrets);
    if (!result.error.ok()) {
        return result.error;
    }
    if (result.response.status < 200 || result.response.status >= 300) {
        return {ErrorCode::HttpStatus,
                "HTTP " + std::to_string(result.response.status) + " from " + request.url};
    }
    response = std::move(result.response);
    return ok_error();
}

Error validate_query(const std::string& query) {
    if (trim_ascii(query).empty()) {
        return {ErrorCode::BadArgs, "web search requires a non-empty query"};
    }
    if (query.size() > 2048) {
        return {ErrorCode::BadArgs, "web search query exceeds 2048 characters"};
    }
    return ok_error();
}

std::string json_string_field(const json::Value& object, const char* key) {
    const json::Value* value = object.get(key);
    if (value == nullptr || !value->is_string()) {
        return {};
    }
    return value->string;
}

void append_related_topics(const json::Value& node, int max_results, std::vector<SearchResult>& results) {
    if (!node.is_object()) {
        return;
    }
    const json::Value* text = node.get("Text");
    const json::Value* first_url = node.get("FirstURL");
    if (text != nullptr && text->is_string() && first_url != nullptr && first_url->is_string()) {
        SearchResult item;
        const std::string full_text = text->string;
        const size_t dash = full_text.find(" - ");
        if (dash != std::string::npos) {
            item.title = full_text.substr(0, dash);
            item.snippet = full_text.substr(dash + 3);
        } else {
            item.title = full_text;
        }
        item.url = first_url->string;
        append_unique_result(results, std::move(item), max_results);
    }
    const json::Value* topics = node.get("Topics");
    if (topics != nullptr && topics->is_array()) {
        for (size_t i = 0; i < topics->array.size(); ++i) {
            const json::Value* child = topics->at(i);
            if (child != nullptr) {
                append_related_topics(*child, max_results, results);
            }
        }
    }
}

}  // namespace

Options default_options() {
    Options options;
    options.max_results = parse_positive_int_env("MAXIMUM_WEB_SEARCH_RESULTS", 3);
    options.tavily_api_key = getenv_string("TAVILY_API_KEY");
    options.firecrawl_api_key = getenv_string("FIRECRAWL_API_KEY");
    options.exa_api_key = getenv_string("EXA_API_KEY");
    const std::string exa_base = getenv_string("EXA_BASE_URL");
    if (!exa_base.empty()) {
        options.exa_base_url = exa_base;
    }
    options.searxng_base_url = getenv_string("SEARXNG_BASE_URL");
    return options;
}

Options options_for(const cli::Options& cli_options) {
    Options options;
    options.provider = cli_options.web_search_provider;
    options.max_results = cli_options.max_web_search_results;
    options.connect_timeout_seconds = cli_options.connect_timeout_seconds;
    options.timeout_seconds = cli_options.timeout_seconds;
    options.proxy = cli_options.proxy;
    options.insecure_tls = cli_options.insecure_tls;
    options.trace_http = cli_options.trace_http;
    options.allow_private = cli_options.allow_private_url_fetch;
    options.tavily_api_key = resolve_key_env(cli_options.tavily_key_env, "TAVILY_API_KEY");
    options.firecrawl_api_key = resolve_key_env(cli_options.firecrawl_key_env, "FIRECRAWL_API_KEY");
    options.exa_api_key = resolve_key_env(cli_options.exa_key_env, "EXA_API_KEY");
    options.exa_base_url = cli_options.exa_base_url.empty() ? "https://api.exa.ai" : cli_options.exa_base_url;
    options.searxng_base_url = cli_options.searxng_base_url;
    if (options.searxng_base_url.empty()) {
        options.searxng_base_url = getenv_string("SEARXNG_BASE_URL");
    }
    const int env_max = parse_positive_int_env("MAXIMUM_WEB_SEARCH_RESULTS", 0);
    if (env_max > 0 && !cli_options.max_web_search_results_explicit) {
        options.max_results = env_max;
    }
    return options;
}

std::string format_context_message(const std::string& query, const SearchResponse& response) {
    std::ostringstream out;
    out << "Web search results for: " << query << "\n";
    out << "Provider: " << response.provider_used << "\n\n";
    for (size_t i = 0; i < response.results.size(); ++i) {
        const SearchResult& item = response.results[i];
        out << (i + 1) << ". " << item.title << "\n";
        if (!item.url.empty()) {
            out << "URL: " << item.url << "\n";
        }
        if (!item.snippet.empty()) {
            out << item.snippet << "\n";
        }
        if (i + 1 != response.results.size()) {
            out << "\n";
        }
    }
    return out.str();
}

std::string format_plaintext_output(const std::string& query, const SearchResponse& response) {
    return format_context_message(query, response);
}

Error parse_duckduckgo_instant_answer(const std::string& body,
                                      int max_results,
                                      std::vector<SearchResult>& results) {
    results.clear();
    if (max_results <= 0) {
        return {ErrorCode::BadArgs, "max web search results must be greater than zero"};
    }
    json::ParseResult parsed = json::parse(body);
    if (!parsed.error.ok() || !parsed.value.is_object()) {
        return {ErrorCode::JsonParse, "could not parse DuckDuckGo search response"};
    }
    const json::Value& root = parsed.value;
    SearchResult primary;
    primary.title = json_string_field(root, "Heading");
    primary.url = json_string_field(root, "AbstractURL");
    primary.snippet = json_string_field(root, "Abstract");
    if (!primary.snippet.empty() || !primary.url.empty()) {
        if (primary.title.empty()) {
            primary.title = "DuckDuckGo instant answer";
        }
        append_unique_result(results, std::move(primary), max_results);
    }
    const json::Value* related = root.get("RelatedTopics");
    if (related != nullptr && related->is_array()) {
        for (size_t i = 0; i < related->array.size(); ++i) {
            const json::Value* child = related->at(i);
            if (child != nullptr) {
                append_related_topics(*child, max_results, results);
            }
        }
    }
    if (results.empty()) {
        return {ErrorCode::ProviderSchema, "DuckDuckGo returned no instant answer or related topics"};
    }
    return ok_error();
}

Error parse_tavily_response(const std::string& body, int max_results, std::vector<SearchResult>& results) {
    results.clear();
    json::ParseResult parsed = json::parse(body);
    if (!parsed.error.ok() || !parsed.value.is_object()) {
        return {ErrorCode::JsonParse, "could not parse Tavily search response"};
    }
    const json::Value* items = parsed.value.get("results");
    if (items == nullptr || !items->is_array()) {
        return {ErrorCode::ProviderSchema, "Tavily search response did not include results"};
    }
    for (size_t i = 0; i < items->array.size(); ++i) {
        const json::Value* item = items->at(i);
        if (item == nullptr || !item->is_object()) {
            continue;
        }
        SearchResult result;
        result.title = json_string_field(*item, "title");
        result.url = json_string_field(*item, "url");
        result.snippet = json_string_field(*item, "content");
        append_unique_result(results, std::move(result), max_results);
    }
    if (results.empty()) {
        return {ErrorCode::ProviderSchema, "Tavily search returned no results"};
    }
    return ok_error();
}

Error parse_firecrawl_response(const std::string& body, int max_results, std::vector<SearchResult>& results) {
    results.clear();
    json::ParseResult parsed = json::parse(body);
    if (!parsed.error.ok() || !parsed.value.is_object()) {
        return {ErrorCode::JsonParse, "could not parse Firecrawl search response"};
    }
    const json::Value* data = parsed.value.get("data");
    if (data == nullptr || !data->is_array()) {
        return {ErrorCode::ProviderSchema, "Firecrawl search response did not include data"};
    }
    for (size_t i = 0; i < data->array.size(); ++i) {
        const json::Value* item = data->at(i);
        if (item == nullptr || !item->is_object()) {
            continue;
        }
        SearchResult result;
        result.title = json_string_field(*item, "title");
        result.url = json_string_field(*item, "url");
        result.snippet = json_string_field(*item, "description");
        if (result.snippet.empty()) {
            result.snippet = json_string_field(*item, "markdown");
        }
        append_unique_result(results, std::move(result), max_results);
    }
    if (results.empty()) {
        return {ErrorCode::ProviderSchema, "Firecrawl search returned no results"};
    }
    return ok_error();
}

Error parse_exa_response(const std::string& body, int max_results, std::vector<SearchResult>& results) {
    results.clear();
    json::ParseResult parsed = json::parse(body);
    if (!parsed.error.ok() || !parsed.value.is_object()) {
        return {ErrorCode::JsonParse, "could not parse Exa search response"};
    }
    const json::Value* items = parsed.value.get("results");
    if (items == nullptr || !items->is_array()) {
        return {ErrorCode::ProviderSchema, "Exa search response did not include results"};
    }
    for (size_t i = 0; i < items->array.size(); ++i) {
        const json::Value* item = items->at(i);
        if (item == nullptr || !item->is_object()) {
            continue;
        }
        SearchResult result;
        result.title = json_string_field(*item, "title");
        result.url = json_string_field(*item, "url");
        result.snippet = json_string_field(*item, "text");
        if (result.snippet.empty()) {
            result.snippet = json_string_field(*item, "summary");
        }
        append_unique_result(results, std::move(result), max_results);
    }
    if (results.empty()) {
        return {ErrorCode::ProviderSchema, "Exa search returned no results"};
    }
    return ok_error();
}

Error parse_searxng_response(const std::string& body, int max_results, std::vector<SearchResult>& results) {
    results.clear();
    json::ParseResult parsed = json::parse(body);
    if (!parsed.error.ok() || !parsed.value.is_object()) {
        return {ErrorCode::JsonParse, "could not parse Searxng search response"};
    }
    const json::Value* items = parsed.value.get("results");
    if (items == nullptr || !items->is_array()) {
        return {ErrorCode::ProviderSchema, "Searxng search response did not include results"};
    }
    for (size_t i = 0; i < items->array.size(); ++i) {
        const json::Value* item = items->at(i);
        if (item == nullptr || !item->is_object()) {
            continue;
        }
        SearchResult result;
        result.title = json_string_field(*item, "title");
        result.url = json_string_field(*item, "url");
        result.snippet = json_string_field(*item, "content");
        append_unique_result(results, std::move(result), max_results);
    }
    if (results.empty()) {
        return {ErrorCode::ProviderSchema, "Searxng search returned no results"};
    }
    return ok_error();
}

Error parse_google_search_html(const std::string& html, int max_results, std::vector<SearchResult>& results) {
    results.clear();
    if (max_results <= 0) {
        return {ErrorCode::BadArgs, "max web search results must be greater than zero"};
    }

    size_t pos = 0;
    while (pos < html.size() && results.size() < static_cast<size_t>(max_results)) {
        const size_t h3_open = html.find("<h3", pos);
        if (h3_open == std::string::npos) {
            break;
        }
        const size_t h3_close = html.find("</h3>", h3_open);
        if (h3_close == std::string::npos) {
            break;
        }
        const size_t anchor_open = html.rfind("<a ", h3_open);
        if (anchor_open == std::string::npos || h3_open - anchor_open > 512) {
            pos = h3_close + 5;
            continue;
        }
        const size_t href_pos = html.find("href=\"", anchor_open);
        if (href_pos == std::string::npos || href_pos > h3_open) {
            pos = h3_close + 5;
            continue;
        }
        const size_t href_start = href_pos + 6;
        const size_t href_end = html.find('"', href_start);
        if (href_end == std::string::npos) {
            pos = h3_close + 5;
            continue;
        }
        std::string url = html.substr(href_start, href_end - href_start);
        if (url.rfind("/url?", 0) == 0) {
            const size_t q_pos = url.find("q=");
            const size_t amp = url.find('&', q_pos == std::string::npos ? 0 : q_pos + 2);
            if (q_pos != std::string::npos) {
                url = url.substr(q_pos + 2, amp == std::string::npos ? std::string::npos : amp - q_pos - 2);
            }
        }
        if (url.empty() || url[0] == '#' || url.rfind("javascript:", 0) == 0) {
            pos = h3_close + 5;
            continue;
        }

        SearchResult item;
        item.url = decode_html_entities(url);
        item.title = decode_html_entities(strip_tags(html.substr(h3_open, h3_close + 5 - h3_open)));

        const size_t snippet_window_end = std::min(html.size(), h3_close + 1200);
        const std::string window = html.substr(h3_close + 5, snippet_window_end - h3_close - 5);
        std::string snippet;
        const std::string markers[] = {"VwiC3b", "yXK7lf", "MUxGbd", "st"};
        for (const std::string& marker : markers) {
            const std::string needle = "class=\"" + marker;
            const size_t marker_pos = window.find(needle);
            if (marker_pos != std::string::npos) {
                const size_t gt = window.find('>', marker_pos);
                const size_t close = window.find("</", gt == std::string::npos ? marker_pos : gt);
                if (gt != std::string::npos && close != std::string::npos && close > gt) {
                    snippet = decode_html_entities(strip_tags(window.substr(gt + 1, close - gt - 1)));
                    break;
                }
            }
        }
        item.snippet = snippet;
        append_unique_result(results, std::move(item), max_results);
        pos = h3_close + 5;
    }

    if (results.empty()) {
        return {ErrorCode::ProviderSchema, "could not parse Google search results from HTML"};
    }
    return ok_error();
}

namespace {

Error search_tavily(const std::string& query,
                    const Options& options,
                    std::vector<SearchResult>& results,
                    runtime::CancellationToken cancellation) {
    if (options.tavily_api_key.empty()) {
        return {ErrorCode::Auth, "Tavily search requires TAVILY_API_KEY or web_search.tavily_key_env"};
    }
    http::Request request = base_request(options);
    request.method = "POST";
    request.url = "https://api.tavily.com/search";
    request.headers.push_back("Content-Type: application/json");
    request.body = std::string("{\"query\":") + json::quote(query) +
                   ",\"max_results\":" + std::to_string(options.max_results) + ",\"api_key\":" +
                   json::quote(options.tavily_api_key) + "}";
    request.cancellation = cancellation;
    http::Response response;
    Error err = perform_http(request, {options.tavily_api_key}, response);
    if (!err.ok()) {
        return err;
    }
    return parse_tavily_response(response.body, options.max_results, results);
}

Error search_firecrawl(const std::string& query,
                       const Options& options,
                       std::vector<SearchResult>& results,
                       runtime::CancellationToken cancellation) {
    if (options.firecrawl_api_key.empty()) {
        return {ErrorCode::Auth, "Firecrawl search requires FIRECRAWL_API_KEY or web_search.firecrawl_key_env"};
    }
    http::Request request = base_request(options);
    request.method = "POST";
    request.url = "https://api.firecrawl.dev/v1/search";
    request.headers.push_back("Content-Type: application/json");
    request.headers.push_back("Authorization: Bearer " + options.firecrawl_api_key);
    request.body = std::string("{\"query\":") + json::quote(query) + ",\"limit\":" +
                   std::to_string(options.max_results) + "}";
    request.cancellation = cancellation;
    http::Response response;
    Error err = perform_http(request, {options.firecrawl_api_key}, response);
    if (!err.ok()) {
        return err;
    }
    return parse_firecrawl_response(response.body, options.max_results, results);
}

Error search_exa(const std::string& query,
                 const Options& options,
                 std::vector<SearchResult>& results,
                 runtime::CancellationToken cancellation) {
    if (options.exa_api_key.empty()) {
        return {ErrorCode::Auth, "Exa search requires EXA_API_KEY or web_search.exa_key_env"};
    }
    std::string base = trim_ascii(options.exa_base_url);
    while (!base.empty() && base.back() == '/') {
        base.pop_back();
    }
    if (base.empty()) {
        base = "https://api.exa.ai";
    }
    http::Request request = base_request(options);
    request.method = "POST";
    request.url = base + "/search";
    request.headers.push_back("Content-Type: application/json");
    request.headers.push_back("x-api-key: " + options.exa_api_key);
    request.body = std::string("{\"query\":") + json::quote(query) + ",\"numResults\":" +
                   std::to_string(options.max_results) + "}";
    request.cancellation = cancellation;
    http::Response response;
    Error err = perform_http(request, {options.exa_api_key}, response);
    if (!err.ok()) {
        return err;
    }
    return parse_exa_response(response.body, options.max_results, results);
}

Error search_searxng(const std::string& query,
                     const Options& options,
                     std::vector<SearchResult>& results,
                     runtime::CancellationToken cancellation) {
    if (options.searxng_base_url.empty()) {
        return {ErrorCode::BadArgs, "Searxng search requires web_search.searxng_base_url or SEARXNG_BASE_URL"};
    }
    std::string base = trim_ascii(options.searxng_base_url);
    while (!base.empty() && base.back() == '/') {
        base.pop_back();
    }
    http::Request request = base_request(options);
    request.method = "GET";
    request.url = base + "/search?q=" + url_encode(query) + "&format=json";
    request.headers.push_back("Accept: application/json");
    request.cancellation = cancellation;
    http::Response response;
    Error err = perform_http(request, {}, response);
    if (!err.ok()) {
        return err;
    }
    return parse_searxng_response(response.body, options.max_results, results);
}

Error search_duckduckgo(const std::string& query,
                        const Options& options,
                        std::vector<SearchResult>& results,
                        runtime::CancellationToken cancellation) {
    http::Request request = base_request(options);
    request.method = "GET";
    request.url = "https://api.duckduckgo.com/?q=" + url_encode(query) + "&format=json&no_html=1&skip_disambig=1";
    request.headers.push_back("Accept: application/json");
    request.cancellation = cancellation;
    http::Response response;
    Error err = perform_http(request, {}, response);
    if (!err.ok()) {
        return err;
    }
    return parse_duckduckgo_instant_answer(response.body, options.max_results, results);
}

Error search_google_html(const std::string& query,
                         const Options& options,
                         std::vector<SearchResult>& results,
                         runtime::CancellationToken cancellation) {
    http::Request request = base_request(options);
    request.method = "GET";
    request.url = "https://www.google.com/search?q=" + url_encode(query) + "&hl=en";
    request.headers.push_back("Accept: text/html,application/xhtml+xml");
    request.cancellation = cancellation;
    http::Response response;
    Error err = perform_http(request, {}, response);
    if (!err.ok()) {
        return err;
    }
    return parse_google_search_html(response.body, options.max_results, results);
}

using ProviderFn = Error (*)(const std::string&, const Options&, std::vector<SearchResult>&,
                             runtime::CancellationToken);

struct ProviderEntry {
    const char* name;
    ProviderFn search;
    bool needs_credentials;
    bool (*available)(const Options&);
};

bool tavily_available(const Options& options) { return !options.tavily_api_key.empty(); }
bool firecrawl_available(const Options& options) { return !options.firecrawl_api_key.empty(); }
bool exa_available(const Options& options) { return !options.exa_api_key.empty(); }
bool searxng_available(const Options& options) { return !options.searxng_base_url.empty(); }
bool duckduckgo_available(const Options&) { return true; }
bool google_available(const Options&) { return true; }

const std::vector<ProviderEntry>& provider_catalog() {
    static const std::vector<ProviderEntry> entries = {
        {"tavily", search_tavily, true, tavily_available},
        {"firecrawl", search_firecrawl, true, firecrawl_available},
        {"exa", search_exa, true, exa_available},
        {"searxng", search_searxng, true, searxng_available},
        {"duckduckgo", search_duckduckgo, false, duckduckgo_available},
        {"google", search_google_html, false, google_available},
    };
    return entries;
}

const ProviderEntry* find_provider(const std::string& name) {
    const std::string normalized = lower_ascii(trim_ascii(name));
    for (const ProviderEntry& entry : provider_catalog()) {
        if (normalized == entry.name) {
            return &entry;
        }
    }
    if (normalized == "ddg" || normalized == "duckduckgo_instant") {
        return find_provider("duckduckgo");
    }
    return nullptr;
}

std::vector<const ProviderEntry*> auto_provider_order(const Options& options) {
    std::vector<const ProviderEntry*> ordered;
    for (const ProviderEntry& entry : provider_catalog()) {
        if (entry.needs_credentials && entry.available(options)) {
            ordered.push_back(&entry);
        }
    }
    return ordered;
}

Error run_provider(const ProviderEntry& entry,
                   const std::string& query,
                   const Options& options,
                   std::vector<SearchResult>& results,
                   runtime::CancellationToken cancellation) {
    if (entry.needs_credentials && !entry.available(options)) {
        return {ErrorCode::Auth, std::string(entry.name) + " search is not configured"};
    }
    return entry.search(query, options, results, cancellation);
}

}  // namespace

Error search(const std::string& query,
             const Options& options,
             SearchResponse& response,
             runtime::CancellationToken cancellation) {
    response = {};
    Error err = validate_query(query);
    if (!err.ok()) {
        return err;
    }
    if (options.max_results <= 0) {
        return {ErrorCode::BadArgs, "MAXIMUM_WEB_SEARCH_RESULTS must be greater than zero"};
    }
    if (cancellation.cancelled()) {
        return {ErrorCode::Cancelled, "web search cancelled"};
    }

    std::vector<Error> failures;
    const std::string provider_name = lower_ascii(trim_ascii(options.provider));

    auto try_provider = [&](const ProviderEntry& entry) -> bool {
        std::vector<SearchResult> results;
        Error provider_error = run_provider(entry, query, options, results, cancellation);
        if (provider_error.ok()) {
            response.results = std::move(results);
            response.provider_used = entry.name;
            return true;
        }
        failures.push_back(std::move(provider_error));
        return false;
    };

    if (!provider_name.empty() && provider_name != "auto") {
        const ProviderEntry* entry = find_provider(provider_name);
        if (entry == nullptr) {
            return {ErrorCode::BadArgs,
                    "unknown web search provider: " + options.provider +
                        " (expected auto, tavily, firecrawl, exa, searxng, duckduckgo, or google)"};
        }
        if (try_provider(*entry)) {
            return ok_error();
        }
        if (entry->needs_credentials) {
            for (const ProviderEntry* fallback : {find_provider("duckduckgo"), find_provider("google")}) {
                if (fallback != nullptr && fallback != entry && try_provider(*fallback)) {
                    return ok_error();
                }
            }
        }
    } else {
        for (const ProviderEntry* entry : auto_provider_order(options)) {
            if (try_provider(*entry)) {
                return ok_error();
            }
        }
        for (const char* fallback_name : {"duckduckgo", "google"}) {
            const ProviderEntry* entry = find_provider(fallback_name);
            if (entry != nullptr && try_provider(*entry)) {
                return ok_error();
            }
        }
    }

    std::ostringstream message;
    message << "web search failed for query: " << query;
    for (size_t i = 0; i < failures.size(); ++i) {
        message << "\n- " << failures[i].message;
    }
    return {ErrorCode::HttpStatus, message.str()};
}

}  // namespace pkchat::search