template <typename T>
class Box {
public:
    explicit Box(T value) : value_(value) {}
private:
    T value_;
};
