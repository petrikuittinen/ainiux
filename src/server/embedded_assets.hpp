#pragma once

#include <string>
#include <string_view>

namespace ainiux::server {

struct EmbeddedAsset {
    std::string_view content_type;
    std::string_view content;
    bool immutable = false;
};

bool is_web_ui_path(std::string_view path);
bool find_embedded_asset(std::string_view path, EmbeddedAsset& asset);

}  // namespace ainiux::server
