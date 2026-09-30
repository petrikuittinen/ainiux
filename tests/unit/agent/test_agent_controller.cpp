#include "agent/test_agent_controller.hpp"

#include "agent/agent_controller.hpp"
#include "support/test_support.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

namespace ainiux::test::agent_controller {
namespace {
using ainiux::test::check;

void test_ensure_and_basic_lifecycle() {
    std::shared_ptr<ainiux::agent::AgentController> slot;
    auto first = ainiux::agent::ensure_agent_controller(slot);
    check(first != nullptr && slot == first, "ensure_agent_controller creates controller");
    auto second = ainiux::agent::ensure_agent_controller(slot);
    check(second == first, "ensure_agent_controller reuses existing controller");
    check(first->runtime() != nullptr, "controller owns a session runtime");
    check(first->approval_gate() != nullptr, "controller owns an approval gate");
    check(!first->prepared(), "fresh controller is not prepared");
    check(!first->turn_running(), "fresh controller has no turn");
}

void test_start_turn_posts_done_and_clears_running() {
    ainiux::agent::AgentController controller;
    check(controller.start_turn([](ainiux::runtime::CancellationToken) {
              ainiux::agent::AgentSurfaceEvent event;
              event.type = ainiux::agent::AgentSurfaceEvent::Type::TurnDone;
              event.agent_turn = true;
              event.agent_final_text = "hello";
              return event;
          }),
          "start_turn accepts work when idle");
    check(controller.turn_running() || controller.job_joinable(),
          "turn is running or joinable immediately after start");

    ainiux::agent::AgentSurfaceEvent event;
    bool got = false;
    for (int i = 0; i < 200; ++i) {
        if (controller.events().try_pop(event)) {
            got = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    check(got, "turn posts a completion event");
    check(event.type == ainiux::agent::AgentSurfaceEvent::Type::TurnDone,
          "completion event is TurnDone");
    check(event.agent_final_text == "hello", "final text is preserved");
    controller.join_turn();
    check(!controller.turn_running(), "join_turn clears running flag");
}

void test_start_turn_rejects_concurrent() {
    ainiux::agent::AgentController controller;
    check(controller.start_turn([](ainiux::runtime::CancellationToken token) {
              while (!token.cancelled()) {
                  std::this_thread::sleep_for(std::chrono::milliseconds(5));
              }
              ainiux::agent::AgentSurfaceEvent event;
              event.type = ainiux::agent::AgentSurfaceEvent::Type::TurnError;
              event.error = {ainiux::ErrorCode::Cancelled, "cancelled"};
              event.agent_turn = true;
              return event;
          }),
          "first turn starts");
    check(!controller.start_turn([](ainiux::runtime::CancellationToken) {
              ainiux::agent::AgentSurfaceEvent event;
              event.type = ainiux::agent::AgentSurfaceEvent::Type::TurnDone;
              return event;
          }),
          "second concurrent turn is rejected");
    controller.cancel_turn();
    controller.join_turn();
    check(!controller.turn_running(), "cancelled turn is no longer running");
}

void test_guard_notify_posts_event() {
    ainiux::agent::AgentController controller;
    controller.arm_guard_notify();
    ainiux::agent::GuardApprovalRequest request;
    request.tool_name = "write";
    request.command_preview = "docs/x.md";
    request.rule_id = "test";
    request.message = "needs approval";
    request.review_path = "scripts/ainiux/check.sh";

    std::thread worker([&]() {
        const auto decision = controller.approval_gate()->request(
            request, ainiux::runtime::CancellationToken());
        check(decision == ainiux::agent::GuardApprovalDecision::Allow,
              "gate resolves Allow from test harness");
    });

    ainiux::agent::AgentSurfaceEvent event;
    bool got = false;
    for (int i = 0; i < 200; ++i) {
        if (controller.events().try_pop(event)) {
            got = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    check(got, "guard notify posts GuardApproval event");
    check(event.type == ainiux::agent::AgentSurfaceEvent::Type::GuardApproval,
          "event type is GuardApproval");
    check(event.guard_tool_name == "write", "guard tool name is forwarded");
    check(event.guard_review_path == "scripts/ainiux/check.sh",
          "guard review path is forwarded");
    controller.approval_gate()->resolve(ainiux::agent::GuardApprovalDecision::Allow);
    worker.join();
}

void test_questionnaire_gate_event_validation_and_decline() {
    ainiux::agent::AgentController controller;
    ainiux::agent::QuestionnaireRequest request;
    ainiux::agent::QuestionnaireQuestion question;
    question.question = "Choose a mode";
    question.options.push_back({{}, "Safe (Recommended)", "Safer", false});
    question.options.push_back({{}, "Other", "Custom", true});
    request.questions.push_back(std::move(question));

    ainiux::agent::QuestionnaireResponse response;
    std::thread worker([&]() {
        response = controller.questionnaire_gate()->request(
            request, ainiux::runtime::CancellationToken());
    });
    ainiux::agent::AgentSurfaceEvent event;
    bool got = false;
    for (int i = 0; i < 200; ++i) {
        if (controller.events().try_pop(event)) {
            got = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    check(got && event.type ==
                     ainiux::agent::AgentSurfaceEvent::Type::QuestionnaireRequired,
          "questionnaire notify posts a typed surface event");
    check(!event.questionnaire.id.empty() &&
              !event.questionnaire.questions.front().id.empty() &&
              !event.questionnaire.questions.front().options.front().id.empty(),
          "questionnaire gate assigns opaque host IDs");
    std::vector<ainiux::agent::QuestionnaireAnswer> missing;
    check(!controller.answer_questionnaire(event.questionnaire.id, missing).ok() &&
              controller.questionnaire_gate()->has_pending(),
          "malformed partial answers leave questionnaire pending");
    check(controller.decline_questionnaire(event.questionnaire.id).ok(),
          "questionnaire can be declined explicitly");
    worker.join();
    check(response.outcome == ainiux::agent::QuestionnaireOutcome::Declined,
          "decline wakes the blocked questionnaire worker");

    std::thread answered_worker([&]() {
        response = controller.questionnaire_gate()->request(
            request, ainiux::runtime::CancellationToken());
    });
    got = false;
    for (int i = 0; i < 200; ++i) {
        if (controller.events().try_pop(event)) { got = true; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    check(got, "second questionnaire event is delivered");
    ainiux::agent::QuestionnaireAnswer accepted;
    accepted.question_id = event.questionnaire.questions.front().id;
    accepted.option_id = event.questionnaire.questions.front().options.front().id;
    accepted.comment = u8"Looks good ✓";
    check(controller.answer_questionnaire(event.questionnaire.id, {accepted}).ok(),
          "complete ID-based answer resolves questionnaire");
    answered_worker.join();
    check(response.outcome == ainiux::agent::QuestionnaireOutcome::Answered &&
              response.answers.size() == 1 &&
              response.answers.front().question == "Choose a mode" &&
              response.answers.front().selected_label == "Safe (Recommended)" &&
              response.answers.front().comment == u8"Looks good ✓",
          "accepted answer is ordered and enriched from host-owned labels");

    std::thread cancelled_worker([&]() {
        response = controller.questionnaire_gate()->request(
            request, ainiux::runtime::CancellationToken());
    });
    for (int i = 0; i < 200 &&
                    !controller.questionnaire_gate()->has_pending(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    controller.cancel_turn();
    cancelled_worker.join();
    check(response.outcome == ainiux::agent::QuestionnaireOutcome::Cancelled,
          "turn cancellation wakes a pending questionnaire as cancelled");
}

void test_questionnaire_queue_cancellation_and_back_to_back_state() {
    ainiux::agent::QuestionnaireRequest request;
    ainiux::agent::QuestionnaireQuestion question;
    question.question = "Choose a mode";
    question.options.push_back({{}, "Safe (Recommended)", "Safer", false});
    question.options.push_back({{}, "Other", "Custom", true});
    request.questions.push_back(std::move(question));

    ainiux::agent::QuestionnaireGate gate;
    ainiux::agent::QuestionnaireResponse first_response;
    std::thread first([&]() { first_response = gate.request(request); });
    for (int i = 0; i < 200 && !gate.has_pending(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    check(gate.has_pending(), "first questionnaire occupies the gate");

    ainiux::runtime::CancellationSource queued_cancellation;
    ainiux::agent::QuestionnaireResponse queued_response;
    std::atomic<bool> queued_done{false};
    std::thread queued([&]() {
        queued_response = gate.request(request, queued_cancellation.token());
        queued_done.store(true, std::memory_order_release);
    });
    queued_cancellation.cancel();
    for (int i = 0; i < 100 &&
                    !queued_done.load(std::memory_order_acquire); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    check(queued_done.load(std::memory_order_acquire) &&
              queued_response.outcome ==
                  ainiux::agent::QuestionnaireOutcome::Cancelled,
          "a cancelled questionnaire queued behind another request exits promptly");
    gate.cancel_pending();
    first.join();
    queued.join();
    check(first_response.outcome ==
              ainiux::agent::QuestionnaireOutcome::Cancelled,
          "gate cancellation still wakes the active questionnaire");

    ainiux::agent::AgentController controller;
    ainiux::agent::QuestionnaireResponse sequential_first;
    ainiux::agent::QuestionnaireResponse sequential_second;
    std::thread sequential([&]() {
        sequential_first = controller.questionnaire_gate()->request(request);
        sequential_second = controller.questionnaire_gate()->request(request);
    });
    ainiux::agent::AgentSurfaceEvent first_event;
    check(controller.events().wait_pop_for(
              first_event, std::chrono::milliseconds(1000)),
          "first sequential questionnaire is delivered");
    check(controller.decline_questionnaire(first_event.questionnaire.id).ok(),
          "first sequential questionnaire resolves");
    ainiux::agent::AgentSurfaceEvent second_event;
    check(controller.events().wait_pop_for(
              second_event, std::chrono::milliseconds(1000)),
          "second sequential questionnaire is delivered");
    check(second_event.questionnaire.id != first_event.questionnaire.id &&
              controller.waiting_questionnaire() &&
              controller.status_label() == "Agent waiting for your answer",
          "resolving an old questionnaire preserves the next wait state");
    check(controller.decline_questionnaire(second_event.questionnaire.id).ok(),
          "second sequential questionnaire resolves");
    sequential.join();
    check(sequential_first.outcome ==
              ainiux::agent::QuestionnaireOutcome::Declined &&
              sequential_second.outcome ==
                  ainiux::agent::QuestionnaireOutcome::Declined,
          "back-to-back questionnaire responses reach the correct requests");
}

}  // namespace

void run_all() {
    test_ensure_and_basic_lifecycle();
    test_start_turn_posts_done_and_clears_running();
    test_start_turn_rejects_concurrent();
    test_guard_notify_posts_event();
    test_questionnaire_gate_event_validation_and_decline();
    test_questionnaire_queue_cancellation_and_back_to_back_state();
}

}  // namespace ainiux::test::agent_controller
