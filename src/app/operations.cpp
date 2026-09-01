#include "app/operations.hpp"

#include <sstream>
#include <utility>

#include "input/input.hpp"

namespace ainiux::app::operation {
namespace {

Error cancelled_error() {
    return {ErrorCode::Cancelled, "operation cancelled"};
}

Error publish(const EventSink& sink, Event event) {
    if (!sink) return ok_error();
    return sink(event);
}

Error load_image_attachments(const provider::RequestContext& context,
                             const ImageCapability& capability,
                             const ImageRequest& request,
                             runtime::CancellationToken cancellation,
                             const EventSink& events,
                             std::vector<provider::ImageInput>& images) {
    for (const std::string& path : request.attachment_paths) {
        if (cancellation.cancelled()) return cancelled_error();
        input::FileType type;
        Error error = input::classify_file_type(path, type);
        if (!error.ok()) return error;
        if (type.kind != input::Kind::Image) {
            return {ErrorCode::BadArgs,
                    "image mode --attach supports PNG and JPEG only: " + path};
        }
        if (type.mime_type != "image/png" && type.mime_type != "image/jpeg") {
            return {ErrorCode::BadArgs,
                    "image mode --attach supports PNG and JPEG only; GIF and other types "
                    "are not used as image references: " + path};
        }
        if (!capability.edits) {
            return {ErrorCode::BadArgs,
                    "image model " + context.options.model +
                        " does not support --attach reference images"};
        }
        input::ImageData loaded;
        error = input::load_image_file(path, type, request.max_image_bytes, loaded, cancellation);
        if (!error.ok()) return error;
        provider::ImageInput image{loaded.mime_type, std::move(loaded.base64_data)};
        image.display_name = path;
        image.source_ref = path;
        image.byte_size = static_cast<long long>(loaded.byte_size);
        images.push_back(std::move(image));
        const int max_inputs = capability.max_input_images > 0
                                   ? capability.max_input_images
                                   : provider::kMaxImageEditInputs;
        if (static_cast<int>(images.size()) > max_inputs) {
            return {ErrorCode::BadArgs,
                    "this image model accepts at most " + std::to_string(max_inputs) +
                        " --attach images"};
        }
        std::ostringstream status;
        status << "Attached image: " << path << " (" << loaded.mime_type << ", "
               << loaded.byte_size << " bytes)";
        error = publish(events, {EventType::Progress, status.str(), images.size(),
                                 static_cast<std::size_t>(max_inputs)});
        if (!error.ok()) return error;
    }
    return ok_error();
}

}  // namespace

ChatResult run_chat(const provider::RequestContext& context,
                    const ChatRequest& request,
                    runtime::CancellationToken cancellation,
                    EventSink events,
                    ChatExecutor executor) {
    ChatResult result;
    if (cancellation.cancelled()) {
        result.error = cancelled_error();
        return result;
    }
    if (request.messages.empty()) {
        result.error = {ErrorCode::BadArgs, "chat operation requires at least one message"};
        return result;
    }

    result.error = publish(events, {EventType::Started, {}, 0, 0});
    if (!result.error.ok()) return result;

    if (!executor) executor = provider::send_chat_messages;
    auto on_delta = [&](const std::string& delta) -> Error {
        if (cancellation.cancelled()) return cancelled_error();
        return publish(events, {EventType::Delta, delta, 0, 0});
    };
    result.error = executor(context, request.messages, on_delta, result.response, cancellation);
    if (!result.error.ok()) return result;
    if (cancellation.cancelled()) {
        result.error = cancelled_error();
        return result;
    }
    result.error = publish(events, {EventType::Completed, {}, 0, 0});
    return result;
}

ImageResult run_image(provider::RequestContext context,
                      const ImageRequest& input,
                      runtime::CancellationToken cancellation,
                      EventSink events,
                      ImageExecutor executor) {
    ImageResult result;
    if (cancellation.cancelled()) {
        result.error = cancelled_error();
        return result;
    }
    const std::string provider_name = provider::canonical_profile_name(context.options.provider);
    result.selected_model = input.model;
    if (result.selected_model.empty()) {
        result.selected_model =
            config::default_image_model(context.options.image_catalog, provider_name);
    }
    if (result.selected_model.empty()) {
        result.error = {ErrorCode::BadArgs,
                        "image mode requires -m/--model or an images.conf default for provider " +
                            provider_name};
        return result;
    }
    context.options.model = result.selected_model;

    const ImageCapability* capability = config::resolve_image_capability(
        context.options.image_catalog, provider_name, result.selected_model);
    if (capability == nullptr) {
        result.error = {
            ErrorCode::BadArgs,
            "images.conf has no record for provider " + provider_name + " model " +
                result.selected_model + "; known models: " +
                config::known_image_models_description(context.options.image_catalog,
                                                        provider_name)};
        return result;
    }
    if (!config::image_protocol_implemented(capability->protocol)) {
        result.error = {
            ErrorCode::UnsupportedFeature,
            std::string("image protocol ") + config::image_protocol_name(capability->protocol) +
                " is not built into this ainiux"};
        return result;
    }

    result.error = provider::resolve_image_size(
        *capability, input.size, input.aspect, result.request.size, result.request.aspect);
    if (!result.error.ok()) return result;
    result.error =
        provider::normalize_image_quality(*capability, input.quality, result.request.quality);
    if (!result.error.ok()) return result;
    result.error = provider::normalize_image_format(
        *capability, input.format_explicit ? input.format : capability->format_default,
        result.request.output_format);
    if (!result.error.ok()) return result;
    result.request.prompt = ascii_trim(input.prompt);
    if (result.request.prompt.empty()) {
        result.error = {ErrorCode::BadArgs, "image mode requires a non-empty prompt"};
        return result;
    }
    result.error = load_image_attachments(context, *capability, input, cancellation, events,
                                          result.request.images);
    if (!result.error.ok()) return result;

    result.request.capability = *capability;
    result.request.protocol = capability->protocol;
    result.request.model =
        ((capability->protocol == ImageProtocol::ReplicatePredictions ||
          capability->protocol == ImageProtocol::FalQueue ||
          capability->protocol == ImageProtocol::GeminiInteractions) &&
         !capability->api_model.empty())
            ? capability->api_model
            : result.selected_model;

    std::ostringstream status;
    status << "Generating image with " << result.request.model;
    if (!result.request.size.empty()) status << " (" << result.request.size;
    else status << " (size auto";
    if (!result.request.aspect.empty()) status << ", ar " << result.request.aspect;
    status << ", quality "
           << (result.request.quality.empty() ? "auto" : result.request.quality) << ", "
           << (result.request.output_format.empty() ? "auto" : result.request.output_format)
           << ")";
    if (provider::image_size_is_experimental(result.request.size)) {
        status << " [experimental size]";
    }
    result.error = publish(events, {EventType::Started, status.str(), 0, 0});
    if (!result.error.ok()) return result;
    if (!executor) executor = provider::generate_or_edit_image;
    result.error = executor(context, result.request, result.response, cancellation);
    // Reference images can be large temporary base64 buffers. They are request
    // input, not operation output, so release them immediately after transport.
    result.request.images.clear();
    if (!result.error.ok()) return result;
    if (cancellation.cancelled()) {
        result.error = cancelled_error();
        return result;
    }
    if (result.response.bytes.empty()) {
        result.error = {ErrorCode::ProviderSchema, "image response decoded to an empty file"};
        return result;
    }
    result.error = publish(events, {EventType::Completed, {}, 0, 0});
    return result;
}

}  // namespace ainiux::app::operation
