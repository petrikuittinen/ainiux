#include "agent/questionnaire.hpp"

#include <atomic>
#include <chrono>
#include <iomanip>
#include <random>
#include <set>
#include <sstream>

#include "html/html.hpp"

namespace ainiux::agent {
namespace {

std::string opaque_id(const char* prefix) {
    static std::atomic<unsigned long long> sequence{1};
    static const unsigned long long salt = [] {
        std::random_device random;
        return (static_cast<unsigned long long>(random()) << 32U) ^ random();
    }();
    std::ostringstream out;
    out << prefix << '_' << std::hex << std::setfill('0') << std::setw(16)
        << (salt ^ sequence.fetch_add(1, std::memory_order_relaxed));
    return out.str();
}

const QuestionnaireQuestion* find_question(const QuestionnaireRequest& request,
                                           const std::string& id) {
    for (const QuestionnaireQuestion& question : request.questions)
        if (question.id == id) return &question;
    return nullptr;
}

const QuestionnaireOption* find_option(const QuestionnaireQuestion& question,
                                       const std::string& id) {
    for (const QuestionnaireOption& option : question.options)
        if (option.id == id) return &option;
    return nullptr;
}

}  // namespace

Error validate_questionnaire_answers(
    const QuestionnaireRequest& request,
    const std::vector<QuestionnaireAnswer>& answers,
    std::vector<QuestionnaireAnswer>& ordered) {
    ordered.clear();
    if (answers.size() != request.questions.size())
        return {ErrorCode::BadArgs,
                "answers must contain exactly one entry for every question"};
    std::set<std::string> seen;
    for (const QuestionnaireAnswer& answer : answers) {
        if (answer.question_id.empty() || !seen.insert(answer.question_id).second)
            return {ErrorCode::BadArgs,
                    "answers contain a missing or duplicate question_id"};
        const QuestionnaireQuestion* question =
            find_question(request, answer.question_id);
        if (question == nullptr)
            return {ErrorCode::BadArgs,
                    "answers contain an unknown question_id"};
        const QuestionnaireOption* option =
            find_option(*question, answer.option_id);
        if (option == nullptr)
            return {ErrorCode::BadArgs,
                    "answer contains an unknown option_id for question " +
                        answer.question_id};
        if (answer.comment.size() > 4096 ||
            !html::is_valid_utf8(answer.comment))
            return {ErrorCode::BadArgs,
                    "answer comment must be valid UTF-8 and at most 4096 bytes"};
        if (option->other && ascii_trim(answer.comment).empty())
            return {ErrorCode::BadArgs,
                    "Other requires a non-empty comment"};
    }
    for (const QuestionnaireQuestion& question : request.questions) {
        for (const QuestionnaireAnswer& answer : answers) {
            if (answer.question_id == question.id) {
                QuestionnaireAnswer accepted = answer;
                const QuestionnaireOption* option =
                    find_option(question, answer.option_id);
                accepted.question = question.question;
                accepted.selected_label = option == nullptr ? std::string()
                                                            : option->label;
                accepted.other = option != nullptr && option->other;
                ordered.push_back(std::move(accepted));
                break;
            }
        }
    }
    return ok_error();
}

void QuestionnaireGate::set_notify(NotifyFn notify) {
    std::lock_guard<std::mutex> lock(mutex_);
    notify_ = std::move(notify);
}

QuestionnaireResponse QuestionnaireGate::request(
    QuestionnaireRequest request,
    runtime::CancellationToken cancellation) {
    NotifyFn notify_copy;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        while (pending_ && !cancellation.cancelled())
            cv_.wait_for(lock, std::chrono::milliseconds(50));
        if (cancellation.cancelled()) return {};
        request.id = opaque_id("questionnaire");
        for (QuestionnaireQuestion& question : request.questions) {
            question.id = opaque_id("question");
            for (QuestionnaireOption& option : question.options)
                option.id = opaque_id(option.other ? "other" : "option");
        }
        request_ = std::move(request);
        response_ = {};
        pending_ = true;
        answered_ = false;
        notify_copy = notify_;
    }
    if (notify_copy) notify_copy(request_);

    std::unique_lock<std::mutex> lock(mutex_);
    while (!answered_ && !cancellation.cancelled())
        cv_.wait_for(lock, std::chrono::milliseconds(50));
    if (!answered_) {
        pending_ = false;
        answered_ = true;
        response_ = {};
        cv_.notify_all();
        return response_;
    }
    QuestionnaireResponse result = response_;
    pending_ = false;
    answered_ = false;
    cv_.notify_all();
    return result;
}

bool QuestionnaireGate::has_pending() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return pending_ && !answered_;
}

bool QuestionnaireGate::try_get_pending(QuestionnaireRequest& out) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!pending_ || answered_) return false;
    out = request_;
    return true;
}

Error QuestionnaireGate::answer(
    const std::string& questionnaire_id,
    const std::vector<QuestionnaireAnswer>& answers) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!pending_ || answered_ || questionnaire_id.empty() ||
        questionnaire_id != request_.id)
        return {ErrorCode::FileRead,
                "questionnaire was not found or is no longer pending"};
    std::vector<QuestionnaireAnswer> ordered;
    Error error = validate_questionnaire_answers(request_, answers, ordered);
    if (!error.ok()) return error;
    response_.outcome = QuestionnaireOutcome::Answered;
    response_.answers = std::move(ordered);
    answered_ = true;
    cv_.notify_all();
    return ok_error();
}

Error QuestionnaireGate::decline(const std::string& questionnaire_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!pending_ || answered_ || questionnaire_id.empty() ||
        questionnaire_id != request_.id)
        return {ErrorCode::FileRead,
                "questionnaire was not found or is no longer pending"};
    response_.outcome = QuestionnaireOutcome::Declined;
    response_.answers.clear();
    answered_ = true;
    cv_.notify_all();
    return ok_error();
}

void QuestionnaireGate::cancel_pending() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!pending_ || answered_) return;
    response_ = {};
    answered_ = true;
    cv_.notify_all();
}

}  // namespace ainiux::agent
