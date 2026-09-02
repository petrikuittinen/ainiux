#include "server/web_ui_launch.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <set>

#include "platform/environment.hpp"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <shellapi.h>
#include <windows.h>
#include "platform/windows_utf.hpp"
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace ainiux::server {

std::vector<std::string> local_ipv4_addresses() {
    std::set<std::string> addresses;
#if defined(_WIN32)
    ULONG bytes = 16U * 1024U;
    std::vector<unsigned char> storage(bytes);
    auto* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(storage.data());
    ULONG result = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST |
                                                     GAA_FLAG_SKIP_MULTICAST |
                                                     GAA_FLAG_SKIP_DNS_SERVER,
                                        nullptr, adapters, &bytes);
    if (result == ERROR_BUFFER_OVERFLOW) {
        storage.resize(bytes);
        adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(storage.data());
        result = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST |
                                                   GAA_FLAG_SKIP_MULTICAST |
                                                   GAA_FLAG_SKIP_DNS_SERVER,
                                      nullptr, adapters, &bytes);
    }
    if (result == NO_ERROR) {
        for (const IP_ADAPTER_ADDRESSES* adapter = adapters; adapter != nullptr;
             adapter = adapter->Next) {
            if (adapter->OperStatus != IfOperStatusUp) continue;
            for (const IP_ADAPTER_UNICAST_ADDRESS* item = adapter->FirstUnicastAddress;
                 item != nullptr; item = item->Next) {
                if (item->Address.lpSockaddr == nullptr ||
                    item->Address.lpSockaddr->sa_family != AF_INET)
                    continue;
                char text[INET_ADDRSTRLEN]{};
                const auto* address = reinterpret_cast<const sockaddr_in*>(
                    item->Address.lpSockaddr);
                if (InetNtopA(AF_INET, const_cast<IN_ADDR*>(&address->sin_addr), text,
                              static_cast<DWORD>(sizeof(text))) != nullptr)
                    addresses.emplace(text);
            }
        }
    }
#else
    ifaddrs* raw = nullptr;
    if (::getifaddrs(&raw) == 0) {
        struct Guard {
            ifaddrs* value;
            ~Guard() { if (value != nullptr) ::freeifaddrs(value); }
        } guard{raw};
        for (const ifaddrs* item = raw; item != nullptr; item = item->ifa_next) {
            if (item->ifa_addr == nullptr || item->ifa_addr->sa_family != AF_INET ||
                (item->ifa_flags & IFF_UP) == 0)
                continue;
            char text[INET_ADDRSTRLEN]{};
            const auto* address = reinterpret_cast<const sockaddr_in*>(item->ifa_addr);
            if (::inet_ntop(AF_INET, &address->sin_addr, text, sizeof(text)) != nullptr)
                addresses.emplace(text);
        }
    }
#endif
    return {addresses.begin(), addresses.end()};
}

std::vector<std::string> web_ui_urls(const std::string& bind_address,
                                     unsigned short port,
                                     bool tls,
                                     const std::vector<std::string>& local_addresses) {
    std::set<std::string> hosts;
    if (bind_address == "0.0.0.0") {
        hosts.emplace("127.0.0.1");
        for (const std::string& address : local_addresses) {
            if (!address.empty() && address != "0.0.0.0") hosts.emplace(address);
        }
    } else {
        hosts.emplace(bind_address == "localhost" ? "127.0.0.1" : bind_address);
    }
    std::vector<std::string> urls;
    urls.reserve(hosts.size());
    for (const std::string& host : hosts) {
        urls.push_back(std::string(tls ? "https" : "http") + "://" + host + ':' +
                       std::to_string(port) + "/ui/");
    }
    std::stable_sort(urls.begin(), urls.end(), [](const std::string& left,
                                                  const std::string& right) {
        const bool left_loopback = left.find("://127.") != std::string::npos;
        const bool right_loopback = right.find("://127.") != std::string::npos;
        if (left_loopback != right_loopback) return left_loopback;
        return left < right;
    });
    return urls;
}

BrowserLaunchResult launch_browser_best_effort(const std::string& url) {
    BrowserLaunchResult result;
    if (url.empty()) {
        result.detail = "no browser URL was available";
        return result;
    }
#if defined(_WIN32)
    std::wstring wide_url;
    const Error conversion = platform::utf8_to_utf16(url, wide_url);
    if (!conversion.ok()) {
        result.detail = conversion.message;
        return result;
    }
    result.attempted = true;
    const HINSTANCE opened = ShellExecuteW(nullptr, L"open", wide_url.c_str(), nullptr,
                                           nullptr, SW_SHOWNORMAL);
    result.started = reinterpret_cast<std::intptr_t>(opened) > 32;
    if (!result.started) result.detail = "Windows could not open the default browser";
    return result;
#else
    const bool remote_text_session =
        !platform::environment_value("SSH_CONNECTION").empty() ||
        !platform::environment_value("SSH_CLIENT").empty() ||
        !platform::environment_value("SSH_TTY").empty();
#if defined(__APPLE__)
    const char* executable = "/usr/bin/open";
    const bool graphical_environment = !remote_text_session;
#else
    const char* executable = "/usr/bin/xdg-open";
    if (::access(executable, X_OK) != 0) executable = "/usr/local/bin/xdg-open";
    const bool graphical_environment =
        !platform::environment_value("DISPLAY").empty() ||
        !platform::environment_value("WAYLAND_DISPLAY").empty();
#endif
    if (remote_text_session && !graphical_environment) {
        result.detail = "browser launch skipped in a remote text session";
        return result;
    }
    if (!graphical_environment) {
        result.detail = "browser launch skipped because no graphical session was detected";
        return result;
    }
    if (::access(executable, X_OK) != 0) {
        result.detail = std::string("browser launcher is unavailable: ") + executable;
        return result;
    }

    result.attempted = true;
    const pid_t child = ::fork();
    if (child < 0) {
        result.detail = std::string("could not fork browser launcher: ") + std::strerror(errno);
        return result;
    }
    if (child == 0) {
        const pid_t grandchild = ::fork();
        if (grandchild < 0) _exit(127);
        if (grandchild > 0) _exit(0);
        (void)::setsid();
        const int null_fd = ::open("/dev/null", O_RDWR);
        if (null_fd >= 0) {
            (void)::dup2(null_fd, STDIN_FILENO);
            (void)::dup2(null_fd, STDOUT_FILENO);
            (void)::dup2(null_fd, STDERR_FILENO);
            if (null_fd > STDERR_FILENO) (void)::close(null_fd);
        }
        ::execl(executable, executable, url.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }
    int status = 0;
    pid_t waited;
    do {
        waited = ::waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    result.started = waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    if (!result.started) result.detail = "could not start the browser launcher";
    return result;
#endif
}

}  // namespace ainiux::server
