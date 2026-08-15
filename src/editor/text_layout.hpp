#pragma once

#include <cstddef>
#include <string>

#include "common.hpp"
#include "editor/editor.hpp"

namespace ainiux::editor {

enum class TextAlignMode {
    Left,
    Right,
    Center,
    Justify,
};

struct TextLayoutResult {
    Error error;
    std::string replacement;
    bool changed = false;
    size_t line_count = 0;
};

// Reflow paragraphs (blank-line separated) to WIDTH display cells and align.
// Fails without mutation when any whitespace-delimited word is wider than WIDTH.
// WIDTH must be in (kMinTextAlignWidthExclusive, kMaxTextAlignWidth].
TextLayoutResult reflow_align(const std::string& text, TextAlignMode mode, size_t width);

// Display-oriented reflow for chat/agent history: preserves fenced code and
// table lines (GFM pipes and Unicode box rows). Overlong words occupy their own
// line instead of failing. WIDTH may be any positive size (clamped by the caller).
TextLayoutResult reflow_align_display(const std::string& text, TextAlignMode mode, size_t width);

// True when width is a chat/session alignment width: -1 means unlimited (off),
// otherwise must be greater than 20 and at most kMaxTextAlignWidth.
bool valid_chat_align_width(long long width);

// Drop empty or whitespace-only lines.
TextLayoutResult remove_blank_lines(const std::string& text);

// Collapse consecutive blank lines to a single blank line.
TextLayoutResult remove_duplicate_blank_lines(const std::string& text);

// Collapse consecutive identical lines (uniq-style).
TextLayoutResult remove_duplicate_lines(const std::string& text);

const char* text_align_mode_name(TextAlignMode mode);
bool parse_text_align_mode(const std::string& name, TextAlignMode& out);

// True when width is a valid editor alignment width (21..1000).
bool valid_text_align_width(size_t width);

// Markdown fence / table line detectors used by display reflow and TUI wrap.
bool looks_like_fence_open(const std::string& line, std::string& fence);
bool looks_like_fence_close(const std::string& line, const std::string& fence);
bool looks_like_table_line(const std::string& line);

// Expand selection to full physical lines, or use the whole buffer.
// Outputs byte range [start, end) to replace.
void text_layout_scope(const EditorState& state, size_t& start, size_t& end, bool& had_selection);

// Apply a layout result over [start, end). Restores selection when had_selection.
Error apply_text_layout_result(EditorState& state,
                               size_t start,
                               size_t end,
                               bool had_selection,
                               const TextLayoutResult& result);

}  // namespace ainiux::editor
