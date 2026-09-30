#pragma once

#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "common.hpp"
#include "runtime/runtime.hpp"

namespace ainiux::agent {

struct QuestionnaireOption {
    std::string id;
    std::string label;
    std::string description;
    bool other = false;
};

struct QuestionnaireQuestion {
    std::string id;
    std::string question;
    std::vector<QuestionnaireOption> options;
};

struct QuestionnaireRequest {
    std::string id;
    std::vector<QuestionnaireQuestion> questions;
};

struct QuestionnaireAnswer {
    std::string question_id;
    std::string option_id;
    std::string comment;
    // Filled by the host after ID validation; never trusted from wire input.
    std::string question;
    std::string selected_label;
    bool other = false;
};

enum class QuestionnaireOutcome { Answered, Declined, Cancelled };

struct QuestionnaireResponse {
    QuestionnaireOutcome outcome = QuestionnaireOutcome::Cancelled;
    std::vector<QuestionnaireAnswer> answers;
};

using QuestionnaireCallback =
    std::function<QuestionnaireResponse(const QuestionnaireRequest&,
                                        runtime::CancellationToken)>;

// Validates a client answer against the host-owned request. Labels are never
// accepted from the client. `ordered` follows request question order.
Error validate_questionnaire_answers(
    const QuestionnaireRequest& request,
    const std::vector<QuestionnaireAnswer>& answers,
    std::vector<QuestionnaireAnswer>& ordered);

class QuestionnaireGate {
   public:
    using NotifyFn = std::function<void(const QuestionnaireRequest&)>;

    QuestionnaireGate() = default;
    QuestionnaireGate(const QuestionnaireGate&) = delete;
    QuestionnaireGate& operator=(const QuestionnaireGate&) = delete;

    void set_notify(NotifyFn notify);
    QuestionnaireResponse request(
        QuestionnaireRequest request,
        runtime::CancellationToken cancellation = runtime::CancellationToken());

    bool has_pending() const;
    bool try_get_pending(QuestionnaireRequest& out) const;
    Error answer(const std::string& questionnaire_id,
                 const std::vector<QuestionnaireAnswer>& answers);
    Error decline(const std::string& questionnaire_id);
    void cancel_pending();

   private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    bool pending_ = false;
    bool answered_ = false;
    QuestionnaireRequest request_;
    QuestionnaireResponse response_;
    NotifyFn notify_;
};

}  // namespace ainiux::agent
