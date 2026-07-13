#include <vector>

template <typename T>
T first_or(const std::vector<T>& values, T fallback) {
    return values.empty() ? fallback : values.front();
}
