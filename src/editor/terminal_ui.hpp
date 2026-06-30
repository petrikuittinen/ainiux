#pragma once

#include <termios.h>

#include <string>

#include "editor/editor.hpp"
#include "editor/editor_assist.hpp"

namespace pkchat::editor {

struct TerminalSize {
    int rows = 24;
    int cols = 80;
};

enum class MinibufferAction {
    None,
    SaveFile,
    LoadFile,
    Search,
    ReplaceSearch,
    ReplaceWith,
    ConfirmLoad,
    ConfirmQuit,
    ConfirmSaveOnQuit,
    ConfirmOverwrite,
    AssistCommand,
    AssistScopeChoice,
    AssistPromptMode,
};

struct PendingSaveRequest {
    std::string path;
    bool update_path = true;
    bool quit_after_save = false;
};

struct MinibufferState {
    bool active = false;
    MinibufferAction action = MinibufferAction::None;
    std::string prompt;
    std::string input;
    std::string message = "Ready";
};

struct ReplaceSession {
    bool active = false;
    bool match_valid = false;
    size_t match_start = 0;
    std::string needle;
    std::string replacement;
};

struct PendingAssist {
    AssistCommandKind kind = AssistCommandKind::Unknown;
    size_t command_index = 0;
    std::string custom_prompt;
};

class TerminalSession {
   public:
    TerminalSession() = default;
    ~TerminalSession();
    TerminalSession(const TerminalSession&) = delete;
    TerminalSession& operator=(const TerminalSession&) = delete;

    Error enter();
    void restore();

   private:
    termios original_{};
    bool active_ = false;
};

TerminalSize terminal_size();
void render_terminal(EditorState& state, const MinibufferState& minibuffer, bool help_view = false);
std::string editor_status_line(const EditorState& state, bool help_view = false);
void dispatch_escape_sequence(EditorState& state,
                              const std::string& sequence,
                              std::string& status,
                              const std::string& last_search);
void handle_escape(EditorState& state, std::string& status, const std::string& last_search);

bool is_assist_minibuffer_action(MinibufferAction action);
void exit_assist_command_mode(MinibufferState& minibuffer, AssistCompleterState& completer);
void start_assist_command_mode(MinibufferState& minibuffer, AssistCompleterState& completer);
void minibuffer_message(MinibufferState& minibuffer, std::string message);
void start_minibuffer(MinibufferState& minibuffer,
                      MinibufferAction action,
                      std::string prompt,
                      std::string initial = "");

bool confirm_huge_load_before_terminal(const std::string& path, const FileLoadCheck& check);

bool handle_minibuffer_key(EditorState& state,
                           MinibufferState& minibuffer,
                           unsigned char ch,
                           bool& quit,
                           std::string& last_search,
                           ReplaceSession& replace,
                           const EditorSettings& settings,
                           std::string& pending_load_path,
                           bool& pending_quit_after_save,
                           PendingSaveRequest& pending_save);

bool handle_replace_key(EditorState& state,
                        MinibufferState& minibuffer,
                        ReplaceSession& replace,
                        unsigned char ch);

void request_save_editor_to_path(EditorState& state,
                                 const std::string& path,
                                 MinibufferState& minibuffer,
                                 bool update_path,
                                 bool quit_after_save,
                                 bool& quit,
                                 PendingSaveRequest& pending_save);

std::string read_escape_suffix();

}  // namespace pkchat::editor