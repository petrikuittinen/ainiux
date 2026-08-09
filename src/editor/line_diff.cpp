#include "editor/line_diff.hpp"

#include <algorithm>
#include <cstdint>
#include <utility>

namespace ainiux::editor {
namespace {

std::vector<std::string> split_lines_lf(const std::string& text) {
    std::vector<std::string> lines;
    size_t start = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\n') {
            lines.emplace_back(text, start, i - start);
            start = i + 1;
        }
    }
    // Match PieceTable: content after the last newline is a line; a trailing newline
    // also creates a final empty line. Empty input is one empty line (line_count == 1).
    lines.emplace_back(text, start, text.size() - start);
    return lines;
}

// LCS-based mark: lines of current not on an LCS path with previous are "changed".
// Uses O(n*m) uint16 lengths with a product cap; max_lines already bounds n and m.
std::vector<bool> mark_via_lcs(const std::vector<std::string>& prev,
                               const std::vector<std::string>& curr) {
    const size_t n = prev.size();
    const size_t m = curr.size();
    // (n+1)*(m+1) cells of uint16_t.
    std::vector<std::uint16_t> dp((n + 1) * (m + 1), 0);
    auto at = [&](size_t i, size_t j) -> std::uint16_t& { return dp[i * (m + 1) + j]; };

    for (size_t i = 1; i <= n; ++i) {
        for (size_t j = 1; j <= m; ++j) {
            if (prev[i - 1] == curr[j - 1]) {
                const std::uint32_t next = static_cast<std::uint32_t>(at(i - 1, j - 1)) + 1U;
                at(i, j) = static_cast<std::uint16_t>(std::min(next, 0xFFFFU));
            } else {
                at(i, j) = std::max(at(i - 1, j), at(i, j - 1));
            }
        }
    }

    std::vector<bool> matched(m, false);
    size_t i = n;
    size_t j = m;
    while (i > 0 && j > 0) {
        if (prev[i - 1] == curr[j - 1] && at(i, j) == static_cast<std::uint16_t>(at(i - 1, j - 1) + 1)) {
            matched[j - 1] = true;
            --i;
            --j;
        } else if (at(i - 1, j) >= at(i, j - 1)) {
            --i;
        } else {
            --j;
        }
    }

    std::vector<bool> changed(m, false);
    for (size_t k = 0; k < m; ++k) {
        changed[k] = !matched[k];
    }
    return changed;
}

}  // namespace

std::vector<std::string> split_logical_lines(const std::string& text) {
    return split_lines_lf(normalize_newlines_for_diff(text));
}

std::string normalize_newlines_for_diff(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\r') {
            if (i + 1 < text.size() && text[i + 1] == '\n') {
                ++i;
            }
            out.push_back('\n');
        } else {
            out.push_back(text[i]);
        }
    }
    return out;
}

std::vector<bool> mark_changed_lines(const std::string& previous,
                                     const std::string& current,
                                     size_t max_lines) {
    const std::string prev_norm = normalize_newlines_for_diff(previous);
    const std::string curr_norm = normalize_newlines_for_diff(current);
    if (prev_norm == curr_norm) {
        // Still need correct size for empty vs non-empty.
        const std::vector<std::string> curr_lines = split_lines_lf(curr_norm);
        return std::vector<bool>(curr_lines.size(), false);
    }

    const std::vector<std::string> prev_lines = split_lines_lf(prev_norm);
    const std::vector<std::string> curr_lines = split_lines_lf(curr_norm);

    if (max_lines == 0) {
        max_lines = kDefaultLineDiffMaxLines;
    }
    // Bound LCS memory: product of (n+1)*(m+1) uint16 cells under ~8MB (~4e6 cells).
    constexpr size_t kMaxCells = 4U * 1024U * 1024U;
    const size_t n = prev_lines.size();
    const size_t m = curr_lines.size();
    const bool too_large =
        n > max_lines || m > max_lines ||
        (n + 1) > kMaxCells / (m + 1);

    if (too_large) {
        // Coarse: every current line is considered changed when whole-file differs.
        return std::vector<bool>(m, true);
    }
    return mark_via_lcs(prev_lines, curr_lines);
}

size_t count_changed_lines(const std::vector<bool>& marks) {
    size_t count = 0;
    for (bool mark : marks) {
        if (mark) {
            ++count;
        }
    }
    return count;
}

}  // namespace ainiux::editor
