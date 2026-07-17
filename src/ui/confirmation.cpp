#include "ui/confirmation.hpp"

namespace ainiux::ui {

bool yes_answer(const std::string& value) {
    return value == "y" || value == "Y" || value == "yes" || value == "YES";
}

bool no_answer(const std::string& value) {
    return value == "n" || value == "N" || value == "no" || value == "NO";
}

bool yes_key(unsigned char ch) {
    return ch == 'y' || ch == 'Y';
}

bool no_key(unsigned char ch) {
    return ch == 'n' || ch == 'N';
}

bool cancel_key(unsigned char ch) {
    return ch == 27;
}

ConfirmationKeyResult parse_confirmation_key(unsigned char ch, bool treat_esc_as_reject) {
    if (yes_key(ch)) {
        return ConfirmationKeyResult::Accepted;
    }
    if (no_key(ch) || (treat_esc_as_reject && cancel_key(ch))) {
        return ConfirmationKeyResult::Rejected;
    }
    return ConfirmationKeyResult::Pending;
}

}  // namespace ainiux::ui