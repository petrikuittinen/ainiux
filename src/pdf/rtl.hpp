#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace ainiux::pdf {

bool is_hebrew_cp(unsigned cp);
bool is_arabic_cp(unsigned cp);
bool is_rtl_letter(unsigned cp);
bool utf8_has_rtl(std::string_view utf8);

void decode_utf8(std::string_view utf8, std::vector<unsigned>& cps);
std::string encode_utf8(const std::vector<unsigned>& cps);

// Map logical Arabic letters to Presentation Forms-B (and Lam-Alef ligatures).
// Hebrew, Latin, and already-shaped forms are left unchanged.
void shape_arabic(std::vector<unsigned>& cps);
std::string shape_utf8(std::string_view utf8);

unsigned mirror_cp(unsigned cp);
void reverse_grapheme_clusters(std::vector<unsigned>& cps);

// Paragraph is treated as RTL. LTR runs (Latin, digits, URLs) keep internal order.
std::vector<unsigned> visual_order_rtl(const std::vector<unsigned>& logical);

}  // namespace ainiux::pdf
