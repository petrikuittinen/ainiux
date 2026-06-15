#pragma once

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
int run(provider::RequestContext context, chat::Session session);

}  // namespace pkchat::tui
