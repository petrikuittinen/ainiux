#pragma once

#include <string>

#include "agent/agent_loop.hpp"
#include "common.hpp"

namespace ainiux::agent {

// Trusted prompts are never loaded from the reviewed/agent workspace.
// master_prompt.md is the shared foundation; security_prompt.md is the
// security-review task layer only (keep it task-specific).
struct TrustedPrompts {
    std::string master;
    std::string security;

    // security-review system prompt: master + security task layer.
    std::string security_system_prompt() const;

    // Agent session system prompt: master + a static per-session protocol
    // appendix. Keep this text stable for the whole session so provider-side
    // prompt caching works; inject per-turn notices as separate messages.
    std::string agent_system_prompt(ToolProtocol protocol) const;
};

// Static protocol appendices (trusted code, not workspace files).
const char* native_protocol_appendix();
const char* xml_protocol_appendix();

Error load_trusted_prompts(const std::string& override_directory, TrustedPrompts& prompts);

// Build the initial agent conversation with a static system prompt, optional
// untrusted project AGENTS.md injection (as a separate user message), and the
// first user goal. agents_md_injection must already be framed as untrusted data.
void seed_agent_conversation(provider::ToolConversation& conversation,
                             const TrustedPrompts& prompts,
                             ToolProtocol protocol,
                             const std::string& user_goal,
                             const std::string& agents_md_injection = {});

}  // namespace ainiux::agent
