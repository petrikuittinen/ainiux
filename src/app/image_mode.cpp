#include "app/app.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <thread>
#include <utility>

#include "config/image_catalog.hpp"
#include "input/input.hpp"
#include "platform/filesystem.hpp"
#include "provider/image.hpp"
#include "provider/provider.hpp"
#include "runtime/interrupt.hpp"

namespace ainiux::app {
namespace {

bool path_exists(const std::string& path) {
    std::error_code error;
    return std::filesystem::exists(std::filesystem::u8path(path), error) && !error;
}

Error load_image_attachments(const provider::RequestContext& context,
                             const ImageCapability& capability,
                             runtime::CancellationToken cancellation,
                             std::vector<provider::ImageInput>& images) {
    images.clear();
    for (const std::string& path : context.options.attachment_paths) {
        input::FileType type;
        Error type_error = input::classify_file_type(path, type);
        if (!type_error.ok()) {
            return type_error;
        }
        if (type.kind != input::Kind::Image) {
            return {ErrorCode::BadArgs,
                    "image mode --attach supports PNG and JPEG only: " + path};
        }
        if (type.mime_type != "image/png" && type.mime_type != "image/jpeg") {
            return {ErrorCode::BadArgs,
                    "image mode --attach supports PNG and JPEG only; GIF and other types "
                    "are not used as image references: " +
                        path};
        }
        if (!capability.edits) {
            return {ErrorCode::BadArgs,
                    "image model " + context.options.model +
                        " does not support --attach reference images"};
        }
        const size_t limit = context.options.max_image_bytes > 0
                                 ? static_cast<size_t>(context.options.max_image_bytes)
                                 : 20U * 1024U * 1024U;
        input::ImageData loaded;
        Error load_error = input::load_image_file(path, type, limit, loaded, cancellation);
        if (!load_error.ok()) return load_error;
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
        if (!context.options.quiet) {
            std::cerr << "Attached image: " << path << " (" << loaded.mime_type << ", "
                      << loaded.byte_size << " bytes)\n";
        }
    }
    return ok_error();
}

Error resolve_output_path(const cli::Options& options,
                          const std::string& extension,
                          std::string& path,
                          bool& write_stdout) {
    write_stdout = false;
    if (options.output_path == "stdout") {
        write_stdout = true;
        path = "stdout";
        return ok_error();
    }
    if (!options.output_path.empty()) {
        path = options.output_path;
        if (path_exists(path) && !options.image_force) {
            return {ErrorCode::FileWrite,
                    "output file already exists (pass --force to overwrite): " + path};
        }
        return ok_error();
    }
    return provider::allocate_unused_image_path(".", extension, path);
}

}  // namespace

int run_image_mode(provider::RequestContext context) {
    runtime::InterruptGuard interrupt_guard;
    runtime::CancellationSource cancellation;
    std::atomic<bool> finished{false};
    std::thread interrupt_monitor([&] {
        while (!finished.load(std::memory_order_acquire)) {
            if (interrupt_guard.interrupted()) {
                cancellation.cancel();
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
    });
    struct MonitorJoin {
        std::atomic<bool>& finished;
        std::thread& thread;
        ~MonitorJoin() {
            finished.store(true, std::memory_order_release);
            if (thread.joinable()) thread.join();
        }
    } monitor_join{finished, interrupt_monitor};

    const std::string provider_name =
        provider::canonical_profile_name(context.options.provider);
    if (context.options.model.empty()) {
        context.options.model =
            config::default_image_model(context.options.image_catalog, provider_name);
    }
    if (context.options.model.empty()) {
        const Error error{ErrorCode::BadArgs,
                          "image mode requires -m/--model or an images.conf default for provider " +
                              provider_name};
        print_error(error);
        return exit_code_for(error.code);
    }

    const ImageCapability* capability = config::resolve_image_capability(
        context.options.image_catalog, provider_name, context.options.model);
    if (capability == nullptr) {
        const Error error{
            ErrorCode::BadArgs,
            "images.conf has no record for provider " + provider_name + " model " +
                context.options.model + "; known models: " +
                config::known_image_models_description(context.options.image_catalog,
                                                       provider_name)};
        print_error(error);
        return exit_code_for(error.code);
    }
    if (!config::image_protocol_implemented(capability->protocol)) {
        const Error error{
            ErrorCode::UnsupportedFeature,
            std::string("image protocol ") + config::image_protocol_name(capability->protocol) +
                " is not built into this ainiux"};
        print_error(error);
        return exit_code_for(error.code);
    }

    std::string size;
    std::string aspect;
    Error size_error = provider::resolve_image_size(
        *capability, context.options.image_size, context.options.image_ar, size, aspect);
    if (!size_error.ok()) {
        print_error(size_error);
        return exit_code_for(size_error.code);
    }
    std::string quality;
    Error quality_error =
        provider::normalize_image_quality(*capability, context.options.image_quality, quality);
    if (!quality_error.ok()) {
        print_error(quality_error);
        return exit_code_for(quality_error.code);
    }
    std::string format;
    Error format_error = provider::normalize_image_format(
        *capability,
        context.options.image_format_explicit ? context.options.image_format
                                              : capability->format_default,
        format);
    if (!format_error.ok()) {
        print_error(format_error);
        return exit_code_for(format_error.code);
    }

    const std::string prompt = ascii_trim(context.options.prompt);
    if (prompt.empty()) {
        const Error error{ErrorCode::BadArgs, "image mode requires a non-empty prompt"};
        print_error(error);
        return exit_code_for(error.code);
    }

    std::vector<provider::ImageInput> images;
    Error attach_error =
        load_image_attachments(context, *capability, cancellation.token(), images);
    if (!attach_error.ok()) {
        print_error(attach_error);
        return exit_code_for(attach_error.code);
    }

    provider::ImageGenerateRequest request;
    request.capability = *capability;
    request.protocol = capability->protocol;
    request.model = ((capability->protocol == ImageProtocol::ReplicatePredictions ||
                      capability->protocol == ImageProtocol::FalQueue ||
                      capability->protocol == ImageProtocol::GeminiInteractions) &&
                     !capability->api_model.empty())
                        ? capability->api_model
                        : context.options.model;
    request.prompt = prompt;
    request.size = size;
    request.aspect = aspect;
    request.quality = quality;
    request.output_format = format;
    request.images = std::move(images);

    if (!context.options.quiet) {
        std::cerr << "Generating image with " << request.model;
        if (!request.size.empty()) std::cerr << " (" << request.size;
        else std::cerr << " (size auto";
        if (!request.aspect.empty()) std::cerr << ", ar " << request.aspect;
        std::cerr << ", quality " << (request.quality.empty() ? "auto" : request.quality)
                  << ", "
                  << (request.output_format.empty() ? "auto" : request.output_format) << ")";
        if (provider::image_size_is_experimental(request.size)) {
            std::cerr << " [experimental size]";
        }
        std::cerr << "...\n";
    }

    provider::ImageGenerateResult generated;
    Error generate_error =
        provider::generate_or_edit_image(context, request, generated, cancellation.token());
    if (!generate_error.ok()) {
        print_error(generate_error);
        return exit_code_for(generate_error.code);
    }
    if (generated.bytes.empty()) {
        const Error error{ErrorCode::ProviderSchema, "image response decoded to an empty file"};
        print_error(error);
        return exit_code_for(error.code);
    }

    std::string ext = provider::image_extension_for_format(
        request.output_format.empty() ? generated.output_format : request.output_format);
    bool write_stdout = false;
    std::string output_path;
    Error path_error = resolve_output_path(context.options, ext, output_path, write_stdout);
    if (!path_error.ok()) {
        print_error(path_error);
        return exit_code_for(path_error.code);
    }

    if (write_stdout) {
        std::cout.write(generated.bytes.data(), static_cast<std::streamsize>(generated.bytes.size()));
        std::cout.flush();
        if (!std::cout) {
            const Error error{ErrorCode::FileWrite, "could not write image bytes to stdout"};
            print_error(error);
            return exit_code_for(error.code);
        }
    } else {
        Error write_error = platform::atomic_write_shared(output_path, generated.bytes, true);
        if (!write_error.ok()) {
            print_error(write_error);
            return exit_code_for(write_error.code);
        }
        std::cout << output_path << "\n";
    }

    if (!context.options.quiet) {
        std::cerr << "Wrote " << (write_stdout ? std::string("stdout") : output_path) << " ("
                  << generated.bytes.size() << " bytes";
        if (generated.total_ms >= 0) std::cerr << ", " << generated.total_ms << " ms";
        if (generated.total_tokens > 0) std::cerr << ", " << generated.total_tokens << " tokens";
        std::cerr << ")\n";
    }
    return 0;
}

}  // namespace ainiux::app
