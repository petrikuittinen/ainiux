#include "pdf/layout.hpp"

#include "ainiux/version.hpp"
#include "markdown/blocks.hpp"
#include "pdf/fonts.hpp"
#include "pdf/rtl.hpp"
#include "pdf/ttf.hpp"
#include "pdf/write.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ainiux::pdf {
namespace {

constexpr double kMm = 72.0 / 25.4;
constexpr double kPageWidth = 210.0 * kMm;
constexpr double kPageHeight = 11.0 * 72.0;
// 1 inch body margins. CropBox equals MediaBox (not an inset art box). A white
// page fill makes PDFium/Edge ink-bbox match the paper so side margins stay visible.
constexpr double kMargin = 72.0;
constexpr double kPageLeft = kMargin;
constexpr double kPageRight = kPageWidth - kMargin;
constexpr double kPageBottom = kMargin;
constexpr double kPageTop = kPageHeight - kMargin;
constexpr double kPageHeader = kPageHeight - 36.0;
constexpr double kPageFooter = 36.0;
constexpr double kLineHeight = 1.4;
constexpr double kSizeBody = 11.0;
constexpr double kSizeCode = 10.0;
constexpr double kSizeTable = 10.0;
constexpr double kSizeHeadFoot = 9.0;
constexpr double kHeadingSize[] = {18.0, 16.0, 15.0, 14.0, 13.0, 12.0};
constexpr double kListPadding = 36.0;
constexpr double kQuotePadding = 18.0;
constexpr double kQuoteBarGap = 6.0;
constexpr double kQuoteThickness = 3.0;
constexpr double kCodePadding = 4.5;
constexpr double kTablePadding = 4.5;
constexpr double kRuleGap = 8.0;
constexpr double kRuleThickness = 0.5;
constexpr int kMaxTableCols = 20;

const char* kFontRes[] = {"FR", "FB", "FI", "FBI", "FM"};

BaseFont font_for_style(unsigned style) {
    if (style & static_cast<unsigned>(markdown::RunStyle::Code)) {
        return BaseFont::Mono;
    }
    const bool bold = (style & static_cast<unsigned>(markdown::RunStyle::Bold)) != 0;
    const bool italic = (style & static_cast<unsigned>(markdown::RunStyle::Italic)) != 0;
    if (bold && italic) {
        return BaseFont::BoldItalic;
    }
    if (bold) {
        return BaseFont::Bold;
    }
    if (italic) {
        return BaseFont::Italic;
    }
    return BaseFont::Regular;
}

struct Frag {
    BaseFont font = BaseFont::Regular;
    double size = kSizeBody;
    std::string winansi;
    std::vector<std::uint16_t> cids;
    int cid_kind = 0;  // 0 Latin, 1 CJK /FC, 2 RTL /FA
    std::string url;
    double width = 0;
    bool strike = false;
    bool link = false;
    bool empty() const { return winansi.empty() && cids.empty(); }
};

struct LinkBox {
    double x = 0;
    double y = 0;
    double w = 0;
    double h = 0;
    std::string url;
};

struct PageRec {
    std::uint32_t content = 0;
    std::vector<LinkBox> links;
};

struct Layout {
    WriteOptions* options = nullptr;
    DocumentWriter writer;
    std::uint32_t encoding = 0;
    std::uint32_t fonts[static_cast<int>(BaseFont::Count)]{};
    std::uint32_t cjk_type0 = 0;
    std::uint32_t rtl_type0 = 0;
    TrueTypeFont cjk_face;
    TrueTypeFont rtl_face;
    std::unordered_map<unsigned, std::uint16_t> cjk_cid_of_cp;
    std::unordered_map<unsigned, std::uint16_t> rtl_cid_of_cp;
    bool have_cjk = false;
    bool have_rtl = false;
    int current_cid_kind = 0;
    std::string title;
    std::string heading;
    std::string content;
    double y = kPageTop;
    bool page_open = false;
    std::size_t page_number = 0;
    std::vector<PageRec> pages;
    std::vector<LinkBox> links;
    BaseFont current_font = BaseFont::Regular;
    double current_size = 0;
    bool in_text = false;
    bool current_rgb = false;
    double current_gray = -1;
    double current_r = 0;
    double current_g = 0;
    double current_b = 0;

    explicit Layout(WriteOptions& opts) : options(&opts) {}

    Error cancelled() const {
        if (options->cancellation.cancelled()) {
            return {ErrorCode::Cancelled, "Markdown-to-PDF conversion cancelled"};
        }
        return ok_error();
    }

    void append_op(const std::string& line) {
        content += line;
        content.push_back('\n');
    }

