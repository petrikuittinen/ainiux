#pragma once

#include <termios.h>

#include <functional>
#include <string>
#include <vector>

#include "editor/editor.hpp"
#include "editor/editor_assist.hpp"
#include "editor/path_completion.hpp"
#include "editor/split.hpp"
#include "tui/activity.hpp"

namespace ainiux::tui {
class ThemeRegistry;
enum class TuiMode;
namespace detail {
class TerminalFrameRenderer;
}
}

namespace ainiux::editor {

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
    ConfirmReloadAfterLock,
    ConfirmReasoning,
    AssistCommand,
    AssistScopeChoice,
    AssistPromptMode,
};

struct PendingSaveRequest {
    std::string path;
    std::string canonical_path;
    bool update_path = true;
    bool quit_after_save = false;
    bool external_change = false;
    FileFingerprint observed_disk;
    std::shared_ptr<EditorFileLock> destination_lock;
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
struct TerminalThemeStyle {
    const tui::ThemeRegistry* themes = nullptr;
    std::string theme_name = "dark";
    bool use_colors = true;
};

void render_terminal(EditorState& state,
                     const MinibufferState& minibuffer,
                     tui::detail::TerminalFrameRenderer& frame_renderer,
                     const TerminalThemeStyle& theme_style = {},
                     bool help_view = false,
                     const EditorAssistDisplay* assist_display = nullptr);

// Multi-pane editor layout. panes come from SplitLayout::layout_panes(editor_main_area()).
// buffer_at(index) must return the EditorState for that buffer index; the focused buffer
// may be a live working copy rather than the vector entry.
void render_terminal_splits(
    const std::vector<SplitPaneRect>& panes,
    const std::function<const EditorState&(size_t buffer_index)>& buffer_at,
    EditorState& focused_state,
    const MinibufferState& minibuffer,
    tui::detail::TerminalFrameRenderer& frame_renderer,
    const TerminalThemeStyle& theme_style = {},
    bool help_view = false,
    const EditorAssistDisplay* assist_display = nullptr,
    size_t pane_count_hint = 1);

void render_terminal_panel(EditorState& state,
                           const MinibufferState& minibuffer,
                           tui::detail::TerminalFrameRenderer& frame_renderer,
                           const TerminalThemeStyle& theme_style,
                           tui::TuiMode mode,
                           int& panel_scroll,
                           const char* panel_title_override = nullptr);
std::string editor_status_line(const EditorState& state,
                               bool help_view = false,
                               size_t split_pane_count = 1);
// Content area above status and minibuffer lines.
Rect editor_main_area();
void dispatch_escape_sequence(EditorState& state,
                              const std::string& sequence,
                              std::string& status,
                              const std::string& last_search,
                              const Rect* panel_rect = nullptr);
void handle_escape(EditorState& state,
                   std::string& status,
                   const std::string& last_search,
                   const Rect* panel_rect = nullptr);

bool is_assist_minibuffer_action(MinibufferAction action);
void exit_assist_command_mode(MinibufferState& minibuffer, AssistCompleterState& completer);
void start_assist_command_mode(MinibufferState& minibuffer, AssistCompleterState& completer);
void minibuffer_message(MinibufferState& minibuffer, std::string message);
Error paste_into_minibuffer(MinibufferState& minibuffer, const std::string& text);
void start_minibuffer(MinibufferState& minibuffer,
                      MinibufferAction action,
                      std::string prompt,
                      std::string initial = "",
                      PathCompleter* path_completer = nullptr);

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
                           PendingAutosaveRecovery& pending_autosave_recovery,
                           PathCompleter& path_completer);

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

}  // namespace ainiux::editor
