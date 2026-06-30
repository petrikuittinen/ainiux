#pragma once

#include <string>

#include "common.hpp"

namespace pkchat::editor {

std::string embedded_editor_help_markdown();
Error load_editor_help_markdown(std::string& out);
bool is_editor_help_command(const std::string& line);

}  // namespace pkchat::editor