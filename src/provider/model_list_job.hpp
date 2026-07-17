#pragma once

#include <functional>
#include <string>
#include <vector>

#include "common.hpp"
#include "provider/provider.hpp"
#include "runtime/runtime.hpp"

namespace ainiux::provider {

using ModelListJobCallback = std::function<void(Error error, ModelsResult models)>;

void start_list_models_job(runtime::JobHandle& job,
                           RequestContext context,
                           ModelListJobCallback deliver);

}  // namespace ainiux::provider