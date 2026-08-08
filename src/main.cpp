#include <fstream>
#include <iostream>
#include <optional>
#include <utility>
#include <vector>

#include "app/app.hpp"
#include "app/interactive_mode.hpp"
#include "agent/project_settings.hpp"
#include "chat/session.hpp"
#include "chat/sqlite_store.hpp"
#include "cli/args.hpp"
#include "config/config.hpp"
#include "editor/ai_continue.hpp"
#include "editor/editor.hpp"
#include "input/input.hpp"
#include "json/json.hpp"
#include "ainiux/version.hpp"
#include "provider/provider.hpp"
#include "provider/model_selection.hpp"
#include "platform/environment.hpp"
#if defined(_WIN32)
#include "platform/windows_utf.hpp"
#endif
#include "search/search.hpp"
#include "tui/tui.hpp"

namespace {

using InputKind = ainiux::input::Kind;

ainiux::editor::EditorSettings editor_settings_from_options(const ainiux::cli::Options& options) {
    ainiux::editor::EditorSettings settings;
    settings.undo_limit = static_cast<size_t>(options.editor_undo_limit);
    settings.huge_file_size_warning = options.editor_huge_file_size_warning;
    settings.file_size_limit = options.editor_file_size_limit;
    settings.auto_save_mode = options.editor_auto_save_mode;
    settings.auto_save_postfix = options.editor_auto_save_postfix;
    settings.auto_save_threshold = options.editor_auto_save_threshold;
    settings.auto_save_timeout_seconds = options.editor_auto_save_timeout_seconds;
    settings.auto_save_size_limit = options.editor_auto_save_size_limit;
    settings.tab_width = options.editor_tab_width;
    settings.tab_style = options.editor_tab_style;
    settings.linebreak = options.editor_linebreak;
    settings.text_align_width = options.editor_text_align_width;
    settings.themes = &options.tui_themes;
    settings.theme_name = options.tui_theme;
    settings.use_colors = !options.no_colors;
    settings.highlight_enabled = options.tui_highlight;
    settings.start_dired = options.dired;
    settings.start_dired_path = options.dired_path.empty() ? "." : options.dired_path;
    return settings;
}

ainiux::app::InteractiveSession interactive_session_from_editor_startup(
    ainiux::cli::Options& options,
    std::optional<ainiux::editor::AiContinueContext> ai_continue) {
    ainiux::app::InteractiveSession session;
    session.start_mode = ainiux::app::InteractiveMode::Editor;
    session.editor_path = options.editor_path;
    session.editor_save_as = options.output_path;
    session.editor_settings = editor_settings_from_options(options);
    session.context.options = options;
    session.ai_continue = std::move(ai_continue);
    session.assist_config = options.editor_assist_config;
    session.highlight_enabled = options.tui_highlight;
    session.theme_name = options.tui_theme;
    session.use_colors = !options.no_colors;
    if (session.ai_continue.has_value()) {
        session.context = session.ai_continue->request;
    }
    return session;
}

}  // namespace

