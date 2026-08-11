#include "mcp/transport_http.hpp"

#include <algorithm>

#include "http/http.hpp"
#include "json/json.hpp"
#include "security/redact.hpp"

namespace ainiux::mcp {
namespace {

std::vector<std::string> base_headers(const HttpSession& session,
                                      const std::string& method,
                                      const std::string& mcp_name) {
    std::vector<std::string> headers = {
        "Content-Type: application/json",
        "Accept: application/json, text/event-stream",
    };
    headers.insert(headers.end(), session.extra_headers.begin(), session.extra_headers.end());
    if (!session.session_id.empty())
        headers.push_back("Mcp-Session-Id: " + session.session_id);

    if (session.dialect == Dialect::Stateless20260728) {
        headers.push_back("MCP-Protocol-Version: 2026-07-28");
        if (!method.empty()) headers.push_back("Mcp-Method: " + method);
        if (!mcp_name.empty()) headers.push_back("Mcp-Name: " + mcp_name);
    } else if (session.dialect == Dialect::Streamable20251125) {
        headers.push_back("MCP-Protocol-Version: 2025-11-25");
    } else if (session.dialect == Dialect::Streamable20250326) {
        headers.push_back("MCP-Protocol-Version: 2025-03-26");
    }
    return headers;
}

Error perform_post(const std::string& url,
                   const std::vector<std::string>& headers,
                   const std::string& body,
                   const HttpTransportOptions& options,
                   http::Response& response) {
    http::Request request;
    request.method = "POST";
    request.url = url;
    request.headers = headers;
    request.body = body;
    request.connect_timeout_seconds = options.connect_timeout_seconds;
    request.timeout_seconds = options.timeout_seconds;
    request.block_private_addresses = options.block_private_addresses;
    request.insecure_tls = options.insecure_tls;
    request.trace = options.trace;
    request.cancellation = options.cancellation;
    request.follow_redirects = false;
    http::Result result = http::perform(request, options.secrets_to_redact);
    response = std::move(result.response);
    return result.error;
}

std::string response_session_header(const http::Response& response) {
    for (const auto& header : response.diagnostic_headers) {
        std::string key = header.first;
        std::transform(key.begin(), key.end(), key.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (key == "mcp-session-id") return header.second;
    }
    return {};
}

Error try_stateless(const ServerConfig& config,
                    const HttpTransportOptions& options,
                    HttpSession& session) {
    session = HttpSession{};
    session.endpoint_url = config.url;
    session.dialect = Dialect::Stateless20260728;
    for (const std::string& header : config.headers)
        session.extra_headers.push_back(expand_env_refs(header));

    const ClientInfo client = default_client_info();
    json::Value params = make_json_object();
    params.object["_meta"] = build_stateless_meta(client, "2026-07-28");
    json::Value req = build_request(1, "server/discover", params);
    JsonRpcResponse response;
    Error err = http_post_jsonrpc(session, "server/discover", "", json::stringify(req), options,
                                  response, nullptr);
    if (!err.ok()) return err;
    if (response.has_error)
        return {ErrorCode::ProviderSchema,
                "server/discover failed: " + response.error_message};
    return ok_error();
}

Error try_legacy_initialize(const ServerConfig& config,
                            const HttpTransportOptions& options,
                            const std::string& protocol_version,
                            Dialect dialect,
                            HttpSession& session) {
    session = HttpSession{};
    session.endpoint_url = config.url;
    session.dialect = dialect;
    for (const std::string& header : config.headers)
        session.extra_headers.push_back(expand_env_refs(header));

    const ClientInfo client = default_client_info();
    json::Value req =
        build_request(1, "initialize", build_initialize_params(protocol_version, client));
    std::string new_session;
    JsonRpcResponse response;
    Error err = http_post_jsonrpc(session, "initialize", "", json::stringify(req), options,
                                  response, &new_session);
    if (!err.ok()) return err;
    if (response.has_error)
        return {ErrorCode::ProviderSchema, "initialize failed: " + response.error_message};
    if (!new_session.empty()) session.session_id = new_session;

    // notifications/initialized (no id)
    json::Value note = make_json_object();
    note.object["jsonrpc"] = make_json_string("2.0");
    note.object["method"] = make_json_string("notifications/initialized");
    JsonRpcResponse note_response;
    (void)http_post_jsonrpc(session, "notifications/initialized", "", json::stringify(note),
                            options, note_response, nullptr);
    return ok_error();
}

}  // namespace

Error http_post_jsonrpc(const HttpSession& session,
                        const std::string& method,
                        const std::string& mcp_name,
                        const std::string& body_json,
                        const HttpTransportOptions& options,
                        JsonRpcResponse& response,
                        std::string* response_session_id) {
    response = JsonRpcResponse{};
    if (session.endpoint_url.empty())
        return {ErrorCode::BadUrl, "MCP HTTP endpoint URL is empty"};

    const std::vector<std::string> headers = base_headers(session, method, mcp_name);
    http::Response http_response;
    Error err = perform_post(session.endpoint_url, headers, body_json, options, http_response);
    if (!err.ok()) return err;

    if (response_session_id != nullptr)
        *response_session_id = response_session_header(http_response);

    // Notifications may return 202 with empty body.
    if (http_response.status == 202 || http_response.body.empty()) {
        response.is_notification_ack = true;
        return ok_error();
    }
    if (http_response.status < 200 || http_response.status >= 300) {
        std::string detail = "MCP HTTP status " + std::to_string(http_response.status);
        if (!http_response.body.empty()) {
            detail += ": ";
            detail += http_response.body.substr(0, 400);
        }
        ErrorCode code = ErrorCode::HttpStatus;
        if (http_response.status == 401 || http_response.status == 403) code = ErrorCode::Auth;
        return {code, ::ainiux::redact_secrets(detail, options.secrets_to_redact)};
    }
    return parse_jsonrpc_response(http_response.body, response);
}

Error http_connect_negotiate(const ServerConfig& config,
                             const HttpTransportOptions& options,
                             HttpSession& session) {
    if (config.url.empty())
        return {ErrorCode::BadArgs, "HTTP MCP server requires a URL"};

    auto try_hint = [&](ProtocolHint hint) -> Error {
        switch (hint) {
            case ProtocolHint::Stateless20260728:
                return try_stateless(config, options, session);
            case ProtocolHint::Legacy20251125:
                return try_legacy_initialize(config, options, "2025-11-25",
                                             Dialect::Streamable20251125, session);
            case ProtocolHint::Legacy20250326:
                return try_legacy_initialize(config, options, "2025-03-26",
                                             Dialect::Streamable20250326, session);
            case ProtocolHint::Auto:
                break;
        }
        return {ErrorCode::Internal, "invalid protocol hint"};
    };

    if (config.protocol_hint != ProtocolHint::Auto) return try_hint(config.protocol_hint);

    if (config.last_dialect == Dialect::Stateless20260728) {
        Error err = try_stateless(config, options, session);
        if (err.ok()) return err;
    } else if (config.last_dialect == Dialect::Streamable20251125) {
        Error err = try_legacy_initialize(config, options, "2025-11-25",
                                          Dialect::Streamable20251125, session);
        if (err.ok()) return err;
    } else if (config.last_dialect == Dialect::Streamable20250326) {
        Error err = try_legacy_initialize(config, options, "2025-03-26",
                                          Dialect::Streamable20250326, session);
        if (err.ok()) return err;
    }

    // Probe order: newest first, then common older.
    Error last = try_stateless(config, options, session);
    if (last.ok()) return last;

    last = try_legacy_initialize(config, options, "2025-11-25", Dialect::Streamable20251125,
                                session);
    if (last.ok()) return last;

    last = try_legacy_initialize(config, options, "2025-03-26", Dialect::Streamable20250326,
                                session);
    if (last.ok()) return last;

    return {last.code, "MCP HTTP negotiate failed: " + last.message};
}

}  // namespace ainiux::mcp
