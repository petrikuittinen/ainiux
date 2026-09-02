#pragma once

#include <string>
#include <vector>

namespace ainiux::server {

struct BrowserLaunchResult {
    bool attempted = false;
    bool started = false;
    std::string detail;
};

std::vector<std::string> local_ipv4_addresses();
std::vector<std::string> web_ui_urls(const std::string& bind_address,
                                     unsigned short port,
                                     bool tls,
                                     const std::vector<std::string>& local_addresses);
BrowserLaunchResult launch_browser_best_effort(const std::string& url);

}  // namespace ainiux::server
