#pragma once

#include <optional>
#include <string>
#include <vector>

#include "common.hpp"
#include "editor/editor.hpp"
#include "editor/editor_prompts.hpp"
#include "markdown/markdown.hpp"
#include "ainiux/model_setting.hpp"
#include "tui/theme_registry.hpp"

namespace ainiux::cli {

enum class OutputFormat { Text, Json, Ndjson };

struct BenchmarkGradingPrompts {
    std::string system_prompt;
    std::string case_prompt;
};

struct Options {
    bool help = false;
    bool version = false;
    bool list_models = false;
    bool stream = true;
    bool stream_explicit = false;
    bool quiet = false;
    bool verbose = false;
    bool debug = false;
    bool no_config = false;
    bool trace_http = false;
    bool insecure_tls = false;
    bool key_stdin = false;
    bool repl = false;
    bool tui = false;
    bool editor = false;
    std::string editor_path;
    bool no_colors = false;
    bool tui_highlight = true;
    bool allow_private_url_fetch = false;
    bool auto_convert_html_to_markdown = true;
    bool show_thinking_traces = false;
    bool benchmark = false;
    bool grade = false;
    bool benchmark_validate = false;
    bool benchmark_list = false;
    bool benchmark_options_seen = false;
    bool benchmark_dataset_explicit = false;
    bool benchmark_mode_explicit = false;
    bool benchmark_runs_explicit = false;
    bool benchmark_warmup_explicit = false;
    bool benchmark_duration_explicit = false;
    bool grade_input_explicit = false;

    std::string positional_url;
    std::string prompt;
    std::string prompt_file;
    std::string system;
    std::string system_file;
    std::string model;
    std::string provider = "openai";
    bool provider_explicit = false;
    std::string profile;
    std::string api = "chat";
    std::string base_url;
    std::string chat_url;
    std::string models_url;
    std::string responses_url;
    std::string key_env;
    std::string key_file;
    std::string key;
    std::string output_path;
    std::string fetch_url;
    std::string search_query;
    std::string web_search_provider = "auto";
    std::string tavily_key_env;
    std::string firecrawl_key_env;
    std::string exa_key_env;
    std::string exa_base_url;
    std::string searxng_base_url;
    std::string input_path;
    std::string html_file;
    std::string html_format = "markdown";
    ainiux::markdown::OutputFormat output_format = ainiux::markdown::OutputFormat::Markdown;
    bool output_format_explicit = false;
    bool rendered_output_format_explicit = false;
    std::string save_chat_path;
    std::string load_chat_path;
    std::string proxy;
    std::string context_policy = "error";
    std::string image_capability = "auto";
    std::string tui_theme = "dark";
    tui::ThemeRegistry tui_themes = tui::default_theme_registry();
    std::string benchmark_dataset = "builtin";
    std::string grade_input;
    std::string benchmark_category;
    std::string benchmark_case;
    std::string benchmark_mode = "quality";
    std::string benchmark_summary_format = "table";
    OutputFormat format = OutputFormat::Text;
    BenchmarkGradingPrompts benchmark_grading_prompts;

    double temperature = 0.0;
    bool has_temperature = false;
    double top_p = 0.0;
    bool has_top_p = false;
    int top_k = 0;
    bool has_top_k = false;
    double min_p = 0.0;
    bool has_min_p = false;
    double repeat_penalty = 1.0;
    bool has_repeat_penalty = false;
    double presence_penalty = 0.0;
    bool has_presence_penalty = false;
    bool enable_thinking = false;
    bool has_enable_thinking = false;
    std::string thinking_budget;
    bool has_thinking_budget = false;
    std::string chat_purpose;
    bool has_chat_purpose = false;
    bool has_context_tokens = false;
    bool has_show_thinking_traces = false;
    int max_output_tokens = 0;
    bool has_max_output_tokens = false;
    long connect_timeout_seconds = 10;
    long timeout_seconds = 0;
    long max_fetch_bytes = 1048576;
    int max_web_search_results = 3;
    bool max_web_search_results_explicit = false;
    long max_input_bytes = 1048576;
    long max_image_bytes = 20971520;
    long max_context_bytes = 0;
    int editor_undo_limit = static_cast<int>(ainiux::editor::kDefaultUndoLimit);
    long long editor_huge_file_size_warning = ainiux::editor::kDefaultHugeFileSizeWarningBytes;
    long long editor_file_size_limit = ainiux::editor::kNoEditorFileSizeLimit;
    bool editor_auto_save_mode = true;
    std::string editor_auto_save_postfix = ainiux::editor::kDefaultAutoSavePostfix;
    size_t editor_auto_save_threshold = ainiux::editor::kDefaultAutoSaveThreshold;
    int editor_auto_save_timeout_seconds = ainiux::editor::kDefaultAutoSaveTimeoutSeconds;
    long long editor_auto_save_size_limit = ainiux::editor::kDefaultAutoSaveSizeLimit;
    size_t editor_tab_width = ainiux::editor::kDefaultTabWidth;
    ainiux::editor::TabStyle editor_tab_style = ainiux::editor::TabStyle::Spaces;
    ainiux::editor::LineBreak editor_linebreak = ainiux::editor::LineBreak::Lf;
    size_t editor_ai_continue_prefix_max_chars =
        ainiux::editor::kDefaultAiContinuePrefixMaxChars;
    size_t editor_ai_continue_postfix_max_chars =
        ainiux::editor::kDefaultAiContinuePostfixMaxChars;
    size_t editor_ai_continue_prose_prefix_max_chars =
        ainiux::editor::kDefaultAiContinueProsePrefixMaxChars;
    size_t editor_ai_continue_prose_postfix_max_chars =
        ainiux::editor::kDefaultAiContinueProsePostfixMaxChars;
    int editor_ai_continue_max_tokens = ainiux::editor::kDefaultAiContinueMaxTokens;
    ainiux::editor::EditorAssistConfig editor_assist_config = ainiux::editor::empty_editor_assist_config();
    long long context_tokens = 0;
    int benchmark_runs = 1;
    int benchmark_warmup = 0;
    int benchmark_limit = 0;
    int benchmark_concurrency = 1;
    long long benchmark_duration_ms = 60000;

    std::vector<std::string> headers;
    std::vector<std::string> attachment_paths;
    std::vector<ModelSetting> model_settings;
};

struct ParseResult {
    Options options;
    Error error;
};

ParseResult parse_args(int argc, char** argv);
ParseResult parse_args(int argc, char** argv, const Options& base_options);
std::string help_text();
const char* format_name(OutputFormat format);

}  // namespace ainiux::cli
