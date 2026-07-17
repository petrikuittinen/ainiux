#include "context/policy.hpp"

namespace ainiux::context::policy {

const std::vector<const char*>& values() {
    static const std::vector<const char*> kValues = {
        kError,
        kTruncateOldest,
        kTruncateMiddle,
        kSummarizeOldest,
        kSummarizeMiddle,
        kProviderAuto,
    };
    return kValues;
}

std::vector<std::string> value_strings() {
    std::vector<std::string> out;
    out.reserve(values().size());
    for (const char* value : values()) {
        out.emplace_back(value);
    }
    return out;
}

bool is_valid(const std::string& policy) {
    for (const char* value : values()) {
        if (policy == value) {
            return true;
        }
    }
    return false;
}

std::string usage_description() {
    return "error, truncate-oldest, truncate-middle, summarize-oldest, summarize-middle, "
           "or provider-auto";
}

}  // namespace ainiux::context::policy