#include "agent/prompts.hpp"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

#include "embedded_agent_prompts.hpp"

namespace ainiux::agent {
namespace {
namespace fs = std::filesystem;

Error read_prompt(const fs::path& path, std::string& output) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return {ErrorCode::FileRead, "could not open trusted prompt: " + path.string()};
    output.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    if (input.bad()) return {ErrorCode::FileRead, "could not read trusted prompt: " + path.string()};
    if (output.empty()) return {ErrorCode::FileRead, "trusted prompt is empty: " + path.string()};
    return ok_error();
}

Error load_directory(const fs::path& directory, TrustedPrompts& prompts) {
    Error error = read_prompt(directory / "master_prompt.md", prompts.master);
    if (!error.ok()) return error;
    error = read_prompt(directory / "security_prompt.md", prompts.security);
    if (!error.ok()) return error;
    return read_prompt(directory / "coding_prompt.md", prompts.coding);
}

// Always insert exactly one separating newline between non-empty parts so
// joined prompts stay stable when source Markdown files end with a newline.
std::string join_prompt_parts(const std::string& left, const std::string& right) {
    if (left.empty()) return right;
    if (right.empty()) return left;
    std::string head = left;
    while (!head.empty() && (head.back() == '\n' || head.back() == '\r')) head.pop_back();
    std::string tail = right;
    while (!tail.empty() && (tail.front() == '\n' || tail.front() == '\r')) tail.erase(tail.begin());
    return head + "\n" + tail;
}

std::string protocol_block(ToolProtocol protocol) {
    const char* appendix =
        protocol == ToolProtocol::Xml ? xml_protocol_appendix() : native_protocol_appendix();
    std::string block = appendix;
    if (!block.empty() && block.front() == '\n') block.erase(block.begin());
    while (!block.empty() && (block.back() == '\n' || block.back() == ' ')) block.pop_back();
    return block;
}

}  // namespace

const char* native_protocol_appendix() {
    return R"AINIUX_NATIVE(
## Active channel: native tools

This session uses provider-native tool calling. Use the provided tools only. Do not describe tool calls in prose, and do not emit XML-style <tool_call> markup.
)AINIUX_NATIVE";
}

const char* xml_protocol_appendix() {
    return R"AINIUX_XML(
## Active channel: XML tool markup

This session does not use provider-native tool calling. Emit exactly one <tool_call> block per assistant turn, nothing after it. Arguments must be one valid JSON object. Do not wrap the block in Markdown code fences.

Example:

<tool_call>
<name>read_file</name>
<args>{"path":"src/main.cpp","max_bytes":65536}</args>
</tool_call>
)AINIUX_XML";
}

std::string TrustedPrompts::security_system_prompt() const {
    // Preserve the historical exact concatenation used by security-review.
    return master + "\n" + security;
}

std::string TrustedPrompts::agent_system_prompt(ToolProtocol protocol) const {
    // Default agent: master + coding task layer + static protocol appendix.
    return join_prompt_parts(join_prompt_parts(master, coding), protocol_block(protocol));
}

Error load_trusted_prompts(const std::string& override_directory, TrustedPrompts& prompts) {
    if (!override_directory.empty()) return load_directory(fs::path(override_directory), prompts);

    const std::vector<fs::path> installed = {
        fs::path("/usr/local/share/ainiux/prompts"),
        fs::path("/usr/share/ainiux/prompts")};
    for (const fs::path& directory : installed) {
        std::error_code error;
        if (fs::is_directory(directory, error) && !error) {
            TrustedPrompts loaded;
            const Error load_error = load_directory(directory, loaded);
            if (load_error.ok()) {
                prompts = std::move(loaded);
                return ok_error();
            }
        }
    }
    prompts.master = kEmbeddedMasterPrompt;
    prompts.security = kEmbeddedSecurityPrompt;
    prompts.coding = kEmbeddedCodingPrompt;
    return ok_error();
}

void seed_agent_conversation(provider::ToolConversation& conversation,
                             const TrustedPrompts& prompts,
                             ToolProtocol protocol,
                             const std::string& user_goal,
                             const std::string& agents_md_injection) {
    conversation = provider::ToolConversation{};
    conversation.messages.push_back({"system", prompts.agent_system_prompt(protocol)});
    // Keep the system prompt static for provider-side caching; project rules are
    // separate user-role context and remain untrusted data.
    if (!agents_md_injection.empty())
        conversation.messages.push_back({"user", agents_md_injection});
    if (!user_goal.empty()) conversation.messages.push_back({"user", user_goal});
}

}  // namespace ainiux::agent
