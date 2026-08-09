#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include "editor/selection.hpp"

namespace ainiux::ui {

constexpr const char kTextSelectorStandardHint[] =
    "↑↓ move · / search · . sort · type to jump · Enter select · Esc cancel";
constexpr const char kTextSelectorThreadHint[] =
    "Newest first · Enter opens · Tab/Insert new · DEL delete · Esc cancels";
constexpr const char kTextSelectorBufferHint[] =
    "Buffers - Enter opens - / search - . sort - Tab/Insert new - DEL close - Esc cancels";
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

// Ephemeral navigation state for list pickers that support / search and . sort.
// last_search is intentionally kept across picker reopenings within one process.
struct TextSelectorNavState {
    bool search_active = false;
    std::string search_draft;
    std::string last_search;
    bool sorted = false;
    std::vector<std::string> original_items;
    std::vector<std::string> original_labels;
    std::vector<size_t> display_order;

    void reset_for_open();
    std::string draft_status() const;
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

// Case-insensitive ASCII-fold substring match (byte-wise, consistent with
// jump_text_selector_by_char). Empty needle matches nothing.
bool label_contains_ci(const std::string& label, const std::string& needle);

// Find-next substring match from selected+1 with wrap. Empty needle returns
// false. Returns true when the selection index changed.
bool find_next_text_selector_match(size_t& selected,
                                   size_t item_count,
                                   const std::function<std::string(size_t)>& label_at,
                                   const std::string& needle);

// Toggle alphabetical (UTF-8 / Unicode code-point) order of `items` by their
// own string values. First call stores `original_items` and sorts; second call
// restores. Keeps selection on the same element value when possible.
// Returns true when the order actually changed.
bool toggle_text_selector_alpha_sort(std::vector<std::string>& items,
                                     size_t& selected,
                                     bool& sorted,
                                     std::vector<std::string>& original_items);

// Like toggle_text_selector_alpha_sort, but sort keys come from label_at(i)
// against the current item order (e.g. provider display names).
bool toggle_text_selector_alpha_sort_by_label(
    std::vector<std::string>& items,
    size_t& selected,
    bool& sorted,
    std::vector<std::string>& original_items,
    const std::function<std::string(size_t)>& label_at);

// Parallel values + labels: sort by labels, reorder both in lockstep.
bool toggle_text_selector_alpha_sort_paired(std::vector<std::string>& values,
                                            std::vector<std::string>& labels,
                                            size_t& selected,
                                            bool& sorted,
                                            std::vector<std::string>& original_values,
                                            std::vector<std::string>& original_labels);

// Display-order permutation for non-reorderable underlying stores (buffers).
// order[i] = underlying index shown at display row i. Empty order means identity
// while unsorted. When sorting, fills order; when restoring, clears it.
// `selected` is a display-row index and is remapped to keep the same underlying item.
bool toggle_text_selector_alpha_sort_order(
    std::vector<size_t>& order,
    size_t& selected,
    bool& sorted,
    size_t item_count,
    const std::function<std::string(size_t)>& label_at_underlying);

// Map a display-row selection through an optional display_order permutation.
// Empty order, or size mismatch, is treated as identity.
size_t text_selector_underlying_index(const std::vector<size_t>& order,
                                      size_t display_selected,
                                      size_t item_count);

// Build an identity display order of length item_count.
void text_selector_identity_order(std::vector<size_t>& order, size_t item_count);

std::string text_selector_status(const std::string& label, size_t selected, size_t item_count);

// Status helpers for / search feedback.
std::string text_selector_no_match_status(const std::string& needle);
std::string text_selector_no_previous_search_status();

enum class SelectorMovementResult { Cancelled, Navigated };

SelectorMovementResult handle_selector_escape_sequence(const std::string& sequence,
                                                       size_t item_count,
                                                       size_t& selected,
                                                       std::string& status,
                                                       const std::string& selection_label);

}  // namespace ainiux::ui
