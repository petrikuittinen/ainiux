#include "cli/args.hpp"

#include <cstdlib>
#include <limits>
#include <sstream>

#include "chat/settings.hpp"
#include "config/model_catalog.hpp"
#include "cli/option_values.hpp"
#include "context/policy.hpp"
#include "chat/generation_settings.hpp"
#include "editor/autosave.hpp"
#include "ainiux/version.hpp"

namespace ainiux::cli {

namespace {

bool needs_value(const std::string& opt) {
    static const char* with_values[] = {
        "-p", "--prompt", "--prompt-file", "-s", "--system", "--system-file", "-m", "--model", "-model",
        "-t", "--temperature", "--top-p", "--top-k", "--min-p", "--repeat-penalty", "--presence-penalty",
        "--reasoning", "--purpose", "--max-output-tokens", "--format", "--output-format",
        "--output",
        "--provider", "--profile", "--api", "--base-url", "--chat-url", "--models-url", "--responses-url",
        "--key-env", "--key-file", "-k", "--key", "--header", "--connect-timeout", "--timeout",
        "--proxy", "--fetch-url", "--search", "--web-search-provider", "--input", "--attach",
        "--html-file", "--html-format", "--max-fetch-bytes", "--max-web-search-results",
        "--max-input-bytes", "--max-image-bytes", "--max-context-bytes",
        "--max-source-code-file-size", "--trusted-prompt-dir",
        "--context", "--context-policy", "--image-capability",
        "--save-chat", "--load-chat", "--dataset", "--grade-input", "--category", "--case",
        "--runs", "--warmup", "--limit", "--mode", "--concurrency", "--duration",
        "--summary-format",
        "-r", "--run", "--run-file", "--plan", "--plan-file"};
    for (const char* item : with_values) {
        if (opt == item) {
            return true;
        }
    }
    return false;
}

Error parse_double(const std::string& name, const std::string& text, double& out) {
    char* end = nullptr;
    out = std::strtod(text.c_str(), &end);
    if (end == text.c_str() || *end != '\0') {
        return {ErrorCode::BadArgs, name + " expects a floating point number"};
    }
    return ok_error();
}

Error parse_long(const std::string& name, const std::string& text, long& out) {
    char* end = nullptr;
    out = std::strtol(text.c_str(), &end, 10);
    if (end == text.c_str() || *end != '\0' || out < 0) {
        return {ErrorCode::BadArgs, name + " expects a non-negative integer"};
    }
    return ok_error();
}

Error parse_int(const std::string& name, const std::string& text, int& out) {
    long value = 0;
    Error err = parse_long(name, text, value);
    if (!err.ok()) {
        return err;
    }
    if (value > std::numeric_limits<int>::max()) {
        return {ErrorCode::BadArgs, name + " value is too large"};
    }
    out = static_cast<int>(value);
    return ok_error();
}

Error parse_context_tokens_impl(const std::string& text, long long& out) {
    if (text.empty()) {
        return {ErrorCode::BadArgs,
                "--context expects a positive token count such as 65536, 64k, or 1M"};
    }
    size_t digits = text.size();
    long long multiplier = 1;
    const char suffix = text.back();
    if (suffix == 'k' || suffix == 'K') {
        multiplier = 1024;
        --digits;
    } else if (suffix == 'm' || suffix == 'M') {
        multiplier = 1000000;
        --digits;
    }
    if (digits == 0) {
        return {ErrorCode::BadArgs,
                "--context expects a positive token count such as 65536, 64k, or 1M"};
    }
    long long value = 0;
    for (size_t i = 0; i < digits; ++i) {
        const char ch = text[i];
        if (ch < '0' || ch > '9') {
            return {ErrorCode::BadArgs,
                    "--context expects a positive token count with an optional k or M suffix"};
        }
        const int digit = ch - '0';
        if (value > (std::numeric_limits<long long>::max() - digit) / 10) {
            return {ErrorCode::BadArgs, "--context token count is too large"};
        }
        value = value * 10 + digit;
    }
    if (value == 0) {
        return {ErrorCode::BadArgs, "--context must be greater than zero"};
    }
    if (value > std::numeric_limits<long long>::max() / multiplier) {
        return {ErrorCode::BadArgs, "--context token count is too large"};
    }
    out = value * multiplier;
    return ok_error();
}

Error parse_duration(const std::string& text, long long& out) {
    if (text.empty()) {
        return {ErrorCode::BadArgs, "--duration expects a positive duration such as 500ms, 60s, or 2m"};
    }
    size_t digits = text.size();
    long long multiplier = 1000;
    if (text.size() >= 2 && text.compare(text.size() - 2, 2, "ms") == 0) {
        multiplier = 1;
        digits -= 2;
    } else if (text.back() == 's') {
        digits -= 1;
    } else if (text.back() == 'm') {
        multiplier = 60 * 1000;
        digits -= 1;
    } else if (text.back() == 'h') {
        multiplier = 60 * 60 * 1000;
        digits -= 1;
    } else {
        return {ErrorCode::BadArgs, "--duration requires an ms, s, m, or h suffix"};
    }
    if (digits == 0) {
        return {ErrorCode::BadArgs, "--duration expects a positive duration such as 500ms, 60s, or 2m"};
    }
    long long value = 0;
    for (size_t i = 0; i < digits; ++i) {
        if (text[i] < '0' || text[i] > '9') {
            return {ErrorCode::BadArgs, "--duration expects an integer followed by ms, s, m, or h"};
        }
        const int digit = text[i] - '0';
        if (value > (std::numeric_limits<long long>::max() - digit) / 10) {
            return {ErrorCode::BadArgs, "--duration value is too large"};
        }
        value = value * 10 + digit;
    }
    if (value == 0 || value > std::numeric_limits<long long>::max() / multiplier) {
        return {ErrorCode::BadArgs, value == 0 ? "--duration must be greater than zero"
                                               : "--duration value is too large"};
    }
    const long long duration = value * multiplier;
    constexpr long long kMaxBenchmarkDurationMs = 7LL * 24LL * 60LL * 60LL * 1000LL;
    if (duration > kMaxBenchmarkDurationMs) {
        return {ErrorCode::BadArgs, "--duration cannot exceed 168h"};
    }
    out = duration;
    return ok_error();
}

bool valid_benchmark_mode_list(const std::string& text) {
    if (text.empty()) {
        return false;
    }
    size_t start = 0;
    size_t count = 0;
    bool saw_speed = false;
    while (start < text.size()) {
        const size_t comma = text.find(',', start);
        const std::string mode = text.substr(start, comma == std::string::npos
                                                        ? std::string::npos
                                                        : comma - start);
        if (mode != "speed" && mode != "long-context" && mode != "quality" &&
            mode != "refusals") {
            return false;
        }
        ++count;
        saw_speed = saw_speed || mode == "speed";
        if (comma == std::string::npos) {
            break;
        }
        start = comma + 1;
        if (start == text.size()) {
            return false;
        }
    }
    return !saw_speed || count == 1;
}

}  // namespace

Error parse_context_tokens(const std::string& text, long long& out) {
    return parse_context_tokens_impl(text, out);
}

const char* format_name(OutputFormat format) {
    switch (format) {
        case OutputFormat::Text:
            return "text";
        case OutputFormat::Json:
            return "json";
        case OutputFormat::Ndjson:
            return "ndjson";
    }
    return "text";
}

ParseResult parse_args(int argc, char** argv, const Options& base_options) {
    Options opts = base_options;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        std::string value;
        const size_t eq = arg.find('=');
        std::string opt = arg;
        if (eq != std::string::npos && arg.rfind("--", 0) == 0) {
            opt = arg.substr(0, eq);
            value = arg.substr(eq + 1);
        }

        auto take_value = [&]() -> std::optional<Error> {
            if (!value.empty() || eq != std::string::npos) {
                return std::nullopt;
            }
            if (i + 1 >= argc) {
                return Error{ErrorCode::BadArgs, opt + " requires a value"};
            }
            value = argv[++i];
            return std::nullopt;
        };

        if (arg == "--help" || arg == "-h") {
            opts.help = true;
        } else if ((arg == "benchmark" && i == 1) || arg == "--benchmark") {
            opts.benchmark = true;
            opts.format = OutputFormat::Ndjson;
        } else if ((arg == "agent" && i == 1) || arg == "--agent" || arg == "-a") {
            opts.agent = true;
        } else if ((arg == "run" && i == 1)) {
            opts.agent_run = true;
        } else if ((arg == "plan" && i == 1)) {
            opts.agent_run = true;
            opts.agent_plan = true;
        } else if ((arg == "grade" && i == 1) || arg == "--grade") {
            opts.grade = true;
            opts.format = OutputFormat::Ndjson;
            if (!opts.stream_explicit) {
                opts.stream = false;
            }
            if (!opts.has_temperature) {
                opts.temperature = 0.0;
                opts.has_temperature = true;
            }
        } else if (arg == "--validate-dataset") {
            opts.benchmark_validate = true;
            opts.benchmark_options_seen = true;
        } else if (arg == "--list-cases") {
            opts.benchmark_list = true;
            opts.benchmark_options_seen = true;
        } else if (arg == "--version") {
            opts.version = true;
        } else if (arg == "--list-models") {
            opts.list_models = true;
        } else if (arg == "--index-code") {
            opts.index_code = true;
        } else if (arg == "--print-index") {
            opts.print_index = true;
        } else if (arg == "--clear-index") {
            opts.clear_index = true;
        } else if (arg == "--security-review") {
            opts.security_review = true;
        } else if (arg == "--security-review-log") {
            opts.security_review_log_enabled = true;
            opts.security_review_log_cli_explicit = true;
        } else if (arg == "--no-security-review-log") {
            opts.security_review_log_enabled = false;
            opts.security_review_log_cli_explicit = true;
        } else if (arg == "--agent-log") {
            opts.agent_log_enabled = true;
            opts.agent_log_cli_explicit = true;
        } else if (arg == "--no-agent-log") {
            opts.agent_log_enabled = false;
            opts.agent_log_cli_explicit = true;
        } else if (arg == "--stream") {
            opts.stream = true;
            opts.stream_explicit = true;
        } else if (arg == "--no-stream") {
            opts.stream = false;
            opts.stream_explicit = true;
        } else if (arg == "--responses") {
            opts.api = "responses";
        } else if (arg == "--quiet") {
            opts.quiet = true;
        } else if (arg == "--verbose" || arg == "-v") {
            opts.verbose = true;
        } else if (arg == "--debug") {
            opts.debug = true;
        } else if (arg == "--no-config") {
            opts.no_config = true;
        } else if (arg == "--trace-http") {
            opts.trace_http = true;
        } else if (arg == "--insecure-tls") {
            opts.insecure_tls = true;
        } else if (arg == "--key-stdin") {
            opts.key_stdin = true;
        } else if (arg == "--repl" || arg == "-i") {
            opts.repl = true;
        } else if (arg == "--chat" || arg == "-c" || arg == "--tui") {
            opts.tui = true;
        } else if (arg == "--nocolors" || arg == "--no-colors") {
            opts.no_colors = true;
        } else if (arg == "--allow-private-url-fetch") {
            opts.allow_private_url_fetch = true;
        } else if (arg == "--editor" || opt == "--editor" || arg == "-e") {
            opts.editor = true;
            if (!value.empty()) {
                opts.editor_path = value;
            } else if (i + 1 < argc) {
                const char* next = argv[i + 1];
                if (next[0] != '\0' && next[0] != '-') {
                    opts.editor_path = argv[++i];
                }
            }
        } else if (needs_value(opt)) {
            if (auto err = take_value()) {
                return {opts, *err};
            }
            if (opt == "-p" || opt == "--prompt") {
                opts.prompt = value;
            } else if (opt == "--prompt-file") {
                opts.prompt_file = value;
            } else if (opt == "-r" || opt == "--run") {
                opts.agent_run = true;
                opts.prompt = value;
            } else if (opt == "--run-file") {
                opts.agent_run = true;
                opts.prompt_file = value;
            } else if (opt == "--plan") {
                opts.agent_run = true;
                opts.agent_plan = true;
                opts.prompt = value;
            } else if (opt == "--plan-file") {
                opts.agent_run = true;
                opts.agent_plan = true;
                opts.prompt_file = value;
            } else if (opt == "-s" || opt == "--system") {
                opts.system = value;
            } else if (opt == "--system-file") {
                opts.system_file = value;
            } else if (opt == "-m" || opt == "--model" || opt == "-model") {
                opts.model = value;
                opts.model_explicit = true;
            } else if (opt == "-t" || opt == "--temperature") {
                Error err = parse_double(opt, value, opts.temperature);
                if (!err.ok()) {
                    return {opts, err};
                }
                opts.has_temperature = true;
                opts.temperature_preset_applied = false;
            } else if (opt == "--top-p") {
                Error err = parse_double(opt, value, opts.top_p);
                if (!err.ok()) {
                    return {opts, err};
                }
                opts.has_top_p = true;
            } else if (opt == "--top-k") {
                Error err = parse_int(opt, value, opts.top_k);
                if (!err.ok()) {
                    return {opts, err};
                }
                if (opts.top_k < 0) {
                    return {opts, {ErrorCode::BadArgs, "--top-k must be a non-negative integer"}};
                }
                opts.has_top_k = true;
            } else if (opt == "--min-p") {
                Error err = parse_double(opt, value, opts.min_p);
                if (!err.ok()) {
                    return {opts, err};
                }
                opts.has_min_p = true;
            } else if (opt == "--repeat-penalty") {
                Error err = parse_double(opt, value, opts.repeat_penalty);
                if (!err.ok()) {
                    return {opts, err};
                }
                opts.has_repeat_penalty = true;
            } else if (opt == "--presence-penalty") {
                Error err = parse_double(opt, value, opts.presence_penalty);
                if (!err.ok()) {
                    return {opts, err};
                }
                opts.has_presence_penalty = true;
            } else if (opt == "--reasoning") {
                Error err = config::parse_reasoning_selection(value, opts.reasoning);
                if (!err.ok()) {
                    return {opts, err};
                }
                opts.reasoning_explicit = true;
                opts.reasoning_cli_explicit = true;
            } else if (opt == "--purpose") {
                if (!chat::generation::is_chat_purpose(value)) {
                    return {opts,
                            {ErrorCode::BadArgs,
                             "--purpose must be " + chat::generation::chat_purpose_description()}};
                }
                opts.chat_purpose = value;
            } else if (opt == "--max-output-tokens") {
                Error err = parse_int(opt, value, opts.max_output_tokens);
                if (!err.ok()) {
                    return {opts, err};
                }
                opts.has_max_output_tokens = true;
            } else if (opt == "--format") {
                if (value == "text") {
                    opts.format = OutputFormat::Text;
                } else if (value == "json") {
                    opts.format = OutputFormat::Json;
                } else if (value == "ndjson" || value == "jsond" || value == "jsonl") {
                    opts.format = OutputFormat::Ndjson;
                } else {
                    return {opts, {ErrorCode::BadArgs, "--format must be text, json, ndjson, jsonl, or jsond"}};
                }
            } else if (opt == "--output-format") {
                if (value == "json") {
                    opts.format = OutputFormat::Json;
                    opts.output_format_explicit = true;
                } else if (value == "ndjson" || value == "jsond") {
                    opts.format = OutputFormat::Ndjson;
                    opts.output_format_explicit = true;
                } else if (ainiux::markdown::parse_output_format(value, opts.output_format)) {
                    opts.output_format_explicit = true;
                    opts.rendered_output_format_explicit = true;
                } else {
                    return {opts, {ErrorCode::BadArgs, "--output-format must be html, md, plaintext, json, jsond, or ndjson"}};
                }
            } else if (opt == "--output") {
                opts.output_path = value;
            } else if (opt == "--save-chat") {
                opts.save_chat_path = value;
            } else if (opt == "--load-chat") {
                opts.load_chat_path = value;
            } else if (opt == "--provider") {
                opts.provider = value;
                opts.provider_explicit = true;
            } else if (opt == "--profile") {
                opts.profile = value;
                opts.provider = value;
                opts.provider_explicit = true;
            } else if (opt == "--api") {
                opts.api_explicit = true;
                if (value == "chat" || value == "chat_completions" || value == "chat-completions") {
                    opts.api = "chat";
                } else if (value == "responses" || value == "responses_api" || value == "responses-api") {
                    opts.api = "responses";
                } else {
                    return {opts, {ErrorCode::BadArgs, "--api must be chat or responses"}};
                }
            } else if (opt == "--base-url") {
                opts.base_url = value;
                opts.base_url_cli_explicit = true;
            } else if (opt == "--chat-url") {
                opts.chat_url = value;
                opts.chat_url_cli_explicit = true;
            } else if (opt == "--models-url") {
                opts.models_url = value;
                opts.models_url_cli_explicit = true;
            } else if (opt == "--responses-url") {
                opts.responses_url = value;
                opts.responses_url_cli_explicit = true;
            } else if (opt == "--key-env") {
                opts.key_env = value;
            } else if (opt == "--key-file") {
                opts.key_file = value;
            } else if (opt == "-k" || opt == "--key") {
                opts.key = value;
            } else if (opt == "--header") {
                opts.headers.push_back(value);
            } else if (opt == "--connect-timeout") {
                Error err = parse_long(opt, value, opts.connect_timeout_seconds);
                if (!err.ok()) {
                    return {opts, err};
                }
            } else if (opt == "--timeout") {
                Error err = parse_long(opt, value, opts.timeout_seconds);
                if (!err.ok()) {
                    return {opts, err};
                }
            } else if (opt == "--proxy") {
                opts.proxy = value;
            } else if (opt == "--fetch-url") {
                opts.fetch_url = value;
            } else if (opt == "--search") {
                opts.search_query = value;
            } else if (opt == "--web-search-provider") {
                opts.web_search_provider = value;
            } else if (opt == "--input") {
                opts.input_path = value;
            } else if (opt == "--attach") {
                opts.attachment_paths.push_back(value);
            } else if (opt == "--html-file") {
                opts.html_file = value;
            } else if (opt == "--html-format") {
                if (value != "text" && value != "plain" && value != "plaintext" && value != "markdown" && value != "md") {
                    return {opts, {ErrorCode::BadArgs, "--html-format must be text or markdown"}};
                }
                opts.html_format = value;
            } else if (opt == "--max-fetch-bytes") {
                Error err = parse_long(opt, value, opts.max_fetch_bytes);
                if (!err.ok()) {
                    return {opts, err};
                }
            } else if (opt == "--max-web-search-results") {
                long parsed = 0;
                Error err = parse_long(opt, value, parsed);
                if (!err.ok() || parsed <= 0) {
                    return {opts, {ErrorCode::BadArgs, "--max-web-search-results expects an integer greater than zero"}};
                }
                opts.max_web_search_results = static_cast<int>(parsed);
                opts.max_web_search_results_explicit = true;
            } else if (opt == "--max-input-bytes") {
                Error err = parse_long(opt, value, opts.max_input_bytes);
                if (!err.ok()) {
                    return {opts, err};
                }
            } else if (opt == "--max-source-code-file-size") {
                long long parsed_size = 0;
                Error err = editor::parse_byte_size(value, parsed_size);
                if (!err.ok() || parsed_size < 0 ||
                    static_cast<unsigned long long>(parsed_size) >
                        static_cast<unsigned long long>(std::numeric_limits<size_t>::max())) {
                    return {opts, {ErrorCode::BadArgs,
                                   "--max-source-code-file-size expects a byte size such as 1M"}};
                }
                opts.max_source_code_file_size = static_cast<size_t>(parsed_size);
            } else if (opt == "--trusted-prompt-dir") {
                opts.trusted_prompt_dir = value;
            } else if (opt == "--max-image-bytes") {
                Error err = parse_long(opt, value, opts.max_image_bytes);
                if (!err.ok()) {
                    return {opts, err};
                }
            } else if (opt == "--max-context-bytes") {
                Error err = parse_long(opt, value, opts.max_context_bytes);
                if (!err.ok()) {
                    return {opts, err};
                }
            } else if (opt == "--context") {
                Error err = parse_context_tokens(value, opts.context_tokens);
                if (!err.ok()) {
                    return {opts, err};
                }
                opts.has_context_tokens = true;
            } else if (opt == "--context-policy") {
                if (!context::policy::is_valid(value)) {
                    return {opts, {ErrorCode::BadArgs,
                                   "--context-policy must be " + context::policy::usage_description()}};
                }
                opts.context_policy = value;
            } else if (opt == "--image-capability") {
                if (!option_values::is_image_capability(value)) {
                    return {opts,
                            {ErrorCode::BadArgs,
                             "--image-capability must be " + option_values::image_capability_description()}};
                }
                opts.image_capability = value;
            } else if (opt == "--dataset") {
                opts.benchmark_dataset = value;
                opts.benchmark_dataset_explicit = true;
                opts.benchmark_options_seen = true;
            } else if (opt == "--grade-input") {
                opts.grade_input = value;
                opts.grade_input_explicit = true;
                opts.benchmark_options_seen = true;
            } else if (opt == "--category") {
                opts.benchmark_category = value;
                opts.benchmark_options_seen = true;
            } else if (opt == "--case") {
                opts.benchmark_case = value;
                opts.benchmark_options_seen = true;
            } else if (opt == "--runs") {
                Error err = parse_int(opt, value, opts.benchmark_runs);
                if (!err.ok() || opts.benchmark_runs < 1) {
                    return {opts, {ErrorCode::BadArgs, "--runs expects an integer greater than zero"}};
                }
                opts.benchmark_options_seen = true;
                opts.benchmark_runs_explicit = true;
            } else if (opt == "--warmup") {
                Error err = parse_int(opt, value, opts.benchmark_warmup);
                if (!err.ok()) {
                    return {opts, err};
                }
                opts.benchmark_options_seen = true;
                opts.benchmark_warmup_explicit = true;
            } else if (opt == "--limit") {
                Error err = parse_int(opt, value, opts.benchmark_limit);
                if (!err.ok()) {
                    return {opts, err};
                }
                opts.benchmark_options_seen = true;
            } else if (opt == "--mode") {
                if (value.rfind("speed,", 0) == 0 || value.find(",speed") != std::string::npos) {
                    return {opts, {ErrorCode::BadArgs,
                                   "speed benchmark mode cannot be combined with other modes"}};
                }
                if (!valid_benchmark_mode_list(value)) {
                    return {opts, {ErrorCode::BadArgs,
                                   "--mode expects a comma-separated list of speed, long-context, quality, or refusals"}};
                }
                opts.benchmark_mode = value;
                opts.benchmark_options_seen = true;
                opts.benchmark_mode_explicit = true;
            } else if (opt == "--concurrency") {
                Error err = parse_int(opt, value, opts.benchmark_concurrency);
                if (!err.ok() || opts.benchmark_concurrency < 1 || opts.benchmark_concurrency > 256) {
                    return {opts, {ErrorCode::BadArgs,
                                   "--concurrency expects an integer from 1 through 256"}};
                }
                opts.benchmark_options_seen = true;
            } else if (opt == "--duration") {
                Error err = parse_duration(value, opts.benchmark_duration_ms);
                if (!err.ok()) {
                    return {opts, err};
                }
                opts.benchmark_options_seen = true;
                opts.benchmark_duration_explicit = true;
            } else if (opt == "--summary-format") {
                if (value != "table" && value != "csv") {
                    return {opts, {ErrorCode::BadArgs,
                                   "--summary-format must be table or csv"}};
                }
                opts.benchmark_summary_format = value;
                opts.benchmark_options_seen = true;
            }
        } else if (!arg.empty() && arg[0] == '-') {
            return {opts, {ErrorCode::BadArgs, "unknown option: " + arg}};
        } else {
            if (opts.agent_plan && argc > 1 && std::string(argv[1]) == "plan" &&
                opts.prompt.empty() && opts.prompt_file.empty()) {
                opts.prompt = arg;
                continue;
            }
            if (opts.agent_plan && argc > 1 && std::string(argv[1]) == "plan") {
                return {opts,
                        {ErrorCode::BadArgs,
                         "ainiux plan accepts one positional goal; select providers with "
                         "--provider or another named option"}};
            }
            if (!opts.positional_url.empty()) {
                return {opts, {ErrorCode::BadArgs, "unexpected extra positional argument: " + arg}};
            }
            opts.positional_url = arg;
        }
    }
    if (!opts.provider_explicit &&
        (opts.positional_url.rfind("http://", 0) == 0 ||
         opts.positional_url.rfind("https://", 0) == 0)) {
        // A direct OpenAI-compatible endpoint must override an inherited
        // remembered/configured provider selection. Explicit --provider still wins.
        opts.provider = "openai";
    }
    return {opts, ok_error()};
}

