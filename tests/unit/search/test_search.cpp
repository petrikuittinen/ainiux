#include "search/test_search.hpp"
#include "support/test_support.hpp"

#include "search/search.hpp"

#include <string>
#include <vector>

namespace pkchat::test::search {

namespace {

using pkchat::test::check;
using pkchat::test::read_fixture;

void test_duckduckgo_parser_fixture() {
    const std::string body = read_fixture("tests/fixtures/duckduckgo_web_scraping.json");
    std::vector<pkchat::search::SearchResult> results;
    pkchat::Error err = pkchat::search::parse_duckduckgo_instant_answer(body, 3, results);
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
    std::vector<pkchat::search::SearchResult> results;
    pkchat::Error err = pkchat::search::parse_duckduckgo_instant_answer(body, 3, results);
    check(err.ok(), "DuckDuckGo related topics parse when abstract is empty");
    check(results.size() == 1, "DuckDuckGo related topic fills empty abstract");
    check(results[0].title == "Alpha", "DuckDuckGo related topic title is split from text");
}

void test_google_html_parser_fixture() {
    const std::string html = read_fixture("tests/fixtures/google_search_sample.html");
    std::vector<pkchat::search::SearchResult> results;
    pkchat::Error err = pkchat::search::parse_google_search_html(html, 3, results);
    check(err.ok(), "Google HTML fixture parses successfully");
    check(results.size() == 2, "Google HTML fixture returns two results");
    check(results[0].title == "Pkchat Example Result", "Google HTML title is extracted");
    check(results[0].url == "https://example.com/pkchat", "Google HTML URL is extracted");
    check(results[0].snippet.find("pkchat") != std::string::npos,
          "Google HTML snippet is extracted");
    check(results[1].url == "https://docs.example.com/guide",
          "Google /url?q= links are decoded");
}

void test_tavily_parser_fixture() {
    const std::string body = read_fixture("tests/fixtures/tavily_search.json");
    std::vector<pkchat::search::SearchResult> results;
    pkchat::Error err = pkchat::search::parse_tavily_response(body, 3, results);
    check(err.ok(), "Tavily fixture parses successfully");
    check(results.size() == 1, "Tavily fixture returns one result");
    check(results[0].title == "Pkchat", "Tavily title is preserved");
}

void test_search_rejects_empty_query() {
    pkchat::search::Options options = pkchat::search::default_options();
    pkchat::search::SearchResponse response;
    pkchat::Error err = pkchat::search::search("   ", options, response);
    check(!err.ok() && err.code == pkchat::ErrorCode::BadArgs,
          "empty web search query is rejected");
}

void test_format_context_message() {
    pkchat::search::SearchResponse response;
    response.provider_used = "duckduckgo";
    response.results.push_back({"Pkchat", "https://example.com", "A CLI chat client"});
    const std::string message =
        pkchat::search::format_context_message("pkchat", response);
    check(message.find("Web search results for: pkchat") != std::string::npos,
          "search context message includes query");
    check(message.find("Provider: duckduckgo") != std::string::npos,
          "search context message includes provider");
    check(message.find("URL: https://example.com") != std::string::npos,
          "search context message includes result URL");
}

}  // namespace

void run_all() {
    test_duckduckgo_parser_fixture();
    test_duckduckgo_empty_abstract_uses_related_topics();
    test_google_html_parser_fixture();
    test_tavily_parser_fixture();
    test_search_rejects_empty_query();
    test_format_context_message();
}

}  // namespace pkchat::test::search