    void end_text() {
        if (in_text) {
            append_op("ET");
            in_text = false;
        }
    }

    void begin_text() {
        if (!in_text) {
            append_op("BT");
            in_text = true;
        }
    }

    void set_fill_gray(double g) {
        end_text();
        if (!current_rgb && current_gray == g) {
            return;
        }
        append_op(pdf_number(g) + " g");
        current_gray = g;
        current_rgb = false;
    }

    void set_fill_rgb(double r, double g, double b) {
        if (current_rgb && current_r == r && current_g == g && current_b == b) {
            return;
        }
        append_op(pdf_number(r) + " " + pdf_number(g) + " " + pdf_number(b) + " rg");
        current_rgb = true;
        current_r = r;
        current_g = g;
        current_b = b;
        current_gray = -1;
    }

    void ensure_font(BaseFont font, double size, int cid_kind = 0) {
        if (cid_kind != 0) {
            if (current_cid_kind == cid_kind && current_size == size && current_size != 0) {
                return;
            }
            append_op(std::string(cid_kind == 2 ? "/FA " : "/FC ") + pdf_number(size) + " Tf");
            current_cid_kind = cid_kind;
            current_size = size;
            return;
        }
        if (current_cid_kind == 0 && current_size == size && current_font == font && current_size != 0) {
            return;
        }
        append_op(std::string("/") + kFontRes[static_cast<int>(font)] + " " + pdf_number(size) +
                  " Tf");
        current_font = font;
        current_size = size;
        current_cid_kind = 0;
    }

    void fill_rect(double x, double yb, double w, double h) {
        end_text();
        append_op(pdf_number(x) + " " + pdf_number(yb) + " " + pdf_number(w) + " " + pdf_number(h) +
                  " re");
        append_op("f");
    }

    Error close_page() {
        if (!page_open) {
            return ok_error();
        }
        end_text();
        PageRec rec;
        Error err = writer.add_stream("", content, true, rec.content);
        if (!err.ok()) {
            return err;
        }
        rec.links = std::move(links);
        pages.push_back(std::move(rec));
        content.clear();
        links.clear();
        page_open = false;
        current_size = 0;
        current_cid_kind = 0;
        current_rgb = false;
        current_gray = -1;
        return ok_error();
    }

    Error new_page() {
        Error err = cancelled();
        if (!err.ok()) {
            return err;
        }
        err = close_page();
        if (!err.ok()) {
            return err;
        }
        if (options->max_pages > 0 && pages.size() >= options->max_pages) {
            return {ErrorCode::UnsupportedFeature,
                    "PDF output exceeds --max-pages limit of " + std::to_string(options->max_pages)};
        }
        ++page_number;
        page_open = true;
        y = kPageTop;
        // White MediaBox fill so PDFium/Edge ink-bbox equals the paper, not the text.
        set_fill_gray(1);
        fill_rect(0, 0, kPageWidth, kPageHeight);
        set_fill_gray(0);
        if (page_number > 1 && !title.empty()) {
            const double width = measure_utf8(title, kSizeHeadFoot, BaseFont::Regular);
            draw_utf8_at(title, (kPageWidth - width) / 2.0, kPageHeader - 2, kSizeHeadFoot,
                         BaseFont::Regular);
            end_text();
        }
        begin_text();
        ensure_font(BaseFont::Regular, kSizeHeadFoot);
        if (!heading.empty()) {
            draw_utf8_at(heading, kPageLeft, kPageFooter, kSizeHeadFoot, BaseFont::Regular);
        }
        const std::string page_text = std::to_string(page_number);
        const double pw = text_width(BaseFont::Regular, page_text, kSizeHeadFoot);
        append_op("1 0 0 1 " + pdf_number(kPageRight - pw) + " " + pdf_number(kPageFooter) + " Tm");
        append_op("(" + pdf_escape_string(page_text) + ") Tj");
        end_text();
        return ok_error();
    }

    Error ensure_space(double need) {
        if (!page_open) {
            return new_page();
        }
        if (y - need < kPageBottom) {
            return new_page();
        }
        return ok_error();
    }

