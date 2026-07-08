#pragma once

#include <termios.h>

#include <string>

#include "editor/editor.hpp"
#include "editor/editor_assist.hpp"
#include "tui/activity.hpp"

namespace pkchat::editor {

struct TerminalSize {
    int rows = 24;
    int cols = 80;
};

enum class MinibufferAction {
    None,
    SaveFile,
    SaveAsFile,
    LoadFile,
    Search,
    ReplaceSearch,
    ReplaceWith,
    ConfirmLoad,
    ConfirmAutosaveRecovery,
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

struct PendingAutosaveRecovery {
    std::string path;
    std::string autosave_path;
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

struct EditorAssistDisplay {
    bool active = false;
    std::string provider_name;
    std::string model_name;
    std::string suffix;
    tui::ActivityKind kind = tui::ActivityKind::None;
    size_t frame = 0;
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
void render_terminal(EditorState& state,
                     const MinibufferState& minibuffer,
                     bool help_view = false,
                     const EditorAssistDisplay* assist_display = nullptr);
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

void recover_editor_from_autosave(EditorState& state,
                                  const std::string& path,
                                  const std::string& autosave_path,
                                  const EditorSettings& settings,
                                  MinibufferState& minibuffer);

bool offer_autosave_recovery_before_load(const std::string& path,
                                         const EditorSettings& settings,
                                         MinibufferState& minibuffer,
                                         PendingAutosaveRecovery& pending);

bool handle_minibuffer_key(EditorState& state,
                           MinibufferState& minibuffer,
                           unsigned char ch,
                           bool& quit,
                           std::string& last_search,
                           ReplaceSession& replace,
                           const EditorSettings& settings,
                           std::string& pending_load_path,
                           bool& pending_quit_after_save,
                           PendingSaveRequest& pending_save,
                           PendingAutosaveRecovery& pending_autosave_recovery);

bool handle_replace_key(EditorState& state,
                        MinibufferState& minibuffer,
                        ReplaceSession& replace,
                        unsigned char ch);

bool needs_overwrite_confirm(const std::string& target, const std::string& current_path);
std::string overwrite_prompt_message(const std::string& path);

void request_save_editor_to_path(EditorState& state,
                                 const std::string& path,
                                 MinibufferState& minibuffer,
                                 bool update_path,
                                 bool quit_after_save,
                                 bool& quit,
                                 PendingSaveRequest& pending_save,
                                 const EditorSettings& settings);

void request_load_editor_from_path(EditorState& state,
                                   const std::string& path,
                                   const EditorSettings& settings,
                                   MinibufferState& minibuffer,
                                   std::string& pending_load_path,
                                   PendingAutosaveRecovery& pending_autosave_recovery);

std::string read_escape_suffix();

}  // namespace pkchat::editor