#include "http/http.hpp"

#include <curl/curl.h>

#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>

#include "security/redact.hpp"

namespace pkchat::http {

namespace {

class CurlGlobal {
   public:
    CurlGlobal() : code_(curl_global_init(CURL_GLOBAL_DEFAULT)) {}
    ~CurlGlobal() {
        if (code_ == CURLE_OK) {
            curl_global_cleanup();
        }
    }
    CurlGlobal(const CurlGlobal&) = delete;
    CurlGlobal& operator=(const CurlGlobal&) = delete;
    CURLcode code() const { return code_; }

   private:
    CURLcode code_;
};

class CurlHandle {
   public:
    CurlHandle() : handle_(curl_easy_init()) {}
    ~CurlHandle() {
        if (handle_ != nullptr) {
            curl_easy_cleanup(handle_);
        }
    }
    CurlHandle(const CurlHandle&) = delete;
    CurlHandle& operator=(const CurlHandle&) = delete;
    CURL* get() const { return handle_; }

   private:
    CURL* handle_ = nullptr;
};

class CurlHeaders {
   public:
    CurlHeaders() = default;
    ~CurlHeaders() {
        if (list_ != nullptr) {
            curl_slist_free_all(list_);
        }
    }
    CurlHeaders(const CurlHeaders&) = delete;
    CurlHeaders& operator=(const CurlHeaders&) = delete;

    Error append(const std::string& header) {
        curl_slist* next = curl_slist_append(list_, header.c_str());
        if (next == nullptr) {
            return {ErrorCode::Internal, "could not allocate HTTP request header list"};
        }
        list_ = next;
        return ok_error();
    }

    curl_slist* get() const { return list_; }

   private:
    curl_slist* list_ = nullptr;
};

std::string lower_ascii(std::string text) {
    for (char& ch : text) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return text;
}

std::string trim_ascii(std::string text) {
    auto is_ws = [](unsigned char ch) { return ch == ' ' || ch == '\n' || ch == '\r' || ch == '\t'; };
    while (!text.empty() && is_ws(static_cast<unsigned char>(text.front()))) {
        text.erase(text.begin());
    }
    while (!text.empty() && is_ws(static_cast<unsigned char>(text.back()))) {
        text.pop_back();
    }
    return text;
}

std::string trim_line_end(std::string line) {
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
        line.pop_back();
    }
    return line;
}

long parse_status_line(const std::string& line) {
    if (line.rfind("HTTP/", 0) != 0) {
        return 0;
    }
    const size_t first_space = line.find(' ');
    if (first_space == std::string::npos) {
        return 0;
    }
    char* end = nullptr;
    const char* start = line.c_str() + first_space + 1;
    const long status = std::strtol(start, &end, 10);
    if (end == start) {
        return 0;
    }
    return status;
}

bool is_proxy_connect_line(const std::string& line) {
    return line.find("Connection established") != std::string::npos;
}

Error classify_curl_error(CURLcode code, const std::string& detail, const std::string& url) {
    const std::string msg = "libcurl transport failed for " + url + ": " + detail;
    switch (code) {
        case CURLE_COULDNT_RESOLVE_HOST:
        case CURLE_COULDNT_RESOLVE_PROXY:
            return {ErrorCode::Dns, msg};
        case CURLE_COULDNT_CONNECT:
            return {ErrorCode::Connect, msg};
        case CURLE_OPERATION_TIMEDOUT:
            return {ErrorCode::Timeout, msg};
        case CURLE_SSL_CONNECT_ERROR:
        case CURLE_PEER_FAILED_VERIFICATION:
        case CURLE_SSL_CACERT_BADFILE:
        case CURLE_USE_SSL_FAILED:
            return {ErrorCode::Tls, msg};
        case CURLE_OUT_OF_MEMORY:
            return {ErrorCode::Internal, msg};
        default:
            return {ErrorCode::Internal, msg};
    }
}

struct TransferState {
    const Request* request = nullptr;
    Response response;
    Error callback_error;
    std::string debug_text;
    long current_status = 0;
    bool current_proxy_connect = false;
    bool final_headers_seen = false;
    bool cancelled = false;
    bool blocked_address = false;
    std::string blocked_address_text;
    std::chrono::steady_clock::time_point transfer_start;
    bool first_body_seen = false;
};

bool blocked_ipv4(const in_addr& address) {
    const uint32_t ip = ntohl(address.s_addr);
    const unsigned int a = (ip >> 24U) & 0xffU;
    const unsigned int b = (ip >> 16U) & 0xffU;
    return a == 0U || a == 10U || a == 127U ||
           (a == 100U && b >= 64U && b <= 127U) ||
           (a == 169U && b == 254U) ||
           (a == 172U && b >= 16U && b <= 31U) ||
           (a == 192U && b == 168U) || a >= 224U;
}

bool blocked_socket_address(const curl_sockaddr* address, std::string& printable) {
    char buffer[INET6_ADDRSTRLEN] = {};
    if (address->family == AF_INET) {
        const sockaddr_in* ipv4 = reinterpret_cast<const sockaddr_in*>(&address->addr);
        inet_ntop(AF_INET, &ipv4->sin_addr, buffer, sizeof(buffer));
        printable = buffer;
        return blocked_ipv4(ipv4->sin_addr);
    }
    if (address->family == AF_INET6) {
        const sockaddr_in6* ipv6 = reinterpret_cast<const sockaddr_in6*>(&address->addr);
        inet_ntop(AF_INET6, &ipv6->sin6_addr, buffer, sizeof(buffer));
        printable = buffer;
        const unsigned char* bytes = ipv6->sin6_addr.s6_addr;
        if (IN6_IS_ADDR_UNSPECIFIED(&ipv6->sin6_addr) || IN6_IS_ADDR_LOOPBACK(&ipv6->sin6_addr) ||
            bytes[0] == 0xffU || (bytes[0] & 0xfeU) == 0xfcU ||
            (bytes[0] == 0xfeU && (bytes[1] & 0xc0U) == 0x80U)) {
            return true;
        }
        if (IN6_IS_ADDR_V4MAPPED(&ipv6->sin6_addr)) {
            in_addr mapped{};
            std::memcpy(&mapped.s_addr, bytes + 12, sizeof(mapped.s_addr));
            return blocked_ipv4(mapped);
        }
    }
    return false;
}

curl_socket_t open_socket_callback(void* userdata, curlsocktype purpose, struct curl_sockaddr* address) {
    TransferState* state = static_cast<TransferState*>(userdata);
    if (purpose == CURLSOCKTYPE_IPCXN) {
        std::string printable;
        if (blocked_socket_address(address, printable)) {
            state->blocked_address = true;
            state->blocked_address_text = std::move(printable);
            return CURL_SOCKET_BAD;
        }
    }
    return ::socket(address->family, address->socktype, address->protocol);
}

size_t header_callback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    const size_t bytes = size * nmemb;
    TransferState* state = static_cast<TransferState*>(userdata);
    std::string line(ptr, bytes);
    const std::string trimmed = trim_line_end(line);

