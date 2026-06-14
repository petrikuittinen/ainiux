#pragma once

#include <optional>
#include <string>
#include <vector>

#include "common.hpp"

namespace pkchat::cli {

enum class OutputFormat { Text, Json, Ndjson };

struct Options {
    bool help = false;
    bool version = false;
    bool list_models = false;
    bool stream = true;
    bool stream_explicit = false;
    bool quiet = false;
    bool debug = false;
    bool trace_http = false;
    bool insecure_tls = false;
    bool key_stdin = false;

    std::string positional_url;
    std::string prompt;
    std::string prompt_file;
    std::string system;
    std::string system_file;
    std::string model;
    std::string provider = "openai";
    std::string profile;
    std::string base_url;
    std::string chat_url;
    std::string models_url;
    std::string responses_url;
    std::string key_env;
    std::string key_file;
    std::string key;
    std::string output_path;
    std::string proxy;
    OutputFormat format = OutputFormat::Text;

    double temperature = 0.0;
    bool has_temperature = false;
    double top_p = 0.0;
    bool has_top_p = false;
    int max_output_tokens = 0;
    bool has_max_output_tokens = false;
    long connect_timeout_seconds = 10;
    long timeout_seconds = 0;

    std::vector<std::string> headers;
};

struct ParseResult {
    Options options;
    Error error;
};

ParseResult parse_args(int argc, char** argv);
std::string help_text();
const char* format_name(OutputFormat format);

}  // namespace pkchat::cli
