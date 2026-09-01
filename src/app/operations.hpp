#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include "common.hpp"
#include "config/image_catalog.hpp"
#include "provider/image.hpp"
#include "provider/provider.hpp"
#include "runtime/runtime.hpp"

namespace ainiux::app::operation {

// Surface-neutral events emitted by reusable application operations. Protocol
// adapters convert these to stable public DTOs instead of serializing this enum.
enum class EventType { Started, Delta, Progress, Completed };

struct Event {
    EventType type = EventType::Progress;
    std::string text;
    std::size_t current = 0;
    std::size_t total = 0;
};

using EventSink = std::function<Error(const Event&)>;

struct ChatRequest {
    std::vector<provider::Message> messages;
};

struct ChatResult {
    Error error;
    provider::ChatResult response;
};

using ChatExecutor = std::function<Error(
    const provider::RequestContext&,
    const std::vector<provider::Message>&,
    provider::DeltaCallback,
    provider::ChatResult&,
    runtime::CancellationToken)>;

ChatResult run_chat(const provider::RequestContext& context,
                    const ChatRequest& request,
                    runtime::CancellationToken cancellation = runtime::CancellationToken(),
                    EventSink events = {},
                    ChatExecutor executor = {});

struct ImageRequest {
    std::string model;
    std::string prompt;
    std::vector<std::string> attachment_paths;
    std::string size;
    std::string aspect;
    std::string quality;
    std::string format;
    bool format_explicit = false;
    std::size_t max_image_bytes = 20U * 1024U * 1024U;
};

struct ImageResult {
    Error error;
    std::string selected_model;
    provider::ImageGenerateRequest request;
    provider::ImageGenerateResult response;
};

using ImageExecutor = std::function<Error(
    const provider::RequestContext&,
    const provider::ImageGenerateRequest&,
    provider::ImageGenerateResult&,
    runtime::CancellationToken)>;

ImageResult run_image(provider::RequestContext context,
                      const ImageRequest& request,
                      runtime::CancellationToken cancellation = runtime::CancellationToken(),
                      EventSink events = {},
                      ImageExecutor executor = {});

}  // namespace ainiux::app::operation