ParseResult parse_args(int argc, char** argv) {
    return parse_args(argc, argv, Options{});
}

Error validate_index_mode_arguments(int argc, char** argv, const Options& options) {
    if (!options.index_code && !options.print_index && !options.clear_index) return ok_error();
    if (options.clear_index && (options.index_code || options.print_index)) {
        return {ErrorCode::BadArgs,
                "--clear-index cannot be combined with --index-code or --print-index"};
    }
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        const std::size_t equals = argument.find('=');
        const std::string option = equals == std::string::npos ? argument : argument.substr(0, equals);
        if (option == "--index-code" || option == "--print-index" || option == "--clear-index" ||
            option == "--no-config" || option == "--debug") {
            continue;
        }
        if (!options.clear_index &&
            (option == "--output" || option == "--max-source-code-file-size")) {
            if (equals == std::string::npos) ++i;
            continue;
        }
        return {ErrorCode::BadArgs,
                option + " cannot be combined with code index mode"};
    }
    if (options.clear_index) return ok_error();
    if (!options.print_index && !options.output_path.empty()) {
        return {ErrorCode::BadArgs, "--output requires --print-index in code index mode"};
    }
    return ok_error();
}

Error validate_security_review_arguments(int argc, char** argv, const Options& options) {
    if (!options.security_review) return ok_error();
    if (options.agent || options.agent_run) {
        return {ErrorCode::BadArgs,
                "--security-review cannot be combined with --agent or --run"};
    }
    if (options.index_code || options.print_index || options.clear_index) {
        return {ErrorCode::BadArgs,
                "--security-review cannot be combined with code index mode flags; it refreshes the index itself"};
    }
    bool positional_seen = false;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        const std::size_t equals = argument.find('=');
        const std::string option = equals == std::string::npos ? argument : argument.substr(0, equals);
        if (!option.empty() && option.front() != '-') {
            if (positional_seen) {
                return {ErrorCode::BadArgs,
                        "unexpected extra positional argument in security review mode: " + option};
            }
            positional_seen = true;
            continue;
        }
        if (option == "--security-review" || option == "--security-review-log" ||
            option == "--no-security-review-log" || option == "--responses" ||
            option == "--key-stdin" || option == "--quiet" || option == "--debug" ||
            option == "--no-config" || option == "--trace-http" ||
            option == "--insecure-tls" || option == "--stream" || option == "--no-stream") {
            continue;
        }
        const bool takes_value =
            option == "-m" || option == "--model" || option == "-model" ||
            option == "--provider" || option == "--profile" || option == "--api" ||
            option == "--base-url" || option == "--chat-url" || option == "--models-url" ||
            option == "--responses-url" || option == "--key-env" || option == "--key-file" ||
            option == "-k" || option == "--key" || option == "--header" ||
            option == "--reasoning" || option == "--connect-timeout" || option == "--timeout" ||
            option == "--proxy" || option == "--trusted-prompt-dir";
        if (takes_value) {
            if (equals == std::string::npos) ++i;
            continue;
        }
        return {ErrorCode::BadArgs, option + " cannot be combined with --security-review"};
    }
    if (options.format != OutputFormat::Text || options.rendered_output_format_explicit) {
        return {ErrorCode::BadArgs,
                "--security-review emits Markdown only and cannot use alternate output formats"};
    }
    if (!options.output_path.empty()) {
        return {ErrorCode::BadArgs,
                "--security-review writes Markdown to stdout; redirect stdout instead of using --output"};
    }
    return ok_error();
}

