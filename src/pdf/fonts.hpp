#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace ainiux::pdf {

enum class BaseFont {
    Regular = 0,
    Bold,
    Italic,
    BoldItalic,
    Mono,
    Count
};

void fill_win_ansi(int encoding[256]);

extern const short kHelveticaAscii[95];
extern const short kTimesAscii[95];

const char* base_font_name(BaseFont font);
double glyph_width_em(BaseFont font, unsigned char win_ansi);
double text_width(BaseFont font, std::string_view win_ansi, double size);
std::string utf8_to_win_ansi(std::string_view utf8, std::size_t& substituted);

}  // namespace ainiux::pdf
