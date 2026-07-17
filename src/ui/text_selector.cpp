#include "ui/text_selector.hpp"

#include "editor/terminal_input.hpp"

#include <algorithm>
#include <sstream>

namespace ainiux::ui {
namespace {

size_t clamp_selection(size_t selected, size_t item_count) {
    if (item_count == 0) {
        return 0;
    }
    return std::min(selected, item_count - 1);
}

}  // namespace

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

std::string text_selector_status(const std::string& label, size_t selected, size_t item_count) {
    if (item_count == 0) {
        return label;
    }
    return label + " " + std::to_string(clamp_selection(selected, item_count) + 1) + "/" +
           std::to_string(item_count);
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