#include "pdf/fonts.hpp"

#include "pdf/rtl.hpp"

#include <cstdint>

namespace ainiux::pdf {
namespace {

const int kWinAnsi[32] = {
    0x20AC, 0x0000, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x0000, 0x017D, 0x0000,
    0x0000, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x0000, 0x017E, 0x0178,
};

// Adobe AFM WinAnsi widths for 32..255 (controls 0..31 are unused).
const short kHelvetica[224] = {
    278, 278, 355, 556, 556, 889, 667, 191, 333, 333, 389, 584, 278, 333, 278, 278,
    556, 556, 556, 556, 556, 556, 556, 556, 556, 556, 278, 278, 584, 584, 584, 556,
    1015, 667, 667, 722, 722, 667, 611, 778, 722, 278, 500, 667, 556, 833, 722, 778,
    667, 778, 722, 667, 611, 722, 667, 944, 667, 667, 611, 278, 278, 278, 469, 556,
    333, 556, 556, 500, 556, 556, 278, 556, 556, 222, 222, 500, 222, 833, 556, 556,
    556, 556, 333, 500, 278, 556, 500, 722, 500, 500, 500, 334, 260, 334, 584, 0,
    556, 0, 222, 556, 333, 1000, 556, 556, 333, 1000, 667, 333, 1000, 0, 611, 0,
    0, 222, 221, 333, 333, 350, 556, 1000, 333, 1000, 500, 333, 944, 0, 500, 667,
    0, 333, 556, 556, 556, 556, 260, 556, 333, 737, 370, 556, 584, 584, 737, 333,
    606, 584, 351, 351, 333, 556, 537, 278, 333, 351, 365, 556, 869, 869, 869, 611,
    667, 667, 667, 667, 667, 667, 1000, 722, 667, 667, 667, 667, 278, 278, 278, 278,
    722, 722, 778, 778, 778, 778, 778, 584, 778, 722, 722, 722, 722, 666, 666, 611,
    556, 556, 556, 556, 556, 556, 889, 500, 556, 556, 556, 556, 278, 278, 278, 278,
    556, 556, 556, 556, 556, 556, 556, 584, 611, 556, 556, 556, 556, 500, 555, 500
};

const short kHelveticaBold[224] = {
    278, 333, 474, 556, 556, 889, 722, 238, 333, 333, 389, 584, 278, 333, 278, 278,
    556, 556, 556, 556, 556, 556, 556, 556, 556, 556, 333, 333, 584, 584, 584, 611,
    975, 722, 722, 722, 722, 667, 611, 778, 722, 278, 556, 722, 611, 833, 722, 778,
    667, 778, 722, 667, 611, 722, 667, 944, 667, 667, 611, 333, 278, 333, 584, 556,
    333, 556, 611, 556, 611, 556, 333, 611, 611, 278, 278, 556, 278, 889, 611, 611,
    611, 611, 389, 556, 333, 611, 556, 778, 556, 556, 500, 389, 280, 389, 584, 0,
    556, 0, 278, 556, 500, 1000, 556, 556, 333, 1000, 667, 333, 1000, 0, 611, 0,
    0, 278, 278, 500, 500, 350, 556, 1000, 333, 1000, 556, 333, 944, 0, 500, 667,
    0, 333, 556, 556, 556, 556, 280, 556, 333, 737, 370, 556, 584, 584, 737, 333,
    606, 584, 351, 351, 333, 611, 556, 278, 333, 351, 365, 556, 869, 869, 869, 611,
    722, 722, 722, 722, 722, 722, 1000, 722, 667, 667, 667, 667, 278, 278, 278, 278,
    722, 722, 778, 778, 778, 778, 778, 584, 778, 722, 722, 722, 722, 667, 667, 611,
    556, 556, 556, 556, 556, 556, 889, 556, 556, 556, 556, 556, 278, 278, 278, 278,
    611, 611, 611, 611, 611, 611, 611, 584, 611, 611, 611, 611, 611, 556, 611, 556
};

unsigned decode_next(std::string_view text, std::size_t& i) {
    if (i >= text.size()) {
        return 0;
    }
    const unsigned char c0 = static_cast<unsigned char>(text[i]);
    if (c0 < 0x80) {
        ++i;
        return c0;
    }
    int need = 0;
    unsigned cp = 0;
    if ((c0 & 0xE0) == 0xC0) {
        need = 1;
        cp = c0 & 0x1F;
    } else if ((c0 & 0xF0) == 0xE0) {
        need = 2;
        cp = c0 & 0x0F;
    } else if ((c0 & 0xF8) == 0xF0) {
        need = 3;
        cp = c0 & 0x07;
    } else {
        ++i;
        return 0xFFFD;
    }
    ++i;
    for (int n = 0; n < need; ++n) {
        if (i >= text.size()) {
            return 0xFFFD;
        }
        const unsigned char cx = static_cast<unsigned char>(text[i]);
        if ((cx & 0xC0) != 0x80) {
            return 0xFFFD;
        }
        cp = (cp << 6) | (cx & 0x3F);
        ++i;
    }
    return cp;
}

int map_win_ansi(unsigned cp) {
    if (cp == 0x09 || cp == 0x0A || cp == 0x0D) {
        return ' ';
    }
    if (cp < 0x80 || (cp >= 0xA0 && cp <= 0xFF)) {
        return static_cast<int>(cp);
    }
    for (int i = 0; i < 32; ++i) {
        if (kWinAnsi[i] > 0 && static_cast<unsigned>(kWinAnsi[i]) == cp) {
            return 128 + i;
        }
    }
    if (cp == 0x2010 || cp == 0x2011) {
        return '-';
    }
    return -1;
}

const short* widths_for(BaseFont font) {
    switch (font) {
        case BaseFont::Bold:
        case BaseFont::BoldItalic:
            return kHelveticaBold;
        case BaseFont::Italic:
            return kHelvetica;
        case BaseFont::Mono:
            return nullptr;
        case BaseFont::Regular:
        default:
            return kHelvetica;
    }
}

}  // namespace

const short kHelveticaAscii[95] = {
    278, 278, 355, 556, 556, 889, 667, 191, 333, 333, 389, 584, 278, 333, 278, 278, 556, 556, 556, 556,
    556, 556, 556, 556, 556, 556, 278, 278, 584, 584, 584, 556, 1015, 667, 667, 722, 722, 667, 611, 778,
    722, 278, 500, 667, 556, 833, 722, 778, 667, 778, 722, 667, 611, 722, 667, 944, 667, 667, 611, 278,
    278, 278, 469, 556, 333, 556, 556, 500, 556, 556, 278, 556, 556, 222, 222, 500, 222, 833, 556, 556,
    556, 556, 333, 500, 278, 556, 500, 722, 500, 500, 500, 334, 260, 334, 584};

const short kTimesAscii[95] = {
    250, 333, 408, 500, 500, 833, 778, 180, 333, 333, 500, 564, 250, 333, 250, 278, 500, 500, 500, 500,
    500, 500, 500, 500, 500, 500, 278, 278, 564, 564, 564, 444, 921, 722, 667, 667, 722, 611, 556, 722,
    722, 333, 389, 722, 611, 889, 722, 722, 556, 722, 667, 556, 611, 722, 722, 944, 722, 722, 611, 333,
    278, 333, 469, 500, 333, 444, 500, 444, 500, 444, 333, 500, 500, 278, 278, 500, 278, 778, 500, 500,
    500, 500, 333, 389, 278, 500, 500, 722, 500, 500, 444, 480, 200, 480, 541};

void fill_win_ansi(int encoding[256]) {
    for (int i = 0; i < 256; ++i) {
        encoding[i] = i;
    }
    for (int i = 0; i < 32; ++i) {
        encoding[128 + i] = kWinAnsi[i];
    }
}

const char* base_font_name(BaseFont font) {
    switch (font) {
        case BaseFont::Bold:
            return "Helvetica-Bold";
        case BaseFont::Italic:
            return "Helvetica-Oblique";
        case BaseFont::BoldItalic:
            return "Helvetica-BoldOblique";
        case BaseFont::Mono:
            return "Courier";
        case BaseFont::Regular:
        default:
            return "Helvetica";
    }
}

double glyph_width_em(BaseFont font, unsigned char win_ansi) {
    if (font == BaseFont::Mono) {
        return 600;
    }
    if (win_ansi < 32) {
        return 0;
    }
    const short* table = widths_for(font);
    const short width = table[win_ansi - 32];
    if (width > 0) {
        return width;
    }
    return 556;
}

double text_width(BaseFont font, std::string_view win_ansi, double size) {
    double em = 0;
    for (unsigned char ch : win_ansi) {
        em += glyph_width_em(font, ch);
    }
    return em * size / 1000.0;
}

unsigned next_utf8(std::string_view utf8, std::size_t& i) {
    return decode_next(utf8, i);
}

int win_ansi_code(unsigned cp) {
    return map_win_ansi(cp);
}

bool is_cjk_cp(unsigned cp) {
    if (cp >= 0x2E80 && cp <= 0x9FFF) {
        return true;
    }
    if (cp >= 0xAC00 && cp <= 0xD7AF) {
        return true;
    }
    if (cp >= 0xF900 && cp <= 0xFAFF) {
        return true;
    }
    if (cp >= 0xFF00 && cp <= 0xFFEF) {
        return true;
    }
    if (cp >= 0x20000 && cp <= 0x2FA1F) {
        return true;
    }
    return cp >= 0x30000 && cp <= 0x323AF;
}

bool needs_embedded_cp(unsigned cp) {
    if (is_cjk_cp(cp) || is_hebrew_cp(cp) || is_arabic_cp(cp)) {
        return true;
    }
    if (cp >= 0x0100 && cp <= 0x024F) {
        return true;
    }
    return cp >= 0x1E00 && cp <= 0x1EFF;
}

std::string utf8_to_win_ansi(std::string_view utf8, std::size_t& substituted) {
    std::string out;
    out.reserve(utf8.size());
    std::size_t i = 0;
    while (i < utf8.size()) {
        const unsigned cp = decode_next(utf8, i);
        const int mapped = map_win_ansi(cp);
        if (mapped >= 0) {
            out.push_back(static_cast<char>(mapped));
        } else if (cp != 0) {
            out.push_back('?');
            ++substituted;
        }
    }
    return out;
}

}  // namespace ainiux::pdf
