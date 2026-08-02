#include "app/app.hpp"
#include "app/detail.hpp"

#include <iomanip>
#include <iostream>
#include <utility>

#include "chat/settings.hpp"
#include "ainiux/model_setting.hpp"
#include "context/context.hpp"
#include "json/json.hpp"
#include "markdown/markdown.hpp"
#include "output/thinking.hpp"
#include "ainiux/version.hpp"

namespace ainiux::app {

namespace {

void write_json_chat(std::ostream& out,
                     const provider::RequestContext& context,
                     const provider::ChatResult& result) {
    out << "{"
        << "\"model\":" << json::quote(result.model) << ","
        << "\"provider\":" << json::quote(context.profile.name) << ","
        << "\"content\":" << json::quote(result.content) << ","
        << "\"usage\":" << result.usage_json << ","
        << "\"timing\":{\"ttft_ms\":" << result.ttft_ms << ",\"total_ms\":" << result.total_ms << "}"
        << "}\n";
}

bool has_system_message(const chat::Session& session) {
    for (const provider::Message& message : session.messages) {
        if (message.role == "system") {
            return true;
        }
    }
    return false;
}

bool streams_raw_markdown_output(const cli::Options& options) {
    return options.format == cli::OutputFormat::Text &&
           options.output_format == markdown::OutputFormat::Markdown;
}

void write_rendered_assistant_output(const cli::Options& options,
                                     const std::string& content,
                                     std::ostream& out) {
    const bool complete_html_document = options.output_format == markdown::OutputFormat::Html &&
                                        !options.output_path.empty() && options.output_path != "stdout";
    const std::string rendered = markdown::render(content, options.output_format, complete_html_document);
    out << rendered;
    if (rendered.empty() || rendered.back() != '\n') {
        out << '\n';
    }
}

}  // namespace

void refresh_session_metadata(chat::Session& session, const provider::RequestContext& context) {
    session.provider = context.profile.name;
    session.base_url = context.base_url;
    session.model = context.options.model;
    session.settings_json = chat::settings_json_from_options(context.options);
}

void apply_system_prompt(chat::Session& session, const std::string& system) {
    if (detail::trim_ascii(system).empty() || has_system_message(session)) {
        return;
    }
    session.messages.insert(session.messages.begin(), {"system", system});
}

void replace_system_prompt(chat::Session& session, const std::string& system) {
    for (auto it = session.messages.begin(); it != session.messages.end();) {
        if (it->role == "system") {
            it = session.messages.erase(it);
        } else {
            ++it;
        }
    }
    if (!detail::trim_ascii(system).empty()) {
        session.messages.insert(session.messages.begin(), {"system", system});
    }
}

void print_verbose_metrics(provider::RequestContext& context,
                           const provider::ChatResult& result,
                           const std::vector<provider::Message>& messages) {
    if (!context.options.verbose || context.options.quiet) {
        return;
    }
    // Discover the window when it is not an explicit positive override.
    if (!context.options.has_context_tokens || context.options.context_tokens <= 0) {
        const std::string selector = !result.model.empty() ? result.model : context.options.model;
        provider::resolve_context_window(context, selector);
    }
    std::cerr << "TTFT: " << result.ttft_ms << " ms, ";
    std::cerr << "Token/s: " << std::fixed << std::setprecision(1)
              << provider::tokens_per_second(result, context.options.stream);
    if (result.completion_tokens_estimated) {
        std::cerr << " (estimated)";
    }
    const std::string context_usage = context::format_context_usage(
        context::estimated_usage_tokens(messages, result), context.options.context_tokens);
    if (!context_usage.empty()) {
        std::cerr << ", context: " << context_usage;
    }
    std::cerr << "\n";
}

Error save_if_requested(const cli::Options& options, const chat::Session& session) {
    if (options.save_chat_path.empty()) {
        return ok_error();
    }
    return chat::save_session_atomic(options.save_chat_path, session);
}

Error choose_default_model(provider::RequestContext& context) {
    if (context.profile.offline) {
        return ok_error();
    }
    provider::ModelsResult models;
    bool listed_models = false;
    if (context.options.model.empty()) {
        Error err = provider::list_models(context, models);
        if (!err.ok()) {
            return err;
        }
        listed_models = true;
        if (!models.model_ids.empty()) {
            context.options.model = models.model_ids.front();
        }
    }
    if (!context.options.has_context_tokens && context.options.context_tokens <= 0 &&
        !context.options.model.empty()) {
        if (!listed_models) {
            const Error err = provider::list_models(context, models);
            if (err.ok()) {
                listed_models = true;
            }
        }
        if (listed_models) {
            provider::apply_context_window_from_models(context, models);
        }
    }
    if (!context.options.chat_purpose.empty() && !context.options.model.empty()) {
        const ModelCapability* capability = provider::matched_model_capability(context);
        if (capability != nullptr) {
            const ModelSetting* preset = config::find_model_preset(context.options.model_catalog,
                                                                   *capability,
                                                                   context.options.chat_purpose);
            if (preset != nullptr) {
                return chat::apply_model_setting_preset(context.options, *preset, capability);
            }
        }
    }
    return ok_error();
}

void print_chat_start(const provider::RequestContext& context) {
    if (context.options.quiet) {
        return;
    }
    if (!context.profile.offline) {
        const std::string catalog_warning = config::reasoning_catalog_warning(
            context.options.model_catalog,
            context.profile.name,
            context.api_kind == provider::ApiKind::Responses ? "responses" : "chat",
            context.options.model,
            context.options.reasoning);
        if (!catalog_warning.empty()) {
            std::cerr << "Warning: " << catalog_warning << ".\n";
        }
        const std::string advisory = provider::reasoning_temperature_advisory(context);
        if (!advisory.empty()) {
            std::cerr << "Warning: " << advisory << ".\n";
        }
    }
    if (context.options.repl) {
        std::cerr << app_version_label() << " REPL | ";
        if (context.profile.offline) {
            std::cerr << "Provider: none (offline; AI/model requests disabled)\n";
        } else {
            std::cerr << "Endpoint: " << provider::active_request_url(context) << " | Model: "
                      << (context.options.model.empty() ? "unknown" : context.options.model) << "\n";
        }
        std::cerr << "Type /help for commands, /quit to exit.\n";
        return;
    }
    if (context.profile.offline) {
        std::cerr << "Provider: none (offline; AI/model requests disabled)" << std::endl;
        return;
    }
    std::cerr << "Endpoint: " << provider::active_request_url(context) << std::endl;
    std::cerr << "Model: " << (context.options.model.empty() ? "unknown" : context.options.model) << std::endl;
}

Error send_session_turn(provider::RequestContext& context,
                        chat::Session& session,
                        const std::string& prompt,
                        std::ostream& out,
                        provider::ChatResult& chat,
                        std::vector<provider::ImageInput> images,
                        bool separate_thinking_traces) {
    if (session.read_only) {
        return {ErrorCode::FileLock,
                "chat thread is read-only" +
                    (session.read_only_reason.empty()
                         ? std::string(": managed attachment media is unavailable")
                         : ": " + session.read_only_reason)};
    }
    session.messages.push_back({"user", prompt, std::move(images)});
    context::PreparedMessages prepared = context::prepare(
        session.messages, context.options.context_policy,
        context.options.max_context_bytes > 0 ? static_cast<size_t>(context.options.max_context_bytes) : 0U);
    if (!prepared.error.ok()) {
        session.messages.pop_back();
        return prepared.error;
    }
    session.messages.back().images.clear();
    bool started_ndjson = false;
    output::ThinkingTraceSplitter thinking_splitter;
    std::string visible_content;
    bool emitted_trace = false;
    char last_trace_character = '\0';
    auto emit_trace = [&](const std::string& trace) {
        if (trace.empty()) {
            return;
        }
        emitted_trace = true;
        last_trace_character = trace.back();
        std::cerr << trace;
        std::cerr.flush();
    };
    auto on_delta = [&](const std::string& delta) -> Error {
        std::string visible_delta = delta;
        if (separate_thinking_traces) {
            output::ThinkingChunk split = thinking_splitter.feed(delta);
            visible_delta = std::move(split.visible);
            visible_content += visible_delta;
            emit_trace(split.trace);
        }
        if (visible_delta.empty()) {
            return ok_error();
        }
        if (context.options.format == cli::OutputFormat::Text) {
            if (streams_raw_markdown_output(context.options)) {
                out << visible_delta;
                out.flush();
            }
        } else if (context.options.format == cli::OutputFormat::Ndjson) {
            if (!started_ndjson) {
                out << "{\"event\":\"start\",\"model\":" << json::quote(context.options.model) << "}\n";
                started_ndjson = true;
            }
            out << "{\"event\":\"delta\",\"text\":" << json::quote(visible_delta) << "}\n";
            out.flush();
        }
        return ok_error();
    };

    Error err = provider::send_chat_messages(context, prepared.messages, on_delta, chat);
    if (!err.ok()) {
        session.messages.pop_back();
        return err;
    }
    if (separate_thinking_traces) {
        if (context.options.stream) {
            output::ThinkingChunk final = thinking_splitter.finish();
            visible_content += final.visible;
            emit_trace(final.trace);
            if (!final.visible.empty()) {
                if (context.options.format == cli::OutputFormat::Text &&
                    streams_raw_markdown_output(context.options)) {
                    out << final.visible;
                    out.flush();
                } else if (context.options.format == cli::OutputFormat::Ndjson) {
                    if (!started_ndjson) {
                        out << "{\"event\":\"start\",\"model\":" << json::quote(context.options.model)
                            << "}\n";
                        started_ndjson = true;
                    }
                    out << "{\"event\":\"delta\",\"text\":" << json::quote(final.visible) << "}\n";
                }
            }
        } else {
            output::ThinkingChunk split = output::split_thinking_traces(chat.content);
            visible_content = std::move(split.visible);
            emit_trace(split.trace);
        }
        if (emitted_trace && last_trace_character != '\n') {
            std::cerr << '\n';
        }
    } else {
        visible_content = chat.content;
    }
    session.messages.push_back({"assistant", chat.content});
    if (prepared.compacted) {
        prepared.event.timestamp = chat::current_timestamp_utc();
        session.compaction_events.push_back(prepared.event);
        if (!context.options.quiet) {
            std::cerr << prepared.event.notice << "\n";
        }
    }
    if (!chat.model.empty()) {
        session.model = chat.model;
    }
    if (!chat.usage_json.empty() && chat.usage_json != "null") {
        session.usage_json = chat.usage_json;
    }

    if (context.options.format == cli::OutputFormat::Text) {
        if (context.options.output_format == markdown::OutputFormat::Markdown) {
            if (!context.options.stream) {
                out << visible_content;
            }
            out << "\n";
        } else {
            write_rendered_assistant_output(context.options, visible_content, out);
        }
    } else if (context.options.format == cli::OutputFormat::Json) {
        provider::ChatResult visible_chat = chat;
        visible_chat.content = std::move(visible_content);
        write_json_chat(out, context, visible_chat);
    } else {
        if (!started_ndjson) {
            out << "{\"event\":\"start\",\"model\":" << json::quote(context.options.model) << "}\n";
        }
        if (!context.options.stream && !visible_content.empty()) {
            out << "{\"event\":\"delta\",\"text\":" << json::quote(visible_content) << "}\n";
        }
        out << "{\"event\":\"done\",\"usage\":null}\n";
    }
    return ok_error();
}

}  // namespace ainiux::app
