#include "app/test_operations.hpp"

#include <string>
#include <vector>

#include "app/operations.hpp"
#include "config/config.hpp"
#include "support/test_support.hpp"

namespace ainiux::test::app_operations {
namespace {

using ainiux::test::check;

provider::RequestContext context() {
    provider::RequestContext value;
    value.options.model = "test-model";
    return value;
}

app::operation::ChatRequest request() {
    app::operation::ChatRequest value;
    value.messages.push_back({"user", "hello"});
    return value;
}

void test_chat_events_and_result() {
    std::vector<app::operation::Event> events;
    app::operation::ChatExecutor executor =
        [](const provider::RequestContext&,
           const std::vector<provider::Message>& messages,
           provider::DeltaCallback on_delta,
           provider::ChatResult& result,
           runtime::CancellationToken cancellation) -> Error {
            check(messages.size() == 1 && messages[0].content == "hello",
                  "chat operation forwards explicit messages");
            check(!cancellation.cancelled(), "chat operation forwards live cancellation token");
            Error error = on_delta("hel");
            if (!error.ok()) return error;
            error = on_delta("lo");
            if (!error.ok()) return error;
            result.content = "hello";
            result.model = "test-model";
            return ok_error();
        };
    app::operation::ChatResult result = app::operation::run_chat(
        context(), request(), runtime::CancellationToken(),
        [&](const app::operation::Event& event) {
            events.push_back(event);
            return ok_error();
        },
        executor);
    check(result.error.ok() && result.response.content == "hello",
          "chat operation returns provider result");
    check(events.size() == 4, "chat operation emits started, deltas, and completed");
    check(events.size() == 4 && events[0].type == app::operation::EventType::Started &&
              events[1].type == app::operation::EventType::Delta && events[1].text == "hel" &&
              events[2].text == "lo" &&
              events[3].type == app::operation::EventType::Completed,
          "chat operation event order is deterministic");
}

void test_pre_cancel_does_not_execute() {
    runtime::CancellationSource source;
    source.cancel();
    bool called = false;
    app::operation::ChatExecutor executor =
        [&](const provider::RequestContext&,
            const std::vector<provider::Message>&,
            provider::DeltaCallback,
            provider::ChatResult&,
            runtime::CancellationToken) {
            called = true;
            return ok_error();
        };
    const app::operation::ChatResult result =
        app::operation::run_chat(context(), request(), source.token(), {}, executor);
    check(result.error.code == ErrorCode::Cancelled, "pre-cancelled operation is cancelled");
    check(!called, "pre-cancelled operation does not call provider executor");
}

void test_mid_stream_cancel_propagates() {
    runtime::CancellationSource source;
    app::operation::ChatExecutor executor =
        [&](const provider::RequestContext&,
            const std::vector<provider::Message>&,
            provider::DeltaCallback on_delta,
            provider::ChatResult&,
            runtime::CancellationToken) -> Error {
            Error error = on_delta("first");
            if (!error.ok()) return error;
            source.cancel();
            return on_delta("second");
        };
    int deltas = 0;
    const app::operation::ChatResult result = app::operation::run_chat(
        context(), request(), source.token(),
        [&](const app::operation::Event& event) {
            if (event.type == app::operation::EventType::Delta) ++deltas;
            return ok_error();
        },
        executor);
    check(result.error.code == ErrorCode::Cancelled,
          "mid-stream cancellation reaches operation result");
    check(deltas == 1, "cancelled delta is not published");
}

void test_empty_request_is_rejected() {
    const app::operation::ChatResult result =
        app::operation::run_chat(context(), app::operation::ChatRequest{});
    check(result.error.code == ErrorCode::BadArgs, "empty chat request is rejected locally");
}

void test_image_operation_is_output_neutral() {
    const config::ParseResult parsed = config::read_file("config/images.conf");
    check(parsed.error.ok(), "images.conf parses for image operation test");
    provider::RequestContext image_context;
    image_context.options.provider = "openai";
    check(config::apply_images_document(parsed.document, image_context.options).ok(),
          "images.conf applies for image operation test");
    ImageCapability const* capability = config::resolve_image_capability(
        image_context.options.image_catalog, "openai", "gpt-image-2");
    check(capability != nullptr, "image operation test model is available");
    if (capability == nullptr) return;

    app::operation::ImageRequest image_request;
    image_request.model = "gpt-image-2";
    image_request.prompt = "a small blue square";
    std::vector<app::operation::Event> events;
    app::operation::ImageExecutor executor =
        [](const provider::RequestContext& context,
           const provider::ImageGenerateRequest& request,
           provider::ImageGenerateResult& result,
           runtime::CancellationToken token) {
            check(context.options.model == "gpt-image-2",
                  "image operation selects model in its private context");
            check(request.prompt == "a small blue square" &&
                      request.protocol == ImageProtocol::OpenAiImages,
                  "image operation builds provider request without CLI output policy");
            check(!token.cancelled(), "image operation forwards cancellation token");
            result.bytes = "fake-png";
            result.output_format = "png";
            return ok_error();
        };
    const app::operation::ImageResult result = app::operation::run_image(
        image_context, image_request, runtime::CancellationToken(),
        [&](const app::operation::Event& event) {
            events.push_back(event);
            return ok_error();
        },
        executor);
    check(result.error.ok() && result.response.bytes == "fake-png",
          "image operation returns bytes without selecting an output file");
    check(events.size() == 2 && events.front().type == app::operation::EventType::Started &&
              events.back().type == app::operation::EventType::Completed,
          "image operation emits typed lifecycle events");
    check(events.size() == 2 &&
              events.front().text ==
                  "Generating image with gpt-image-2 (size auto, quality auto, png)",
          "image operation preserves the CLI generation status text");
}

}  // namespace

void run_all() {
    test_chat_events_and_result();
    test_pre_cancel_does_not_execute();
    test_mid_stream_cancel_propagates();
    test_empty_request_is_rejected();
    test_image_operation_is_output_neutral();
}

}  // namespace ainiux::test::app_operations
