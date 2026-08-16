#include "agent/project_scripts.hpp"

#include <algorithm>
#include <filesystem>
#include <sstream>

#include "platform/filesystem.hpp"

namespace ainiux::agent {
namespace fs = std::filesystem;

bool portable_project_script_name(const std::string& name) {
    if (name.empty() || name.size() > 128 || name == "." || name == ".." ||
        name.back() == '.' || name.back() == ' ')
        return false;
    for (unsigned char ch : name) {
        const bool allowed = (ch >= 'a' && ch <= 'z') ||
                             (ch >= 'A' && ch <= 'Z') ||
                             (ch >= '0' && ch <= '9') || ch == '.' ||
                             ch == '_' || ch == '-';
        if (!allowed) return false;
    }
    return platform::validate_windows_path_syntax(name).ok();
}

bool project_script_path(const std::string& path, std::string* name) {
#if defined(_WIN32)
    if (!platform::validate_windows_path_syntax(path).ok()) return false;
#endif
    const fs::path candidate = fs::u8path(path);
    if (candidate.is_absolute()) return false;
    std::vector<std::string> components;
    for (const fs::path& component : candidate) {
        const std::string part = component.u8string();
        if (part.empty() || part == ".") continue;
        components.push_back(part);
    }
    if (components.size() != 3 || components[0] != "scripts" ||
        components[1] != "ainiux" || !portable_project_script_name(components[2]))
        return false;
    if (name != nullptr) *name = components[2];
    return true;
}

bool project_scripts_directory(const std::string& path) {
    const std::string generic = fs::u8path(path).generic_u8string();
    return generic == kProjectScriptsDirectory || generic == "scripts/ainiux/";
}

bool retired_project_script_path(const std::string& path) {
    if (path.empty()) return false;
    std::string generic = fs::u8path(path).generic_u8string();
    while (generic.size() >= 2 && generic[0] == '.' && generic[1] == '/')
        generic.erase(0, 2);
    const auto has_retired = [](const std::string& value) {
        return value.find(".ainiux-pr/scripts") != std::string::npos ||
               value.find(".ainiux-pr/script/") != std::string::npos ||
               (value.size() >= 16 &&
                value.compare(value.size() - 16, 16, ".ainiux-pr/script") == 0);
    };
    if (has_retired(generic)) return true;
#if defined(_WIN32)
    for (char& ch : generic)
        if (ch == '\\') ch = '/';
    if (has_retired(generic)) return true;
#endif
    return false;
}

const char* retired_project_script_message() {
    return "reusable scripts live at scripts/ainiux/NAME; .ainiux-pr/scripts was removed. "
           "Do not list or read that folder. Write scripts/ainiux/NAME instead.";
}

bool project_script_invocation(const std::vector<std::string>& arguments,
                               std::string* interpreter,
                               std::string* relative_path) {
    if (arguments.empty()) return false;
    std::string name;
    auto set_relative = [&](const std::string& raw) {
        if (relative_path == nullptr) return;
        *relative_path = std::string(kProjectScriptsDirectory) + "/" + name;
        (void)raw;
    };
    if (project_script_path(arguments[0], &name)) {
        if (interpreter != nullptr) *interpreter = {};
        set_relative(arguments[0]);
        return true;
    }
    if (arguments.size() < 2) return false;
    const std::string& command = arguments[0];
    if (command != "bash" && command != "sh" && command != "python3" &&
        command != "python")
        return false;
    if (!project_script_path(arguments[1], &name)) return false;
    if (interpreter != nullptr) *interpreter = command;
    set_relative(arguments[1]);
    return true;
}

Error list_project_scripts(const std::string& workspace, std::vector<std::string>& names) {
    names.clear();
    if (workspace.empty()) return ok_error();
    const fs::path directory =
        fs::u8path(workspace) / "scripts" / "ainiux";
    std::error_code ec;
    if (!fs::is_directory(directory, ec) || ec) return ok_error();
    bool linked = false;
    Error link_error = platform::path_contains_link_or_reparse(directory.u8string(), linked);
    if (!link_error.ok() || linked) return ok_error();
    fs::directory_iterator it(directory, fs::directory_options::skip_permission_denied, ec);
    if (ec) return ok_error();
    for (const fs::directory_entry& entry : it) {
        const std::string name = entry.path().filename().u8string();
        if (!portable_project_script_name(name)) continue;
        const fs::file_status status = entry.symlink_status(ec);
        if (ec || fs::is_symlink(status) || !fs::is_regular_file(status)) continue;
        names.push_back(name);
    }
    std::sort(names.begin(), names.end());
    return ok_error();
}

std::string project_script_catalog_text(const std::vector<std::string>& names) {
    if (names.empty()) return {};
    std::ostringstream out;
    out << "Project scripts under scripts/ainiux/ (reuse these; do not rewrite as "
           "python3 -c):\n";
    for (const std::string& name : names) out << "- scripts/ainiux/" << name << "\n";
    return out.str();
}

}  // namespace ainiux::agent
