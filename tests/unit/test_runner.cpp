#include <iostream>

#include "benchmark/test_benchmark.hpp"
#include "app/test_user_shell.hpp"
#include "agent/test_agent_adversarial.hpp"
#include "agent/test_agent_controller.hpp"
#include "agent/test_agent_loop.hpp"
#include "agent/test_agents_md.hpp"
#include "agent/test_apply_patch.hpp"
#include "agent/test_command_guard.hpp"
#include "agent/test_file_tools.hpp"
#include "agent/test_index.hpp"
#include "agent/test_review.hpp"
#include "agent/test_session_store.hpp"
#include "agent/test_session_runtime.hpp"
#include "agent/test_project_root.hpp"
#include "chat/test_chat.hpp"
#include "cli/test_cli.hpp"
#include "config/test_config.hpp"
#include "context/test_context.hpp"
#include "editor/test_editor.hpp"
#include "fetch/test_fetch.hpp"
#include "html/test_html.hpp"
#include "highlight/test_highlight.hpp"
#include "http/test_http.hpp"
#include "input/test_input.hpp"
#include "json/test_json.hpp"
#include "markdown/test_markdown.hpp"
#include "output/test_output.hpp"
#include "provider/test_provider.hpp"
#include "runtime/test_runtime.hpp"
#include "search/test_search.hpp"
#include "security/test_security.hpp"
#include "support/test_support.hpp"
#include "tui/test_tui.hpp"
#include "ui/test_text_selector.hpp"

int main() {
    ainiux::test::app_user_shell::run_all();
    ainiux::test::agent_index::run_all();
    ainiux::test::agent_loop::run_all();
    ainiux::test::agent_agents_md::run_all();
    ainiux::test::agent_file_tools::run_all();
    ainiux::test::agent_apply_patch::run_all();
    ainiux::test::agent_session_store::run_all();
    ainiux::test::agent_session_runtime::run_all();
    ainiux::test::agent_controller::run_all();
    ainiux::test::agent_project_root::run_all();
    ainiux::test::agent_command_guard::run_all();
    ainiux::test::agent_adversarial::run_all();
    ainiux::test::agent_review::run_all();
    ainiux::test::output::run_all();
    ainiux::test::config::run_all();
    ainiux::test::cli::run_all();
    ainiux::test::benchmark::run_all();
    ainiux::test::input::run_all();
    ainiux::test::context::run_all();
    ainiux::test::http::run_all();
    ainiux::test::fetch::run_all();
    ainiux::test::search::run_all();
    ainiux::test::html::run_all();
    ainiux::test::highlight::run_all();
    ainiux::test::markdown::run_all();
    ainiux::test::provider::run_all();
    ainiux::test::json::run_all();
    ainiux::test::chat::run_all();
    ainiux::test::runtime::run_all();
    ainiux::test::security::run_all();
    ainiux::test::ui::run_all();
    ainiux::test::editor::run_all();
    ainiux::test::tui::run_all();

    if (ainiux::test::failures != 0) {
        std::cerr << ainiux::test::failures << " unit test(s) failed\n";
        return 1;
    }
    std::cout << "unit tests passed\n";
    return 0;
}