    void show_frag(const Frag& frag, double x, double baseline) {
        begin_text();
        if (frag.link) {
            set_fill_rgb(0, 0, 0.8);
        } else if (current_rgb || current_gray != 0) {
            append_op("0 g");
            current_rgb = false;
            current_gray = 0;
        }
        ensure_font(frag.font, frag.size, frag.cid_kind);
        append_op("1 0 0 1 " + pdf_number(x) + " " + pdf_number(baseline) + " Tm");
        if (frag.cid_kind != 0) {
            std::string hex = "<";
            for (std::uint16_t cid : frag.cids) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "%04X", cid);
                hex += buf;
            }
            hex += ">";
            append_op(hex + " Tj");
        } else {
            append_op("(" + pdf_escape_string(frag.winansi) + ") Tj");
        }
        if (frag.strike || frag.link) {
            end_text();
            append_op("0.5 w");
            if (frag.link) {
                append_op("0 0 0.8 RG");
            } else {
                append_op("0 G");
            }
            const double ly = frag.strike ? baseline + frag.size * 0.3 : baseline - 1.0;
            append_op(pdf_number(x) + " " + pdf_number(ly) + " m " + pdf_number(x + frag.width) +
                      " " + pdf_number(ly) + " l S");
        }
        if (!frag.url.empty()) {
            LinkBox box;
            box.x = x;
            box.y = baseline - 1.0;
            box.w = frag.width;
            box.h = frag.size * kLineHeight;
            box.url = frag.url;
            links.push_back(std::move(box));
        }
    }

    Frag latin_frag(const markdown::Run& run, std::string_view text, double size, BaseFont def_font) {
        Frag frag;
        unsigned style = run.style;
        if ((style & static_cast<unsigned>(markdown::RunStyle::Code)) == 0) {
            if (def_font == BaseFont::Italic || def_font == BaseFont::BoldItalic) {
                style |= static_cast<unsigned>(markdown::RunStyle::Italic);
            }
            if (def_font == BaseFont::Bold || def_font == BaseFont::BoldItalic) {
                style |= static_cast<unsigned>(markdown::RunStyle::Bold);
            }
        }
        frag.font = font_for_style(style);
        frag.size = (style & static_cast<unsigned>(markdown::RunStyle::Code)) ? std::min(size, kSizeCode + 1)
                                                                               : size;
        frag.winansi = utf8_to_win_ansi(text, options->substituted_glyphs);
        frag.url = run.url;
        frag.link = !run.url.empty();
        frag.strike = (style & static_cast<unsigned>(markdown::RunStyle::Strike)) != 0;
        frag.width = text_width(frag.font, frag.winansi, frag.size);
        return frag;
    }

    Frag cid_frag(const markdown::Run& run, unsigned cp, double size, BaseFont def_font, int cid_kind) {
        Frag frag;
        unsigned style = run.style;
        if ((style & static_cast<unsigned>(markdown::RunStyle::Code)) == 0) {
            if (def_font == BaseFont::Italic || def_font == BaseFont::BoldItalic) {
                style |= static_cast<unsigned>(markdown::RunStyle::Italic);
            }
            if (def_font == BaseFont::Bold || def_font == BaseFont::BoldItalic) {
                style |= static_cast<unsigned>(markdown::RunStyle::Bold);
            }
        }
        frag.font = font_for_style(style);
        frag.size = (style & static_cast<unsigned>(markdown::RunStyle::Code)) ? std::min(size, kSizeCode + 1)
                                                                               : size;
        frag.cid_kind = cid_kind;
        if (cid_kind == 2) {
            frag.cids.push_back(rtl_cid_of_cp[cp]);
            frag.width = rtl_face.width_pt(cp, frag.size);
        } else {
            frag.cids.push_back(cjk_cid_of_cp[cp]);
            frag.width = cjk_face.width_pt(cp, frag.size);
        }
        frag.url = run.url;
        frag.link = !run.url.empty();
        frag.strike = (style & static_cast<unsigned>(markdown::RunStyle::Strike)) != 0;
        return frag;
    }

    std::vector<Frag> split_run(const markdown::Run& run, double size, BaseFont def_font) {
        std::vector<Frag> out;
        if (run.text.empty()) {
            return out;
        }
        markdown::Run shaped = run;
        shaped.text = shape_utf8(run.text);
        size_t i = 0;
        while (i < shaped.text.size()) {
            if (shaped.text[i] == ' ' || shaped.text[i] == '\t') {
                size_t end = i;
                while (end < shaped.text.size() && (shaped.text[end] == ' ' || shaped.text[end] == '\t')) {
                    ++end;
                }
                out.push_back(latin_frag(shaped, shaped.text.substr(i, end - i), size, def_font));
                i = end;
                continue;
            }
            const size_t cp_start = i;
            const unsigned cp = next_utf8(shaped.text, i);
            if (cp == 0) {
                continue;
            }
            if (have_rtl && rtl_cid_of_cp.count(cp)) {
                out.push_back(cid_frag(run, cp, size, def_font, 2));
                continue;
            }
            if (have_cjk && cjk_cid_of_cp.count(cp)) {
                out.push_back(cid_frag(run, cp, size, def_font, 1));
                continue;
            }
            size_t end = i;
            while (end < shaped.text.size()) {
                const char ch = shaped.text[end];
                if (ch == ' ' || ch == '\t') {
                    break;
                }
                size_t next = end;
                const unsigned look = next_utf8(shaped.text, next);
                if ((have_rtl && rtl_cid_of_cp.count(look)) || (have_cjk && cjk_cid_of_cp.count(look))) {
                    break;
                }
                end = next;
            }
            out.push_back(latin_frag(shaped, shaped.text.substr(cp_start, end - cp_start), size, def_font));
            i = end;
        }
        return out;
    }

    static bool same_paint(const Frag& a, const Frag& b) {
        return a.font == b.font && a.size == b.size && a.link == b.link && a.strike == b.strike &&
               a.url == b.url && a.cid_kind == b.cid_kind;
    }

    double measure_utf8(std::string_view text, double size, BaseFont font) {
        markdown::Run run;
        run.text.assign(text.data(), text.size());
        double w = 0;
        for (const Frag& frag : split_run(run, size, font)) {
            if (frag.width >= 0) {
                w += frag.width;
            }
        }
        return w;
    }

    void draw_utf8_at(std::string_view text, double x, double baseline, double size, BaseFont font) {
        markdown::Run run;
        run.text.assign(text.data(), text.size());
        std::vector<Frag> frags = split_run(run, size, font);
        if (utf8_has_rtl(text)) {
            std::reverse(frags.begin(), frags.end());
        }
        for (const Frag& frag : frags) {
            if (!frag.empty()) {
                show_frag(frag, x, baseline);
            }
            if (frag.width >= 0) {
                x += frag.width;
            }
        }
    }

    Error render_line(double left, double text_x, const std::vector<Frag>& frags, double lineheight) {
        Error err = ensure_space(lineheight);
        if (!err.ok()) {
            return err;
        }
        double x = text_x;
        size_t i = 0;
        while (i < frags.size()) {
            const Frag& frag = frags[i];
            if (frag.empty()) {
                if (frag.width >= 0) {
                    x += frag.width;
                }
                ++i;
                continue;
            }
            const double fx = frag.width < 0 ? left + frag.width : x;
            Frag joined = frag;
            double total_w = frag.width < 0 ? -frag.width : frag.width;
            size_t j = i + 1;
            if (frag.width >= 0) {
                while (j < frags.size() && frags[j].width >= 0 && !frags[j].empty() &&
                       same_paint(joined, frags[j])) {
                    joined.winansi += frags[j].winansi;
                    joined.cids.insert(joined.cids.end(), frags[j].cids.begin(), frags[j].cids.end());
                    total_w += frags[j].width;
                    ++j;
                }
            }
            joined.width = total_w;
            show_frag(joined, fx, y);
            if (frag.width >= 0) {
                x += total_w;
            }
            i = j;
        }
        y -= lineheight;
        return ok_error();
    }

    Error format_runs(const std::vector<markdown::Run>& runs, double size, double left, double right,
                      BaseFont def_font, const std::string& leader) {
        std::vector<Frag> words;
        bool rtl_para = false;
        for (const markdown::Run& run : runs) {
            if (utf8_has_rtl(run.text)) {
                rtl_para = true;
            }
            auto parts = split_run(run, size, def_font);
            words.insert(words.end(), parts.begin(), parts.end());
            if (run.hard_break_after && !words.empty()) {
                words.back().url = words.back().url;
            }
        }
        std::vector<Frag> line;
        double lineheight = size * kLineHeight;
        double used = 0;
        const double width = right - left;
        if (!leader.empty()) {
            Frag lead;
            lead.font = def_font;
            lead.size = size;
            lead.winansi = utf8_to_win_ansi(leader, options->substituted_glyphs);
            lead.width = -text_width(lead.font, lead.winansi, lead.size);
            line.push_back(lead);
        }
        auto line_text_width = [](const std::vector<Frag>& frags) {
            double w = 0;
            for (const Frag& frag : frags) {
                if (frag.width >= 0) {
                    w += frag.width;
                }
            }
            return w;
        };
        auto prepare_rtl = [&](std::vector<Frag>& frags) {
            if (!rtl_para) {
                return;
            }
            bool any_rtl = false;
            for (const Frag& frag : frags) {
                if (frag.cid_kind == 2) {
                    any_rtl = true;
                    break;
                }
            }
            if (!any_rtl) {
                return;
            }
            const std::size_t n = frags.size();
            enum { DirL = 0, DirR = 1, DirN = 2 };
            std::vector<int> cls(n, DirN);
            auto is_ltr_word = [](const Frag& frag) {
                if (frag.cid_kind != 0) {
                    return false;
                }
                for (unsigned char ch : frag.winansi) {
                    if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')) {
                        return true;
                    }
                }
                return false;
            };
            for (std::size_t i = 0; i < n; ++i) {
                if (frags[i].cid_kind == 2) {
                    cls[i] = DirR;
                } else if (frags[i].cid_kind == 1 || is_ltr_word(frags[i])) {
                    cls[i] = DirL;
                }
            }
            for (std::size_t i = 0; i < n; ++i) {
                if (cls[i] != DirN) {
                    continue;
                }
                int prev = DirR;
                int next = DirR;
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
                std::size_t begin;
                std::size_t end;
                int dir;
            };
            std::vector<Run> runs;
            std::size_t i = 0;
            while (i < n) {
                Run run{i, i, cls[i]};
                ++i;
                while (i < n && cls[i] == run.dir) {
                    ++i;
                }
                run.end = i;
                runs.push_back(run);
            }
            std::vector<Frag> ordered;
            ordered.reserve(n);
            for (auto it = runs.rbegin(); it != runs.rend(); ++it) {
                std::vector<Frag> slice(frags.begin() + static_cast<std::ptrdiff_t>(it->begin),
                                        frags.begin() + static_cast<std::ptrdiff_t>(it->end));
                if (it->dir == DirR) {
                    for (Frag& frag : slice) {
                        if (frag.cid_kind == 0 && frag.winansi.size() == 1) {
                            const unsigned mirrored =
                                mirror_cp(static_cast<unsigned char>(frag.winansi[0]));
                            frag.winansi[0] = static_cast<char>(mirrored);
                        }
                    }
                    std::reverse(slice.begin(), slice.end());
                }
                ordered.insert(ordered.end(), slice.begin(), slice.end());
            }
            frags.swap(ordered);
        };
        auto flush = [&]() -> Error {
            if (line.empty()) {
                return ok_error();
            }
            prepare_rtl(line);
            const double text_x = rtl_para ? right - line_text_width(line) : left;
            Error err = render_line(left, text_x, line, lineheight);
            line.clear();
            used = 0;
            return err;
        };
        for (size_t i = 0; i < words.size(); ++i) {
            Frag word = words[i];
            const bool space = !word.winansi.empty() && word.winansi[0] == ' ';
            if (space && line.empty()) {
                continue;
            }
            if (!line.empty() && used + word.width > width && !space) {
                Error err = flush();
                if (!err.ok()) {
                    return err;
                }
            }
            if (!space && word.width > width && line.empty()) {
                std::vector<Frag> one{word};
                prepare_rtl(one);
                const double text_x = rtl_para ? right - line_text_width(one) : left;
                Error err = render_line(left, text_x, one, lineheight);
                if (!err.ok()) {
                    return err;
                }
                continue;
            }
            line.push_back(word);
            used += word.width >= 0 ? word.width : 0;
            if (i + 1 < runs.size() && runs[std::min(i, runs.size() - 1)].hard_break_after) {
                Error err = flush();
                if (!err.ok()) {
                    return err;
                }
            }
        }
        return flush();
    }

    Error format_code(const std::string& text, double left, double right) {
        std::vector<std::string> lines;
        std::string cur;
        for (char ch : text) {
            if (ch == '\n') {
                lines.push_back(cur);
                cur.clear();
            } else if (ch != '\r') {
                cur.push_back(ch);
            }
        }
        if (!cur.empty() || lines.empty()) {
            lines.push_back(cur);
        }
        const double lineheight = kSizeCode * kLineHeight;
        const double block_h = lineheight * static_cast<double>(std::max<std::size_t>(lines.size(), 1)) +
                               2.0 * kCodePadding;
        Error err = ensure_space(block_h);
        if (!err.ok()) {
            return err;
        }
        y -= kCodePadding;
        const double top = y + kCodePadding;
        set_fill_gray(0.93);
        fill_rect(left - kCodePadding, top - block_h + kCodePadding, right - left + 2 * kCodePadding,
                  block_h);
        set_fill_gray(0);
        for (const std::string& line : lines) {
            err = ensure_space(lineheight);
            if (!err.ok()) {
                return err;
            }
            draw_utf8_at(line, left, y, kSizeCode, BaseFont::Mono);
            y -= lineheight;
        }
        y -= kCodePadding;
        return ok_error();
    }

    Error format_table(const markdown::Block& table, double left, double right) {
        if (table.table_cells.empty()) {
            return ok_error();
        }
        const size_t rows = table.table_cells.size();
        size_t cols = 0;
        for (const auto& row : table.table_cells) {
            cols = std::max(cols, row.size());
        }
        cols = std::min(cols, static_cast<size_t>(kMaxTableCols));
        if (cols == 0) {
            return ok_error();
        }
        std::vector<double> widths(cols, 0);
        for (const auto& row : table.table_cells) {
            for (size_t c = 0; c < cols && c < row.size(); ++c) {
                double w = 0;
                for (const markdown::Run& run : row[c]) {
                    w += measure_utf8(run.text, kSizeTable, BaseFont::Regular);
                }
                widths[c] = std::max(widths[c], w + 2 * kTablePadding);
            }
        }
        double table_width = 0;
        for (double w : widths) {
            table_width += w;
        }
        const double avail = right - left;
        if (table_width > avail && table_width > 0) {
            const double scale = avail / table_width;
            for (double& w : widths) {
                w *= scale;
            }
        }
        const double lineheight = kSizeTable * kLineHeight;
        for (size_t r = 0; r < rows; ++r) {
            Error err = ensure_space(lineheight + kTablePadding);
            if (!err.ok()) {
                return err;
            }
            double x = left;
            const BaseFont row_font = r == 0 ? BaseFont::Bold : BaseFont::Regular;
            for (size_t c = 0; c < cols; ++c) {
                const double cell_right = x + widths[c];
                std::vector<markdown::Run> runs;
                if (c < table.table_cells[r].size()) {
                    runs = table.table_cells[r][c];
                }
                const double saved_y = y;
                err = format_runs(runs, kSizeTable, x + kTablePadding, cell_right - kTablePadding,
                                  row_font, "");
                if (!err.ok()) {
                    return err;
                }
                y = saved_y;
                x = cell_right;
            }
            y -= lineheight + kTablePadding;
        }
        return ok_error();
    }

    Error format_blocks(const std::vector<markdown::Block>& blocks, double left, double right,
                        BaseFont def_font);
};

