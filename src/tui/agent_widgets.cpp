#include "tui/agent_widgets.hpp"

#include "ainiux/version.hpp"
#include "editor/detail/wrap.hpp"
#include "editor/detail/unicode.hpp"
#include "tui/tui.hpp"
#include "ui/provider_model_display.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <sstream>

namespace ainiux::tui {
namespace {

namespace fs = std::filesystem;

char lower_ascii(char ch) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
}

size_t text_cells(const std::string& text) {
    return editor::detail::display_width_for_range(text, 0, text.size());
}

std::string shorten_end(const std::string& text, size_t cells) {
    if (text_cells(text) <= cells) return text;
    if (cells == 0) return "";
    if (cells == 1) return u8"…";
    std::string out;
    size_t pos = 0;
    size_t used = 0;
    while (pos < text.size()) {
        const size_t next = editor::detail::next_grapheme_offset(text, pos);
        const size_t width =
            editor::detail::display_width_for_range(text, pos, next);
        if (used + width > cells - 1) break;
        out.append(text, pos, next - pos);
        used += width;
        pos = next;
    }
    return out + u8"…";
}

std::string status_core(const std::string& provider_model_label,
                        const std::string& usage) {
    return app_version_label() + " " + provider_model_label + " " + usage;
}

std::string state_text(AgentActivityState state) {
    switch (state) {
        case AgentActivityState::Preparing:
            return "Agent preparing";
        case AgentActivityState::Thinking:
            return "Agent thinking";
        case AgentActivityState::Working:
            return "Agent working";
        case AgentActivityState::Compacting:
            return "Agent compacting";
        case AgentActivityState::Ready:
            return "Agent ready";
        case AgentActivityState::Unavailable:
            return "Agent unavailable";
    }
    return "Agent unavailable";
}

}  // namespace

bool valid_inline_choices(const InlineChoiceModel& model, std::string* reason) {
    auto fail = [&](const std::string& text) {
        if (reason != nullptr) *reason = text;
        return false;
    };
    if (model.choices.size() < 2 || model.choices.size() > 4) {
        return fail("inline choices require 2 through 4 choices");
    }
    if (model.escape_choice >= model.choices.size()) {
        return fail("inline choice escape default is out of range");
    }
    std::string seen;
    for (const InlineChoice& choice : model.choices) {
        if (choice.label.empty() || choice.mnemonic == '\0') {
            return fail("each inline choice requires a label and mnemonic");
        }
        const char mnemonic = lower_ascii(choice.mnemonic);
        bool found = false;
        for (char label_char : choice.label) {
            if (lower_ascii(label_char) == mnemonic) {
                found = true;
                break;
            }
        }
        if (!found) return fail("inline choice mnemonic must occur in its label");
        if (seen.find(mnemonic) != std::string::npos) {
            return fail("inline choice mnemonics must be unique");
        }
        seen.push_back(mnemonic);
    }
    if (reason != nullptr) reason->clear();
    return true;
}

std::string render_inline_choices(const InlineChoiceModel& model) {
    if (!valid_inline_choices(model)) return "";
    std::ostringstream out;
    for (size_t i = 0; i < model.choices.size(); ++i) {
        if (i != 0) out << "  ";
        const InlineChoice& choice = model.choices[i];
        out << "(" << (i + 1) << ") ";
        bool marked = false;
        for (char ch : choice.label) {
            if (!marked && lower_ascii(ch) == lower_ascii(choice.mnemonic)) {
                out << "[" << static_cast<char>(
                    std::toupper(static_cast<unsigned char>(ch))) << "]";
                marked = true;
            } else {
                out << ch;
            }
        }
    }
    return out.str();
}

InlineChoiceResult parse_inline_choice_key(const InlineChoiceModel& model, unsigned char key) {
    if (!valid_inline_choices(model)) return {};
    if (key == 27) return {true, model.escape_choice};
    if (key >= '1' && key <= '4') {
        const size_t index = static_cast<size_t>(key - '1');
        if (index < model.choices.size()) return {true, index};
    }
    const char lowered = lower_ascii(static_cast<char>(key));
    for (size_t i = 0; i < model.choices.size(); ++i) {
        if (lowered == lower_ascii(model.choices[i].mnemonic)) return {true, i};
    }
    return {};
}

