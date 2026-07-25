#pragma once

#include <functional>
#include <string>
#include <vector>

#include "common.hpp"
#include "provider/provider.hpp"
#include "runtime/runtime.hpp"

namespace ainiux::editor {

struct EditorModelsEvent {
    Error error;
    provider::ModelsResult models;
};

struct EditorModelListRuntime {
    runtime::JobHandle job;
    runtime::EventQueue<EditorModelsEvent> events;

    void start(const provider::RequestContext& context);
    bool process(const std::function<void(provider::ModelsResult)>& on_success,
                 const std::function<void(const std::string&)>& on_error);
};

}  // namespace ainiux::editor