Error Layout::format_blocks(const std::vector<markdown::Block>& blocks, double left, double right,
                            BaseFont def_font) {
    for (const markdown::Block& block : blocks) {
        Error err = cancelled();
        if (!err.ok()) {
            return err;
        }
        switch (block.kind) {
            case markdown::BlockKind::Rule:
                err = ensure_space(kRuleGap * 2.0 + kRuleThickness);
                if (!err.ok()) {
                    return err;
                }
                y -= kRuleGap;
                end_text();
                append_op(pdf_number(kRuleThickness) + " w");
                append_op("0.55 G");
                append_op(pdf_number(left) + " " + pdf_number(y) + " m " + pdf_number(right) + " " +
                          pdf_number(y) + " l S");
                append_op("0 G");
                y -= kRuleGap;
                break;
            case markdown::BlockKind::Heading: {
                int level = std::max(1, std::min(block.heading_level, 6));
                const double size = kHeadingSize[level - 1];
                std::string text;
                for (const markdown::Run& run : block.runs) {
                    text += run.text;
                }
                heading = text;
                if (title.empty() && level == 1) {
                    title = text;
                }
                err = ensure_space(size * kLineHeight * 2.0);
                if (!err.ok()) {
                    return err;
                }
                y -= size * 0.4;
                err = format_runs(block.runs, size, left, right, BaseFont::Bold, "");
                if (!err.ok()) {
                    return err;
                }
                break;
            }
            case markdown::BlockKind::Paragraph:
                err = ensure_space(kSizeBody * kLineHeight);
                if (!err.ok()) {
                    return err;
                }
                y -= kSizeBody * 0.4;
                err = format_runs(block.runs, kSizeBody, left, right, def_font, "");
                if (!err.ok()) {
                    return err;
                }
                break;
            case markdown::BlockKind::ListItem: {
                const double indent = left + kListPadding + static_cast<double>(block.indent) * 0.5;
                std::string leader;
                if (block.ordered) {
                    leader = std::to_string(block.list_index) + ". ";
                } else {
                    leader = "• ";
                }
                err = format_runs(block.runs, kSizeBody, indent, right, def_font, leader);
                if (!err.ok()) {
                    return err;
                }
                break;
            }
            case markdown::BlockKind::Quote: {
                err = ensure_space(kSizeBody * kLineHeight);
                if (!err.ok()) {
                    return err;
                }
                const double saved_y = y;
                err = format_blocks(block.children, left + kQuotePadding, right - kQuotePadding,
                                    BaseFont::Italic);
                if (!err.ok()) {
                    return err;
                }
                end_text();
                set_fill_gray(0.55);
                const double top = saved_y - 2.0;
                const double bottom = y + kSizeBody * 0.35;
                const double bar_h = std::max(top - bottom, kSizeBody * 0.8);
                fill_rect(left + kQuoteBarGap, bottom, kQuoteThickness, bar_h);
                set_fill_gray(0);
                break;
            }
            case markdown::BlockKind::Code:
                err = format_code(block.text, left + kCodePadding, right - kCodePadding);
                if (!err.ok()) {
                    return err;
                }
                break;
            case markdown::BlockKind::Table:
                err = format_table(block, left, right);
                if (!err.ok()) {
                    return err;
                }
                break;
            case markdown::BlockKind::Html:
                err = format_runs(block.runs, kSizeBody, left, right, def_font, "");
                if (!err.ok()) {
                    return err;
                }
                break;
        }
    }
    return ok_error();
}

