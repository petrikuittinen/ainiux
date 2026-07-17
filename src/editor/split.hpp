#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "editor/editor.hpp"

namespace ainiux::editor {

// Horizontal: stacked top/bottom (divider runs horizontally).
// Vertical: side-by-side left/right (divider runs vertically).
enum class SplitKind {
    Horizontal,
    Vertical,
};

struct SplitPaneRect {
    size_t buffer_index = 0;
    size_t leaf_index = 0;
    Rect rect;
    bool focused = false;
};

// Binary-tree window layout for the standalone editor (Emacs-style).
// Leaves point at buffer indices; the tree is independent of buffer content.
class SplitLayout {
   public:
    SplitLayout();
    explicit SplitLayout(size_t buffer_index);
    ~SplitLayout();
    SplitLayout(const SplitLayout&) = delete;
    SplitLayout& operator=(const SplitLayout&) = delete;
    SplitLayout(SplitLayout&&) noexcept;
    SplitLayout& operator=(SplitLayout&&) noexcept;

    void reset(size_t buffer_index);

    size_t leaf_count() const;
    size_t focused_leaf() const;
    size_t focused_buffer() const;
    bool has_split() const { return leaf_count() > 1; }

    // Split the focused leaf; both panes show the same buffer initially.
    // Returns false if the focused pane is too small to split.
    bool split_focused(SplitKind kind, const Rect& outer_area);

    // Split the focused leaf, put new_buffer_index in the new sibling pane, and
    // focus that sibling. The original pane keeps its buffer. Returns false if
    // the focused pane is too small to split.
    bool split_and_open_buffer(SplitKind kind, const Rect& outer_area, size_t new_buffer_index);

    // Focus the next leaf in preorder (wraps). Records the prior focus as the
    // "other" scroll target for Ctrl+B / Ctrl+D.
    void focus_next();

    // Close the focused leaf. Returns false when only one leaf remains.
    bool close_focused();

    // Collapse the tree to a single leaf (the current focus).
    void maximize_focused();

    void set_focused_buffer(size_t buffer_index);

    // Leaf to scroll with Ctrl+B/Ctrl+D without moving focus: the last pane the
    // user left (via focus_next or after a split). Falls back to the next pane
    // when no prior focus is recorded.
    std::optional<size_t> other_scroll_leaf() const;

    // After buffers.erase(removed), renumber leaf buffer indices.
    // Leaves that pointed at the removed buffer take fallback_buffer.
    void on_buffer_removed(size_t removed_index, size_t fallback_buffer);

    // Clamp all buffer indices into [0, buffer_count).
    void clamp_buffers(size_t buffer_count);

    std::vector<SplitPaneRect> layout_panes(const Rect& outer_area) const;
    Rect focused_rect(const Rect& outer_area) const;

    static constexpr int kMinPaneWidth = 8;
    static constexpr int kMinPaneHeight = 2;
    static constexpr int kSeparatorSize = 1;

   private:
    struct Node {
        bool is_leaf = true;
        size_t buffer_index = 0;
        SplitKind kind = SplitKind::Vertical;
        double ratio = 0.5;
        Node* first = nullptr;
        Node* second = nullptr;
    };

    Node* root_ = nullptr;
    // Heap-owned nodes so pointers stay stable across tree rewrites of other nodes.
    std::vector<Node*> nodes_;
    size_t focused_leaf_ = 0;
    size_t previous_leaf_ = 0;
    bool has_previous_leaf_ = false;

    void remember_previous_leaf(size_t leaf_index);
    void clear_previous_leaf();
    void clamp_previous_leaf();

    Node* make_leaf(size_t buffer_index);
    Node* make_split(SplitKind kind, Node* first, Node* second);
    void clear();
    void collect_leaves(const Node* node, std::vector<Node*>& out) const;
    Node* leaf_at(size_t index) const;
    void layout_node(const Node* node,
                     const Rect& area,
                     size_t& leaf_counter,
                     std::vector<SplitPaneRect>& out) const;
    bool area_can_split(const Rect& area, SplitKind kind) const;
    static Rect first_child_rect(const Rect& area, SplitKind kind, double ratio);
    static Rect second_child_rect(const Rect& area, SplitKind kind, double ratio);
    Node* close_leaf_in(Node* node, size_t target_leaf, size_t& counter, bool& removed);
};

// Parse the second key of a Ctrl+G window command sequence.
// Returns an action name: "split-v", "split-h", "other", "close", "maximize",
// "cancel", or empty for unknown.
std::string window_prefix_action(unsigned char ch);

}  // namespace ainiux::editor
