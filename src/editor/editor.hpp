#pragma once

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "common.hpp"
#include "editor/clipboard.hpp"
#include "editor/editor_prompts.hpp"
#include "editor/file_session.hpp"
#include "editor/selection.hpp"
#include "editor/word_completion.hpp"
#include "highlight/highlight.hpp"

namespace ainiux::app {
struct EditorRunResult;
struct InteractiveSession;
}

namespace ainiux::tui {
class ThemeRegistry;
}

namespace ainiux::editor {

struct Rect {
    int row = 1;
    int col = 1;
    int height = 1;
    int width = 1;
};

struct CursorPoint {
    int row = 0;
    int col = 0;
    bool visible = false;
};

struct RenderedPanel {
    std::vector<std::string> lines;
    struct Span {
        size_t start = 0;
        size_t end = 0;
        highlight::TokenRole role = highlight::TokenRole::Operator;
        bool syntax = false;
        bool selected = false;
    };
    std::vector<std::vector<Span>> line_spans;
    CursorPoint cursor;
};

struct EditorSnapshot {
    std::string content;
    size_t cursor = 0;
    size_t preferred_column = 0;
    size_t scroll_line = 0;
    size_t scroll_column = 0;
};

constexpr size_t kDefaultUndoLimit = 5;
constexpr long long kDefaultHugeFileSizeWarningBytes = 1024LL * 1024LL * 1024LL;
constexpr long long kNoEditorFileSizeLimit = -1;
constexpr size_t kDefaultAutoSaveThreshold = 300;
constexpr int kDefaultAutoSaveTimeoutSeconds = 30;
constexpr long long kDefaultAutoSaveSizeLimit = 10LL * 1024LL * 1024LL;
constexpr const char* kDefaultAutoSavePostfix = "~";
constexpr size_t kDefaultAiContinuePrefixMaxChars = 4000;
constexpr size_t kDefaultAiContinuePostfixMaxChars = 2000;
constexpr size_t kDefaultAiContinueProsePrefixMaxChars = 16384;
constexpr size_t kDefaultAiContinueProsePostfixMaxChars = 4096;
constexpr int kDefaultAiContinueMaxTokens = 32768;
constexpr size_t kDefaultTabWidth = 4;
constexpr size_t kMaxTabWidth = 32;
constexpr size_t kDefaultTextAlignWidth = 78;
constexpr size_t kMinTextAlignWidthExclusive = 20;
constexpr size_t kMaxTextAlignWidth = 1000;

enum class TabStyle {
    Spaces,
    Tab,
};

enum class LineBreak {
    Lf,
    Cr,
    Crlf,
};

const char* tab_style_name(TabStyle style);
const char* linebreak_name(LineBreak linebreak);
bool parse_tab_style(const std::string& value, TabStyle& out);
bool parse_linebreak(const std::string& value, LineBreak& out);

struct EditorSettings {
    size_t undo_limit = kDefaultUndoLimit;
    long long huge_file_size_warning = kDefaultHugeFileSizeWarningBytes;
    long long file_size_limit = kNoEditorFileSizeLimit;
    bool auto_save_mode = true;
    std::string auto_save_postfix = kDefaultAutoSavePostfix;
    size_t auto_save_threshold = kDefaultAutoSaveThreshold;
    int auto_save_timeout_seconds = kDefaultAutoSaveTimeoutSeconds;
    long long auto_save_size_limit = kDefaultAutoSaveSizeLimit;
    const tui::ThemeRegistry* themes = nullptr;
    std::string theme_name = "dark";
    bool use_colors = true;
    bool highlight_enabled = true;
    size_t tab_width = kDefaultTabWidth;
    TabStyle tab_style = TabStyle::Spaces;
    LineBreak linebreak = LineBreak::Lf;
    // Default column width for /left-align, /right-align, /center-align, /justify
    // when the user omits WIDTH (must be greater than 20).
    size_t text_align_width = kDefaultTextAlignWidth;
    // When true, open dired after the editor UI is ready (CLI --dired).
    bool start_dired = false;
    // Directory or glob for start_dired; empty means ".".
    std::string start_dired_path;
};

struct AiContinueContext;

struct FileLoadCheck {
    std::uintmax_t size = 0;
    bool should_warn = false;
};

enum class VerticalMovementMode {
    LogicalLine,
    VisualRow,
};

enum class EditorMode {
    Editor,
    Chat,
};

class PieceTable {
   public:
    static PieceTable from_string(std::string original);

    size_t size() const { return total_size_; }
    bool empty() const { return total_size_ == 0; }
    std::string str() const;
    Error write_to(std::ostream& out) const;
    Error write_to(std::ostream& out, LineBreak linebreak) const;