InlineChoiceModel agent_inline_choices_for_mode(TuiMode mode) {
    switch (mode) {
        case TuiMode::GuardApprovalConfirm:
            return {{{"Yes", 'y'}, {"No", 'n'}}, 1};
        case TuiMode::AgentPermissionSelect:
            return {{{"Confirm", 'c'}, {"Smart", 's'}, {"Yolo", 'y'}}, 1};
        case TuiMode::AgentContinueConfirm:
            return {{{"Continue", 'c'}, {"Stop", 's'}}, 1};
        case TuiMode::AgentNewConfirm:
            return {{{"Reset", 'r'}, {"Cancel", 'c'}}, 1};
        case TuiMode::ReasoningConfirm:
            return {{{"Proceed", 'p'}, {"Cancel", 'c'}}, 1};
        case TuiMode::ModelConfirm:
            return {{{"Keep current", 'k'}, {"Use thread", 'u'}}, 1};
        case TuiMode::RemoveConfirm:
            return {{{"Approve", 'a'}, {"Cancel", 'c'}}, 1};
        default:
            return {};
    }
}

AgentInputGeometry agent_input_geometry(int terminal_rows,
                                        int terminal_cols,
                                        size_t measured_visual_rows,
                                        int max_height_percent) {
    const int rows = std::max(6, terminal_rows);
    const int cols = std::max(4, terminal_cols);
    const int percent = std::min(80, std::max(10, max_height_percent));
    const int percentage_cap = std::max(3, (rows * percent) / 100);
    // History + activity + status consume three rows. Tiny terminals retain
    // those rows even
    // when doing so must override the configured percentage.
    const int screen_cap = std::max(3, rows - 3);
    const int cap = std::min(screen_cap, percentage_cap);
    const int desired = static_cast<int>(
        std::min<size_t>(measured_visual_rows == 0 ? 1 : measured_visual_rows,
                         static_cast<size_t>(std::max(1, cap - 2)))) +
                        2;
    AgentInputGeometry geometry;
    geometry.box_height = std::max(3, std::min(cap, desired));
    geometry.content_rect = {rows - geometry.box_height + 1,
                             2,
                             geometry.box_height - 2,
                             std::max(1, cols - 2)};
    return geometry;
}

std::string abbreviate_agent_workspace(const std::string& workspace) {
    if (workspace.empty()) return ".";
    std::string result = workspace;
    const char* home_value = std::getenv("HOME");
    if (home_value != nullptr) {
        const std::string home(home_value);
        if (result == home) return "~";
        if (!home.empty() && result.size() > home.size() &&
            result.compare(0, home.size(), home) == 0 && result[home.size()] == '/') {
            result.replace(0, home.size(), "~");
        }
    }
    return result;
}

std::string agent_input_title(const AgentInputFrame& frame, int available_cells) {
    if (available_cells <= 0) return "";
    const std::string mode = frame.mode_label.empty() ? "act" : frame.mode_label;
    std::string path = abbreviate_agent_workspace(frame.workspace);
    const std::string suffix = " " + mode;
    if (static_cast<int>(text_cells(path + suffix)) <= available_cells) return path + suffix;
    const std::string leaf = fs::path(path).filename().string();
    const int path_cells = std::max(1, available_cells - static_cast<int>(suffix.size()));
    if (static_cast<int>(text_cells(leaf)) + 2 <= path_cells) {
        path = u8"…/" + leaf;
    } else {
        path = shorten_end(leaf.empty() ? path : leaf, static_cast<size_t>(path_cells));
    }
    return shorten_end(path + suffix, static_cast<size_t>(available_cells));
}