Error validate_agent_run_arguments(int argc, char** argv, const Options& options) {
    if (!options.agent_run) return ok_error();
    if (options.agent) {
        return {ErrorCode::BadArgs,
                "--run cannot be combined with interactive --agent/-a; use one entry mode"};
    }
    if (options.security_review) {
        return {ErrorCode::BadArgs, "--run cannot be combined with --security-review"};
    }
    if (options.benchmark || options.grade) {
        return {ErrorCode::BadArgs, "--run cannot be combined with --benchmark or --grade"};
    }
    if (options.index_code || options.print_index || options.clear_index) {
        return {ErrorCode::BadArgs,
                "--run cannot be combined with code index mode flags; it refreshes the index itself"};
    }
    if (options.editor || options.repl || options.tui || options.list_models) {
        return {ErrorCode::BadArgs,
                "--run is a one-shot headless agent; use --agent/-a for the interactive agent TUI, "
                "or --chat/--repl/--editor for ordinary interactive modes"};
    }
    if (options.prompt.empty() && options.prompt_file.empty()) {
        return {ErrorCode::BadArgs,
                "one-shot agent requires a goal via --run/--run-file or --plan/--plan-file"};
    }
    if (!options.prompt.empty() && !options.prompt_file.empty()) {
        return {ErrorCode::BadArgs,
                "one-shot agent accepts exactly one goal source, not both text and a file"};
    }
    bool saw_act_entry = false;
    bool saw_plan_entry = false;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        const std::size_t equals = argument.find('=');
        const std::string option =
            equals == std::string::npos ? argument : argument.substr(0, equals);
        if ((i == 1 && option == "run") || option == "-r" || option == "--run" ||
            option == "--run-file")
            saw_act_entry = true;
        if ((i == 1 && option == "plan") || option == "--plan" ||
            option == "--plan-file")
            saw_plan_entry = true;
    }
    if (saw_act_entry && saw_plan_entry)
        return {ErrorCode::BadArgs,
                "Act run and Plan entry forms cannot be combined; choose one task mode"};
    if (!options.system.empty() || !options.system_file.empty()) {
        return {ErrorCode::BadArgs,
                "one-shot agent uses the trusted master/agent system prompt; omit -s/--system-file"};
    }
    bool positional_seen = false;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        const std::size_t equals = argument.find('=');
        const std::string option = equals == std::string::npos ? argument : argument.substr(0, equals);
        if (!option.empty() && option.front() != '-') {
            if (option == "run" && i == 1) continue;
            if (option == "plan" && i == 1) continue;
            if (options.agent_plan && argc > 1 && std::string(argv[1]) == "plan" &&
                option == options.prompt)
                continue;
            if (positional_seen) {
                return {ErrorCode::BadArgs,
                        "unexpected extra positional argument in --run mode: " + option};
            }
            positional_seen = true;
            continue;
        }
        if (option == "run" || option == "--agent-log" || option == "--no-agent-log" ||
            option == "--responses" || option == "--key-stdin" || option == "--quiet" ||
            option == "--debug" || option == "--no-config" || option == "--trace-http" ||
            option == "--insecure-tls" || option == "--stream" || option == "--no-stream") {
            continue;
        }
        // Goal comes only from --run/--run-file; reject ordinary -p for this mode.
        if (option == "-p" || option == "--prompt" || option == "--prompt-file") {
            return {ErrorCode::BadArgs,
                    "one-shot agent goal comes from -r/--run or --run-file; do not also pass "
                    "-p/--prompt/--prompt-file"};
        }
        const bool takes_value =
            option == "-m" || option == "--model" || option == "-model" ||
            option == "--provider" || option == "--profile" || option == "--api" ||
            option == "--base-url" || option == "--chat-url" || option == "--models-url" ||
            option == "--responses-url" || option == "--key-env" || option == "--key-file" ||
            option == "-k" || option == "--key" || option == "--header" ||
            option == "--reasoning" || option == "--connect-timeout" || option == "--timeout" ||
            option == "--proxy" || option == "--trusted-prompt-dir" ||
            option == "--max-source-code-file-size" || option == "-r" || option == "--run" ||
            option == "--run-file" || option == "--plan" || option == "--plan-file";
        if (takes_value) {
            if (equals == std::string::npos) ++i;
            continue;
        }
        return {ErrorCode::BadArgs, option + " cannot be combined with --run"};
    }
    if (options.format != OutputFormat::Text || options.rendered_output_format_explicit) {
        return {ErrorCode::BadArgs,
                "--run writes final assistant text to stdout and cannot use alternate formats"};
    }
    if (!options.output_path.empty()) {
        return {ErrorCode::BadArgs,
                "--run writes final text to stdout; redirect stdout instead of using --output"};
    }
    return ok_error();
}

