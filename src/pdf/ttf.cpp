#include "pdf/ttf.hpp"

#include "platform/environment.hpp"
#include "platform/filesystem.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <unordered_set>

namespace ainiux::pdf {
namespace {

constexpr std::size_t kMaxFontBytes = 32 * 1024 * 1024;

std::uint16_t ru16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>((p[0] << 8) | p[1]);
}

std::uint32_t ru32(const std::uint8_t* p) {
    return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) | static_cast<std::uint32_t>(p[3]);
}

std::int16_t ri16(const std::uint8_t* p) {
    return static_cast<std::int16_t>(ru16(p));
}

void wu16(std::string& out, std::uint16_t v) {
    out.push_back(static_cast<char>(v >> 8));
    out.push_back(static_cast<char>(v & 0xFF));
}

void wu32(std::string& out, std::uint32_t v) {
    out.push_back(static_cast<char>(v >> 24));
    out.push_back(static_cast<char>((v >> 16) & 0xFF));
    out.push_back(static_cast<char>((v >> 8) & 0xFF));
    out.push_back(static_cast<char>(v & 0xFF));
}

void wi16(std::string& out, std::int16_t v) {
    wu16(out, static_cast<std::uint16_t>(v));
}

bool in_range(const std::string& data, std::size_t off, std::size_t n) {
    return n <= data.size() && off <= data.size() - n;
}

std::uint32_t checksum(const std::string& bytes) {
    std::uint32_t sum = 0;
    const std::size_t n = (bytes.size() + 3) & ~std::size_t{3};
    for (std::size_t i = 0; i < n; i += 4) {
        std::uint32_t w = 0;
        for (int b = 0; b < 4; ++b) {
            w <<= 8;
            if (i + static_cast<std::size_t>(b) < bytes.size()) {
                w |= static_cast<unsigned char>(bytes[i + static_cast<std::size_t>(b)]);
            }
        }
        sum += w;
    }
    return sum;
}

struct TableDir {
    char tag[4]{};
    std::uint32_t offset = 0;
    std::uint32_t length = 0;
};

bool find_table(const std::string& data, std::size_t face, const char* tag, TableDir& out) {
    if (!in_range(data, face + 12, 4)) {
        return false;
    }
    const auto* p = reinterpret_cast<const std::uint8_t*>(data.data());
    const std::uint16_t n = ru16(p + face + 4);
    if (!in_range(data, face + 12, static_cast<std::size_t>(n) * 16)) {
        return false;
    }
    for (std::uint16_t i = 0; i < n; ++i) {
        const std::size_t rec = face + 12 + static_cast<std::size_t>(i) * 16;
        if (std::memcmp(data.data() + rec, tag, 4) == 0) {
            out.offset = ru32(p + rec + 8);
            out.length = ru32(p + rec + 12);
            std::memcpy(out.tag, tag, 4);
            return in_range(data, out.offset, out.length);
        }
    }
    return false;
}

void parse_cmap(const std::string& data, const TableDir& table, std::unordered_map<unsigned, std::uint16_t>& cmap) {
    const auto* p = reinterpret_cast<const std::uint8_t*>(data.data()) + table.offset;
    if (table.length < 4) {
        return;
    }
    const std::uint16_t nsub = ru16(p + 2);
    struct Enc {
        std::uint16_t plat;
        std::uint16_t enc;
        std::uint32_t off;
    };
    std::vector<Enc> encs;
    if (table.length < 4u + static_cast<std::uint32_t>(nsub) * 8u) {
        return;
    }
    for (std::uint16_t i = 0; i < nsub; ++i) {
        Enc e;
        e.plat = ru16(p + 4 + i * 8);
        e.enc = ru16(p + 6 + i * 8);
        e.off = ru32(p + 8 + i * 8);
        encs.push_back(e);
    }
    auto parse_fmt4 = [&](const std::uint8_t* s, std::uint32_t avail) {
        if (avail < 16) {
            return;
        }
        const std::uint16_t segsx2 = ru16(s + 6);
        const std::uint16_t segs = segsx2 / 2;
        const std::size_t need = 16u + static_cast<std::size_t>(segs) * 8u;
        if (avail < need) {
            return;
        }
        const std::uint8_t* endc = s + 14;
        const std::uint8_t* startc = endc + segsx2 + 2;
        const std::uint8_t* delta = startc + segsx2;
        const std::uint8_t* range = delta + segsx2;
        const std::uint8_t* gids = range + segsx2;
        for (std::uint16_t i = 0; i < segs; ++i) {
            const unsigned end = ru16(endc + i * 2);
            const unsigned start = ru16(startc + i * 2);
            const std::int16_t d = ri16(delta + i * 2);
            const std::uint16_t ro = ru16(range + i * 2);
            for (unsigned c = start; c <= end; ++c) {
                std::uint16_t gid = 0;
                if (ro == 0) {
                    gid = static_cast<std::uint16_t>((static_cast<int>(c) + d) & 0xFFFF);
                } else {
                    const std::ptrdiff_t gpos =
                        (range + i * 2 + ro) - p + static_cast<std::ptrdiff_t>((c - start) * 2);
                    if (gpos < 0 || static_cast<std::uint32_t>(gpos) + 1 >= table.length) {
                        continue;
                    }
                    const std::uint16_t raw = ru16(p + gpos);
                    if (raw == 0) {
                        continue;
                    }
                    gid = static_cast<std::uint16_t>((static_cast<int>(raw) + d) & 0xFFFF);
                }
                if (gid != 0) {
                    cmap[c] = gid;
                }
                if (c == 0xFFFF) {
                    break;
                }
            }
        }
    };
    auto parse_fmt12 = [&](const std::uint8_t* s, std::uint32_t avail) {
        if (avail < 16) {
            return;
        }
        const std::uint32_t ng = ru32(s + 12);
        if (avail < 16 + ng * 12) {
            return;
        }
        for (std::uint32_t i = 0; i < ng; ++i) {
            const unsigned start = ru32(s + 16 + i * 12);
            const unsigned end = ru32(s + 20 + i * 12);
            std::uint32_t gid = ru32(s + 24 + i * 12);
            for (unsigned c = start; c <= end; ++c) {
                if (gid > 0 && gid < 65536) {
                    cmap[c] = static_cast<std::uint16_t>(gid);
                }
                ++gid;
                if (c == 0xFFFFFFFFu) {
                    break;
                }
            }
        }
    };
    auto try_off = [&](std::uint32_t off) {
        if (off >= table.length) {
            return;
        }
        const std::uint8_t* s = p + off;
        const std::uint32_t avail = table.length - off;
        if (avail < 2) {
            return;
        }
        const std::uint16_t fmt = ru16(s);
        if (fmt == 4) {
            parse_fmt4(s, avail);
        } else if (fmt == 12 && avail >= 4) {
            parse_fmt12(s, avail);
        }
    };
    for (const Enc& e : encs) {
        if ((e.plat == 3 && (e.enc == 1 || e.enc == 10)) || (e.plat == 0)) {
            try_off(e.off);
        }
    }
    if (cmap.empty()) {
        for (const Enc& e : encs) {
            try_off(e.off);
        }
    }
}

