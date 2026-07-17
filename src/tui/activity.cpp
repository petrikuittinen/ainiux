#include "tui/activity.hpp"

#include "ainiux/version.hpp"
#include "provider/provider.hpp"

#include <algorithm>


namespace ainiux::tui {
namespace {

constexpr const char* kThinkingFrames[] = {
    "\xe2\x97\x90",  // ◐
    "\xe2\x97\x93",  // ◓
    "\xe2\x97\x91",  // ◑
    "\xe2\x97\x92",  // ◒
};

constexpr const char* kStreamingFrames[] = {
    "\xe2\x96\xb9",  // ▹
    "\xe2\x96\xb8",  // ▸
    "\xe2\x96\xba",  // ►
};

constexpr size_t kThinkingFrameCount = sizeof(kThinkingFrames) / sizeof(kThinkingFrames[0]);
constexpr size_t kStreamingFrameCount = sizeof(kStreamingFrames) / sizeof(kStreamingFrames[0]);
constexpr size_t kMaxActivityIndicatorWidth = 4;

std::string rotated_activity_text(const char* const* symbols, size_t symbol_count, size_t frame) {
    if (symbol_count == 0) {
        return "";
    }
    const size_t width = std::min(symbol_count, kMaxActivityIndicatorWidth);
    const size_t start = frame % symbol_count;
    std::string out;
    out.reserve(width * 3);
    for (size_t i = 0; i < width; ++i) {
        out += symbols[(start + i) % symbol_count];
    }
    return out;
}

void append_segment(std::vector<StyledSegment>& segments, std::string text, StyleRole role) {
    if (text.empty()) {
        return;
    }
    segments.push_back({std::move(text), role});
}

constexpr const char kInputLabelStatusMessage[] =
    " | /help | history Ctrl+B ↑ Ctrl+D ↓";

}  // namespace

const char* input_label_status_message() {
    return kInputLabelStatusMessage;
}

std::string input_label_text() {
    return app_version_label() + input_label_status_message();
}

std::vector<StyledSegment> input_label_segments() {
    const std::string label = input_label_text();
    const std::string& version = app_version_label();
    return {
        {version, StyleRole::PanelTitle},
        {label.substr(version.size()), StyleRole::InputLabel},
    };
}

std::string activity_indicator_text(ActivityKind kind, size_t frame) {
    switch (kind) {
        case ActivityKind::Thinking:
            return rotated_activity_text(kThinkingFrames, kThinkingFrameCount, frame);
        case ActivityKind::Streaming:
            return rotated_activity_text(kStreamingFrames, kStreamingFrameCount, frame);
        case ActivityKind::None:
            break;
    }
    return "";
}

size_t activity_indicator_width(ActivityKind kind) {
    switch (kind) {
        case ActivityKind::Thinking:
            return std::min(kThinkingFrameCount, kMaxActivityIndicatorWidth);
        case ActivityKind::Streaming:
            return std::min(kStreamingFrameCount, kMaxActivityIndicatorWidth);
        case ActivityKind::None:
            break;
    }
    return 0;
}

StyleRole activity_indicator_role(ActivityKind kind) {
    switch (kind) {
        case ActivityKind::Thinking:
            return StyleRole::ThinkingActivity;
        case ActivityKind::Streaming:
            return StyleRole::StreamingActivity;
        case ActivityKind::None:
            break;
    }
    return StyleRole::Muted;
}

std::vector<StyledSegment> activity_status_segments(const std::string& label,
                                                            ActivityKind kind,
                                                            size_t frame,
                                                            const std::string& suffix) {
    std::vector<StyledSegment> segments;
    if (!label.empty()) {
        append_segment(segments, label, StyleRole::Status);
        append_segment(segments, " ", StyleRole::Status);
    }
    append_segment(segments, activity_indicator_text(kind, frame), activity_indicator_role(kind));
    append_segment(segments, " ", StyleRole::Status);
    append_segment(segments, suffix, StyleRole::Status);
    return segments;
}

std::vector<StyledSegment> activity_placeholder_segments(const std::string& label,
                                                                 ActivityKind kind,
                                                                 size_t frame,
                                                                 const std::string& suffix) {
    std::vector<StyledSegment> segments;
    if (!label.empty()) {
        append_segment(segments, label, StyleRole::Muted);
        append_segment(segments, " ", StyleRole::Muted);
    }
    append_segment(segments, activity_indicator_text(kind, frame), activity_indicator_role(kind));
    append_segment(segments, " ", StyleRole::Muted);
    append_segment(segments, suffix, StyleRole::Muted);
    return segments;
}

std::string session_status_label(const chat::Session& session) {
    const std::string display_provider =
        session.provider.empty() ? "" : provider::display_name_for_profile(session.provider);
    if (display_provider.empty() && session.model.empty()) {
        return "";
    }
    if (display_provider.empty()) {
        return "[" + session.model + "]";
    }
    if (session.model.empty()) {
        return "[" + display_provider + " / model unknown]";
    }
    return "[" + display_provider + " / " + session.model + "]";
}

ActivityKind activity_kind_for_pending_assistant(const chat::Session& session,
                                                 size_t pending_assistant_index,
                                                 bool show_thinking_traces) {
    if (pending_assistant_index == static_cast<size_t>(-1) ||
        pending_assistant_index >= session.messages.size()) {
        return ActivityKind::None;
    }
    const provider::Message& message = session.messages[pending_assistant_index];
    if (message.role != "assistant") {
        return ActivityKind::None;
    }
    const ThinkingDisplay display = thinking_display_text(message.content, show_thinking_traces);
    if (!show_thinking_traces && display.saw_thinking_tag && display.text.empty()) {
        return ActivityKind::Thinking;
    }
    if (!message.content.empty()) {
        return ActivityKind::Streaming;
    }
    return ActivityKind::Streaming;
}

}  // namespace ainiux::tui
