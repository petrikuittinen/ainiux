#include "tui/tui.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <string>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>
#include <vector>

#include "runtime/runtime.hpp"

namespace pkchat::tui {

Layout layout_for_terminal(int terminal_rows, int terminal_cols) {
    Layout layout;
    layout.rows = std::max(8, terminal_rows);
    layout.cols = std::max(20, terminal_cols);
    layout.header_rows = 2;

    const int fixed_without_input = layout.header_rows + 1 + 1 + 1;
    const int max_input_height = std::max(1, layout.rows - fixed_without_input);
    int input_height = std::max(3, layout.rows / 5);
    input_height = std::min(input_height, max_input_height);

    layout.history_row = layout.header_rows + 1;
    layout.history_rows = std::max(1, layout.rows - layout.header_rows - 1 - 1 - input_height);
    layout.status_row = layout.history_row + layout.history_rows;
    layout.input_label_row = layout.status_row + 1;
    layout.input_rect = editor::Rect{layout.input_label_row + 1, 1, input_height, layout.cols};
    return layout;
}

namespace {

int exit_code_for(ErrorCode code) {
    switch (code) {
        case ErrorCode::Ok:
            return 0;
        case ErrorCode::BadArgs:
        case ErrorCode::BadUrl:
            return 2;
        case ErrorCode::Dns:
        case ErrorCode::Connect:
        case ErrorCode::Tls:
        case ErrorCode::Timeout:
            return 3;
        case ErrorCode::HttpStatus:
        case ErrorCode::Auth:
        case ErrorCode::RateLimit:
        case ErrorCode::JsonParse:
        case ErrorCode::SseParse:
        case ErrorCode::ProviderSchema:
            return 4;
        case ErrorCode::FileRead:
        case ErrorCode::FileWrite:
        case ErrorCode::Config:
            return 5;
        case ErrorCode::Cancelled:
            return 130;
        case ErrorCode::UnsupportedFeature:
        case ErrorCode::Internal:
            return 6;
    }
    return 6;
}

std::string trim_ascii(std::string text) {
    auto is_ws = [](unsigned char ch) { return ch == ' ' || ch == '\n' || ch == '\r' || ch == '\t'; };
    while (!text.empty() && is_ws(static_cast<unsigned char>(text.front()))) {
        text.erase(text.begin());
    }
    while (!text.empty() && is_ws(static_cast<unsigned char>(text.back()))) {
        text.pop_back();
    }
    return text;
}

void refresh_session_metadata(chat::Session& session, const provider::RequestContext& context) {
    session.provider = context.profile.name;
    session.base_url = context.base_url;
    session.model = context.options.model;
}

bool has_system_message(const chat::Session& session) {
    for (const provider::Message& message : session.messages) {
        if (message.role == "system") {
            return true;
        }
    }
    return false;
}

void apply_system_prompt(chat::Session& session, const std::string& system) {
    if (trim_ascii(system).empty() || has_system_message(session)) {
        return;
    }
    session.messages.insert(session.messages.begin(), {"system", system});
}

void replace_system_prompt(chat::Session& session, const std::string& system) {
    for (auto it = session.messages.begin(); it != session.messages.end();) {
        if (it->role == "system") {
            it = session.messages.erase(it);
        } else {
            ++it;
        }
    }
    if (!trim_ascii(system).empty()) {
        session.messages.insert(session.messages.begin(), {"system", system});
    }
}

struct TuiSize {
    int rows = 24;
    int cols = 80;
};

TuiSize terminal_size() {
    TuiSize size;
    winsize ws{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0 && ws.ws_col > 0) {
        size.rows = ws.ws_row;
        size.cols = ws.ws_col;
    }
    return size;
}

class TerminalSession {
   public:
    TerminalSession() = default;
    ~TerminalSession() { restore(); }
    TerminalSession(const TerminalSession&) = delete;
    TerminalSession& operator=(const TerminalSession&) = delete;

