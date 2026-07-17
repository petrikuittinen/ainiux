#pragma once

#include "common.hpp"
#include "editor/editor.hpp"
#include "highlight/highlight.hpp"
#include "runtime/runtime.hpp"

#include <cstddef>
#include <string>

namespace ainiux::editor {

struct ReformatRequest {
    std::string content;
    highlight::Language language = highlight::Language::Text;
    size_t tab_width = kDefaultTabWidth;
    TabStyle tab_style = TabStyle::Spaces;
    size_t first_line = 0;
    size_t last_line = 0;
};

struct ReformatResult {
    Error error;
    size_t replace_start = 0;
    size_t replace_count = 0;
    size_t first_line = 0;
    size_t last_line = 0;
    std::string replacement;
    bool changed = false;
    std::string warning;
};

struct ReformatEvent {
    ReformatResult result;
};

struct ReformatSession {
    bool active = false;
    bool all = false;
    bool cancel_requested = false;
    std::uint64_t buffer_id = 0;
    std::uint64_t revision = 0;
    highlight::Language language = highlight::Language::Text;
    size_t tab_width = kDefaultTabWidth;
    TabStyle tab_style = TabStyle::Spaces;
    runtime::EventQueue<ReformatEvent> events;
    // Declared after the queue so its destructor cancels and joins the worker
    // before the referenced event queue is destroyed.
    runtime::JobHandle job;
};

ReformatResult reformat_indentation(const ReformatRequest& request,
                                    const runtime::CancellationToken& cancellation = {});

Error build_reformat_request(const EditorState& state, bool all, ReformatRequest& request);
Error apply_reformat_result(EditorState& state, const ReformatResult& result, bool all);
void start_reformat_job(ReformatRequest request,
                        runtime::EventQueue<ReformatEvent>& events,
                        runtime::JobHandle& job);

}  // namespace ainiux::editor
