#include <iostream>

#include "benchmark/test_benchmark.hpp"
#include "chat/test_chat.hpp"
#include "cli/test_cli.hpp"
#include "config/test_config.hpp"
#include "context/test_context.hpp"
#include "editor/test_editor.hpp"
#include "fetch/test_fetch.hpp"
#include "html/test_html.hpp"
#include "http/test_http.hpp"
#include "input/test_input.hpp"
#include "json/test_json.hpp"
#include "markdown/test_markdown.hpp"
#include "output/test_output.hpp"
#include "provider/test_provider.hpp"
#include "runtime/test_runtime.hpp"
#include "security/test_security.hpp"
#include "support/test_support.hpp"
#include "tui/test_tui.hpp"

int main() {
    pkchat::test::output::run_all();
    pkchat::test::config::run_all();
    pkchat::test::cli::run_all();
    pkchat::test::benchmark::run_all();
    pkchat::test::input::run_all();
    pkchat::test::context::run_all();
    pkchat::test::http::run_all();
    pkchat::test::fetch::run_all();
    pkchat::test::html::run_all();
    pkchat::test::markdown::run_all();
    pkchat::test::provider::run_all();
    pkchat::test::json::run_all();
    pkchat::test::chat::run_all();
    pkchat::test::runtime::run_all();
    pkchat::test::security::run_all();
    pkchat::test::editor::run_all();
    pkchat::test::tui::run_all();

    if (pkchat::test::failures != 0) {
        std::cerr << pkchat::test::failures << " unit test(s) failed\n";
        return 1;
    }
    std::cout << "unit tests passed\n";
    return 0;
}