std::string wrap_sfnt(std::vector<std::pair<std::string, std::string>> tables) {
    std::sort(tables.begin(), tables.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    const std::uint16_t n = static_cast<std::uint16_t>(tables.size());
    std::uint16_t pow2 = 1;
    std::uint16_t log2 = 0;
    while (static_cast<std::uint16_t>(pow2 << 1) <= n) {
        pow2 = static_cast<std::uint16_t>(pow2 << 1);
        ++log2;
    }
    std::string out;
    wu32(out, 0x00010000);
    wu16(out, n);
    wu16(out, static_cast<std::uint16_t>(pow2 * 16));
    wu16(out, log2);
    wu16(out, static_cast<std::uint16_t>((n - pow2) * 16));
    const std::size_t dir = out.size();
    out.append(static_cast<std::size_t>(n) * 16, '\0');
    for (std::uint16_t i = 0; i < n; ++i) {
        while (out.size() % 4 != 0) {
            out.push_back('\0');
        }
        auto& body = tables[i].second;
        const std::uint32_t off = static_cast<std::uint32_t>(out.size());
        const std::uint32_t len = static_cast<std::uint32_t>(body.size());
        const std::uint32_t sum = checksum(body);
        char* rec = &out[dir + static_cast<std::size_t>(i) * 16];
        std::memcpy(rec, tables[i].first.data(), 4);
        rec[4] = static_cast<char>(sum >> 24);
        rec[5] = static_cast<char>((sum >> 16) & 0xFF);
        rec[6] = static_cast<char>((sum >> 8) & 0xFF);
        rec[7] = static_cast<char>(sum & 0xFF);
        rec[8] = static_cast<char>(off >> 24);
        rec[9] = static_cast<char>((off >> 16) & 0xFF);
        rec[10] = static_cast<char>((off >> 8) & 0xFF);
        rec[11] = static_cast<char>(off & 0xFF);
        rec[12] = static_cast<char>(len >> 24);
        rec[13] = static_cast<char>((len >> 16) & 0xFF);
        rec[14] = static_cast<char>((len >> 8) & 0xFF);
        rec[15] = static_cast<char>(len & 0xFF);
        out += body;
    }
    while (out.size() % 4 != 0) {
        out.push_back('\0');
    }
    std::uint32_t total = checksum(out);
    std::int32_t adj = static_cast<std::int32_t>(0xB1B0AFBAu - total);
    for (std::uint16_t i = 0; i < n; ++i) {
        if (tables[i].first == "head" && tables[i].second.size() >= 12) {
            const std::uint32_t off = ru32(reinterpret_cast<const std::uint8_t*>(&out[dir + i * 16 + 8]));
            if (off + 11 < out.size()) {
                out[off + 8] = static_cast<char>(static_cast<std::uint32_t>(adj) >> 24);
                out[off + 9] = static_cast<char>((static_cast<std::uint32_t>(adj) >> 16) & 0xFF);
                out[off + 10] = static_cast<char>((static_cast<std::uint32_t>(adj) >> 8) & 0xFF);
                out[off + 11] = static_cast<char>(static_cast<std::uint32_t>(adj) & 0xFF);
            }
            break;
        }
    }
    return out;
}

std::string name_table(const std::string& ps) {
    std::string utf16;
    for (unsigned char ch : ps) {
        utf16.push_back('\0');
        utf16.push_back(static_cast<char>(ch));
    }
    std::string out;
    wu16(out, 0);
    wu16(out, 1);
    wu16(out, 18);
    wu16(out, 3);
    wu16(out, 1);
    wu16(out, 0x0409);
    wu16(out, 6);
    wu16(out, static_cast<std::uint16_t>(utf16.size()));
    wu16(out, 0);
    out += utf16;
    return out;
}

std::string post_table() {
    std::string out;
    wu32(out, 0x00030000);
    wu32(out, 0);
    wu16(out, 0);
    wu16(out, 0);
    wu32(out, 0);
    wu32(out, 0);
    wu32(out, 0);
    wu32(out, 0);
    wu32(out, 0);
    return out;
}

std::string make_head(const TtfInfo& info, std::uint16_t loca_fmt) {
    std::string out(54, '\0');
    auto* p = reinterpret_cast<std::uint8_t*>(&out[0]);
    auto put32 = [&](std::size_t o, std::uint32_t v) {
        p[o] = static_cast<std::uint8_t>(v >> 24);
        p[o + 1] = static_cast<std::uint8_t>((v >> 16) & 0xFF);
        p[o + 2] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
        p[o + 3] = static_cast<std::uint8_t>(v & 0xFF);
    };
    auto put16 = [&](std::size_t o, std::uint16_t v) {
        p[o] = static_cast<std::uint8_t>(v >> 8);
        p[o + 1] = static_cast<std::uint8_t>(v & 0xFF);
    };
    put32(0, 0x00010000);
    put32(4, 0x00010000);
    put32(8, 0);
    put32(12, 0x5F0F3CF5);
    put16(16, 0x081B);
    put16(18, info.units_per_em);
    put16(36, static_cast<std::uint16_t>(info.x_min));
    put16(38, static_cast<std::uint16_t>(info.y_min));
    put16(40, static_cast<std::uint16_t>(info.x_max));
    put16(42, static_cast<std::uint16_t>(info.y_max));
    put16(44, 0);
    put16(46, 0);
    put16(48, 0);
    put16(50, loca_fmt);
    put16(52, 0);
    return out;
}

std::string make_hhea(const TtfInfo& info, std::uint16_t ng) {
    std::string out(36, '\0');
    auto* p = reinterpret_cast<std::uint8_t*>(&out[0]);
    auto put16 = [&](std::size_t o, std::uint16_t v) {
        p[o] = static_cast<std::uint8_t>(v >> 8);
        p[o + 1] = static_cast<std::uint8_t>(v & 0xFF);
    };
    p[0] = 0;
    p[1] = 1;
    put16(4, static_cast<std::uint16_t>(info.ascent));
    put16(6, static_cast<std::uint16_t>(info.descent));
    put16(34, ng);
    return out;
}

std::string make_maxp(std::uint16_t ng, const std::string& rest) {
    std::string out;
    wu32(out, 0x00010000);
    wu16(out, ng);
    if (rest.size() >= 6) {
        out.append(rest.data() + 6, rest.size() - 6);
    } else {
        out.append(26, '\0');
    }
    if (out.size() < 32) {
        out.append(32 - out.size(), '\0');
    }
    return out;
}

std::string cmap_from_map(const std::unordered_map<unsigned, std::uint16_t>& cmap) {
    std::vector<std::pair<unsigned, std::uint16_t>> bmp;
    std::vector<std::pair<unsigned, std::uint16_t>> all;
    for (const auto& kv : cmap) {
        all.push_back(kv);
        if (kv.first <= 0xFFFF) {
            bmp.push_back(kv);
        }
    }
    std::sort(bmp.begin(), bmp.end());
    std::sort(all.begin(), all.end());
    std::string fmt4;
    const std::uint16_t segs = static_cast<std::uint16_t>(bmp.size() + 1);
    wu16(fmt4, 4);
    const std::uint16_t len = static_cast<std::uint16_t>(16 + segs * 8);
    wu16(fmt4, len);
    wu16(fmt4, 0);
    wu16(fmt4, static_cast<std::uint16_t>(segs * 2));
    std::uint16_t pow2 = 1;
    std::uint16_t log2 = 0;
    while (static_cast<std::uint16_t>(pow2 << 1) <= segs) {
        pow2 = static_cast<std::uint16_t>(pow2 << 1);
        ++log2;
    }
    wu16(fmt4, static_cast<std::uint16_t>(pow2 * 2));
    wu16(fmt4, log2);
    wu16(fmt4, static_cast<std::uint16_t>((segs - pow2) * 2));
    for (const auto& kv : bmp) {
        wu16(fmt4, static_cast<std::uint16_t>(kv.first));
    }
    wu16(fmt4, 0xFFFF);
    wu16(fmt4, 0);
    for (const auto& kv : bmp) {
        wu16(fmt4, static_cast<std::uint16_t>(kv.first));
    }
    wu16(fmt4, 0xFFFF);
    for (const auto& kv : bmp) {
        const int d = static_cast<int>(kv.second) - static_cast<int>(kv.first);
        wi16(fmt4, static_cast<std::int16_t>(d));
    }
    wi16(fmt4, 1);
    for (std::uint16_t i = 0; i < segs; ++i) {
        wu16(fmt4, 0);
    }

    std::string out;
    const bool need12 = all.size() != bmp.size();
    const std::uint16_t nenc = need12 ? 2 : 1;
    wu16(out, 0);
    wu16(out, nenc);
    wu16(out, 3);
    wu16(out, 1);
    wu32(out, 4u + static_cast<std::uint32_t>(nenc) * 8u);
    std::string fmt12;
    if (need12) {
        wu16(fmt12, 12);
        wu16(fmt12, 0);
        wu32(fmt12, 16 + static_cast<std::uint32_t>(all.size()) * 12u);
        wu32(fmt12, 0);
        wu32(fmt12, static_cast<std::uint32_t>(all.size()));
        for (const auto& kv : all) {
            wu32(fmt12, kv.first);
            wu32(fmt12, kv.first);
            wu32(fmt12, kv.second);
        }
        wu16(out, 3);
        wu16(out, 10);
        wu32(out, 4u + static_cast<std::uint32_t>(nenc) * 8u + static_cast<std::uint32_t>(fmt4.size()));
    }
    out += fmt4;
    out += fmt12;
    return out;
}

std::uint32_t glyph_offset(const std::string& data, std::uint32_t loca_off, std::int16_t loca_fmt,
                           std::uint16_t gid) {
    const auto* p = reinterpret_cast<const std::uint8_t*>(data.data()) + loca_off;
    if (loca_fmt == 0) {
        return static_cast<std::uint32_t>(ru16(p + gid * 2)) * 2u;
    }
    return ru32(p + gid * 4);
}

void collect_components(const std::string& data, std::uint32_t glyf_off, std::uint32_t glyf_len,
                        std::uint32_t loca_off, std::int16_t loca_fmt, std::uint16_t gid,
                        std::uint16_t num_glyphs, std::unordered_set<std::uint16_t>& used) {
    if (gid >= num_glyphs || !used.insert(gid).second) {
        return;
    }
    const std::uint32_t a = glyph_offset(data, loca_off, loca_fmt, gid);
    const std::uint32_t b = glyph_offset(data, loca_off, loca_fmt, static_cast<std::uint16_t>(gid + 1));
    if (b <= a || a + 2 > glyf_len) {
        return;
    }
    const auto* g = reinterpret_cast<const std::uint8_t*>(data.data()) + glyf_off + a;
    const std::int16_t contours = ri16(g);
    if (contours >= 0) {
        return;
    }
    std::size_t i = 10;
    const std::size_t glen = b - a;
    while (i + 4 <= glen) {
        const std::uint16_t flags = ru16(g + i);
        const std::uint16_t child = ru16(g + i + 2);
        i += 4;
        if (flags & 0x0001) {
            i += 4;
        } else {
            i += 2;
        }
        if (flags & 0x0008) {
            i += 2;
        } else if (flags & 0x0040) {
            i += 4;
        } else if (flags & 0x0080) {
            i += 8;
        }
        collect_components(data, glyf_off, glyf_len, loca_off, loca_fmt, child, num_glyphs, used);
        if ((flags & 0x0020) == 0) {
            break;
        }
    }
}

std::string remap_glyph(const std::uint8_t* g, std::size_t glen,
                        const std::unordered_map<std::uint16_t, std::uint16_t>& old_to_new) {
    if (glen < 2) {
        return {};
    }
    const std::int16_t contours = ri16(g);
    if (contours >= 0) {
        return std::string(reinterpret_cast<const char*>(g), glen);
    }
    std::string out(reinterpret_cast<const char*>(g), glen);
    std::size_t i = 10;
    while (i + 4 <= out.size()) {
        const std::uint16_t flags = ru16(reinterpret_cast<const std::uint8_t*>(out.data()) + i);
        const std::uint16_t child = ru16(reinterpret_cast<const std::uint8_t*>(out.data()) + i + 2);
        const auto it = old_to_new.find(child);
        const std::uint16_t mapped = it == old_to_new.end() ? 0 : it->second;
        out[i + 2] = static_cast<char>(mapped >> 8);
        out[i + 3] = static_cast<char>(mapped & 0xFF);
        i += 4;
        if (flags & 0x0001) {
            i += 4;
        } else {
            i += 2;
        }
        if (flags & 0x0008) {
            i += 2;
        } else if (flags & 0x0040) {
            i += 4;
        } else if (flags & 0x0080) {
            i += 8;
        }
        if ((flags & 0x0020) == 0) {
            break;
        }
    }
    return out;
}

std::string hex_utf16(unsigned cp) {
    char buf[16];
    if (cp <= 0xFFFF) {
        std::snprintf(buf, sizeof(buf), "%04X", cp);
        return buf;
    }
    const unsigned s1 = 0xD800 + ((cp - 0x10000) >> 10);
    const unsigned s2 = 0xDC00 + ((cp - 0x10000) & 0x3FF);
    std::snprintf(buf, sizeof(buf), "%04X%04X", s1, s2);
    return buf;
}

std::string pdf_name_escape(const std::string& name) {
    std::string out;
    for (unsigned char ch : name) {
        if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '+' ||
            ch == '-') {
            out.push_back(static_cast<char>(ch));
        }
    }
    return out.empty() ? std::string("Embedded") : out;
}

