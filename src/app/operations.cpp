#include "app/operations.hpp"

#include <sstream>
#include <utility>

#include "input/input.hpp"
#include "platform/filesystem.hpp"
#include "config/video_catalog.hpp"

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
    const int max_inputs = capability.max_input_images > 0
                               ? capability.max_input_images
                               : provider::kMaxImageEditInputs;
    if (!request.input_images.empty() && !capability.edits) {
        return {ErrorCode::BadArgs,
                "image model " + context.options.model +
                    " does not support reference images"};
    }
    for (const ImageRequest::InlineImage& source : request.input_images) {
        if (cancellation.cancelled()) return cancelled_error();
        if (!source.bytes) return {ErrorCode::Internal, "uploaded image data is unavailable"};
        Error error = input::validate_image_bytes(*source.bytes, source.mime_type);
        if (!error.ok()) return error;
        provider::ImageInput image{source.mime_type, input::encode_base64(*source.bytes)};
        image.display_name = source.display_name;
        image.source_ref = source.display_name;
        image.byte_size = static_cast<long long>(source.bytes->size());
        images.push_back(std::move(image));
        if (static_cast<int>(images.size()) > max_inputs) {
            return {ErrorCode::BadArgs,
                    "this image model accepts at most " + std::to_string(max_inputs) +
                        " reference images"};
        }
        error = publish(events, {EventType::Progress,
                                 "Attached uploaded image: " + source.display_name,
                                 images.size(), static_cast<std::size_t>(max_inputs)});
        if (!error.ok()) return error;
    }
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

namespace {
std::string media_mime(const std::string& path, const std::string& bytes) {
    std::string lower = ascii_lower(path);
    auto ends = [&](const char* suffix) { const std::string s(suffix); return lower.size() >= s.size() && lower.compare(lower.size() - s.size(), s.size(), s) == 0; };
    if (bytes.size() >= 8 && bytes.compare(0, 8, "\x89PNG\r\n\x1a\n", 8) == 0) return "image/png";
    if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xff && static_cast<unsigned char>(bytes[1]) == 0xd8) return "image/jpeg";
    if (bytes.rfind("RIFF", 0) == 0 && bytes.size() >= 12 && bytes.compare(8, 4, "WEBP") == 0) return "image/webp";
    if (bytes.rfind("GIF8", 0) == 0) return "image/gif";
    if (bytes.rfind("BM", 0) == 0) return "image/bmp";
    if (bytes.size() >= 8 && (bytes.rfind("II*\0", 0) == 0 || bytes.rfind("MM\0*", 0) == 0)) return "image/tiff";
    if ((ends(".heic") || ends(".heif")) && bytes.size() >= 12 && bytes.compare(4, 4, "ftyp") == 0) return ends(".heic") ? "image/heic" : "image/heif";
    if (bytes.size() >= 12 && bytes.compare(4, 4, "ftyp") == 0) return ends(".mov") ? "video/quicktime" : "video/mp4";
    if (bytes.rfind("RIFF", 0) == 0 && bytes.size() >= 12 && bytes.compare(8, 4, "WAVE") == 0) return "audio/wav";
    if (bytes.rfind("ID3", 0) == 0 || ends(".mp3")) return "audio/mpeg";
    return {};
}
}

