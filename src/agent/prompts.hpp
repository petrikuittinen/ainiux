#pragma once

#include <string>
#include <vector>

#include "agent/agent_loop.hpp"
#include "common.hpp"

namespace ainiux::agent {

enum class AgentLane { Act, Lead };
const char* agent_lane_name(AgentLane mode);

// Trusted prompts are never loaded from the reviewed/agent workspace.
// agent_prompt.md is the stable agent foundation (base + protocol).
// master_prompt.md and security_prompt.md are retained exclusively for the
// byte-stable security-review prompt (master + security).
struct TrustedPrompts {
    std::string master;
    std::string security;
    std::string agent;

    // security-review system prompt: master + security task layer.
    std::string security_system_prompt() const;

    // Stable agent session system prompt: agent + protocol appendix.
    std::string agent_system_prompt(ToolProtocol protocol) const;
};

// Static protocol appendices (trusted code, not workspace files).
const char* native_protocol_appendix();
const char* xml_protocol_appendix();
std::string agent_lane_control(
    AgentLane mode,
    const std::vector<std::string>& project_scripts = {});

Error load_trusted_prompts(const std::string& override_directory, TrustedPrompts& prompts);

// Build the initial conversation with a stable system prompt, optional framed
// AGENTS.md project instructions, the initial lane control, and the first goal.
void seed_agent_conversation(provider::ToolConversation& conversation,
                             const TrustedPrompts& prompts,
                             AgentLane mode,
                             ToolProtocol protocol,
                             const std::string& user_goal,
                             const std::string& agents_md_injection = {},
                             const std::vector<std::string>& project_scripts = {});

}  // namespace ainiux::agent
