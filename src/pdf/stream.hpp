#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "common.hpp"
#include "pdf/value.hpp"

namespace ainiux::pdf {

Error inflate_flate(const std::uint8_t* data, std::size_t size, std::string& out, std::size_t max_out);

Error apply_png_predictor(std::string& data,
                          std::size_t columns,
                          int colors,
                          int bits_per_component);

Error decode_stream_bytes(std::string_view raw,
                          const Value& dict,
                          std::string& out,
                          std::size_t max_out);

}  // namespace ainiux::pdf
