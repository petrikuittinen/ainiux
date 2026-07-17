#pragma once

#include <string>

namespace ainiux::test {

extern int failures;

void check(bool condition, const std::string& message);
std::string read_fixture(const std::string& path);

}  // namespace ainiux::test