#include "tui/provider_actions.hpp"

#include "app/app.hpp"
#include "app/detail.hpp"
#include "cli/args.hpp"
#include "tui/detail/render.hpp"

namespace ainiux::tui {

bool apply_selected_provider(provider::RequestContext& context,
                             chat::Session& session,
                             bool& show_thinking_traces,
                             const std::string& provider_target,
                             std::string& status) {
    cli::Options next = context.options;
    provider::apply_provider_target(next, provider_target);
    next.model.clear();
    provider::ContextResult rebuilt = provider::build_context(next);
    if (!rebuilt.error.ok()) {
        status = detail::error_line(rebuilt.error);
        return false;
    }
    context = std::move(rebuilt.context);
    show_thinking_traces = context.options.show_thinking_traces;
    app::refresh_session_metadata(session, context);
    return true;
}

}  // namespace ainiux::tui