#pragma once
#include <string>
#include "ainiux/video_setting.hpp"
namespace ainiux::server {
std::string public_video_catalog_json(const VideoCatalog& catalog,
                                      const std::string& configured_provider);
}
