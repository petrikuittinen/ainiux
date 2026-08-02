#include "editor/terminal_input.hpp"

#include "editor/editor.hpp"
#include "html/html.hpp"

#include <cctype>
#include <deque>
#include <iostream>
#include <limits>
#include <sys/select.h>
#include <unistd.h>

namespace ainiux::editor {
namespace {

std::deque<unsigned char> g_input_queue;
bool g_pending_escape_alt_meta = false;
bool g_terminal_clipboard_query_pending = false;
constexpr size_t kMaxOsc52EncodedSize = ((kExternalClipboardReadLimit + 2U) / 3U) * 4U;

bool is_bracketed_paste_prefix(const std::string& text) {
    static constexpr char kStart[] = "[200~";
    if (text.size() > sizeof(kStart) - 1) {
        return false;
    }
    return std::string(kStart, sizeof(kStart) - 1).compare(0, text.size(), text) == 0;
}

std::string base64_encode(const std::string& data) {
    static const char* alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((data.size() + 2) / 3) * 4);
    size_t i = 0;
    while (i < data.size()) {
        const size_t remaining = data.size() - i;
        const unsigned int b0 = static_cast<unsigned char>(data[i]);
        const unsigned int b1 = remaining > 1 ? static_cast<unsigned char>(data[i + 1]) : 0U;
        const unsigned int b2 = remaining > 2 ? static_cast<unsigned char>(data[i + 2]) : 0U;
        const unsigned int triple = (b0 << 16) | (b1 << 8) | b2;
        out.push_back(alphabet[(triple >> 18) & 0x3F]);
        out.push_back(alphabet[(triple >> 12) & 0x3F]);
        out.push_back(remaining > 1 ? alphabet[(triple >> 6) & 0x3F] : '=');
        out.push_back(remaining > 2 ? alphabet[triple & 0x3F] : '=');
        i += 3;
    }
    return out;
}

int base64_value(unsigned char ch) {
    if (ch >= 'A' && ch <= 'Z') return ch - 'A';
    if (ch >= 'a' && ch <= 'z') return ch - 'a' + 26;
    if (ch >= '0' && ch <= '9') return ch - '0' + 52;
    if (ch == '+') return 62;
    if (ch == '/') return 63;
    return -1;
}

void push_bytes_front(const std::string& bytes) {
    for (auto it = bytes.rbegin(); it != bytes.rend(); ++it) {
        g_input_queue.push_front(static_cast<unsigned char>(*it));
    }
}

bool parse_positive_int(const std::string& text, int& out) {
    if (text.empty()) {
        return false;
    }
    int value = 0;
    for (char ch : text) {
        if (!std::isdigit(static_cast<unsigned char>(ch))) {
            return false;
        }
        const int digit = ch - '0';
        if (value > (std::numeric_limits<int>::max() - digit) / 10) {
            return false;
        }
        value = value * 10 + digit;
    }
    out = value;
    return true;
}

bool is_ctrl_modifier(int modifier) {
    return modifier == 5 || modifier == 6 || modifier == 7 || modifier == 8;
}

// Enhanced keyboard modes used by supported terminals may encode the shifted
// codepoint while retaining modifier 5. Keep the established save-as decoding.
bool has_shift_modifier(int modifier) { return (modifier & 1) != 0; }
bool has_alt_modifier(int modifier) { return ((modifier - 1) & 2) != 0; }

bool ctrl_byte_from_codepoint(int codepoint, unsigned char& out) {
    if (codepoint >= 1 && codepoint <= 26) {
        out = static_cast<unsigned char>(codepoint);
        return true;
    }
    // Kitty/xterm may report Ctrl+letter as lowercase or uppercase codepoints.
    if (codepoint >= 97 && codepoint <= 122) {
        out = static_cast<unsigned char>(codepoint - 96);
        return true;
    }
    if (codepoint >= 65 && codepoint <= 90) {
        out = static_cast<unsigned char>(codepoint - 64);
        return true;
    }
    return false;
}

bool read_bracketed_paste_body(std::string& out) {
    out.clear();
    static constexpr char kEnd[] = "\x1b[201~";
    const size_t end_len = sizeof(kEnd) - 1;
    std::string window;
    unsigned char ch = 0;
    while (read_terminal_byte(ch, 100)) {
        out.push_back(static_cast<char>(ch));
        window.push_back(static_cast<char>(ch));
        if (window.size() > end_len) {
            window.erase(0, window.size() - end_len);
        }
        if (window == kEnd) {
            out.erase(out.size() - end_len);
            return true;
        }
    }
    return false;
}

bool read_osc_body(std::string& out) {
    out.clear();
    unsigned char ch = 0;
    while (out.size() <= kMaxOsc52EncodedSize + 16U && read_terminal_byte(ch, 100)) {
        if (ch == 7) return true;
        if (ch == 27) {
            unsigned char next = 0;
            if (read_terminal_byte(next, 25)) {
                if (next == '\\') return true;
                out.push_back(static_cast<char>(ch));
                out.push_back(static_cast<char>(next));
                continue;
            }
        }
        out.push_back(static_cast<char>(ch));
    }
    return false;
}

bool csi_sequence_complete(const std::string& sequence) {
    if (sequence.empty()) {
        return false;
    }
    const char last = sequence.back();
    return last == 'u' || last == '~' || (last >= 'A' && last <= 'Z') || (last >= 'a' && last <= 'z');
}

}  // namespace

