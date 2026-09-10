#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "common.hpp"
#include "pdf/write.hpp"

namespace ainiux::pdf {

struct TtfInfo {
    std::uint16_t units_per_em = 1000;
    std::int16_t x_min = 0;
    std::int16_t y_min = 0;
    std::int16_t x_max = 0;
    std::int16_t y_max = 0;
    std::int16_t ascent = 800;
    std::int16_t descent = -200;
    std::int16_t cap_height = 700;
    bool fixed_pitch = false;
    std::string postscript_name = "Embedded";
};

class TrueTypeFont {
   public:
    Error load_file(const std::string& path);
    Error load_bytes(std::string bytes, int face_index = -1);

    bool has_glyf() const { return has_glyf_; }
    const TtfInfo& info() const { return info_; }
    std::uint16_t glyph_id(unsigned cp) const;
    std::uint16_t advance(std::uint16_t gid) const;
    double width_pt(unsigned cp, double size) const;

    Error subset(const std::vector<unsigned>& codepoints, std::string& ttf_out,
                 std::unordered_map<unsigned, std::uint16_t>& cid_of_cp,
                 std::vector<std::uint16_t>& cid_widths_1000) const;

   private:
    std::string data_;
    std::size_t face_off_ = 0;
    TtfInfo info_;
    bool has_glyf_ = false;
    std::uint16_t num_glyphs_ = 0;
    std::uint16_t number_of_hmetrics_ = 0;
    std::int16_t loca_format_ = 1;
    std::unordered_map<unsigned, std::uint16_t> cmap_;
    std::vector<std::uint16_t> advances_;
    std::vector<std::int16_t> lsbs_;
    std::uint32_t glyf_off_ = 0;
    std::uint32_t glyf_len_ = 0;
    std::uint32_t loca_off_ = 0;
    std::uint32_t loca_len_ = 0;
    std::string os2_;
    std::string maxp_rest_;

    Error parse_face(std::size_t offset);
};

std::string find_cjk_font_path(const std::string& explicit_path);
Error embed_cid_type0(DocumentWriter& writer, const std::string& subset_ttf, const TtfInfo& info,
                      const std::vector<std::uint16_t>& cid_widths_1000,
                      const std::unordered_map<unsigned, std::uint16_t>& cid_of_cp,
                      std::uint32_t& type0);
Error make_placeholder_cjk_ttf(std::string& out);

}  // namespace ainiux::pdf