int ainiux_main(int argc, char** argv) {
    ainiux::cli::ParseResult parsed = ainiux::cli::parse_args(argc, argv);
    if (!parsed.error.ok()) {
        ainiux::app::print_error(parsed.error);
        return ainiux::app::exit_code_for(parsed.error.code);
    }
    if (parsed.options.help) {
        std::cout << ainiux::cli::help_text();
        return 0;
    }
    if (parsed.options.version) {
        std::cout << ainiux::app_version_label() << "\n";
        return 0;
    }
    ainiux::config::LoadResult configured = ainiux::config::load_automatic(
        ainiux::cli::Options{}, ainiux::config::process_environment(), !parsed.options.no_config);
    if (parsed.options.debug && !parsed.options.quiet) {
        ainiux::app::print_config_diagnostics(configured);
    }
    if (!configured.error.ok()) {
        ainiux::app::print_error(configured.error);
        return ainiux::app::exit_code_for(configured.error.code);
    }
    if (parsed.options.editor && !parsed.options.index_code && !parsed.options.print_index &&
        !parsed.options.clear_index) {
        ainiux::chat::SqliteStore state_store;
        const ainiux::Error open_error = state_store.open_default();
        if (open_error.ok()) {
            std::string saved;
            bool found = false;
            const ainiux::Error state_error =
                state_store.app_state("editor_model_selection", saved, found);
            if (state_error.ok() && found) {
                ainiux::provider::ModelSelection selection;
                const ainiux::Error parse_error =
                    ainiux::provider::parse_model_selection(saved, selection);
                if (parse_error.ok() &&
                    ainiux::provider::can_restore_model_selection(configured.options,
                                                                  selection)) {
                    ainiux::provider::apply_model_selection(configured.options, selection);
                } else if (parse_error.ok() && parsed.options.debug && !parsed.options.quiet) {
                    std::cerr << "Config debug: ignored editor model selection because its "
                                 "provider/API endpoint is no longer configured\n";
                } else if (parsed.options.debug && !parsed.options.quiet) {
                    std::cerr << "Config debug: ignored invalid editor model selection: "
                              << parse_error.message << "\n";
                }
            }
        } else if (parsed.options.debug && !parsed.options.quiet) {
            std::cerr << "Config debug: editor model selection unavailable: "
                      << open_error.message << "\n";
        }
    }
    if (parsed.options.agent) {
        bool restored = false;
        const ainiux::Error restore_error =
            ainiux::agent::restore_project_settings(".", configured.options, restored);
        if (!restore_error.ok()) {
            ainiux::app::print_error(restore_error);
            return ainiux::app::exit_code_for(restore_error.code);
        }
    }
    parsed = ainiux::cli::parse_args(argc, argv, configured.options);
    if (!parsed.error.ok()) {
        ainiux::app::print_error(parsed.error);
        return ainiux::app::exit_code_for(parsed.error.code);
    }
    ainiux::cli::Options options = parsed.options;
    const ainiux::Error disable_indexing_arguments =
        ainiux::cli::validate_disable_indexing_arguments(options);
    if (!disable_indexing_arguments.ok()) {
        ainiux::app::print_error(disable_indexing_arguments);
        return ainiux::app::exit_code_for(disable_indexing_arguments.code);
    }
    if (options.index_code || options.print_index || options.clear_index) {
        const ainiux::Error index_arguments =
            ainiux::cli::validate_index_mode_arguments(argc, argv, options);
        if (!index_arguments.ok()) {
            ainiux::app::print_error(index_arguments);
            return ainiux::app::exit_code_for(index_arguments.code);
        }
        return ainiux::app::run_index_mode(options);
    }
    bool positional_target_changed = false;
    if (!options.positional_url.empty()) {
        if (ainiux::provider::looks_like_api_url(options.positional_url)) {
            ainiux::Error target_error;
            ainiux::Error configured_error;
            const std::string target = ainiux::provider::normalize_base_url(
                options.positional_url, nullptr, target_error);
            const std::string configured_target = configured.options.base_url.empty()
                                                      ? std::string{}
                                                      : ainiux::provider::normalize_base_url(
                                                            configured.options.base_url,
                                                            nullptr,
                                                            configured_error);
            positional_target_changed =
                !target_error.ok() || !configured_error.ok() ||
                configured_target.empty() || target != configured_target;
        } else {
            positional_target_changed =
                ainiux::provider::canonical_profile_name(options.positional_url) !=
                ainiux::provider::canonical_profile_name(configured.options.provider);
        }
    }
    ainiux::provider::apply_cli_target_change(options, configured.options,
                                              positional_target_changed);
    if (!options.security_review && !options.agent && !options.agent_run &&
        !options.trusted_prompt_dir.empty()) {
        ainiux::app::print_error({ainiux::ErrorCode::BadArgs,
                                  "--trusted-prompt-dir requires --security-review, --agent, or --run"});
        return ainiux::app::exit_code_for(ainiux::ErrorCode::BadArgs);
    }
    if (!options.security_review && options.security_review_log_cli_explicit) {
        ainiux::app::print_error({ainiux::ErrorCode::BadArgs,
                                  "--security-review-log and --no-security-review-log require --security-review"});
        return ainiux::app::exit_code_for(ainiux::ErrorCode::BadArgs);
    }
    if (!options.agent && !options.agent_run && options.agent_log_cli_explicit) {
        ainiux::app::print_error({ainiux::ErrorCode::BadArgs,
                                  "--agent-log and --no-agent-log require --agent or --run"});
        return ainiux::app::exit_code_for(ainiux::ErrorCode::BadArgs);
    }
    if (options.agent && options.agent_run) {
        ainiux::app::print_error({ainiux::ErrorCode::BadArgs,
                                  "--agent and --run cannot be combined; use --agent for the interactive "
                                  "agent TUI or --run for a one-shot goal"});
        return ainiux::app::exit_code_for(ainiux::ErrorCode::BadArgs);
    }
    if (options.agent_run) {
        const ainiux::Error argument_error =
            ainiux::cli::validate_agent_run_arguments(argc, argv, options);
        if (!argument_error.ok()) {
            ainiux::app::print_error(argument_error);
            return ainiux::app::exit_code_for(argument_error.code);
        }
        const ainiux::Error profile_error = ainiux::provider::validate_profile_name(options.provider);
        if (!profile_error.ok()) {
            ainiux::app::print_error(profile_error);
            return ainiux::app::exit_code_for(profile_error.code);
        }
        if (!options.key.empty() && !options.quiet) {
            std::cerr << "Warning: command line API keys may be visible to other local users; prefer --key-env, "
                         "--key-file, or --key-stdin.\n";
        }
        if (options.insecure_tls) {
            std::cerr << "Warning: TLS certificate verification is disabled by the effective configuration.\n";
        }
        ainiux::provider::ContextResult context_result = ainiux::provider::build_context(options);
        if (!context_result.error.ok()) {
            ainiux::app::print_error(context_result.error);
            return ainiux::app::exit_code_for(context_result.error.code);
        }
        if (context_result.context.profile.offline) {
            const ainiux::Error error{ainiux::ErrorCode::UnsupportedFeature,
                                      "--run requires an online provider with tool-capable models"};
            ainiux::app::print_error(error);
            return ainiux::app::exit_code_for(error.code);
        }
        ainiux::Error model_error = ainiux::app::choose_default_model(context_result.context);
        if (!model_error.ok()) {
            ainiux::app::print_error(model_error);
            return ainiux::app::exit_code_for(model_error.code);
        }
        return ainiux::app::run_agent_mode(std::move(context_result.context));
    }
    if (options.agent) {
        const ainiux::Error argument_error =
            ainiux::cli::validate_agent_interactive_arguments(argc, argv, options);
        if (!argument_error.ok()) {
            ainiux::app::print_error(argument_error);
            return ainiux::app::exit_code_for(argument_error.code);
        }
        // Interactive agent is a separate product mode from --chat. It shares the
        // full-screen TUI shell and provider/model/reasoning selectors, but must
        // not set options.tui (that flag remains chat-only).
    }
    if (options.security_review) {
        const ainiux::Error argument_error =
            ainiux::cli::validate_security_review_arguments(argc, argv, options);
        if (!argument_error.ok()) {
            ainiux::app::print_error(argument_error);
            return ainiux::app::exit_code_for(argument_error.code);
        }
        const ainiux::Error profile_error = ainiux::provider::validate_profile_name(options.provider);
        if (!profile_error.ok()) {
            ainiux::app::print_error(profile_error);
            return ainiux::app::exit_code_for(profile_error.code);
        }
        if (!options.key.empty() && !options.quiet) {
            std::cerr << "Warning: command line API keys may be visible to other local users; prefer --key-env, "
                         "--key-file, or --key-stdin.\n";
        }
        if (options.insecure_tls) {
            std::cerr << "Warning: TLS certificate verification is disabled by the effective configuration.\n";
        }
        ainiux::provider::ContextResult context_result = ainiux::provider::build_context(options);
        if (!context_result.error.ok()) {
            ainiux::app::print_error(context_result.error);
            return ainiux::app::exit_code_for(context_result.error.code);
        }
        if (context_result.context.profile.offline) {
            const ainiux::Error error{ainiux::ErrorCode::UnsupportedFeature,
                                      "--security-review requires an online provider with native function calling"};
            ainiux::app::print_error(error);
            return ainiux::app::exit_code_for(error.code);
        }
        ainiux::Error model_error = ainiux::app::choose_default_model(context_result.context);
        if (!model_error.ok()) {
            ainiux::app::print_error(model_error);
            return ainiux::app::exit_code_for(model_error.code);
        }
        return ainiux::app::run_security_review_mode(std::move(context_result.context));
    }
    if (options.benchmark && options.grade) {
        ainiux::app::print_error({ainiux::ErrorCode::BadArgs,
                                  "--benchmark and --grade cannot be combined"});
        return ainiux::app::exit_code_for(ainiux::ErrorCode::BadArgs);
    }
    if (options.benchmark) {
        return ainiux::app::run_benchmark_mode(options);
    }
    if (options.grade) {
        return ainiux::app::run_grade_mode(options);
    }
    if (options.benchmark_options_seen) {
        ainiux::app::print_error({ainiux::ErrorCode::BadArgs,
                                  "benchmark dataset options require the 'benchmark' subcommand"});
        return ainiux::app::exit_code_for(ainiux::ErrorCode::BadArgs);
    }
    ainiux::Error profile_error = ainiux::provider::validate_profile_name(options.provider);
    if (!profile_error.ok()) {
        ainiux::app::print_error(profile_error);
        return ainiux::app::exit_code_for(profile_error.code);
    }
    ainiux::Error stdin_error = ainiux::app::validate_stdin_sources(options);
    if (!stdin_error.ok()) {
        ainiux::app::print_error(stdin_error);
        return ainiux::app::exit_code_for(stdin_error.code);
    }
    if (!options.attachment_paths.empty()) {
        for (const std::string& path : options.attachment_paths) {
            if (path.empty()) {
                ainiux::app::print_error({ainiux::ErrorCode::BadArgs, "--attach requires a non-empty path"});
                return ainiux::app::exit_code_for(ainiux::ErrorCode::BadArgs);
            }
        }
        if (options.prompt.empty() && options.prompt_file.empty()) {
            ainiux::app::print_error({ainiux::ErrorCode::BadArgs,
                                      "--attach requires -p/--prompt or --prompt-file in non-interactive mode"});
            return ainiux::app::exit_code_for(ainiux::ErrorCode::BadArgs);
        }
        if (options.editor || options.repl || options.tui || options.list_models) {
            ainiux::app::print_error(
                {ainiux::ErrorCode::BadArgs,
                 "--attach currently supports non-interactive prompt mode only; use /insert in the REPL or TUI"});
            return ainiux::app::exit_code_for(ainiux::ErrorCode::BadArgs);
        }
    }
    if (ainiux::app::has_document_source(options) &&
        !ainiux::app::wants_document_prompt_context(options)) {
        std::ofstream out_file;
        ainiux::Error output_error;
        std::ostream* out = ainiux::app::output_stream(options, out_file, output_error);
        if (!output_error.ok()) {
            ainiux::app::print_error(output_error);
            return ainiux::app::exit_code_for(output_error.code);
        }
        return ainiux::app::run_document_extract(options, *out);
    }
    if (ainiux::app::has_search_source(options) && !ainiux::app::wants_search_prompt_context(options)) {
        std::ofstream out_file;
        ainiux::Error output_error;
        std::ostream* out = ainiux::app::output_stream(options, out_file, output_error);
        if (!output_error.ok()) {
            ainiux::app::print_error(output_error);
            return ainiux::app::exit_code_for(output_error.code);
        }
        return ainiux::app::run_search_extract(options, *out);
    }
    if (ainiux::app::wants_document_prompt_context(options) &&
        (options.editor || options.repl || options.tui || options.list_models)) {
        ainiux::app::print_error({ainiux::ErrorCode::BadArgs,
                                  "--fetch-url/--input prompt context currently supports non-interactive "
                                  "prompt mode only"});
        return ainiux::app::exit_code_for(ainiux::ErrorCode::BadArgs);
    }
    if (ainiux::app::wants_search_prompt_context(options) &&
        (options.editor || options.repl || options.tui || options.list_models)) {
        ainiux::app::print_error({ainiux::ErrorCode::BadArgs,
                                  "--search prompt context currently supports non-interactive prompt mode only"});
        return ainiux::app::exit_code_for(ainiux::ErrorCode::BadArgs);
    }
    if (options.rendered_output_format_explicit && options.format != ainiux::cli::OutputFormat::Text) {
        ainiux::app::print_error(
            {ainiux::ErrorCode::BadArgs, "--output-format can only be combined with --format text"});
        return ainiux::app::exit_code_for(ainiux::ErrorCode::BadArgs);
    }
    if (options.rendered_output_format_explicit && options.list_models) {
        ainiux::app::print_error(
            {ainiux::ErrorCode::BadArgs, "rendered --output-format cannot be combined with --list-models"});
        return ainiux::app::exit_code_for(ainiux::ErrorCode::BadArgs);
    }
    if (options.editor && (options.repl || options.tui || options.agent)) {
        ainiux::app::print_error(
            {ainiux::ErrorCode::BadArgs, "--editor cannot be combined with --repl, --chat, or --agent"});
        return ainiux::app::exit_code_for(ainiux::ErrorCode::BadArgs);
    }
    if (options.repl && (options.tui || options.agent)) {
        ainiux::app::print_error(
            {ainiux::ErrorCode::BadArgs, "--repl cannot be combined with --chat or --agent"});
        return ainiux::app::exit_code_for(ainiux::ErrorCode::BadArgs);
    }
    if (options.tui && options.agent) {
        ainiux::app::print_error(
            {ainiux::ErrorCode::BadArgs, "--chat and --agent are separate modes and cannot be combined"});
        return ainiux::app::exit_code_for(ainiux::ErrorCode::BadArgs);
    }
    if (options.repl && options.list_models) {
        ainiux::app::print_error({ainiux::ErrorCode::BadArgs, "--repl cannot be combined with --list-models"});
        return ainiux::app::exit_code_for(ainiux::ErrorCode::BadArgs);
    }
    if (options.tui && options.list_models) {
        ainiux::app::print_error({ainiux::ErrorCode::BadArgs,
                                  "--chat cannot be combined with --list-models; use /models inside the chat UI"});
        return ainiux::app::exit_code_for(ainiux::ErrorCode::BadArgs);
    }
    if (options.editor && options.list_models) {
        ainiux::app::print_error({ainiux::ErrorCode::BadArgs, "--editor cannot be combined with --list-models"});
        return ainiux::app::exit_code_for(ainiux::ErrorCode::BadArgs);
    }
    if (options.repl && options.format != ainiux::cli::OutputFormat::Text) {
        ainiux::app::print_error({ainiux::ErrorCode::BadArgs, "--repl currently supports --format text only"});
        return ainiux::app::exit_code_for(ainiux::ErrorCode::BadArgs);
    }
    if (options.tui && options.format != ainiux::cli::OutputFormat::Text) {
        ainiux::app::print_error({ainiux::ErrorCode::BadArgs, "--chat currently supports --format text only"});
        return ainiux::app::exit_code_for(ainiux::ErrorCode::BadArgs);
    }
    if (options.tui && !options.output_path.empty()) {
        ainiux::app::print_error({ainiux::ErrorCode::BadArgs, "--chat cannot be combined with --output"});
        return ainiux::app::exit_code_for(ainiux::ErrorCode::BadArgs);
    }
    if (options.tui && options.rendered_output_format_explicit) {
        ainiux::app::print_error({ainiux::ErrorCode::BadArgs, "--chat does not use rendered --output-format"});
        return ainiux::app::exit_code_for(ainiux::ErrorCode::BadArgs);
    }
    if (options.editor && options.format != ainiux::cli::OutputFormat::Text) {
        ainiux::app::print_error({ainiux::ErrorCode::BadArgs, "--editor does not use --format"});
        return ainiux::app::exit_code_for(ainiux::ErrorCode::BadArgs);
    }
    if (options.editor && options.rendered_output_format_explicit) {
        ainiux::app::print_error({ainiux::ErrorCode::BadArgs, "--editor does not use rendered --output-format"});
        return ainiux::app::exit_code_for(ainiux::ErrorCode::BadArgs);
    }
    if (options.editor && (!options.prompt.empty() || !options.prompt_file.empty())) {
        ainiux::app::print_error({ainiux::ErrorCode::BadArgs, "--editor cannot be combined with prompt options"});
        return ainiux::app::exit_code_for(ainiux::ErrorCode::BadArgs);
    }
    if (options.editor && (!options.load_chat_path.empty() || !options.save_chat_path.empty())) {
        ainiux::app::print_error(
            {ainiux::ErrorCode::BadArgs, "--editor cannot be combined with --load-chat or --save-chat"});
        return ainiux::app::exit_code_for(ainiux::ErrorCode::BadArgs);
    }
    if (options.editor) {
        ainiux::provider::apply_editor_startup_default(options);
        ainiux::provider::apply_editor_offline_default(options);
        ainiux::provider::ContextResult context_result = ainiux::provider::build_context(options);
        if (!context_result.error.ok()) {
            ainiux::app::print_error(context_result.error);
            return ainiux::app::exit_code_for(context_result.error.code);
        }
        ainiux::provider::RequestContext editor_context = std::move(context_result.context);
        const std::string temperature_advisory =
            ainiux::provider::reasoning_temperature_advisory(editor_context);
        if (!temperature_advisory.empty() && !options.quiet) {
            std::cerr << "Warning: " << temperature_advisory << ".\n";
        }
        // Always seed ai_continue, including offline/provider-none startups.
        // Dropping it previously discarded model_catalog; a later /provider
        // switch then had no models.conf reasoning options for Ctrl+T.
        ainiux::editor::AiContinueContext configured;
        configured.request = std::move(editor_context);
        configured.settings = ainiux::editor::ai_continue_settings(options);
        configured.assist_config = options.editor_assist_config;
        std::optional<ainiux::editor::AiContinueContext> ai_continue = std::move(configured);
        return ainiux::app::run_interactive(
            interactive_session_from_editor_startup(options, std::move(ai_continue)));
    }
    if (!options.key.empty() && !options.quiet) {
        std::cerr << "Warning: command line API keys may be visible to other local users; prefer --key-env, "
                     "--key-file, or --key-stdin.\n";
    }
    if (options.insecure_tls) {
        std::cerr << "Warning: TLS certificate verification is disabled by the effective configuration.\n";
    }

    ainiux::chat::Session session;
    bool loaded_session = false;
    if (!options.load_chat_path.empty()) {
        ainiux::Error err = ainiux::chat::load_session(options.load_chat_path, session);
        if (!err.ok()) {
            ainiux::app::print_error(err);
            return ainiux::app::exit_code_for(err.code);
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

    ainiux::provider::apply_tui_startup_default(options);
    ainiux::provider::ContextResult context_result = ainiux::provider::build_context(options);
    if (!context_result.error.ok()) {
        ainiux::app::print_error(context_result.error);
        return ainiux::app::exit_code_for(context_result.error.code);
    }
    ainiux::provider::RequestContext context = context_result.context;

    std::ofstream out_file;
    ainiux::Error output_error;
    std::ostream* out = ainiux::app::output_stream(context.options, out_file, output_error);
    if (!output_error.ok()) {
        ainiux::app::print_error(output_error);
        return ainiux::app::exit_code_for(output_error.code);
    }

    std::string fetched_context_message;
    std::vector<std::string> attachment_context_messages;
    std::vector<ainiux::provider::ImageInput> prompt_images;
    std::vector<ainiux::input::FileType> attachment_types;
    attachment_types.reserve(context.options.attachment_paths.size());
    bool image_requested = false;
    if (ainiux::app::wants_document_prompt_context(context.options) &&
        !context.options.fetch_url.empty()) {
        image_requested = false;
    } else if (ainiux::app::wants_document_prompt_context(context.options)) {
        ainiux::input::FileType type;
        ainiux::Error err = ainiux::app::local_input_type_for_options(context.options, type);
        if (!err.ok()) {
            ainiux::app::print_error(err);
            return ainiux::app::exit_code_for(err.code);
        }
        image_requested = type.kind == InputKind::Image;
    }
    for (const std::string& path : context.options.attachment_paths) {
        ainiux::input::FileType type;
        ainiux::Error err = ainiux::input::classify_file_type(path, type);
        if (!err.ok()) {
            ainiux::app::print_error(err);
            return ainiux::app::exit_code_for(err.code);
        }
        image_requested = image_requested || type.kind == InputKind::Image;
        attachment_types.push_back(std::move(type));
    }
    bool model_chosen = false;
    if (image_requested) {
        ainiux::Error model_err = ainiux::app::choose_default_model(context);
        if (!model_err.ok()) {
            ainiux::app::print_error(model_err);
            return ainiux::app::exit_code_for(model_err.code);
        }
        model_chosen = true;
        ainiux::Error capability_error = ainiux::provider::validate_image_input(context);
        if (!capability_error.ok()) {
            ainiux::app::print_error(capability_error);
            return ainiux::app::exit_code_for(capability_error.code);
        }
    }
    if (ainiux::app::wants_document_prompt_context(context.options)) {
        ainiux::app::LoadedDocument document;
        ainiux::Error err = ainiux::app::load_document(context.options, false, document);
        if (!err.ok()) {
            ainiux::app::print_error(err);
            return ainiux::app::exit_code_for(err.code);
        }
        if (document.input_kind == InputKind::Image) {
            prompt_images.push_back(std::move(document.image));
        } else {
            fetched_context_message = ainiux::app::document_context_message(document);
        }
    }
    attachment_context_messages.reserve(context.options.attachment_paths.size());
    for (size_t attachment_index = 0; attachment_index < context.options.attachment_paths.size();
         ++attachment_index) {
        const std::string& path = context.options.attachment_paths[attachment_index];
        const ainiux::input::FileType& type = attachment_types[attachment_index];
        ainiux::Error err;
        if (type.kind == InputKind::Image) {
            if (context.options.max_image_bytes <= 0) {
                err = {ainiux::ErrorCode::BadArgs, "--max-image-bytes must be greater than zero"};
            } else {
                ainiux::input::ImageData image;
                err = ainiux::input::load_image_file(path, type,
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
            ainiux::app::LoadedDocument document;
            err = ainiux::app::load_text_context_file(context.options, path, "--attach", document);
            if (err.ok()) {
                attachment_context_messages.push_back(ainiux::app::document_context_message(document));
                if (!context.options.quiet) {
                    std::cerr << "Attached context: " << path << "\n";
                }
            }
        }
        if (!err.ok()) {
            ainiux::app::print_error(err);
            return ainiux::app::exit_code_for(err.code);
        }
    }

    if (context.options.list_models) {
        ainiux::provider::ModelsResult models;
        ainiux::Error err = ainiux::provider::list_models(context, models);
        if (!err.ok()) {
            ainiux::app::print_error(err);
            return ainiux::app::exit_code_for(err.code);
        }
        if (context.options.format == ainiux::cli::OutputFormat::Json) {
            *out << "{\"provider\":" << ainiux::json::quote(context.profile.name) << ",\"models\":[";
            for (size_t i = 0; i < models.model_ids.size(); ++i) {
                if (i != 0) {
                    *out << ",";
                }
                *out << ainiux::json::quote(models.model_ids[i]);
            }
            *out << "]}\n";
        } else {
            *out << ainiux::provider::format_models_markdown(context.profile.name, context.models_url, models);
        }
        return 0;
    }

    if (!loaded_session) {
        session = ainiux::chat::new_session(context);
    }
    if (!model_chosen && !context.options.tui && !context.options.agent) {
        ainiux::Error model_err = ainiux::app::choose_default_model(context);
        if (!model_err.ok()) {
            ainiux::app::print_error(model_err);
            return ainiux::app::exit_code_for(model_err.code);
        }
    }
    ainiux::app::refresh_session_metadata(session, context);
    ainiux::app::apply_system_prompt(session, context.options.system);

    // Full-screen interactive surfaces: Chat (--chat/-c) and Agent (--agent/-a).
    // They share TUI widgets (history, input, provider/model/reasoning pickers)
    // but remain separate modes and entry points.
    if (context.options.tui || context.options.agent) {
        if (context.options.tui && context.options.agent) {
            const ainiux::Error error{ainiux::ErrorCode::BadArgs,
                                      "--chat and --agent are separate modes and cannot be combined"};
            ainiux::app::print_error(error);
            return ainiux::app::exit_code_for(error.code);
        }
        ainiux::app::InteractiveSession interactive;
        interactive.start_mode = context.options.agent ? ainiux::app::InteractiveMode::Agent
                                                       : ainiux::app::InteractiveMode::Chat;
        interactive.context = std::move(context);
        interactive.chat_session = std::move(session);
        interactive.chat_session_initialized = true;
        interactive.editor_path = interactive.context.options.editor_path;
        interactive.editor_save_as = interactive.context.options.output_path;
        interactive.editor_settings = editor_settings_from_options(interactive.context.options);
        interactive.assist_config = interactive.context.options.editor_assist_config;
        interactive.highlight_enabled = interactive.context.options.tui_highlight;
        interactive.theme_name = interactive.context.options.tui_theme;
        interactive.use_colors = !interactive.context.options.no_colors;
        ainiux::app::sync_shared_provider_to_editor(interactive);
        return ainiux::app::run_interactive(std::move(interactive));
    }

    ainiux::app::print_chat_start(context);

    if (context.options.repl) {
        return ainiux::app::run_repl(context, std::move(session), *out);
    }

    if (!fetched_context_message.empty()) {
        session.messages.push_back({"user", fetched_context_message});
    }
    if (ainiux::app::wants_search_prompt_context(context.options)) {
        ainiux::search::SearchResponse search_response;
        ainiux::Error search_err =
            ainiux::search::search(context.options.search_query, ainiux::search::options_for(context.options),
                                   search_response);
        if (!search_err.ok()) {
            ainiux::app::print_error(search_err);
            return ainiux::app::exit_code_for(search_err.code);
        }
        if (!context.options.quiet) {
            std::cerr << "Web search provider: " << search_response.provider_used << " ("
                      << search_response.results.size() << " results)\n";
        }
        session.messages.push_back(
            {"user", ainiux::app::search_context_message(context.options, search_response)});
    }
    for (std::string& message : attachment_context_messages) {
        session.messages.push_back({"user", std::move(message)});
    }

    ainiux::provider::ChatResult chat;
    ainiux::Error err = ainiux::app::send_session_turn(context, session, context.options.prompt, *out, chat,
                                                      std::move(prompt_images), true);
    if (!err.ok()) {
        ainiux::app::print_error(err);
        return ainiux::app::exit_code_for(err.code);
    }
    err = ainiux::app::save_if_requested(context.options, session);
    if (!err.ok()) {
        ainiux::app::print_error(err);
        return ainiux::app::exit_code_for(err.code);
    }
    ainiux::app::print_verbose_metrics(context, chat, session.messages);
    return 0;
}

#if defined(_WIN32)
int wmain(int argc, wchar_t** wide_argv) {
    ainiux::platform::ProcessEnvironmentGuard environment_guard;
    std::vector<std::string> arguments;
    arguments.reserve(static_cast<std::size_t>(argc));
    for (int index = 0; index < argc; ++index) {
        std::string utf8;
        const ainiux::Error error =
            ainiux::platform::utf16_to_utf8(wide_argv[index], utf8);
        if (!error.ok()) {
            ainiux::app::print_error(error);
            return ainiux::app::exit_code_for(error.code);
        }
        arguments.push_back(std::move(utf8));
    }
    std::vector<char*> argv;
    argv.reserve(arguments.size() + 1U);
    for (std::string& argument : arguments) argv.push_back(argument.data());
    argv.push_back(nullptr);
    return ainiux_main(argc, argv.data());
}
#else
int main(int argc, char** argv) {
    ainiux::platform::ProcessEnvironmentGuard environment_guard;
    return ainiux_main(argc, argv);
}
#endif
