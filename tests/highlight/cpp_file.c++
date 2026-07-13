#include <memory>

struct Widget { virtual ~Widget() = default; };
auto widget = std::make_unique<Widget>();