Error validate_agent_interactive_arguments(int argc, char** argv, const Options& options) {
    if (!options.agent) return ok_error();
    if (options.agent_run) {
        return {ErrorCode::BadArgs,
                "--agent cannot be combined with --run; use --agent for the interactive TUI or "
                "--run for a one-shot goal"};
    }
    if (options.security_review) {
        return {ErrorCode::BadArgs, "--agent cannot be combined with --security-review"};
    }
    if (options.benchmark || options.grade) {
        return {ErrorCode::BadArgs, "--agent cannot be combined with --benchmark or --grade"};
    }
    if (options.index_code || options.print_index || options.clear_index) {
        return {ErrorCode::BadArgs,
                "--agent cannot be combined with code index mode flags; it refreshes the index itself"};
    }
    if (options.editor || options.repl || options.tui || options.list_models) {
        return {ErrorCode::BadArgs,
                "--agent cannot be combined with --chat/--repl/--editor/--list-models"};
    }
    if (!options.system.empty() || !options.system_file.empty()) {
        return {ErrorCode::BadArgs,
                "agent mode uses the trusted master/agent system prompt; omit -s/--system-file"};
    }
    bool positional_seen = false;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        const std::size_t equals = argument.find('=');
        const std::string option = equals == std::string::npos ? argument : argument.substr(0, equals);
        if (!option.empty() && option.front() != '-') {
            if (option == "agent" && i == 1) continue;
            if (positional_seen) {
                return {ErrorCode::BadArgs,
                        "unexpected extra positional argument in --agent mode: " + option};
            }
            positional_seen = true;
            continue;
        }
        if (option == "--agent" || option == "-a" || option == "agent" || option == "--agent-log" ||
            option == "--no-agent-log" || option == "--responses" || option == "--key-stdin" ||
            option == "--quiet" || option == "--debug" || option == "--no-config" ||
            option == "--trace-http" || option == "--insecure-tls" || option == "--stream" ||
            option == "--no-stream" || option == "--nocolors" || option == "--no-colors") {
            continue;
        }
        const bool takes_value =
            option == "-m" || option == "--model" || option == "-model" ||
            option == "--provider" || option == "--profile" || option == "--api" ||
            option == "--base-url" || option == "--chat-url" || option == "--models-url" ||
            option == "--responses-url" || option == "--key-env" || option == "--key-file" ||
            option == "-k" || option == "--key" || option == "--header" ||
            option == "--reasoning" || option == "--connect-timeout" || option == "--timeout" ||
            option == "--proxy" || option == "--trusted-prompt-dir" || option == "-p" ||
            option == "--prompt" || option == "--prompt-file" ||
            option == "--max-source-code-file-size" || option == "--load-chat" ||
            option == "--save-chat";
        if (takes_value) {
            if (equals == std::string::npos) ++i;
            continue;
        }
        return {ErrorCode::BadArgs, option + " cannot be combined with --agent"};
    }
    if (options.format != OutputFormat::Text || options.rendered_output_format_explicit) {
        return {ErrorCode::BadArgs, "--agent currently supports --format text only"};
    }
    if (!options.output_path.empty()) {
        return {ErrorCode::BadArgs, "--agent cannot be combined with --output"};
    }
    return ok_error();
}

