inline constexpr int clamp_positive(int value) {
    return value < 0 ? 0 : value;
}
