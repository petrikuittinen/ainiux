#include "agent/test_agent_controller.hpp"

#include "agent/agent_controller.hpp"
#include "support/test_support.hpp"

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

}  // namespace

void run_all() {
    test_ensure_and_basic_lifecycle();
    test_start_turn_posts_done_and_clears_running();
    test_start_turn_rejects_concurrent();
    test_guard_notify_posts_event();
}

}  // namespace ainiux::test::agent_controller
