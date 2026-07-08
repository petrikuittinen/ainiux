#pragma once

#include "common.hpp"
#include "editor/editor.hpp"

#include <chrono>
#include <string>

namespace pkchat::editor {

Error parse_byte_size(const std::string& text, long long& out);

std::string autosave_path_for(const std::string& path, const std::string& postfix);

bool autosave_allowed_for_buffer(const EditorState& state, const EditorSettings& settings);

struct AutosaveEvaluation {
    bool should_save = false;
    bool threshold_met = false;
    bool timeout_met = false;
};

AutosaveEvaluation evaluate_autosave(const EditorState& state,
                                     const EditorSettings& settings,
                                     std::chrono::steady_clock::duration idle_time);

Error perform_autosave(EditorState& state, const EditorSettings& settings, std::string& message);

void remove_autosave_file(const std::string& path, const EditorSettings& settings);

struct AutosaveRecoveryOffer {
    bool should_offer = false;
    std::string autosave_path;
};

AutosaveRecoveryOffer check_autosave_recovery_offer(const std::string& path,
                                                    const EditorSettings& settings);

std::string autosave_recovery_prompt_message(const std::string& path,
                                             const std::string& autosave_path);

bool confirm_autosave_recovery_before_terminal(const std::string& path,
                                               const std::string& autosave_path);

}  // namespace pkchat::editor