#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include "editor/selection.hpp"

namespace ainiux::ui {

constexpr const char kTextSelectorStandardHint[] =
    "↑↓ move · type to jump · Enter select · Esc cancel";
constexpr const char kTextSelectorThreadHint[] =
    "Newest first · Enter opens · Tab/Insert new · DEL delete · Esc cancels";
constexpr const char kTextSelectorBufferHint[] =
    "Buffers - Enter opens - Tab/Insert new - DEL close - Esc cancels";
constexpr const char kTextSelectorAttachmentHint[] = "↑↓ move · DEL delete · Esc close";

constexpr const char kTextSelectorArrowPrefix[] = u8"› ";
constexpr const char kTextSelectorCaretPrefix[] = "> ";
constexpr const char kTextSelectorUnselectedPrefix[] = "  ";

constexpr size_t kTextSelectorDefaultPageStep = 10;

struct TextSelectorConfig {
    std::string header;
    std::string selected_prefix = kTextSelectorArrowPrefix;
    std::string unselected_prefix = kTextSelectorUnselectedPrefix;
};

std::string render_text_selector(const TextSelectorConfig& config,
                                 size_t selected,
                                 const std::vector<std::string>& labels);

std::string render_text_selector(const TextSelectorConfig& config,
                                 size_t selected,
                                 size_t item_count,
                                 const std::function<std::string(size_t)>& label_at);

size_t move_text_selector_selection(size_t selected,
                                    size_t item_count,
                                    editor::MovementKey key,
                                    size_t page_step = kTextSelectorDefaultPageStep);

// Case-insensitive contains match on displayed labels. Searches forward from
// the item after `selected`, wrapping to the start. Printable chars only;
// control keys (Tab, Esc, Ctrl+*) return false without changing selection.
// Returns true when the selection index changed.
bool jump_text_selector_by_char(size_t& selected,
                                size_t item_count,
                                const std::function<std::string(size_t)>& label_at,
                                unsigned char ch);

std::string text_selector_status(const std::string& label, size_t selected, size_t item_count);

enum class SelectorMovementResult { Cancelled, Navigated };

SelectorMovementResult handle_selector_escape_sequence(const std::string& sequence,
                                                       size_t item_count,
                                                       size_t& selected,
                                                       std::string& status,
                                                       const std::string& selection_label);

}  // namespace ainiux::ui