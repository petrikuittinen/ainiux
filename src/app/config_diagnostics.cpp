#include "app/app.hpp"

#include <iostream>

namespace ainiux::app {

void print_config_diagnostics(const config::LoadResult& configured) {
    for (const config::ConfigDiagnostic& diagnostic : configured.diagnostics) {
        const char* scope = diagnostic.scope == config::ConfigScope::Bundled ? "bundled" : "user";
        const char* state = "missing";
        switch (diagnostic.state) {
            case config::ConfigFileState::Loaded:
                state = "loaded";
                break;
            case config::ConfigFileState::Missing:
                break;
            case config::ConfigFileState::Skipped:
                state = "skipped (--no-config)";
                break;
            case config::ConfigFileState::Error:
                state = "failed";
                break;
            case config::ConfigFileState::Unavailable:
                state = "path unavailable";
                break;
        }
        const char* kind = "config";
        if (diagnostic.kind == config::ConfigFileKind::EditorCommands) {
            kind = "editor commands";
        } else if (diagnostic.kind == config::ConfigFileKind::Themes) {
            kind = "themes";
        } else if (diagnostic.kind == config::ConfigFileKind::Benchmarks) {
            kind = "benchmark prompts";
        } else if (diagnostic.kind == config::ConfigFileKind::Models) {
            kind = "models";
        } else if (diagnostic.kind == config::ConfigFileKind::Images) {
            kind = "images";
        }
        std::cerr << "Config debug: " << state << " " << scope << " " << kind;
        if (!diagnostic.path.empty()) {
            std::cerr << ": " << diagnostic.path;
        }
        std::cerr << "\n";
    }
}

}  // namespace ainiux::app
