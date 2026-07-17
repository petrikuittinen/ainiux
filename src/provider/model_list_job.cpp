#include "provider/model_list_job.hpp"

namespace ainiux::provider {

void start_list_models_job(runtime::JobHandle& job,
                           RequestContext context,
                           ModelListJobCallback deliver) {
    job.start([context = std::move(context), deliver = std::move(deliver)](
                  runtime::CancellationToken token) mutable {
        ModelsResult models;
        Error error = list_models(context, models, token);
        deliver(std::move(error), std::move(models));
    });
}

}  // namespace ainiux::provider