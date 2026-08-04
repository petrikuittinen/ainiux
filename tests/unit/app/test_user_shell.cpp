#include "app/test_user_shell.hpp"

#include "app/user_shell.hpp"
#include "support/test_support.hpp"

#include <optional>
#include <string>
#include <vector>

namespace ainiux::test::app_user_shell {
namespace {

using ainiux::test::check;

void test_parse_user_shell_invocation() {
    std::string command;
    std::string error;
    app::UserShellDestination dest = app::UserShellDestination::Notice;

    check(app::parse_user_shell_invocation("!ls -laFg", command, error, dest) && error.empty() &&
              command == "ls -laFg" && dest == app::UserShellDestination::Notice,
          "bang form parses notice destination");
    check(app::parse_user_shell_invocation("!!ls -laFg", command, error, dest) && error.empty() &&
              command == "ls -laFg" && dest == app::UserShellDestination::Draft,
          "double-bang form parses draft destination");
    check(app::parse_user_shell_invocation("/shell cat example.c", command, error, dest) &&
              error.empty() && command == "cat example.c" &&
              dest == app::UserShellDestination::Notice,
          "/shell form parses notice destination");
    check(app::parse_user_shell_invocation("/shell-stdout cat example.c", command, error, dest) &&
              error.empty() && command == "cat example.c" &&
              dest == app::UserShellDestination::Draft,
          "/shell-stdout form parses draft destination");
    check(app::parse_user_shell_invocation("!", command, error, dest) && !error.empty() &&
              dest == app::UserShellDestination::Notice,
          "bare bang reports usage error");
    check(app::parse_user_shell_invocation("!!", command, error, dest) && !error.empty() &&
              dest == app::UserShellDestination::Draft,
          "bare double-bang reports usage error");
    check(app::parse_user_shell_invocation("/shell", command, error, dest) && !error.empty(),
          "bare /shell reports usage error");
    check(app::parse_user_shell_invocation("/shell-stdout", command, error, dest) && !error.empty() &&
              dest == app::UserShellDestination::Draft,
          "bare /shell-stdout reports usage error");
    check(app::parse_user_shell_invocation("/shelling foo", command, error, dest) == false,
          "/shelling is not a shell invocation");
    check(app::parse_user_shell_invocation("hello", command, error, dest) == false,
          "ordinary text is not a shell invocation");
    check(app::parse_user_shell_invocation("!  echo hi  ", command, error, dest) && error.empty() &&
              command == "echo hi",
          "bang trims command body whitespace");
    check(app::parse_user_shell_invocation("!!  echo hi  ", command, error, dest) && error.empty() &&
              command == "echo hi" && dest == app::UserShellDestination::Draft,
          "double-bang trims command body whitespace");
}

void test_run_user_shell_echo() {
    const std::optional<std::string> previous_key =
        ainiux::test::test_environment("OPENAI_API_KEY");
    constexpr const char* inherited_secret = "ainiux-shell-secret-must-not-leak";
    ainiux::test::set_test_environment("OPENAI_API_KEY", inherited_secret);
    app::UserShellOptions options;
    options.timeout_ms = 5000;
    app::UserShellResult result;
    Error err = app::run_user_shell(
#if defined(_WIN32)
        "Write-Output hello-user-shell; Write-Output $env:OPENAI_API_KEY",
#else
        "printf 'hello-user-shell\\n%s\\n' \"$OPENAI_API_KEY\"",
#endif
        options, result);
    if (previous_key.has_value())
        ainiux::test::set_test_environment("OPENAI_API_KEY", *previous_key);
    else
        ainiux::test::unset_test_environment("OPENAI_API_KEY");
    check(err.ok(), "echo shell command succeeds");
    check(result.exit_status == 0, "echo exit status is 0");
    check(result.stdout_text.find("hello-user-shell") != std::string::npos,
          "echo stdout contains payload");
    check(result.stdout_text.find(inherited_secret) == std::string::npos,
          "user shell subprocess excludes inherited API keys");
    check(!result.cancelled && !result.timed_out, "echo was not cancelled or timed out");
}

void test_run_user_shell_nonzero_exit() {
    app::UserShellOptions options;
    options.timeout_ms = 5000;
    app::UserShellResult result;
    Error err = app::run_user_shell(
#if defined(_WIN32)
        "exit 9",
#else
        "false",
#endif
        options, result);
    check(err.ok(), "non-zero exit is still an ok Error");
    check(result.exit_status != 0, "false returns non-zero exit status");
}

void test_run_user_shell_empty() {
    app::UserShellOptions options;
    app::UserShellResult result;
    Error err = app::run_user_shell("   ", options, result);
    check(!err.ok() && err.code == ErrorCode::BadArgs, "empty command is BadArgs");
}

void test_run_user_shell_timeout() {
    app::UserShellOptions options;
    options.timeout_ms = 200;
    app::UserShellResult result;
    Error err = app::run_user_shell(
#if defined(_WIN32)
        "Start-Sleep -Seconds 5",
#else
        "sleep 5",
#endif
        options, result);
    check(!err.ok() && err.code == ErrorCode::Timeout, "short timeout returns Timeout");
    check(result.timed_out, "result.timed_out is set");
}

void test_format_and_provider_filter() {
    app::UserShellResult result;
    result.command = "echo hi";
    result.cwd = "/tmp";
    result.exit_status = 0;
    result.duration_ms = 3;
    result.stdout_text = "hi\n";
    result.stderr_text = "warn\n";
    const std::string notice = app::format_user_shell_notice(result);
    check(notice.find("$ echo hi") != std::string::npos, "notice includes command line");
    check(notice.find("hi") != std::string::npos, "notice includes stdout");
    check(notice.find("exit=0") != std::string::npos, "notice includes exit status");
    check(notice.find("warn") != std::string::npos, "notice includes stderr");

    const std::string draft = app::format_user_shell_draft_stdout(result);
    check(draft == "hi\n", "draft formatter is pure stdout");
    check(draft.find("exit=") == std::string::npos, "draft has no exit metadata");
    check(draft.find("warn") == std::string::npos, "draft has no stderr");

    const std::string secret = "sk-test-secret-value";
    result.stdout_text = std::string("token=") + secret + "\n";
    const std::string redacted = app::format_user_shell_draft_stdout(result, {secret});
    check(redacted.find(secret) == std::string::npos, "draft redacts secrets");

    std::vector<provider::Message> messages = {
        {"system", "sys"},
        {"user", "u"},
        {"notice", "shell out"},
        {"assistant", "a"},
        {"tool", "t"},
    };
    const std::vector<provider::Message> filtered = app::provider_chat_messages(messages);
    check(filtered.size() == 3, "provider filter keeps only chat roles");
    check(filtered[0].role == "system" && filtered[1].role == "user" &&
              filtered[2].role == "assistant",
          "provider filter order preserved for chat roles");
    check(app::is_provider_chat_role("user") && !app::is_provider_chat_role("notice"),
          "is_provider_chat_role distinguishes notice");
}

void test_shell_stdout_failure_messages() {
    app::UserShellResult ok_result;
    ok_result.command = "echo hi";
    ok_result.exit_status = 0;
    ok_result.stdout_text = "hi\n";
    check(!app::user_shell_failed(ok_error(), ok_result), "exit 0 is not a failure");

    app::UserShellResult fail;
    fail.command = "nosuch";
    fail.cwd = "/tmp";
    fail.exit_status = 127;
    fail.duration_ms = 2;
    fail.stderr_text = "/bin/sh: 1: nosuch: not found\n";
    check(app::user_shell_failed(ok_error(), fail), "exit 127 is a failure");

    const std::string status = app::format_user_shell_draft_status(ok_error(), fail);
    check(status.find("failed") != std::string::npos, "status says failed");
    check(status.find("127") != std::string::npos, "status includes exit code");
    check(status.find("nosuch") != std::string::npos, "status includes stderr snippet");

    const std::string diag = app::format_user_shell_failure_notice(ok_error(), fail);
    check(diag.find("shell-stdout failed") != std::string::npos, "failure notice header");
    check(diag.find("command: nosuch") != std::string::npos, "failure notice includes command");
    check(diag.find("exit: 127") != std::string::npos, "failure notice includes exit");
    check(diag.find("stderr:") != std::string::npos, "failure notice includes stderr section");
    check(diag.find("not found") != std::string::npos, "failure notice includes stderr body");

    app::UserShellResult cancelled;
    cancelled.command = "sleep 9";
    cancelled.cancelled = true;
    cancelled.exit_status = -1;
    Error cancel_err{ErrorCode::Cancelled, "shell command cancelled"};
    check(app::user_shell_failed(cancel_err, cancelled), "cancelled is a failure");
    const std::string cancel_status =
        app::format_user_shell_draft_status(cancel_err, cancelled);
    check(cancel_status.find("cancelled") != std::string::npos, "cancel status is clear");

    Error spawn_err{ErrorCode::FileRead, "could not find executable /bin/sh or /usr/bin/sh"};
    app::UserShellResult empty;
    empty.command = "echo x";
    check(app::user_shell_failed(spawn_err, empty), "spawn error is a failure");
    const std::string spawn_status = app::format_user_shell_draft_status(spawn_err, empty);
    check(spawn_status.find("error") != std::string::npos ||
              spawn_status.find("/bin/sh") != std::string::npos,
          "spawn failure status mentions error");
    const std::string spawn_diag = app::format_user_shell_failure_notice(spawn_err, empty);
    check(spawn_diag.find("FileRead") != std::string::npos ||
              spawn_diag.find("could not find") != std::string::npos,
          "spawn failure notice includes error detail");
}

}  // namespace

void run_all() {
    test_parse_user_shell_invocation();
    test_run_user_shell_echo();
    test_run_user_shell_nonzero_exit();
    test_run_user_shell_empty();
    test_run_user_shell_timeout();
    test_format_and_provider_filter();
    test_shell_stdout_failure_messages();
}

}  // namespace ainiux::test::app_user_shell
