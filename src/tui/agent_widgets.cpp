#include "tui/agent_widgets.hpp"

#include "ainiux/version.hpp"
#include "editor/detail/wrap.hpp"
#include "editor/detail/unicode.hpp"
#include "provider/provider.hpp"
#include "tui/tui.hpp"
#include "ui/provider_model_display.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <sstream>

namespace ainiux::tui {
namespace {

namespace fs = std::filesystem;

char lower_ascii(char ch) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
}

std::string percent_only(long long used_tokens, long long window_tokens) {
    const std::string full = format_agent_context_usage(used_tokens, window_tokens);
    const size_t open = full.find('(');
    return open == std::string::npos ? full : full.substr(open + 1, full.size() - open - 2);
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

std::string status_core(const std::string& provider,
                        const std::string& model,
                        const std::string& reasoning,
                        const std::string& usage) {
    return app_version_label() + " [" + provider + "/" + model + " " + reasoning + "] " + usage;
}

std::string suffix_for(bool cancellable, const std::string& transient) {
    std::string suffix;
    if (cancellable) suffix = " · ESC to abort";
    if (!transient.empty()) suffix += " · " + transient;
    return suffix;
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
            return {{{"Approve", 'a'}, {"Cancel", 'c'}}, 1};
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
    const int rows = std::max(5, terminal_rows);
    const int cols = std::max(4, terminal_cols);
    const int percent = std::min(80, std::max(10, max_height_percent));
    const int percentage_cap = std::max(3, (rows * percent) / 100);
    // history + status consume two rows. Tiny terminals retain those rows even
    // when doing so must override the configured percentage.
    const int screen_cap = std::max(3, rows - 2);
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
    const std::string title = agent_input_title(frame, std::max(0, cols - 4));
    std::string line = u8"┌─";
    if (!title.empty()) line += title + " ";
    const int occupied =
        4 + static_cast<int>(editor::detail::display_width_for_range(
                title, 0, title.size()));
    for (int cell = occupied; cell < cols; ++cell) line += u8"─";
    line += u8"┐";
    return line;
}

std::string agent_input_bottom_border(int cols) {
    cols = std::max(2, cols);
    std::string line = u8"└";
    for (int cell = 0; cell < cols - 2; ++cell) line += u8"─";
    line += u8"┘";
    return line;
}

std::string agent_status_bar(const std::string& provider_name,
                             const std::string& model_name,
                             const std::string& reasoning,
                             long long used_tokens,
                             long long window_tokens,
                             int cols,
                             bool cancellable,
                             const std::string& transient_status) {
    cols = std::max(1, cols);
    const std::string provider = provider::display_name_for_profile(provider_name.empty()
                                                                        ? "no-provider"
                                                                        : provider_name);
    std::string model = model_name.empty() ? "no-model"
                                           : ui::compact_model_name_for_display(model_name);
    const std::string reason = reasoning.empty() ? "auto" : reasoning;
    std::string usage = format_agent_context_usage(used_tokens, window_tokens);
    std::string transient_suffix = suffix_for(cancellable, transient_status);
    std::string line = status_core(provider, model, reason, usage) + transient_suffix;
    if (static_cast<int>(text_cells(line)) <= cols) return line;

    // Transient text is opportunistic. The abort hint is never sacrificed.
    transient_suffix = suffix_for(cancellable, "");
    line = status_core(provider, model, reason, usage) + transient_suffix;
    const size_t fixed =
        text_cells(status_core(provider, "", reason, usage) + transient_suffix);
    const size_t full_model_room =
        fixed < static_cast<size_t>(cols) ? static_cast<size_t>(cols) - fixed : 0;
    const size_t minimum_useful_model_cells = std::min<size_t>(8, text_cells(model));
    if (full_model_room >= minimum_useful_model_cells) {
        model = shorten_end(model, full_model_room);
        line = status_core(provider, model, reason, usage) + transient_suffix;
        if (static_cast<int>(text_cells(line)) <= cols) return line;
    }

    usage = percent_only(used_tokens, window_tokens);
    const size_t compact_fixed =
        text_cells(status_core(provider, "", reason, usage) + transient_suffix);
    if (compact_fixed < static_cast<size_t>(cols)) {
        model = shorten_end(
            model, static_cast<size_t>(cols) - compact_fixed);
    } else {
        model.clear();
    }
    line = status_core(provider, model, reason, usage) + transient_suffix;
    if (static_cast<int>(text_cells(line)) <= cols) return line;

    // Extremely narrow terminals still keep both the leading version and the
    // cancellation affordance: clip only the core portion before the suffix.
    const size_t suffix_cells = text_cells(transient_suffix);
    if (suffix_cells >= static_cast<size_t>(cols)) {
        return shorten_end(transient_suffix, static_cast<size_t>(cols));
    }
    const std::string core = status_core(provider, model, reason, usage);
    return shorten_end(core, static_cast<size_t>(cols) - suffix_cells) +
           transient_suffix;
}

}  // namespace ainiux::tui