    char char_at(size_t pos) const;
    Error insert(size_t pos, const std::string& text);
    Error erase(size_t pos, size_t count);

    size_t previous_char_offset(size_t pos) const;
    size_t next_char_offset(size_t pos) const;

    size_t line_count() const;
    size_t line_start(size_t line) const;
    size_t line_length(size_t line) const;
    size_t line_for_offset(size_t offset) const;
    size_t display_column_for_offset(size_t offset, size_t tab_width = kDefaultTabWidth) const;
    size_t offset_for_line_column(size_t line,
                                  size_t display_column,
                                  size_t tab_width = kDefaultTabWidth) const;
    std::string line_text(size_t line) const;
    std::string range_text(size_t start, size_t length) const;

   private:
    enum class Source { Original, Add };

    struct Piece {
        Source source = Source::Original;
        size_t start = 0;
        size_t length = 0;
    };

    const std::string& source_for(const Piece& piece) const;
    void append_range(std::string& out, size_t start, size_t length) const;
    void invalidate_line_cache();
    void rebuild_line_cache() const;

    std::string original_;
    std::string add_;
    std::vector<Piece> pieces_;
    size_t total_size_ = 0;
    mutable bool line_cache_valid_ = false;
    mutable std::vector<size_t> line_starts_;
};

struct EditorState {
    EditorState();

    PieceTable text;
    size_t cursor = 0;
    size_t preferred_column = 0;
    size_t scroll_line = 0;
    size_t scroll_column = 0;
    std::string path;
    bool dirty = false;
    bool read_only = false;
    bool reload_required = false;
    std::string canonical_path;
    FileFingerprint disk_fingerprint;
    bool has_disk_fingerprint = false;
    // EditorState is copied while switching buffers and temporarily entering
    // chat/help modes. Shared ownership keeps one logical buffer lock alive;
    // EditorFileLock remains the sole owner responsible for directory cleanup.
    std::shared_ptr<EditorFileLock> file_lock;
    VerticalMovementMode vertical_movement = VerticalMovementMode::LogicalLine;
    EditorMode mode = EditorMode::Editor;
    Selection selection;
    highlight::Language language = highlight::Language::Text;
    bool language_automatic = true;
    bool highlight_enabled = true;
    size_t tab_width = kDefaultTabWidth;
    TabStyle tab_style = TabStyle::Spaces;
    LineBreak linebreak = LineBreak::Lf;

    static EditorState from_text(std::string content);
    std::uint64_t buffer_id() const { return buffer_id_; }
    std::uint64_t revision() const { return revision_; }
    void set_path(std::string value);
    void set_language(highlight::Language value, bool automatic);
    void redetect_language();
    Error begin_file_session(const std::string& value, bool target_exists);
    Error retry_file_lock();
    void release_file_session();
    Error refresh_disk_fingerprint();
    Error mutation_allowed();

    Error insert(const std::string& value);
    Error insert_without_undo(const std::string& value);
    void finalize_stream_edit(const EditorSnapshot& before);
    EditorSnapshot capture_state() const;
    void restore_captured_state(const EditorSnapshot& snapshot);
    Error replace(size_t pos, size_t count, const std::string& value);
    Error replace_completion(size_t pos,
                             size_t count,
                             const std::string& value,
                             bool record_undo);
    Error erase_before_cursor();
    Error erase_at_cursor();
    bool undo();
    bool redo();
    void revert_to_snapshot(const EditorSnapshot& snapshot);
    bool can_undo() const;
    bool can_redo() const;
    void set_undo_limit(size_t limit);
    size_t undo_limit() const;
    void clear_undo_history();
    bool search(const std::string& needle);
    bool search_next(const std::string& needle);
    bool search_previous(const std::string& needle);
    Error replace_all_from(size_t start, const std::string& needle, const std::string& value, size_t& replacements);
    void clear_selection();
    void select_all();
    // Move cursor to 1-based line start. Sets message; returns true if moved.
    // Invalid values (non-numeric, ≤0, past end) leave the cursor and set an error message.
    bool goto_line(const std::string& line_text, std::string& message);
    size_t selection_end_exclusive() const;
    std::string selected_text() const;
    Error copy_selection(Clipboard& clipboard);
    Error cut_selection(Clipboard& clipboard);
    Error paste(Clipboard& clipboard);
    Error indent();
    Error outdent();
    void apply_movement(MovementKey key,
                        const Rect& rect,
                        bool extend_selection,
                        bool alt = false,
                        bool ctrl = false);
    void move_left();
    void move_right();
    void move_up();
    void move_down();
    void move_up(const Rect& rect);
    void move_down(const Rect& rect);
    void page_up(const Rect& rect);
    void page_down(const Rect& rect);
    void move_up_visual(const Rect& rect);
    void move_down_visual(const Rect& rect);
    void move_home();
    void move_end();
    void move_line_home(const Rect& rect);
    void move_line_end(const Rect& rect);
    Error kill_to_line_end(Clipboard& clipboard);
    void ensure_cursor_visible(const Rect& rect);
    // Scroll the viewport by rendered rows without moving or editing the caret.
    // Positive values move toward the document end; negative values move up.
    bool scroll_view_rows(const Rect& rect, int rows);
    RenderedPanel render(const Rect& rect) const;
    // Render using an explicit window view (per-pane cursor/scroll) while
    // keeping this buffer's text, selection, and highlight cache.
    RenderedPanel render(const Rect& rect,
                         size_t view_cursor,
                         size_t view_scroll_line,
                         size_t view_scroll_column) const;
    // Counts visual rows only until limit is reached. This is intended for
    // responsive UI sizing and avoids scanning the rest of a large draft.
    size_t visual_row_count_bounded(size_t width, size_t limit) const;
    size_t autosave_pending_bytes() const;
    void record_autosave_change(size_t bytes);
    void reset_autosave_pending();
    const WordIndex& completion_word_index() const;
    const WordIndex& completion_word_index(const std::string& current_text) const;
    void invalidate_word_index();

