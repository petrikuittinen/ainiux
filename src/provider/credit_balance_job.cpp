#include "provider/credit_balance_job.hpp"

namespace ainiux::provider {

void start_credit_balance_job(runtime::JobHandle& job,
                              RequestContext context,
                              CreditBalanceJobCallback deliver) {
    job.start([context = std::move(context), deliver = std::move(deliver)](
                  runtime::CancellationToken token) mutable {
        CreditBalanceResult result;
        Error error = get_credit_balance(context, result, token);
        deliver(std::move(error), std::move(result));
    });
}

}  // namespace ainiux::provider
