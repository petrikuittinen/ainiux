#pragma once

#include <cstddef>
#include <string>

#include "chat/session.hpp"
#include "editor/editor.hpp"
#include "provider/provider.hpp"

namespace pkchat::tui {

struct Layout {
    int rows = 24;
    int cols = 80;
    int header_rows = 0;
    int history_row = 1;
    int history_rows = 18;
    int status_row = 19;
    int input_label_row = 20;
    editor::Rect input_rect{21, 1, 4, 80};
};

Layout layout_for_terminal(int rows, int cols);

struct RegenerationPlan {
    bool available = false;
    std::size_t erase_from = 0;
    std::string prompt;
};

RegenerationPlan regeneration_plan_for_session(const chat::Session& session);
int run(provider::RequestContext context, chat::Session session);

}  // namespace pkchat::tui
