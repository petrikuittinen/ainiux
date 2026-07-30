#include "editor/split.hpp"

#include <algorithm>
#include <cctype>

namespace ainiux::editor {

PaneViewState pane_view_from_state(const EditorState& state) {
    PaneViewState view;
    view.cursor = state.cursor;
    view.preferred_column = state.preferred_column;
    view.scroll_line = state.scroll_line;
    view.scroll_column = state.scroll_column;
    return view;
}

void apply_pane_view_to_state(EditorState& state, const PaneViewState& view) {
    state.cursor = std::min(view.cursor, state.text.size());
    state.preferred_column = view.preferred_column;
    state.scroll_line = view.scroll_line;
    state.scroll_column = view.scroll_column;
}

SplitLayout::SplitLayout() {
    reset(0);
}

SplitLayout::SplitLayout(size_t buffer_index) {
    reset(buffer_index);
}

SplitLayout::~SplitLayout() {
    clear();
}

SplitLayout::SplitLayout(SplitLayout&& other) noexcept
    : root_(other.root_), nodes_(std::move(other.nodes_)), focused_leaf_(other.focused_leaf_) {
    other.root_ = nullptr;
    other.focused_leaf_ = 0;
}

SplitLayout& SplitLayout::operator=(SplitLayout&& other) noexcept {
    if (this != &other) {
        clear();
        root_ = other.root_;
        nodes_ = std::move(other.nodes_);
        focused_leaf_ = other.focused_leaf_;
        other.root_ = nullptr;
        other.focused_leaf_ = 0;
    }
    return *this;
}

void SplitLayout::clear() {
    for (Node* node : nodes_) {
        delete node;
    }
    nodes_.clear();
    root_ = nullptr;
    focused_leaf_ = 0;
    clear_previous_leaf();
}

void SplitLayout::remember_previous_leaf(size_t leaf_index) {
    previous_leaf_ = leaf_index;
    has_previous_leaf_ = true;
}

void SplitLayout::clear_previous_leaf() {
    previous_leaf_ = 0;
    has_previous_leaf_ = false;
}

void SplitLayout::clamp_previous_leaf() {
    const size_t count = leaf_count();
    if (!has_previous_leaf_ || count <= 1) {
        clear_previous_leaf();
        return;
    }
    if (previous_leaf_ >= count) {
        previous_leaf_ = count - 1;
    }
    if (previous_leaf_ == focused_leaf()) {
        // Prefer another pane so Ctrl+B/D still do something.
        previous_leaf_ = (focused_leaf() + 1) % count;
    }
}

SplitLayout::Node* SplitLayout::make_leaf(size_t buffer_index) {
    Node* node = new Node{};
    node->is_leaf = true;
    node->buffer_index = buffer_index;
    nodes_.push_back(node);
    return node;
}

SplitLayout::Node* SplitLayout::make_split(SplitKind kind, Node* first, Node* second) {
    Node* node = new Node{};
    node->is_leaf = false;
    node->kind = kind;
    node->ratio = 0.5;
    node->first = first;
    node->second = second;
    nodes_.push_back(node);
    return node;
}

void SplitLayout::reset(size_t buffer_index) {
    clear();
    root_ = make_leaf(buffer_index);
    focused_leaf_ = 0;
}

void SplitLayout::collect_leaves(const Node* node, std::vector<Node*>& out) const {
    if (node == nullptr) {
        return;
    }
    if (node->is_leaf) {
        out.push_back(const_cast<Node*>(node));
        return;
    }
    collect_leaves(node->first, out);
    collect_leaves(node->second, out);
}

size_t SplitLayout::leaf_count() const {
    std::vector<Node*> leaves;
    collect_leaves(root_, leaves);
    return leaves.size();
}

SplitLayout::Node* SplitLayout::leaf_at(size_t index) const {
    std::vector<Node*> leaves;
    collect_leaves(root_, leaves);
    if (index >= leaves.size()) {
        return nullptr;
    }
    return leaves[index];
}

size_t SplitLayout::focused_leaf() const {
    const size_t count = leaf_count();
    if (count == 0) {
        return 0;
    }
    return std::min(focused_leaf_, count - 1);
}

size_t SplitLayout::focused_buffer() const {
    const Node* leaf = leaf_at(focused_leaf());
    return leaf != nullptr ? leaf->buffer_index : 0;
}

bool SplitLayout::area_can_split(const Rect& area, SplitKind kind) const {
    if (kind == SplitKind::Vertical) {
        return area.width >= (kMinPaneWidth * 2 + kSeparatorSize);
    }
    return area.height >= (kMinPaneHeight * 2 + kSeparatorSize);
}

Rect SplitLayout::first_child_rect(const Rect& area, SplitKind kind, double ratio) {
    if (kind == SplitKind::Vertical) {
        const int inner = std::max(0, area.width - kSeparatorSize);
        const int first_w = std::max(kMinPaneWidth, static_cast<int>(inner * ratio));
        const int clamped = std::min(first_w, std::max(kMinPaneWidth, inner - kMinPaneWidth));
        return Rect{area.row, area.col, area.height, clamped};
    }
    const int inner = std::max(0, area.height - kSeparatorSize);
    const int first_h = std::max(kMinPaneHeight, static_cast<int>(inner * ratio));
    const int clamped = std::min(first_h, std::max(kMinPaneHeight, inner - kMinPaneHeight));
    return Rect{area.row, area.col, clamped, area.width};
}

Rect SplitLayout::second_child_rect(const Rect& area, SplitKind kind, double ratio) {
    const Rect first = first_child_rect(area, kind, ratio);
    if (kind == SplitKind::Vertical) {
        const int col = first.col + first.width + kSeparatorSize;
        const int width = std::max(1, area.col + area.width - col);
        return Rect{area.row, col, area.height, width};
    }
    const int row = first.row + first.height + kSeparatorSize;
    const int height = std::max(1, area.row + area.height - row);
    return Rect{row, area.col, height, area.width};
}

void SplitLayout::layout_node(const Node* node,
                             const Rect& area,
                             size_t& leaf_counter,
                             std::vector<SplitPaneRect>& out) const {
    if (node == nullptr) {
        return;
    }
    if (node->is_leaf) {
        SplitPaneRect pane;
        pane.buffer_index = node->buffer_index;
        pane.leaf_index = leaf_counter;
        pane.rect = area;
        pane.focused = (leaf_counter == focused_leaf());
        pane.view = node->view;
        ++leaf_counter;
        out.push_back(pane);
        return;
    }
    layout_node(node->first, first_child_rect(area, node->kind, node->ratio), leaf_counter, out);
    layout_node(node->second, second_child_rect(area, node->kind, node->ratio), leaf_counter, out);
}

std::vector<SplitPaneRect> SplitLayout::layout_panes(const Rect& outer_area) const {
    std::vector<SplitPaneRect> panes;
    size_t counter = 0;
    layout_node(root_, outer_area, counter, panes);
    return panes;
}

Rect SplitLayout::focused_rect(const Rect& outer_area) const {
    const std::vector<SplitPaneRect> panes = layout_panes(outer_area);
    const size_t focus = focused_leaf();
    for (const SplitPaneRect& pane : panes) {
        if (pane.leaf_index == focus) {
            return pane.rect;
        }
    }
    return outer_area;
}

bool SplitLayout::split_focused(SplitKind kind, const Rect& outer_area) {
    if (root_ == nullptr) {
        reset(0);
    }
    const Rect focus_rect = focused_rect(outer_area);
    if (!area_can_split(focus_rect, kind)) {
        return false;
    }
    std::vector<Node*> leaves;
    collect_leaves(root_, leaves);
    if (focused_leaf_ >= leaves.size()) {
        focused_leaf_ = leaves.empty() ? 0 : leaves.size() - 1;
    }
    Node* target = leaves[focused_leaf_];
    const size_t buffer = target->buffer_index;
    const PaneViewState inherited = target->view;
    Node* left = make_leaf(buffer);
    Node* right = make_leaf(buffer);
    left->view = inherited;
    right->view = inherited;
    // Replace target leaf in place by converting it into a split node.
    target->is_leaf = false;
    target->kind = kind;
    target->ratio = 0.5;
    target->buffer_index = 0;
    target->view = PaneViewState{};
    target->first = left;
    target->second = right;
    // Keep focus on the first child of the new split (original content).
    // Leaf indices: all leaves before focus unchanged; focus stays the same index.
    // The new sibling is the natural "other pane" for Ctrl+B / Ctrl+D.
    remember_previous_leaf(focused_leaf_ + 1);
    return true;
}

bool SplitLayout::split_and_open_buffer(SplitKind kind,
                                        const Rect& outer_area,
                                        size_t new_buffer_index) {
    const size_t source_leaf = focused_leaf();
    if (!split_focused(kind, outer_area)) {
        return false;
    }
    // After a successful split, focus remains on source_leaf; the sibling is next.
    const size_t sibling_leaf = source_leaf + 1;
    Node* sibling = leaf_at(sibling_leaf);
    if (sibling == nullptr || !sibling->is_leaf) {
        return false;
    }
    sibling->buffer_index = new_buffer_index;
    remember_previous_leaf(source_leaf);
    focused_leaf_ = sibling_leaf;
    return true;
}

void SplitLayout::focus_next() {
    const size_t count = leaf_count();
    if (count <= 1) {
        focused_leaf_ = 0;
        clear_previous_leaf();
        return;
    }
    remember_previous_leaf(focused_leaf());
    focused_leaf_ = (focused_leaf() + 1) % count;
}

std::optional<size_t> SplitLayout::other_scroll_leaf() const {
    const size_t count = leaf_count();
    if (count < 2) {
        return std::nullopt;
    }
    const size_t focus = focused_leaf();
    if (has_previous_leaf_ && previous_leaf_ < count && previous_leaf_ != focus) {
        return previous_leaf_;
    }
    return (focus + 1) % count;
}

SplitLayout::Node* SplitLayout::close_leaf_in(Node* node,
                                             size_t target_leaf,
                                             size_t& counter,
                                             bool& removed) {
    if (node == nullptr) {
        return nullptr;
    }
    if (node->is_leaf) {
        if (counter == target_leaf) {
            removed = true;
            ++counter;
            return nullptr;
        }
        ++counter;
        return node;
    }
    Node* first = close_leaf_in(node->first, target_leaf, counter, removed);
    Node* second = close_leaf_in(node->second, target_leaf, counter, removed);
    if (first == nullptr && second == nullptr) {
        return nullptr;
    }
    if (first == nullptr) {
        return second;
    }
    if (second == nullptr) {
        return first;
    }
    node->first = first;
    node->second = second;
    return node;
}

bool SplitLayout::close_focused() {
    if (leaf_count() <= 1) {
        return false;
    }
    const size_t target = focused_leaf();
    size_t counter = 0;
    bool removed = false;
    root_ = close_leaf_in(root_, target, counter, removed);
    if (root_ == nullptr) {
        reset(0);
        return true;
    }
    const size_t remaining = leaf_count();
    if (target >= remaining) {
        focused_leaf_ = remaining - 1;
    } else {
        focused_leaf_ = target;
    }
    // Renumber: leaves after the closed index shift down.
    if (has_previous_leaf_) {
        if (previous_leaf_ == target) {
            clear_previous_leaf();
        } else if (previous_leaf_ > target) {
            --previous_leaf_;
        }
    }
    clamp_previous_leaf();
    return removed;
}

void SplitLayout::maximize_focused() {
    const size_t buffer = focused_buffer();
    const PaneViewState view = focused_view();
    reset(buffer);
    set_focused_view(view);
}

PaneViewState SplitLayout::leaf_view(size_t leaf_index) const {
    const Node* node = leaf_at(leaf_index);
    if (node == nullptr || !node->is_leaf) {
        return PaneViewState{};
    }
    return node->view;
}

void SplitLayout::set_leaf_view(size_t leaf_index, const PaneViewState& view) {
    Node* node = leaf_at(leaf_index);
    if (node == nullptr || !node->is_leaf) {
        return;
    }
    node->view = view;
}

PaneViewState SplitLayout::focused_view() const {
    return leaf_view(focused_leaf());
}

void SplitLayout::set_focused_view(const PaneViewState& view) {
    set_leaf_view(focused_leaf(), view);
}

void SplitLayout::set_focused_buffer(size_t buffer_index) {
    Node* leaf = leaf_at(focused_leaf());
    if (leaf != nullptr) {
        leaf->buffer_index = buffer_index;
    }
}

void SplitLayout::on_buffer_removed(size_t removed_index, size_t fallback_buffer) {
    // Adjust fallback as if the removed buffer is already erased from the vector.
    size_t adjusted_fallback = fallback_buffer;
    if (adjusted_fallback == removed_index) {
        adjusted_fallback = removed_index > 0 ? removed_index - 1 : 0;
    } else if (adjusted_fallback > removed_index) {
        --adjusted_fallback;
    }
    std::vector<Node*> leaves;
    collect_leaves(root_, leaves);
    for (Node* leaf : leaves) {
        if (leaf->buffer_index == removed_index) {
            leaf->buffer_index = adjusted_fallback;
        } else if (leaf->buffer_index > removed_index) {
            --leaf->buffer_index;
        }
    }
}

void SplitLayout::clamp_buffers(size_t buffer_count) {
    if (buffer_count == 0) {
        return;
    }
    std::vector<Node*> leaves;
    collect_leaves(root_, leaves);
    for (Node* leaf : leaves) {
        if (leaf->buffer_index >= buffer_count) {
            leaf->buffer_index = buffer_count - 1;
        }
    }
}

std::string window_prefix_action(unsigned char ch) {
    // Esc or a second Ctrl+X cancels the window-command prefix.
    if (ch == 27 || ch == 24) {
        return "cancel";
    }
    if (ch == 'v' || ch == 'V' || ch == '3') {
        return "split-v";
    }
    if (ch == 'h' || ch == 'H' || ch == '2') {
        return "split-h";
    }
    if (ch == 'o' || ch == 'O') {
        return "other";
    }
    if (ch == '0') {
        return "close";
    }
    if (ch == '1') {
        return "maximize";
    }
    return "";
}

}  // namespace ainiux::editor
