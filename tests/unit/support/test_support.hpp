#pragma once

#include <optional>
#include <string>

namespace ainiux::test {

extern int failures;

void check(bool condition, const std::string& message);
std::string read_fixture(const std::string& path);
std::optional<std::string> test_environment(const char* name);
void set_test_environment(const char* name, const std::string& value);
void unset_test_environment(const char* name);

}  // namespace ainiux::test
