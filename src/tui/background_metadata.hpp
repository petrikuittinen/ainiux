#pragma once

#include "provider/provider.hpp"

namespace ainiux::tui {

constexpr int kAutomaticMetadataTimeoutSeconds = 15;

provider::RequestContext automatic_metadata_request(
    provider::RequestContext context);

void apply_automatic_context_catalog(provider::RequestContext& context);

}  // namespace ainiux::tui
