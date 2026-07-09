#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "editor/editor.hpp"

namespace pkchat::editor {

struct EditorProviderModelPicker {
    bool active = false;
    bool for_provider = false;
    std::vector<std::string> items;
    size_t selected = 0;
    EditorState view;

    std::string selection_label() const;
    std::string cancel_message() const;
    std::string empty_message() const;
    std::string status_message() const;
    void refresh_view();
    void open_providers();
    void open_models(std::vector<std::string> models);
    void clear();
    bool handle_escape(const std::string& sequence, std::string& status_out);
};

}  // namespace pkchat::editor