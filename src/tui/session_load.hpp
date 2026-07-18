#pragma once

#include <string>

#include "chat/session.hpp"
#include "common.hpp"
#include "context/context.hpp"

namespace ainiux::tui {

bool active_context_has_provider_selection(const provider::RequestContext& active);

bool saved_provider_model_complete(const std::string& provider_name,
                                   const std::string& model_name);

std::string saved_provider_model_missing(const std::string& provider_name,
                                         const std::string& model_name);

bool session_has_complete_provider_model(const chat::Session& session);

bool loaded_session_differs_from_context(const provider::RequestContext& active,
                                         const chat::Session& loaded);

bool loaded_session_differs_from_cli(const provider::RequestContext& cli_context,
                                     const chat::Session& loaded);

std::string model_confirm_text(const provider::RequestContext& active,
                               const chat::Session& loaded);

Error apply_loaded_session_to_context(provider::RequestContext& context, const chat::Session& loaded);

void restore_cli_context(provider::RequestContext& context, const provider::RequestContext& cli_context);

}  // namespace ainiux::tui