void EscapeRepeatGuard::arm(Clock::time_point now) {
    deadline_ = now + std::chrono::milliseconds(1000);
    armed_ = true;
}

void EscapeRepeatGuard::reset() {
    armed_ = false;
}

bool EscapeRepeatGuard::suppress(Clock::time_point now) {
    if (!armed_ || now >= deadline_) {
        armed_ = false;
        return false;
    }
    // Continuous repeats keep the guard alive; after release it expires soon.
    deadline_ = now + std::chrono::milliseconds(500);
    return true;
}

namespace {

bool decode_mouse_code(int code, bool sgr_release, MouseInputEvent& out) {
    if (code < 0 || code > 255) return false;
    out.shift = (code & 4) != 0;
    out.alt = (code & 8) != 0;
    out.ctrl = (code & 16) != 0;
    out.motion = (code & 32) != 0;
    out.pressed = !sgr_release;
    const int button = code & 3;
    if ((code & 64) != 0) {
        switch (button) {
            case 0: out.button = MouseButton::WheelUp; break;
            case 1: out.button = MouseButton::WheelDown; break;
            case 2: out.button = MouseButton::WheelLeft; break;
            case 3: out.button = MouseButton::WheelRight; break;
        }
        return true;
    }
    if (sgr_release || button == 3) {
        out.button = MouseButton::Release;
        out.pressed = false;
        return true;
    }
    switch (button) {
        case 0: out.button = MouseButton::Left; break;
        case 1: out.button = MouseButton::Middle; break;
        case 2: out.button = MouseButton::Right; break;
        default: return false;
    }
    return true;
}

}  // namespace

bool decode_sgr_mouse_sequence(const std::string& sequence, MouseInputEvent& out) {
    out = MouseInputEvent{};
    if (sequence.size() < 7 || sequence.rfind("[<", 0) != 0 ||
        (sequence.back() != 'M' && sequence.back() != 'm')) {
        return false;
    }
    const size_t first = sequence.find(';', 2);
    const size_t second = first == std::string::npos ? first : sequence.find(';', first + 1);
    if (first == std::string::npos || second == std::string::npos ||
        sequence.find(';', second + 1) != std::string::npos) {
        return false;
    }
    int code = 0;
    if (!parse_positive_int(sequence.substr(2, first - 2), code) ||
        !parse_positive_int(sequence.substr(first + 1, second - first - 1), out.col) ||
        !parse_positive_int(sequence.substr(second + 1, sequence.size() - second - 2), out.row) ||
        out.col < 1 || out.row < 1) {
        out = MouseInputEvent{};
        return false;
    }
    if (!decode_mouse_code(code, sequence.back() == 'm', out)) {
        out = MouseInputEvent{};
        return false;
    }
    return true;
}

