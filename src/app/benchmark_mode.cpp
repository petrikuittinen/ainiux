#include "app/app.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <utility>
#include <vector>

#include "benchmark/benchmark.hpp"
#include "json/json.hpp"
#include "runtime/interrupt.hpp"

namespace ainiux::app {

namespace {

std::ostream* benchmark_output_stream(const cli::Options& options,
                                      std::ofstream& file,
                                      Error& error,
                                      std::string& actual_path) {
    if (options.output_path.empty() || options.output_path == "stdout") {
        return &std::cout;
    }
    std::filesystem::path path = std::filesystem::u8path(options.output_path);
    std::error_code filesystem_error;
    const bool trailing_separator =
        options.output_path.back() == '/' || options.output_path.back() == '\\';
    const bool path_exists = std::filesystem::exists(path, filesystem_error);
    if (filesystem_error) {
        error = {ErrorCode::FileWrite,
                 "could not inspect benchmark output path " + options.output_path + ": " +
                     filesystem_error.message()};
        return nullptr;
    }
    const bool existing_directory =
        path_exists && std::filesystem::is_directory(path, filesystem_error);
    if (filesystem_error) {
        error = {ErrorCode::FileWrite,
                 "could not inspect benchmark output path " + options.output_path + ": " +
                     filesystem_error.message()};
        return nullptr;
    }
    if (trailing_separator || existing_directory) {
        std::filesystem::create_directories(path, filesystem_error);
        if (filesystem_error) {
            error = {ErrorCode::FileWrite,
                     "could not create benchmark output directory " + options.output_path + ": " +
                         filesystem_error.message()};
            return nullptr;
        }
        const long long timestamp = std::chrono::duration_cast<std::chrono::seconds>(
                                        std::chrono::system_clock::now().time_since_epoch())
                                        .count();
        std::filesystem::path candidate =
            path / ("benchmark-" + std::to_string(timestamp) + ".jsonl");
        size_t suffix = 1;
        while (!filesystem_error &&
               (std::filesystem::exists(candidate, filesystem_error) ||
                (!filesystem_error &&
                 std::filesystem::exists(
                     std::filesystem::u8path(
                         benchmark::markdown_report_path(candidate.u8string())),
                                         filesystem_error)))) {
            candidate = path / ("benchmark-" + std::to_string(timestamp) + "-" +
                                std::to_string(suffix++) + ".jsonl");
        }
        if (filesystem_error) {
            error = {ErrorCode::FileWrite,
                     "could not select a benchmark result file in " + options.output_path + ": " +
                         filesystem_error.message()};
            return nullptr;
        }
        path = std::move(candidate);
    }
    actual_path = path.u8string();
    file.open(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        error = {ErrorCode::FileWrite,
                 "could not open benchmark output file for writing: " + actual_path};
        return nullptr;
    }
    return &file;
}

Error write_benchmark_markdown(std::ofstream& jsonl_file,
                               const std::string& jsonl_path,
                               std::string& markdown_path) {
    if (jsonl_path.empty()) {
        return ok_error();
    }
    jsonl_file.flush();
    if (!jsonl_file) {
        return {ErrorCode::FileWrite,
                "could not flush benchmark JSONL output before creating Markdown report: " +
                    jsonl_path};
    }
    jsonl_file.close();
    if (jsonl_file.fail()) {
        return {ErrorCode::FileWrite,
                "could not close benchmark JSONL output before creating Markdown report: " +
                    jsonl_path};
    }
    markdown_path = benchmark::markdown_report_path(jsonl_path);
    return benchmark::write_markdown_report(jsonl_path, markdown_path);
}

}  // namespace

int run_benchmark_mode(const cli::Options& options) {
    if (options.benchmark_dataset.empty()) {
        print_error({ErrorCode::BadArgs, "--dataset requires a non-empty path"});
        return exit_code_for(ErrorCode::BadArgs);
    }
    if (options.format != cli::OutputFormat::Ndjson) {
        print_error({ErrorCode::BadArgs,
                     "benchmark mode currently writes JSONL; use --format jsonl or omit --format"});
        return exit_code_for(ErrorCode::BadArgs);
    }
    if (options.benchmark_validate && options.benchmark_list) {
        print_error({ErrorCode::BadArgs, "--validate-dataset and --list-cases cannot be combined"});
        return exit_code_for(ErrorCode::BadArgs);
    }
    if (options.grade_input_explicit) {
        print_error({ErrorCode::BadArgs,
                     "--grade-input can only be used with --grade"});
        return exit_code_for(ErrorCode::BadArgs);
    }
    if (!options.prompt.empty() || !options.prompt_file.empty() || !options.system.empty() ||
        !options.system_file.empty() || options.repl || options.tui || options.editor ||
        options.list_models || has_document_source(options) || !options.attachment_paths.empty()) {
        print_error({ErrorCode::BadArgs,
                     "benchmark cannot be combined with chat prompts, interactive modes, input attachments, "
                     "or --list-models"});
        return exit_code_for(ErrorCode::BadArgs);
    }
    if (!options.key.empty() && !options.quiet) {
        std::cerr << "Warning: command line API keys may be visible to other local users; "
                     "prefer --key-env, --key-file, or --key-stdin.\n";
    }
    if (options.insecure_tls) {
        std::cerr << "Warning: TLS certificate verification is disabled by the effective configuration.\n";
    }

    benchmark::LoadResult loaded = benchmark::load_jsonl(options.benchmark_dataset);
    if (!loaded.error.ok()) {
        print_error(loaded.error);
        return exit_code_for(loaded.error.code);
    }
    const size_t limit =
        options.benchmark_limit > 0 ? static_cast<size_t>(options.benchmark_limit) : 0U;
    const std::vector<const benchmark::Case*> selected =
        benchmark::select_cases(loaded.dataset, options.benchmark_category, options.benchmark_case, limit);
    if (selected.empty()) {
        print_error({ErrorCode::BadArgs,
                     "benchmark selection matched no cases in " + options.benchmark_dataset});
        return exit_code_for(ErrorCode::BadArgs);
    }

    std::ofstream out_file;
    Error output_error;
    std::string actual_output_path;
    std::ostream* out = benchmark_output_stream(options, out_file, output_error, actual_output_path);
    if (!output_error.ok()) {
        print_error(output_error);
        return exit_code_for(output_error.code);
    }
    if (!actual_output_path.empty() && !options.quiet) {
        std::cerr << "Benchmark results: " << actual_output_path << "\n";
    }
    auto finish_file_outputs = [&]() -> Error {
        std::string markdown_path;
        Error report_error = write_benchmark_markdown(out_file, actual_output_path, markdown_path);
        if (report_error.ok() && !markdown_path.empty() && !options.quiet) {
            std::cerr << "Benchmark Markdown report: " << markdown_path << "\n";
        }
        return report_error;
    };
    if (options.benchmark_validate) {
        *out << "{\"type\":\"dataset\",\"path\":" << json::quote(options.benchmark_dataset)
             << ",\"total_cases\":" << loaded.dataset.cases.size()
             << ",\"selected_cases\":" << selected.size() << ",\"valid\":true}\n";
        Error report_error = finish_file_outputs();
        if (!report_error.ok()) {
            print_error(report_error);
            return exit_code_for(report_error.code);
        }
        return 0;
    }
    if (options.benchmark_list) {
        for (const benchmark::Case* benchmark_case : selected) {
            benchmark::write_case_json(*out, *benchmark_case);
        }
        Error report_error = finish_file_outputs();
        if (!report_error.ok()) {
            print_error(report_error);
            return exit_code_for(report_error.code);
        }
        return 0;
    }

    provider::ContextResult context_result = provider::build_context(options);
    if (!context_result.error.ok()) {
        print_error(context_result.error);
        return exit_code_for(context_result.error.code);
    }
    runtime::InterruptGuard interrupt_guard;
    if (!interrupt_guard.installed()) {
        print_error({ErrorCode::Internal, "could not install the benchmark Ctrl+C signal handler"});
        return exit_code_for(ErrorCode::Internal);
    }
    Error err = benchmark::run(context_result.context, selected, options, *out, std::cerr,
                               [&interrupt_guard] { return interrupt_guard.interrupted(); });
    Error report_error = finish_file_outputs();
    if (!report_error.ok()) {
        print_error(report_error);
        if (err.ok()) {
            return exit_code_for(report_error.code);
        }
    }
    if (!err.ok()) {
        print_error(err);
        return exit_code_for(err.code);
    }
    return 0;
}

}  // namespace ainiux::app
