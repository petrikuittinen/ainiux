#include "cli/args.hpp"

#include <cstdlib>
#include <sstream>

namespace pkchat::cli {

namespace {

bool needs_value(const std::string& opt) {
    static const char* with_values[] = {
        "-p", "--prompt", "--prompt-file", "-s", "--system", "--system-file", "-m", "--model", "-model",
        "-t", "--temperature", "--top-p", "--max-output-tokens", "--format", "--output-format", "--output",
        "--provider", "--profile", "--api", "--base-url", "--chat-url", "--models-url", "--responses-url",
        "--key-env", "--key-file", "-k", "--key", "--header", "--connect-timeout", "--timeout",
        "--proxy", "--fetch-url", "--input", "--attach", "--html-file", "--html-format",
        "--max-fetch-bytes", "--max-input-bytes", "--max-image-bytes", "--max-context-bytes",
        "--context-policy", "--image-capability",
        "--save-chat", "--load-chat"};
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
    out = static_cast<int>(value);
    return ok_error();
}

}  // namespace

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

ParseResult parse_args(int argc, char** argv) {
    Options opts;
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
        } else if (arg == "--version") {
            opts.version = true;
        } else if (arg == "--list-models") {
            opts.list_models = true;
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
        } else if (arg == "--trace-http") {
            opts.trace_http = true;
        } else if (arg == "--insecure-tls") {
            opts.insecure_tls = true;
        } else if (arg == "--key-stdin") {
            opts.key_stdin = true;
        } else if (arg == "--repl" || arg == "-i") {
            opts.repl = true;
        } else if (arg == "--chat" || arg == "--tui") {
            opts.tui = true;
        } else if (arg == "--nocolors" || arg == "--no-colors") {
            opts.no_colors = true;
        } else if (arg == "--allow-private-url-fetch") {
            opts.allow_private_url_fetch = true;
        } else if (arg == "--editor") {
            opts.editor = true;
        } else if (needs_value(opt)) {
            if (auto err = take_value()) {
                return {opts, *err};
            }
            if (opt == "-p" || opt == "--prompt") {
                opts.prompt = value;
            } else if (opt == "--prompt-file") {
                opts.prompt_file = value;
            } else if (opt == "-s" || opt == "--system") {
                opts.system = value;
            } else if (opt == "--system-file") {
                opts.system_file = value;
            } else if (opt == "-m" || opt == "--model" || opt == "-model") {
                opts.model = value;
            } else if (opt == "-t" || opt == "--temperature") {
                Error err = parse_double(opt, value, opts.temperature);
                if (!err.ok()) {
                    return {opts, err};
                }
                opts.has_temperature = true;
            } else if (opt == "--top-p") {
                Error err = parse_double(opt, value, opts.top_p);
                if (!err.ok()) {
                    return {opts, err};
                }
                opts.has_top_p = true;
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
                } else if (value == "ndjson" || value == "jsond") {
                    opts.format = OutputFormat::Ndjson;
                } else {
                    return {opts, {ErrorCode::BadArgs, "--format must be text, json, ndjson, or jsond"}};
                }
            } else if (opt == "--output-format") {
                if (value == "json") {
                    opts.format = OutputFormat::Json;
                    opts.output_format_explicit = true;
                } else if (value == "ndjson" || value == "jsond") {
                    opts.format = OutputFormat::Ndjson;
                    opts.output_format_explicit = true;
                } else if (pkchat::markdown::parse_output_format(value, opts.output_format)) {
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
            } else if (opt == "--profile") {
                opts.profile = value;
                opts.provider = value;
            } else if (opt == "--api") {
                if (value == "chat" || value == "chat_completions" || value == "chat-completions") {
                    opts.api = "chat";
                } else if (value == "responses" || value == "responses_api" || value == "responses-api") {
                    opts.api = "responses";
                } else {
                    return {opts, {ErrorCode::BadArgs, "--api must be chat or responses"}};
                }
            } else if (opt == "--base-url") {
                opts.base_url = value;
            } else if (opt == "--chat-url") {
                opts.chat_url = value;
            } else if (opt == "--models-url") {
                opts.models_url = value;
            } else if (opt == "--responses-url") {
                opts.responses_url = value;
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
            } else if (opt == "--max-input-bytes") {
                Error err = parse_long(opt, value, opts.max_input_bytes);
                if (!err.ok()) {
                    return {opts, err};
                }
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
            } else if (opt == "--context-policy") {
                if (value != "error" && value != "truncate-oldest" && value != "summarize-oldest" &&
                    value != "summarize-middle" && value != "provider-auto") {
                    return {opts, {ErrorCode::BadArgs,
                                   "--context-policy must be error, truncate-oldest, summarize-oldest, "
                                   "summarize-middle, or provider-auto"}};
                }
                opts.context_policy = value;
            } else if (opt == "--image-capability") {
                if (value != "auto" && value != "allow" && value != "deny") {
                    return {opts, {ErrorCode::BadArgs, "--image-capability must be auto, allow, or deny"}};
                }
                opts.image_capability = value;
            }
        } else if (!arg.empty() && arg[0] == '-') {
            return {opts, {ErrorCode::BadArgs, "unknown option: " + arg}};
        } else {
            if (!opts.positional_url.empty()) {
                return {opts, {ErrorCode::BadArgs, "unexpected extra positional argument: " + arg}};
            }
            opts.positional_url = arg;
        }
    }
    return {opts, ok_error()};
}

