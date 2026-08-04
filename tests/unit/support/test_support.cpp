#include "support/test_support.hpp"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>

namespace ainiux::test {

int failures = 0;

void check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

std::string read_fixture(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    check(input.is_open(), "fixture opens: " + path);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

std::optional<std::string> test_environment(const char* name) {
    const char* value = std::getenv(name);
    if (value == nullptr) return std::nullopt;
    return std::string(value);
}

void set_test_environment(const char* name, const std::string& value) {
#if defined(_WIN32)
    (void)_putenv_s(name, value.c_str());
#else
    (void)::setenv(name, value.c_str(), 1);
#endif
}

void unset_test_environment(const char* name) {
#if defined(_WIN32)
    (void)_putenv_s(name, "");
#else
    (void)::unsetenv(name);
#endif
}

}  // namespace ainiux::test
