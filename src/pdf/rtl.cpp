#include "pdf/rtl.hpp"

#include "pdf/fonts.hpp"

namespace ainiux::pdf {
namespace {

enum Join : unsigned char { JoinU = 0, JoinR = 1, JoinD = 2, JoinC = 3, JoinT = 4 };

void append_utf8(std::string& out, unsigned cp) {
    if (cp == 0) {
        return;
    }
    if (cp <= 0x7F) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        if (cp >= 0xD800 && cp <= 0xDFFF) {
            return;
        }
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0x10FFFF) {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

bool is_combining_mark(unsigned cp) {
    if (cp >= 0x0591 && cp <= 0x05BD) {
        return true;
    }
    if (cp == 0x05BF || cp == 0x05C1 || cp == 0x05C2 || cp == 0x05C4 || cp == 0x05C5 || cp == 0x05C7) {
        return true;
    }
    if (cp >= 0x064B && cp <= 0x065F) {
        return true;
    }
    if (cp == 0x0670) {
        return true;
    }
    if (cp >= 0x06D6 && cp <= 0x06DC) {
        return true;
    }
    if (cp >= 0x06DF && cp <= 0x06E4) {
        return true;
    }
    if (cp == 0x06E7 || cp == 0x06E8) {
        return true;
    }
    if (cp >= 0x06EA && cp <= 0x06ED) {
        return true;
    }
    if (cp >= 0x08D3 && cp <= 0x08FF) {
        return true;
    }
    return cp >= 0xFE70 && cp <= 0xFE7F;
}

bool already_shaped(unsigned cp) {
    if (cp >= 0xFB50 && cp <= 0xFDFF) {
        return true;
    }
    return cp >= 0xFE70 && cp <= 0xFEFF;
}

Join join_type(unsigned cp) {
    if (already_shaped(cp)) {
        return JoinU;
    }
    if (is_combining_mark(cp)) {
        return JoinT;
    }
    if (cp == 0x200C) {
        return JoinU;
    }
    if (cp == 0x200D || cp == 0x0640) {
        return JoinC;
    }
    if (cp < 0x0621 || cp > 0x06D5) {
        if (cp >= 0x0750 && cp <= 0x077F) {
            return JoinD;
        }
        if (cp >= 0x08A0 && cp <= 0x08D2) {
            return JoinD;
        }
        return JoinU;
    }
    switch (cp) {
        case 0x0621:
            return JoinU;
        case 0x0622:
        case 0x0623:
        case 0x0624:
        case 0x0625:
        case 0x0627:
        case 0x0629:
        case 0x062F:
        case 0x0630:
        case 0x0631:
        case 0x0632:
        case 0x0648:
        case 0x0649:
        case 0x0671:
        case 0x0672:
        case 0x0673:
        case 0x0675:
        case 0x0688:
        case 0x0689:
        case 0x068A:
        case 0x068B:
        case 0x068C:
        case 0x068D:
        case 0x068E:
        case 0x068F:
        case 0x0690:
        case 0x0691:
        case 0x0692:
        case 0x0693:
        case 0x0694:
        case 0x0695:
        case 0x0696:
        case 0x0697:
        case 0x0698:
        case 0x0699:
        case 0x06C0:
        case 0x06D2:
        case 0x06D3:
        case 0x06D5:
            return JoinR;
        default:
            return JoinD;
    }
}

// isol, final, init, medi for U+0621..U+064A. Zero means that form does not exist.
const unsigned kBasicForms[42][4] = {
    {0xFE80, 0, 0, 0},          // 0621 hamza
    {0xFE81, 0xFE82, 0, 0},     // 0622 alef madda
    {0xFE83, 0xFE84, 0, 0},     // 0623 alef hamza
    {0xFE85, 0xFE86, 0, 0},     // 0624 waw hamza
    {0xFE87, 0xFE88, 0, 0},     // 0625 alef hamza below
    {0xFE89, 0xFE8A, 0xFE8B, 0xFE8C},  // 0626 yeh hamza
    {0xFE8D, 0xFE8E, 0, 0},     // 0627 alef
    {0xFE8F, 0xFE90, 0xFE91, 0xFE92},  // 0628 beh
    {0xFE93, 0xFE94, 0, 0},     // 0629 teh marbuta
    {0xFE95, 0xFE96, 0xFE97, 0xFE98},  // 062A teh
    {0xFE99, 0xFE9A, 0xFE9B, 0xFE9C},  // 062B theh
    {0xFE9D, 0xFE9E, 0xFE9F, 0xFEA0},  // 062C jeem
    {0xFEA1, 0xFEA2, 0xFEA3, 0xFEA4},  // 062D hah
    {0xFEA5, 0xFEA6, 0xFEA7, 0xFEA8},  // 062E khah
    {0xFEA9, 0xFEAA, 0, 0},     // 062F dal
    {0xFEAB, 0xFEAC, 0, 0},     // 0630 thal
    {0xFEAD, 0xFEAE, 0, 0},     // 0631 reh
    {0xFEAF, 0xFEB0, 0, 0},     // 0632 zain
    {0xFEB1, 0xFEB2, 0xFEB3, 0xFEB4},  // 0633 seen
    {0xFEB5, 0xFEB6, 0xFEB7, 0xFEB8},  // 0634 sheen
    {0xFEB9, 0xFEBA, 0xFEBB, 0xFEBC},  // 0635 sad
    {0xFEBD, 0xFEBE, 0xFEBF, 0xFEC0},  // 0636 dad
    {0xFEC1, 0xFEC2, 0xFEC3, 0xFEC4},  // 0637 tah
    {0xFEC5, 0xFEC6, 0xFEC7, 0xFEC8},  // 0638 zah
    {0xFEC9, 0xFECA, 0xFECB, 0xFECC},  // 0639 ain
    {0xFECD, 0xFECE, 0xFECF, 0xFED0},  // 063A ghain
    {0, 0, 0, 0},               // 063B
    {0, 0, 0, 0},               // 063C
    {0, 0, 0, 0},               // 063D
    {0, 0, 0, 0},               // 063E
    {0, 0, 0, 0},               // 063F
    {0, 0, 0, 0},               // 0640 tatweel
    {0xFED1, 0xFED2, 0xFED3, 0xFED4},  // 0641 feh
    {0xFED5, 0xFED6, 0xFED7, 0xFED8},  // 0642 qaf
    {0xFED9, 0xFEDA, 0xFEDB, 0xFEDC},  // 0643 kaf
    {0xFEDD, 0xFEDE, 0xFEDF, 0xFEE0},  // 0644 lam
    {0xFEE1, 0xFEE2, 0xFEE3, 0xFEE4},  // 0645 meem
    {0xFEE5, 0xFEE6, 0xFEE7, 0xFEE8},  // 0646 noon
    {0xFEE9, 0xFEEA, 0xFEEB, 0xFEEC},  // 0647 heh
    {0xFEED, 0xFEEE, 0, 0},     // 0648 waw
    {0xFEEF, 0xFEF0, 0, 0},     // 0649 alef maksura
    {0xFEF1, 0xFEF2, 0xFEF3, 0xFEF4},  // 064A yeh
};

unsigned presentation_form(unsigned cp, int form) {
    if (form < 0 || form > 3) {
        return 0;
    }
    if (cp >= 0x0621 && cp <= 0x064A) {
        const unsigned mapped = kBasicForms[cp - 0x0621][form];
        if (mapped != 0) {
            return mapped;
        }
        if (form >= 2) {
            const unsigned isol = kBasicForms[cp - 0x0621][0];
            return isol;
        }
        return kBasicForms[cp - 0x0621][0];
    }
    switch (cp) {
        case 0x067E: {  // peh
            const unsigned f[] = {0xFB56, 0xFB57, 0xFB58, 0xFB59};
            return f[form];
        }
        case 0x0686: {  // tcheh
            const unsigned f[] = {0xFB7A, 0xFB7B, 0xFB7C, 0xFB7D};
            return f[form];
        }
        case 0x0698: {  // jeh
            const unsigned f[] = {0xFB8A, 0xFB8B, 0, 0};
            return f[form] != 0 ? f[form] : f[0];
        }
        case 0x06A9: {  // keheh
            const unsigned f[] = {0xFB8E, 0xFB8F, 0xFB90, 0xFB91};
            return f[form];
        }
        case 0x06AF: {  // gaf
            const unsigned f[] = {0xFB92, 0xFB93, 0xFB94, 0xFB95};
            return f[form];
        }
        case 0x06CC: {  // farsi yeh
            const unsigned f[] = {0xFBFC, 0xFBFD, 0xFBFE, 0xFBFF};
            return f[form];
        }
        default:
            return 0;
    }
}

bool is_alef_ligature(unsigned cp) {
    return cp == 0x0622 || cp == 0x0623 || cp == 0x0625 || cp == 0x0627;
}

unsigned lam_alef_ligature(unsigned alef, bool joined_prev) {
    unsigned isol = 0;
    unsigned fin = 0;
    switch (alef) {
        case 0x0622:
            isol = 0xFEF5;
            fin = 0xFEF6;
            break;
        case 0x0623:
            isol = 0xFEF7;
            fin = 0xFEF8;
            break;
        case 0x0625:
            isol = 0xFEF9;
            fin = 0xFEFA;
            break;
        case 0x0627:
            isol = 0xFEFB;
            fin = 0xFEFC;
            break;
        default:
            return 0;
    }
    return joined_prev ? fin : isol;
}

bool joins_from_prev(Join t) {
    return t == JoinD || t == JoinC;
}

bool joins_to_prev(Join t) {
    return t == JoinD || t == JoinC || t == JoinR;
}

bool joins_to_next(Join t) {
    return t == JoinD || t == JoinC;
}

bool joins_from_next(Join t) {
    return t == JoinD || t == JoinC || t == JoinR;
}

enum Dir : unsigned char { DirL = 0, DirR = 1, DirN = 2 };

Dir bidi_dir(unsigned cp) {
    if (is_combining_mark(cp)) {
        return DirN;
    }
    if (is_rtl_letter(cp)) {
        return DirR;
    }
    if ((cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z')) {
        return DirL;
    }
    if (cp >= '0' && cp <= '9') {
        return DirL;
    }
    if (cp >= 0x0660 && cp <= 0x0669) {
        return DirL;
    }
    if (cp >= 0x06F0 && cp <= 0x06F9) {
        return DirL;
    }
    if (cp >= 0x00C0 && cp <= 0x024F) {
        return DirL;
    }
    if (cp == '/' || cp == ':' || cp == '.' || cp == '-' || cp == '_' || cp == '@' || cp == '%' || cp == '?' ||
        cp == '&' || cp == '=' || cp == '#' || cp == '+') {
        return DirN;
    }
    return DirN;
}

}  // namespace

bool is_hebrew_cp(unsigned cp) {
    if (cp >= 0x0590 && cp <= 0x05FF) {
        return true;
    }
    return cp >= 0xFB1D && cp <= 0xFB4F;
}

bool is_arabic_cp(unsigned cp) {
    if (cp >= 0x0600 && cp <= 0x06FF) {
        return true;
    }
    if (cp >= 0x0750 && cp <= 0x077F) {
        return true;
    }
    if (cp >= 0x08A0 && cp <= 0x08FF) {
        return true;
    }
    if (cp >= 0xFB50 && cp <= 0xFDFF) {
        return true;
    }
    return cp >= 0xFE70 && cp <= 0xFEFF;
}

bool is_rtl_letter(unsigned cp) {
    if (is_combining_mark(cp)) {
        return false;
    }
    if (cp >= 0x05D0 && cp <= 0x05F2) {
        return true;
    }
    if (cp >= 0xFB1D && cp <= 0xFB4F) {
        return true;
    }
    if (cp >= 0x0621 && cp <= 0x064A) {
        return true;
    }
    if (cp >= 0x066E && cp <= 0x06D5) {
        return true;
    }
    if (cp >= 0x0750 && cp <= 0x077F) {
        return true;
    }
    if (cp >= 0x08A0 && cp <= 0x08D2) {
        return true;
    }
    if (cp >= 0xFB50 && cp <= 0xFDFF) {
        return true;
    }
    if (cp >= 0xFE80 && cp <= 0xFEFC) {
        return true;
    }
    return cp >= 0x0600 && cp <= 0x06FF && !is_combining_mark(cp) && (cp < 0x0660 || cp > 0x0669);
}

bool utf8_has_rtl(std::string_view utf8) {
    std::size_t i = 0;
    while (i < utf8.size()) {
        const unsigned cp = next_utf8(utf8, i);
        if (is_rtl_letter(cp)) {
            return true;
        }
    }
    return false;
}

void decode_utf8(std::string_view utf8, std::vector<unsigned>& cps) {
    cps.clear();
    std::size_t i = 0;
    while (i < utf8.size()) {
        const unsigned cp = next_utf8(utf8, i);
        if (cp != 0) {
            cps.push_back(cp);
        }
    }
}

std::string encode_utf8(const std::vector<unsigned>& cps) {
    std::string out;
    out.reserve(cps.size() * 3);
    for (unsigned cp : cps) {
        append_utf8(out, cp);
    }
    return out;
}

void shape_arabic(std::vector<unsigned>& cps) {
    const std::size_t n = cps.size();
    if (n == 0) {
        return;
    }
    std::vector<Join> jt(n);
    bool any_arabic = false;
    for (std::size_t i = 0; i < n; ++i) {
        jt[i] = join_type(cps[i]);
        if (cps[i] >= 0x0621 && cps[i] <= 0x06D5 && !already_shaped(cps[i])) {
            any_arabic = true;
        }
    }
    if (!any_arabic) {
        return;
    }
    auto prev_offers_join = [&](std::size_t i) -> bool {
        std::size_t k = i;
        while (k > 0) {
            --k;
            if (jt[k] == JoinT) {
                continue;
            }
            return joins_from_prev(jt[k]);
        }
        return false;
    };
    auto next_offers_join = [&](std::size_t i) -> bool {
        for (std::size_t k = i + 1; k < n; ++k) {
            if (jt[k] == JoinT) {
                continue;
            }
            return joins_from_next(jt[k]);
        }
        return false;
    };
    std::vector<int> form(n, 0);
    for (std::size_t i = 0; i < n; ++i) {
        if (jt[i] == JoinT || jt[i] == JoinU) {
            continue;
        }
        const bool jr = joins_to_prev(jt[i]) && prev_offers_join(i);
        const bool jl = joins_to_next(jt[i]) && next_offers_join(i);
        if (jr && jl) {
            form[i] = 3;
        } else if (jr) {
            form[i] = 1;
        } else if (jl) {
            form[i] = 2;
        }
    }
    std::vector<unsigned> out;
    out.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        if (jt[i] == JoinT) {
            out.push_back(cps[i]);
            continue;
        }
        if (cps[i] == 0x0644 && (form[i] == 2 || form[i] == 3)) {
            std::size_t k = i + 1;
            while (k < n && jt[k] == JoinT) {
                ++k;
            }
            if (k < n && is_alef_ligature(cps[k])) {
                const unsigned lig = lam_alef_ligature(cps[k], form[i] == 3);
                if (lig != 0) {
                    out.push_back(lig);
                    for (std::size_t m = i + 1; m < k; ++m) {
                        out.push_back(cps[m]);
                    }
                    i = k;
                    continue;
                }
            }
        }
        const unsigned mapped = presentation_form(cps[i], form[i]);
        out.push_back(mapped != 0 ? mapped : cps[i]);
    }
    cps = std::move(out);
}

std::string shape_utf8(std::string_view utf8) {
    std::vector<unsigned> cps;
    decode_utf8(utf8, cps);
    shape_arabic(cps);
    return encode_utf8(cps);
}

unsigned mirror_cp(unsigned cp) {
    switch (cp) {
        case '(':
            return ')';
        case ')':
            return '(';
        case '[':
            return ']';
        case ']':
            return '[';
        case '{':
            return '}';
        case '}':
            return '{';
        case '<':
            return '>';
        case '>':
            return '<';
        default:
            return cp;
    }
}

void reverse_grapheme_clusters(std::vector<unsigned>& cps) {
    if (cps.size() < 2) {
        return;
    }
    std::vector<std::vector<unsigned>> clusters;
    for (unsigned cp : cps) {
        if (is_combining_mark(cp) && !clusters.empty()) {
            clusters.back().push_back(cp);
        } else {
            clusters.push_back({cp});
        }
    }
    cps.clear();
    for (auto it = clusters.rbegin(); it != clusters.rend(); ++it) {
        cps.insert(cps.end(), it->begin(), it->end());
    }
}

std::vector<unsigned> visual_order_rtl(const std::vector<unsigned>& logical) {
    const std::size_t n = logical.size();
    if (n == 0) {
        return {};
    }
    std::vector<Dir> cls(n, DirN);
    for (std::size_t i = 0; i < n; ++i) {
        if (is_combining_mark(logical[i]) && i > 0) {
            cls[i] = cls[i - 1];
        } else {
            cls[i] = bidi_dir(logical[i]);
        }
    }
    for (std::size_t i = 0; i < n; ++i) {
        if (cls[i] != DirN) {
            continue;
        }
        Dir prev = DirR;
        Dir next = DirR;
        std::size_t p = i;
        while (p > 0) {
            --p;
            if (cls[p] != DirN) {
                prev = cls[p];
                break;
            }
        }
        std::size_t q = i + 1;
        while (q < n && cls[q] == DirN) {
            ++q;
        }
        if (q < n) {
            next = cls[q];
        }
        cls[i] = (prev == next) ? prev : DirR;
    }
    struct Run {
        std::size_t begin = 0;
        std::size_t end = 0;
        Dir dir = DirL;
    };
    std::vector<Run> runs;
    std::size_t i = 0;
    while (i < n) {
        Run run;
        run.begin = i;
        run.dir = cls[i];
        ++i;
        while (i < n && cls[i] == run.dir) {
            ++i;
        }
        run.end = i;
        runs.push_back(run);
    }
    std::vector<unsigned> out;
    out.reserve(n);
    for (auto it = runs.rbegin(); it != runs.rend(); ++it) {
        std::vector<unsigned> slice(logical.begin() + static_cast<std::ptrdiff_t>(it->begin),
                                    logical.begin() + static_cast<std::ptrdiff_t>(it->end));
        if (it->dir == DirR) {
            for (unsigned& cp : slice) {
                cp = mirror_cp(cp);
            }
            reverse_grapheme_clusters(slice);
        }
        out.insert(out.end(), slice.begin(), slice.end());
    }
    return out;
}

}  // namespace ainiux::pdf
