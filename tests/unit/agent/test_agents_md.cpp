#include "agent/test_agents_md.hpp"

#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

#include "agent/agents_md.hpp"
#include "agent/prompts.hpp"
#include "support/test_support.hpp"

namespace ainiux::test::agent_agents_md {
namespace {
using ainiux::test::check;
namespace fs = std::filesystem;

std::string temp_workspace(const std::string& name) {
    const fs::path root =
        fs::temp_directory_path() / ("ainiux-agents-md-" + name + "-" +
                                     std::to_string(static_cast<long long>(::getpid())));
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    return root.string();
}

void test_missing_agents_md() {
    const std::string workspace = temp_workspace("missing");
    agent::AgentsMdBundle bundle;
    const Error error = agent::load_root_agents_md(workspace, 20000, bundle);
    check(error.ok(), "missing AGENTS.md is not an error");
    check(bundle.documents.empty() && bundle.injection_text.empty(), "empty bundle when missing");
    std::error_code ec;
    fs::remove_all(workspace, ec);
}

void test_load_and_seed_injection() {
    const std::string workspace = temp_workspace("present");
    {
        std::ofstream out(fs::path(workspace) / "AGENTS.md");
        out << "# Project rules\nUse KISS.\n";
    }
    agent::AgentsMdBundle bundle;
    const Error error = agent::load_root_agents_md(workspace, 20000, bundle);
    check(error.ok(), "load root AGENTS.md");
    check(bundle.documents.size() == 1 && bundle.documents[0].path == "AGENTS.md",
          "one root document");
    check(bundle.injection_text.find("untrusted") != std::string::npos, "injection marks untrusted");
    check(bundle.injection_text.find("Use KISS.") != std::string::npos, "injection includes content");

    agent::TrustedPrompts prompts;
    prompts.master = "MASTER";
    prompts.security = "SECURITY";
    provider::ToolConversation conversation;
    agent::seed_agent_conversation(conversation, prompts, agent::ToolProtocol::Native, "Do the task",
                                   bundle.injection_text);
    check(conversation.messages.size() == 3, "system + agents.md + goal");
    check(conversation.messages[0].role == "system" &&
              conversation.messages[0].content.find("MASTER") != std::string::npos,
          "system stays trusted master");
    check(conversation.messages[1].role == "user" &&
              conversation.messages[1].content.find("AGENTS.md") != std::string::npos,
          "AGENTS.md is user-role context");
    check(conversation.messages[2].role == "user" && conversation.messages[2].content == "Do the task",
          "goal is final user message");

    std::error_code ec;
    fs::remove_all(workspace, ec);
}

void test_truncation() {
    const std::string workspace = temp_workspace("truncate");
    {
        std::ofstream out(fs::path(workspace) / "AGENTS.md");
        out << std::string(500, 'a');
    }
    agent::AgentsMdBundle bundle;
    const Error error = agent::load_root_agents_md(workspace, 50, bundle);
    check(error.ok() && bundle.truncated && bundle.documents[0].truncated, "truncates over budget");
    check(bundle.total_bytes <= 50, "respects byte budget");
    std::error_code ec;
    fs::remove_all(workspace, ec);
}

}  // namespace

void run_all() {
    test_missing_agents_md();
    test_load_and_seed_injection();
    test_truncation();
}

}  // namespace ainiux::test::agent_agents_md