std::string placeholder_from_cps(const std::vector<unsigned>& cps, const std::string& ps_name) {
    auto simple_square = []() {
        std::string g;
        wi16(g, 1);
        wi16(g, 100);
        wi16(g, 100);
        wi16(g, 900);
        wi16(g, 900);
        wu16(g, 3);
        wu16(g, 0);
        g.push_back(static_cast<char>(0x01));
        g.push_back(static_cast<char>(0x01));
        g.push_back(static_cast<char>(0x01));
        g.push_back(static_cast<char>(0x01));
        wi16(g, 100);
        wi16(g, 800);
        wi16(g, 0);
        wi16(g, -800);
        wi16(g, 100);
        wi16(g, 0);
        wi16(g, 0);
        wi16(g, 800);
        return g;
    };
    std::string glyf;
    std::string loca;
    wu32(loca, 0);
    wu32(loca, 0);
    const std::string sq = simple_square();
    std::string hmtx;
    wu16(hmtx, 0);
    wi16(hmtx, 0);
    std::unordered_map<unsigned, std::uint16_t> cmap;
    for (std::size_t i = 0; i < cps.size(); ++i) {
        glyf += sq;
        wu32(loca, static_cast<std::uint32_t>(glyf.size()));
        wu16(hmtx, 1000);
        wi16(hmtx, 100);
        cmap[cps[i]] = static_cast<std::uint16_t>(i + 1);
    }
    TtfInfo info;
    info.units_per_em = 1000;
    info.x_min = 100;
    info.y_min = 100;
    info.x_max = 900;
    info.y_max = 900;
    info.postscript_name = ps_name;
    const std::uint16_t ng = static_cast<std::uint16_t>(cps.size() + 1);
    std::vector<std::pair<std::string, std::string>> tables;
    tables.push_back({"cmap", cmap_from_map(cmap)});
    tables.push_back({"glyf", glyf});
    tables.push_back({"head", make_head(info, 1)});
    tables.push_back({"hhea", make_hhea(info, ng)});
    tables.push_back({"hmtx", hmtx});
    tables.push_back({"loca", loca});
    tables.push_back({"maxp", make_maxp(ng, {})});
    tables.push_back({"name", name_table(info.postscript_name)});
    tables.push_back({"post", post_table()});
    return wrap_sfnt(std::move(tables));
}

}  // namespace