bool decode_x10_mouse_bytes(unsigned char code_byte,
                            unsigned char col_byte,
                            unsigned char row_byte,
                            MouseInputEvent& out) {
    out = MouseInputEvent{};
    if (code_byte < 32 || col_byte < 33 || row_byte < 33) return false;
    out.col = static_cast<int>(col_byte) - 32;
    out.row = static_cast<int>(row_byte) - 32;
    if (!decode_mouse_code(static_cast<int>(code_byte) - 32, false, out)) {
        out = MouseInputEvent{};
        return false;
    }
    return true;
}

bool decode_control_key_sequence(const std::string& sequence, unsigned char& out) {
    if (sequence == "[Z" || sequence == "OZ") {
        out = editor_key_backtab();
        return true;
    }
    if (sequence == "[27u") {
        out = 27;
        return true;
    }
    if (sequence.size() >= 4 && sequence[0] == '[' && sequence.back() == 'u') {
        const size_t semi = sequence.find(';');
        if (semi != std::string::npos && semi > 1) {
            int codepoint = 0;
            int modifier = 0;
            if (!parse_positive_int(sequence.substr(1, semi - 1), codepoint) ||
                !parse_positive_int(sequence.substr(semi + 1, sequence.size() - semi - 2), modifier)) {
                return false;
            }
            if (codepoint == 27 && modifier == 1) {
                out = 27;
                return true;
            }
            if (codepoint == 'x' && modifier == 3) {
                out = editor_key_command_minibuffer();
                return true;
            }
            if (codepoint == 9 && modifier == 2) {
                out = editor_key_backtab();
                return true;
            }
            if (!is_ctrl_modifier(modifier)) {
                return false;
            }
            if (has_alt_modifier(modifier) && (codepoint == 'T' || codepoint == 't' ||
                                               codepoint == 20)) {
                out = editor_key_toggle_thinking_traces();
                return true;
            }
            if (has_shift_modifier(modifier) && (codepoint == 'S' || codepoint == 's')) {
                out = editor_key_save_as();
                return true;
            }
            return ctrl_byte_from_codepoint(codepoint, out);
        }
    }
    if (sequence.size() >= 6 && sequence[0] == '[' && sequence.back() == '~' &&
        sequence.rfind(';') != std::string::npos) {
        const size_t semi = sequence.rfind(';');
        const size_t prev_semi = sequence.rfind(';', semi == 0 ? 0 : semi - 1);
        if (prev_semi != std::string::npos && prev_semi + 1 < semi && semi + 1 < sequence.size() - 1) {
            int modifier = 0;
            int key = 0;
            if (!parse_positive_int(sequence.substr(prev_semi + 1, semi - prev_semi - 1), modifier) ||
                !parse_positive_int(sequence.substr(semi + 1, sequence.size() - semi - 2), key)) {
                return false;
            }
            if (modifier == 1 && key == 27) {
                out = 27;
                return true;
            }
            if (key == 'x' && modifier == 3) {
                out = editor_key_command_minibuffer();
                return true;
            }
            if (modifier == 2 && key == 9) {
                out = editor_key_backtab();
                return true;
            }
            if (!is_ctrl_modifier(modifier)) {
                return false;
            }
            if (has_alt_modifier(modifier) && (key == 'T' || key == 't' || key == 20)) {
                out = editor_key_toggle_thinking_traces();
                return true;
            }
            return ctrl_byte_from_codepoint(key, out);
        }
    }
    return false;
}