std::string help_text() {
    return app_version_label() + R"( - script-friendly OpenAI-compatible chat CLI

Usage:
  ainiux [BASE_URL|PROFILE] -p TEXT [options]
  ainiux --list-models [BASE_URL|PROFILE] [options]
  ainiux -i, --repl [BASE_URL|PROFILE] [options]
  ainiux -c, --chat [BASE_URL|PROFILE] [options]
  ainiux [BASE_URL|PROFILE] -e, --editor [PATH] [--output PATH]
  ainiux --input PATH [--output-format md|html|plaintext|json|jsond] [--output PATH]
  ainiux --fetch-url URL [--output-format md|html|plaintext|json|jsond] [--output PATH]
  ainiux --search QUERY [--output-format md|html|plaintext|json|jsond] [--output PATH]
  ainiux --benchmark [--dataset FILE] [--mode MODE] [--provider NAME] [-m MODEL]
  ainiux benchmark [--dataset FILE] [--mode MODE] [--provider NAME] [-m MODEL]
  ainiux --grade [--grade-input FILE] [--provider NAME] [-m JUDGE_MODEL]
  ainiux --index-code [--max-source-code-file-size SIZE]
  ainiux --print-index [--output PATH]
  ainiux --clear-index
  ainiux [BASE_URL|PROFILE] -m MODEL --security-review
  ainiux -a, --agent [BASE_URL|PROFILE] [-m MODEL]
  ainiux agent [BASE_URL|PROFILE] [-m MODEL]
  ainiux [BASE_URL|PROFILE] -m MODEL -r, --run "goal"
  ainiux run [BASE_URL|PROFILE] -m MODEL -r "goal"
  ainiux [BASE_URL|PROFILE] -m MODEL --run-file PATH
  ainiux plan "goal" --provider PROFILE -m MODEL
  ainiux [BASE_URL|PROFILE] -m MODEL --plan "goal"
  ainiux [BASE_URL|PROFILE] -m MODEL --plan-file PATH

Examples:
  ainiux http://localhost:8000 -p "What is the capital of Norway?"
  ainiux --base-url http://localhost:8000/v1 -p "Hello"
  ainiux --provider openai -m MODEL -p "Hello"
  ainiux --provider lm_studio -m MODEL -p "Hello from local LM Studio"
  ainiux --provider lmstudio --list-models
  ainiux --prompt-file prompt.txt --system-file system.txt --format json
  ainiux http://localhost:8000 -p "Write a report" --output-format html --output report.html
  ainiux openrouter -model MODEL -i
  ainiux lmstudio -i
  ainiux -c lmstudio
  ainiux --chat lmstudio
  ainiux --repl --load-chat chat.json --save-chat chat.json
  ainiux -e notes.txt
  ainiux --editor
  ainiux --provider none -e notes.txt
  ainiux lmstudio -e notes.txt
  ainiux openrouter --editor notes.txt
  ainiux http://localhost:1234/v1 --editor draft.md
  ainiux --provider none --input page.html --output-format md
  ainiux --provider none --fetch-url https://example.com --output-format md
  ainiux --provider none --search "web scraping" --output-format plaintext
  ainiux http://localhost:8000 -p "Summarize" --search "latest news"
  ainiux --input page.html --output-format plaintext
  printf 'piped text' | ainiux --input stdin --output stdout
  command | ainiux http://localhost:8000 -p "Summarize" --attach stdin
  ainiux http://localhost:8000 -p "Compare these" --attach one.md --attach two.txt
  ainiux http://localhost:30000 -p "Describe this image" --input photo.png
  ainiux -a lmstudio -m MODEL
  ainiux --agent openrouter -m MODEL
  ainiux agent http://localhost:30000 -m MODEL
  ainiux http://localhost:30000 -m MODEL -r "remove all empty files and folders"
  ainiux run openrouter -m MODEL --run "add unit tests to compute.py"
  ainiux run lmstudio -m MODEL --run-file goal.txt
  ainiux plan "design server mode" --provider openai -m MODEL
  ainiux benchmark --validate-dataset
  ainiux benchmark --category reasoning --limit 2 --provider lm_studio -m MODEL
  ainiux --benchmark --dataset prompts.jsonl --mode speed --concurrency 4 --duration 60s
  ainiux --benchmark --dataset eval.jsonl --mode quality,refusals --output results/
  ainiux --grade --category reasoning --output results/ --provider openai -m JUDGE_MODEL

Options:
  Mode:
      --list-models             List models from the configured endpoint.
  -i, --repl                    Start a simple line-oriented interactive chat.
  -c, --chat                    Start the full-screen non-blocking terminal chat.
      --nocolors                Disable TUI color styling.
  -e, --editor [PATH]           Start the standalone multiline editor; PATH is the file to open.
                                A provider shortcut/profile may precede -e/--editor without
                                -m/--model; choose a model inside the editor with /model
                                (like -c/--chat). Use --provider none for offline local editing.
  -a, --agent                   Start interactive agent mode (separate from --chat; shares
                                the full-screen shell and provider/model/reasoning pickers,
                                but runs the agent tool loop; also: ainiux agent ...).
  -r, --run TEXT                One-shot headless agent goal with read+write tools
                                (also: ainiux run ...).
      --run-file PATH           One-shot agent goal from a file; use '-' for stdin.
      --plan TEXT               One-shot Plan agent goal; writes only planning documents.
      --plan-file PATH          One-shot Plan goal from a file; use '-' for stdin.
      --benchmark               Run benchmark mode (also: ainiux benchmark ...).
      --grade                   Grade benchmark results with a judge model (also: ainiux grade ...).
      --index-code              Create or incrementally refresh .ainiux-pr/index.sqlite.
      --print-index             Print the stored project code index as Markdown.
      --clear-index             Remove the project code index database.
      --security-review         Review every eligible indexed workspace file and print Markdown.
      --security-review-log     Enable the local per-run JSONL diagnostic log (default).
      --no-security-review-log  Disable the local per-run JSONL diagnostic log.
      --agent-log               Enable the local per-run agent JSONL diagnostic log (default;
                                used with --run / interactive --agent).
      --no-agent-log            Disable the local per-run agent JSONL diagnostic log.
      --trusted-prompt-dir DIR  Trusted prompt resource override for testing/installations.
      --max-source-code-file-size SIZE
                                Maximum supported source file size; default 10M.

  Prompt and generation:
  -p, --prompt TEXT
      --prompt-file PATH        Use '-' to read the prompt from stdin.
  -s, --system TEXT
      --system-file PATH
  -m, --model, -model MODEL
  -t, --temperature FLOAT
      --top-p FLOAT
      --top-k N
      --min-p FLOAT
      --repeat-penalty FLOAT
      --presence-penalty FLOAT
      --reasoning auto|VALUE|TOKENS
                                Provider default, a named value (for example high),
                                or an exact non-negative token budget.
      --purpose general|coding|instruct|creative
      --max-output-tokens N
      --stream | --no-stream

  Output:
      --format text|json|ndjson|jsonl|jsond
      --output-format html|md|plaintext|json|jsond|ndjson
      --output PATH             Use 'stdout' to write to standard output.

  Input and attachments:
      --input PATH              Read text/Markdown/HTML, or attach PNG/JPEG/GIF with -p;
                                'stdin' reads UTF-8 plaintext from standard input.
      --attach PATH             Add text/Markdown/HTML or PNG/JPEG/GIF; repeatable;
                                'stdin' reads UTF-8 plaintext from standard input.
      --fetch-url URL           Fetch HTML for extraction, or as prompt context with -p.
      --search QUERY            Run a web search and use results as prompt context with -p.
      --web-search-provider NAME
                                auto, tavily, firecrawl, exa, searxng, or duckduckgo.
      --max-web-search-results N
                                Maximum ranked web search hits to include; default 3.
                                Override with config web_search.max_results or
                                MAXIMUM_WEB_SEARCH_RESULTS.
      --html-format text|markdown
                                Compatibility alias for old HTML extraction commands.
      --max-fetch-bytes N       Default 1048576.
      --max-input-bytes N       Maximum bytes per text input/attachment; default 1048576.
      --max-image-bytes N       Maximum image file size; default 20971520.
      --allow-private-url-fetch Allow loopback/private URL fetches.

  Context:
      --context TOKENS          Model context-window size; k is 1024, M is 1000000.
      --max-context-bytes N     Request text budget; 0 disables the client budget.
      --context-policy POLICY   error, truncate-oldest, truncate-middle,
                                summarize-oldest, summarize-middle, or provider-auto.
      --image-capability MODE   auto, allow, or deny; allow overrides model detection.

  Chat history:
      --save-chat PATH          Save JSON chat history after a successful reply.
      --load-chat PATH          Load JSON chat history before sending.

  Benchmark:
      --dataset PATH            Benchmark JSONL dataset; default 'builtin'.
      --mode MODE               speed, long-context, quality, refusals; comma-separated.
      --concurrency N           Concurrent benchmark requests; default 1, maximum 256.
      --duration TIME           Speed-test duration with ms, s, m, or h suffix; default 60s.
      --summary-format FORMAT   Human summary on stderr: table (default) or csv.
      --category NAME           Run only benchmark cases in this category.
      --case ID                 Run only one benchmark case.
      --runs N                  Measured runs per case; default 1.
      --warmup N                Unreported warmup runs per case; default 0.
      --limit N                 Limit selected benchmark cases; 0 means unlimited.
      --validate-dataset        Validate and summarize the dataset without model calls.
      --list-cases              List selected benchmark cases without model calls.
      --grade-input PATH        Grade this result JSONL; otherwise select the newest matching
                                benchmark-*.jsonl from the relevant output directory.

  Provider and endpoint:
      --provider NAME           none (offline), openrouter, openai, kimi, llama.cpp,
                                lm_studio, ollama, vllm, sglang, zai, qwen, etc.
      --profile NAME            Alias for --provider.
      --api chat|responses      Use Chat Completions (default) or Responses API.
      --responses               Shortcut for --api responses.
      --base-url URL
      --chat-url URL
      --models-url URL
      --responses-url URL       Override the Responses API endpoint.

  Credentials:
      --key-env NAME
      --key-file PATH
      --key-stdin
  -k, --key TEXT                Discouraged; command line keys may be visible locally.
      --header "Name: Value"

  Network:
      --connect-timeout SECONDS
      --timeout SECONDS
      --proxy URL
      --insecure-tls
      --trace-http

  General:
      --quiet
  -v, --verbose                 Print TTFT and token/s metrics to stderr.
      --debug                   Print configuration diagnostics to stderr.
      --no-config               Skip the automatic user config; keep system config.
      --version
  -h, --help
)";
}

}  // namespace ainiux::cli
