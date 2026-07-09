#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include "chat/sqlite_store.hpp"
#include "tui/events.hpp"

namespace pkchat::tui {

struct TuiPickerCallbacks {
    std::function<void(const std::string&)> on_provider_selected;
    std::function<void(const std::string&)> on_model_selected;
    std::function<void(long long)> on_thread_selected;
    std::function<void()> on_thread_new;
    std::function<void()> on_remove_accepted;
    std::function<void()> on_remove_rejected;
    std::function<void(const std::string&)> on_remove_retry;
    std::function<void()> on_model_confirm_accepted;
    std::function<void()> on_model_confirm_rejected;
    std::function<void(const std::string&)> on_model_confirm_retry;
};

struct TuiPickerInputState {
    TuiMode& mode;
    bool& quit;
    std::string& status;
    std::vector<std::string>& picker_items;
    size_t& picker_selected;
    bool& picker_cancel_quits;
    std::vector<chat::ThreadSummary>& thread_picker_threads;
    size_t& thread_picker_selected;
    bool input_empty = true;
};

bool handle_tui_picker_input(unsigned char ch,
                             TuiPickerInputState& state,
                             const TuiPickerCallbacks& callbacks);

}  // namespace pkchat::tui