#include "tui/background_metadata.hpp"

namespace ainiux::tui {

provider::RequestContext automatic_metadata_request(
    provider::RequestContext context) {
    if (context.options.timeout_seconds <= 0)
        context.options.timeout_seconds = kAutomaticMetadataTimeoutSeconds;
    context.options.quiet = true;
    return context;
}

void apply_automatic_context_catalog(provider::RequestContext& context) {
    provider::apply_context_window_from_models(
        context, provider::ModelsResult{}, context.options.model);
}

}  // namespace ainiux::tui