    Error enter() {
        if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
            return {ErrorCode::BadArgs, "--tui requires an interactive terminal"};
        }
        if (tcgetattr(STDIN_FILENO, &original_) != 0) {
            return {ErrorCode::Internal, std::string("could not read terminal mode: ") + std::strerror(errno)};
        }
        termios raw = original_;
        raw.c_lflag &= static_cast<tcflag_t>(~(ECHO | ICANON | IEXTEN | ISIG));
        raw.c_iflag &= static_cast<tcflag_t>(~(IXON | ICRNL | BRKINT));
        raw.c_oflag &= static_cast<tcflag_t>(~OPOST);
        raw.c_cc[VMIN] = 0;
        raw.c_cc[VTIME] = 0;
        if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0) {
            return {ErrorCode::Internal, std::string("could not set terminal mode: ") + std::strerror(errno)};
        }
        active_ = true;
        std::cout << "\x1b[?1049h\x1b[?25h\x1b[2J\x1b[H";
        std::cout.flush();
        return ok_error();
    }

    void restore() {
        if (!active_) {
            return;
        }
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &original_);
        std::cout << "\x1b[0m\x1b[?25h\x1b[2J\x1b[H\x1b[?1049l";
        std::cout.flush();
        active_ = false;
    }

   private:
    termios original_{};
    bool active_ = false;
};