std::string font_object(std::uint32_t encoding, BaseFont font) {
    return "<< /Type /Font /Subtype /Type1 /BaseFont /" + std::string(base_font_name(font)) +
           " /Encoding " + std::to_string(encoding) + " 0 R >>";
}

std::string page_resources(const std::uint32_t* fonts, std::uint32_t cjk_type0, std::uint32_t rtl_type0) {
    std::string out = "<< /Font <<";
    for (int i = 0; i < static_cast<int>(BaseFont::Count); ++i) {
        out += " /";
        out += kFontRes[i];
        out += " ";
        out += std::to_string(fonts[i]);
        out += " 0 R";
    }
    if (cjk_type0 != 0) {
        out += " /FC ";
        out += std::to_string(cjk_type0);
        out += " 0 R";
    }
    if (rtl_type0 != 0) {
        out += " /FA ";
        out += std::to_string(rtl_type0);
        out += " 0 R";
    }
    out += " >> >>";
    return out;
}

void collect_codepoints(const std::vector<markdown::Block>& blocks, std::vector<unsigned>& cjk_cps,
                        std::vector<unsigned>& rtl_cps, bool& need_hebrew, bool& need_arabic) {
    std::unordered_set<unsigned> seen;
    need_hebrew = false;
    need_arabic = false;
    auto add_text = [&](const std::string& text) {
        const std::string shaped = shape_utf8(text);
        size_t i = 0;
        while (i < shaped.size()) {
            const unsigned cp = next_utf8(shaped, i);
            if (cp == 0 || !needs_embedded_cp(cp) || !seen.insert(cp).second) {
                continue;
            }
            if (is_hebrew_cp(cp) || is_arabic_cp(cp)) {
                rtl_cps.push_back(cp);
                if (is_hebrew_cp(cp)) {
                    need_hebrew = true;
                }
                if (is_arabic_cp(cp)) {
                    need_arabic = true;
                }
            } else {
                cjk_cps.push_back(cp);
            }
        }
    };
    auto add_runs = [&](const std::vector<markdown::Run>& runs) {
        for (const markdown::Run& run : runs) {
            add_text(run.text);
        }
    };
    auto walk = [&](auto&& self, const std::vector<markdown::Block>& items) -> void {
        for (const markdown::Block& block : items) {
            add_runs(block.runs);
            add_text(block.text);
            for (const auto& row : block.table_cells) {
                for (const auto& cell : row) {
                    add_runs(cell);
                }
            }
            self(self, block.children);
        }
    };
    walk(walk, blocks);
}

}  // namespace

