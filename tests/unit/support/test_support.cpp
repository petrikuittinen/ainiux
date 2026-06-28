#include "support/test_support.hpp"

#include <fstream>
#include <iostream>
#include <iterator>

namespace pkchat::test {

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

}  // namespace pkchat::test