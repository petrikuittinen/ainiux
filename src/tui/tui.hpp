#pragma once

#include "chat/session.hpp"
#include "provider/provider.hpp"

namespace pkchat::tui {

int run(provider::RequestContext context, chat::Session session);

}  // namespace pkchat::tui