bool read_byte(unsigned char& out, int timeout_ms) {
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

size_t utf8_len_at(const std::string& text, size_t pos) {
    const unsigned char ch = static_cast<unsigned char>(text[pos]);
    if (ch < 0x80U) return 1;
    if ((ch & 0xE0U) == 0xC0U && pos + 1 < text.size()) return 2;
    if ((ch & 0xF0U) == 0xE0U && pos + 2 < text.size()) return 3;
    if ((ch & 0xF8U) == 0xF0U && pos + 3 < text.size()) return 4;
    return 1;
}

std::string take_cells(const std::string& text, size_t& pos, int width) {
    std::string out;
    int cells = 0;
    while (pos < text.size() && cells < width) {
        if (text[pos] == '\n') {
            ++pos;
            break;
        }
        const size_t len = utf8_len_at(text, pos);
        out.append(text, pos, len);
        pos += len;
        ++cells;
    }
    return out;
}

std::string clip_cells(const std::string& text, int width) {
    if (width <= 0) {
        return "";
    }
    size_t pos = 0;
    return take_cells(text, pos, width);
}

int displayed_cells(const std::string& text) {
    int cells = 0;
    size_t pos = 0;
    while (pos < text.size()) {
        if (text[pos] == '\n') {
            break;
        }
        pos += utf8_len_at(text, pos);
        ++cells;
    }
    return cells;
}

std::string pad_or_clip(const std::string& text, int width) {
    std::string out = clip_cells(text, width);
    while (displayed_cells(out) < width) {
        out.push_back(' ');
    }
    return out;
}

void draw_line(int row, int cols, const std::string& text, bool inverse = false) {
    std::cout << "\x1b[" << row << ";1H";
    if (inverse) {
        std::cout << "\x1b[7m";
    }
    std::cout << pad_or_clip(text, cols);
    if (inverse) {
        std::cout << "\x1b[0m";
    }
    std::cout << "\x1b[K";
}

void append_wrapped_line(std::vector<std::string>& lines, const std::string& logical, int width) {
    if (width <= 0) {
        lines.push_back("");
        return;
    }
    if (logical.empty()) {
        lines.push_back("");
        return;
    }
    size_t pos = 0;
    while (pos < logical.size()) {
        lines.push_back(take_cells(logical, pos, width));
    }
}

void append_wrapped(std::vector<std::string>& lines, const std::string& text, int width) {
    size_t start = 0;
    while (start <= text.size()) {
        const size_t newline = text.find('\n', start);
        if (newline == std::string::npos) {
            append_wrapped_line(lines, text.substr(start), width);
            break;
        }
        append_wrapped_line(lines, text.substr(start, newline - start), width);
        start = newline + 1;
    }
}

std::string message_label(const std::string& role) {
    if (role == "user") return "you";
    if (role == "assistant") return "assistant";
    return role;
}

std::string error_line(const Error& error) {
    return std::string(error_code_name(error.code)) + ": " + error.message;
}

std::string model_line(const provider::RequestContext& context) {
    return context.options.model.empty() ? "unknown" : context.options.model;
}

std::vector<std::string> history_lines_for_session(const chat::Session& session, int cols) {
    std::vector<std::string> history;
    const int min_content_width = 8;
    for (const provider::Message& message : session.messages) {
        const std::string prefix = message_label(message.role) + ": ";
        const std::string content = message.role == "assistant" && message.content.empty() ? "(waiting...)" : message.content;
        std::vector<std::string> wrapped;
        append_wrapped(wrapped, content, std::max(min_content_width, cols - static_cast<int>(prefix.size())));
        for (size_t i = 0; i < wrapped.size(); ++i) {
            history.push_back((i == 0 ? prefix : std::string(prefix.size(), ' ')) + wrapped[i]);
        }
    }
    return history;
}

editor::EditorState empty_input_editor() {
    editor::EditorState input = editor::EditorState::from_text("");
    input.vertical_movement = editor::VerticalMovementMode::VisualRow;
    return input;
}

void set_status_from_error(const Error& err, std::string& status) {
    if (!err.ok()) {
        status = error_line(err);
    }
}

void insert_input(editor::EditorState& input, const std::string& text, std::string& status) {
    set_status_from_error(input.insert(text), status);
}

void render(const provider::RequestContext& context,
            const chat::Session& session,
            editor::EditorState& input,
            std::string& status,
            int& history_scroll) {
    const TuiSize terminal = terminal_size();
    const Layout layout = layout_for_terminal(terminal.rows, terminal.cols);
    const int cols = layout.cols;

    input.ensure_cursor_visible(layout.input_rect);
    const editor::RenderedPanel input_panel = input.render(layout.input_rect);
    std::vector<std::string> history = history_lines_for_session(session, cols);
    const int max_history_scroll = std::max(0, static_cast<int>(history.size()) - layout.history_rows);
    history_scroll = std::min(std::max(0, history_scroll), max_history_scroll);

    std::cout << "\x1b[?25l";
    draw_line(1, cols, "pkchat TUI  Endpoint: " + context.chat_url);
    draw_line(2, cols, "Model: " + model_line(context));

    const int history_start = std::max(0, static_cast<int>(history.size()) - layout.history_rows - history_scroll);
    int printed = 0;
    for (int i = history_start; i < static_cast<int>(history.size()) && printed < layout.history_rows; ++i, ++printed) {
        draw_line(layout.history_row + printed, cols, history[static_cast<size_t>(i)]);
    }
    while (printed < layout.history_rows) {
        draw_line(layout.history_row + printed, cols, "");
        ++printed;
    }

    draw_line(layout.status_row, cols, status, true);
    draw_line(layout.input_label_row,
              cols,
              "Input  Enter send | Alt+Enter newline | arrows edit | PageUp/PageDown scroll | Ctrl+C cancel/quit",
              true);

    for (int row = 0; row < layout.input_rect.height; ++row) {
        const std::string line = row < static_cast<int>(input_panel.lines.size())
                                     ? input_panel.lines[static_cast<size_t>(row)]
                                     : std::string();
        draw_line(layout.input_rect.row + row, cols, line);
    }

    const int cursor_row = input_panel.cursor.visible ? layout.input_rect.row + input_panel.cursor.row : layout.input_rect.row;
    const int cursor_col = input_panel.cursor.visible ? layout.input_rect.col + input_panel.cursor.col : layout.input_rect.col;
    std::cout << "\x1b[" << std::min(layout.rows, std::max(1, cursor_row)) << ";"
              << std::min(cols, std::max(1, cursor_col)) << "H\x1b[?25h";
    std::cout.flush();
}

enum class TuiEventType { Delta, Done, Error, SaveDone, LoadDone, ModelsDone };

enum class ActiveJob { None, Chat, Models };

struct TuiEvent {
    TuiEventType type = TuiEventType::Delta;
    std::string text;
    Error error;
    provider::ChatResult chat;
    chat::Session session;
    std::vector<std::string> models;
};

std::string join_models_preview(const std::vector<std::string>& models) {
    if (models.empty()) {
        return "No models returned";
    }
    std::string out = "Models:";
    const size_t limit = std::min<size_t>(models.size(), 5);
    for (size_t i = 0; i < limit; ++i) {
        out += (i == 0 ? " " : ", ");
        out += models[i];
    }
    if (models.size() > limit) {
        out += ", ...";
    }
    return out;
}

bool is_escape_final(unsigned char ch) {
    return (ch >= 'A' && ch <= 'Z') || ch == '~';
}

void handle_escape(editor::EditorState& input, const Layout& layout, int& history_scroll, std::string& status) {
    unsigned char ch = 0;
    if (!read_byte(ch, 25)) {
        return;
    }
    if (ch == '\r' || ch == '\n') {
        insert_input(input, "\n", status);
        status = "Inserted newline. Enter sends; Ctrl+S also sends.";
        return;
    }

    std::string sequence;
    if (ch == '[' || ch == 'O') {
        sequence.push_back(static_cast<char>(ch));
        while (sequence.size() < 16 && read_byte(ch, 25)) {
            sequence.push_back(static_cast<char>(ch));
            if (is_escape_final(ch)) {
                break;
            }
        }
    } else {
        if (ch >= 32 || ch == '\t') {
            insert_input(input, std::string(1, static_cast<char>(ch)), status);
        }
        return;
    }

    if (sequence == "[A" || sequence == "OA") {
        input.move_up(layout.input_rect);
    } else if (sequence == "[B" || sequence == "OB") {
        input.move_down(layout.input_rect);
    } else if (sequence == "[C" || sequence == "OC") {
        input.move_right();
    } else if (sequence == "[D" || sequence == "OD") {
        input.move_left();
    } else if (sequence == "[H" || sequence == "[1~" || sequence == "OH") {
        input.move_home();
    } else if (sequence == "[F" || sequence == "[4~" || sequence == "OF") {
        input.move_end();
    } else if (sequence == "[3~") {
        set_status_from_error(input.erase_at_cursor(), status);
    } else if (sequence == "[5~") {
        history_scroll += std::max(1, layout.history_rows / 2);
    } else if (sequence == "[6~") {
        history_scroll -= std::max(1, layout.history_rows / 2);
    }
}

}  // namespace