bool decode_osc52_clipboard_payload(const std::string& encoded,
                                    std::string& text,
                                    std::string& error) {
    text.clear();
    error.clear();
    if (encoded.empty()) {
        error = "terminal clipboard contains no text";
        return false;
    }
    if (encoded.size() > kMaxOsc52EncodedSize) {
        error = "terminal clipboard exceeds the 16 MiB text limit";
        return false;
    }
    if (encoded.size() % 4 != 0) {
        error = "terminal returned malformed OSC 52 base64";
        return false;
    }
    text.reserve((encoded.size() / 4) * 3);
    for (size_t i = 0; i < encoded.size(); i += 4) {
        const bool last = i + 4 == encoded.size();
        const int a = base64_value(static_cast<unsigned char>(encoded[i]));
        const int b = base64_value(static_cast<unsigned char>(encoded[i + 1]));
        const bool pad2 = encoded[i + 2] == '=';
        const bool pad3 = encoded[i + 3] == '=';
        const int c = pad2 ? 0 : base64_value(static_cast<unsigned char>(encoded[i + 2]));
        const int d = pad3 ? 0 : base64_value(static_cast<unsigned char>(encoded[i + 3]));
        if (a < 0 || b < 0 || c < 0 || d < 0 || (pad2 && !pad3) ||
            (!last && (pad2 || pad3)) || (pad2 && (b & 0x0F) != 0) ||
            (pad3 && !pad2 && (c & 0x03) != 0)) {
            error = "terminal returned malformed OSC 52 base64";
            text.clear();
            return false;
        }
        const unsigned int value = (static_cast<unsigned int>(a) << 18) |
                                   (static_cast<unsigned int>(b) << 12) |
                                   (static_cast<unsigned int>(c) << 6) |
                                   static_cast<unsigned int>(d);
        text.push_back(static_cast<char>((value >> 16) & 0xFF));
        if (!pad2) text.push_back(static_cast<char>((value >> 8) & 0xFF));
        if (!pad3) text.push_back(static_cast<char>(value & 0xFF));
    }
    if (text.empty()) {
        error = "terminal clipboard contains no text";
        return false;
    }
    if (text.find('\0') != std::string::npos || !html::is_valid_utf8(text)) {
        error = "terminal clipboard is not valid UTF-8 text";
        text.clear();
        return false;
    }
    return true;
}

void clear_terminal_input_queue() {
    g_input_queue.clear();
    g_pending_escape_alt_meta = false;
}

void push_terminal_input_bytes(const std::string& bytes) {
    push_bytes_front(bytes);
}

bool consume_pending_escape_alt_meta() {
    const bool pending = g_pending_escape_alt_meta;
    g_pending_escape_alt_meta = false;
    return pending;
}

bool read_terminal_byte(unsigned char& out, int timeout_ms) {
    if (!g_input_queue.empty()) {
        out = g_input_queue.front();
        g_input_queue.pop_front();
        return true;
    }

    fd_set read_fds;
    FD_ZERO(&read_fds);
    FD_SET(STDIN_FILENO, &read_fds);
    timeval tv{};
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    const int ready = select(STDIN_FILENO + 1, &read_fds, nullptr, nullptr, &tv);
    if (ready <= 0 || !FD_ISSET(STDIN_FILENO, &read_fds)) {
        return false;
    }
    const ssize_t n = read(STDIN_FILENO, &out, 1);
    return n == 1;
}

