#include "encoding/encoding.hpp"

#include "platform/environment.hpp"
#include "runtime/subprocess.hpp"

#include <algorithm>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "platform/windows_utf.hpp"
#else
#include <unistd.h>
#endif

namespace ainiux::encoding {
namespace {

#if !defined(_WIN32)
bool executable_on_path(const std::string& name, std::string& resolved) {
    const std::string path = platform::environment_value("PATH");
    size_t start = 0;
    while (start <= path.size()) {
        const size_t end = path.find(':', start);
        const std::string directory =
            path.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!directory.empty() && directory.front() == '/') {
            const std::string candidate = directory + "/" + name;
            if (access(candidate.c_str(), X_OK) == 0) {
                resolved = candidate;
                return true;
            }
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return false;
}

const char* iconv_from_name(const std::string& canonical) {
    if (canonical == "gbk") return "GBK";
    if (canonical == "gb2312") return "GB2312";
    if (canonical == "gb18030") return "GB18030";
    if (canonical == "big5") return "BIG5";
    if (canonical == "shift_jis") return "SHIFT_JIS";
    if (canonical == "euc-jp") return "EUC-JP";
    if (canonical == "euc-kr") return "EUC-KR";
    return nullptr;
}
#endif

#if defined(_WIN32)
unsigned windows_code_page(const std::string& canonical) {
    if (canonical == "gbk" || canonical == "gb2312") return 936;
    if (canonical == "gb18030") return 54936;
    if (canonical == "big5") return 950;
    if (canonical == "shift_jis") return 932;
    if (canonical == "euc-kr") return 949;
    if (canonical == "euc-jp") return 20932;
    return 0;
}
#endif

}  // namespace

Error convert_external(const std::string& bytes,
                       const std::string& canonical,
                       std::string& utf8,
                       runtime::CancellationToken cancellation) {
    utf8.clear();
    if (!is_external_encoding_name(canonical)) {
        return {ErrorCode::UnsupportedFeature,
                "character encoding '" + canonical +
                    "' is not a built-in or allowlisted external encoding"};
    }
    if (cancellation.cancelled()) {
        return {ErrorCode::Cancelled, "character encoding conversion cancelled"};
    }
    if (bytes.empty()) {
        return ok_error();
    }

#if defined(_WIN32)
    const unsigned code_page = windows_code_page(canonical);
    if (code_page == 0) {
        return {ErrorCode::UnsupportedFeature,
                "Windows has no built-in converter for '" + canonical +
                    "'; convert the document to UTF-8"};
    }
    const int wide_count = MultiByteToWideChar(code_page, MB_ERR_INVALID_CHARS, bytes.data(),
                                               static_cast<int>(bytes.size()), nullptr, 0);
    if (wide_count <= 0) {
        return {ErrorCode::UnsupportedFeature,
                "could not convert '" + canonical + "' to UTF-8 using the Windows code page"};
    }
    std::wstring wide(static_cast<size_t>(wide_count), L'\0');
    if (MultiByteToWideChar(code_page, MB_ERR_INVALID_CHARS, bytes.data(),
                            static_cast<int>(bytes.size()), wide.data(), wide_count) <= 0) {
        return {ErrorCode::UnsupportedFeature,
                "could not convert '" + canonical + "' to UTF-8 using the Windows code page"};
    }
    return platform::utf16_to_utf8(wide, utf8);
#else
    const char* from = iconv_from_name(canonical);
    if (from == nullptr) {
        return {ErrorCode::UnsupportedFeature,
                "character encoding '" + canonical + "' is not allowlisted for iconv"};
    }
    std::string iconv_path;
    if (!executable_on_path("iconv", iconv_path)) {
        return {ErrorCode::UnsupportedFeature,
                "cannot convert '" + canonical +
                    "' to UTF-8: iconv is not installed. Install iconv or convert the "
                    "document to UTF-8"};
    }
    runtime::SubprocessOptions options;
    options.executable = iconv_path;
    options.arguments = {"-f", from, "-t", "UTF-8"};
    options.provide_stdin = true;
    options.stdin_text = bytes;
    options.stdout_limit = std::max(bytes.size() * 4 + 64, static_cast<size_t>(4096));
    options.stderr_limit = 4096;
    options.timeout_ms = 30000;
    options.cancellation = cancellation;
    options.environment.push_back("PATH=" + platform::environment_value("PATH"));
    const std::string locale = platform::environment_value("LANG");
    if (!locale.empty()) {
        options.environment.push_back("LANG=" + locale);
    }
    runtime::SubprocessResult result;
    Error err = runtime::run_subprocess(options, result);
    if (result.termination == runtime::SubprocessTerminationReason::Cancelled) {
        return {ErrorCode::Cancelled, "character encoding conversion cancelled"};
    }
    if (result.termination == runtime::SubprocessTerminationReason::TimedOut) {
        return {ErrorCode::Timeout, "iconv timed out converting '" + canonical + "' to UTF-8"};
    }
    if (!err.ok() || result.exit_code != 0) {
        std::string detail = result.stderr_text;
        if (detail.size() > 200) {
            detail.resize(200);
        }
        return {ErrorCode::UnsupportedFeature,
                "iconv could not convert '" + canonical + "' to UTF-8" +
                    (detail.empty() ? std::string() : (": " + detail))};
    }
    if (!is_valid_utf8(result.stdout_text)) {
        return {ErrorCode::UnsupportedFeature,
                "iconv produced invalid UTF-8 while converting '" + canonical + "'"};
    }
    utf8 = std::move(result.stdout_text);
    return ok_error();
#endif
}

}  // namespace ainiux::encoding