    if (trimmed.rfind("HTTP/", 0) == 0) {
        state->current_status = parse_status_line(trimmed);
        state->current_proxy_connect = is_proxy_connect_line(trimmed);
        state->final_headers_seen = false;
        return bytes;
    }

    const std::string lowered = lower_ascii(trimmed);
    if (!state->current_proxy_connect && lowered.rfind("content-type:", 0) == 0) {
        state->response.content_type = trim_ascii(trimmed.substr(std::string("Content-Type:").size()));
    }

    if (trimmed.empty()) {
        if (state->current_status >= 100 && state->current_status < 200) {
            return bytes;
        }
        if (state->current_proxy_connect) {
            return bytes;
        }
        if (state->current_status != 0) {
            state->response.status = state->current_status;
            state->final_headers_seen = true;
        }
    }
    return bytes;
}

size_t write_callback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    const size_t bytes = size * nmemb;
    TransferState* state = static_cast<TransferState*>(userdata);
    if (state->request->cancellation.cancelled()) {
        state->cancelled = true;
        state->callback_error = {ErrorCode::Cancelled, "HTTP request cancelled: " + state->request->url};
        return 0;
    }
    if (state->request->max_body_bytes > 0) {
        const size_t limit = static_cast<size_t>(state->request->max_body_bytes);
        if (bytes > limit || state->response.body.size() > limit - bytes) {
            state->callback_error = {ErrorCode::FileRead,
                                     "HTTP response exceeded maximum body size for " + state->request->url};
            return 0;
        }
    }
    const std::string chunk(ptr, bytes);
    if (!state->first_body_seen && !chunk.empty()) {
        state->first_body_seen = true;
        state->response.first_body_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - state->transfer_start)
                .count();
    }
    state->response.body += chunk;
    if (state->request->on_body && state->response.status >= 200 && state->response.status < 300 && !chunk.empty()) {
        state->callback_error = state->request->on_body(chunk);
        if (!state->callback_error.ok()) {
            return 0;
        }
    }
    return bytes;
}

