#include "editor/autosave.hpp"

#include "editor/detail/editor_common.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <iostream>
#include <limits>

namespace pkchat::editor {
namespace {

bool parse_positive_digits(const std::string& text, long long& out) {
    if (text.empty()) {
        return false;
    }
    long long value = 0;
    for (char ch : text) {
        if (ch < '0' || ch > '9') {
            return false;
        }
        const int digit = ch - '0';
        if (value > (std::numeric_limits<long long>::max() - digit) / 10) {
            return false;
        }
        value = value * 10 + digit;
    }
    out = value;
    return true;
}

}  // namespace

Error parse_byte_size(const std::string& text, long long& out) {
    const std::string trimmed = [&]() {
        size_t begin = 0;
        size_t end = text.size();
        while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) {
            ++begin;
        }
        while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) {
            --end;
        }
        return text.substr(begin, end - begin);
    }();
    if (trimmed.empty()) {
        return {ErrorCode::BadArgs, "byte size value is empty"};
    }

    long long multiplier = 1;
    std::string digits = trimmed;
    if (digits.size() >= 2) {
        const char suffix = digits.back();
        if (suffix == 'k' || suffix == 'K') {
            multiplier = 1024LL;
            digits.pop_back();
        } else if (suffix == 'm' || suffix == 'M') {
            multiplier = 1024LL * 1024LL;
            digits.pop_back();
        } else if (suffix == 'g' || suffix == 'G') {
            multiplier = 1024LL * 1024LL * 1024LL;
            digits.pop_back();
        } else if (suffix == 't' || suffix == 'T') {
            multiplier = 1024LL * 1024LL * 1024LL * 1024LL;
            digits.pop_back();
        }
    }

    long long value = 0;
    if (!parse_positive_digits(digits, value)) {
        return {ErrorCode::BadArgs,
                "expected a byte size such as 300, 10M, or 1G with optional k/M/G/T suffix"};
    }
    if (value > std::numeric_limits<long long>::max() / multiplier) {
        return {ErrorCode::BadArgs, "byte size is too large"};
    }
    out = value * multiplier;
    return ok_error();
}

std::string autosave_path_for(const std::string& path, const std::string& postfix) {
    if (path.empty() || postfix.empty()) {
        return path;
    }
    return path + postfix;
}

bool autosave_allowed_for_buffer(const EditorState& state, const EditorSettings& settings) {
    if (!settings.auto_save_mode || state.path.empty() || !state.dirty) {
        return false;
    }
    if (settings.auto_save_size_limit >= 0 &&
        static_cast<long long>(state.text.size()) > settings.auto_save_size_limit) {
        return false;
    }
    return true;
}

AutosaveEvaluation evaluate_autosave(const EditorState& state,
                                     const EditorSettings& settings,
                                     std::chrono::steady_clock::duration idle_time) {
    AutosaveEvaluation result;
    if (!autosave_allowed_for_buffer(state, settings) || state.autosave_pending_bytes() == 0) {
        return result;
    }

    if (settings.auto_save_threshold > 0 &&
        state.autosave_pending_bytes() >= settings.auto_save_threshold) {
        result.threshold_met = true;
        result.should_save = true;
    }

    if (settings.auto_save_timeout_seconds > 0 &&
        idle_time >= std::chrono::seconds(settings.auto_save_timeout_seconds)) {
        result.timeout_met = true;
        result.should_save = true;
    }

    return result;
}

Error perform_autosave(EditorState& state, const EditorSettings& settings, std::string& message) {
    message.clear();
    if (!autosave_allowed_for_buffer(state, settings)) {
        if (settings.auto_save_mode && state.dirty && !state.path.empty() &&
            settings.auto_save_size_limit >= 0 &&
            static_cast<long long>(state.text.size()) > settings.auto_save_size_limit) {
            message = "Auto-save skipped: buffer exceeds auto_save_size_limit";
        }
        return {ErrorCode::UnsupportedFeature, "auto-save is not applicable for the current buffer"};
    }
    if (state.autosave_pending_bytes() == 0) {
        return {ErrorCode::UnsupportedFeature, "auto-save has no pending changes"};
    }

    const std::string backup_path = autosave_path_for(state.path, settings.auto_save_postfix);
    Error save_error = save_file(backup_path, state.text);
    if (!save_error.ok()) {
        message = save_error.message;
        return save_error;
    }
    state.reset_autosave_pending();
    message = "Auto-saved " + backup_path;
    return ok_error();
}

void remove_autosave_file(const std::string& path, const EditorSettings& settings) {
    if (path.empty() || settings.auto_save_postfix.empty()) {
        return;
    }
    const std::string backup_path = autosave_path_for(path, settings.auto_save_postfix);
    std::error_code filesystem_error;
    std::filesystem::remove(backup_path, filesystem_error);
}

AutosaveRecoveryOffer check_autosave_recovery_offer(const std::string& path,
                                                    const EditorSettings& settings) {
    AutosaveRecoveryOffer offer;
    if (!settings.auto_save_mode || path.empty() || settings.auto_save_postfix.empty()) {
        return offer;
    }

    const std::string backup_path = autosave_path_for(path, settings.auto_save_postfix);
    std::error_code filesystem_error;
    const std::filesystem::file_status main_status = std::filesystem::status(path, filesystem_error);
    if (filesystem_error || !std::filesystem::is_regular_file(main_status)) {
        return offer;
    }
    const std::filesystem::file_status backup_status =
        std::filesystem::status(backup_path, filesystem_error);
    if (filesystem_error || !std::filesystem::is_regular_file(backup_status)) {
        return offer;
    }

    const std::filesystem::file_time_type main_time =
        std::filesystem::last_write_time(path, filesystem_error);
    if (filesystem_error) {
        return offer;
    }
    const std::filesystem::file_time_type backup_time =
        std::filesystem::last_write_time(backup_path, filesystem_error);
    if (filesystem_error || backup_time <= main_time) {
        return offer;
    }

    offer.should_offer = true;
    offer.autosave_path = backup_path;
    return offer;
}

std::string autosave_recovery_prompt_message(const std::string& path,
                                             const std::string& autosave_path) {
    return autosave_path + " is newer than " + path +
           "; load auto-save backup instead of the saved file?";
}

bool confirm_autosave_recovery_before_terminal(const std::string& path,
                                               const std::string& autosave_path) {
    std::cerr << autosave_recovery_prompt_message(path, autosave_path) << " [y/N] ";
    std::cerr.flush();
    std::string response;
    if (!std::getline(std::cin, response)) {
        return false;
    }
    const std::string value = trim_ascii_copy(response);
    return value == "y" || value == "Y" || value == "yes" || value == "YES";
}

}  // namespace pkchat::editor