std::string agent_input_top_border(const AgentInputFrame& frame, int cols) {
    cols = std::max(2, cols);
    if (cols == 2) return u8"┌┐";
    if (cols == 3) return u8"┌─┐";
    const std::string permission =
        frame.permission_label.empty() ? "smart" : frame.permission_label;
    const int right_budget = std::max(0, cols - 6);
    std::string right_label;
    const int permission_cells = static_cast<int>(text_cells(permission));
    if (permission_cells >= right_budget) {
        right_label = shorten_end(permission, static_cast<std::size_t>(right_budget));
    } else if (frame.credit_label.empty()) {
        right_label = permission;
    } else {
        const int credit_budget = right_budget - permission_cells - 1;
        right_label = permission;
        if (credit_budget > 0)
            right_label +=
                " " + shorten_end(frame.credit_label,
                                  static_cast<std::size_t>(credit_budget));
    }
    const int right_label_cells = static_cast<int>(text_cells(right_label));
    // Corners (2), leading/trailing dashes (2), spaces around both labels (4).
    const int title_budget = std::max(0, cols - right_label_cells - 8);
    const std::string title = agent_input_title(frame, title_budget);
    std::string line = u8"┌─";
    if (!title.empty()) line += " " + title + " ";
    const int used_without_fill =
        2 + (title.empty() ? 0 : 2 + static_cast<int>(text_cells(title))) +
        right_label_cells + 4;
    for (int cell = used_without_fill; cell < cols; ++cell) line += u8"─";
    if (!right_label.empty()) line += " " + right_label + " ";
    line += u8"─";
    line += u8"┐";
    // Very narrow terminals may not fit both labels. Preserve permission first.
    while (static_cast<int>(text_cells(line)) > cols && !title.empty()) {
        AgentInputFrame narrower = frame;
        const int smaller = std::max(0, title_budget - 1);
        const std::string clipped = agent_input_title(narrower, smaller);
        std::string compact = u8"┌─";
        if (!clipped.empty()) compact += " " + clipped + " ";
        int fill = cols - (2 + (clipped.empty() ? 0 : 2 + static_cast<int>(text_cells(clipped))) +
                           right_label_cells + 4);
        while (fill-- > 0) compact += u8"─";
        compact += " " + right_label + " ─┐";
        line = std::move(compact);
        break;
    }
    return line;
}

std::string agent_input_bottom_border(int cols) {
    cols = std::max(2, cols);
    std::string line = u8"└";
    for (int cell = 0; cell < cols - 2; ++cell) line += u8"─";
    line += u8"┘";
    return line;
}

std::string agent_status_line(const std::string& model_name,
                              const std::string& reasoning,
                              long long used_tokens,
                              long long window_tokens,
                              int cols) {
    cols = std::max(1, cols);
    const std::string reason = reasoning.empty() ? "auto" : reasoning;
    const std::string label =
        ui::provider_model_display_label("", model_name, reason);
    std::string line =
        status_core(label, format_agent_context_usage(used_tokens, window_tokens));
    if (static_cast<int>(text_cells(line)) <= cols) return line;
    return shorten_end(line, static_cast<size_t>(cols));
}

std::string agent_activity_line(AgentActivityState state,
                                bool cancellable,
                                long long elapsed_seconds,
                                long long completed_task_ms,
                                int cols) {
    cols = std::max(1, cols);
    std::ostringstream out;
    if (state == AgentActivityState::Ready) {
        out << state_text(state) << ". ";
        if (completed_task_ms >= 0) {
            out << "Task completed in " << std::fixed << std::setprecision(2)
                << static_cast<double>(completed_task_ms) / 1000.0 << " seconds.";
        } else {
            out << "/help /quit";
        }
    } else if (state == AgentActivityState::Unavailable) {
        out << state_text(state)
            << ". Check the notice above or switch provider/model.";
    } else {
        elapsed_seconds = std::max(0LL, elapsed_seconds);
        out << state_text(state);
        if (state == AgentActivityState::Compacting)
            out << std::string(
                static_cast<std::size_t>(elapsed_seconds % 3 + 1), '.');
        if (cancellable) out << " (ESC to abort)";
        out << " " << elapsed_seconds / 60 << ":" << std::setw(2)
            << std::setfill('0') << elapsed_seconds % 60;
    }
    return shorten_end(out.str(), static_cast<size_t>(cols));
}

}  // namespace ainiux::tui