int debug_callback(CURL*, curl_infotype, char* data, size_t size, void* userdata) {
    TransferState* state = static_cast<TransferState*>(userdata);
    state->debug_text.append(data, size);
    return 0;
}

int progress_callback(void* userdata, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    TransferState* state = static_cast<TransferState*>(userdata);
    if (state->request->cancellation.cancelled()) {
        state->cancelled = true;
        return 1;
    }
    return 0;
}

Error setopt(CURL* curl, CURLoption option, const char* value) {
    CURLcode code = curl_easy_setopt(curl, option, value);
    if (code != CURLE_OK) {
        return {ErrorCode::Internal, std::string("curl_easy_setopt failed: ") + curl_easy_strerror(code)};
    }
    return ok_error();
}

Error setopt_long(CURL* curl, CURLoption option, long value) {
    CURLcode code = curl_easy_setopt(curl, option, value);
    if (code != CURLE_OK) {
        return {ErrorCode::Internal, std::string("curl_easy_setopt failed: ") + curl_easy_strerror(code)};
    }
    return ok_error();
}

Error setopt_ptr(CURL* curl, CURLoption option, void* value) {
    CURLcode code = curl_easy_setopt(curl, option, value);
    if (code != CURLE_OK) {
        return {ErrorCode::Internal, std::string("curl_easy_setopt failed: ") + curl_easy_strerror(code)};
    }
    return ok_error();
}

long long curl_elapsed_ms(CURL* curl, CURLINFO info) {
    double seconds = 0.0;
    if (curl_easy_getinfo(curl, info, &seconds) != CURLE_OK || seconds < 0.0) {
        return -1;
    }
    return static_cast<long long>(seconds * 1000.0);
}

}  // namespace

