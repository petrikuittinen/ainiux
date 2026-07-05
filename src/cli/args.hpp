#pragma once

#include <optional>
#include <string>
#include <vector>

#include "common.hpp"
#include "editor/editor.hpp"
#include "editor/editor_prompts.hpp"
#include "markdown/markdown.hpp"
#include "pkchat/model_setting.hpp"

namespace pkchat::cli {

enum class OutputFormat { Text, Json, Ndjson };

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
    bool allow_private_url_fetch = false;
    bool show_thinking_traces = false;
    bool benchmark = false;
    bool benchmark_validate = false;
    bool benchmark_list = false;
    bool benchmark_options_seen = false;

    std::string positional_url;
    std::string prompt;
    std::string prompt_file;
    std::string system;
    std::string system_file;
    std::string model;
    std::string provider = "openai";
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
    pkchat::markdown::OutputFormat output_format = pkchat::markdown::OutputFormat::Markdown;
    bool output_format_explicit = false;
    bool rendered_output_format_explicit = false;
    std::string save_chat_path;
    std::string load_chat_path;
    std::string proxy;
    std::string context_policy = "error";
    std::string image_capability = "auto";
    std::string tui_theme = "dark";
    std::string benchmark_dataset = "builtin";
    std::string benchmark_category;
    std::string benchmark_case;
    std::string benchmark_mode = "quality";
    std::string benchmark_summary_format = "table";
    OutputFormat format = OutputFormat::Text;

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
    int editor_undo_limit = static_cast<int>(pkchat::editor::kDefaultUndoLimit);
    long long editor_huge_file_size_warning = pkchat::editor::kDefaultHugeFileSizeWarningBytes;
    long long editor_file_size_limit = pkchat::editor::kNoEditorFileSizeLimit;
    pkchat::editor::EditorAssistConfig editor_assist_config = pkchat::editor::default_editor_assist_config();
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

}  // namespace pkchat::cli