Error TrueTypeFont::parse_face(std::size_t offset) {
    face_off_ = offset;
    TableDir cmap_t, head_t, hhea_t, hmtx_t, maxp_t, glyf_t, loca_t, name_t, os2_t;
    if (!find_table(data_, offset, "cmap", cmap_t) || !find_table(data_, offset, "head", head_t) ||
        !find_table(data_, offset, "hhea", hhea_t) || !find_table(data_, offset, "hmtx", hmtx_t) ||
        !find_table(data_, offset, "maxp", maxp_t) || !find_table(data_, offset, "glyf", glyf_t) ||
        !find_table(data_, offset, "loca", loca_t)) {
        return {ErrorCode::UnsupportedFeature,
                "font is not a TrueType glyf face (CFF/OpenType CJK fonts need --font PATH to a .ttf)"};
    }
    has_glyf_ = true;
    glyf_off_ = glyf_t.offset;
    glyf_len_ = glyf_t.length;
    loca_off_ = loca_t.offset;
    loca_len_ = loca_t.length;
    const auto* hp = reinterpret_cast<const std::uint8_t*>(data_.data()) + head_t.offset;
    if (head_t.length < 54) {
        return {ErrorCode::FileRead, "TrueType head table is truncated"};
    }
    info_.units_per_em = ru16(hp + 18);
    if (info_.units_per_em == 0) {
        info_.units_per_em = 1000;
    }
    info_.x_min = ri16(hp + 36);
    info_.y_min = ri16(hp + 38);
    info_.x_max = ri16(hp + 40);
    info_.y_max = ri16(hp + 42);
    loca_format_ = ri16(hp + 50);
    const auto* hhp = reinterpret_cast<const std::uint8_t*>(data_.data()) + hhea_t.offset;
    if (hhea_t.length < 36) {
        return {ErrorCode::FileRead, "TrueType hhea table is truncated"};
    }
    info_.ascent = ri16(hhp + 4);
    info_.descent = ri16(hhp + 6);
    number_of_hmetrics_ = ru16(hhp + 34);
    const auto* mp = reinterpret_cast<const std::uint8_t*>(data_.data()) + maxp_t.offset;
    if (maxp_t.length < 6) {
        return {ErrorCode::FileRead, "TrueType maxp table is truncated"};
    }
    num_glyphs_ = ru16(mp + 4);
    maxp_rest_.assign(data_.data() + maxp_t.offset, maxp_t.length);
    parse_cmap(data_, cmap_t, cmap_);
    advances_.assign(num_glyphs_, 0);
    lsbs_.assign(num_glyphs_, 0);
    const auto* hm = reinterpret_cast<const std::uint8_t*>(data_.data()) + hmtx_t.offset;
    const std::uint16_t nh = std::min(number_of_hmetrics_, num_glyphs_);
    if (hmtx_t.length < static_cast<std::uint32_t>(nh) * 4u) {
        return {ErrorCode::FileRead, "TrueType hmtx table is truncated"};
    }
    for (std::uint16_t i = 0; i < nh; ++i) {
        advances_[i] = ru16(hm + i * 4);
        lsbs_[i] = ri16(hm + i * 4 + 2);
    }
    const std::uint16_t last_adv = nh == 0 ? 0 : advances_[nh - 1];
    std::size_t lsb_off = static_cast<std::size_t>(nh) * 4;
    for (std::uint16_t i = nh; i < num_glyphs_; ++i) {
        advances_[i] = last_adv;
        if (lsb_off + 2 <= hmtx_t.length) {
            lsbs_[i] = ri16(hm + lsb_off);
            lsb_off += 2;
        }
    }
    if (find_table(data_, offset, "OS/2", os2_t) && os2_t.length >= 78) {
        os2_.assign(data_.data() + os2_t.offset, os2_t.length);
        const auto* o = reinterpret_cast<const std::uint8_t*>(data_.data()) + os2_t.offset;
        if (os2_t.length >= 90) {
            info_.cap_height = ri16(o + 88);
        } else if (os2_t.length >= 70) {
            info_.cap_height = ri16(o + 68);
        }
    }
    if (find_table(data_, offset, "name", name_t)) {
        const auto* np = reinterpret_cast<const std::uint8_t*>(data_.data()) + name_t.offset;
        if (name_t.length >= 6) {
            const std::uint16_t count = ru16(np + 2);
            const std::uint16_t store = ru16(np + 4);
            for (std::uint16_t i = 0; i < count; ++i) {
                const std::size_t rec = 6 + static_cast<std::size_t>(i) * 12;
                if (rec + 12 > name_t.length) {
                    break;
                }
                const std::uint16_t id = ru16(np + rec + 6);
                if (id != 6) {
                    continue;
                }
                const std::uint16_t plat = ru16(np + rec);
                const std::uint16_t length = ru16(np + rec + 8);
                const std::uint16_t noff = ru16(np + rec + 10);
                if (static_cast<std::uint32_t>(store) + noff + length > name_t.length) {
                    continue;
                }
                std::string raw(data_.data() + name_t.offset + store + noff, length);
                if (plat == 3 || plat == 0) {
                    std::string ascii;
                    for (std::size_t k = 1; k < raw.size(); k += 2) {
                        ascii.push_back(raw[k]);
                    }
                    if (!ascii.empty()) {
                        info_.postscript_name = pdf_name_escape(ascii);
                    }
                } else if (!raw.empty()) {
                    info_.postscript_name = pdf_name_escape(raw);
                }
                break;
            }
        }
    }
    return ok_error();
}

