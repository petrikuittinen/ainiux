#include "ui/text_selector.hpp"

#include "editor/terminal_input.hpp"

#include <algorithm>
#include <sstream>
#include <utility>

namespace ainiux::ui {
namespace {

size_t clamp_selection(size_t selected, size_t item_count) {
    if (item_count == 0) {
        return 0;
    }
    return std::min(selected, item_count - 1);
}

bool is_printable_jump_char(unsigned char ch) {
    return ch >= 0x20U && ch != 0x7fU;
}

char ascii_lower_char(unsigned char ch) {
    if (ch >= 'A' && ch <= 'Z') {
        return static_cast<char>(ch - 'A' + 'a');
    }
    return static_cast<char>(ch);
}

bool label_contains_char_ci(const std::string& label, unsigned char ch) {
    const char needle = ascii_lower_char(ch);
    for (unsigned char byte : label) {
        if (ascii_lower_char(byte) == needle) {
            return true;
        }
    }
    return false;
}

std::string ascii_fold_copy(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (unsigned char byte : text) {
        out.push_back(ascii_lower_char(byte));
    }
    return out;
}

// Remap selection after reordering so the same element identity is selected.
// Prefer exact string match at old index if unique; otherwise first equal value.
size_t remap_selection_after_reorder(const std::vector<std::string>& before,
                                     size_t selected_before,
                                     const std::vector<std::string>& after) {
    if (before.empty() || after.empty()) {
        return 0;
    }
    selected_before = clamp_selection(selected_before, before.size());
    const std::string& target = before[selected_before];
    for (size_t i = 0; i < after.size(); ++i) {
        if (after[i] == target) {
            return i;
        }
    }
    return clamp_selection(selected_before, after.size());
}

std::vector<size_t> sorted_indices_by_label(size_t item_count,
                                            const std::function<std::string(size_t)>& label_at) {
    std::vector<size_t> indices(item_count);
    for (size_t i = 0; i < item_count; ++i) {
        indices[i] = i;
    }
    std::stable_sort(indices.begin(), indices.end(), [&](size_t a, size_t b) {
        return label_at(a) < label_at(b);
    });
    return indices;
}

template <typename T>
std::vector<T> permute_by_indices(const std::vector<T>& source, const std::vector<size_t>& indices) {
    std::vector<T> out;
    out.reserve(indices.size());
    for (size_t index : indices) {
        out.push_back(source[index]);
    }
    return out;
}

}  // namespace

void TextSelectorNavState::reset_for_open() {
    search_active = false;
    search_draft.clear();
    sorted = false;
    original_items.clear();
    original_labels.clear();
    display_order.clear();
}

std::string TextSelectorNavState::draft_status() const {
    return "/" + search_draft;
}

std::string render_text_selector(const TextSelectorConfig& config,
                                 size_t selected,
                                 const std::vector<std::string>& labels) {
    return render_text_selector(config, selected, labels.size(),
                                [&](size_t index) { return labels[index]; });
}

std::string render_text_selector(const TextSelectorConfig& config,
                                 size_t selected,
                                 size_t item_count,
                                 const std::function<std::string(size_t)>& label_at) {
    std::ostringstream out;
    out << config.header;
    if (!config.header.empty()) {
        out << '\n';
    }
    selected = clamp_selection(selected, item_count);
    for (size_t i = 0; i < item_count; ++i) {
        out << (i == selected ? config.selected_prefix : config.unselected_prefix);
        out << label_at(i);
        if (i + 1 != item_count) {
            out << '\n';
        }
    }
    return out.str();
}

size_t move_text_selector_selection(size_t selected,
                                    size_t item_count,
                                    editor::MovementKey key,
                                    size_t page_step) {
    if (item_count == 0) {
        return 0;
    }
    selected = clamp_selection(selected, item_count);
    switch (key) {
        case editor::MovementKey::Up:
            return selected == 0 ? 0 : selected - 1;
        case editor::MovementKey::Down:
            return std::min(item_count - 1, selected + 1);
        case editor::MovementKey::PageUp:
            return selected > page_step ? selected - page_step : 0;
        case editor::MovementKey::PageDown:
            return std::min(item_count - 1, selected + page_step);
        case editor::MovementKey::Home:
            return 0;
        case editor::MovementKey::End:
            return item_count - 1;
        case editor::MovementKey::Left:
        case editor::MovementKey::Right:
            break;
    }
    return selected;
}

bool jump_text_selector_by_char(size_t& selected,
                                size_t item_count,
                                const std::function<std::string(size_t)>& label_at,
                                unsigned char ch) {
    if (!is_printable_jump_char(ch) || item_count == 0 || !label_at) {
        return false;
    }
    selected = clamp_selection(selected, item_count);
    for (size_t step = 1; step <= item_count; ++step) {
        const size_t index = (selected + step) % item_count;
        if (label_contains_char_ci(label_at(index), ch)) {
            if (index == selected) {
                return false;
            }
            selected = index;
            return true;
        }
    }
    return false;
}

bool label_contains_ci(const std::string& label, const std::string& needle) {
    if (needle.empty()) {
        return false;
    }
    const std::string hay = ascii_fold_copy(label);
    const std::string ned = ascii_fold_copy(needle);
    return hay.find(ned) != std::string::npos;
}

bool find_next_text_selector_match(size_t& selected,
                                   size_t item_count,
                                   const std::function<std::string(size_t)>& label_at,
                                   const std::string& needle) {
    if (needle.empty() || item_count == 0 || !label_at) {
        return false;
    }
    selected = clamp_selection(selected, item_count);
    for (size_t step = 1; step <= item_count; ++step) {
        const size_t index = (selected + step) % item_count;
        if (label_contains_ci(label_at(index), needle)) {
            if (index == selected) {
                return false;
            }
            selected = index;
            return true;
        }
    }
    return false;
}

bool toggle_text_selector_alpha_sort(std::vector<std::string>& items,
                                     size_t& selected,
                                     bool& sorted,
                                     std::vector<std::string>& original_items) {
    return toggle_text_selector_alpha_sort_by_label(
        items, selected, sorted, original_items,
        [&](size_t index) { return items[index]; });
}

bool toggle_text_selector_alpha_sort_by_label(
    std::vector<std::string>& items,
    size_t& selected,
    bool& sorted,
    std::vector<std::string>& original_items,
    const std::function<std::string(size_t)>& label_at) {
    if (items.empty() || !label_at) {
        return false;
    }
    selected = clamp_selection(selected, items.size());
    if (sorted) {
        if (original_items.size() != items.size()) {
            sorted = false;
            original_items.clear();
            return false;
        }
        const std::vector<std::string> before = items;
        items = original_items;
        selected = remap_selection_after_reorder(before, selected, items);
        original_items.clear();
        sorted = false;
        return true;
    }
    original_items = items;
    const std::vector<size_t> indices = sorted_indices_by_label(items.size(), label_at);
    const std::vector<std::string> before = items;
    items = permute_by_indices(items, indices);
    selected = remap_selection_after_reorder(before, selected, items);
    sorted = true;
    return true;
}

bool toggle_text_selector_alpha_sort_paired(std::vector<std::string>& values,
                                            std::vector<std::string>& labels,
                                            size_t& selected,
                                            bool& sorted,
                                            std::vector<std::string>& original_values,
                                            std::vector<std::string>& original_labels) {
    if (values.empty() || values.size() != labels.size()) {
        return false;
    }
    selected = clamp_selection(selected, values.size());
    if (sorted) {
        if (original_values.size() != values.size() || original_labels.size() != labels.size()) {
            sorted = false;
            original_values.clear();
            original_labels.clear();
            return false;
        }
        const std::vector<std::string> before = values;
        values = original_values;
        labels = original_labels;
        selected = remap_selection_after_reorder(before, selected, values);
        original_values.clear();
        original_labels.clear();
        sorted = false;
        return true;
    }
    original_values = values;
    original_labels = labels;
    const std::vector<size_t> indices =
        sorted_indices_by_label(labels.size(), [&](size_t index) { return labels[index]; });
    const std::vector<std::string> before = values;
    values = permute_by_indices(values, indices);
    labels = permute_by_indices(labels, indices);
    selected = remap_selection_after_reorder(before, selected, values);
    sorted = true;
    return true;
}

bool toggle_text_selector_alpha_sort_order(
    std::vector<size_t>& order,
    size_t& selected,
    bool& sorted,
    size_t item_count,
    const std::function<std::string(size_t)>& label_at_underlying) {
    if (item_count == 0 || !label_at_underlying) {
        return false;
    }
    selected = clamp_selection(selected, item_count);
    if (sorted) {
        // selected is a display row into the sorted order; recover underlying id then map to identity.
        size_t underlying = selected;
        if (order.size() == item_count) {
            underlying = order[selected];
        }
        order.clear();
        selected = clamp_selection(underlying, item_count);
        sorted = false;
        return true;
    }
    // Current display order: use existing order if present, else identity.
    std::vector<size_t> current;
    if (order.size() == item_count) {
        current = order;
    } else {
        text_selector_identity_order(current, item_count);
    }
    const size_t underlying_selected = current[selected];
    // Sort by label of underlying indices, stable for ties.
    std::vector<size_t> next = current;
    std::stable_sort(next.begin(), next.end(), [&](size_t a, size_t b) {
        return label_at_underlying(a) < label_at_underlying(b);
    });
    order = std::move(next);
    selected = 0;
    for (size_t i = 0; i < order.size(); ++i) {
        if (order[i] == underlying_selected) {
            selected = i;
            break;
        }
    }
    sorted = true;
    return true;
}

size_t text_selector_underlying_index(const std::vector<size_t>& order,
                                      size_t display_selected,
                                      size_t item_count) {
    if (item_count == 0) {
        return 0;
    }
    display_selected = clamp_selection(display_selected, item_count);
    if (order.size() != item_count) {
        return display_selected;
    }
    return order[display_selected];
}

void text_selector_identity_order(std::vector<size_t>& order, size_t item_count) {
    order.resize(item_count);
    for (size_t i = 0; i < item_count; ++i) {
        order[i] = i;
    }
}

std::string text_selector_status(const std::string& label, size_t selected, size_t item_count) {
    if (item_count == 0) {
        return label;
    }
    return label + " " + std::to_string(clamp_selection(selected, item_count) + 1) + "/" +
           std::to_string(item_count);
}

std::string text_selector_no_match_status(const std::string& needle) {
    return "No match for '" + needle + "'";
}

std::string text_selector_no_previous_search_status() {
    return "No previous search";
}

SelectorMovementResult handle_selector_escape_sequence(const std::string& sequence,
                                                       size_t item_count,
                                                       size_t& selected,
                                                       std::string& status,
                                                       const std::string& selection_label) {
    if (sequence.empty()) {
        return SelectorMovementResult::Cancelled;
    }
    editor::MovementKeyEvent movement;
    if (!editor::parse_movement_sequence(sequence, movement) || item_count == 0) {
        return SelectorMovementResult::Cancelled;
    }
    selected = move_text_selector_selection(selected, item_count, movement.key);
    status = text_selector_status(selection_label, selected, item_count);
    return SelectorMovementResult::Navigated;
}

}  // namespace ainiux::ui
