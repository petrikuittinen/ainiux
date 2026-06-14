#include <fstream>
#include <iostream>
#include <memory>

#include "cli/args.hpp"
#include "common.hpp"
#include "json/json.hpp"
#include "pkchat/version.hpp"
#include "provider/provider.hpp"

namespace {

int exit_code_for(pkchat::ErrorCode code) {
    using pkchat::ErrorCode;
    switch (code) {
        case ErrorCode::Ok:
            return 0;
        case ErrorCode::BadArgs:
        case ErrorCode::BadUrl:
            return 2;
        case ErrorCode::Dns:
        case ErrorCode::Connect:
        case ErrorCode::Tls:
        case ErrorCode::Timeout:
            return 3;
        case ErrorCode::HttpStatus:
        case ErrorCode::Auth:
        case ErrorCode::RateLimit:
        case ErrorCode::JsonParse:
        case ErrorCode::SseParse:
        case ErrorCode::ProviderSchema:
            return 4;
        case ErrorCode::FileRead:
        case ErrorCode::FileWrite:
        case ErrorCode::Config:
            return 5;
        case ErrorCode::UnsupportedFeature:
        case ErrorCode::Internal:
            return 6;
    }
    return 6;
}

void print_error(const pkchat::Error& error) {
    std::cerr << pkchat::error_code_name(error.code) << ": " << error.message << "\n";
}

std::ostream* output_stream(const pkchat::cli::Options& options, std::ofstream& file, pkchat::Error& error) {
    if (options.output_path.empty()) {
        return &std::cout;
    }
    file.open(options.output_path, std::ios::binary | std::ios::trunc);
    if (!file) {
        error = {pkchat::ErrorCode::FileWrite, "could not open output file for writing: " + options.output_path};
        return nullptr;
    }
    return &file;
}

void write_json_chat(std::ostream& out,
                     const pkchat::provider::RequestContext& context,
                     const pkchat::provider::ChatResult& result) {
    out << "{"
        << "\"model\":" << pkchat::json::quote(result.model) << ","
        << "\"provider\":" << pkchat::json::quote(context.profile.name) << ","
        << "\"content\":" << pkchat::json::quote(result.content) << ","
        << "\"usage\":" << result.usage_json << ","
        << "\"timing\":{\"ttft_ms\":null,\"total_ms\":" << result.total_ms << "}"
        << "}\n";
}

}  // namespace

int main(int argc, char** argv) {
    pkchat::cli::ParseResult parsed = pkchat::cli::parse_args(argc, argv);
    if (!parsed.error.ok()) {
        print_error(parsed.error);
        return exit_code_for(parsed.error.code);
    }
    pkchat::cli::Options options = parsed.options;
    if (options.help) {
        std::cout << pkchat::cli::help_text();
        return 0;
    }
    if (options.version) {
        std::cout << "pkchat " << pkchat::kVersion << "\n";
        return 0;
    }
    if (!options.key.empty() && !options.quiet) {
        std::cerr << "Warning: command line API keys may be visible to other local users; prefer --key-env, --key-file, or --key-stdin.\n";
    }

    pkchat::provider::ContextResult context_result = pkchat::provider::build_context(options);
    if (!context_result.error.ok()) {
        print_error(context_result.error);
        return exit_code_for(context_result.error.code);
    }
    const pkchat::provider::RequestContext& context = context_result.context;

    std::ofstream out_file;
    pkchat::Error output_error;
    std::ostream* out = output_stream(context.options, out_file, output_error);
    if (!output_error.ok()) {
        print_error(output_error);
        return exit_code_for(output_error.code);
    }

    if (context.options.list_models) {
        pkchat::provider::ModelsResult models;
        pkchat::Error err = pkchat::provider::list_models(context, models);
        if (!err.ok()) {
            print_error(err);
            return exit_code_for(err.code);
        }
        if (context.options.format == pkchat::cli::OutputFormat::Json) {
            *out << "{\"provider\":" << pkchat::json::quote(context.profile.name) << ",\"models\":[";
            for (size_t i = 0; i < models.model_ids.size(); ++i) {
                if (i != 0) {
                    *out << ",";
                }
                *out << pkchat::json::quote(models.model_ids[i]);
            }
            *out << "]}\n";
        } else {
            for (const std::string& id : models.model_ids) {
                *out << id << "\n";
            }
        }
        return 0;
    }

    pkchat::provider::ChatResult chat;
    bool started_ndjson = false;
    auto on_delta = [&](const std::string& delta) -> pkchat::Error {
        if (context.options.format == pkchat::cli::OutputFormat::Text) {
            *out << delta;
            out->flush();
        } else if (context.options.format == pkchat::cli::OutputFormat::Ndjson) {
            if (!started_ndjson) {
                *out << "{\"event\":\"start\",\"model\":" << pkchat::json::quote(context.options.model) << "}\n";
                started_ndjson = true;
            }
            *out << "{\"event\":\"delta\",\"text\":" << pkchat::json::quote(delta) << "}\n";
            out->flush();
        }
        return pkchat::ok_error();
    };

    pkchat::Error err = pkchat::provider::send_chat(context, on_delta, chat);
    if (!err.ok()) {
        print_error(err);
        return exit_code_for(err.code);
    }
    if (context.options.format == pkchat::cli::OutputFormat::Text) {
        if (!context.options.stream) {
            *out << chat.content;
        }
        *out << "\n";
    } else if (context.options.format == pkchat::cli::OutputFormat::Json) {
        write_json_chat(*out, context, chat);
    } else {
        if (!started_ndjson) {
            *out << "{\"event\":\"start\",\"model\":" << pkchat::json::quote(context.options.model) << "}\n";
        }
        if (!context.options.stream && !chat.content.empty()) {
            *out << "{\"event\":\"delta\",\"text\":" << pkchat::json::quote(chat.content) << "}\n";
        }
        *out << "{\"event\":\"done\",\"usage\":null}\n";
    }
    return 0;
}
