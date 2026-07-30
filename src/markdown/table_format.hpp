#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace ainiux::markdown {

enum class TableStyle {
    UnicodeBox,
    PaddedGfm,
};

enum class TableAlign {
    Left,
    Center,
    Right,
};

struct TableFormatOptions {
    TableStyle style = TableStyle::UnicodeBox;
    // When true, format a GFM table that has a header + separator even if body
    // rows are still arriving (streaming assistant output).
    bool reformat_open_tables = true;
};

// Prefer Unicode box borders when the process locale looks UTF-8 capable;
// otherwise padded GFM pipes.
TableStyle default_table_style();

// Format a structured table. Aligns shorter than the column count default to Left.
// Empty headers with empty body yields an empty string.
std::string format_table(const std::vector<std::string>& headers,
                         const std::vector<TableAlign>& aligns,
                         const std::vector<std::vector<std::string>>& body_rows,
                         TableStyle style);

// Rewrite GFM pipe tables in text for human display. Fenced code is left alone.
// Unicode-box tables already present are left alone (idempotent for generators).
// Does not mutate model transcripts; callers use this for display or reports.
std::string pretty_format_tables(const std::string& text, const TableFormatOptions& options);

// Convenience: default style + open-table reformatting.
std::string pretty_format_tables(const std::string& text);

// Display-cell width of a single line (no newlines). Used by tests and formatters.
std::size_t table_display_width(const std::string& text);

}  // namespace ainiux::markdown
