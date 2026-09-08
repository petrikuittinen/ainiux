#include "app/app.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>

#include "app/operations.hpp"
#include "platform/filesystem.hpp"
#include "provider/video.hpp"
#include "runtime/interrupt.hpp"

namespace ainiux::app {
namespace {
json::Value parse_scalar(const std::string& text) {
    const json::ParseResult parsed = json::parse(text);
    if (parsed.error.ok() && !parsed.value.is_array() && !parsed.value.is_object() && !parsed.value.is_null()) return parsed.value;
    json::Value value; value.type = json::Value::Type::String; value.string = text; return value;
}
Error add_setting(std::map<std::string, json::Value>& settings, const std::string& item) {
    const std::size_t equals = item.find('=');
    if (equals == std::string::npos || equals == 0 || equals + 1 >= item.size()) return {ErrorCode::BadArgs, "--video-setting expects NAME=VALUE"};
    const std::string name = item.substr(0, equals);
    if (!settings.emplace(name, parse_scalar(item.substr(equals + 1))).second) return {ErrorCode::BadArgs, "duplicate video setting: " + name};
    return ok_error();
}
}  // namespace

int run_video_mode(provider::RequestContext context) {
    cli::Options& options = context.options;
    std::string output_path;
    const bool stdout_output = options.output_path == "stdout";
    if (stdout_output) {
        std::string random; Error err = platform::secure_random_hex(8, random);
        if (!err.ok()) { print_error(err); return exit_code_for(err.code); }
        output_path = (std::filesystem::temp_directory_path() / ("ainiux-video-" + random + ".mp4")).u8string();
    } else if (!options.output_path.empty()) output_path = options.output_path;
    else {
        Error err = provider::allocate_unused_video_path(".", output_path);
        if (!err.ok()) { print_error(err); return exit_code_for(err.code); }
    }
    operation::VideoRequest request;
    request.model = options.model; request.prompt = options.prompt; request.attachment_paths = options.attachment_paths;
    request.output_path = output_path; request.overwrite = options.image_force;
    for (const std::string& item : options.video_settings) { Error err = add_setting(request.settings, item); if (!err.ok()) { print_error(err); return exit_code_for(err.code); } }
    auto set = [&](const std::string& name, const std::string& value) -> Error { return value.empty() ? ok_error() : add_setting(request.settings, name + "=" + value); };
    Error setting_error = set("resolution", options.video_resolution);
    if (setting_error.ok()) setting_error = set("duration", options.video_duration);
    if (setting_error.ok()) setting_error = set("seed", options.video_seed);
    if (setting_error.ok()) setting_error = set("negative_prompt", options.video_negative_prompt);
    if (setting_error.ok()) setting_error = set("aspect_ratio", options.image_ar);
    if (setting_error.ok() && !options.video_audio.empty()) {
        const std::string audio = ascii_lower(options.video_audio);
        if (audio != "on" && audio != "off" && audio != "true" && audio != "false") setting_error = {ErrorCode::BadArgs, "--audio must be on or off"};
        else setting_error = add_setting(request.settings, "audio=" + std::string(audio == "on" || audio == "true" ? "true" : "false"));
    }
    if (!setting_error.ok()) { print_error(setting_error); return exit_code_for(setting_error.code); }
    runtime::InterruptGuard interrupt_guard; runtime::CancellationSource cancellation;
    std::atomic<bool> done{false};
    std::thread monitor([&] { while (!done.load(std::memory_order_acquire)) { if (interrupt_guard.interrupted()) { cancellation.cancel(); break; } std::this_thread::sleep_for(std::chrono::milliseconds(25)); } });
    struct Join { std::atomic<bool>& done; std::thread& thread; ~Join() { done.store(true, std::memory_order_release); if (thread.joinable()) thread.join(); } } join{done, monitor};
    const bool quiet = options.quiet;
    operation::VideoResult generated = operation::run_video(context, request, cancellation.token(), [quiet](const operation::Event& event) { if (!quiet && !event.text.empty()) std::cerr << event.text << (event.type == operation::EventType::Started ? "..." : "") << '\n'; return ok_error(); });
    if (!generated.error.ok()) { print_error(generated.error); return exit_code_for(generated.error.code); }
    if (stdout_output) {
        std::ifstream file(std::filesystem::u8path(output_path), std::ios::binary);
        std::cout << file.rdbuf(); std::cout.flush();
        std::error_code ec; std::filesystem::remove(std::filesystem::u8path(output_path), ec);
        if (!file || !std::cout) { Error err{ErrorCode::FileWrite, "could not write video bytes to stdout"}; print_error(err); return exit_code_for(err.code); }
    } else std::cout << output_path << '\n';
    return 0;
}
}  // namespace ainiux::app