bool read_terminal_input(TerminalInputEvent& out, int timeout_ms) {
    out = TerminalInputEvent{};
    unsigned char ch = 0;
    if (!read_terminal_byte(ch, timeout_ms)) {
        out.type = TerminalInputType::Timeout;
        return false;
    }

    if (ch != 27) {
        out.type = TerminalInputType::Byte;
        out.byte = ch;
        return true;
    }

    std::string after_esc;
    const bool have_after_esc =
        read_terminal_byte(ch, terminal_escape_inter_byte_timeout_ms());
    if (have_after_esc && ch == ']' && g_terminal_clipboard_query_pending) {
        std::string osc;
        const bool terminated = read_osc_body(osc);
        if (terminated) {
            g_terminal_clipboard_query_pending = false;
            static constexpr char kPrefix[] = "52;c;";
            if (osc.rfind(kPrefix, 0) == 0) {
                out.type = TerminalInputType::Osc52ClipboardResponse;
                std::string error;
                if (!decode_osc52_clipboard_payload(osc.substr(sizeof(kPrefix) - 1), out.text, error))
                    out.text.clear(), out.message = std::move(error);
                return true;
            }
        } else if (osc.rfind("52;c;", 0) == 0 &&
                   osc.size() > kMaxOsc52EncodedSize) {
            g_terminal_clipboard_query_pending = false;
            out.type = TerminalInputType::Osc52ClipboardResponse;
            out.message = "terminal clipboard exceeds the 16 MiB text limit";
            return true;
        }
        push_bytes_front(osc);
        push_bytes_front("]");
        out.type = TerminalInputType::Byte;
        out.byte = 27;
        return true;
    } else if (have_after_esc) {
        after_esc.push_back(static_cast<char>(ch));
    }
    if (after_esc == "x") {
        out.type = TerminalInputType::Byte;
        out.byte = editor_key_command_minibuffer();
        return true;
    }
    while (read_terminal_byte(ch, terminal_escape_inter_byte_timeout_ms())) {
        after_esc.push_back(static_cast<char>(ch));
        if (after_esc == "[200~") {
            std::string pasted;
            if (!read_bracketed_paste_body(pasted)) {
                push_bytes_front(after_esc);
                push_bytes_front("\x1b");
                out.type = TerminalInputType::Byte;
                out.byte = 27;
                return true;
            }
            out.type = TerminalInputType::BracketedPaste;
            out.text = std::move(pasted);
            return true;
        }
        if (is_bracketed_paste_prefix(after_esc)) {
            continue;
        }
        if (after_esc.rfind("[<", 0) == 0) {
            if (ch == 'M' || ch == 'm') break;
            continue;
        }
        if (csi_sequence_complete(after_esc)) {
            break;
        }
        if (after_esc.size() > 32) {
            break;
        }
    }

    if (after_esc == "[M") {
        unsigned char code = 0;
        unsigned char col = 0;
        unsigned char row = 0;
        out.type = TerminalInputType::Mouse;
        if (read_terminal_byte(code, terminal_escape_inter_byte_timeout_ms()) &&
            read_terminal_byte(col, terminal_escape_inter_byte_timeout_ms()) &&
            read_terminal_byte(row, terminal_escape_inter_byte_timeout_ms())) {
            (void)decode_x10_mouse_bytes(code, col, row, out.mouse);
        }
        return true;
    }
    if (after_esc.rfind("[<", 0) == 0) {
        out.type = TerminalInputType::Mouse;
        (void)decode_sgr_mouse_sequence(after_esc, out.mouse);
        return true;
    }

    unsigned char decoded = 0;
    if (after_esc.size() == 1 &&
        static_cast<unsigned char>(after_esc[0]) == 20) {
        out.type = TerminalInputType::Byte;
        out.byte = editor_key_toggle_thinking_traces();
        return true;
    }
    if (decode_control_key_sequence(after_esc, decoded)) {
        out.type = TerminalInputType::Byte;
        out.byte = decoded;
        out.decoded_escape = decoded == 27;
        return true;
    }

    if (!after_esc.empty()) {
        if (after_esc.size() >= 2 && static_cast<unsigned char>(after_esc[0]) == 27 &&
            (after_esc[1] == '[' || after_esc[1] == 'O')) {
            g_pending_escape_alt_meta = true;
            push_bytes_front(after_esc.substr(1));
        } else {
            push_bytes_front(after_esc);
        }
    }
    out.type = TerminalInputType::Byte;
    out.byte = 27;
    return true;
}

bool is_editor_undo_key(unsigned char ch) {
    return ch == 21 || ch == 26;
}

bool is_editor_redo_key(unsigned char ch) {
    return ch == 25;
}

Error paste_with_clipboard_preference(EditorState& state,
                                      Clipboard& clipboard,
                                      const std::string& terminal_paste_text) {
    if (!clipboard.empty()) {
        return state.paste(clipboard);
    }
    if (!terminal_paste_text.empty()) {
        Clipboard external;
        external.set(terminal_paste_text);
        return state.paste(external);
    }
    return {ErrorCode::BadArgs, "clipboard is empty"};
}

void publish_terminal_clipboard(const std::string& text) {
    if (text.empty() || text.size() > kExternalClipboardReadLimit || !isatty(STDOUT_FILENO)) {
        return;
    }
    std::cout << "\x1b]52;c;" << base64_encode(text) << "\x07";
    std::cout.flush();
}

void request_terminal_clipboard() {
    g_terminal_clipboard_query_pending = true;
    if (!isatty(STDOUT_FILENO)) return;
    std::cout << "\x1b]52;c;?\x07";
    std::cout.flush();
}

void cancel_terminal_clipboard_request() {
    g_terminal_clipboard_query_pending = false;
}

}  // namespace ainiux::editor
