#include "tui/picker_input.hpp"

#include "provider/provider.hpp"
#include "tui/agent_widgets.hpp"
#include "tui/input_handlers.hpp"
#include "ui/confirmation.hpp"
#include "ui/text_selector.hpp"

namespace ainiux::tui {

namespace {

const char* list_picker_selection_label(TuiMode mode) {
    switch (mode) {
        case TuiMode::ProviderList:
            return "Selected provider";
        case TuiMode::ModelList:
            return "Selected model";
        case TuiMode::ReasoningList:
            return "Selected reasoning";
        default:
            return "Selected item";
    }
}

std::string list_picker_label_at(const TuiPickerInputState& state, size_t index) {
    if (index >= state.picker_items.size()) {
        return {};
    }
    if (state.mode == TuiMode::ProviderList) {
        return provider::display_name_for_profile(state.picker_items[index]);
    }
    return state.picker_items[index];
}

bool is_printable_search_char(unsigned char ch) {
    return ch >= 0x20U && ch != 0x7fU;
}

// While search draft is active, Esc cancels the draft. Arrow keys cancel the
// draft and move the selection (escape sequence is fully consumed).
bool handle_search_draft_escape(TuiPickerInputState& state) {
    ui::TextSelectorNavState& nav = state.picker_nav;
    const char* selection_label = list_picker_selection_label(state.mode);
    nav.search_active = false;
    nav.search_draft.clear();

    const PickerEscapeResult result = handle_list_picker_escape(
        state.picker_items.size(), state.picker_selected, state.status, selection_label);
    if (result == PickerEscapeResult::Navigated) {
        // Movement already updated status.
        return true;
    }
    // Bare Esc / unknown: leave picker open with draft cancelled.
    state.status = ui::text_selector_status(selection_label, state.picker_selected,
                                            state.picker_items.size());
    return true;
}

bool handle_list_picker_search_and_sort(unsigned char ch, TuiPickerInputState& state) {
    ui::TextSelectorNavState& nav = state.picker_nav;
    auto label_at = [&](size_t index) { return list_picker_label_at(state, index); };
    const char* selection_label = list_picker_selection_label(state.mode);

    if (nav.search_active) {
        if (ch == 27) {
            return handle_search_draft_escape(state);
        }
        if (ch == '\r' || ch == '\n') {
            const std::string needle =
                nav.search_draft.empty() ? nav.last_search : nav.search_draft;
            nav.search_active = false;
            nav.search_draft.clear();
            if (needle.empty()) {
                state.status = ui::text_selector_no_previous_search_status();
                return true;
            }
            nav.last_search = needle;
            if (ui::find_next_text_selector_match(state.picker_selected, state.picker_items.size(),
                                                  label_at, needle)) {
                state.status = ui::text_selector_status(selection_label, state.picker_selected,
                                                        state.picker_items.size());
            } else {
                state.status = ui::text_selector_no_match_status(needle);
            }
            return true;
        }
        if (ch == 127 || ch == 8) {
            if (!nav.search_draft.empty()) {
                nav.search_draft.pop_back();
            }
            state.status = nav.draft_status();
            return true;
        }
        if (is_printable_search_char(ch)) {
            nav.search_draft.push_back(static_cast<char>(ch));
            state.status = nav.draft_status();
            return true;
        }
        return true;
    }

    if (ch == '/') {
        nav.search_active = true;
        nav.search_draft.clear();
        state.status = nav.draft_status();
        return true;
    }

    if (ch == '.') {
        if (state.mode == TuiMode::ProviderList) {
            ui::toggle_text_selector_alpha_sort_by_label(
                state.picker_items, state.picker_selected, nav.sorted, nav.original_items, label_at);
        } else {
            ui::toggle_text_selector_alpha_sort(state.picker_items, state.picker_selected, nav.sorted,
                                               nav.original_items);
        }
        state.status =
            ui::text_selector_status(selection_label, state.picker_selected, state.picker_items.size());
        return true;
    }

    return false;
}

}  // namespace

bool handle_tui_picker_input(unsigned char ch,
                             TuiPickerInputState& state,
                             const TuiPickerCallbacks& callbacks) {
    if (state.mode == TuiMode::ProviderList || state.mode == TuiMode::ModelList ||
        state.mode == TuiMode::ReasoningList) {
        if (ch == 17) {
            state.quit = true;
            return true;
        }
        // Search draft must handle Esc before picker-cancel Esc (so bare Esc only
        // leaves the draft, while arrows move after cancelling the draft).
        if (state.picker_nav.search_active) {
            return handle_list_picker_search_and_sort(ch, state);
        }
        if (ch == 27) {
            const std::string selection_label = list_picker_selection_label(state.mode);
            const PickerEscapeResult result = handle_list_picker_escape(
                state.picker_items.size(), state.picker_selected, state.status, selection_label);
            if (result == PickerEscapeResult::Cancelled) {
                const bool provider_picker = state.mode == TuiMode::ProviderList;
                const bool reasoning_picker = state.mode == TuiMode::ReasoningList;
                state.picker_items.clear();
                state.picker_selected = 0;
                state.picker_nav.reset_for_open();
                if (state.picker_cancel_quits) {
                    state.quit = true;
                } else {
                    state.mode = TuiMode::Chat;
                }
                state.status = provider_picker
                                   ? "Provider selection cancelled"
                                   : reasoning_picker ? "Reasoning selection cancelled"
                                                      : "Model selection cancelled";
            }
            return true;
        }
        if (handle_list_picker_search_and_sort(ch, state)) {
            return true;
        }
        if (ch == '\r' || ch == '\n') {
            if (state.picker_selected < state.picker_items.size()) {
                if (state.mode == TuiMode::ProviderList) {
                    callbacks.on_provider_selected(state.picker_items[state.picker_selected]);
                } else if (state.mode == TuiMode::ModelList) {
                    callbacks.on_model_selected(state.picker_items[state.picker_selected]);
                } else {
                    callbacks.on_reasoning_selected(state.picker_items[state.picker_selected]);
                }
            }
            return true;
        }
        if (ui::jump_text_selector_by_char(
                state.picker_selected,
                state.picker_items.size(),
                [&](size_t index) { return list_picker_label_at(state, index); },
                ch)) {
            state.status = ui::text_selector_status(list_picker_selection_label(state.mode),
                                                    state.picker_selected,
                                                    state.picker_items.size());
        }
        return true;
    }
    if (state.mode == TuiMode::ThreadList) {
        if (ch == 17) {
            state.quit = true;
            return true;
        }
        if (ch == 27) {
            const PickerEscapeResult res =
                handle_thread_list_escape(state.thread_picker_threads,
                                          state.thread_picker_selected,
                                          state.status,
                                          state.pending_thread_delete,
                                          state.mode,
                                          !state.agent_mode);
            if (res == PickerEscapeResult::Cancelled) {
                state.mode = TuiMode::Chat;
                state.status = "Thread list cancelled";
                if (callbacks.on_thread_list_cancelled) {
                    callbacks.on_thread_list_cancelled();
                }
            } else if (res == PickerEscapeResult::CreateNew) {
                if (callbacks.on_thread_new) {
                    callbacks.on_thread_new();
                }
            }
            return true;
        }
        if (ch == '\r' || ch == '\n') {
            if (state.thread_picker_selected < state.thread_picker_threads.size()) {
                callbacks.on_thread_selected(state.thread_picker_threads[state.thread_picker_selected].id);
            }
            return true;
        }
        // Tab/Insert create a new chat thread. Agent mode requires explicit /new.
        if (ch == '\t') {
            if (!state.agent_mode && callbacks.on_thread_new) {
                callbacks.on_thread_new();
            }
            return true;
        }
        // DEL deletes the selected thread. Ctrl+H is help (handled by the TUI loop).
        if (ch == 127) {
            if (state.thread_picker_selected < state.thread_picker_threads.size()) {
                state.pending_thread_delete = state.thread_picker_selected;
                state.mode = TuiMode::ThreadDeleteConfirm;
                state.status = "Delete thread? y/n (Esc cancels)";
            }
            return true;
        }
        if (ch == 8) {
            return false;
        }
        if (ui::jump_text_selector_by_char(
                state.thread_picker_selected,
                state.thread_picker_threads.size(),
                [&](size_t index) {
                    return thread_picker_label(state.thread_picker_threads[index]);
                },
                ch)) {
            state.status = ui::text_selector_status("Selected thread",
                                                    state.thread_picker_selected,
                                                    state.thread_picker_threads.size());
        }
        return true;
    }
    if (state.mode == TuiMode::ThreadDeleteConfirm) {
        if (ch == 17) {
            state.quit = true;
            return true;
        }
        switch (ui::parse_confirmation_key(ch)) {
            case ui::ConfirmationKeyResult::Accepted:
                callbacks.on_thread_delete_accepted();
                return true;
            case ui::ConfirmationKeyResult::Rejected:
                callbacks.on_thread_delete_rejected();
                return true;
            case ui::ConfirmationKeyResult::Pending:
                callbacks.on_thread_delete_retry("Press y to delete, n or Esc to cancel");
                return true;
        }
    }
    if (state.mode == TuiMode::ReasoningConfirm) {
        if (ch == 17) {
            state.quit = true;
            return true;
        }
        if (state.agent_mode) {
            const InlineChoiceResult choice =
                parse_inline_choice_key(agent_inline_choices_for_mode(state.mode), ch);
            if (!choice.matched) return true;
            if (choice.index == 0) callbacks.on_reasoning_confirm_accepted();
            else callbacks.on_reasoning_confirm_rejected();
            return true;
        }
        switch (ui::parse_confirmation_key(ch)) {
            case ui::ConfirmationKeyResult::Accepted:
                callbacks.on_reasoning_confirm_accepted();
                return true;
            case ui::ConfirmationKeyResult::Rejected:
                callbacks.on_reasoning_confirm_rejected();
                return true;
            case ui::ConfirmationKeyResult::Pending:
                callbacks.on_reasoning_confirm_retry(
                    "Press y to use the unlisted reasoning value, n or Esc to cancel");
                return true;
        }
    }
    if (state.mode == TuiMode::RemoveConfirm) {
        if (ch == 17) {
            state.quit = true;
            return true;
        }
        if (state.agent_mode) {
            const InlineChoiceResult choice =
                parse_inline_choice_key(agent_inline_choices_for_mode(state.mode), ch);
            if (!choice.matched) return true;
            if (choice.index == 0) callbacks.on_remove_accepted();
            else callbacks.on_remove_rejected();
            return true;
        }
        switch (ui::parse_confirmation_key(ch)) {
            case ui::ConfirmationKeyResult::Accepted:
                callbacks.on_remove_accepted();
                return true;
            case ui::ConfirmationKeyResult::Rejected:
                callbacks.on_remove_rejected();
                return true;
            case ui::ConfirmationKeyResult::Pending:
                callbacks.on_remove_retry("Press y to remove, n or Esc to cancel");
                return true;
        }
    }
    if (state.mode == TuiMode::ModelConfirm) {
        if (ch == 17) {
            state.quit = true;
            return true;
        }
        if (!state.input_empty) {
            return false;
        }
        if (state.agent_mode) {
            const InlineChoiceResult choice =
                parse_inline_choice_key(agent_inline_choices_for_mode(state.mode), ch);
            if (!choice.matched) return true;
            if (choice.index == 0) callbacks.on_model_confirm_accepted();
            else callbacks.on_model_confirm_rejected();
            return true;
        }
        switch (ui::parse_confirmation_key(ch)) {
            case ui::ConfirmationKeyResult::Accepted:
                callbacks.on_model_confirm_accepted();
                return true;
            case ui::ConfirmationKeyResult::Rejected:
                callbacks.on_model_confirm_rejected();
                return true;
            case ui::ConfirmationKeyResult::Pending:
                if (ch != '/' && ch != '\r' && ch != '\n' && ch < 32) {
                    callbacks.on_model_confirm_retry(
                        "Press y to keep current provider/model, n or Esc to use thread model");
                }
                return true;
        }
    }
    if (state.mode == TuiMode::GuardApprovalConfirm) {
        if (ch == 17) {
            state.quit = true;
            return true;
        }
        if (ch == 27) {
            const PickerEscapeResult result =
                handle_guard_approval_escape(state.history_scroll);
            if (result == PickerEscapeResult::Navigated) return true;
            if (result == PickerEscapeResult::OpenDired) {
                if (callbacks.on_guard_approval_review)
                    callbacks.on_guard_approval_review();
                return true;
            }
            if (callbacks.on_guard_approval_rejected)
                callbacks.on_guard_approval_rejected();
            return true;
        }
        const InlineChoiceResult choice = parse_inline_choice_key(
            agent_guard_approval_choices(state.guard_can_review), ch);
        if (!choice.matched) return true;
        switch (choice.index) {
            case 0:
                if (callbacks.on_guard_approval_accepted) callbacks.on_guard_approval_accepted();
                return true;
            case 1:
                if (callbacks.on_guard_approval_rejected) callbacks.on_guard_approval_rejected();
                return true;
            case 2:
                if (callbacks.on_guard_approval_review) callbacks.on_guard_approval_review();
                return true;
            default:
                return true;
        }
    }
    if (state.mode == TuiMode::AgentPermissionSelect) {
        if (ch == 17) {
            state.quit = true;
            return true;
        }
        const InlineChoiceResult choice =
            parse_inline_choice_key(agent_inline_choices_for_mode(state.mode), ch);
        if (!choice.matched) return true;
        if (callbacks.on_agent_permission_selected)
            callbacks.on_agent_permission_selected(choice.index);
        return true;
    }
    if (state.mode == TuiMode::AgentContinueConfirm) {
        if (ch == 17) {
            state.quit = true;
            return true;
        }
        const InlineChoiceResult choice =
            parse_inline_choice_key(agent_inline_choices_for_mode(state.mode), ch);
        if (!choice.matched) return true;
        if (choice.index == 0) {
            if (callbacks.on_agent_continue_accepted)
                callbacks.on_agent_continue_accepted();
        } else if (callbacks.on_agent_continue_rejected) {
            callbacks.on_agent_continue_rejected();
        }
        return true;
    }
    if (state.mode == TuiMode::AgentNewConfirm) {
        if (ch == 17) {
            state.quit = true;
            return true;
        }
        const InlineChoiceResult choice =
            parse_inline_choice_key(agent_inline_choices_for_mode(state.mode), ch);
        if (!choice.matched) return true;
        switch (choice.index) {
            case 0:
                if (callbacks.on_agent_new_accepted) callbacks.on_agent_new_accepted();
                return true;
            case 1:
                if (callbacks.on_agent_new_rejected) callbacks.on_agent_new_rejected();
                return true;
            default:
                return true;
        }
    }
    if (state.mode == TuiMode::AgentIndexBuildConfirm) {
        if (ch == 17) {
            state.quit = true;
            return true;
        }
        const InlineChoiceResult choice =
            parse_inline_choice_key(agent_inline_choices_for_mode(state.mode), ch);
        if (!choice.matched) return true;
        if (choice.index == 0) {
            if (callbacks.on_agent_index_build_accepted)
                callbacks.on_agent_index_build_accepted();
        } else if (callbacks.on_agent_index_build_rejected) {
            callbacks.on_agent_index_build_rejected();
        }
        return true;
    }
    return false;
}

}  // namespace ainiux::tui