Error TrueTypeFont::load_bytes(std::string bytes, int face_index) {
    data_ = std::move(bytes);
    cmap_.clear();
    if (data_.size() < 12) {
        return {ErrorCode::FileRead, "font file is too small to be TrueType"};
    }
    const auto* p = reinterpret_cast<const std::uint8_t*>(data_.data());
    if (std::memcmp(data_.data(), "ttcf", 4) == 0) {
        const std::uint32_t nfaces = ru32(p + 8);
        if (nfaces == 0 || data_.size() < 12u + nfaces * 4u) {
            return {ErrorCode::FileRead, "TrueType collection header is truncated"};
        }
        if (face_index >= 0) {
            if (static_cast<std::uint32_t>(face_index) >= nfaces) {
                return {ErrorCode::FileRead, "TrueType collection has no such face"};
            }
            const std::uint32_t off = ru32(p + 12 + static_cast<std::uint32_t>(face_index) * 4);
            return parse_face(off);
        }
        Error last = {ErrorCode::UnsupportedFeature, "no glyf face in TrueType collection"};
        for (std::uint32_t i = 0; i < nfaces; ++i) {
            const std::uint32_t off = ru32(p + 12 + i * 4);
            TrueTypeFont trial;
            trial.data_ = data_;
            last = trial.parse_face(off);
            if (!last.ok()) {
                continue;
            }
            if (trial.glyph_id(0x4E00) != 0 || trial.glyph_id(0x4E2D) != 0) {
                *this = std::move(trial);
                return ok_error();
            }
            if (!has_glyf_) {
                *this = std::move(trial);
            }
        }
        return has_glyf_ ? ok_error() : last;
    }
    return parse_face(0);
}

