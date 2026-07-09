// Copyright (c) 2026 Petri Kuittinen
//
// Licensed under the Modified MIT License. See LICENSE in the repository root.

#include "pkchat/version.hpp"

#include <string>

namespace pkchat {

const char appName[] = "Pkchat";

const char versionNumber[] = "0.93";

const char kCopyright[] = "Copyright (c) 2026 Petri Kuittinen";

const char kLicenseName[] = "Modified MIT License";

const std::string& app_version_label() {
    static const std::string label = std::string(appName) + " v" + versionNumber;
    return label;
}

}  // namespace pkchat