VideoResult run_video(provider::RequestContext context, const VideoRequest& input,
                      runtime::CancellationToken cancellation, EventSink events,
                      VideoExecutor executor) {
    VideoResult result;
    if (cancellation.cancelled()) { result.error = cancelled_error(); return result; }
    const std::string provider_name = provider::canonical_profile_name(context.options.provider);
    result.selected_model = input.model.empty() ? config::default_video_model(context.options.video_catalog, provider_name) : input.model;
    if (result.selected_model.empty()) { result.error = {ErrorCode::BadArgs, "video mode requires a model or videos.conf default for provider " + provider_name}; return result; }
    const VideoCapability* capability = config::resolve_video_capability(context.options.video_catalog, provider_name, result.selected_model);
    if (!capability) { result.error = {ErrorCode::BadArgs, "videos.conf has no record for provider " + provider_name + " model " + result.selected_model + "; known models: " + config::known_video_models_description(context.options.video_catalog, provider_name)}; return result; }
    result.request.model = capability->api_model;
    result.request.prompt = ascii_trim(input.prompt);
    if (result.request.prompt.empty()) { result.error = {ErrorCode::BadArgs, "video mode requires a non-empty prompt"}; return result; }
    result.request.capability = *capability; result.request.output_path = input.output_path; result.request.overwrite = input.overwrite;
    result.error = provider::normalize_video_settings(*capability, input.settings, result.request.settings);
    if (!result.error.ok()) return result;
    result.request.inputs = input.input_media;
    for (const std::string& path : input.attachment_paths) {
        if (cancellation.cancelled()) { result.error = cancelled_error(); return result; }
        std::string bytes; result.error = platform::read_file_bounded(path, input.max_input_bytes, bytes);
        if (!result.error.ok()) return result;
        const std::string mime = media_mime(path, bytes);
        if (mime.empty()) { result.error = {ErrorCode::BadArgs, "unsupported video reference media: " + path}; return result; }
        const std::size_t per_media_limit = mime.rfind("audio/", 0) == 0 ? 15U * 1024U * 1024U :
                                            mime.rfind("image/", 0) == 0 ? 30U * 1024U * 1024U :
                                            200U * 1024U * 1024U;
        if (bytes.size() > per_media_limit) { result.error = {ErrorCode::BadArgs, "video reference exceeds its media-type size limit: " + path}; return result; }
        result.request.inputs.push_back({mime, path,
            std::make_shared<const std::string>(std::move(bytes)), {}});
        result.error = publish(events, {EventType::Progress, "Attached video reference: " + path, result.request.inputs.size(), input.attachment_paths.size()});
        if (!result.error.ok()) return result;
    }
    int images = 0, videos = 0, audios = 0;
    std::size_t total_bytes = 0;
    for (const auto& media : result.request.inputs) {
        if (!media.bytes || media.bytes->empty()) {
            result.error = {ErrorCode::BadArgs, "video reference data is empty or unavailable"};
            return result;
        }
        const bool image = media.mime_type.rfind("image/", 0) == 0;
        const bool video = media.mime_type.rfind("video/", 0) == 0;
        const bool audio = media.mime_type.rfind("audio/", 0) == 0;
        if (!image && !video && !audio) {
            result.error = {ErrorCode::BadArgs,
                            "unsupported video reference media type: " + media.mime_type};
            return result;
        }
        if (image) ++images;
        else if (video) ++videos;
        else ++audios;
        const std::size_t general_limit = image ? 30U * 1024U * 1024U
                                                : audio ? 15U * 1024U * 1024U
                                                        : 200U * 1024U * 1024U;
        const int configured_limit = image ? capability->max_input_image_bytes
                                           : audio ? capability->max_input_audio_bytes
                                                   : capability->max_input_video_bytes;
        const std::size_t media_limit = configured_limit > 0
                                            ? static_cast<std::size_t>(configured_limit)
                                            : general_limit;
        if (media.bytes->size() > media_limit) {
            result.error = {ErrorCode::BadArgs,
                            "video reference exceeds this model's media-type size limit"};
            return result;
        }
        if (total_bytes > 1024U * 1024U * 1024U - media.bytes->size()) {
            result.error = {ErrorCode::BadArgs,
                            "video references exceed the combined 1 GiB input limit"};
            return result;
        }
        total_bytes += media.bytes->size();
    }
    if (images > capability->max_input_images || videos > capability->max_input_videos ||
        audios > capability->max_input_audios ||
        (capability->max_input_total > 0 && images + videos + audios > capability->max_input_total)) {
        result.error = {ErrorCode::BadArgs, "attached media exceeds this video model's input limits"}; return result;
    }
    if (capability->input_mode == VideoInputMode::Text && images + videos + audios > 0) {
        result.error = {ErrorCode::BadArgs, "text-to-video models do not accept --attach"};
        return result;
    }
    if (capability->input_mode == VideoInputMode::Image && images < 1) {
        result.error = {ErrorCode::BadArgs, "this image-to-video model requires a start image"};
        return result;
    }
    if (capability->input_mode == VideoInputMode::Reference && images + videos < 1) {
        result.error = {ErrorCode::BadArgs,
                        "reference-to-video requires an image or video reference"};
        return result;
    }
    result.error = publish(events, {EventType::Started, "Generating video with " + result.request.model, 0, 0});
    if (!result.error.ok()) return result;
    if (!executor) executor = provider::generate_video;
    result.error = executor(context, result.request, result.response, cancellation);
    result.request.inputs.clear();
    if (!result.error.ok()) return result;
    result.error = publish(events, {EventType::Completed, {}, 0, 0});
    return result;
}

}  // namespace ainiux::app::operation
