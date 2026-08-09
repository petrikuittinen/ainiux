#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "editor/editor.hpp"
#include "ui/text_selector.hpp"

namespace ainiux::editor {

struct EditorProviderModelPicker {
    bool active = false;
    bool for_provider = false;
    bool for_reasoning = false;
    std::vector<std::string> items;
    std::vector<std::string> display_labels;
    size_t selected = 0;
    int scroll = 0;
    EditorState view;
    ui::TextSelectorNavState nav;

    std::string selection_label() const;
    std::string cancel_message() const;
    std::string empty_message() const;
    std::string status_message() const;
    void refresh_view();
    void open_providers();
    void open_models(std::vector<std::string> models);
    void open_reasoning(std::vector<std::string> values,
                        std::vector<std::string> labels,
                        size_t current);
    void clear();
    bool handle_escape(const std::string& sequence, std::string& status_out);
    // Type-ahead jump by character against displayed labels. Returns true if
    // selection moved (caller should refresh_view and update status).
    bool handle_jump_char(unsigned char ch, std::string& status_out);
    // Handle / search draft, . sort, and related keys. Returns true when the key
    // was consumed (caller should refresh_view when the list order or selection
    // may have changed).
    bool handle_search_sort_char(unsigned char ch, std::string& status_out);
    std::string label_at(size_t index) const;
};

}  // namespace ainiux::editor