   private:
    void begin_movement(bool extend_selection);
    void finish_movement(bool extend_selection);

    EditorSnapshot snapshot() const;
    void restore_snapshot(const EditorSnapshot& snapshot);
    void remember_undo(EditorSnapshot snapshot);

    std::vector<EditorSnapshot> undo_stack_;
    std::vector<EditorSnapshot> redo_stack_;
    size_t undo_limit_ = kDefaultUndoLimit;
    size_t autosave_pending_bytes_ = 0;
    mutable highlight::DocumentCache highlight_cache_;
    mutable WordIndex word_index_;
    std::uint64_t buffer_id_ = 0;
    std::uint64_t revision_ = 0;
};

std::string editor_buffer_display_name(const EditorState& state, size_t index);
std::string editor_buffer_list_text(const std::vector<EditorState>& buffers, size_t selected);
// display_order maps display row -> underlying buffer index. Empty = identity.
std::string editor_buffer_list_text(const std::vector<EditorState>& buffers,
                                    size_t selected,
                                    const std::vector<size_t>& display_order);
size_t move_editor_buffer_selection(size_t selected, size_t count, MovementKey key);

RenderedPanel render_panel(const PieceTable& text,
                           const Rect& rect,
                           size_t cursor,
                           size_t scroll_line,
                           size_t scroll_column,
                           const std::optional<Selection>& selection = std::nullopt,
                           highlight::Language language = highlight::Language::Text,
                           bool highlight_enabled = false,
                           highlight::DocumentCache* highlight_cache = nullptr,
                           size_t tab_width = kDefaultTabWidth);

struct LoadedFile {
    PieceTable text;
    LineBreak linebreak = LineBreak::Lf;
    bool mixed_linebreaks = false;
    size_t tab_width = kDefaultTabWidth;
    TabStyle tab_style = TabStyle::Spaces;
    bool tab_width_detected = false;
    bool tab_style_detected = false;
};

struct IndentationDetection {
    size_t tab_width = kDefaultTabWidth;
    TabStyle tab_style = TabStyle::Spaces;
    bool tab_width_detected = false;
    bool tab_style_detected = false;
};

IndentationDetection detect_indentation(const std::string& text,
                                        size_t fallback_width = kDefaultTabWidth,
                                        TabStyle fallback_style = TabStyle::Spaces);

Error load_file(const std::string& path, PieceTable& out);
Error load_file(const std::string& path, const EditorSettings& settings, PieceTable& out);
Error load_file(const std::string& path, const EditorSettings& settings, LoadedFile& out);
Error check_load_file_size(const std::string& path, const EditorSettings& settings, FileLoadCheck& check);
Error save_file(const std::string& path, const PieceTable& text);
Error save_file(const std::string& path, const PieceTable& text, LineBreak linebreak);
Error ensure_empty_file(const std::string& path);

ainiux::app::EditorRunResult run_editor(const std::string& path,
                                        const std::string& save_as,
                                        const EditorSettings& settings,
                                        std::optional<AiContinueContext> ai_continue,
                                        const EditorAssistConfig& assist_config,
                                        ainiux::app::InteractiveSession* interactive = nullptr);

}  // namespace ainiux::editor
