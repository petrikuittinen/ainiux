#include <fstream>
#include <iostream>
#include <utility>
#include <vector>

#include "app/app.hpp"
#include "chat/session.hpp"
#include "cli/args.hpp"
#include "config/config.hpp"
#include "editor/ai_continue.hpp"
#include "editor/editor.hpp"
#include "input/input.hpp"
#include "json/json.hpp"
#include "pkchat/version.hpp"
#include "provider/provider.hpp"
#include "search/search.hpp"
#include "tui/tui.hpp"

namespace {

using InputKind = pkchat::input::Kind;

}  // namespace

int main(int argc, char** argv) {
    pkchat::cli::ParseResult parsed = pkchat::cli::parse_args(argc, argv);
    if (!parsed.error.ok()) {
        pkchat::app::print_error(parsed.error);
        return pkchat::app::exit_code_for(parsed.error.code);
    }
    if (parsed.options.help) {
        std::cout << pkchat::cli::help_text();
        return 0;
    }
    if (parsed.options.version) {
        std::cout << "pkchat " << pkchat::kVersion << "\n";
        return 0;
    }
    pkchat::config::LoadResult configured = pkchat::config::load_automatic(
        pkchat::cli::Options{}, pkchat::config::process_environment(), !parsed.options.no_config);
    if (parsed.options.debug && !parsed.options.quiet) {
        pkchat::app::print_config_diagnostics(configured);
    }
    if (!configured.error.ok()) {
        pkchat::app::print_error(configured.error);
        return pkchat::app::exit_code_for(configured.error.code);
    }
    parsed = pkchat::cli::parse_args(argc, argv, configured.options);
    if (!parsed.error.ok()) {
        pkchat::app::print_error(parsed.error);
        return pkchat::app::exit_code_for(parsed.error.code);
    }
    pkchat::cli::Options options = parsed.options;
    if (options.benchmark) {
        return pkchat::app::run_benchmark_mode(options);
    }
    if (options.benchmark_options_seen) {
        pkchat::app::print_error({pkchat::ErrorCode::BadArgs,
                                  "benchmark dataset options require the 'benchmark' subcommand"});
        return pkchat::app::exit_code_for(pkchat::ErrorCode::BadArgs);
    }
    pkchat::Error profile_error = pkchat::provider::validate_profile_name(options.provider);
    if (!profile_error.ok()) {
        pkchat::app::print_error(profile_error);
        return pkchat::app::exit_code_for(profile_error.code);
    }
    pkchat::Error stdin_error = pkchat::app::validate_stdin_sources(options);
    if (!stdin_error.ok()) {
        pkchat::app::print_error(stdin_error);
        return pkchat::app::exit_code_for(stdin_error.code);
    }
    if (!options.attachment_paths.empty()) {
        for (const std::string& path : options.attachment_paths) {
            if (path.empty()) {
                pkchat::app::print_error({pkchat::ErrorCode::BadArgs, "--attach requires a non-empty path"});
                return pkchat::app::exit_code_for(pkchat::ErrorCode::BadArgs);
            }
        }
        if (options.prompt.empty() && options.prompt_file.empty()) {
            pkchat::app::print_error({pkchat::ErrorCode::BadArgs,
                                      "--attach requires -p/--prompt or --prompt-file in non-interactive mode"});
            return pkchat::app::exit_code_for(pkchat::ErrorCode::BadArgs);
        }
        if (options.editor || options.repl || options.tui || options.list_models) {
            pkchat::app::print_error(
                {pkchat::ErrorCode::BadArgs,
                 "--attach currently supports non-interactive prompt mode only; use /insert in the REPL or TUI"});
            return pkchat::app::exit_code_for(pkchat::ErrorCode::BadArgs);
        }
    }
    if (pkchat::app::has_document_source(options) &&
        !pkchat::app::wants_document_prompt_context(options)) {
        std::ofstream out_file;
        pkchat::Error output_error;
        std::ostream* out = pkchat::app::output_stream(options, out_file, output_error);
        if (!output_error.ok()) {
            pkchat::app::print_error(output_error);
            return pkchat::app::exit_code_for(output_error.code);
        }
        return pkchat::app::run_document_extract(options, *out);
    }
    if (pkchat::app::has_search_source(options) && !pkchat::app::wants_search_prompt_context(options)) {
        std::ofstream out_file;
        pkchat::Error output_error;
        std::ostream* out = pkchat::app::output_stream(options, out_file, output_error);
        if (!output_error.ok()) {
            pkchat::app::print_error(output_error);
            return pkchat::app::exit_code_for(output_error.code);
        }
        return pkchat::app::run_search_extract(options, *out);
    }
    if (pkchat::app::wants_document_prompt_context(options) &&
        (options.editor || options.repl || options.tui || options.list_models)) {
        pkchat::app::print_error({pkchat::ErrorCode::BadArgs,
                                  "--fetch-url/--input prompt context currently supports non-interactive "
                                  "prompt mode only"});
        return pkchat::app::exit_code_for(pkchat::ErrorCode::BadArgs);
    }
    if (pkchat::app::wants_search_prompt_context(options) &&
        (options.editor || options.repl || options.tui || options.list_models)) {
        pkchat::app::print_error({pkchat::ErrorCode::BadArgs,
                                  "--search prompt context currently supports non-interactive prompt mode only"});
        return pkchat::app::exit_code_for(pkchat::ErrorCode::BadArgs);
    }
    if (options.rendered_output_format_explicit && options.format != pkchat::cli::OutputFormat::Text) {
        pkchat::app::print_error(
            {pkchat::ErrorCode::BadArgs, "--output-format can only be combined with --format text"});
        return pkchat::app::exit_code_for(pkchat::ErrorCode::BadArgs);
    }
    if (options.rendered_output_format_explicit && options.list_models) {
        pkchat::app::print_error(
            {pkchat::ErrorCode::BadArgs, "rendered --output-format cannot be combined with --list-models"});
        return pkchat::app::exit_code_for(pkchat::ErrorCode::BadArgs);
    }
    if (options.editor && (options.repl || options.tui)) {
        pkchat::app::print_error({pkchat::ErrorCode::BadArgs, "--editor cannot be combined with --repl or --chat"});
        return pkchat::app::exit_code_for(pkchat::ErrorCode::BadArgs);
    }
    if (options.repl && options.tui) {
        pkchat::app::print_error({pkchat::ErrorCode::BadArgs, "--repl cannot be combined with --chat"});
        return pkchat::app::exit_code_for(pkchat::ErrorCode::BadArgs);
    }
    if (options.repl && options.list_models) {
        pkchat::app::print_error({pkchat::ErrorCode::BadArgs, "--repl cannot be combined with --list-models"});
        return pkchat::app::exit_code_for(pkchat::ErrorCode::BadArgs);
    }
    if (options.tui && options.list_models) {
        pkchat::app::print_error({pkchat::ErrorCode::BadArgs,
                                  "--chat cannot be combined with --list-models; use /models inside the chat UI"});
        return pkchat::app::exit_code_for(pkchat::ErrorCode::BadArgs);
    }
    if (options.editor && options.list_models) {
        pkchat::app::print_error({pkchat::ErrorCode::BadArgs, "--editor cannot be combined with --list-models"});
        return pkchat::app::exit_code_for(pkchat::ErrorCode::BadArgs);
    }
    if (options.repl && options.format != pkchat::cli::OutputFormat::Text) {
        pkchat::app::print_error({pkchat::ErrorCode::BadArgs, "--repl currently supports --format text only"});
        return pkchat::app::exit_code_for(pkchat::ErrorCode::BadArgs);
    }
    if (options.tui && options.format != pkchat::cli::OutputFormat::Text) {
        pkchat::app::print_error({pkchat::ErrorCode::BadArgs, "--chat currently supports --format text only"});
        return pkchat::app::exit_code_for(pkchat::ErrorCode::BadArgs);
    }
    if (options.tui && !options.output_path.empty()) {
        pkchat::app::print_error({pkchat::ErrorCode::BadArgs, "--chat cannot be combined with --output"});
        return pkchat::app::exit_code_for(pkchat::ErrorCode::BadArgs);
    }
    if (options.tui && options.rendered_output_format_explicit) {
        pkchat::app::print_error({pkchat::ErrorCode::BadArgs, "--chat does not use rendered --output-format"});
        return pkchat::app::exit_code_for(pkchat::ErrorCode::BadArgs);
    }
    if (options.editor && options.format != pkchat::cli::OutputFormat::Text) {
        pkchat::app::print_error({pkchat::ErrorCode::BadArgs, "--editor does not use --format"});
        return pkchat::app::exit_code_for(pkchat::ErrorCode::BadArgs);
    }
    if (options.editor && options.rendered_output_format_explicit) {
        pkchat::app::print_error({pkchat::ErrorCode::BadArgs, "--editor does not use rendered --output-format"});
        return pkchat::app::exit_code_for(pkchat::ErrorCode::BadArgs);
    }
    if (options.editor && (!options.prompt.empty() || !options.prompt_file.empty())) {
        pkchat::app::print_error({pkchat::ErrorCode::BadArgs, "--editor cannot be combined with prompt options"});
        return pkchat::app::exit_code_for(pkchat::ErrorCode::BadArgs);
    }
    if (options.editor && (!options.load_chat_path.empty() || !options.save_chat_path.empty())) {
        pkchat::app::print_error(
            {pkchat::ErrorCode::BadArgs, "--editor cannot be combined with --load-chat or --save-chat"});
        return pkchat::app::exit_code_for(pkchat::ErrorCode::BadArgs);
    }
    if (options.editor) {
        pkchat::editor::EditorSettings editor_settings;
        editor_settings.undo_limit = static_cast<size_t>(options.editor_undo_limit);
        editor_settings.huge_file_size_warning = options.editor_huge_file_size_warning;
        editor_settings.file_size_limit = options.editor_file_size_limit;
        pkchat::provider::apply_editor_offline_default(options);
        pkchat::provider::ContextResult context_result = pkchat::provider::build_context(options);
        if (!context_result.error.ok()) {
            pkchat::app::print_error(context_result.error);
            return pkchat::app::exit_code_for(context_result.error.code);
        }
        pkchat::provider::RequestContext editor_context = std::move(context_result.context);
        pkchat::editor::AiContinueContext ai_continue;
        const pkchat::editor::AiContinueContext* ai_continue_ptr = nullptr;
        if (!editor_context.profile.offline) {
            ai_continue.request = std::move(editor_context);
            ai_continue.settings = pkchat::editor::ai_continue_settings_from_env();
            ai_continue.assist_config = options.editor_assist_config;
            const bool auto_select_model = options.model.empty();
            pkchat::Error model_err = pkchat::editor::resolve_editor_default_model(ai_continue);
            if (!model_err.ok()) {
                pkchat::app::print_error(model_err);
                return pkchat::app::exit_code_for(model_err.code);
            }
            if (auto_select_model && !options.quiet) {
                std::cerr << "Editor model: " << ai_continue.request.options.model
                          << " (first model from " << ai_continue.request.models_url << ")\n";
            }
            ai_continue_ptr = &ai_continue;
        }
        return pkchat::editor::run_editor(options.editor_path, options.output_path, editor_settings,
                                          ai_continue_ptr);
    }
    if (!options.key.empty() && !options.quiet) {
        std::cerr << "Warning: command line API keys may be visible to other local users; prefer --key-env, "
                     "--key-file, or --key-stdin.\n";
    }
    if (options.insecure_tls) {
        std::cerr << "Warning: TLS certificate verification is disabled by the effective configuration.\n";
    }

    pkchat::chat::Session session;
    bool loaded_session = false;
    if (!options.load_chat_path.empty()) {
        pkchat::Error err = pkchat::chat::load_session(options.load_chat_path, session);
        if (!err.ok()) {
            pkchat::app::print_error(err);
            return pkchat::app::exit_code_for(err.code);
        }
        loaded_session = true;
        if (options.model.empty()) {
            options.model = session.model;
        }
        if (options.base_url.empty() && options.positional_url.empty() && options.chat_url.empty()) {
            options.base_url = session.base_url;
        }
        if (options.provider == "openai" && options.positional_url.empty() &&
            options.base_url == session.base_url) {
            options.provider = session.provider;
        }
    }

    pkchat::provider::apply_tui_startup_default(options);
    pkchat::provider::ContextResult context_result = pkchat::provider::build_context(options);
    if (!context_result.error.ok()) {
        pkchat::app::print_error(context_result.error);
        return pkchat::app::exit_code_for(context_result.error.code);
    }
    pkchat::provider::RequestContext context = context_result.context;

    std::ofstream out_file;
    pkchat::Error output_error;
    std::ostream* out = pkchat::app::output_stream(context.options, out_file, output_error);
    if (!output_error.ok()) {
        pkchat::app::print_error(output_error);
        return pkchat::app::exit_code_for(output_error.code);
    }

    std::string fetched_context_message;
    std::vector<std::string> attachment_context_messages;
    std::vector<pkchat::provider::ImageInput> prompt_images;
    std::vector<pkchat::input::FileType> attachment_types;
    attachment_types.reserve(context.options.attachment_paths.size());
    bool image_requested = false;
    if (pkchat::app::wants_document_prompt_context(context.options) &&
        !context.options.fetch_url.empty()) {
        image_requested = false;
    } else if (pkchat::app::wants_document_prompt_context(context.options)) {
        pkchat::input::FileType type;
        pkchat::Error err = pkchat::app::local_input_type_for_options(context.options, type);
        if (!err.ok()) {
            pkchat::app::print_error(err);
            return pkchat::app::exit_code_for(err.code);
        }
        image_requested = type.kind == InputKind::Image;
    }
    for (const std::string& path : context.options.attachment_paths) {
        pkchat::input::FileType type;
        pkchat::Error err = pkchat::input::classify_file_type(path, type);
        if (!err.ok()) {
            pkchat::app::print_error(err);
            return pkchat::app::exit_code_for(err.code);
        }
        image_requested = image_requested || type.kind == InputKind::Image;
        attachment_types.push_back(std::move(type));
    }
    bool model_chosen = false;
    if (image_requested) {
        pkchat::Error model_err = pkchat::app::choose_default_model(context);
        if (!model_err.ok()) {
            pkchat::app::print_error(model_err);
            return pkchat::app::exit_code_for(model_err.code);
        }
        model_chosen = true;
        pkchat::Error capability_error = pkchat::provider::validate_image_input(context);
        if (!capability_error.ok()) {
            pkchat::app::print_error(capability_error);
            return pkchat::app::exit_code_for(capability_error.code);
        }
    }
    if (pkchat::app::wants_document_prompt_context(context.options)) {
        pkchat::app::LoadedDocument document;
        pkchat::Error err = pkchat::app::load_document(context.options, false, document);
        if (!err.ok()) {
            pkchat::app::print_error(err);
            return pkchat::app::exit_code_for(err.code);
        }
        if (document.input_kind == InputKind::Image) {
            prompt_images.push_back(std::move(document.image));
        } else {
            fetched_context_message = pkchat::app::document_context_message(document);
        }
    }
    attachment_context_messages.reserve(context.options.attachment_paths.size());
    for (size_t attachment_index = 0; attachment_index < context.options.attachment_paths.size();
         ++attachment_index) {
        const std::string& path = context.options.attachment_paths[attachment_index];
        const pkchat::input::FileType& type = attachment_types[attachment_index];
        pkchat::Error err;
        if (type.kind == InputKind::Image) {
            if (context.options.max_image_bytes <= 0) {
                err = {pkchat::ErrorCode::BadArgs, "--max-image-bytes must be greater than zero"};
            } else {
                pkchat::input::ImageData image;
                err = pkchat::input::load_image_file(path, type,
                                                     static_cast<size_t>(context.options.max_image_bytes), image);
                if (err.ok()) {
                    prompt_images.push_back({image.mime_type, std::move(image.base64_data)});
                    if (!context.options.quiet) {
                        std::cerr << "Attached image: " << path << " (" << image.mime_type << ", "
                                  << image.byte_size << " bytes)\n";
                    }
                }
            }
        } else {
            pkchat::app::LoadedDocument document;
            err = pkchat::app::load_text_context_file(context.options, path, "--attach", document);
            if (err.ok()) {
                attachment_context_messages.push_back(pkchat::app::document_context_message(document));
                if (!context.options.quiet) {
                    std::cerr << "Attached context: " << path << "\n";
                }
            }
        }
        if (!err.ok()) {
            pkchat::app::print_error(err);
            return pkchat::app::exit_code_for(err.code);
        }
    }

    if (context.options.list_models) {
        pkchat::provider::ModelsResult models;
        pkchat::Error err = pkchat::provider::list_models(context, models);
        if (!err.ok()) {
            pkchat::app::print_error(err);
            return pkchat::app::exit_code_for(err.code);
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
            *out << pkchat::provider::format_models_markdown(context.profile.name, context.models_url, models);
        }
        return 0;
    }

    if (!loaded_session) {
        session = pkchat::chat::new_session(context);
    }
    const bool defer_tui_model_selection =
        context.options.tui && context.options.model.empty() && !context.profile.offline;
    if (!model_chosen && !defer_tui_model_selection) {
        pkchat::Error model_err = pkchat::app::choose_default_model(context);
        if (!model_err.ok()) {
            pkchat::app::print_error(model_err);
            return pkchat::app::exit_code_for(model_err.code);
        }
    }
    pkchat::app::refresh_session_metadata(session, context);
    pkchat::app::apply_system_prompt(session, context.options.system);

    if (context.options.tui) {
        return pkchat::tui::run(context, std::move(session));
    }

    pkchat::app::print_chat_start(context);

    if (context.options.repl) {
        return pkchat::app::run_repl(context, std::move(session), *out);
    }

    if (!fetched_context_message.empty()) {
        session.messages.push_back({"user", fetched_context_message});
    }
    if (pkchat::app::wants_search_prompt_context(context.options)) {
        pkchat::search::SearchResponse search_response;
        pkchat::Error search_err =
            pkchat::search::search(context.options.search_query, pkchat::search::options_for(context.options),
                                   search_response);
        if (!search_err.ok()) {
            pkchat::app::print_error(search_err);
            return pkchat::app::exit_code_for(search_err.code);
        }
        if (!context.options.quiet) {
            std::cerr << "Web search provider: " << search_response.provider_used << " ("
                      << search_response.results.size() << " results)\n";
        }
        session.messages.push_back(
            {"user", pkchat::app::search_context_message(context.options, search_response)});
    }
    for (std::string& message : attachment_context_messages) {
        session.messages.push_back({"user", std::move(message)});
    }

    pkchat::provider::ChatResult chat;
    pkchat::Error err = pkchat::app::send_session_turn(context, session, context.options.prompt, *out, chat,
                                                      std::move(prompt_images), true);
    if (!err.ok()) {
        pkchat::app::print_error(err);
        return pkchat::app::exit_code_for(err.code);
    }
    err = pkchat::app::save_if_requested(context.options, session);
    if (!err.ok()) {
        pkchat::app::print_error(err);
        return pkchat::app::exit_code_for(err.code);
    }
    pkchat::app::print_verbose_metrics(context.options, chat);
    return 0;
}