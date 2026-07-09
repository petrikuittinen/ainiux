#pragma once

#include <string>

#include "chat/session.hpp"
#include "common.hpp"
#include "provider/provider.hpp"

namespace pkchat::tui {

bool apply_selected_provider(provider::RequestContext& context,
                             chat::Session& session,
                             bool& show_thinking_traces,
                             const std::string& provider_target,
                             std::string& status);

}  // namespace pkchat::tui