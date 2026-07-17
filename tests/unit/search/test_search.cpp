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

void test_google_html_parser_fixture() {
    const std::string html = read_fixture("tests/fixtures/google_search_sample.html");
    std::vector<ainiux::search::SearchResult> results;
    ainiux::Error err = ainiux::search::parse_google_search_html(html, 3, results);
    check(err.ok(), "Google HTML fixture parses successfully");
    check(results.size() == 2, "Google HTML fixture returns two results");
    check(results[0].title == "Ainiux Example Result", "Google HTML title is extracted");
    check(results[0].url == "https://example.com/ainiux", "Google HTML URL is extracted");
    check(results[0].snippet.find("ainiux") != std::string::npos,
          "Google HTML snippet is extracted");
    check(results[1].url == "https://docs.example.com/guide",
          "Google /url?q= links are decoded");
}

void test_google_modern_html_parser_fixture() {
    const std::string html = read_fixture("tests/fixtures/google_search_modern.html");
    std::vector<ainiux::search::SearchResult> results;
    ainiux::Error err = ainiux::search::parse_google_search_html(html, 5, results);
    check(err.ok(), "Google modern HTML fixture parses successfully");
    check(results.size() == 3, "Google modern HTML fixture returns three results");
    check(results[0].title == "Modern Google Result Title", "Google modern nested h3 title is extracted");
    check(results[0].url == "https://example.com/article", "Google modern /url?q= link is decoded");
    check(results[0].snippet.find("modern layout") != std::string::npos,
          "Google modern snippet is extracted");
    check(results[1].title == "Direct Link Result", "Google aria-label title is used when present");
    check(results[1].url == "https://direct.example.com/page", "Google direct https link is preserved");
    check(results[2].title == "Sibling Title Before Link", "Google sibling h3 title is extracted");
    check(results[2].url == "https://sibling.example.com/path", "Google sibling /url?q= link is decoded");
}

void test_google_blocked_html_parser() {
    const std::string html = read_fixture("tests/fixtures/google_search_blocked.html");
    std::vector<ainiux::search::SearchResult> results;
    ainiux::Error err = ainiux::search::parse_google_search_html(html, 3, results);
    check(!err.ok() && err.code == ainiux::ErrorCode::ProviderSchema,
          "Google blocked HTML is rejected");
    check(err.message.find("JavaScript-only") != std::string::npos,
          "Google blocked HTML error mentions JavaScript-only page");
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
    test_google_html_parser_fixture();
    test_google_modern_html_parser_fixture();
    test_google_blocked_html_parser();
    test_tavily_parser_fixture();
    test_search_rejects_empty_query();
    test_format_context_message();
}

}  // namespace ainiux::test::search