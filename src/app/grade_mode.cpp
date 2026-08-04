#include "app/app.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <utility>

#include "benchmark/benchmark.hpp"
#include "runtime/interrupt.hpp"

namespace ainiux::app {
namespace {

std::ostream* grade_output_stream(const cli::Options& options,
                                  const std::string& source_path,
                                  std::ofstream& file,
                                  Error& error,
                                  std::string& actual_path) {
    if (options.output_path.empty() || options.output_path == "stdout") {
        return &std::cout;
    }
    std::filesystem::path path = std::filesystem::u8path(options.output_path);
    std::error_code filesystem_error;
    const bool trailing_separator = options.output_path.back() == '/' ||
                                    options.output_path.back() == '\\';
    const bool path_exists = std::filesystem::exists(path, filesystem_error);
    if (filesystem_error) {
        error = {ErrorCode::FileWrite,
                 "could not inspect grading output path " + options.output_path +
                     ": " + filesystem_error.message()};
        return nullptr;
    }
    const bool existing_directory =
        path_exists && std::filesystem::is_directory(path, filesystem_error);
    if (filesystem_error) {
        error = {ErrorCode::FileWrite,
                 "could not inspect grading output path " + options.output_path +
                     ": " + filesystem_error.message()};
        return nullptr;
    }
    if (trailing_separator || existing_directory) {
        std::filesystem::create_directories(path, filesystem_error);
        if (filesystem_error) {
            error = {ErrorCode::FileWrite,
                     "could not create grading output directory " +
                         options.output_path + ": " + filesystem_error.message()};
            return nullptr;
        }
        const long long timestamp =
            std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count();
        const std::string source_stem =
            std::filesystem::u8path(source_path).stem().u8string();
        const std::string base = "grade-" + source_stem + "-" +
                                 std::to_string(timestamp);
        std::filesystem::path candidate = path / (base + ".jsonl");
        size_t suffix = 1;
        while (!filesystem_error &&
               (std::filesystem::exists(candidate, filesystem_error) ||
                (!filesystem_error &&
                 std::filesystem::exists(
                     std::filesystem::u8path(
                         benchmark::markdown_report_path(candidate.u8string())),
                     filesystem_error)))) {
            candidate = path / (base + "-" + std::to_string(suffix++) +
                                ".jsonl");
        }
        if (filesystem_error) {
            error = {ErrorCode::FileWrite,
                     "could not select a grading result file in " +
                         options.output_path + ": " + filesystem_error.message()};
            return nullptr;
        }
        path = std::move(candidate);
    }
    std::string extension = ascii_lower(path.extension().u8string());
    if (extension != ".jsonl") {
        error = {ErrorCode::BadArgs,
                 "explicit grading output must use a .jsonl filename: " +
                     path.u8string()};
        return nullptr;
    }
    std::error_code equivalence_error;
    if (std::filesystem::exists(path, equivalence_error) && !equivalence_error &&
        std::filesystem::equivalent(path, source_path, equivalence_error) &&
        !equivalence_error) {
        error = {ErrorCode::BadArgs,
                 "grading output must not overwrite its benchmark source: " +
                     path.u8string()};
        return nullptr;
    }
    actual_path = path.u8string();
    file.open(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        error = {ErrorCode::FileWrite,
                 "could not open grading output file for writing: " + actual_path};
        return nullptr;
    }
    return &file;
}

Error finish_grade_outputs(std::ofstream& jsonl_file,
                           const std::string& jsonl_path,
                           std::string& markdown_path) {
    if (jsonl_path.empty()) {
        return ok_error();
    }
    jsonl_file.flush();
    if (!jsonl_file) {
        return {ErrorCode::FileWrite,
                "could not flush grading JSONL output before creating Markdown report: " +
                    jsonl_path};
    }
    jsonl_file.close();
    if (jsonl_file.fail()) {
        return {ErrorCode::FileWrite,
                "could not close grading JSONL output before creating Markdown report: " +
                    jsonl_path};
    }
    markdown_path = benchmark::markdown_report_path(jsonl_path);
    return benchmark::write_markdown_report(jsonl_path, markdown_path);
}

Error validate_grade_options(const cli::Options& options) {
    if (options.benchmark) {
        return {ErrorCode::BadArgs, "--benchmark and --grade cannot be combined"};
    }
    if (options.benchmark_dataset_explicit || options.benchmark_mode_explicit ||
        options.benchmark_runs_explicit || options.benchmark_warmup_explicit ||
        options.benchmark_duration_explicit) {
        return {ErrorCode::BadArgs,
                "--grade cannot be combined with --dataset, --mode, --runs, --warmup, or --duration"};
    }
    if (options.benchmark_validate || options.benchmark_list) {
        return {ErrorCode::BadArgs,
                "--grade cannot be combined with --validate-dataset or --list-cases"};
    }
    if (options.format != cli::OutputFormat::Ndjson) {
        return {ErrorCode::BadArgs,
                "grading writes JSONL; use --format jsonl or omit --format"};
    }
    if (!options.prompt.empty() || !options.prompt_file.empty() ||
        !options.system.empty() || !options.system_file.empty() || options.repl ||
        options.tui || options.editor || options.list_models ||
        has_document_source(options) || !options.attachment_paths.empty()) {
        return {ErrorCode::BadArgs,
                "grading cannot be combined with chat prompts, interactive modes, input attachments, or --list-models"};
    }
    if (options.grade_input_explicit && ascii_trim(options.grade_input).empty()) {
        return {ErrorCode::BadArgs, "--grade-input requires a non-empty path"};
    }
    return config::validate_benchmark_grading_prompts(
        options.benchmark_grading_prompts);
}

}  // namespace

int run_grade_mode(const cli::Options& options) {
    Error validation_error = validate_grade_options(options);
    if (!validation_error.ok()) {
        print_error(validation_error);
        return exit_code_for(validation_error.code);
    }
    if (!options.key.empty() && !options.quiet) {
        std::cerr << "Warning: command line API keys may be visible to other local users; "
                     "prefer --key-env, --key-file, or --key-stdin.\n";
    }
    if (options.insecure_tls) {
        std::cerr << "Warning: TLS certificate verification is disabled by the effective configuration.\n";
    }

    std::string source_path;
    Error source_error = benchmark::find_grade_input(options, source_path);
    if (!source_error.ok()) {
        print_error(source_error);
        return exit_code_for(source_error.code);
    }
    if (!options.quiet) {
        std::cerr << "Grading benchmark results: " << source_path << "\n";
    }

    std::ofstream out_file;
    Error output_error;
    std::string actual_output_path;
    std::ostream* out = grade_output_stream(options, source_path, out_file,
                                            output_error, actual_output_path);
    if (!output_error.ok()) {
        print_error(output_error);
        return exit_code_for(output_error.code);
    }
    if (!actual_output_path.empty() && !options.quiet) {
        std::cerr << "Grade results: " << actual_output_path << "\n";
    }

    provider::ContextResult context_result = provider::build_context(options);
    if (!context_result.error.ok()) {
        print_error(context_result.error);
        return exit_code_for(context_result.error.code);
    }
    runtime::InterruptGuard interrupt_guard;
    if (!interrupt_guard.installed()) {
        print_error({ErrorCode::Internal,
                     "could not install the grading Ctrl+C signal handler"});
        return exit_code_for(ErrorCode::Internal);
    }
    Error err = benchmark::grade(
        context_result.context, source_path, options, *out, std::cerr,
        [&interrupt_guard] { return interrupt_guard.interrupted(); });
    std::string markdown_path;
    Error report_error = finish_grade_outputs(out_file, actual_output_path,
                                              markdown_path);
    if (report_error.ok() && !markdown_path.empty() && !options.quiet) {
        std::cerr << "Grade Markdown report: " << markdown_path << "\n";
    }
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
