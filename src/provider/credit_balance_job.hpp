#pragma once

#include <functional>

#include "common.hpp"
#include "provider/provider.hpp"
#include "runtime/runtime.hpp"

namespace ainiux::provider {

using CreditBalanceJobCallback =
    std::function<void(Error error, CreditBalanceResult result)>;

void start_credit_balance_job(runtime::JobHandle& job,
                              RequestContext context,
                              CreditBalanceJobCallback deliver);

}  // namespace ainiux::provider