Error TrueTypeFont::load_file(const std::string& path) {
    std::string bytes;
    Error err = platform::read_file_bounded(path, kMaxFontBytes, bytes);
    if (!err.ok()) {
        return {err.code, "could not read PDF font '" + path + "': " + err.message};
    }
    return load_bytes(std::move(bytes), -1);
}

std::uint16_t TrueTypeFont::glyph_id(unsigned cp) const {
    const auto it = cmap_.find(cp);
    return it == cmap_.end() ? 0 : it->second;
}

std::uint16_t TrueTypeFont::advance(std::uint16_t gid) const {
    return gid < advances_.size() ? advances_[gid] : 0;
}

double TrueTypeFont::width_pt(unsigned cp, double size) const {
    const std::uint16_t gid = glyph_id(cp);
    const double adv = gid < advances_.size() ? advances_[gid] : info_.units_per_em;
    return adv * size / static_cast<double>(info_.units_per_em);
}

Error TrueTypeFont::subset(const std::vector<unsigned>& codepoints, std::string& ttf_out,
                           std::unordered_map<unsigned, std::uint16_t>& cid_of_cp,
                           std::vector<std::uint16_t>& cid_widths_1000) const {
    ttf_out.clear();
    cid_of_cp.clear();
    cid_widths_1000.clear();
    if (!has_glyf_) {
        return {ErrorCode::UnsupportedFeature, "font has no glyf table to subset"};
    }
    std::unordered_set<std::uint16_t> used;
    used.insert(0);
    for (unsigned cp : codepoints) {
        const std::uint16_t gid = glyph_id(cp);
        if (gid != 0) {
            collect_components(data_, glyf_off_, glyf_len_, loca_off_, loca_format_, gid, num_glyphs_, used);
        }
    }
    std::vector<std::uint16_t> old_gids(used.begin(), used.end());
    std::sort(old_gids.begin(), old_gids.end());
    std::unordered_map<std::uint16_t, std::uint16_t> old_to_new;
    for (std::size_t i = 0; i < old_gids.size(); ++i) {
        old_to_new[old_gids[i]] = static_cast<std::uint16_t>(i);
    }
    for (unsigned cp : codepoints) {
        const std::uint16_t gid = glyph_id(cp);
        if (gid != 0) {
            cid_of_cp[cp] = old_to_new[gid];
        }
    }
    const std::uint16_t ng = static_cast<std::uint16_t>(old_gids.size());
    cid_widths_1000.assign(ng, 0);
    std::string glyf;
    std::string loca;
    wu32(loca, 0);
    for (std::uint16_t i = 0; i < ng; ++i) {
        const std::uint16_t old = old_gids[i];
        const std::uint32_t a = glyph_offset(data_, loca_off_, loca_format_, old);
        const std::uint32_t b =
            glyph_offset(data_, loca_off_, loca_format_, static_cast<std::uint16_t>(old + 1));
        std::string g;
        if (b > a && a < glyf_len_) {
            const std::size_t n = std::min<std::size_t>(b - a, glyf_len_ - a);
            g = remap_glyph(reinterpret_cast<const std::uint8_t*>(data_.data()) + glyf_off_ + a, n, old_to_new);
        }
        while (glyf.size() % 2 != 0) {
            glyf.push_back('\0');
        }
        glyf += g;
        wu32(loca, static_cast<std::uint32_t>(glyf.size()));
        const double w = old < advances_.size() ? advances_[old] : info_.units_per_em;
        cid_widths_1000[i] =
            static_cast<std::uint16_t>(w * 1000.0 / static_cast<double>(info_.units_per_em) + 0.5);
    }
    std::string hmtx;
    for (std::uint16_t i = 0; i < ng; ++i) {
        const std::uint16_t old = old_gids[i];
        wu16(hmtx, old < advances_.size() ? advances_[old] : 0);
        wi16(hmtx, old < lsbs_.size() ? lsbs_[old] : 0);
    }
    std::unordered_map<unsigned, std::uint16_t> new_cmap = cid_of_cp;
    std::vector<std::pair<std::string, std::string>> tables;
    tables.push_back({"cmap", cmap_from_map(new_cmap)});
    tables.push_back({"glyf", glyf});
    tables.push_back({"head", make_head(info_, 1)});
    tables.push_back({"hhea", make_hhea(info_, ng)});
    tables.push_back({"hmtx", hmtx});
    tables.push_back({"loca", loca});
    tables.push_back({"maxp", make_maxp(ng, maxp_rest_)});
    tables.push_back({"name", name_table(info_.postscript_name)});
    tables.push_back({"post", post_table()});
    if (os2_.size() >= 78) {
        tables.push_back({"OS/2", os2_});
    }
    ttf_out = wrap_sfnt(std::move(tables));
    return ok_error();
}

