#pragma once

#include <string>

#include "ainiux/image_setting.hpp"

namespace ainiux::server {

// Safe, browser-facing projection of the effective layered images.conf catalog.
std::string public_image_catalog_json(const ImageCatalog& catalog,
                                      const std::string& configured_provider);

}  // namespace ainiux::server
