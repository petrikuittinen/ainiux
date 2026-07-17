#pragma once

#include <cstddef>
#include <string>

namespace ainiux::ui {

constexpr const char kConfirmationRetryPrompt[] = "Type y or n: ";

bool yes_answer(const std::string& value);
bool no_answer(const std::string& value);
bool yes_key(unsigned char ch);
bool no_key(unsigned char ch);
bool cancel_key(unsigned char ch);

enum class ConfirmationKeyResult { Pending, Accepted, Rejected };

ConfirmationKeyResult parse_confirmation_key(unsigned char ch, bool treat_esc_as_reject = true);

}  // namespace ainiux::ui