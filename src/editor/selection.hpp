#pragma once

#include <cstddef>
#include <string>

namespace pkchat::editor {

struct Selection {
    size_t anchor = 0;
    size_t active = 0;

    bool has_range() const { return anchor != active; }
    size_t start() const { return anchor < active ? anchor : active; }
    size_t end() const { return anchor < active ? active : anchor; }

    void clear(size_t cursor) {
        anchor = cursor;
        active = cursor;
    }

    bool contains(size_t offset) const {
        return has_range() && offset >= start() && offset < end();
    }
};

enum class MovementKey {
    Left,
    Right,
    Up,
    Down,
    PageUp,
    PageDown,
    Home,
    End,
};

struct MovementKeyEvent {
    MovementKey key = MovementKey::Left;
    bool shift = false;
    bool alt = false;
    bool recognized = false;
};

bool parse_movement_sequence(const std::string& sequence, MovementKeyEvent& out);

}  // namespace pkchat::editor