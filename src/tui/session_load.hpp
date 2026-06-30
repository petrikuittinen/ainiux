#pragma once

#include <string>

#include "chat/session.hpp"
#include "common.hpp"
#include "context/context.hpp"

namespace pkchat::tui {

bool loaded_session_differs_from_cli(const provider::RequestContext& cli_context,
                                     const chat::Session& loaded);

std::string model_confirm_text(const provider::RequestContext& cli_context,
                               const chat::Session& loaded);

Error apply_loaded_session_to_context(provider::RequestContext& context, const chat::Session& loaded);

void restore_cli_context(provider::RequestContext& context, const provider::RequestContext& cli_context);

}  // namespace pkchat::tui