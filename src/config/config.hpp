#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "cli/args.hpp"
#include "common.hpp"
#include "tui/theme_registry.hpp"

namespace pkchat::config {

constexpr size_t kMaxConfigBytes = 1024 * 1024;

struct SourceLocation {
    std::string path;
    size_t line = 1;
    size_t column = 1;
};

struct Value {
    enum class Type { Boolean, Integer, Float, String };

    Type type = Type::String;
    bool boolean = false;
    std::int64_t integer = 0;
    double floating = 0.0;
    std::string string;

    bool is_boolean() const { return type == Type::Boolean; }
    bool is_integer() const { return type == Type::Integer; }
    bool is_float() const { return type == Type::Float; }
    bool is_string() const { return type == Type::String; }
};

struct Entry {
    std::string section;
    std::string key;
    Value value;
    SourceLocation source;
};

struct Document {
    std::map<std::string, Entry> entries;

    const Entry* find(const std::string& qualified_key) const;
    const Entry* find(const std::string& section, const std::string& key) const;
};

struct ParseResult {
    Document document;
    Error error;
};

struct Environment {
    std::string xdg_config_home;
    std::string xdg_config_dirs;
    std::string home;
};

enum class ConfigScope { System, User };
enum class ConfigFileKind { Config, EditorCommands, Themes };
enum class ConfigFileState { Loaded, Missing, Skipped, Error, Unavailable };

struct ConfigDiagnostic {
    ConfigScope scope = ConfigScope::System;
    ConfigFileKind kind = ConfigFileKind::Config;
    ConfigFileState state = ConfigFileState::Missing;
    std::string path;
};

struct LoadResult {
    cli::Options options;
    std::vector<std::string> loaded_paths;
    std::vector<ConfigDiagnostic> diagnostics;
    Error error;
};

ParseResult parse(const std::string& input, const std::string& source_path = "<memory>");
ParseResult read_file(const std::string& path, size_t max_bytes = kMaxConfigBytes);
Error apply_document(const Document& document, cli::Options& options);
Error apply_editor_commands_document(const Document& document, cli::Options& options);
Error apply_themes_document(const Document& document, cli::Options& options);
Environment process_environment();
std::string user_config_path(const Environment& environment);
std::string user_editor_commands_path(const Environment& environment);
std::string user_themes_path(const Environment& environment);
std::vector<std::string> system_config_paths(const Environment& environment);
std::vector<std::string> system_editor_commands_paths(const Environment& environment);
std::vector<std::string> system_themes_paths(const Environment& environment);
std::vector<std::string> bundled_editor_commands_paths();
std::vector<std::string> bundled_themes_paths();
LoadResult load_automatic(const cli::Options& base_options,
                          const Environment& environment,
                          bool load_user_config = true);
const char* value_type_name(Value::Type type);

}  // namespace pkchat::config
