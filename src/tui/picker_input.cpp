#include "tui/picker_input.hpp"

#include "tui/agent_widgets.hpp"
#include "tui/input_handlers.hpp"
#include "ui/confirmation.hpp"

namespace ainiux::tui {

bool handle_tui_picker_input(unsigned char ch,
                             TuiPickerInputState& state,
                             const TuiPickerCallbacks& callbacks) {
    if (state.mode == TuiMode::ProviderList || state.mode == TuiMode::ModelList ||
        state.mode == TuiMode::ReasoningList) {
        if (ch == 17) {
            state.quit = true;
            return true;
        }
        if (ch == 27) {
            const std::string selection_label = state.mode == TuiMode::ProviderList
                                                    ? "Selected provider"
                                                    : state.mode == TuiMode::ModelList
                                                          ? "Selected model"
                                                          : "Selected reasoning";
            const PickerEscapeResult result = handle_list_picker_escape(
                state.picker_items.size(), state.picker_selected, state.status, selection_label);
            if (result == PickerEscapeResult::Cancelled) {
                const bool provider_picker = state.mode == TuiMode::ProviderList;
                const bool reasoning_picker = state.mode == TuiMode::ReasoningList;
                state.picker_items.clear();
                state.picker_selected = 0;
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
                                          state.mode);
            if (res == PickerEscapeResult::Cancelled) {
                state.mode = TuiMode::Chat;
                state.status = "Thread list cancelled";
                if (callbacks.on_thread_list_cancelled) {
                    callbacks.on_thread_list_cancelled();
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
        if (ch == 'n' || ch == 'N') {
            callbacks.on_thread_new();
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
        const InlineChoiceResult choice =
            parse_inline_choice_key(agent_inline_choices_for_mode(state.mode), ch);
        if (!choice.matched) return true;
        switch (choice.index) {
            case 0:
                if (callbacks.on_guard_approval_accepted) callbacks.on_guard_approval_accepted();
                return true;
            case 1:
                if (callbacks.on_guard_approval_rejected) callbacks.on_guard_approval_rejected();
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
