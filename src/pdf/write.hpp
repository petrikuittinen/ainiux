#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "common.hpp"

namespace ainiux::pdf {

class DocumentWriter {
   public:
    DocumentWriter();

    std::uint32_t add_object(std::string body);
    std::uint32_t reserve_object();
    void set_object(std::uint32_t number, std::string body);
    Error add_stream(std::string extra_keys, std::string_view raw, bool flate, std::uint32_t& number);
    std::string finish(std::uint32_t root, std::uint32_t info) const;
    std::uint32_t object_count() const;

   private:
    std::vector<std::string> bodies_;
};

std::string pdf_escape_string(std::string_view text);
std::string pdf_number(double value);
std::string pdf_creation_date();

}  // namespace ainiux::pdf
