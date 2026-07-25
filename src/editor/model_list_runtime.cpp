#include "editor/model_list_runtime.hpp"

#include "provider/model_list_job.hpp"

namespace ainiux::editor {

void EditorModelListRuntime::start(const provider::RequestContext& context) {
    provider::start_list_models_job(job, context, [this](Error error, provider::ModelsResult models) {
        EditorModelsEvent event;
        event.error = std::move(error);
        event.models = std::move(models);
        events.push(std::move(event));
    });
}

bool EditorModelListRuntime::process(
                                     const std::function<void(provider::ModelsResult)>& on_success,
                                     const std::function<void(const std::string&)>& on_error) {
    EditorModelsEvent event;
    if (!events.try_pop(event)) {
        return false;
    }
    job.join();
    if (!event.error.ok()) {
        on_error(event.error.message);
        return true;
    }
    if (event.models.model_ids.empty()) {
        on_error("No models returned");
        return true;
    }
    on_success(std::move(event.models));
    return true;
}

}  // namespace ainiux::editor
