#include "search/test_search.hpp"
#include "support/test_support.hpp"

#include "search/search.hpp"

#include <string>
#include <vector>

namespace ainiux::test::search {

namespace {

using ainiux::test::check;
using ainiux::test::read_fixture;

void test_duckduckgo_parser_fixture() {
    const std::string body = read_fixture("tests/fixtures/duckduckgo_web_scraping.json");
    std::vector<ainiux::search::SearchResult> results;
    ainiux::Error err = ainiux::search::parse_duckduckgo_instant_answer(body, 3, results);
    check(err.ok(), "DuckDuckGo fixture parses successfully");
    check(results.size() == 2, "DuckDuckGo fixture returns instant answer and related topic");
    check(results[0].title == "Web scraping", "DuckDuckGo instant answer title is preserved");
    check(results[0].url == "https://en.wikipedia.org/wiki/Web_scraping",
          "DuckDuckGo instant answer URL is preserved");
    check(results[0].snippet.find("Web scraping") == 0,
          "DuckDuckGo instant answer abstract is preserved");
}

void test_duckduckgo_empty_abstract_uses_related_topics() {
    const std::string body =
        "{\"Heading\":\"\",\"Abstract\":\"\",\"RelatedTopics\":[{\"Text\":\"Alpha - one\","
        "\"FirstURL\":\"https://example.com/a\"}]}";
    std::vector<ainiux::search::SearchResult> results;
    ainiux::Error err = ainiux::search::parse_duckduckgo_instant_answer(body, 3, results);
    check(err.ok(), "DuckDuckGo related topics parse when abstract is empty");
    check(results.size() == 1, "DuckDuckGo related topic fills empty abstract");
    check(results[0].title == "Alpha", "DuckDuckGo related topic title is split from text");
}

void test_duckduckgo_html_parser_fixture() {
    const std::string html = read_fixture("tests/fixtures/duckduckgo_html_sample.html");
    std::vector<ainiux::search::SearchResult> results;
    ainiux::Error err = ainiux::search::parse_duckduckgo_html(html, 10, results);
    check(err.ok(), "DuckDuckGo HTML fixture parses successfully");
    check(results.size() == 3, "DuckDuckGo HTML fixture returns three results");
    check(results[0].title == "Nocturne by Eino Leino | Poemist",
          "DuckDuckGo HTML title is extracted");
    check(results[0].url == "https://www.poemist.com/eino-leino/nocturne",
          "DuckDuckGo uddg= redirect is decoded");
    check(results[0].snippet.find("corncrake") != std::string::npos,
          "DuckDuckGo HTML snippet is extracted");
    check(results[0].snippet.find('\'') != std::string::npos,
          "DuckDuckGo HTML numeric entity in snippet is decoded");
    check(results[1].title.find("Nocturne") != std::string::npos,
          "DuckDuckGo HTML second title is extracted");
    check(results[1].url == "https://fi.wikipedia.org/wiki/Nocturne_(runo)",
          "DuckDuckGo HTML second URL is decoded");
    check(results[2].url == "https://example.com/direct-link",
          "DuckDuckGo HTML preserves direct https links");
    check(results[2].title == "Direct HTTPS Result",
          "DuckDuckGo HTML direct-link title is extracted");

    std::vector<ainiux::search::SearchResult> capped;
    err = ainiux::search::parse_duckduckgo_html(html, 1, capped);
    check(err.ok() && capped.size() == 1, "DuckDuckGo HTML respects max_results");
}

void test_duckduckgo_html_empty_rejected() {
    std::vector<ainiux::search::SearchResult> results;
    ainiux::Error err =
        ainiux::search::parse_duckduckgo_html("<html><body>no results</body></html>", 3, results);
    check(!err.ok() && err.code == ainiux::ErrorCode::ProviderSchema,
          "DuckDuckGo HTML with no results is rejected");
}

void test_result_url_truncation_via_tavily_shape() {
    // Overlong query strings should not bloat search results.
    std::string long_query(600, 'a');
    const std::string body =
        "{\"results\":[{\"title\":\"Long\",\"url\":\"https://example.com/path?q=" + long_query +
        "\",\"content\":\"snippet here\"}]}";
    std::vector<ainiux::search::SearchResult> results;
    ainiux::Error err = ainiux::search::parse_tavily_response(body, 3, results);
    check(err.ok() && results.size() == 1, "tavily parse with long URL succeeds");
    check(results[0].url.size() <= 512, "result URL is truncated to a bounded length");
    check(results[0].url.find("https://example.com/path") == 0,
          "truncated URL keeps scheme host and path prefix");
}

void test_tavily_parser_fixture() {
    const std::string body = read_fixture("tests/fixtures/tavily_search.json");
    std::vector<ainiux::search::SearchResult> results;
    ainiux::Error err = ainiux::search::parse_tavily_response(body, 3, results);
    check(err.ok(), "Tavily fixture parses successfully");
    check(results.size() == 1, "Tavily fixture returns one result");
    check(results[0].title == "Ainiux", "Tavily title is preserved");
}

void test_search_rejects_empty_query() {
    ainiux::search::Options options = ainiux::search::default_options();
    ainiux::search::SearchResponse response;
    ainiux::Error err = ainiux::search::search("   ", options, response);
    check(!err.ok() && err.code == ainiux::ErrorCode::BadArgs,
          "empty web search query is rejected");
}

void test_format_context_message() {
    ainiux::search::SearchResponse response;
    response.provider_used = "duckduckgo";
    response.results.push_back({"Ainiux", "https://example.com", "A CLI chat client"});
    const std::string message =
        ainiux::search::format_context_message("ainiux", response);
    check(message.find("Web search results for: ainiux") != std::string::npos,
          "search context message includes query");
    check(message.find("Provider:") == std::string::npos,
          "search context message does not duplicate provider line");
    check(message.find("URL: https://example.com") != std::string::npos,
          "search context message includes result URL");
}

}  // namespace

void run_all() {
    test_duckduckgo_parser_fixture();
    test_duckduckgo_empty_abstract_uses_related_topics();
    test_duckduckgo_html_parser_fixture();
    test_duckduckgo_html_empty_rejected();
    test_result_url_truncation_via_tavily_shape();
    test_tavily_parser_fixture();
    test_search_rejects_empty_query();
    test_format_context_message();
}

}  // namespace ainiux::test::search