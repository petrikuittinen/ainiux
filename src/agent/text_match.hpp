#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace ainiux::agent {

// One match span inside the original haystack (byte offsets, UTF-8 safe as long as
// both needle and haystack are valid UTF-8; matching is byte-oriented).
struct TextSpan {
    std::size_t offset = 0;
    std::size_t length = 0;
    std::size_t start_line = 1;  // 1-based
    std::size_t end_line = 1;
};

// Result of the Gemini-style fallback order from PLANS.md §13:
// exact → normalized whitespace → leading-indent-stripped.
struct TextMatchResult {
    // "exact" | "normalized_whitespace" | "indent_stripped" | empty when none
    std::string mode;
    std::vector<TextSpan> matches;
};

// Find all non-overlapping? No: report all (possibly overlapping) candidates at the
// strongest successful mode. Overlapping is rare; callers disambiguate with
// replace_all / line_range_hint.
//
// When region_start/region_end are non-zero-width bounds inside haystack, only that
// substring is searched; reported offsets remain absolute in haystack.
TextMatchResult find_text_matches(const std::string& haystack,
                                  const std::string& needle,
                                  bool allow_fuzzy,
                                  std::size_t region_start = 0,
                                  std::size_t region_end = std::string::npos);

// Apply replacements. When replace_all is false, only the first span in matches is used
// (caller must already have disambiguated). Spans must be non-overlapping and sorted
// by offset ascending for replace_all.
std::string apply_text_replacements(const std::string& haystack,
                                    const std::vector<TextSpan>& matches,
                                    const std::string& replacement,
                                    bool replace_all,
                                    std::size_t& replacements_made);

// 1-based line number for a byte offset (1 if empty / offset 0).
std::size_t line_number_at(const std::string& text, std::size_t offset);

}  // namespace ainiux::agent
