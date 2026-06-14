#include "http/http.hpp"

#include <curl/curl.h>

#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

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
};

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
    const std::string chunk(ptr, bytes);
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

    code = curl_easy_perform(curl);
    if (state.response.status == 0) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &state.response.status);
    }
    state.response.stderr_text = redact_secrets(state.debug_text, secrets);

    if (code != CURLE_OK) {
        if (code == CURLE_WRITE_ERROR && !state.callback_error.ok()) {
            return {state.response, state.callback_error};
        }
        std::string detail = error_buffer[0] == '\0' ? curl_easy_strerror(code) : error_buffer;
        detail = redact_secrets(detail, secrets);
        return {state.response, classify_curl_error(code, detail, request.url)};
    }
    return {state.response, ok_error()};
}

}  // namespace pkchat::http