Result perform(const Request& request, const std::vector<std::string>& secrets) {
    static CurlGlobal global;
    if (global.code() != CURLE_OK) {
        return {{}, {ErrorCode::Internal, std::string("curl_global_init failed: ") + curl_easy_strerror(global.code())}};
    }

    CurlHandle handle;
    if (handle.get() == nullptr) {
        return {{}, {ErrorCode::Internal, "could not allocate libcurl easy handle"}};
    }
    CURL* curl = handle.get();

    CurlHeaders headers;
    for (const std::string& header : request.headers) {
        Error err = headers.append(header);
        if (!err.ok()) {
            return {{}, err};
        }
    }

    TransferState state;
    state.request = &request;
    char error_buffer[CURL_ERROR_SIZE] = {};

    Error err = setopt(curl, CURLOPT_URL, request.url.c_str());
    if (!err.ok()) return {{}, err};
    err = setopt_long(curl, CURLOPT_NOSIGNAL, 1L);
    if (!err.ok()) return {{}, err};
    err = setopt_long(curl, CURLOPT_CONNECTTIMEOUT, request.connect_timeout_seconds);
    if (!err.ok()) return {{}, err};
    if (request.timeout_seconds > 0) {
        err = setopt_long(curl, CURLOPT_TIMEOUT, request.timeout_seconds);
        if (!err.ok()) return {{}, err};
    }
    if (!request.proxy.empty()) {
        err = setopt(curl, CURLOPT_PROXY, request.proxy.c_str());
        if (!err.ok()) return {{}, err};
    }
    if (request.insecure_tls) {
        err = setopt_long(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        if (!err.ok()) return {{}, err};
        err = setopt_long(curl, CURLOPT_SSL_VERIFYHOST, 0L);
        if (!err.ok()) return {{}, err};
    }
    if (headers.get() != nullptr) {
        err = setopt_ptr(curl, CURLOPT_HTTPHEADER, headers.get());
        if (!err.ok()) return {{}, err};
    }
    if (request.block_private_addresses) {
        err = setopt_ptr(curl, CURLOPT_OPENSOCKETDATA, &state);
        if (!err.ok()) return {{}, err};
        CURLcode open_code = curl_easy_setopt(curl, CURLOPT_OPENSOCKETFUNCTION, open_socket_callback);
        if (open_code != CURLE_OK) {
            return {{}, {ErrorCode::Internal,
                         std::string("curl_easy_setopt failed: ") + curl_easy_strerror(open_code)}};
        }
    }

    if (request.method == "POST") {
        err = setopt_long(curl, CURLOPT_POST, 1L);
        if (!err.ok()) return {{}, err};
        err = setopt(curl, CURLOPT_POSTFIELDS, request.body.c_str());
        if (!err.ok()) return {{}, err};
        CURLcode code = curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE,
                                         static_cast<curl_off_t>(request.body.size()));
        if (code != CURLE_OK) {
            return {{}, {ErrorCode::Internal, std::string("curl_easy_setopt failed: ") + curl_easy_strerror(code)}};
        }
    } else if (request.method == "GET") {
        err = setopt_long(curl, CURLOPT_HTTPGET, 1L);
        if (!err.ok()) return {{}, err};
    } else {
        err = setopt(curl, CURLOPT_CUSTOMREQUEST, request.method.c_str());
        if (!err.ok()) return {{}, err};
    }

    err = setopt_ptr(curl, CURLOPT_HEADERDATA, &state);
    if (!err.ok()) return {{}, err};
    CURLcode code = curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_callback);
    if (code != CURLE_OK) {
        return {{}, {ErrorCode::Internal, std::string("curl_easy_setopt failed: ") + curl_easy_strerror(code)}};
    }
    err = setopt_ptr(curl, CURLOPT_WRITEDATA, &state);
    if (!err.ok()) return {{}, err};
    code = curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    if (code != CURLE_OK) {
        return {{}, {ErrorCode::Internal, std::string("curl_easy_setopt failed: ") + curl_easy_strerror(code)}};
    }
    err = setopt_ptr(curl, CURLOPT_ERRORBUFFER, error_buffer);
    if (!err.ok()) return {{}, err};
    err = setopt_long(curl, CURLOPT_NOPROGRESS, 0L);
    if (!err.ok()) return {{}, err};
    err = setopt_ptr(curl, CURLOPT_XFERINFODATA, &state);
    if (!err.ok()) return {{}, err};
    code = curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_callback);
    if (code != CURLE_OK) {
        return {{}, {ErrorCode::Internal, std::string("curl_easy_setopt failed: ") + curl_easy_strerror(code)}};
    }
    if (request.trace) {
        err = setopt_long(curl, CURLOPT_VERBOSE, 1L);
        if (!err.ok()) return {{}, err};
        err = setopt_ptr(curl, CURLOPT_DEBUGDATA, &state);
        if (!err.ok()) return {{}, err};
        code = curl_easy_setopt(curl, CURLOPT_DEBUGFUNCTION, debug_callback);
        if (code != CURLE_OK) {
            return {{}, {ErrorCode::Internal, std::string("curl_easy_setopt failed: ") + curl_easy_strerror(code)}};
        }
    }

    state.transfer_start = std::chrono::steady_clock::now();
    code = curl_easy_perform(curl);
    state.response.dns_ms = curl_elapsed_ms(curl, CURLINFO_NAMELOOKUP_TIME);
    state.response.connect_ms = curl_elapsed_ms(curl, CURLINFO_CONNECT_TIME);
    state.response.tls_ms = curl_elapsed_ms(curl, CURLINFO_APPCONNECT_TIME);
    state.response.time_to_first_byte_ms = curl_elapsed_ms(curl, CURLINFO_STARTTRANSFER_TIME);
    state.response.total_ms = curl_elapsed_ms(curl, CURLINFO_TOTAL_TIME);
    if (state.response.status == 0) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &state.response.status);
    }
    state.response.stderr_text = redact_secrets(state.debug_text, secrets);

    if (code != CURLE_OK) {
        if (state.cancelled || request.cancellation.cancelled() || code == CURLE_ABORTED_BY_CALLBACK) {
            return {state.response, {ErrorCode::Cancelled, "HTTP request cancelled: " + request.url}};
        }
        if (code == CURLE_WRITE_ERROR && !state.callback_error.ok()) {
            if (state.callback_error.code == ErrorCode::StreamComplete) {
                return {state.response, ok_error()};
            }
            return {state.response, state.callback_error};
        }
        if (state.blocked_address) {
            return {state.response,
                    {ErrorCode::BadUrl,
                     "refusing HTTP connection to private, loopback, link-local, multicast, or metadata "
                     "address " + state.blocked_address_text + " resolved for " + request.url}};
        }
        std::string detail = error_buffer[0] == '\0' ? curl_easy_strerror(code) : error_buffer;
        detail = redact_secrets(detail, secrets);
        return {state.response, classify_curl_error(code, detail, request.url)};
    }
    return {state.response, ok_error()};
}

}  // namespace pkchat::http
