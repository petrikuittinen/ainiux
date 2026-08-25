#pragma once

#include <string>

#include "ainiux/image_setting.hpp"
#include "common.hpp"

namespace ainiux::config {

bool parse_image_protocol(const std::string& text, ImageProtocol& protocol);
const char* image_protocol_name(ImageProtocol protocol);
std::string image_protocol_names();
bool image_protocol_implemented(ImageProtocol protocol);

bool parse_image_size_mode(const std::string& text, ImageSizeMode& mode);
const char* image_size_mode_name(ImageSizeMode mode);

const ImageCapability* resolve_image_capability(const ImageCatalog& catalog,
                                                const std::string& provider,
                                                const std::string& model);
std::string default_image_model(const ImageCatalog& catalog, const std::string& provider);
std::string known_image_models_description(const ImageCatalog& catalog,
                                           const std::string& provider);

}  // namespace ainiux::config
