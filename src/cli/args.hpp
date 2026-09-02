#pragma once

#include <optional>
#include <string>
#include <vector>

#include "common.hpp"
#include "editor/editor.hpp"
#include "editor/editor_prompts.hpp"
#include "markdown/markdown.hpp"
#include "ainiux/image_setting.hpp"
#include "ainiux/model_setting.hpp"
#include "ainiux/compaction_strategy.hpp"
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
    bool list_mcp = false;
    bool add_mcp = false;
    bool remove_mcp = false;
    bool enable_mcp = false;
    bool disable_mcp = false;
    std::string mcp_name;
    std::string mcp_transport;
    std::string mcp_url;
    std::vector<std::string> mcp_headers;
    std::vector<std::string> mcp_env;
    std::vector<std::string> mcp_command_args;
    bool mcp_allow_private = false;
    std::string mcp_protocol_hint;
    std::string mcp_registry_path;
    bool stream = true;
    bool stream_explicit = false;
    bool stream_cli_explicit = false;
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
    // Start the editor in dired (implies editor). Optional path is the directory/glob;
    // empty means the current working directory.
    bool dired = false;
    std::string dired_path;
    bool no_colors = false;
    // Preferred color wire format; resolved with COLORTERM/TERM at TUI/editor start.
    tui::ColorModePreference color_mode = tui::ColorModePreference::Auto;
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
    bool index_code = false;
    bool print_index = false;
    bool clear_index = false;
    bool security_review = false;
    bool security_review_log_enabled = true;
    bool security_review_log_cli_explicit = false;
    // Interactive agent TUI (chat-like UI + agent tool loop): -a / --agent / ainiux agent
    bool agent = false;
    // Internal startup state: an existing project supplied its provider/settings.
    bool agent_project_settings_restored = false;
    // One-shot headless agent goal: -r / --run / --run-file / ainiux run
    bool agent_run = false;
    // One-shot agent task uses the planning prompt and planning-document policy.
    bool agent_plan = false;
    bool agent_log_enabled = true;
    bool agent_log_cli_explicit = false;
    // Session-scoped strict A/B control. Never persisted to configuration.
    bool disable_indexing = false;
    // One-shot image generation: ainiux image / --image
    bool image = false;
    // v1.3 control API server. Loopback remains the safe default.
    bool server = false;
    bool server_options_seen = false;
    bool insecure_plain_bind = false;
    bool allow_remote_yolo = false;
    bool image_force = false;
    bool image_format_explicit = false;
    bool format_cli_explicit = false;
    std::string image_size;
    std::string image_ar;
    std::string image_quality;
    std::string image_format = "png";
    std::string workspace = ".";
    std::string bind_address = "127.0.0.1";
    std::string server_secret_file;
    std::string mcp_secret_file;
    std::string tls_cert_file;
    std::string tls_key_file;
    int port = 8766;
    int max_connections = 64;
    int max_jobs = 128;
    int max_sessions = 32;

    std::string positional_url;
    std::string prompt;
    std::string prompt_file;
    std::string system;
    std::string system_file;
    std::string model;
    std::string provider = "openai";
    bool provider_explicit = false;
    bool model_explicit = false;
    bool api_explicit = false;
    std::string profile;
    std::string api = "chat";
    std::string base_url;
    bool base_url_cli_explicit = false;
    std::string chat_url;
    bool chat_url_cli_explicit = false;
    std::string models_url;
    bool models_url_cli_explicit = false;
    std::string responses_url;
    bool responses_url_cli_explicit = false;
    std::string key_env;
    std::string key_file;
    std::string key;
    std::string output_path;
    std::string fetch_url;
    std::string search_query;
    std::string web_search_provider = "auto";
    bool builtin_web_search = true;
    std::string tavily_key_env;
    std::string firecrawl_key_env;
    std::string exa_key_env;
    std::string exa_base_url;
    std::string searxng_base_url;
    std::string input_path;
    std::string input_encoding;
    std::string html_file;
    std::string html_format = "markdown";
    ainiux::markdown::OutputFormat output_format = ainiux::markdown::OutputFormat::Markdown;
    bool output_format_explicit = false;
    bool rendered_output_format_explicit = false;
    std::string save_chat_path;
    std::string load_chat_path;
    std::string trusted_prompt_dir;
    std::string proxy;
    std::string context_policy = "error";
    std::string image_capability = "auto";
    std::string tui_theme = "dark";
    tui::ThemeRegistry tui_themes = tui::default_theme_registry();
    int agent_input_max_height_percent = 25;
    int agent_thinking_preview_max_chars = 120;
    bool has_agent_thinking_preview_max_chars = false;
    // Seconds of pure provider reasoning before the opening thinking row
    // freezes if it has not already filled the preview budget. 0 freezes the
    // opening clip as soon as it is complete (or at phase end). Maximum 3600.
    // Default 30. At most two rows: head (first ~max_chars) and a live
    // tail (last ~max_chars) that freezes when reasoning ends.
    int agent_thinking_idle_preview_seconds = 30;
    // How often interactive agent chrome refreshes the in-flight generation
    // token estimate while pure reasoning streams. Default 1 second; 0 disables
    // mid-stream updates (request estimate only); maximum 3600.
    int agent_thinking_token_refresh_seconds = 1;
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
    bool temperature_cli_explicit = false;
    // Distinguishes advisory catalog presets from an explicit CLI/config/UI value.
    bool temperature_preset_applied = false;
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
    ReasoningSelection reasoning;
    // True when a configuration or CLI source explicitly selected reasoning.
    // The Auto value itself remains the canonical cleared override.
    bool reasoning_explicit = false;
    bool reasoning_cli_explicit = false;
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
    size_t max_source_code_file_size = 10U * 1024U * 1024U;
    int max_parallel_agents = 4;
    int agent_max_turns = 250;
    size_t security_review_batch_size = 200U * 1024U;
    int security_review_log_keep_runs = 3;
    // Agent project policy (interactive + --run).
    bool agent_history_backup_enabled = true;
    size_t agent_history_backup_max_bytes = 1024U * 1024U;  // 1M
    int agent_history_backup_ttl_days = 7;
    bool agent_auto_compact = true;
    CompactionStrategy agent_compact_strategy = CompactionStrategy::Smart;
    // 0 = derive from context window (75%).
    int agent_compact_limit = 0;
    bool agent_show_command_output = false;
    // Hard cap on the full agent LLM HTTP body (including SSE framing). 0 = unlimited.
    long agent_max_response_bytes = 32L * 1024L * 1024L;  // 32 MiB
    long max_image_bytes = 20971520;
    long media_max_size_to_store_to_db = 65536;
    int media_expiration_days = 7;
    int media_auto_expiration_days = 30;
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
    size_t editor_text_align_width = ainiux::editor::kDefaultTextAlignWidth;
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
    ModelCatalog model_catalog;
    ImageCatalog image_catalog;
};

// Parse a context-window token count accepted by both --context and interactive
// /context commands (plain integer, binary k suffix, or decimal M suffix).
Error parse_context_tokens(const std::string& text, long long& out);

struct ParseResult {
    Options options;
    Error error;
};

ParseResult parse_args(int argc, char** argv);
ParseResult parse_args(int argc, char** argv, const Options& base_options);
Error validate_index_mode_arguments(int argc, char** argv, const Options& options);
Error validate_security_review_arguments(int argc, char** argv, const Options& options);
// One-shot headless agent: --run / -r / --run-file / ainiux run
Error validate_agent_run_arguments(int argc, char** argv, const Options& options);
// Interactive agent TUI: --agent / -a / ainiux agent
Error validate_agent_interactive_arguments(int argc, char** argv, const Options& options);
Error validate_disable_indexing_arguments(const Options& options);
Error validate_image_mode_arguments(const Options& options);
std::string help_text();
const char* format_name(OutputFormat format);

}  // namespace ainiux::cli