Error layout_markdown(std::string_view markdown, WriteOptions& options, std::string& pdf) {
    pdf.clear();
    options.substituted_glyphs = 0;
    options.cjk_font_missing = false;
    options.rtl_font_missing = false;
    Layout layout(options);
    layout.encoding = layout.writer.add_object("<< /Type /Encoding /BaseEncoding /WinAnsiEncoding >>");
    for (int i = 0; i < static_cast<int>(BaseFont::Count); ++i) {
        layout.fonts[i] =
            layout.writer.add_object(font_object(layout.encoding, static_cast<BaseFont>(i)));
    }

    const std::vector<markdown::Block> blocks = markdown::parse_blocks(std::string(markdown));
    std::vector<unsigned> cjk_cps;
    std::vector<unsigned> rtl_cps;
    bool need_hebrew = false;
    bool need_arabic = false;
    collect_codepoints(blocks, cjk_cps, rtl_cps, need_hebrew, need_arabic);
    if (!cjk_cps.empty()) {
        const std::string path = find_cjk_font_path(options.font_path);
        if (path.empty()) {
            options.cjk_font_missing = true;
        } else {
            Error font_err = layout.cjk_face.load_file(path);
            if (!font_err.ok()) {
                if (!options.font_path.empty()) {
                    return font_err;
                }
                options.cjk_font_missing = true;
            } else {
                std::string subset;
                std::vector<std::uint16_t> widths;
                font_err = layout.cjk_face.subset(cjk_cps, subset, layout.cjk_cid_of_cp, widths);
                if (!font_err.ok()) {
                    return font_err;
                }
                if (layout.cjk_cid_of_cp.empty()) {
                    options.cjk_font_missing = true;
                } else {
                    font_err = embed_cid_type0(layout.writer, subset, layout.cjk_face.info(), widths,
                                               layout.cjk_cid_of_cp, layout.cjk_type0);
                    if (!font_err.ok()) {
                        return font_err;
                    }
                    layout.have_cjk = true;
                }
            }
        }
    }
    if (!rtl_cps.empty()) {
        const std::string path = find_rtl_font_path(options.font_path, need_hebrew, need_arabic);
        if (path.empty()) {
            options.rtl_font_missing = true;
        } else {
            Error font_err = layout.rtl_face.load_file(path);
            if (!font_err.ok()) {
                if (!options.font_path.empty()) {
                    return font_err;
                }
                options.rtl_font_missing = true;
            } else {
                std::string subset;
                std::vector<std::uint16_t> widths;
                font_err = layout.rtl_face.subset(rtl_cps, subset, layout.rtl_cid_of_cp, widths);
                if (!font_err.ok()) {
                    return font_err;
                }
                if (layout.rtl_cid_of_cp.empty()) {
                    options.rtl_font_missing = true;
                } else {
                    font_err = embed_cid_type0(layout.writer, subset, layout.rtl_face.info(), widths,
                                               layout.rtl_cid_of_cp, layout.rtl_type0);
                    if (!font_err.ok()) {
                        return font_err;
                    }
                    layout.have_rtl = true;
                }
            }
        }
    }
    Error err = layout.format_blocks(blocks, kPageLeft, kPageRight, BaseFont::Regular);
    if (!err.ok()) {
        return err;
    }
    if (!layout.page_open && layout.pages.empty()) {
        err = layout.new_page();
        if (!err.ok()) {
            return err;
        }
    }
    err = layout.close_page();
    if (!err.ok()) {
        return err;
    }

    const std::uint32_t pages_obj = layout.writer.reserve_object();
    std::vector<std::uint32_t> page_objs;
    page_objs.reserve(layout.pages.size());
    for (PageRec& rec : layout.pages) {
        std::string annots;
        if (!rec.links.empty()) {
            annots = " /Annots [";
            for (const LinkBox& box : rec.links) {
                const std::uint32_t annot = layout.writer.add_object(
                    "<< /Type /Annot /Subtype /Link /Rect [" + pdf_number(box.x) + " " +
                    pdf_number(box.y) + " " + pdf_number(box.x + box.w) + " " +
                    pdf_number(box.y + box.h) + "] /Border [0 0 0] /A << /S /URI /URI (" +
                    pdf_escape_string(utf8_to_win_ansi(box.url, options.substituted_glyphs)) +
                    ") >> >>");
                annots += " " + std::to_string(annot) + " 0 R";
            }
            annots += " ]";
        }
        const std::string box =
            "[0 0 " + pdf_number(kPageWidth) + " " + pdf_number(kPageHeight) + "]";
        const std::uint32_t page = layout.writer.add_object(
            "<< /Type /Page /Parent " + std::to_string(pages_obj) + " 0 R /MediaBox " + box +
            " /CropBox " + box + " /Resources " +
            page_resources(layout.fonts, layout.cjk_type0, layout.rtl_type0) +
            " /Contents " +
            std::to_string(rec.content) + " 0 R" + annots + " >>");
        page_objs.push_back(page);
    }

    std::string kids = "[";
    for (std::uint32_t page : page_objs) {
        kids += " " + std::to_string(page) + " 0 R";
    }
    kids += " ]";
    layout.writer.set_object(pages_obj, "<< /Type /Pages /Kids " + kids + " /Count " +
                                            std::to_string(page_objs.size()) + " >>");
    const std::uint32_t pages = pages_obj;
    const std::uint32_t catalog =
        layout.writer.add_object("<< /Type /Catalog /Pages " + std::to_string(pages) + " 0 R >>");
    const std::uint32_t info = layout.writer.add_object(
        "<< /Producer (" + pdf_escape_string(app_version_label()) + ") /CreationDate (" +
        pdf_creation_date() + ") >>");
    pdf = layout.writer.finish(catalog, info);
    if (options.max_bytes > 0 && pdf.size() > options.max_bytes) {
        pdf.clear();
        return {ErrorCode::UnsupportedFeature,
                "PDF output exceeds size limit of " + std::to_string(options.max_bytes) + " bytes"};
    }
    return ok_error();
}

}  // namespace ainiux::pdf
