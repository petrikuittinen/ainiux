#include "cli/args.hpp"

#include <cstdlib>
#include <sstream>

namespace pkchat::cli {

namespace {

bool needs_value(const std::string& opt) {
    static const char* with_values[] = {
        "-p", "--prompt", "--prompt-file", "-s", "--system", "--system-file", "-m", "--model",
        "-t", "--temperature", "--top-p", "--max-output-tokens", "--format", "--output",
        "--provider", "--profile", "--base-url", "--chat-url", "--models-url", "--responses-url",
        "--key-env", "--key-file", "-k", "--key", "--header", "--connect-timeout", "--timeout",
        "--proxy", "--save-chat", "--load-chat"};
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
            } else if (opt == "-m" || opt == "--model") {
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
                } else if (value == "ndjson") {
                    opts.format = OutputFormat::Ndjson;
                } else {
                    return {opts, {ErrorCode::BadArgs, "--format must be text, json, or ndjson"}};
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
  pkchat [BASE_URL] -p TEXT [options]
  pkchat --list-models [BASE_URL] [options]
  pkchat --repl [BASE_URL] [options]

Examples:
  pkchat http://localhost:8000 -p "What is the capital of Norway?"
  pkchat --base-url http://localhost:8000/v1 -p "Hello"
  pkchat --provider openai -m MODEL -p "Hello"
  pkchat --provider lm_studio -m MODEL -p "Hello from local LM Studio"
  pkchat --provider lmstudio --list-models
  pkchat --prompt-file prompt.txt --system-file system.txt --format json
  pkchat --repl --load-chat chat.json --save-chat chat.json

Options:
  -p, --prompt TEXT
      --prompt-file PATH        Use '-' to read the prompt from stdin.
  -s, --system TEXT
      --system-file PATH
  -m, --model MODEL
  -t, --temperature FLOAT
      --top-p FLOAT
      --max-output-tokens N
      --stream | --no-stream
      --format text|json|ndjson
      --output PATH
      --repl, -i                Start a simple line-oriented interactive chat.
      --save-chat PATH          Save JSON chat history after a successful reply.
      --load-chat PATH          Load JSON chat history before sending.
      --provider NAME           openai, openrouter, custom_openai_chat, lm_studio
      --profile NAME            Alias for --provider.
      --base-url URL
      --chat-url URL
      --models-url URL
      --responses-url URL       Reserved for later Responses API support.
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
