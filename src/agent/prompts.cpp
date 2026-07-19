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
    return read_prompt(directory / "security_prompt.md", prompts.security);
}

}  // namespace

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
    return ok_error();
}

}  // namespace ainiux::agent
