#include "editor/selection.hpp"

#include <cctype>

namespace pkchat::editor {

namespace {

bool parse_positive_int(const std::string& text, int& out) {
    if (text.empty()) {
        return false;
    }
    int value = 0;
    for (char ch : text) {
        if (!std::isdigit(static_cast<unsigned char>(ch))) {
            return false;
        }
        value = value * 10 + (ch - '0');
    }
    out = value;
    return true;
}

bool parse_modifier_prefix(const std::string& prefix, bool& shift, bool& alt, bool& ctrl) {
    if (prefix.empty()) {
        return true;
    }
    if (prefix == "1;2") {
        shift = true;
        return true;
    }
    if (prefix == "1;3") {
        alt = true;
        return true;
    }
    if (prefix == "1;4") {
        shift = true;
        alt = true;
        return true;
    }
    if (prefix == "1;5") {
        ctrl = true;
        return true;
    }
    if (prefix == "1;6") {
        shift = true;
        ctrl = true;
        return true;
    }
    if (prefix == "1;7") {
        alt = true;
        ctrl = true;
        return true;
    }
    if (prefix == "1;8") {
        shift = true;
        alt = true;
        ctrl = true;
        return true;
    }
    return false;
}

bool strip_tilde_modifier_suffix(std::string& inner, bool& shift, bool& alt, bool& ctrl) {
    static constexpr const char* kSuffixes[] = {";8", ";7", ";6", ";5", ";4", ";3", ";2"};
    static constexpr bool kShift[] = {true, false, true, false, true, false, true};
    static constexpr bool kAlt[] = {true, true, false, false, true, true, false};
    static constexpr bool kCtrl[] = {true, true, true, true, false, false, false};
    for (size_t i = 0; i < sizeof(kSuffixes) / sizeof(kSuffixes[0]); ++i) {
        const std::string suffix = kSuffixes[i];
        if (inner.size() >= suffix.size() &&
            inner.compare(inner.size() - suffix.size(), suffix.size(), suffix) == 0) {
            shift = kShift[i];
            alt = kAlt[i];
            ctrl = kCtrl[i];
            inner.erase(inner.size() - suffix.size());
            return true;
        }
    }
    return true;
}

bool parse_arrow_body(const std::string& body, MovementKeyEvent& out) {
    if (body.empty()) {
        return false;
    }
    const char letter = body.back();
    std::string prefix = body.size() > 1 ? body.substr(0, body.size() - 1) : std::string();
    bool shift = false;
    bool alt = false;
    bool ctrl = false;
    if (!parse_modifier_prefix(prefix, shift, alt, ctrl)) {
        return false;
    }

    MovementKey movement;
    switch (letter) {
        case 'A':
            movement = MovementKey::Up;
            break;
        case 'B':
            movement = MovementKey::Down;
            break;
        case 'C':
            movement = MovementKey::Right;
            break;
        case 'D':
            movement = MovementKey::Left;
            break;
        case 'H':
            movement = MovementKey::Home;
            break;
        case 'F':
            movement = MovementKey::End;
            break;
        default:
            return false;
    }

    out.key = movement;
    out.shift = shift;
    out.alt = alt;
    out.ctrl = ctrl;
    out.recognized = true;
    return true;
}

bool parse_tilde_body(const std::string& body, MovementKeyEvent& out) {
    if (body.empty() || body.back() != '~') {
        return false;
    }
    std::string inner = body.substr(0, body.size() - 1);
    bool shift = false;
    bool alt = false;
    bool ctrl = false;
    if (!strip_tilde_modifier_suffix(inner, shift, alt, ctrl)) {
        return false;
    }

    MovementKey movement;
    if (inner == "1") {
        movement = MovementKey::Home;
    } else if (inner == "4") {
        movement = MovementKey::End;
    } else if (inner == "5") {
        movement = MovementKey::PageUp;
    } else if (inner == "6") {
        movement = MovementKey::PageDown;
    } else {
        return false;
    }

    out.key = movement;
    out.shift = shift;
    out.alt = alt;
    out.ctrl = ctrl;
    out.recognized = true;
    return true;
}

bool parse_csi_movement(const std::string& body, MovementKeyEvent& out) {
    if (body.empty()) {
        return false;
    }
    if (body.back() == '~') {
        return parse_tilde_body(body, out);
    }
    return parse_arrow_body(body, out);
}

bool movement_key_from_codepoint(int codepoint, MovementKey& out) {
    switch (codepoint) {
        case 1:
        case 57360:
            out = MovementKey::Home;
            return true;
        case 4:
        case 57361:
            out = MovementKey::End;
            return true;
        case 5:
        case 57362:
            out = MovementKey::PageUp;
            return true;
        case 6:
        case 57363:
            out = MovementKey::PageDown;
            return true;
        default:
            return false;
    }
}

bool parse_kitty_modifier_value(int modifier, bool& shift, bool& alt, bool& ctrl) {
    if (modifier < 1) {
        return false;
    }
    const int encoded = modifier - 1;
    shift = (encoded & 1) != 0;
    alt = (encoded & 2) != 0;
    ctrl = (encoded & 4) != 0;
    return true;
}

bool parse_kitty_csi_u_body(const std::string& body, MovementKeyEvent& out) {
    if (body.empty() || body.back() != 'u') {
        return false;
    }
    const std::string inner = body.substr(0, body.size() - 1);
    const size_t semi = inner.find(';');
    int codepoint = 0;
    int modifier = 1;
    if (semi == std::string::npos) {
        if (!parse_positive_int(inner, codepoint)) {
            return false;
        }
    } else {
        if (!parse_positive_int(inner.substr(0, semi), codepoint) ||
            !parse_positive_int(inner.substr(semi + 1), modifier)) {
            return false;
        }
    }

    MovementKey movement;
    if (!movement_key_from_codepoint(codepoint, movement)) {
        return false;
    }
    bool shift = false;
    bool alt = false;
    bool ctrl = false;
    if (!parse_kitty_modifier_value(modifier, shift, alt, ctrl)) {
        return false;
    }

    out.key = movement;
    out.shift = shift;
    out.alt = alt;
    out.ctrl = ctrl;
    out.recognized = true;
    return true;
}

}  // namespace

bool parse_movement_sequence(const std::string& sequence, MovementKeyEvent& out) {
    out = MovementKeyEvent{};
    if (sequence.empty()) {
        return false;
    }
    if (sequence[0] == '[') {
        if (sequence.back() == 'u') {
            return parse_kitty_csi_u_body(sequence.substr(1), out);
        }
        return parse_csi_movement(sequence.substr(1), out);
    }
    if (sequence.size() == 2 && sequence[0] == 'O') {
        return parse_arrow_body(sequence.substr(1), out);
    }
    return false;
}

}  // namespace pkchat::editor