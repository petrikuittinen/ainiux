#pragma once

#include <string>

#include "ainiux/video_setting.hpp"

namespace ainiux::config {

bool parse_video_protocol(const std::string& text, VideoProtocol& protocol);
const char* video_protocol_name(VideoProtocol protocol);
bool parse_video_input_mode(const std::string& text, VideoInputMode& mode);
const char* video_input_mode_name(VideoInputMode mode);
const VideoCapability* resolve_video_capability(const VideoCatalog& catalog,
                                                const std::string& provider,
                                                const std::string& model);
std::string default_video_model(const VideoCatalog& catalog, const std::string& provider);
std::string known_video_models_description(const VideoCatalog& catalog,
                                           const std::string& provider);

}  // namespace ainiux::config
