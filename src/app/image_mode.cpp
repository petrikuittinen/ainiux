#include "app/app.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <thread>
#include <utility>

#include "app/operations.hpp"
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

    operation::ImageRequest operation_request;
    operation_request.model = context.options.model;
    operation_request.prompt = context.options.prompt;
    operation_request.attachment_paths = context.options.attachment_paths;
    operation_request.size = context.options.image_size;
    operation_request.aspect = context.options.image_ar;
    operation_request.quality = context.options.image_quality;
    operation_request.format = context.options.image_format;
    operation_request.format_explicit = context.options.image_format_explicit;
    operation_request.max_image_bytes = context.options.max_image_bytes > 0
                                            ? static_cast<std::size_t>(context.options.max_image_bytes)
                                            : 20U * 1024U * 1024U;
    const bool quiet = context.options.quiet;
    operation::ImageResult operation_result = operation::run_image(
        context, operation_request, cancellation.token(),
        [quiet](const operation::Event& event) {
            if (!quiet && !event.text.empty()) {
                std::cerr << event.text;
                if (event.type == operation::EventType::Started) std::cerr << "...";
                std::cerr << "\n";
            }
            return ok_error();
        });
    if (!operation_result.error.ok()) {
        print_error(operation_result.error);
        return exit_code_for(operation_result.error.code);
    }
    const provider::ImageGenerateRequest& request = operation_result.request;
    const provider::ImageGenerateResult& generated = operation_result.response;

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
