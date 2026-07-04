#include "editor/selection.hpp"

namespace pkchat::editor {

namespace {

bool parse_arrow_body(const std::string& body, MovementKeyEvent& out) {
    if (body.empty()) {
        return false;
    }
    const char letter = body.back();
    std::string prefix = body.size() > 1 ? body.substr(0, body.size() - 1) : std::string();
    bool shift = false;
    bool alt = false;
    if (prefix == "1;2") {
        shift = true;
        prefix.clear();
    } else if (prefix == "1;3") {
        alt = true;
        prefix.clear();
    } else if (prefix == "1;4") {
        shift = true;
        alt = true;
        prefix.clear();
    } else if (!prefix.empty()) {
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
    if (inner.size() >= 2 && inner.compare(inner.size() - 2, 2, ";4") == 0) {
        shift = true;
        alt = true;
        inner.erase(inner.size() - 2);
    } else if (inner.size() >= 2 && inner.compare(inner.size() - 2, 2, ";3") == 0) {
        alt = true;
        inner.erase(inner.size() - 2);
    } else if (inner.size() >= 2 && inner.compare(inner.size() - 2, 2, ";2") == 0) {
        shift = true;
        inner.erase(inner.size() - 2);
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

}  // namespace

bool parse_movement_sequence(const std::string& sequence, MovementKeyEvent& out) {
    out = MovementKeyEvent{};
    if (sequence.empty()) {
        return false;
    }
    if (sequence[0] == '[') {
        return parse_csi_movement(sequence.substr(1), out);
    }
    if (sequence.size() == 2 && sequence[0] == 'O') {
        return parse_arrow_body(sequence.substr(1), out);
    }
    return false;
}

}  // namespace pkchat::editor