std::string find_cjk_font_path(const std::string& explicit_path) {
    if (!explicit_path.empty()) {
        return explicit_path;
    }
    const std::string env = platform::environment_value("AINIUX_PDF_FONT");
    if (!env.empty()) {
        return env;
    }
    std::vector<std::string> candidates = {
        "/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf",
        "/usr/share/fonts/truetype/droid/DroidSansFallback.ttf",
        "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc",
        "/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc",
        "/usr/share/fonts/truetype/arphic/uming.ttc",
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/truetype/noto/NotoSansSC-Regular.ttf",
        "/Library/Fonts/Arial Unicode.ttf",
        "/System/Library/Fonts/Supplemental/Arial Unicode.ttf",
        "/System/Library/Fonts/PingFang.ttc",
        "/System/Library/Fonts/STHeiti Light.ttc",
    };
    const std::string windir = platform::environment_value("WINDIR");
    if (!windir.empty()) {
        candidates.insert(candidates.begin(),
                          {windir + "/Fonts/msyh.ttc", windir + "/Fonts/msyh.ttf", windir + "/Fonts/simsun.ttc",
                           windir + "/Fonts/simhei.ttf", windir + "/Fonts/msjh.ttc"});
    }
    const std::string home = platform::home_directory();
    if (!home.empty()) {
        candidates.push_back(home + "/.local/share/fonts/NotoSansSC-Regular.ttf");
        candidates.push_back(home + "/.fonts/NotoSansSC-Regular.ttf");
    }
    for (const std::string& path : candidates) {
        TrueTypeFont font;
        if (font.load_file(path).ok() && font.has_glyf() && font.glyph_id(0x4E00) != 0) {
            return path;
        }
    }
    return {};
}

std::string find_rtl_font_path(const std::string& explicit_path, bool need_hebrew, bool need_arabic) {
    if (!need_hebrew && !need_arabic) {
        return {};
    }
    auto covers = [&](const TrueTypeFont& font) {
        if (!font.has_glyf()) {
            return false;
        }
        if (need_hebrew && font.glyph_id(0x05D0) == 0) {
            return false;
        }
        if (need_arabic && font.glyph_id(0xFE8F) == 0) {
            return false;
        }
        return true;
    };
    auto try_explicit = [&](const std::string& path) -> std::string {
        if (path.empty()) {
            return {};
        }
        TrueTypeFont font;
        if (!font.load_file(path).ok()) {
            return path;
        }
        return covers(font) ? path : std::string{};
    };
    if (!explicit_path.empty()) {
        const std::string hit = try_explicit(explicit_path);
        if (!hit.empty()) {
            return hit;
        }
    } else {
        const std::string env = platform::environment_value("AINIUX_PDF_FONT");
        const std::string hit = try_explicit(env);
        if (!hit.empty()) {
            return hit;
        }
    }
    std::vector<std::string> candidates;
    const auto add = [&](std::initializer_list<const char*> paths) {
        for (const char* path : paths) {
            candidates.push_back(path);
        }
    };
    if (need_hebrew && need_arabic) {
        add({"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
             "/usr/share/fonts/truetype/dejavu/DejaVuSerif.ttf",
             "/usr/share/fonts/truetype/freefont/FreeSerif.ttf"});
    }
    if (need_arabic) {
        add({"/usr/share/fonts/truetype/noto/NotoNaskhArabic-Regular.ttf",
             "/usr/share/fonts/truetype/noto/NotoSansArabic-Regular.ttf",
             "/usr/share/fonts/truetype/kacst/KacstNaskh.ttf",
             "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
             "/usr/share/fonts/truetype/freefont/FreeSerif.ttf"});
    }
    if (need_hebrew) {
        add({"/usr/share/fonts/truetype/noto/NotoSansHebrew-Regular.ttf",
             "/usr/share/fonts/truetype/noto/NotoSerifHebrew-Regular.ttf",
             "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
             "/usr/share/fonts/truetype/dejavu/DejaVuSerif.ttf",
             "/usr/share/fonts/truetype/freefont/FreeSerif.ttf"});
    }
    add({"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "/usr/share/fonts/truetype/freefont/FreeSerif.ttf",
         "/Library/Fonts/Arial.ttf", "/Library/Fonts/Arial Unicode.ttf",
         "/System/Library/Fonts/Supplemental/Arial Unicode.ttf",
         "/System/Library/Fonts/Supplemental/Times New Roman.ttf"});
    const std::string windir = platform::environment_value("WINDIR");
    if (!windir.empty()) {
        candidates.insert(candidates.begin(),
                          {windir + "/Fonts/arial.ttf", windir + "/Fonts/tahoma.ttf", windir + "/Fonts/times.ttf",
                           windir + "/Fonts/david.ttf", windir + "/Fonts/tradbdo.ttf",
                           windir + "/Fonts/segoeui.ttf"});
    }
    const std::string home = platform::home_directory();
    if (!home.empty()) {
        candidates.push_back(home + "/.local/share/fonts/NotoNaskhArabic-Regular.ttf");
        candidates.push_back(home + "/.local/share/fonts/NotoSansHebrew-Regular.ttf");
        candidates.push_back(home + "/.fonts/NotoNaskhArabic-Regular.ttf");
        candidates.push_back(home + "/.fonts/NotoSansHebrew-Regular.ttf");
    }
    for (const std::string& path : candidates) {
        TrueTypeFont font;
        if (font.load_file(path).ok() && covers(font)) {
            return path;
        }
    }
    return {};
}

