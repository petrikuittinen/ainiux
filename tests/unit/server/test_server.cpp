#include "server/test_server.hpp"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <curl/curl.h>

#include "cli/args.hpp"
#include "json/json.hpp"
#include "platform/filesystem.hpp"
#include "server/auth.hpp"
#include "server/chat_service.hpp"
#include "server/http_parser.hpp"
#include "server/event_broker.hpp"
#include "server/job_registry.hpp"
#include "server/job_service.hpp"
#include "server/image_input_store.hpp"
#include "server/listener.hpp"
#include "server/mcp_adapter.hpp"
#include "server/router.hpp"
#include "server/session_hub.hpp"
#include "server/server.hpp"
#include "server/tls.hpp"
#include "server/web_ui_launch.hpp"
#include "server/workspace_service.hpp"
#include "support/test_support.hpp"

namespace ainiux::test::server_control {
namespace {

using ainiux::test::check;
using namespace ainiux::server;
namespace http = ainiux::server::http;

std::string request_text(const std::string& path = "/ainiux/v1/health",
                         const std::string& token = "controller") {
    return "GET " + path + " HTTP/1.1\r\nHost: 127.0.0.1\r\nAuthorization: Bearer " +
           token + "\r\n\r\n";
}

http::Request parsed_request(const std::string& text) {
    http::Parser parser;
    check(parser.feed(text) == http::ParseState::Complete, "server test request parses");
    return parser.request();
}

bool wait_terminal(const std::shared_ptr<Job>& job) {
    for (int i = 0; i < 200; ++i) {
        if (job->terminal()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return job->terminal();
}

void test_fragmented_parser_and_pipeline() {
    const std::string first = request_text();
    const std::string second = request_text("/ainiux/v1/status");
    http::Parser parser;
    for (std::size_t i = 0; i + 1 < first.size(); ++i) {
        check(parser.feed(std::string_view(first.data() + i, 1)) == http::ParseState::NeedMore,
              "fragmented HTTP request waits for all bytes");
    }
    check(parser.feed(std::string_view(first.data() + first.size() - 1, 1)) ==
              http::ParseState::Complete,
          "fragmented HTTP request completes on final byte");
    check(parser.request().path == "/ainiux/v1/health" && parser.request().body.empty(),
          "strict parser preserves normalized path and empty body");

    http::Parser pipelined;
    check(pipelined.feed(first + second) == http::ParseState::Complete,
          "parser completes the first pipelined request");
    check(pipelined.take_remaining() == second,
          "parser returns unconsumed pipelined bytes exactly");
}

void expect_parse_failure(const std::string& text, int status, const char* label) {
    http::Parser parser;
    check(parser.feed(text) == http::ParseState::Failed && parser.error().status == status, label);
}

void test_strict_framing_and_limits() {
    expect_parse_failure("GET /ainiux/v1/health HTTP/1.1\nHost: localhost\n\n", 400,
                         "bare-LF HTTP is rejected");
    expect_parse_failure("GET /ainiux/v1/%2e%2e/x HTTP/1.1\r\nHost: localhost\r\n\r\n", 400,
                         "encoded ambiguous path is rejected");
    http::Parser encoded_query;
    check(encoded_query.feed("GET /ainiux/v1/dired?path=hello%20world HTTP/1.1\r\n"
                             "Host: localhost\r\n\r\n") == http::ParseState::Complete,
          "strict parser leaves percent-encoded query values to the owning route schema");
    expect_parse_failure("GET /../x HTTP/1.1\r\nHost: localhost\r\n\r\n", 400,
                         "path traversal is rejected");
    expect_parse_failure("POST /ainiux/v1/x HTTP/1.1\r\nHost: localhost\r\n"
                         "Content-Length: 0\r\nContent-Length: 0\r\n\r\n", 400,
                         "duplicate Content-Length is rejected");
    expect_parse_failure("POST /ainiux/v1/x HTTP/1.1\r\nHost: localhost\r\n"
                         "Transfer-Encoding: chunked\r\n\r\n", 501,
                         "transfer encoding is rejected");
    expect_parse_failure("GET / HTTP/1.1\r\nHost: localhost\r\n folded\r\n\r\n", 400,
                         "obsolete folded headers are rejected");
    expect_parse_failure("GET / HTTP/1.1\r\n\r\n", 400,
                         "missing HTTP/1.1 Host is rejected");

    http::Parser small(3);
    check(small.feed("POST /x HTTP/1.1\r\nHost: localhost\r\nContent-Length: 4\r\n\r\n") ==
              http::ParseState::Failed && small.error().status == 413,
          "route body limit is enforced before buffering the body");

    const std::string png("\x89PNG\r\n\x1a\n", 8);
    http::Parser binary(Limits::upload_body_bytes);
    check(binary.feed("POST /ainiux/v1/images/inputs HTTP/1.1\r\nHost: localhost\r\n"
                      "Content-Length: 8\r\n\r\n" + png) == http::ParseState::Complete &&
              binary.request().body == png,
          "HTTP header line-ending checks do not reject binary image body bytes");

    std::string headers = "GET / HTTP/1.1\r\nHost: localhost\r\n";
    for (int i = 0; i < 100; ++i) headers += "X-" + std::to_string(i) + ": y\r\n";
    headers += "\r\n";
    expect_parse_failure(headers, 431, "header count includes Host and is bounded at 100");
}

void test_embedded_web_ui_assets_and_browser_security() {
    AuthConfig config{"controller", "mcp-token"};
    std::atomic<std::size_t> active{0};
    PublicStatus status{8766, 64, 128, &active};
    auto public_get = [](const std::string& path) {
        return parsed_request("GET " + path + " HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
    };

    Response index = route_request(public_get("/ui/"), config, status);
    check(index.status == 200 && index.content_type == "text/html; charset=utf-8" &&
              index.body.find("/ui/assets/app-v15.css") != std::string::npos &&
              index.body.find("/ui/assets/app-v16.js") != std::string::npos &&
              index.body.find(">Logout</button>") != std::string::npos &&
              index.body.find("data-panel=\"image-panel\">Image") != std::string::npos &&
              index.body.find("data-panel=\"video-panel\">Video") != std::string::npos &&
              index.body.find("list=\"chat-model-list\"") != std::string::npos &&
              index.body.find("id=\"chat-reasoning\"") != std::string::npos &&
              index.body.find("id=\"agent-reasoning\"") != std::string::npos &&
              index.body.find("aria-keyshortcuts=\"Control+R Alt+T Alt+W Escape\"") != std::string::npos &&
              index.body.find("id=\"chat-regenerate-button\"") != std::string::npos &&
              index.body.find("id=\"chat-cycle-reasoning-button\"") != std::string::npos &&
              index.body.find("id=\"chat-thinking-button\"") != std::string::npos &&
              index.body.find("id=\"agent-list\"") == std::string::npos &&
              index.body.find("id=\"new-agent-dialog\"") == std::string::npos &&
              index.body.find("id=\"image-output\"") != std::string::npos &&
              index.body.find("id=\"image-download-button\"") != std::string::npos &&
              index.body.find("id=\"image-input-files\"") != std::string::npos &&
              index.body.find("id=\"image-reset-button\"") != std::string::npos &&
              index.body.find("id=\"chat-api\"") == std::string::npos &&
              index.body.find("id=\"goal-api\"") == std::string::npos &&
              index.body.find("id=\"agent-api\"") == std::string::npos &&
              index.body.find("id=\"threads-heading\">Threads</h2>") != std::string::npos &&
              index.body.find("id=\"new-thread-button\"") <
                  index.body.find("id=\"thread-list\"") &&
              index.body.find("<p class=\"eyebrow\">Conversation</p>") == std::string::npos &&
              index.body.find("id=\"chat-heading\"") == std::string::npos &&
              index.body.find("id=\"chat-metrics\"") != std::string::npos &&
              index.body.find("id=\"agent-metrics\"") != std::string::npos &&
              index.body.find("id=\"file-edit-layer\"") != std::string::npos &&
              index.body.find("id=\"file-edit-highlight\"") != std::string::npos &&
              index.body.find("wrap=\"off\"") != std::string::npos &&
              index.body.find("Revision-safe files") == std::string::npos &&
              index.body.find("Revision conflict") == std::string::npos &&
              index.body.find("remember-token") == std::string::npos &&
              index.body.find("https://") == std::string::npos &&
              index.body.find("http://") == std::string::npos,
          "embedded WUI index is public boot content with versioned same-origin assets only");

    Response stylesheet = route_request(public_get("/ui/assets/app-v15.css"), config, status);
    const std::string stylesheet_headers = serialize_response(stylesheet, true);
    check(stylesheet.status == 200 && stylesheet.content_type == "text/css; charset=utf-8" &&
              stylesheet.body.find("prefers-color-scheme: dark") != std::string::npos &&
              stylesheet.body.find("prefers-reduced-motion: reduce") != std::string::npos &&
              stylesheet.body.find("html[data-theme=\"light\"]") != std::string::npos &&
              stylesheet.body.find("html[data-theme=\"dark\"]") != std::string::npos &&
              stylesheet.body.find("@media (max-width: 42rem)") != std::string::npos &&
              stylesheet.body.find(".topbar") != std::string::npos &&
              stylesheet.body.find(".image-layout") != std::string::npos &&
              stylesheet.body.find(".panel.placeholder-panel.active") != std::string::npos &&
              stylesheet.body.find("height: 100dvh") != std::string::npos &&
              stylesheet.body.find(".metrics-strip") != std::string::npos &&
              stylesheet.body.find(".agent-toolbar") != std::string::npos &&
              stylesheet.body.find(".agent-console { height: 100%; max-height: 100%; overflow: hidden; }") != std::string::npos &&
              stylesheet.body.find(".markdown-body") != std::string::npos &&
              stylesheet.body.find(".markdown-table-scroll") != std::string::npos &&
              stylesheet.body.find("--syntax-heading: #f9a8d4") != std::string::npos &&
              stylesheet.body.find("--syntax-link: #0451a5") != std::string::npos &&
              stylesheet.body.find("--syntax-keyword: #c4b5fd") != std::string::npos &&
              stylesheet.body.find("--thinking: #93c5fd") != std::string::npos &&
              stylesheet.body.find(".event-card.thinking") != std::string::npos &&
              stylesheet.body.find(".event-card.tool") != std::string::npos &&
              stylesheet.body.find(".event-card.notice") != std::string::npos &&
              stylesheet.body.find(".file-highlight") != std::string::npos &&
              stylesheet.body.find(".file-edit-layer") != std::string::npos &&
              stylesheet.body.find(".directory-entry") != std::string::npos &&
              stylesheet.body.find(".executable-entry") != std::string::npos &&
              stylesheet.body.find(".syntax-comment") != std::string::npos &&
              stylesheet.body.find(".syntax-property") != std::string::npos &&
              stylesheet.body.find("scrollbar-gutter: stable") != std::string::npos &&
              stylesheet.body.find("ui-monospace") != std::string::npos &&
              stylesheet.body.find("@import") == std::string::npos &&
              stylesheet.body.find("https://") == std::string::npos &&
              stylesheet.body.find("http://") == std::string::npos &&
              stylesheet_headers.find("Cache-Control: public, max-age=31536000, immutable") != std::string::npos,
          "embedded WUI CSS carries TUI-derived light/dark themes and responsive accessibility rules");

    Response javascript = route_request(public_get("/ui/assets/app-v16.js"), config, status);
    const std::string javascript_headers = serialize_response(javascript, true);
    check(javascript.status == 200 && javascript.content_type == "text/javascript; charset=utf-8" &&
              javascript.body.find("localStorage") != std::string::npos &&
              javascript.body.find("Invalid authentication") != std::string::npos &&
              javascript.body.find("Reconnecting") != std::string::npos &&
              javascript.body.find("window.addEventListener(\"online\"") != std::string::npos &&
              javascript.body.find("Last-Event-ID") != std::string::npos &&
              javascript.body.find("/jobs/models") != std::string::npos &&
              javascript.body.find("reasoning_options") != std::string::npos &&
              javascript.body.find("chat-reasoning") != std::string::npos &&
              javascript.body.find("event.type === \"delta\"") != std::string::npos &&
              javascript.body.find("updateVisibleChatStream") != std::string::npos &&
              javascript.body.find("data.action === \"append\"") != std::string::npos &&
              javascript.body.find("async function regenerateChat") != std::string::npos &&
              javascript.body.find("async function cancelActiveAgentTurn") != std::string::npos &&
              javascript.body.find("key === \"r\"") != std::string::npos &&
              javascript.body.find("key === \"t\"") != std::string::npos &&
              javascript.body.find("key === \"w\"") != std::string::npos &&
              javascript.body.find("control || event.altKey") != std::string::npos &&
              javascript.body.find("function toggleChatThinking") != std::string::npos &&
              javascript.body.find("function handleThemeCommand") != std::string::npos &&
              javascript.body.find("import { renderMarkdown } from \"./highlight-v4.js\"") != std::string::npos &&
              javascript.body.find("languageForPath") != std::string::npos &&
              javascript.body.find("event-card") != std::string::npos &&
              javascript.body.find("file-highlight") != std::string::npos &&
              javascript.body.find("function renderEditHighlight") != std::string::npos &&
              javascript.body.find("addEventListener(\"scroll\", syncEditorHighlightScroll)") != std::string::npos &&
              javascript.body.find("function renderChatContent") != std::string::npos &&
              javascript.body.find("function scheduleAgentRender") != std::string::npos &&
              javascript.body.find("async function ensureWorkspaceAgent") != std::string::npos &&
              javascript.body.find("const followTail =") != std::string::npos &&
              javascript.body.find("followTail ? events.scrollHeight : previousScrollTop") != std::string::npos &&
              javascript.body.find("function downloadGeneratedImage") != std::string::npos &&
              javascript.body.find("uploadImageInputs") != std::string::npos &&
              javascript.body.find("function resetImageForm") != std::string::npos &&
              javascript.body.find("./image-options-v1.js") != std::string::npos &&
              javascript.body.find("server_path") != std::string::npos &&
              javascript.body.find("function agentEventVisible") != std::string::npos &&
              javascript.body.find("apiId") == std::string::npos &&
              javascript.body.find("byId(\"chat-api\")") == std::string::npos &&
              javascript.body.find("event.key === \"Escape\"") != std::string::npos &&
              javascript.body.find("catalog.done = true") != std::string::npos &&
              javascript.body.find("Intl.DateTimeFormat") != std::string::npos &&
              javascript.body.find("event.shiftKey || event.altKey") != std::string::npos &&
              javascript.body.find("messages · revision") == std::string::npos &&
              javascript.body.find("parent revision") == std::string::npos &&
              javascript.body.find("textContent") != std::string::npos &&
              javascript.body.find("sessionStorage") == std::string::npos &&
              javascript.body.find("document.cookie") == std::string::npos &&
              javascript.body.find("URLSearchParams") == std::string::npos &&
              javascript.body.find("innerHTML") == std::string::npos &&
              javascript.body.find("https://") == std::string::npos &&
              javascript.body.find("http://") == std::string::npos &&
              javascript_headers.find("script-src 'self'") != std::string::npos &&
              javascript_headers.find("connect-src 'self'") != std::string::npos &&
              javascript_headers.find("Referrer-Policy: no-referrer") != std::string::npos &&
              javascript_headers.find("Permissions-Policy:") != std::string::npos &&
              javascript_headers.find("X-Frame-Options: DENY") != std::string::npos,
          "embedded WUI JavaScript uses authenticated fetch/replay and hardened same-origin headers");

    Response highlighter = route_request(public_get("/ui/assets/highlight-v4.js"), config, status);
    const std::string highlighter_headers = serialize_response(highlighter, true);
    check(highlighter.status == 200 &&
              highlighter.content_type == "text/javascript; charset=utf-8" &&
              highlighter.body.find("export function renderMarkdown") != std::string::npos &&
              highlighter.body.find("createDocumentFragment") != std::string::npos &&
              highlighter.body.find("createElement(documentRef, \"table\"") != std::string::npos &&
              highlighter.body.find("createElement(documentRef, `h${") != std::string::npos &&
              highlighter.body.find("noopener noreferrer") != std::string::npos &&
              highlighter.body.find("SAFE_SCHEMES") != std::string::npos &&
              highlighter.body.find("innerHTML") == std::string::npos &&
              highlighter.body.find("outerHTML") == std::string::npos &&
              highlighter.body.find("insertAdjacentHTML") == std::string::npos &&
              highlighter.body.find("DOMParser") == std::string::npos &&
              highlighter.body.find("fetch(") == std::string::npos &&
              highlighter_headers.find("Cache-Control: public, max-age=31536000, immutable") != std::string::npos &&
              highlighter_headers.find("script-src 'self'") != std::string::npos,
          "embedded WUI Markdown module builds safe semantic DOM under immutable CSP headers");

    Response syntax = route_request(public_get("/ui/assets/syntax-v3.js"), config, status);
    const std::string syntax_headers = serialize_response(syntax, true);
    check(syntax.status == 200 &&
              syntax.content_type == "text/javascript; charset=utf-8" &&
              syntax.body.find("export function appendHighlightedCode") != std::string::npos &&
              syntax.body.find("export function languageForPath") != std::string::npos &&
              syntax.body.find("function scanHtml") != std::string::npos &&
              syntax.body.find("javascript: \"javascript\"") != std::string::npos &&
              syntax.body.find("typescript: \"typescript\"") != std::string::npos &&
              syntax.body.find("python: \"python\"") != std::string::npos &&
              syntax.body.find("powershell: \"powershell\"") != std::string::npos &&
              syntax.body.find("assembly: \"assembly\"") != std::string::npos &&
              syntax.body.find("innerHTML") == std::string::npos &&
              syntax.body.find("outerHTML") == std::string::npos &&
              syntax.body.find("DOMParser") == std::string::npos &&
              syntax.body.find("fetch(") == std::string::npos &&
              syntax_headers.find("Cache-Control: public, max-age=31536000, immutable") != std::string::npos &&
              syntax_headers.find("script-src 'self'") != std::string::npos,
          "embedded WUI syntax module is dependency-free, DOM-safe, and immutable");

    Response image_options = route_request(public_get("/ui/assets/image-options-v1.js"), config, status);
    check(image_options.status == 200 &&
              image_options.body.find("export function normalizeImageCatalog") != std::string::npos &&
              image_options.body.find("export function customDimensionError") != std::string::npos &&
              image_options.body.find("export function resetImageFormValues") != std::string::npos &&
              image_options.body.find("fetch(") == std::string::npos,
          "embedded image option module is pure and served as an immutable exact-path asset");

    const std::size_t chat_submit_start = javascript.body.find("async function sendChatMessage");
    const std::size_t chat_submit_end = javascript.body.find("async function finishChatJob");
    check(chat_submit_start != std::string::npos && chat_submit_end > chat_submit_start &&
              javascript.body.substr(chat_submit_start, chat_submit_end - chat_submit_start)
                      .find("switchPanel(\"jobs-panel\")") == std::string::npos,
          "submitting chat work keeps the active browser panel on Chat");

    check(route_request(public_get("/ui/assets/"), config, status).status == 404,
          "WUI route serves only exact embedded assets and never a directory");
    check(route_request(public_get("/ui/assets/app-v1.js"), config, status).status == 404,
          "superseded immutable WUI asset URLs are not silently aliased");
    check(route_request(public_get("/ui/assets/app-v2.js"), config, status).status == 404,
          "the reconnect-loop asset URL is superseded instead of remaining cached");
    check(route_request(public_get("/ui/assets/app-v3.js"), config, status).status == 404,
          "the previous WUI asset URL is superseded after the chat layout update");
    check(route_request(public_get("/ui/assets/app-v4.js"), config, status).status == 404,
          "the previous WUI asset URL is superseded after chat navigation changes");
    check(route_request(public_get("/ui/assets/app-v5.js"), config, status).status == 404,
          "the previous immutable WUI asset is superseded after streaming changes");
    check(route_request(public_get("/ui/assets/app-v6.js"), config, status).status == 404,
          "the previous WUI asset is superseded after compact controls and provider defaults");
    check(route_request(public_get("/ui/assets/app-v7.js"), config, status).status == 404,
          "the previous WUI asset is superseded after the single-workspace redesign");
    check(route_request(public_get("/ui/assets/app-v8.js"), config, status).status == 404,
          "the previous WUI asset is superseded after the agent viewport fix");
    check(route_request(public_get("/ui/assets/app-v9.js"), config, status).status == 404 &&
              route_request(public_get("/ui/assets/app-v9.css"), config, status).status == 404,
          "the previous immutable WUI assets are superseded after Markdown rendering");
    check(route_request(public_get("/ui/assets/app-v10.js"), config, status).status == 404 &&
              route_request(public_get("/ui/assets/app-v10.css"), config, status).status == 404 &&
              route_request(public_get("/ui/assets/highlight-v1.js"), config, status).status == 404,
          "the Markdown-only WUI assets are superseded after fenced-code highlighting");
    check(route_request(public_get("/ui/assets/app-v11.js"), config, status).status == 404 &&
              route_request(public_get("/ui/assets/app-v11.css"), config, status).status == 404 &&
              route_request(public_get("/ui/assets/app-v12.js"), config, status).status == 404 &&
              route_request(public_get("/ui/assets/app-v12.css"), config, status).status == 404 &&
              route_request(public_get("/ui/assets/app-v13.js"), config, status).status == 404 &&
              route_request(public_get("/ui/assets/app-v13.css"), config, status).status == 404 &&
              route_request(public_get("/ui/assets/app-v14.js"), config, status).status == 404 &&
              route_request(public_get("/ui/assets/app-v14.css"), config, status).status == 404 &&
              route_request(public_get("/ui/assets/app-v15.js"), config, status).status == 404 &&
              route_request(public_get("/ui/assets/highlight-v2.js"), config, status).status == 404 &&
              route_request(public_get("/ui/assets/highlight-v3.js"), config, status).status == 404 &&
              route_request(public_get("/ui/assets/syntax-v1.js"), config, status).status == 404 &&
              route_request(public_get("/ui/assets/syntax-v2.js"), config, status).status == 404,
          "the first-batch WUI syntax assets are superseded after full language parity");
    http::Request post = parsed_request("POST /ui/ HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                                        "Content-Length: 0\r\n\r\n");
    check(route_request(post, config, status).status == 405,
          "WUI static routes reject non-GET methods");
    http::Request cross_origin = public_get("/ui/");
    cross_origin.headers["origin"] = "https://example.test";
    check(route_request(cross_origin, config, status).status == 403,
          "WUI boot assets retain the same strict Origin policy as the control API");
    check(route_request(public_get("/ainiux/v1/status"), config, status).status == 401,
          "making boot assets public does not weaken API authentication");
}

void test_auth_and_routes() {
    check(constant_time_equal("same", "same") && !constant_time_equal("same", "samf") &&
              !constant_time_equal("same", "same-longer"),
          "constant-time credential comparison handles equality and unequal lengths");
    AuthConfig config{"controller", "mcp-token"};
    http::Request full = parsed_request(request_text());
    check(authenticate(full, config) == AuthScope::FullControl,
          "full-control credential authenticates control route");
    http::Request wrong_scope = parsed_request(request_text("/ainiux/v1/status", "mcp-token"));
    check(authenticate(wrong_scope, config) == AuthScope::None,
          "MCP-only credential cannot authenticate control API");
    http::Request mcp = parsed_request(request_text("/mcp", "mcp-token"));
    check(authenticate(mcp, config) == AuthScope::McpOnly,
          "MCP-only credential authenticates only the MCP path");
    http::Request full_on_mcp = parsed_request(request_text("/mcp", "controller"));
    check(authenticate(full_on_mcp, config) == AuthScope::None,
          "full-control credential is not reused as an MCP credential");

    std::atomic<std::size_t> active{2};
    PublicStatus status{8766, 64, 128, &active};
    Response health = route_request(full, config, status);
    check(health.status == 200 && health.body == "{\"status\":\"ok\"}",
          "authenticated health is minimal");
    Response denied = route_request(wrong_scope, config, status);
    check(denied.status == 401 && denied.body.find("mcp-token") == std::string::npos,
          "wrong-scope response is unauthorized and does not expose credentials");
    http::Request status_request = parsed_request(request_text("/ainiux/v1/status"));
    Response status_response = route_request(status_request, config, status);
    check(status_response.status == 200 &&
              status_response.body.find("\"active\":2") != std::string::npos &&
              status_response.body.find("127.0.0.1") != std::string::npos,
          "status reports bounded public listener state");
    http::Request capabilities = parsed_request(request_text("/ainiux/v1/capabilities"));
    Response capability_response = route_request(capabilities, config, status);
    check(capability_response.status == 200 &&
              capability_response.body.find("\"mcp\":true") != std::string::npos &&
              capability_response.body.find("\"web_ui\":true") != std::string::npos &&
              capability_response.body.find("controller") == std::string::npos,
          "capabilities advertise MCP and the WUI without exposing a secret");

    http::Request bad_host = full;
    bad_host.headers["host"] = "example.com";
    check(route_request(bad_host, config, status).status == 421,
          "non-loopback Host is rejected");
    http::Request bad_origin = full;
    bad_origin.headers["origin"] = "https://example.com";
    check(route_request(bad_origin, config, status).status == 403,
          "cross-origin request is rejected");

    PublicStatus remote_status = status;
    remote_status.bind_address = "192.0.2.10";
    remote_status.remote = true;
    remote_status.tls = true;
    http::Request remote = full;
    remote.headers["host"] = "192.0.2.10:8766";
    remote.headers["origin"] = "https://192.0.2.10:8766";
    check(route_request(remote, config, remote_status).status == 200,
          "remote TLS requests accept only the configured host and same HTTPS origin");
    remote.headers["origin"] = "http://192.0.2.10:8766";
    check(route_request(remote, config, remote_status).status == 403,
          "remote TLS requests reject a downgraded Origin");
    remote.headers["origin"] = "https://192.0.2.10:8766";
    remote.headers["host"] = "192.0.2.10:9999";
    check(route_request(remote, config, remote_status).status == 421,
          "remote requests reject a Host with the wrong listener port");
}

void test_image_catalog_uploads_and_job_references() {
    const std::string png("\x89PNG\r\n\x1a\n", 8);
    ImageInputStore store(8U);
    StoredImageInput first;
    check(store.add("image/png", png, first).ok() && first.bytes &&
              first.id.rfind("input_", 0) == 0 && store.resident_bytes() == png.size(),
          "managed image store validates and retains an opaque PNG input");
    check(store.erase(first.id) && store.resident_bytes() == png.size(),
          "deleting a managed input leaves bytes alive while a job-style reference owns them");
    StoredImageInput over_capacity;
    check(store.add("image/png", png, over_capacity).code == ErrorCode::RateLimit,
          "managed image store accounts for live shared buffers against its byte budget");
    first = {};
    check(store.resident_bytes() == 0 && store.add("image/png", png, over_capacity).ok(),
          "managed image bytes are released when their final shared owner is gone");

    ImageInputStore expiring(32U, std::chrono::seconds(0));
    StoredImageInput expired;
    check(expiring.add("image/png", png, expired).ok(),
          "zero-lifetime managed input can be created for expiry coverage");
    std::vector<StoredImageInput> resolved;
    check(expiring.resolve({expired.id}, resolved).code == ErrorCode::FileRead,
          "expired managed image identifiers are rejected and removed");

    cli::Options options;
    options.provider = "openai";
    ImageCapability capability;
    capability.id = "test-image";
    capability.provider = "openai";
    capability.model_regex = "^test-image$";
    capability.api_model = "test-image";
    capability.default_for_provider = true;
    capability.edits = true;
    capability.max_input_images = 2;
    capability.size_mode = ImageSizeMode::Pixels;
    capability.size_classes = {{"1k", 1024}};
    capability.aspect_ratios = {"1:1"};
    capability.quality = {"low", "high"};
    capability.format = {"png", "jpeg"};
    capability.format_default = "png";
    capability.multiple = 16;
    capability.max_edge = 2048;
    options.image_catalog.models.push_back(capability);
    JobService jobs(std::move(options), ".", 8U);
    AuthConfig auth{"controller", "mcp-token"};
    std::atomic<std::size_t> active{0};
    PublicStatus status{8766, 64, 8, &active};
    status.jobs = &jobs;

    Response catalog = route_request(
        parsed_request(request_text("/ainiux/v1/images/catalog")), auth, status);
    check(catalog.status == 200 && catalog.body.find("\"model\":\"test-image\"") != std::string::npos &&
              catalog.body.find("\"max_image_bytes\":20971520") != std::string::npos &&
              catalog.body.find("model_regex") == std::string::npos &&
              catalog.body.find("protocol") == std::string::npos,
          "image catalog route exposes safe effective model options and upload limits only");

    http::Request upload = parsed_request(
        "POST /ainiux/v1/images/inputs HTTP/1.1\r\nHost: 127.0.0.1\r\n"
        "Authorization: Bearer controller\r\nContent-Type: image/png\r\nContent-Length: 0\r\n\r\n");
    upload.body = png;
    Response created = route_request(upload, auth, status);
    const json::ParseResult created_json = json::parse(created.body);
    const json::Value* id = created_json.error.ok() ? created_json.value.get("id") : nullptr;
    check(created.status == 201 && id != nullptr && id->is_string() &&
              created.body.find("\"mime_type\":\"image/png\"") != std::string::npos,
          "authenticated raw image upload returns an opaque managed identifier");
    check(id != nullptr && route_request(parsed_request(
              "DELETE /ainiux/v1/images/inputs/" + id->string +
              " HTTP/1.1\r\nHost: 127.0.0.1\r\nAuthorization: Bearer controller\r\n\r\n"),
              auth, status).status == 200,
          "managed image upload can be explicitly deleted");

    Response denial;
    http::Request unauthorized = upload;
    unauthorized.headers["authorization"] = "Bearer wrong";
    check(!preflight_request_body(unauthorized, Limits::upload_body_bytes, auth, status, denial) &&
              denial.status == 401,
          "large upload authorization is checked before its body is accepted");
    http::Request ordinary = upload;
    ordinary.path = "/ainiux/v1/jobs/chat";
    check(!preflight_request_body(ordinary, Limits::json_body_bytes + 1U, auth, status, denial) &&
              denial.status == 413,
          "ordinary JSON routes retain the 1 MiB request limit");
    jobs.shutdown();
}

void test_event_replay_is_ordered_and_bounded() {
    EventBroker broker("job_events", 3, 4096);
    for (int i = 0; i < 5; ++i) {
        broker.publish("progress", "{\"step\":" + std::to_string(i) + "}");
    }
    const ReplayBatch expired = broker.replay_after(1);
    check(expired.expired, "event replay explicitly expires an evicted cursor");
    const ReplayBatch retained = broker.replay_after(2);
    check(!retained.expired && retained.events.size() == 3 &&
              retained.events[0].id == 3 && retained.events[2].id == 5,
          "event replay remains ordered by monotonic ID inside the bounded window");
    broker.close();
    check(broker.wait_after(5, std::chrono::milliseconds(1)).closed,
          "closing an event broker wakes subscribers");
}

void test_job_registry_idempotency_lane_and_cancellation() {
    JobRegistry registry(8, 2);
    std::mutex gate_mutex;
    std::condition_variable gate_cv;
    bool release = false;
    JobWork blocked = [&](runtime::CancellationToken token, JobEvents) {
        std::unique_lock<std::mutex> lock(gate_mutex);
        gate_cv.wait_for(lock, std::chrono::seconds(1), [&] { return release || token.cancelled(); });
        return JobOutcome{token.cancelled() ? Error{ErrorCode::Cancelled, "cancelled"}
                                            : ok_error(),
                          "{\"ok\":true}"};
    };
    SubmitResult first = registry.submit("run", "{\"goal\":\"one\"}", "same-key",
                                         JobClass::Agent, blocked);
    SubmitResult replay = registry.submit("run", "{\"goal\":\"one\"}", "same-key",
                                          JobClass::Agent, blocked);
    SubmitResult changed = registry.submit("run", "{\"goal\":\"two\"}", "same-key",
                                           JobClass::Agent, blocked);
    SubmitResult lane = registry.submit("plan", "{\"goal\":\"plan\"}", "",
                                        JobClass::Agent, blocked);
    check(first.status == SubmitStatus::Created && replay.status == SubmitStatus::Existing &&
              replay.job == first.job,
          "same idempotency operation and payload return the original job");
    check(changed.status == SubmitStatus::IdempotencyConflict,
          "an idempotency key reused with different input is a typed conflict");
    check(lane.status == SubmitStatus::AgentConflict &&
              lane.conflicting_job_id == first.job->id,
          "run and plan share one non-queued workspace agent lane");
    std::shared_ptr<Job> cancelled;
    check(registry.cancel(first.job->id, cancelled) && registry.cancel(first.job->id, cancelled),
          "job cancellation is idempotent");
    gate_cv.notify_all();
    check(wait_terminal(first.job) && first.job->state() == wire::JobState::Cancelled,
          "cancellation reaches a terminal cancelled state");
    registry.shutdown();
}

void test_provider_job_concurrency_cap() {
    JobRegistry registry(12, 2);
    std::atomic<int> active{0};
    std::atomic<int> maximum{0};
    std::atomic<bool> release{false};
    std::vector<std::shared_ptr<Job>> jobs;
    for (int i = 0; i < 6; ++i) {
        JobWork work = [&](runtime::CancellationToken token, JobEvents) {
            const int now = active.fetch_add(1) + 1;
            int seen = maximum.load();
            while (now > seen && !maximum.compare_exchange_weak(seen, now)) {}
            while (!release.load() && !token.cancelled()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            active.fetch_sub(1);
            return JobOutcome{token.cancelled() ? Error{ErrorCode::Cancelled, "cancelled"}
                                                : ok_error(), "{}"};
        };
        SubmitResult submitted = registry.submit("chat", "{\"n\":" + std::to_string(i) + "}",
                                                 "", JobClass::Provider, work);
        jobs.push_back(submitted.job);
    }
    for (int i = 0; i < 100 && maximum.load() < 2; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    check(maximum.load() == 2, "provider chat/image work obeys the global concurrency cap");
    release.store(true);
    for (const auto& job : jobs) check(wait_terminal(job), "provider job reaches terminal state");
    registry.shutdown();
}

void test_generated_images_use_collision_safe_workspace_names() {
    namespace fs = std::filesystem;
    const fs::path workspace = fs::temp_directory_path() / "ainiux-server-image-output-test";
    std::error_code cleanup_error;
    fs::remove_all(workspace, cleanup_error);
    fs::create_directories(workspace, cleanup_error);
    check(!cleanup_error, "image output test creates an isolated workspace");
    std::ofstream(workspace / "image1.png", std::ios::binary) << "existing";

    std::string first_path;
    std::string second_path;
    Error first = persist_generated_image(workspace.u8string(), "png", "first-bytes", first_path);
    Error second = persist_generated_image(workspace.u8string(), "png", "second-bytes", second_path);
    std::string first_bytes;
    std::string second_bytes;
    if (first.ok()) {
        (void)platform::read_file_bounded((workspace / fs::u8path(first_path)).u8string(),
                                          1024U, first_bytes);
    }
    if (second.ok()) {
        (void)platform::read_file_bounded((workspace / fs::u8path(second_path)).u8string(),
                                          1024U, second_bytes);
    }
    check(first.ok() && second.ok() && first_path == "image2.png" &&
              second_path == "image3.png" && first_bytes == "first-bytes" &&
              second_bytes == "second-bytes",
          "generated images skip existing files and return only relative workspace names");
    fs::remove_all(workspace, cleanup_error);
}

void test_terminal_retention_eviction_releases_workers_safely() {
    JobRegistry registry(1, 1);
    for (int i = 0; i < 6; ++i) {
        SubmitResult submitted = registry.submit(
            "chat", "{\"iteration\":" + std::to_string(i) + "}", "",
            JobClass::Provider,
            [](runtime::CancellationToken, JobEvents) {
                return JobOutcome{ok_error(), "{\"done\":true}"};
            });
        check(submitted.status == SubmitStatus::Created && wait_terminal(submitted.job),
              "terminal retention evicts and joins prior workers safely");
    }
    check(registry.size() == 1, "terminal job retention stays within --max-jobs");
    registry.shutdown();
}

void test_job_routes_and_sse() {
    cli::Options options;
    options.provider = "none";
    JobService jobs(options, ".", 8);
    AuthConfig auth{"controller", {}};
    std::atomic<std::size_t> active{1};
    PublicStatus status{8766, 64, 8, &active, &jobs};
    const std::string body =
        "{\"provider\":\"none\",\"reasoning\":\"high\","
        "\"messages\":[{\"role\":\"user\",\"content\":\"hello\"}]}";
    const std::string prefix = "POST /ainiux/v1/jobs/chat HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                               "Authorization: Bearer controller\r\nContent-Type: application/json\r\n"
                               "Idempotency-Key: route-key\r\nContent-Length: ";
    http::Request request = parsed_request(prefix + std::to_string(body.size()) + "\r\n\r\n" + body);
    Response created = route_request(request, auth, status);
    Response existing = route_request(request, auth, status);
    check(created.status == 202 && existing.status == 200 &&
              existing.body.find("\"existing\":true") != std::string::npos,
          "chat submission accepts reasoning and idempotently reuses a job resource");

    ServiceSubmitResult invalid_reasoning = jobs.submit(
        "chat",
        "{\"provider\":\"none\",\"reasoning\":\"high effort\","
        "\"messages\":[{\"role\":\"user\",\"content\":\"hello\"}]}", "");
    check(invalid_reasoning.validation_error.code == ErrorCode::BadArgs &&
              invalid_reasoning.validation_error.message.find("field 'reasoning'") !=
                  std::string::npos,
          "chat reasoning rejects values outside the bounded CLI grammar");

    const json::ParseResult creation = json::parse(created.body);
    const json::Value* job_value = creation.value.get("job");
    const json::Value* id_value = job_value == nullptr ? nullptr : job_value->get("id");
    check(id_value != nullptr && id_value->is_string(), "job submission returns a stable job ID");
    const std::string job_id = id_value == nullptr ? std::string() : id_value->string;
    http::Request events = parsed_request(request_text("/ainiux/v1/jobs/" + job_id + "/events"));
    Response stream = route_request(events, auth, status);
    std::string records;
    check(stream.status == 200 && stream.streaming && stream.stream_body,
          "job event route upgrades to an SSE response");
    if (stream.stream_body) {
        stream.stream_body([&](std::string_view part) {
            records.append(part.data(), part.size());
            return true;
        });
    }
    check(records.find("id: 1\n") != std::string::npos &&
              records.find("event: queued\n") != std::string::npos,
          "SSE returns retained events with ordered IDs and stable envelopes");

    Response capabilities = route_request(
        parsed_request(request_text("/ainiux/v1/capabilities")), auth, status);
    check(capabilities.status == 200 && capabilities.body.find("\"models\"") != std::string::npos,
          "capability discovery advertises the asynchronous model-list operation");
    const std::string models_body = "{\"provider\":\"none\"}";
    const std::string models_prefix =
        "POST /ainiux/v1/jobs/models HTTP/1.1\r\nHost: 127.0.0.1\r\n"
        "Authorization: Bearer controller\r\nContent-Type: application/json\r\nContent-Length: ";
    Response models = route_request(parsed_request(
        models_prefix + std::to_string(models_body.size()) + "\r\n\r\n" + models_body),
        auth, status);
    const json::ParseResult models_json = json::parse(models.body);
    const json::Value* models_job = models_json.value.get("job");
    const json::Value* models_id = models_job == nullptr ? nullptr : models_job->get("id");
    std::shared_ptr<Job> retained_models =
        models_id != nullptr && models_id->is_string()
            ? jobs.registry().find(models_id->string) : nullptr;
    check(models.status == 202 && models.body.find("\"operation\":\"models\"") != std::string::npos &&
              retained_models != nullptr && wait_terminal(retained_models) &&
              retained_models->snapshot_json().find("disables model listing") != std::string::npos &&
              retained_models->snapshot_json().find("prompt is empty") == std::string::npos,
          "the authenticated model-list route creates a cancellable provider job");
    jobs.shutdown();
}

std::string mcp_meta(bool tasks = false) {
    return "\"_meta\":{\"io.modelcontextprotocol/protocolVersion\":\"2026-07-28\","
           "\"io.modelcontextprotocol/clientInfo\":{\"name\":\"test-client\",\"version\":\"1\"},"
           "\"io.modelcontextprotocol/clientCapabilities\":{" +
           std::string(tasks ? "\"extensions\":{\"io.modelcontextprotocol/tasks\":{}}" : "") +
           "}}";
}

http::Request mcp_request(const std::string& method,
                         const std::string& params,
                         const std::string& name = {},
                         const std::string& token = "mcp-token") {
    const std::string body = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":" +
                             json::quote(method) + ",\"params\":{" + params + "}}";
    std::string request = "POST /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\nAuthorization: Bearer " +
                          token + "\r\nContent-Type: application/json\r\nAccept: application/json, text/event-stream\r\n"
                          "MCP-Protocol-Version: 2026-07-28\r\nMcp-Method: " + method + "\r\n";
    if (!name.empty()) request += "Mcp-Name: " + name + "\r\n";
    request += "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
    return parsed_request(request);
}

void test_mcp_stateless_adapter_and_tasks() {
    cli::Options options;
    options.provider = "none";
    JobService jobs(options, ".", 8);
    McpAdapter adapter(&jobs, 8);
    AuthConfig auth{"controller", "mcp-token"};
    std::atomic<std::size_t> active{0};
    PublicStatus status{8766, 64, 8, &active, &jobs, &adapter};

    Response discover = route_request(mcp_request("server/discover", mcp_meta()), auth, status);
    check(discover.status == 200 && discover.body.find("\"supportedVersions\":[\"2026-07-28\"]") != std::string::npos &&
              discover.body.find("io.modelcontextprotocol/tasks") != std::string::npos,
          "MCP discovery returns the supported stateless version and task extension");

    Response list = route_request(mcp_request("tools/list", mcp_meta()), auth, status);
    check(list.status == 200 && list.body.find("ainiux_chat") != std::string::npos &&
              list.body.find("ainiux_job_cancel") != std::string::npos &&
              list.body.find("\"ttlMs\":300000") != std::string::npos,
          "MCP tools/list returns a deterministic cacheable tool catalog");

    http::Request mismatched = mcp_request("tools/list", mcp_meta());
    mismatched.headers["mcp-method"] = "tools/call";
    Response mismatch = route_request(mismatched, auth, status);
    check(mismatch.status == 400 && mismatch.body.find("-32020") != std::string::npos,
          "MCP rejects a standard routing header that disagrees with the JSON-RPC body");

    const std::string call_params = mcp_meta(true) +
        ",\"name\":\"ainiux_chat\",\"arguments\":{\"provider\":\"none\","
        "\"messages\":[{\"role\":\"user\",\"content\":\"hello\"}]}";
    Response call = route_request(mcp_request("tools/call", call_params, "ainiux_chat"), auth, status);
    const json::ParseResult call_json = json::parse(call.body);
    const json::Value* task_result = call_json.value.get("result");
    const json::Value* task_id = task_result == nullptr ? nullptr : task_result->get("taskId");
    check(call.status == 200 && task_id != nullptr && task_id->is_string() &&
              task_id->string.rfind("mcp_task_", 0) == 0,
          "task-enabled tools/call returns an opaque MCP task handle");
    if (task_id != nullptr && task_id->is_string()) {
        const std::string task = task_id->string;
        Response get = route_request(mcp_request("tasks/get", mcp_meta() +
                                                   ",\"taskId\":" + json::quote(task), task), auth, status);
        check(get.status == 200 && get.body.find("\"taskId\":" + json::quote(task)) != std::string::npos,
              "tasks/get resolves the task handle through the shared job registry");
        const std::string job_params = mcp_meta() + ",\"name\":\"ainiux_job_get\",\"arguments\":{" +
                                        "\"job_id\":" + json::quote(task) + "}";
        Response job_get = route_request(mcp_request("tools/call", job_params, "ainiux_job_get"), auth, status);
        check(job_get.status == 200 && job_get.body.find("\"structuredContent\"") != std::string::npos,
              "MCP job_get exposes a structured snapshot without raw server paths");
        Response cancel = route_request(mcp_request("tasks/cancel", mcp_meta() +
                                                       ",\"taskId\":" + json::quote(task), task), auth, status);
        check(cancel.status == 200 && cancel.body.find("\"resultType\":\"complete\"") != std::string::npos,
              "tasks/cancel acknowledges an MCP task through JobRegistry cancellation");
    }
    check(route_request(mcp_request("tools/list", mcp_meta(), {}, "controller"), auth, status).status == 401,
          "full-control credentials cannot be used at the MCP endpoint");
    jobs.shutdown();
}

http::Request session_request(const std::string& method,
                              const std::string& path,
                              const std::string& body = "{}") {
    std::string request = method + " " + path + " HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                          "Authorization: Bearer controller\r\n";
    if (method == "POST" || method == "PUT" || method == "PATCH")
        request += "Content-Type: application/json\r\n";
    request += "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
    return parsed_request(request);
}

void test_interactive_sessions_are_bounded_and_replayable() {
    namespace fs = std::filesystem;
    const fs::path workspace = fs::temp_directory_path() / "ainiux-session-hub-test";
    std::error_code cleanup_error;
    fs::remove_all(workspace, cleanup_error);
    fs::create_directories(workspace, cleanup_error);
    check(!cleanup_error, "session test creates an isolated workspace");

    cli::Options options;
    options.provider = "none";
    SessionHub hub(options, workspace.u8string(), 1);
    AuthConfig auth{"controller", {}};
    std::atomic<std::size_t> active{0};
    PublicStatus status{8766, 64, 8, &active, nullptr};
    status.sessions = &hub;
    status.max_sessions = 1;

    const std::string create_body =
        "{\"kind\":\"agent\",\"provider\":\"none\",\"reasoning\":\"high\","
        "\"permission_mode\":\"confirm\",\"task_mode\":\"act\"}";
    Response created = route_request(session_request("POST", "/ainiux/v1/sessions/agent", create_body),
                                     auth, status);
    const json::ParseResult created_json = json::parse(created.body);
    const json::Value* session_value = created_json.value.get("session");
    const json::Value* id_value = session_value == nullptr ? nullptr : session_value->get("id");
    check(created.status == 202 && id_value != nullptr && id_value->is_string(),
          "interactive agent session creation returns an opaque session ID");
    if (id_value == nullptr || !id_value->is_string()) {
        hub.shutdown();
        fs::remove_all(workspace, cleanup_error);
        return;
    }
    const std::string id = id_value->string;
    std::shared_ptr<InteractiveSession> session = hub.find(id);
    for (int i = 0; i < 200 && session->snapshot_json().find("\"status\":\"ready\"") == std::string::npos; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    check(session->snapshot_json().find("\"status\":\"ready\"") != std::string::npos,
          "session preparation completes asynchronously without blocking creation");
    check(session->snapshot_json().find("\"context\":{") != std::string::npos &&
              session->snapshot_json().find("\"reasoning\":\"high\"") != std::string::npos &&
              session->snapshot_json().find("\"reasoning_options\":[") != std::string::npos &&
              session->snapshot_json().find("\"active_elapsed_ms\":null") != std::string::npos &&
              session->snapshot_json().find("\"last_turn_metrics\":null") != std::string::npos,
          "interactive snapshots expose stable context and turn-metric fields before a turn");

    Response listed = route_request(session_request("GET", "/ainiux/v1/sessions", ""), auth, status);
    check(listed.status == 200 && listed.body.find(id) != std::string::npos,
          "session listing exposes state without workspace paths");
    const ReplayBatch replay = session->events().replay_after(0);
    check(!replay.events.empty() && replay.events.front().session_id == id &&
              replay.events.back().type == "ready",
          "session events retain ordered creation and readiness state");

    Response reasoning = route_request(session_request(
        "POST", "/ainiux/v1/sessions/" + id + "/reasoning",
        "{\"reasoning\":\"low\"}"), auth, status);
    check(reasoning.status == 200 &&
              reasoning.body.find("\"reasoning\":\"low\"") != std::string::npos &&
              session->events().replay_after(0).events.back().type == "reasoning_changed",
          "idle interactive sessions accept a validated reasoning-effort change");
    Response invalid_reasoning = route_request(session_request(
        "POST", "/ainiux/v1/sessions/" + id + "/reasoning",
        "{\"reasoning\":\"\"}"), auth, status);
    check(invalid_reasoning.status == 400,
          "interactive session reasoning rejects an empty selection");
    Response task_mode = route_request(session_request(
        "POST", "/ainiux/v1/sessions/" + id + "/settings",
        "{\"task_mode\":\"plan\"}"), auth, status);
    check(task_mode.status == 200 &&
              task_mode.body.find("\"task_mode\":\"plan\"") != std::string::npos &&
              session->events().replay_after(0).events.back().type == "settings_changed",
          "idle workspace agents accept a single inline setting change");
    Response model_setting = route_request(session_request(
        "POST", "/ainiux/v1/sessions/" + id + "/settings",
        "{\"model\":\"local-model\"}"), auth, status);
    check(model_setting.status == 200 &&
              model_setting.body.find("\"model\":\"local-model\"") != std::string::npos,
          "agent model changes update the active workspace snapshot");
    Response mixed_settings = route_request(session_request(
        "POST", "/ainiux/v1/sessions/" + id + "/settings",
        "{\"model\":\"one\",\"task_mode\":\"act\"}"), auth, status);
    check(mixed_settings.status == 400,
          "agent setting requests reject ambiguous multi-setting updates");

    Response turn = route_request(session_request(
        "POST", "/ainiux/v1/sessions/" + id + "/turns", "{\"text\":\"hello\"}"), auth, status);
    const json::ParseResult turn_json = json::parse(turn.body);
    const json::Value* turn_id_value = turn_json.value.get("turn_id");
    check(turn.status == 202 && turn_id_value != nullptr && turn_id_value->is_string(),
          "ready interactive sessions accept asynchronous turns");
    if (turn_id_value != nullptr && turn_id_value->is_string()) {
        const std::string turn_id = turn_id_value->string;
        Response conflict = route_request(session_request(
            "POST", "/ainiux/v1/sessions/" + id + "/turns", "{\"text\":\"second\"}"), auth, status);
        check(conflict.status == 409, "a session rejects a concurrent turn instead of racing controllers");
        Response busy_setting = route_request(session_request(
            "POST", "/ainiux/v1/sessions/" + id + "/settings",
            "{\"task_mode\":\"act\"}"), auth, status);
        check(busy_setting.status == 409,
              "agent settings cannot change during an active turn");
        Response cancelled = route_request(session_request(
            "POST", "/ainiux/v1/sessions/" + id + "/turns/" + turn_id + "/cancel", ""), auth, status);
        check(cancelled.status == 200, "interactive turn cancellation is explicit and idempotent at the session boundary");
    }
    for (int i = 0; i < 200 && session->snapshot_json().find("\"turn_id\":null") == std::string::npos; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    check(session->snapshot_json().find("\"last_turn_metrics\":{") != std::string::npos &&
              session->snapshot_json().find("\"elapsed_ms\":") != std::string::npos,
          "completed interactive turns retain normalized token and elapsed metrics");

    Response over_capacity = route_request(session_request("POST", "/ainiux/v1/sessions", create_body),
                                            auth, status);
    check(over_capacity.status == 429, "interactive sessions obey the configured bounded capacity");
    Response deleted = route_request(session_request("DELETE", "/ainiux/v1/sessions/" + id, ""), auth, status);
    check(deleted.status == 200 && hub.size() == 0, "deleting a session cancels work and releases its retained state");
    hub.shutdown();
    fs::remove_all(workspace, cleanup_error);
}

void test_read_only_workspace_routes_are_contained_and_bounded() {
    namespace fs = std::filesystem;
    const fs::path workspace = fs::temp_directory_path() / "ainiux-workspace-service-test";
    const fs::path outside = fs::temp_directory_path() / "ainiux-workspace-service-outside";
    std::error_code cleanup_error;
    fs::remove_all(workspace, cleanup_error);
    fs::remove_all(outside, cleanup_error);
    fs::create_directories(workspace / "nested", cleanup_error);
    fs::create_directories(workspace / ".ainiux-pr", cleanup_error);
    fs::create_directories(outside, cleanup_error);
    check(!cleanup_error, "workspace service creates isolated test directories");
    {
        std::ofstream(workspace / "visible.txt", std::ios::binary) << "visible text\n";
        std::ofstream(workspace / fs::u8path("space λ.txt"), std::ios::binary) << "encoded path\n";
        std::ofstream(workspace / "nested" / "inside.md", std::ios::binary) << "nested text\n";
        std::ofstream(workspace / "run.sh", std::ios::binary) << "#!/bin/sh\nexit 0\n";
        std::ofstream(workspace / ".ainiux-pr" / "private.json", std::ios::binary) << "private";
        std::ofstream(workspace / "config.conf", std::ios::binary) << "provider_secret = hidden\n";
        std::ofstream(workspace / "secret.txt", std::ios::binary) << "do not expose\n";
        std::ofstream(outside / "outside.txt", std::ios::binary) << "outside\n";
    }
#if !defined(_WIN32)
    fs::permissions(workspace / "run.sh",
                    fs::perms::owner_read | fs::perms::owner_write | fs::perms::owner_exec,
                    fs::perm_options::replace, cleanup_error);
#endif
#if !defined(_WIN32)
    fs::create_symlink(outside / "outside.txt", workspace / "escape", cleanup_error);
#endif
    std::ofstream large(workspace / "large.txt", std::ios::binary);
    large << std::string(1024U * 1024U + 1U, 'x');
    large.close();

    WorkspaceService service(workspace.u8string());
    std::string body;
    check(service.list("", body).ok() && body.find("visible.txt") != std::string::npos &&
              body.find("nested") != std::string::npos &&
              body.find("run.sh") != std::string::npos &&
#if !defined(_WIN32)
              body.find("\"executable\":true") != std::string::npos &&
#endif
              body.find("private.json") == std::string::npos &&
              body.find("config.conf") == std::string::npos &&
              body.find("secret.txt") == std::string::npos &&
              body.find(workspace.u8string()) == std::string::npos,
          "dired route lists safe relative entries without private state or absolute paths");
    check(service.read("visible.txt", body).ok() && body.find("visible text") != std::string::npos &&
              body.find(workspace.u8string()) == std::string::npos,
          "file route returns bounded content using a workspace-relative path");
    check(!service.read("../outside.txt", body).ok() &&
              !service.read("nested/../visible.txt", body).ok() &&
              !service.read("large.txt", body).ok(),
          "file route rejects traversal and oversized reads");
#if !defined(_WIN32)
    check(!service.read("escape", body).ok(),
          "file route rejects symlink targets even when they point outside the workspace");
#endif

    AuthConfig auth{"controller", {}};
    std::atomic<std::size_t> active{0};
    PublicStatus status{8766, 64, 8, &active};
    status.workspace = &service;
    Response listed = route_request(
        parsed_request(request_text("/ainiux/v1/dired?path=nested")), auth, status);
    check(listed.status == 200 && listed.body.find("inside.md") != std::string::npos,
          "authenticated dired endpoint accepts one relative path query");
    Response file = route_request(
        parsed_request(request_text("/ainiux/v1/files?path=visible.txt")), auth, status);
    check(file.status == 200 && file.body.find("visible text") != std::string::npos,
          "authenticated files endpoint returns a selected workspace file");
    Response encoded_file = route_request(
        parsed_request(request_text("/ainiux/v1/files?path=space%20%CE%BB.txt")), auth, status);
    check(encoded_file.status == 200 && encoded_file.body.find("encoded path") != std::string::npos &&
              encoded_file.body.find("space λ.txt") != std::string::npos,
          "workspace file query strictly decodes browser-compatible UTF-8 and space escapes");
    Response malformed_escape = route_request(
        parsed_request(request_text("/ainiux/v1/files?path=bad%2")), auth, status);
    check(malformed_escape.status == 400,
          "workspace file query rejects incomplete percent escapes");
    Response invalid_utf8 = route_request(
        parsed_request(request_text("/ainiux/v1/files?path=bad%FFname")), auth, status);
    check(invalid_utf8.status == 400,
          "workspace file query rejects percent escapes that are not valid UTF-8");
    Response bad_query = route_request(
        parsed_request(request_text("/ainiux/v1/files?path=visible.txt&other=x")), auth, status);
    check(bad_query.status == 400, "workspace routes reject unrecognized query parameters");
    Response review = route_request(
        parsed_request(request_text("/ainiux/v1/workspace/review")), auth, status);
    check(review.status == 200 && review.body.find("nested/inside.md") != std::string::npos &&
              review.body.find("private.json") == std::string::npos &&
              review.body.find(workspace.u8string()) == std::string::npos,
          "workspace review is recursive, bounded, and excludes private paths");

    fs::remove_all(workspace, cleanup_error);
    fs::remove_all(outside, cleanup_error);
}

void test_revision_safe_workspace_mutations_and_editor_assist() {
    namespace fs = std::filesystem;
    const fs::path workspace = fs::temp_directory_path() / "ainiux-workspace-mutation-test";
    const fs::path outside = fs::temp_directory_path() / "ainiux-workspace-mutation-outside";
    std::error_code cleanup_error;
    fs::remove_all(workspace, cleanup_error);
    fs::remove_all(outside, cleanup_error);
    fs::create_directories(workspace / "nested", cleanup_error);
    fs::create_directories(workspace / "readonly", cleanup_error);
    fs::create_directories(outside, cleanup_error);
    {
        std::ofstream(workspace / "note.txt", std::ios::binary) << "one\n";
        std::ofstream(workspace / "race.txt", std::ios::binary) << "before\n";
        std::ofstream(workspace / "readonly" / "kept.txt", std::ios::binary) << "keep\n";
        std::ofstream(outside / "outside.txt", std::ios::binary) << "outside\n";
    }
    check(!cleanup_error, "workspace mutation test creates isolated directories");

    auto string_member = [](const std::string& encoded, const std::string& name) {
        const json::ParseResult parsed = json::parse(encoded);
        const json::Value* value = parsed.error.ok() ? parsed.value.get(name) : nullptr;
        return value != nullptr && value->is_string() ? value->string : std::string();
    };
    auto file_revision = [&](WorkspaceService& service, const std::string& path) {
        std::string encoded;
        return service.read(path, encoded).ok() ? string_member(encoded, "revision")
                                                : std::string();
    };

    WorkspaceService service(workspace.u8string());
    std::string listing;
    check(service.list("", listing).ok(), "revision-safe dired listing succeeds");
    const std::string root_revision = string_member(listing, "revision");
    const std::string first_revision = file_revision(service, "note.txt");
    check(!root_revision.empty() && !first_revision.empty() &&
              listing.find("\"revision\"") != std::string::npos,
          "workspace listings and file reads expose opaque revisions");

    std::string response;
    std::string current;
    Error saved = service.save("note.txt",
                               "{\"revision\":" + json::quote(first_revision) +
                                   ",\"content\":\"two\\n\"}",
                               response, current);
    check(saved.ok() && current != first_revision &&
              file_revision(service, "note.txt") == current,
          "file save atomically replaces reviewed content and advances its revision");

    {
        std::ofstream(workspace / "note.txt", std::ios::binary | std::ios::trunc) << "external\n";
    }
    const Error stale = service.save("note.txt",
                                     "{\"revision\":" + json::quote(current) +
                                         ",\"content\":\"lost\\n\"}",
                                     response, current);
    std::string disk_text;
    platform::read_file_bounded((workspace / "note.txt").u8string(), 1024U, disk_text);
    check(stale.code == ErrorCode::FileLock && !current.empty() && disk_text == "external\n",
          "stale file saves return the current revision without overwriting external edits");

    service.list("", listing);
    const std::string create_parent_revision = string_member(listing, "revision");
    const Error created = service.create_file(
        "{\"path\":\"created.txt\",\"parent_revision\":" +
            json::quote(create_parent_revision) + ",\"content\":\"created\\n\"}",
        response, current);
    check(created.ok() && fs::exists(workspace / "created.txt"),
          "new files require a reviewed parent revision and use the atomic save primitive");
    const Error create_race = platform::atomic_write_shared_create(
        (workspace / "created.txt").u8string(), "overwrite\n", true);
    disk_text.clear();
    platform::read_file_bounded((workspace / "created.txt").u8string(), 1024U, disk_text);
    check(create_race.code == ErrorCode::FileWrite && disk_text == "created\n",
          "exclusive atomic publication preserves a destination that appears during create/copy");

    service.list("", listing);
    const std::string mutation_parent_revision = string_member(listing, "revision");
    const std::string created_revision = file_revision(service, "created.txt");
    const std::string mutation_body =
        "{\"operations\":["
        "{\"operation\":\"mkdir\",\"path\":\"made\",\"parent_revision\":" +
        json::quote(mutation_parent_revision) + "},"
        "{\"operation\":\"delete\",\"path\":\"created.txt\",\"revision\":" +
        json::quote(created_revision) +
        ",\"confirmation\":\"delete wrong.txt\"},"
        "{\"operation\":\"mkdir\",\"path\":\"../escape\",\"parent_revision\":" +
        json::quote(mutation_parent_revision) + "}]}";
    check(service.mutate(mutation_body, response).ok() &&
              response.find("\"ok\":true") != std::string::npos &&
              response.find("revision_conflict") == std::string::npos &&
              response.find("invalid_target") != std::string::npos &&
              fs::is_directory(workspace / "made") && fs::exists(workspace / "created.txt") &&
              !fs::exists(outside / "escape"),
          "bounded mutation batches report per-target results and reject bad confirmation and traversal");

    std::string nested_listing;
    service.list("nested", nested_listing);
    const std::string nested_revision = string_member(nested_listing, "revision");
    const std::string move_revision = file_revision(service, "created.txt");
    check(service.mutate(
              "{\"operations\":[{\"operation\":\"move\",\"path\":\"created.txt\","
              "\"revision\":" + json::quote(move_revision) +
              ",\"destination\":\"nested/moved.txt\",\"destination_parent_revision\":" +
              json::quote(nested_revision) + "}]}", response).ok() &&
              response.find("\"ok\":true") != std::string::npos &&
              fs::exists(workspace / "nested" / "moved.txt") &&
              !fs::exists(workspace / "created.txt"),
          "move uses exact reviewed source and destination-parent identities across directories");

    service.list("", listing);
    const std::string copy_parent_revision = string_member(listing, "revision");
    const std::string move_target_revision = file_revision(service, "nested/moved.txt");
    check(service.mutate(
              "{\"operations\":[{\"operation\":\"copy\",\"path\":\"nested/moved.txt\","
              "\"revision\":" + json::quote(move_target_revision) +
              ",\"destination\":\"copied.txt\",\"destination_parent_revision\":" +
              json::quote(copy_parent_revision) + "}]}", response).ok() &&
              response.find("\"ok\":true") != std::string::npos &&
              fs::exists(workspace / "copied.txt"),
          "copy is bounded and returns a fresh destination revision");

    const std::string race_revision = file_revision(service, "race.txt");
    check(platform::atomic_write_shared((workspace / "race.txt").u8string(), "before\n", true).ok(),
          "race test replaces the target with a new identity while retaining content");
    const Error replaced = service.save(
        "race.txt", "{\"revision\":" + json::quote(race_revision) +
                        ",\"content\":\"after\\n\"}", response, current);
    check(replaced.code == ErrorCode::FileLock,
          "identity-bearing revisions reject a same-content replacement race");

#if !defined(_WIN32)
    const std::string readonly_revision = file_revision(service, "readonly/kept.txt");
    fs::permissions(workspace / "readonly", fs::perms::owner_read | fs::perms::owner_exec,
                    fs::perm_options::replace, cleanup_error);
    const Error failed_save = service.save(
        "readonly/kept.txt", "{\"revision\":" + json::quote(readonly_revision) +
                                ",\"content\":\"replace\\n\"}", response, current);
    fs::permissions(workspace / "readonly", fs::perms::owner_all,
                    fs::perm_options::replace, cleanup_error);
    disk_text.clear();
    platform::read_file_bounded((workspace / "readonly" / "kept.txt").u8string(), 1024U, disk_text);
    check(failed_save.code == ErrorCode::FileWrite && disk_text == "keep\n",
          "atomic save failure preserves the previously reviewed file");

    const std::string link_revision = file_revision(service, "note.txt");
    fs::remove(workspace / "note.txt", cleanup_error);
    fs::create_symlink(outside / "outside.txt", workspace / "note.txt", cleanup_error);
    const Error link_swap = service.save(
        "note.txt", "{\"revision\":" + json::quote(link_revision) +
                       ",\"content\":\"escaped\\n\"}", response, current);
    disk_text.clear();
    platform::read_file_bounded((outside / "outside.txt").u8string(), 1024U, disk_text);
    check(!link_swap.ok() && disk_text == "outside\n",
          "a symlink swap cannot redirect a revision-checked save outside the workspace");
#endif

    AuthConfig auth{"controller", {}};
    std::atomic<std::size_t> active{0};
    PublicStatus status{8766, 64, 8, &active};
    status.workspace = &service;
    const std::string live_revision = file_revision(service, "copied.txt");
    Response stale_route = route_request(session_request(
        "PUT", "/ainiux/v1/files?path=copied.txt",
        "{\"revision\":\"stale\",\"content\":\"no\\n\"}"), auth, status);
    check(stale_route.status == 409 && stale_route.body.find("current_revision") != std::string::npos,
          "file-save route returns HTTP 409 with conflict UI revision data");
    Response mutation_route = route_request(session_request(
        "POST", "/ainiux/v1/dired/mutations",
        "{\"operations\":[{\"operation\":\"delete\",\"path\":\"copied.txt\","
        "\"revision\":" + json::quote(live_revision) +
        ",\"confirmation\":\"delete copied.txt\"}]}"), auth, status);
    check(mutation_route.status == 200 && mutation_route.body.find("\"ok\":true") != std::string::npos &&
              !fs::exists(workspace / "copied.txt"),
          "authenticated mutation route applies an explicitly confirmed reviewed target");

    cli::Options options;
    options.provider = "none";
    JobService jobs(options, workspace.u8string(), 2);
    status.jobs = &jobs;
    ServiceSubmitResult assist = jobs.submit(
        "editor-assist",
        "{\"provider\":\"none\",\"path\":\"race.txt\",\"revision\":" +
            json::quote(race_revision) +
            ",\"instruction\":\"Improve this text\"}", "assist-stale");
    check(assist.validation_error.ok() && assist.submission.job != nullptr &&
              wait_terminal(assist.submission.job) &&
              assist.submission.job->snapshot_json().find("\"code\":\"conflict\"") != std::string::npos,
          "editor-assist jobs verify the reviewed file revision before contacting a provider");
    jobs.shutdown();

    fs::remove_all(workspace, cleanup_error);
    fs::remove_all(outside, cleanup_error);
}

void test_revision_safe_chat_thread_routes() {
    namespace fs = std::filesystem;
    const fs::path directory = fs::temp_directory_path() / "ainiux-chat-service-test";
    const fs::path database = directory / "ainiux.db";
    std::error_code cleanup_error;
    fs::remove_all(directory, cleanup_error);
    fs::create_directories(directory, cleanup_error);
    check(!cleanup_error, "chat service creates an isolated database directory");

    ChatService service(database.u8string());
    AuthConfig auth{"controller", {}};
    std::atomic<std::size_t> active{0};
    PublicStatus status{8766, 64, 8, &active};
    status.chat_threads = &service;

    Response created = route_request(session_request(
        "POST", "/ainiux/v1/chat/threads",
        "{\"revision\":0,\"provider\":\"openai\",\"model\":\"model-a\"}"),
        auth, status);
    const json::ParseResult created_json = json::parse(created.body);
    const json::Value* created_thread = created_json.value.get("thread");
    const json::Value* id_value = created_thread == nullptr ? nullptr : created_thread->get("id");
    const json::Value* revision_value =
        created_thread == nullptr ? nullptr : created_thread->get("revision");
    check(created.status == 201 && id_value != nullptr &&
              id_value->type == json::Value::Type::Number && revision_value != nullptr &&
              revision_value->number == 1 && created.body.find("\"name\":\"New chat\"") != std::string::npos &&
              created.body.find("\"created_at\":\"20") != std::string::npos &&
              created.body.find("\"modified_at\":\"20") != std::string::npos,
          "unnamed chat creation returns a default title and human-readable timestamps");
    if (id_value == nullptr || id_value->type != json::Value::Type::Number) {
        fs::remove_all(directory, cleanup_error);
        return;
    }
    const long long thread_id = static_cast<long long>(id_value->number);
    const std::string thread_path = "/ainiux/v1/chat/threads/" + std::to_string(thread_id);

    Response listed = route_request(session_request("GET", "/ainiux/v1/chat/threads", ""),
                                    auth, status);
    check(listed.status == 200 && listed.body.find("New chat") != std::string::npos &&
              listed.body.find(database.u8string()) == std::string::npos,
          "chat thread listing exposes bounded summaries without the database path");

    Response appended = route_request(session_request(
        "POST", thread_path + "/messages",
        "{\"revision\":1,\"provider\":\"deepseek\",\"model\":\"model-b\","
        "\"messages\":[{\"role\":\"user\",\"content\":\"hello\"},"
        "{\"role\":\"assistant\",\"content\":\"hi\"}]}"), auth, status);
    check(appended.status == 200 && appended.body.find("\"revision\":2") != std::string::npos &&
              appended.body.find("\"message_count\":2") != std::string::npos &&
              appended.body.find("\"provider\":\"deepseek\"") != std::string::npos &&
              appended.body.find("\"model\":\"model-b\"") != std::string::npos,
          "message append advances the thread and its selected provider/model atomically");
    listed = route_request(session_request("GET", "/ainiux/v1/chat/threads", ""),
                           auth, status);
    check(listed.status == 200 && listed.body.find("\"name\":\"hello\"") != std::string::npos,
          "the first user prompt becomes the persisted title of an unnamed thread");

    Response stale = route_request(session_request(
        "POST", thread_path + "/messages",
        "{\"revision\":1,\"messages\":[{\"role\":\"user\",\"content\":\"stale write\"}]}"),
        auth, status);
    check(stale.status == 409 && stale.body.find("revision_conflict") != std::string::npos &&
              stale.body.find("\"current_revision\":2") != std::string::npos,
          "stale chat clients receive a conflict with the current revision");

    Response loaded = route_request(session_request("GET", thread_path, ""), auth, status);
    check(loaded.status == 200 && loaded.body.find("hello") != std::string::npos &&
              loaded.body.find("stale write") == std::string::npos &&
              loaded.body.find("\"revision\":2") != std::string::npos &&
              loaded.body.find("\"provider\":\"deepseek\"") != std::string::npos &&
              loaded.body.find("\"model\":\"model-b\"") != std::string::npos,
          "thread loading returns its committed transcript and provider/model selection");

    {
        chat::SqliteStore tui_store;
        Error error = tui_store.open(database.u8string());
        chat::Session session;
        if (error.ok()) error = tui_store.load_session(thread_id, session);
        if (error.ok()) {
            std::vector<provider::TextAttachment> attachments;
            for (int index = 0; index < 65; ++index) {
                provider::TextAttachment attachment;
                attachment.markdown_content =
                    "private attachment body " + std::to_string(index);
                attachment.display_name = "notes-" + std::to_string(index) + ".md";
                attachment.source_ref =
                    "/private/source/notes-" + std::to_string(index) + ".md";
                attachment.byte_size =
                    static_cast<long long>(attachment.markdown_content.size());
                attachments.push_back(std::move(attachment));
            }
            session.messages.push_back({"user", "from TUI", {}, std::move(attachments)});
            error = tui_store.save_session(session);
        }
        check(error.ok() && session.revision == 3,
              "ordinary TUI persistence advances the same optimistic revision");
    }
    loaded = route_request(session_request("GET", thread_path, ""), auth, status);
    check(loaded.status == 200 && loaded.body.find("\"revision\":3") != std::string::npos &&
              loaded.body.find("notes-0.md") != std::string::npos &&
              loaded.body.find("notes-64.md") == std::string::npos &&
              loaded.body.find("\"attachments_truncated\":true") != std::string::npos &&
              loaded.body.find("/private/source") == std::string::npos &&
              loaded.body.find("private attachment body") == std::string::npos,
          "remote loads bound attachment metadata without reading source paths or payloads");
    Response stale_after_tui = route_request(session_request(
        "POST", thread_path + "/messages",
        "{\"revision\":2,\"messages\":[{\"role\":\"user\",\"content\":\"raced\"}]}"),
        auth, status);
    check(stale_after_tui.status == 409 &&
              stale_after_tui.body.find("\"current_revision\":3") != std::string::npos,
          "API revisions detect saves made by another chat-store connection");

    chat::Session stale_tui_snapshot;
    {
        chat::SqliteStore store;
        Error error = store.open(database.u8string());
        if (error.ok()) error = store.load_session(thread_id, stale_tui_snapshot);
        check(error.ok() && stale_tui_snapshot.revision == 3,
              "stale full-save fixture observes the current thread revision");
    }
    Response remote_wins = route_request(session_request(
        "POST", thread_path + "/messages",
        "{\"revision\":3,\"messages\":[{\"role\":\"assistant\",\"content\":\"remote wins\"}]}"),
        auth, status);
    Error stale_save_error;
    {
        chat::SqliteStore store;
        stale_save_error = store.open(database.u8string());
        stale_tui_snapshot.messages.push_back({"user", "stale TUI overwrite"});
        if (stale_save_error.ok()) stale_save_error = store.save_session(stale_tui_snapshot);
    }
    loaded = route_request(session_request("GET", thread_path, ""), auth, status);
    check(remote_wins.status == 200 && stale_save_error.code == ErrorCode::FileLock &&
              loaded.body.find("remote wins") != std::string::npos &&
              loaded.body.find("stale TUI overwrite") == std::string::npos &&
              loaded.body.find("\"revision\":4") != std::string::npos,
          "a stale full TUI save cannot overwrite a newer API append");

    Response invalid_attachment = route_request(session_request(
        "POST", thread_path + "/messages",
        "{\"revision\":3,\"messages\":[{\"role\":\"user\",\"content\":\"x\","
        "\"attachments\":[{\"path\":\"/private/file\"}]}]}"), auth, status);
    check(invalid_attachment.status == 400,
          "remote message input rejects unrestricted attachment fields");

    Response regenerated = route_request(session_request(
        "POST", thread_path + "/regenerate", "{\"revision\":4}"), auth, status);
    loaded = route_request(session_request("GET", thread_path, ""), auth, status);
    check(regenerated.status == 200 &&
              regenerated.body.find("\"revision\":5") != std::string::npos &&
              regenerated.body.find("\"prompt\":\"from TUI\"") != std::string::npos &&
              loaded.body.find("from TUI") != std::string::npos &&
              loaded.body.find("remote wins") == std::string::npos,
          "regeneration revision-safely removes only the answer after the latest user prompt");
    Response stale_regenerate = route_request(session_request(
        "POST", thread_path + "/regenerate", "{\"revision\":4}"), auth, status);
    check(stale_regenerate.status == 409 &&
              stale_regenerate.body.find("\"current_revision\":5") != std::string::npos,
          "stale regeneration cannot rewind a thread changed by another client");

    long long large_thread_id = 0;
    {
        chat::SqliteStore store;
        Error error = store.open(database.u8string());
        chat::Session large_session;
        large_session.name = "Large remote thread";
        for (int index = 0; index < 520; ++index) {
            large_session.messages.push_back(
                {"user", "bounded-message-" + std::to_string(index)});
        }
        if (error.ok()) error = store.save_session(large_session);
        large_thread_id = large_session.thread_id;
        check(error.ok(), "large chat fixture persists through the ordinary store");
    }
    Response bounded = route_request(session_request(
        "GET", "/ainiux/v1/chat/threads/" + std::to_string(large_thread_id), ""), auth, status);
    check(bounded.status == 200 &&
              bounded.body.find("\"message_count\":520") != std::string::npos &&
              bounded.body.find("\"messages_truncated\":true") != std::string::npos &&
              bounded.body.find("bounded-message-519") != std::string::npos &&
              bounded.body.find("bounded-message-0\"") == std::string::npos,
          "remote thread loads retain the newest 512 messages and report truncation");

    const fs::path blocked_parent = directory / "not-a-directory";
    std::ofstream(blocked_parent, std::ios::binary) << "file";
    ChatService failed((blocked_parent / "ainiux.db").u8string());
    status.chat_threads = &failed;
    Response failure = route_request(session_request("GET", "/ainiux/v1/chat/threads", ""),
                                     auth, status);
    check(failure.status == 500 &&
              failure.body.find(blocked_parent.u8string()) == std::string::npos,
          "chat database failures do not expose server-side paths");

    fs::remove_all(directory, cleanup_error);
}

void test_job_errors_hide_server_side_paths() {
    cli::Options options;
    options.provider = "none";
    options.key_file = "/private/provider/credential.txt";
    JobService jobs(options, ".", 2);
    const std::string body =
        "{\"provider\":\"none\",\"messages\":[{\"role\":\"user\",\"content\":\"hello\"}]}";
    ServiceSubmitResult submitted = jobs.submit("chat", body, "");
    check(submitted.validation_error.ok() && submitted.submission.job &&
              wait_terminal(submitted.submission.job),
          "server-side provider configuration failure completes the job");
    const std::string snapshot = submitted.submission.job->snapshot_json();
    check(snapshot.find("/private/provider") == std::string::npos &&
              snapshot.find("server-side file") != std::string::npos,
          "job errors do not expose configured local credential paths");
    jobs.shutdown();
}

void test_server_cli_contract() {
    const char* argv[] = {"ainiux", "server", "--workspace", ".", "--bind", "127.0.0.1",
                          "--port", "9001", "--max-connections", "7", "--max-jobs", "9",
                          "--max-sessions", "5"};
    cli::ParseResult parsed = cli::parse_args(
        static_cast<int>(sizeof(argv) / sizeof(argv[0])), const_cast<char**>(argv));
    check(parsed.error.ok() && parsed.options.server && parsed.options.port == 9001 &&
              parsed.options.bind_address == "127.0.0.1" &&
              parsed.options.max_connections == 7 && parsed.options.max_jobs == 9 &&
              parsed.options.max_sessions == 5,
          "server subcommand and bounded options parse");
    check(validate_server_options(parsed.options).ok(), "standalone server options validate");
    const char* webserver[] = {"ainiux", "webserver"};
    parsed = cli::parse_args(2, const_cast<char**>(webserver));
    check(parsed.error.ok() && parsed.options.server && parsed.options.webui &&
              !parsed.options.server_bind_explicit &&
              validate_server_options(parsed.options).ok(),
          "webserver alias selects browser mode with its safe parser defaults");
    const char* server_webui[] = {"ainiux", "server", "--webui"};
    parsed = cli::parse_args(3, const_cast<char**>(server_webui));
    check(parsed.error.ok() && parsed.options.server && parsed.options.webui &&
              validate_server_options(parsed.options).ok(),
          "server --webui selects the browser-oriented server mode");
    const char* stray_webui[] = {"ainiux", "--webui"};
    parsed = cli::parse_args(2, const_cast<char**>(stray_webui));
    check(parsed.error.ok() && !validate_server_options(parsed.options).ok(),
          "standalone --webui is rejected without server mode");
    const char* loopback_webui[] = {"ainiux", "webserver", "--bind", "127.0.0.1"};
    parsed = cli::parse_args(4, const_cast<char**>(loopback_webui));
    check(parsed.error.ok() && parsed.options.server_bind_explicit &&
              parsed.options.bind_address == "127.0.0.1" &&
              validate_server_options(parsed.options).ok(),
          "an explicit webserver bind overrides the wildcard browser default");
    const char* combined[] = {"ainiux", "server", "-p", "hello"};
    parsed = cli::parse_args(4, const_cast<char**>(combined));
    check(parsed.error.ok() && !validate_server_options(parsed.options).ok(),
          "server mode rejects prompt combinations");
    const char* misplaced[] = {"ainiux", "-p", "hello", "--port", "9001"};
    parsed = cli::parse_args(5, const_cast<char**>(misplaced));
    check(parsed.error.ok() && !validate_server_options(parsed.options).ok(),
          "server-only options are rejected outside server mode");
    const char* stdin_key[] = {"ainiux", "server", "--key-stdin"};
    parsed = cli::parse_args(3, const_cast<char**>(stdin_key));
    check(parsed.error.ok() && !validate_server_options(parsed.options).ok(),
          "server mode rejects provider credentials read from process stdin");

    const char* unsafe_remote[] = {"ainiux", "server", "--bind", "0.0.0.0"};
    parsed = cli::parse_args(4, const_cast<char**>(unsafe_remote));
    check(parsed.error.ok() && !validate_server_options(parsed.options).ok(),
          "non-loopback server binding fails closed without TLS or explicit plaintext opt-in");
    const char* explicit_plain[] = {"ainiux", "server", "--bind", "0.0.0.0",
                                    "--insecure-plain-bind"};
    parsed = cli::parse_args(5, const_cast<char**>(explicit_plain));
    check(parsed.error.ok() && parsed.options.insecure_plain_bind &&
              validate_server_options(parsed.options).ok(),
          "non-loopback plaintext binding requires an explicit unsafe opt-in");
    const char* incomplete_tls[] = {"ainiux", "server", "--tls-cert", "cert.pem"};
    parsed = cli::parse_args(4, const_cast<char**>(incomplete_tls));
    check(parsed.error.ok() && !validate_server_options(parsed.options).ok(),
          "server TLS requires a certificate and private key pair");

    cli::Options base;
    base.provider = "none";
    SessionHub remote_sessions(base, ".", 1, false);
    const SessionCreateResult denied_yolo = remote_sessions.create(
        "{\"kind\":\"agent\",\"provider\":\"none\",\"permission_mode\":\"yolo\"}");
    check(!denied_yolo.error.ok() && denied_yolo.error.code == ErrorCode::UnsupportedFeature,
          "remote session policy denies an explicit Yolo request without startup opt-in");
    remote_sessions.shutdown();
}

void test_managed_server_secret_and_web_urls() {
    namespace fs = std::filesystem;
    std::string suffix;
    check(platform::secure_random_hex(8U, suffix).ok(),
          "managed-secret test creates a random temporary suffix");
    const fs::path directory = fs::temp_directory_path() /
                               ("ainiux-managed-server-secret-" + suffix);
    const fs::path path = directory / "server-secret";
    constexpr std::size_t workers = 8U;
    std::vector<std::string> concurrent_secrets(workers);
    std::vector<Error> concurrent_errors(workers);
    std::vector<int> concurrent_created(workers, 0);
    std::vector<std::thread> creators;
    for (std::size_t index = 0; index < workers; ++index) {
        creators.emplace_back([&, index] {
            bool item_created = false;
            concurrent_errors[index] = load_or_create_managed_server_secret(
                path.u8string(), concurrent_secrets[index], item_created);
            concurrent_created[index] = item_created ? 1 : 0;
        });
    }
    for (std::thread& creator : creators) creator.join();
    const std::string first = concurrent_secrets.front();
    std::size_t create_count = 0;
    bool concurrent_ok = first.size() == 64U;
    for (std::size_t index = 0; index < workers; ++index) {
        concurrent_ok = concurrent_ok && concurrent_errors[index].ok() &&
                        concurrent_secrets[index] == first;
        create_count += static_cast<std::size_t>(concurrent_created[index]);
    }
    check(concurrent_ok && create_count == 1U,
          "concurrent managed-secret startup atomically selects one stable 256-bit token");
    std::string second;
    bool created = true;
    Error error = load_or_create_managed_server_secret(path.u8string(), second, created);
    check(error.ok() && !created && second == first,
          "managed server secret remains stable across launches and concurrent-create logic");
    std::error_code filesystem_error;
    const fs::perms permissions = fs::status(path, filesystem_error).permissions();
    check(!filesystem_error &&
              (permissions & (fs::perms::group_all | fs::perms::others_all)) == fs::perms::none,
          "managed server secret is not accessible to group or other users");

    check(platform::atomic_write_private(path.u8string(), "broken", true).ok(),
          "managed-secret test can install corrupt private data");
    created = false;
    error = load_or_create_managed_server_secret(path.u8string(), second, created);
    check(!error.ok() && error.code == ErrorCode::Auth,
          "corrupt managed server secret fails closed instead of being silently replaced");

#if !defined(_WIN32)
    check(platform::atomic_write_private(path.u8string(), first, true).ok(),
          "managed-secret test restores valid data");
    fs::permissions(path, fs::perms::owner_read | fs::perms::owner_write |
                              fs::perms::group_read,
                    fs::perm_options::replace, filesystem_error);
    created = false;
    error = load_or_create_managed_server_secret(path.u8string(), second, created);
    check(!error.ok() && error.code == ErrorCode::Auth,
          "permissive managed server secret permissions fail closed");
    const fs::path linked_target = directory / "linked-secret-target";
    check(platform::atomic_write_private(linked_target.u8string(), first, true).ok(),
          "managed-secret test creates a private symlink target");
    fs::remove(path, filesystem_error);
    fs::create_symlink(linked_target.filename(), path, filesystem_error);
    created = false;
    error = load_or_create_managed_server_secret(path.u8string(), second, created);
    check(!filesystem_error && !error.ok() && error.code == ErrorCode::Auth,
          "managed server secret rejects symlink paths");
#endif

    const std::vector<std::string> urls = web_ui_urls(
        "0.0.0.0", 8766, false, {"192.168.1.20", "127.0.0.1", "10.0.0.5"});
    check(urls.size() == 3U && urls.front() == "http://127.0.0.1:8766/ui/" &&
              urls[1] == "http://10.0.0.5:8766/ui/" &&
              urls[2] == "http://192.168.1.20:8766/ui/",
          "wildcard webserver URL reporting includes loopback and sorted interface links");
    const std::vector<std::string> explicit_url = web_ui_urls(
        "192.0.2.8", 9443, true, {"10.0.0.5"});
    check(explicit_url.size() == 1U && explicit_url.front() == "https://192.0.2.8:9443/ui/",
          "explicit TLS webserver binding reports only its selected browser URL");

    fs::remove_all(directory, filesystem_error);
}

#if !defined(_WIN32)
int open_loopback(unsigned short port) {
    const int socket_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd < 0) return -1;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(socket_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        ::close(socket_fd);
        return -1;
    }
    return socket_fd;
}

std::string raw_request(unsigned short port, const std::string& request) {
    const int socket_fd = open_loopback(port);
    if (socket_fd < 0) return {};
    std::size_t sent = 0;
    while (sent < request.size()) {
        const ssize_t count = ::send(socket_fd, request.data() + sent, request.size() - sent, 0);
        if (count <= 0) break;
        sent += static_cast<std::size_t>(count);
    }
    (void)::shutdown(socket_fd, SHUT_WR);
    std::string response;
    char buffer[2048];
    for (;;) {
        const ssize_t count = ::recv(socket_fd, buffer, sizeof(buffer), 0);
        if (count <= 0) break;
        response.append(buffer, static_cast<std::size_t>(count));
    }
    ::close(socket_fd);
    return response;
}

std::size_t append_curl_body(char* data, std::size_t size, std::size_t count, void* userdata) {
    static_cast<std::string*>(userdata)->append(data, size * count);
    return size * count;
}

void test_tls_listener_lifecycle() {
    if (!TlsContext::available()) return;
    Listener listener;
    ListenerConfig config;
    config.port = 0;
    config.max_connections = 2;
    config.auth.full_control_secret = "controller";
    config.tls_cert_file = "tests/fixtures/server_tls_cert.pem";
    config.tls_key_file = "tests/fixtures/server_tls_key.pem";
    check(listener.start(config).ok() && listener.tls_enabled(),
          "listener loads a matching PEM certificate and private key through RAII TLS state");
    std::atomic<bool> stop{false};
    Error serve_error;
    std::thread server_thread([&] { serve_error = listener.serve_until([&] { return stop.load(); }); });

    CURL* curl = curl_easy_init();
    std::string body;
    long status = 0;
    curl_slist* raw_headers = nullptr;
    raw_headers = curl_slist_append(raw_headers, "Authorization: Bearer controller");
    const std::string origin = "Origin: https://127.0.0.1:" + std::to_string(listener.port());
    raw_headers = curl_slist_append(raw_headers, origin.c_str());
    const std::string url = "https://127.0.0.1:" + std::to_string(listener.port()) +
                            "/ainiux/v1/health";
    check(curl != nullptr && raw_headers != nullptr, "TLS client test resources allocate");
    if (curl != nullptr && raw_headers != nullptr) {
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, raw_headers);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 3L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, append_curl_body);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
        const CURLcode result = curl_easy_perform(curl);
        (void)curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        check(result == CURLE_OK && status == 200 && body == "{\"status\":\"ok\"}",
              "TLS listener completes an authenticated HTTPS request with strict Origin");
    }
    if (raw_headers != nullptr) curl_slist_free_all(raw_headers);
    if (curl != nullptr) curl_easy_cleanup(curl);
    stop.store(true);
    server_thread.join();
    check(serve_error.ok(), "TLS listener shuts down cleanly and releases connection state");

    Listener mismatched;
    config.tls_cert_file = "tests/fixtures/server_tls_key.pem";
    check(!mismatched.start(config).ok(), "listener rejects an invalid certificate file");
}

void test_loopback_listener_lifecycle() {
    Listener listener;
    ListenerConfig config;
    config.port = 0;
    config.max_connections = 2;
    config.auth.full_control_secret = "controller";
    check(listener.start(config).ok() && listener.port() != 0,
          "listener binds an ephemeral loopback port");
    std::atomic<bool> stop{false};
    Error serve_error;
    std::thread server_thread([&] { serve_error = listener.serve_until([&] { return stop.load(); }); });
    const std::string response = raw_request(listener.port(), request_text());
    check(response.find("HTTP/1.1 200 OK") == 0 &&
              response.find("X-Content-Type-Options: nosniff") != std::string::npos &&
              response.find("Content-Security-Policy: default-src 'none'") != std::string::npos,
          "raw loopback request reaches authenticated health with hardened headers");

    const std::string png("\x89PNG\r\n\x1a\n", 8);
    const std::string upload_response = raw_request(
        listener.port(),
        "POST /ainiux/v1/images/inputs HTTP/1.1\r\nHost: 127.0.0.1\r\n"
        "Authorization: Bearer controller\r\nContent-Type: image/png\r\n"
        "Content-Length: 8\r\nConnection: close\r\n\r\n" + png);
    check(upload_response.find("HTTP/1.1 201 Created") == 0 &&
              upload_response.find("\"mime_type\":\"image/png\"") != std::string::npos &&
              upload_response.find("\"id\":\"input_") != std::string::npos,
          "listener accepts an authenticated raw PNG upload through the large-body route");

    const int idle_client = open_loopback(listener.port());
    for (int i = 0; i < 50 && listener.active_connections() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    check(idle_client >= 0 && listener.active_connections() == 1,
          "listener tracks an idle active connection");
    stop.store(true);
    server_thread.join();
    if (idle_client >= 0) ::close(idle_client);
    check(serve_error.ok() && listener.active_connections() == 0,
          "listener shutdown joins workers and releases active connections");

    Listener capped;
    config.max_connections = 1;
    check(capped.start(config).ok(), "connection-cap listener starts");
    stop.store(false);
    std::thread capped_thread([&] { serve_error = capped.serve_until([&] { return stop.load(); }); });
    const int held = open_loopback(capped.port());
    for (int i = 0; i < 50 && capped.active_connections() != 1; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    const std::string busy = raw_request(capped.port(), request_text());
    check(held >= 0 && busy.find("HTTP/1.1 429 Too Many Requests") == 0,
          "simultaneous connection cap rejects excess clients");
    if (held >= 0) ::close(held);
    stop.store(true);
    capped_thread.join();
}
#else
void test_tls_listener_lifecycle() {}
void test_loopback_listener_lifecycle() {
    // Native Windows socket lifecycle is compiled into the product target; the
    // platform integration gate exercises it with the packaged executable.
}
#endif

}  // namespace

void run_all() {
    test_fragmented_parser_and_pipeline();
    test_strict_framing_and_limits();
    test_embedded_web_ui_assets_and_browser_security();
    test_auth_and_routes();
    test_image_catalog_uploads_and_job_references();
    test_event_replay_is_ordered_and_bounded();
    test_job_registry_idempotency_lane_and_cancellation();
    test_provider_job_concurrency_cap();
    test_generated_images_use_collision_safe_workspace_names();
    test_terminal_retention_eviction_releases_workers_safely();
    test_job_routes_and_sse();
    test_mcp_stateless_adapter_and_tasks();
    test_interactive_sessions_are_bounded_and_replayable();
    test_read_only_workspace_routes_are_contained_and_bounded();
    test_revision_safe_workspace_mutations_and_editor_assist();
    test_revision_safe_chat_thread_routes();
    test_job_errors_hide_server_side_paths();
    test_server_cli_contract();
    test_managed_server_secret_and_web_urls();
    test_loopback_listener_lifecycle();
    test_tls_listener_lifecycle();
}

}  // namespace ainiux::test::server_control
