#pragma once

#include <string>
#include <vector>

#include "cli/args.hpp"
#include "common.hpp"
#include "runtime/runtime.hpp"

namespace ainiux::search {

struct SearchResult {
    std::string title;
    std::string url;
    std::string snippet;
};

struct Options {
    std::string provider = "auto";
    int max_results = 3;
    long connect_timeout_seconds = 10;
    long timeout_seconds = 30;
    long max_response_bytes = 2097152;
    std::string proxy;
    bool insecure_tls = false;
    bool trace_http = false;
    bool allow_private = false;
    std::string tavily_api_key;
    std::string firecrawl_api_key;
    std::string exa_api_key;
    std::string exa_base_url = "https://api.exa.ai";
    std::string searxng_base_url;
};

struct SearchResponse {
    std::vector<SearchResult> results;
    std::string provider_used;
};

Options options_for(const cli::Options& cli_options);
Options default_options();

std::string format_context_message(const std::string& query,
                                   const SearchResponse& response);
std::string format_plaintext_output(const std::string& query, const SearchResponse& response);

Error search(const std::string& query,
             const Options& options,
             SearchResponse& response,
             runtime::CancellationToken cancellation = runtime::CancellationToken());

// Test hooks for provider response parsing.
Error parse_duckduckgo_instant_answer(const std::string& body,
                                      int max_results,
                                      std::vector<SearchResult>& results);
// Keyless SERP: html.duckduckgo.com result blocks (title, URL, snippet).
Error parse_duckduckgo_html(const std::string& html,
                            int max_results,
                            std::vector<SearchResult>& results);
Error parse_tavily_response(const std::string& body,
                            int max_results,
                            std::vector<SearchResult>& results);
Error parse_firecrawl_response(const std::string& body,
                               int max_results,
                               std::vector<SearchResult>& results);
Error parse_exa_response(const std::string& body,
                         int max_results,
                         std::vector<SearchResult>& results);
Error parse_searxng_response(const std::string& body,
                             int max_results,
                             std::vector<SearchResult>& results);

}  // namespace ainiux::search