std::string help_text() {
    return R"(pkchat - script-friendly OpenAI-compatible chat CLI

Usage:
  pkchat [BASE_URL|PROFILE] -p TEXT [options]
  pkchat --list-models [BASE_URL|PROFILE] [options]
  pkchat --repl [BASE_URL|PROFILE] [options]
  pkchat --chat [BASE_URL|PROFILE] [options]
  pkchat --editor [PATH] [--output PATH]
  pkchat --input PATH [--output-format md|html|plaintext|json|jsond] [--output PATH]
  pkchat --fetch-url URL [--output-format md|html|plaintext|json|jsond] [--output PATH]

Examples:
  pkchat http://localhost:8000 -p "What is the capital of Norway?"
  pkchat --base-url http://localhost:8000/v1 -p "Hello"
  pkchat --provider openai -m MODEL -p "Hello"
  pkchat --provider lm_studio -m MODEL -p "Hello from local LM Studio"
  pkchat --provider lmstudio --list-models
  pkchat --provider none --editor notes.txt
  pkchat --provider none --input page.html --output-format md
  pkchat --provider none --fetch-url https://example.com --output-format md
  pkchat openrouter -model MODEL -i
  pkchat lmstudio -i
  pkchat --chat lmstudio
  pkchat --editor notes.txt
  pkchat --fetch-url https://example.com --output-format md
  pkchat --input page.html --output-format plaintext
  printf 'piped text' | pkchat --input stdin --output stdout
  command | pkchat http://localhost:8000 -p "Summarize" --attach stdin
  pkchat http://localhost:8000 -p "Compare these" --attach one.md --attach two.txt
  pkchat http://localhost:30000 -p "Describe this image" --input photo.png
  pkchat --prompt-file prompt.txt --system-file system.txt --format json
  pkchat http://localhost:8000 -p "Write a report" --output-format html --output report.html
  pkchat --repl --load-chat chat.json --save-chat chat.json

Options:
  -p, --prompt TEXT
      --prompt-file PATH        Use '-' to read the prompt from stdin.
  -s, --system TEXT
      --system-file PATH
  -m, --model, -model MODEL
  -t, --temperature FLOAT
      --top-p FLOAT
      --max-output-tokens N
      --stream | --no-stream
      --format text|json|ndjson|jsond
      --output-format html|md|plaintext|json|jsond|ndjson
      --output PATH             Use 'stdout' to write to standard output.
      --repl, -i                Start a simple line-oriented interactive chat.
      --chat                    Start the full-screen non-blocking terminal chat.
      --nocolors                Disable TUI color styling.
      --editor                  Start the standalone multiline editor.
      --input PATH              Read text/Markdown/HTML, or attach PNG/JPEG/GIF with -p;
                                'stdin' reads UTF-8 plaintext from standard input.
      --attach PATH             Add text/Markdown/HTML or PNG/JPEG/GIF; repeatable;
                                'stdin' reads UTF-8 plaintext from standard input.
      --fetch-url URL           Fetch HTML for extraction, or as prompt context with -p.
      --html-format text|markdown
                                Compatibility alias for old HTML extraction commands.
      --max-fetch-bytes N       Default 1048576.
      --max-input-bytes N       Maximum bytes per text input/attachment; default 1048576.
      --max-image-bytes N       Maximum image file size; default 20971520.
      --max-context-bytes N     Request text budget; 0 disables the client budget.
      --context-policy POLICY   error, truncate-oldest, summarize-oldest,
                                summarize-middle, or provider-auto.
      --image-capability MODE   auto, allow, or deny; allow overrides model detection.
      --allow-private-url-fetch Allow loopback/private URL fetches.
      --save-chat PATH          Save JSON chat history after a successful reply.
      --load-chat PATH          Load JSON chat history before sending.
      --provider NAME           none (offline), openai, openrouter, lm_studio, ollama,
                                vllm, llama.cpp, etc.
      --profile NAME            Alias for --provider.
      --api chat|responses      Use Chat Completions (default) or Responses API.
      --responses               Shortcut for --api responses.
      --base-url URL
      --chat-url URL
      --models-url URL
      --responses-url URL       Override the Responses API endpoint.
      --key-env NAME
      --key-file PATH
      --key-stdin
  -k, --key TEXT                Discouraged; command line keys may be visible locally.
      --header "Name: Value"
      --connect-timeout SECONDS
      --timeout SECONDS
      --proxy URL
      --insecure-tls
      --quiet
  -v, --verbose             Print TTFT and token/s metrics to stderr.
      --debug
      --trace-http
      --version
      --help
)";
}

}  // namespace pkchat::cli
