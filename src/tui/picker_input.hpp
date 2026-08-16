#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include "chat/sqlite_store.hpp"
#include "tui/events.hpp"
#include "ui/text_selector.hpp"

namespace ainiux::tui {

struct TuiPickerCallbacks {
    std::function<void(const std::string&)> on_provider_selected;
    std::function<void(const std::string&)> on_model_selected;
    std::function<void(const std::string&)> on_reasoning_selected;
    std::function<void()> on_reasoning_confirm_accepted;
    std::function<void()> on_reasoning_confirm_rejected;
    std::function<void(const std::string&)> on_reasoning_confirm_retry;
    std::function<void(long long)> on_thread_selected;
    std::function<void()> on_thread_new;
    std::function<void()> on_thread_list_cancelled;
    std::function<void()> on_remove_accepted;
    std::function<void()> on_remove_rejected;
    std::function<void(const std::string&)> on_remove_retry;
    std::function<void()> on_thread_delete_accepted;
    std::function<void()> on_thread_delete_rejected;
    std::function<void(const std::string&)> on_thread_delete_retry;
    std::function<void()> on_model_confirm_accepted;
    std::function<void()> on_model_confirm_rejected;
    std::function<void(const std::string&)> on_model_confirm_retry;
    std::function<void()> on_guard_approval_accepted;
    std::function<void()> on_guard_approval_rejected;
    std::function<void(const std::string&)> on_guard_approval_retry;
    std::function<void(size_t)> on_agent_permission_selected;
    std::function<void()> on_agent_continue_accepted;
    std::function<void()> on_agent_continue_rejected;
    std::function<void()> on_agent_new_accepted;
    std::function<void()> on_agent_new_rejected;
    std::function<void(const std::string&)> on_agent_new_retry;
    std::function<void()> on_agent_index_build_accepted;
    std::function<void()> on_agent_index_build_rejected;
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
    size_t& pending_thread_delete;
    bool agent_mode = false;
    ui::TextSelectorNavState& picker_nav;
    int* history_scroll = nullptr;
};

bool handle_tui_picker_input(unsigned char ch,
                             TuiPickerInputState& state,
                             const TuiPickerCallbacks& callbacks);

}  // namespace ainiux::tui
