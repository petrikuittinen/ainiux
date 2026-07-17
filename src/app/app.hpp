#pragma once

#include <fstream>
#include <iosfwd>
#include <string>
#include <vector>

#include "chat/session.hpp"
#include "cli/args.hpp"
#include "common.hpp"
#include "config/config.hpp"
#include "fetch/fetch.hpp"
#include "search/search.hpp"
#include "input/input.hpp"
#include "markdown/markdown.hpp"
#include "provider/provider.hpp"

namespace pkchat::app {

int exit_code_for(ErrorCode code);
void print_error(const Error& error);

std::ostream* output_stream(const cli::Options& options, std::ofstream& file, Error& error);

struct LoadedDocument {
    std::string source;
    input::Kind input_kind = input::Kind::Plaintext;
    markdown::OutputFormat output_format = markdown::OutputFormat::Markdown;
    std::string converted;
    provider::ImageInput image;
};

bool has_document_source(const cli::Options& options);
bool has_search_source(const cli::Options& options);
bool wants_document_prompt_context(const cli::Options& options);
bool wants_search_prompt_context(const cli::Options& options);

Error validate_stdin_sources(const cli::Options& options);
Error local_input_type_for_options(const cli::Options& options, input::FileType& type);
Error load_document(const cli::Options& options, bool standalone, LoadedDocument& document);
Error load_text_context_file(const cli::Options& options,
                             const std::string& path,
                             const std::string& option_name,
                             LoadedDocument& document);
std::string document_context_message(const LoadedDocument& document);
fetch::Options fetch_options_for(const cli::Options& options);

int run_document_extract(const cli::Options& options, std::ostream& out);
int run_search_extract(const cli::Options& options, std::ostream& out);
std::string search_context_message(const cli::Options& options, const search::SearchResponse& response);
int run_benchmark_mode(const cli::Options& options);
int run_grade_mode(const cli::Options& options);

void refresh_session_metadata(chat::Session& session, const provider::RequestContext& context);
void apply_system_prompt(chat::Session& session, const std::string& system);
void replace_system_prompt(chat::Session& session, const std::string& system);
void print_verbose_metrics(provider::RequestContext& context,
                           const provider::ChatResult& result,
                           const std::vector<provider::Message>& messages = {});
Error save_if_requested(const cli::Options& options, const chat::Session& session);
Error choose_default_model(provider::RequestContext& context);
void print_chat_start(const provider::RequestContext& context);

Error send_session_turn(provider::RequestContext& context,
                        chat::Session& session,
                        const std::string& prompt,
                        std::ostream& out,
                        provider::ChatResult& chat,
                        std::vector<provider::ImageInput> images = {},
                        bool separate_thinking_traces = false);

int run_repl(provider::RequestContext context, chat::Session session, std::ostream& out);

void print_config_diagnostics(const config::LoadResult& configured);

}  // namespace pkchat::app