Error embed_cid_type0(DocumentWriter& writer, const std::string& subset_ttf, const TtfInfo& info,
                      const std::vector<std::uint16_t>& cid_widths_1000,
                      const std::unordered_map<unsigned, std::uint16_t>& cid_of_cp,
                      std::uint32_t& type0) {
    type0 = 0;
    std::uint32_t file_obj = 0;
    Error err = writer.add_stream(" /Length1 " + std::to_string(subset_ttf.size()), subset_ttf, true, file_obj);
    if (!err.ok()) {
        return err;
    }
    const std::string base = "AINX00+" + pdf_name_escape(info.postscript_name);
    const std::uint32_t desc = writer.add_object(
        "<< /Type /FontDescriptor /FontName /" + base + " /FontFile2 " + std::to_string(file_obj) +
        " 0 R /Flags " + (info.fixed_pitch ? "33" : "32") + " /FontBBox [" +
        std::to_string(info.x_min) + " " + std::to_string(info.y_min) + " " + std::to_string(info.x_max) +
        " " + std::to_string(info.y_max) + "] /ItalicAngle 0 /Ascent " + std::to_string(info.ascent) +
        " /Descent " + std::to_string(info.descent) + " /CapHeight " + std::to_string(info.cap_height) +
        " /StemV 80 >>");
    std::string cmap =
        "/CIDInit /ProcSet findresource begin\n12 dict begin\nbegincmap\n"
        "/CIDSystemInfo << /Registry (Adobe) /Ordering (UCS) /Supplement 0 >> def\n"
        "/CMapName /Adobe-Identity-UCS def\n/CMapType 2 def\n"
        "1 begincodespacerange\n<0000> <FFFF>\nendcodespacerange\n";
    std::vector<std::pair<std::uint16_t, unsigned>> pairs;
    pairs.reserve(cid_of_cp.size());
    for (const auto& kv : cid_of_cp) {
        pairs.push_back({kv.second, kv.first});
    }
    std::sort(pairs.begin(), pairs.end());
    for (std::size_t i = 0; i < pairs.size();) {
        const std::size_t n = std::min<std::size_t>(100, pairs.size() - i);
        cmap += std::to_string(n) + " beginbfchar\n";
        for (std::size_t j = 0; j < n; ++j) {
            char cid[8];
            std::snprintf(cid, sizeof(cid), "%04X", pairs[i + j].first);
            cmap += "<";
            cmap += cid;
            cmap += "> <";
            cmap += hex_utf16(pairs[i + j].second);
            cmap += ">\n";
        }
        cmap += "endbfchar\n";
        i += n;
    }
    cmap += "endcmap\nCMapName currentdict /CMap defineresource pop\nend\nend\n";
    std::uint32_t to_unicode = 0;
    err = writer.add_stream("", cmap, true, to_unicode);
    if (!err.ok()) {
        return err;
    }
    std::string w = "[";
    if (cid_widths_1000.size() > 1) {
        w += " 1 [";
        for (std::size_t i = 1; i < cid_widths_1000.size(); ++i) {
            if (i > 1) {
                w += " ";
            }
            w += std::to_string(cid_widths_1000[i]);
        }
        w += "]";
    }
    w += " ]";
    const std::uint16_t dw = cid_widths_1000.empty() ? 1000 : cid_widths_1000[0];
    const std::uint32_t cidfont = writer.add_object(
        "<< /Type /Font /Subtype /CIDFontType2 /BaseFont /" + base +
        " /CIDSystemInfo << /Registry (Adobe) /Ordering (Identity) /Supplement 0 >> /FontDescriptor " +
        std::to_string(desc) + " 0 R /DW " + std::to_string(dw) + " /W " + w +
        " /CIDToGIDMap /Identity >>");
    type0 = writer.add_object("<< /Type /Font /Subtype /Type0 /BaseFont /" + base +
                              " /Encoding /Identity-H /DescendantFonts [" + std::to_string(cidfont) +
                              " 0 R] /ToUnicode " + std::to_string(to_unicode) + " 0 R >>");
    return ok_error();
}

Error make_placeholder_cjk_ttf(std::string& out) {
    out = placeholder_from_cps({0x4E2D, 0x6587}, "AiniuxTestCJK");
    return ok_error();
}

Error make_placeholder_rtl_ttf(std::string& out) {
    std::vector<unsigned> cps;
    cps.push_back(0x060C);
    cps.push_back(0x061B);
    cps.push_back(0x061F);
    cps.push_back(0x0640);
    cps.push_back(0x06D4);
    for (unsigned cp = 0x05D0; cp <= 0x05EA; ++cp) {
        cps.push_back(cp);
    }
    for (unsigned cp = 0x0621; cp <= 0x064A; ++cp) {
        cps.push_back(cp);
    }
    for (unsigned cp = 0xFE80; cp <= 0xFEFC; ++cp) {
        cps.push_back(cp);
    }
    const unsigned extra[] = {0xFB56, 0xFB57, 0xFB58, 0xFB59, 0xFB7A, 0xFB7B, 0xFB7C, 0xFB7D,
                              0xFB8A, 0xFB8B, 0xFB8E, 0xFB8F, 0xFB90, 0xFB91, 0xFB92, 0xFB93,
                              0xFB94, 0xFB95, 0xFBFC, 0xFBFD, 0xFBFE, 0xFBFF};
    for (unsigned cp : extra) {
        cps.push_back(cp);
    }
    out = placeholder_from_cps(cps, "AiniuxTestRTL");
    return ok_error();
}

}  // namespace ainiux::pdf
