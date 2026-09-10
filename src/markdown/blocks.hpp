#pragma once

#include <string>
#include <vector>

namespace ainiux::markdown {

enum class RunStyle : unsigned {
    None = 0,
    Bold = 1,
    Italic = 2,
    Code = 4,
    Strike = 8,
};

inline constexpr unsigned operator|(RunStyle a, RunStyle b) {
    return static_cast<unsigned>(a) | static_cast<unsigned>(b);
}

struct Run {
    std::string text;
    unsigned style = 0;
    std::string url;
    bool hard_break_after = false;
};

enum class BlockKind {
    Paragraph,
    Heading,
    ListItem,
    Quote,
    Code,
    Table,
    Rule,
    Html,
};

struct Block {
    BlockKind kind = BlockKind::Paragraph;
    int heading_level = 0;
    bool ordered = false;
    int indent = 0;
    int list_index = 1;
    std::string text;
    std::vector<Run> runs;
    std::vector<Block> children;
    std::vector<std::vector<std::vector<Run>>> table_cells;
};

std::vector<Block> parse_blocks(const std::string& markdown);

}  // namespace ainiux::markdown
