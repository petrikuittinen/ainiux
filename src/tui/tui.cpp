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
        raw.c_iflag &= static_cast<tcflag_t>(~(IXON | ICRNL));
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

size_t utf8_char_start(const std::string& text, size_t pos) {
    if (pos == 0 || pos > text.size()) {
        return 0;
    }
    size_t out = pos - 1;
    while (out > 0 && (static_cast<unsigned char>(text[out]) & 0xC0U) == 0x80U) {
        --out;
    }
    return out;
}

void pop_utf8_char(std::string& text) {
    if (text.empty()) {
        return;
    }
    text.erase(utf8_char_start(text, text.size()));
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

std::string pad_or_clip(const std::string& text, int width) {
    std::string out = clip_cells(text, width);
    while (static_cast<int>(out.size()) < width) {
        out.push_back(' ');
    }
    return out;
}

void append_wrapped(std::vector<std::string>& lines, const std::string& text, int width) {
    if (width <= 0) {
        lines.push_back("");
        return;
    }
    size_t pos = 0;
    if (text.empty()) {
        lines.push_back("");
        return;
    }
    while (pos < text.size()) {
        lines.push_back(take_cells(text, pos, width));
    }
    if (!text.empty() && text.back() == '\n') {
        lines.push_back("");
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

int cell_width(const std::string& text) {
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

struct InputLayout {
    std::vector<std::string> lines;
    int cursor_line = 0;
    int cursor_col = 2;
};

InputLayout input_layout(const std::string& input, int cols) {
    InputLayout layout;
    const int first_width = std::max(1, cols - 2);
    const int continuation_width = std::max(1, cols - 2);
    size_t pos = 0;
    bool first_display_line = true;

    auto append_logical_line = [&](const std::string& logical) {
        size_t part_pos = 0;
        const std::string first_prefix = first_display_line ? "> " : "  ";
        const std::string continuation_prefix = "  ";
        bool emitted = false;
        while (part_pos < logical.size()) {
            const std::string prefix = emitted ? continuation_prefix : first_prefix;
            const int width = emitted ? continuation_width : first_width;
            layout.lines.push_back(prefix + take_cells(logical, part_pos, width));
            emitted = true;
            first_display_line = false;
        }
        if (!emitted) {
            layout.lines.push_back(first_prefix);
            first_display_line = false;
        }
    };

    if (input.empty()) {
        layout.lines.push_back("> ");
    } else {
        while (pos <= input.size()) {
            const size_t next = input.find('\n', pos);
            if (next == std::string::npos) {
                append_logical_line(input.substr(pos));
                break;
            }
            append_logical_line(input.substr(pos, next - pos));
            pos = next + 1;
            if (pos == input.size()) {
                append_logical_line("");
                break;
            }
        }
    }

    layout.cursor_line = std::max(0, static_cast<int>(layout.lines.size()) - 1);
    layout.cursor_col = std::min(cols, std::max(1, cell_width(layout.lines.back()) + 1));
    return layout;
}

void render(const provider::RequestContext& context,
            const chat::Session& session,
            const std::string& input,
            const std::string& status) {
    const TuiSize size = terminal_size();
    const int cols = std::max(20, size.cols);
    const int rows = std::max(8, size.rows);
    const InputLayout input_view = input_layout(input, cols);
    const int max_input_rows = std::max(3, rows / 3);
    const int input_rows = std::min(max_input_rows, std::max(3, static_cast<int>(input_view.lines.size())));
    const int history_rows = std::max(1, rows - 3 - input_rows);

    std::vector<std::string> history;
    for (const provider::Message& message : session.messages) {
        const std::string prefix = message_label(message.role) + ": ";
        const std::string content = message.role == "assistant" && message.content.empty() ? "(waiting...)" : message.content;
        std::vector<std::string> wrapped;
        append_wrapped(wrapped, content, std::max(1, cols - static_cast<int>(prefix.size())));
        for (size_t i = 0; i < wrapped.size(); ++i) {
            history.push_back((i == 0 ? prefix : std::string(prefix.size(), ' ')) + wrapped[i]);
        }
    }

    std::cout << "\x1b[?25h\x1b[2J\x1b[H";
    std::cout << pad_or_clip("pkchat TUI  Endpoint: " + context.chat_url, cols) << "\n";
    std::cout << pad_or_clip("Model: " + model_line(context) + "  Enter sends | Alt+Enter newline | Ctrl+S sends | Ctrl+C cancels", cols) << "\n";

    const int start = history.size() > static_cast<size_t>(history_rows)
                          ? static_cast<int>(history.size()) - history_rows
                          : 0;
    int printed = 0;
    for (size_t i = static_cast<size_t>(start); i < history.size() && printed < history_rows; ++i, ++printed) {
        std::cout << pad_or_clip(history[i], cols) << "\n";
    }
    while (printed++ < history_rows) {
        std::cout << std::string(cols, ' ') << "\n";
    }

    std::cout << "\x1b[7m" << pad_or_clip(status, cols) << "\x1b[0m\n";

    const int input_start = input_view.lines.size() > static_cast<size_t>(input_rows)
                                ? static_cast<int>(input_view.lines.size()) - input_rows
                                : 0;
    int input_printed = 0;
    for (size_t i = static_cast<size_t>(input_start);
         i < input_view.lines.size() && input_printed < input_rows;
         ++i, ++input_printed) {
        std::cout << pad_or_clip(input_view.lines[i], cols) << "\n";
    }
    while (input_printed++ < input_rows) {
        std::cout << std::string(cols, ' ') << "\n";
    }

    const int visible_cursor_line = std::max(0, input_view.cursor_line - input_start);
    const int cursor_row = 2 + history_rows + 1 + std::min(visible_cursor_line, input_rows - 1) + 1;
    const int cursor_col = std::min(cols, std::max(1, input_view.cursor_col));
    std::cout << "\x1b[" << cursor_row << ";" << cursor_col << "H";
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
    std::string input;
    std::string status = "Ready";
    bool quit = false;
    bool escape_pending = false;
    bool escape_sequence = false;
    size_t pending_user = static_cast<size_t>(-1);
    size_t pending_assistant = static_cast<size_t>(-1);

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
            status = "Enter sends | Alt+Enter inserts newline | Ctrl+S sends | /quit /models /save /load";
            return;
        }
        if (text == "/clear") {
            session.messages.clear();
            apply_system_prompt(session, context.options.system);
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
        const std::string raw = input;
        const std::string text = trim_ascii(raw);
        input.clear();
        if (text.empty()) {
            return;
        }
        if (raw.find('\n') == std::string::npos && text[0] == '/') {
            handle_command(text);
        } else {
            start_turn(raw);
        }
    };

    if (!trim_ascii(context.options.prompt).empty()) {
        start_turn(context.options.prompt);
    }

    render(context, session, input, status);
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
        if (ready > 0 && FD_ISSET(STDIN_FILENO, &readfds)) {
            char ch = 0;
            while (read(STDIN_FILENO, &ch, 1) > 0) {
                if (escape_sequence) {
                    if (ch >= '@' && ch <= '~') {
                        escape_sequence = false;
                    }
                    continue;
                }
                if (escape_pending) {
                    escape_pending = false;
                    if (ch == '\r' || ch == '\n') {
                        input.push_back('\n');
                        status = "Inserted newline. Ctrl+S sends the multiline prompt.";
                        continue;
                    }
                    if (ch == '[' || ch == 'O') {
                        escape_sequence = true;
                        continue;
                    }
                    if (static_cast<unsigned char>(ch) >= 32 || ch == '\t') {
                        input.push_back(ch);
                    }
                    continue;
                }
                if (ch == 27) {
                    escape_pending = true;
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
                if (ch == 4 && input.empty()) {
                    quit = true;
                    continue;
                }
                if (ch == 127 || ch == 8) {
                    pop_utf8_char(input);
                    continue;
                }
                if (ch == '\r' || ch == '\n') {
                    submit_input();
                    continue;
                }
                if (static_cast<unsigned char>(ch) >= 32 || ch == '\t') {
                    input.push_back(ch);
                }
            }
        }
        render(context, session, input, status);
    }

    model_job.cancel();
    file_job.cancel();
    model_job.join();
    file_job.join();
    return 0;
}

}  // namespace pkchat::tui