int run(provider::RequestContext context, chat::Session session) {
    TerminalSession terminal;
    Error err = terminal.enter();
    if (!err.ok()) {
        std::cerr << error_code_name(err.code) << ": " << err.message << "\n";
        return exit_code_for(err.code);
    }

    runtime::EventQueue<TuiEvent> events;
    runtime::JobHandle model_job;
    runtime::JobHandle file_job;
    ActiveJob active_job = ActiveJob::None;
    editor::EditorState input = empty_input_editor();
    std::string status = "Ready";
    bool quit = false;
    size_t pending_user = static_cast<size_t>(-1);
    size_t pending_assistant = static_cast<size_t>(-1);
    int history_scroll = 0;

    auto rollback_pending_turn = [&]() {
        if (pending_assistant != static_cast<size_t>(-1) && pending_assistant < session.messages.size()) {
            session.messages.erase(session.messages.begin() + static_cast<long>(pending_assistant));
        }
        if (pending_user != static_cast<size_t>(-1) && pending_user < session.messages.size()) {
            session.messages.erase(session.messages.begin() + static_cast<long>(pending_user));
        }
        pending_user = static_cast<size_t>(-1);
        pending_assistant = static_cast<size_t>(-1);
    };

    auto start_save = [&](const std::string& path, chat::Session snapshot) {
        if (path.empty()) {
            return;
        }
        if (file_job.running()) {
            status = "A file job is already running";
            return;
        }
        file_job.start([path, snapshot = std::move(snapshot), &events](runtime::CancellationToken token) mutable {
            TuiEvent event;
            event.type = TuiEventType::SaveDone;
            event.text = path;
            if (token.cancelled()) {
                event.error = {ErrorCode::Cancelled, "save cancelled: " + path};
            } else {
                event.error = chat::save_session_atomic(path, std::move(snapshot));
            }
            events.push(std::move(event));
        });
    };

    auto start_load = [&](const std::string& path) {
        if (file_job.running()) {
            status = "A file job is already running";
            return;
        }
        file_job.start([path, &events](runtime::CancellationToken token) mutable {
            TuiEvent event;
            event.type = TuiEventType::LoadDone;
            event.text = path;
            if (token.cancelled()) {
                event.error = {ErrorCode::Cancelled, "load cancelled: " + path};
            } else {
                event.error = chat::load_session(path, event.session);
            }
            events.push(std::move(event));
        });
        status = "Loading " + path;
    };

    auto start_models = [&]() {
        if (active_job != ActiveJob::None) {
            status = "A model job is already running";
            return;
        }
        active_job = ActiveJob::Models;
        provider::RequestContext job_context = context;
        model_job.start([job_context, &events](runtime::CancellationToken token) mutable {
            TuiEvent event;
            event.type = TuiEventType::ModelsDone;
            provider::ModelsResult models;
            event.error = provider::list_models(job_context, models, token);
            event.models = std::move(models.model_ids);
            events.push(std::move(event));
        });
        status = "Listing models...";
    };

    auto start_turn = [&](const std::string& prompt) {
        if (active_job != ActiveJob::None) {
            status = "A model job is already running";
            return;
        }
        active_job = ActiveJob::Chat;
        history_scroll = 0;
        pending_user = session.messages.size();
        session.messages.push_back({"user", prompt});
        pending_assistant = session.messages.size();
        session.messages.push_back({"assistant", ""});

        std::vector<provider::Message> request_messages = session.messages;
        request_messages.pop_back();
        provider::RequestContext job_context = context;
        model_job.start([job_context, request_messages = std::move(request_messages), &events](runtime::CancellationToken token) mutable {
            provider::ChatResult chat;
            Error send_error = provider::send_chat_messages(
                job_context,
                request_messages,
                [&](const std::string& delta) -> Error {
                    TuiEvent event;
                    event.type = TuiEventType::Delta;
                    event.text = delta;
                    events.push(std::move(event));
                    if (token.cancelled()) {
                        return {ErrorCode::Cancelled, "chat request cancelled while streaming"};
                    }
                    return ok_error();
                },
                chat,
                token);
            TuiEvent event;
            if (send_error.ok()) {
                event.type = TuiEventType::Done;
                event.chat = std::move(chat);
            } else {
                event.type = TuiEventType::Error;
                event.error = send_error;
            }
            events.push(std::move(event));
        });
        status = "Waiting for response...";
    };

    auto handle_command = [&](const std::string& text) {
        if (text == "/quit" || text == "/exit") {
            quit = true;
            return;
        }
        if (text == "/help") {
            status = "Enter sends | Alt+Enter newline | /quit /models /save /load /model /system /clear";
            return;
        }
        if (text == "/clear") {
            session.messages.clear();
            apply_system_prompt(session, context.options.system);
            history_scroll = 0;
            status = "Chat history cleared";
            return;
        }
        if (text.rfind("/model", 0) == 0) {
            const std::string model = trim_ascii(text.substr(6));
            if (model.empty()) {
                status = "Usage: /model MODEL";
                return;
            }
            context.options.model = model;
            session.model = model;
            status = "Model set to " + model;
            return;
        }
        if (text.rfind("/system", 0) == 0) {
            replace_system_prompt(session, trim_ascii(text.substr(7)));
            status = "System prompt updated";
            return;
        }
        if (text == "/models") {
            start_models();
            return;
        }
        if (text.rfind("/save", 0) == 0) {
            std::string path = trim_ascii(text.substr(5));
            if (path.empty()) {
                path = context.options.save_chat_path;
            }
            if (path.empty()) {
                status = "Usage: /save PATH";
                return;
            }
            start_save(path, session);
            status = "Saving " + path;
            return;
        }
        if (text.rfind("/load", 0) == 0) {
            const std::string path = trim_ascii(text.substr(5));
            if (path.empty()) {
                status = "Usage: /load PATH";
                return;
            }
            start_load(path);
            return;
        }
        status = "Unknown command: " + text;
    };

    auto submit_input = [&]() {
        const std::string raw = input.text.str();
        const std::string text = trim_ascii(raw);
        if (text.empty()) {
            input = empty_input_editor();
            return;
        }
        if (raw.find('\n') == std::string::npos && text[0] == '/') {
            input = empty_input_editor();
            handle_command(text);
            return;
        }
        if (active_job != ActiveJob::None) {
            status = "A model job is already running";
            return;
        }
        input = empty_input_editor();
        start_turn(raw);
    };

    if (!trim_ascii(context.options.prompt).empty()) {
        start_turn(context.options.prompt);
    }

    render(context, session, input, status, history_scroll);
    while (!quit) {
        TuiEvent event;
        while (events.try_pop(event)) {
            switch (event.type) {
                case TuiEventType::Delta:
                    if (pending_assistant != static_cast<size_t>(-1) && pending_assistant < session.messages.size()) {
                        session.messages[pending_assistant].content += event.text;
                    }
                    status = "Streaming...";
                    break;
                case TuiEventType::Done:
                    model_job.join();
                    if (pending_assistant != static_cast<size_t>(-1) && pending_assistant < session.messages.size()) {
                        session.messages[pending_assistant].content = event.chat.content;
                    }
                    if (!event.chat.model.empty()) {
                        context.options.model = event.chat.model;
                        session.model = event.chat.model;
                    }
                    if (!event.chat.usage_json.empty() && event.chat.usage_json != "null") {
                        session.usage_json = event.chat.usage_json;
                    }
                    pending_user = static_cast<size_t>(-1);
                    pending_assistant = static_cast<size_t>(-1);
                    active_job = ActiveJob::None;
                    status = "Ready";
                    start_save(context.options.save_chat_path, session);
                    break;
                case TuiEventType::Error:
                    model_job.join();
                    rollback_pending_turn();
                    active_job = ActiveJob::None;
                    status = event.error.code == ErrorCode::Cancelled ? "Cancelled" : error_line(event.error);
                    break;
                case TuiEventType::SaveDone:
                    file_job.join();
                    status = event.error.ok() ? "Saved " + event.text : error_line(event.error);
                    break;
                case TuiEventType::LoadDone:
                    file_job.join();
                    if (event.error.ok()) {
                        session = std::move(event.session);
                        refresh_session_metadata(session, context);
                        history_scroll = 0;
                        status = "Loaded " + event.text;
                    } else {
                        status = error_line(event.error);
                    }
                    break;
                case TuiEventType::ModelsDone:
                    model_job.join();
                    active_job = ActiveJob::None;
                    status = event.error.ok() ? join_models_preview(event.models) : error_line(event.error);
                    break;
            }
        }

        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(STDIN_FILENO, &readfds);
        timeval timeout{};
        timeout.tv_usec = 50000;
        const int ready = select(STDIN_FILENO + 1, &readfds, nullptr, nullptr, &timeout);
        if (ready < 0 && errno != EINTR) {
            status = std::string("terminal input error: ") + std::strerror(errno);
        }
        if (ready > 0 && FD_ISSET(STDIN_FILENO, &readfds)) {
            unsigned char ch = 0;
            while (read(STDIN_FILENO, &ch, 1) == 1) {
                if (ch == 27) {
                    const TuiSize terminal = terminal_size();
                    handle_escape(input, layout_for_terminal(terminal.rows, terminal.cols), history_scroll, status);
                    continue;
                }
                if (ch == 3) {
                    if (active_job != ActiveJob::None) {
                        model_job.cancel();
                        status = "Cancelling...";
                    } else if (file_job.running()) {
                        file_job.cancel();
                        status = "Cancelling file job...";
                    } else {
                        quit = true;
                    }
                    continue;
                }
                if (ch == 19) {
                    submit_input();
                    continue;
                }
                if (ch == 4 && input.text.empty()) {
                    quit = true;
                    continue;
                }
                if (ch == 127 || ch == 8) {
                    set_status_from_error(input.erase_before_cursor(), status);
                    continue;
                }
                if (ch == '\r' || ch == '\n') {
                    submit_input();
                    continue;
                }
                if (ch >= 32 || ch == '\t') {
                    insert_input(input, std::string(1, static_cast<char>(ch)), status);
                }
            }
        }
        render(context, session, input, status, history_scroll);
    }

    model_job.cancel();
    file_job.cancel();
    model_job.join();
    file_job.join();
    return 0;
}

}  // namespace pkchat::tui
