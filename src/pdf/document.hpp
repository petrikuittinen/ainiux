#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "common.hpp"
#include "pdf/pdf.hpp"
#include "pdf/value.hpp"

namespace ainiux::pdf {

struct Object {
    std::uint32_t number = 0;
    std::uint16_t generation = 0;
    std::size_t offset = 0;
    std::uint32_t objstm_number = 0;
    bool compressed = false;
    bool loaded = false;
    Value value;
    std::size_t stream_offset = 0;
    std::size_t stream_length = 0;
};

class Document {
   public:
    static Error open_file(const std::string& path, const Options& options, Document& out);
    static Error open_bytes(std::string bytes, const Options& options, Document& out);

    std::string_view version() const { return version_; }
    std::size_t object_count() const { return objects_.empty() ? 0 : objects_.size() - 1; }
    std::size_t page_count() const { return pages_.size(); }
    const Value* trailer() const { return trailer_.type == ValueType::Dict ? &trailer_ : nullptr; }

    Error load_object(std::uint32_t number);
    const Value* object_dict(std::uint32_t number);
    const Value* object_value(std::uint32_t number);
    const Value* page_dictionary(std::size_t index);
    const Value* page_inherited(std::size_t index, const char* key);
    Error page_content(std::size_t index, std::string& decoded);
    Error decode_stream(std::uint32_t number, std::string& decoded);

   private:
    Error open_loaded(const Options& options);
    Error load_xref(std::size_t xref_offset, const Options& options);
    Error load_xref_table(Tokenizer& tokens, const Options& options, Value& this_trailer);
    Error load_xref_stream(std::size_t object_offset, const Options& options, Value& this_trailer);
    Error repair_xref(const Options& options);
    Error load_pages(std::uint32_t number, int depth, const Options& options);
    Error load_obj_stream(std::uint32_t number, const Options& options);
    Error parse_object_at(std::size_t offset, Object& object);
    Error decode_object_stream(Object& object, std::string& decoded);
    bool add_placeholder(std::uint32_t number, std::uint16_t generation, std::size_t offset, bool compressed,
                         std::uint32_t objstm_number);
    Object* find_object(std::uint32_t number);

    std::string bytes_;
    std::string version_;
    Value trailer_;
    std::vector<Object> objects_;
    std::vector<std::uint32_t> pages_;
    std::vector<std::uint32_t> obj_streams_;
    bool cancelled(const Options& options) const;
};

}  // namespace ainiux::pdf
