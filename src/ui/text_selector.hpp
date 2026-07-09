#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include "editor/selection.hpp"

namespace pkchat::ui {

constexpr const char kTextSelectorStandardHint[] = "↑↓ move · Enter select · Esc cancel";
constexpr const char kTextSelectorThreadHint[] = "Newest first · Enter opens · N new · Esc cancels";
constexpr const char kTextSelectorBufferHint[] = "Buffers - Enter opens - N new - Esc cancels";

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

std::string text_selector_status(const std::string& label, size_t selected, size_t item_count);

}  // namespace pkchat::ui