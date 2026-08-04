#include "benchmark/benchmark.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>
#include <utility>

#include "config/config.hpp"
#include "json/json.hpp"
#include "output/thinking.hpp"
#include "runtime/runtime.hpp"

namespace ainiux::benchmark {
namespace {

constexpr size_t kMaxGradeSourceBytes = 256U * 1024U * 1024U;
constexpr size_t kMaxGradeLineBytes = 64U * 1024U * 1024U;
constexpr const char* kBenchmarkPlaceholder = "{{benchmark_case_json}}";

struct TranscriptTurn {
    size_t turn = 0;
    std::string prompt;
    std::string response;
    bool ok = false;
    bool cancelled = false;
    std::string error_code;
    std::string error;
};

struct GradeGroup {
    std::string id;
    std::string category;
    std::string language;
    std::vector<std::string> tags;
    std::string source_provider;
    std::string source_model;
    std::string reference_answer;
    std::vector<std::string> assessment_criteria;
    std::string safety_classification;
    std::string safety_expected_action;
    size_t run = 0;
    size_t first_line = 0;
    std::map<size_t, TranscriptTurn> turns;
};

struct SourceLoadResult {
    std::vector<GradeGroup> groups;
    Error error;
};

struct GradeStatistics {
    size_t graded = 0;
    size_t errors = 0;
    size_t passes = 0;
    size_t partials = 0;
    size_t failures = 0;
    long long score_sum = 0;
};

class CancellationMonitor {
   public:
    explicit CancellationMonitor(std::function<bool()> requested)
        : requested_(std::move(requested)) {
        if (!requested_) {
            return;
        }
        thread_ = std::thread([this] {
            std::unique_lock<std::mutex> lock(mutex_);
            while (!stop_) {
                lock.unlock();
                if (requested_()) {
                    interrupted_.store(true, std::memory_order_release);
                    source_.cancel();
                    return;
                }
                lock.lock();
                cv_.wait_for(lock, std::chrono::milliseconds(20),
                             [this] { return stop_; });
            }
        });
    }

    ~CancellationMonitor() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
        }
        cv_.notify_one();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    CancellationMonitor(const CancellationMonitor&) = delete;
    CancellationMonitor& operator=(const CancellationMonitor&) = delete;

    runtime::CancellationToken token() const { return source_.token(); }
    bool interrupted() const {
        return interrupted_.load(std::memory_order_acquire) ||
               (requested_ && requested_());
    }

   private:
    std::function<bool()> requested_;
    runtime::CancellationSource source_;
    std::atomic<bool> interrupted_{false};
    std::mutex mutex_;
    std::condition_variable cv_;
    bool stop_ = false;
    std::thread thread_;
};

Error source_error(const std::string& path, size_t line, const std::string& detail) {
    return {ErrorCode::JsonParse,
            path + ":" + std::to_string(line) +
                ": invalid benchmark result JSONL: " + detail};
}

bool json_boolean(const json::Value& object, const std::string& key, bool& output) {
    const json::Value* value = object.get(key);
    if (value == nullptr || value->type != json::Value::Type::Bool) {
        return false;
    }
    output = value->boolean;
    return true;
}

bool json_positive_size(const json::Value& object,
                        const std::string& key,
                        size_t& output) {
    const json::Value* value = object.get(key);
    if (value == nullptr || value->type != json::Value::Type::Number ||
        value->number < 1.0 || std::floor(value->number) != value->number ||
        value->number > static_cast<double>(std::numeric_limits<size_t>::max())) {
        return false;
    }
    output = static_cast<size_t>(value->number);
    return true;
}

bool optional_string(const json::Value& object,
                     const std::string& key,
                     std::string& output) {
    const json::Value* value = object.get(key);
    if (value == nullptr) {
        return true;
    }
    if (!value->is_string()) {
        return false;
    }
    output = value->string;
    return true;
}

bool optional_string_array(const json::Value& object,
                           const std::string& key,
                           std::vector<std::string>& output) {
    const json::Value* value = object.get(key);
    if (value == nullptr) {
        return true;
    }
    if (!value->is_array()) {
        return false;
    }
    for (const json::Value& entry : value->array) {
        if (!entry.is_string() || entry.string.empty()) {
            return false;
        }
        output.push_back(entry.string);
    }
    return true;
}

bool optional_safety(const json::Value& object,
                     std::string& classification,
                     std::string& expected_action) {
    const json::Value* value = object.get("safety");
    if (value == nullptr) {
        return true;
    }
    if (!value->is_object() || value->object.size() != 2) {
        return false;
    }
    const json::Value* classification_value = value->get("classification");
    const json::Value* action_value = value->get("expected_action");
    if (classification_value == nullptr || !classification_value->is_string() ||
        action_value == nullptr || !action_value->is_string()) {
        return false;
    }
    classification = classification_value->string;
    expected_action = action_value->string;
    const bool valid_classification =
        classification == "harmful" || classification == "harmless" ||
        classification == "sensitive";
    const bool valid_action = expected_action == "answer" ||
                              expected_action == "reject";
    if (!valid_classification || !valid_action) {
        return false;
    }
    if (classification == "harmful" && expected_action != "reject") {
        return false;
    }
    if (classification == "harmless" && expected_action != "answer") {
        return false;
    }
    return true;
}

bool same_or_empty(std::string& existing, const std::string& incoming) {
    if (incoming.empty()) {
        return true;
    }
    if (existing.empty()) {
        existing = incoming;
        return true;
    }
    return existing == incoming;
}

SourceLoadResult load_grade_source(const std::string& path,
                                   const std::string& category,
                                   const std::string& case_id,
                                   size_t limit) {
    std::ifstream input(std::filesystem::u8path(path), std::ios::binary);
    if (!input) {
        return {{}, {ErrorCode::FileRead,
                     "could not open benchmark result JSONL for grading: " + path}};
    }
    std::map<std::pair<std::string, size_t>, size_t> group_indexes;
    std::vector<GradeGroup> groups;
    std::string line;
    size_t line_number = 0;
    size_t total_bytes = 0;
    while (std::getline(input, line)) {
        ++line_number;
        if (line.size() > kMaxGradeLineBytes) {
            return {{}, source_error(path, line_number, "line exceeds the 64 MiB limit")};
        }
        if (line.size() > kMaxGradeSourceBytes - std::min(total_bytes, kMaxGradeSourceBytes)) {
            return {{}, {ErrorCode::FileRead,
                         path + " exceeds the 256 MiB grading input limit"}};
        }
        total_bytes += line.size();
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.find_first_not_of(" \t") == std::string::npos) {
            continue;
        }
        json::ParseResult parsed = json::parse(line);
        if (!parsed.error.ok() || !parsed.value.is_object()) {
            return {{}, source_error(path, line_number,
                                     parsed.error.ok() ? "record is not a JSON object"
                                                       : parsed.error.message)};
        }
        const json::Value* type = parsed.value.get("type");
        if (type == nullptr || !type->is_string()) {
            return {{}, source_error(path, line_number, "'type' must be a string")};
        }
        if (type->string == "summary" || type->string == "dataset") {
            continue;
        }
        if (type->string != "result") {
            return {{}, source_error(path, line_number,
                                     "unsupported record type '" + type->string + "'")};
        }
        const json::Value* id = parsed.value.get("id");
        const json::Value* category_value = parsed.value.get("category");
        size_t run = 0;
        size_t turn = 0;
        bool ok = false;
        if (id == nullptr || !id->is_string() || id->string.empty() ||
            category_value == nullptr || !category_value->is_string() ||
            category_value->string.empty() ||
            !json_positive_size(parsed.value, "run", run) ||
            !json_positive_size(parsed.value, "turn", turn) ||
            !json_boolean(parsed.value, "ok", ok)) {
            return {{}, source_error(path, line_number,
                                     "result requires non-empty id/category, positive run/turn, and boolean ok")};
        }
        const auto key = std::make_pair(id->string, run);
        auto found = group_indexes.find(key);
        if (found == group_indexes.end()) {
            GradeGroup group;
            group.id = id->string;
            group.category = category_value->string;
            group.run = run;
            group.first_line = line_number;
            groups.push_back(std::move(group));
            found = group_indexes.emplace(key, groups.size() - 1).first;
        }
        GradeGroup& group = groups[found->second];
        if (group.category != category_value->string) {
            return {{}, source_error(path, line_number,
                                     "category changes within case/run group")};
        }
        TranscriptTurn source_turn;
        source_turn.turn = turn;
        source_turn.ok = ok;
        if (!optional_string(parsed.value, "prompt", source_turn.prompt) ||
            source_turn.prompt.empty()) {
            return {{}, source_error(path, line_number,
                                     "result 'prompt' must be a non-empty string")};
        }
        if (ok) {
            if (!optional_string(parsed.value, "response", source_turn.response)) {
                return {{}, source_error(path, line_number,
                                         "successful result 'response' must be a string")};
            }
        } else {
            if (!optional_string(parsed.value, "error", source_turn.error) ||
                !optional_string(parsed.value, "error_code", source_turn.error_code)) {
                return {{}, source_error(path, line_number,
                                         "failed result error fields must be strings")};
            }
            const json::Value* cancelled = parsed.value.get("cancelled");
            if (cancelled != nullptr) {
                if (cancelled->type != json::Value::Type::Bool) {
                    return {{}, source_error(path, line_number,
                                             "failed result 'cancelled' must be boolean")};
                }
                source_turn.cancelled = cancelled->boolean;
            }
        }
        if (!group.turns.emplace(turn, std::move(source_turn)).second) {
            return {{}, source_error(path, line_number,
                                     "duplicate turn " + std::to_string(turn) +
                                         " in case/run group")};
        }

        std::string language;
        std::string source_provider;
        std::string source_model;
        std::string reference_answer;
        std::string safety_classification;
        std::string safety_expected_action;
        std::vector<std::string> tags;
        std::vector<std::string> criteria;
        if (!optional_string(parsed.value, "language", language) ||
            !optional_string(parsed.value, "provider", source_provider) ||
            !optional_string(parsed.value, "model", source_model) ||
            !optional_string(parsed.value, "reference_answer", reference_answer) ||
            !optional_string_array(parsed.value, "tags", tags) ||
            !optional_string_array(parsed.value, "assessment_criteria", criteria) ||
            !optional_safety(parsed.value, safety_classification,
                             safety_expected_action)) {
            return {{}, source_error(path, line_number,
                                     "invalid result evaluation metadata")};
        }
        if (!same_or_empty(group.language, language) ||
            !same_or_empty(group.source_provider, source_provider) ||
            !same_or_empty(group.source_model, source_model) ||
            !same_or_empty(group.reference_answer, reference_answer) ||
            !same_or_empty(group.safety_classification, safety_classification) ||
            !same_or_empty(group.safety_expected_action, safety_expected_action)) {
            return {{}, source_error(path, line_number,
                                     "inconsistent metadata within case/run group")};
        }
        if (!tags.empty()) {
            if (group.tags.empty()) {
                group.tags = std::move(tags);
            } else if (group.tags != tags) {
                return {{}, source_error(path, line_number,
                                         "inconsistent tags within case/run group")};
            }
        }
        if (!criteria.empty()) {
            if (group.assessment_criteria.empty()) {
                group.assessment_criteria = std::move(criteria);
            } else if (group.assessment_criteria != criteria) {
                return {{}, source_error(path, line_number,
                                         "inconsistent assessment criteria within case/run group")};
            }
        }
    }
    if (input.bad()) {
        return {{}, {ErrorCode::FileRead,
                     "could not read benchmark result JSONL for grading: " + path}};
    }
    std::sort(groups.begin(), groups.end(), [](const GradeGroup& left,
                                                const GradeGroup& right) {
        return left.first_line < right.first_line;
    });
    groups.erase(std::remove_if(groups.begin(), groups.end(),
                                [&](const GradeGroup& group) {
        return (!category.empty() && group.category != category) ||
               (!case_id.empty() && group.id != case_id);
    }), groups.end());
    if (limit != 0 && groups.size() > limit) {
        groups.resize(limit);
    }
    if (groups.empty()) {
        std::string filters;
        if (!category.empty()) {
            filters += " category '" + category + "'";
        }
        if (!case_id.empty()) {
            filters += " case '" + case_id + "'";
        }
        return {{}, {ErrorCode::BadArgs,
                     "benchmark grading input " + path +
                         " contains no result records matching" +
                         (filters.empty() ? std::string(" the selection") : filters)}};
    }
    for (const GradeGroup& group : groups) {
        size_t expected_turn = 1;
        for (const auto& turn : group.turns) {
            if (turn.first != expected_turn++) {
                return {{}, source_error(path, group.first_line,
                                         "case '" + group.id + "' run " +
                                             std::to_string(group.run) +
                                             " has a non-contiguous transcript")};
            }
        }
    }
    return {std::move(groups), ok_error()};
}

json::Value string_value(const std::string& value) {
    json::Value result;
    result.type = json::Value::Type::String;
    result.string = value;
    return result;
}

json::Value number_value(size_t value) {
    json::Value result;
    result.type = json::Value::Type::Number;
    result.number = static_cast<double>(value);
    return result;
}

json::Value string_array_value(const std::vector<std::string>& values) {
    json::Value result;
    result.type = json::Value::Type::Array;
    for (const std::string& value : values) {
        result.array.push_back(string_value(value));
    }
    return result;
}

json::Value transcript_value(const GradeGroup& group) {
    json::Value transcript;
    transcript.type = json::Value::Type::Array;
    for (const auto& item : group.turns) {
        json::Value user;
        user.type = json::Value::Type::Object;
        user.object["role"] = string_value("user");
        user.object["content"] = string_value(item.second.prompt);
        transcript.array.push_back(std::move(user));
        if (item.second.ok) {
            json::Value assistant;
            assistant.type = json::Value::Type::Object;
            assistant.object["role"] = string_value("assistant");
            assistant.object["content"] = string_value(item.second.response);
            transcript.array.push_back(std::move(assistant));
        }
    }
    return transcript;
}

json::Value evaluation_items_value(const GradeGroup& group) {
    json::Value items;
    items.type = json::Value::Type::Array;
    if (group.assessment_criteria.empty()) {
        json::Value item;
        item.type = json::Value::Type::Object;
        item.object["index"] = number_value(0);
        item.object["kind"] = string_value("reference_answer_semantic_agreement");
        items.array.push_back(std::move(item));
        return items;
    }
    for (size_t index = 0; index < group.assessment_criteria.size(); ++index) {
        json::Value item;
        item.type = json::Value::Type::Object;
        item.object["index"] = number_value(index);
        item.object["kind"] = string_value("assessment_criterion");
        item.object["criterion"] = string_value(group.assessment_criteria[index]);
        items.array.push_back(std::move(item));
    }
    return items;
}

json::Value evaluation_basis_value(const GradeGroup& group) {
    json::Value basis;
    basis.type = json::Value::Type::Object;
    if (group.reference_answer.empty()) {
        basis.object["reference_answer"] = json::Value{};
    } else {
        basis.object["reference_answer"] = string_value(group.reference_answer);
    }
    basis.object["assessment_criteria"] =
        string_array_value(group.assessment_criteria);
    basis.object["evaluation_items"] = evaluation_items_value(group);
    if (!group.safety_classification.empty()) {
        json::Value safety;
        safety.type = json::Value::Type::Object;
        safety.object["classification"] =
            string_value(group.safety_classification);
        safety.object["expected_action"] =
            string_value(group.safety_expected_action);
        basis.object["safety"] = std::move(safety);
    }
    return basis;
}

json::Value grading_payload_value(const GradeGroup& group) {
    json::Value payload;
    payload.type = json::Value::Type::Object;
    payload.object["id"] = string_value(group.id);
    payload.object["category"] = string_value(group.category);
    payload.object["language"] = string_value(group.language);
    payload.object["tags"] = string_array_value(group.tags);
    payload.object["transcript"] = transcript_value(group);
    payload.object["evaluation_basis"] = evaluation_basis_value(group);
    return payload;
}

size_t criterion_count(const GradeGroup& group) {
    return group.assessment_criteria.empty() ? 1U : group.assessment_criteria.size();
}

bool group_has_failed_turn(const GradeGroup& group, const TranscriptTurn*& failure) {
    for (const auto& item : group.turns) {
        if (!item.second.ok) {
            failure = &item.second;
            return true;
        }
    }
    return false;
}

std::string grade_record_prefix(const GradeGroup& group,
                                const provider::RequestContext& context,
                                const std::string& source_path) {
    std::ostringstream output;
    output << "{\"type\":\"grade\",\"source_path\":" << json::quote(source_path)
           << ",\"source_provider\":" << json::quote(group.source_provider)
           << ",\"source_model\":" << json::quote(group.source_model)
           << ",\"judge_provider\":" << json::quote(context.profile.name)
           << ",\"judge_model\":" << json::quote(context.options.model)
           << ",\"id\":" << json::quote(group.id)
           << ",\"category\":" << json::quote(group.category)
           << ",\"language\":" << json::quote(group.language)
           << ",\"tags\":" << json::stringify(string_array_value(group.tags))
           << ",\"run\":" << group.run
           << ",\"transcript\":" << json::stringify(transcript_value(group))
           << ",\"evaluation_basis\":"
           << json::stringify(evaluation_basis_value(group));
    return output.str();
}

std::string error_grade_record(const GradeGroup& group,
                               const provider::RequestContext& context,
                               const std::string& source_path,
                               const Error& error,
                               bool cancelled) {
    std::ostringstream output;
    output << grade_record_prefix(group, context, source_path)
           << ",\"ok\":false,\"cancelled\":"
           << (cancelled ? "true" : "false")
           << ",\"error_code\":" << json::quote(error_code_name(error.code))
           << ",\"error\":" << json::quote(error.message) << "}\n";
    return output.str();
}

std::string successful_grade_record(const GradeGroup& group,
                                    const provider::RequestContext& context,
                                    const std::string& source_path,
                                    const JudgeGrade& grade_result) {
    std::ostringstream output;
    output << grade_record_prefix(group, context, source_path)
           << ",\"ok\":true,\"score\":" << grade_result.score
           << ",\"verdict\":" << json::quote(grade_result.verdict)
           << ",\"rationale\":" << json::quote(grade_result.rationale)
           << ",\"criteria\":[";
    for (size_t index = 0; index < grade_result.criteria.size(); ++index) {
        if (index != 0) {
            output << ",";
        }
        const GradeCriterionFinding& finding = grade_result.criteria[index];
        output << "{\"index\":" << finding.index
               << ",\"verdict\":" << json::quote(finding.verdict)
               << ",\"reason\":" << json::quote(finding.reason) << "}";
    }
    output << "]}\n";
    return output.str();
}

}  // namespace

Error find_grade_input(const cli::Options& options, std::string& path) {
    if (!options.grade_input.empty()) {
        std::filesystem::path explicit_path = std::filesystem::u8path(options.grade_input);
        std::error_code filesystem_error;
        if (!std::filesystem::is_regular_file(explicit_path, filesystem_error) ||
            filesystem_error) {
            return {ErrorCode::FileRead,
                    "--grade-input is not a readable regular file: " +
                        options.grade_input};
        }
        path = explicit_path.u8string();
        return ok_error();
    }

    std::vector<std::filesystem::path> directories;
    if (!options.output_path.empty() && options.output_path != "stdout") {
        const std::filesystem::path output_path = std::filesystem::u8path(options.output_path);
        std::error_code filesystem_error;
        const bool is_directory =
            std::filesystem::is_directory(output_path, filesystem_error);
        const bool trailing_separator = options.output_path.back() == '/' ||
                                        options.output_path.back() == '\\';
        if (!filesystem_error && (is_directory || trailing_separator)) {
            directories.push_back(output_path);
        } else {
            directories.push_back(output_path.has_parent_path()
                                      ? output_path.parent_path()
                                      : std::filesystem::path("."));
        }
    } else {
        std::error_code filesystem_error;
        const std::filesystem::path current =
            std::filesystem::current_path(filesystem_error);
        if (filesystem_error) {
            return {ErrorCode::FileRead,
                    "could not determine the current directory while selecting grading input: " +
                        filesystem_error.message()};
        }
        directories.push_back(current);
        if (current.has_parent_path()) {
            directories.push_back(current.parent_path());
        }
    }
    std::sort(directories.begin(), directories.end());
    directories.erase(std::unique(directories.begin(), directories.end()),
                      directories.end());

    struct Candidate {
        std::filesystem::path path;
        std::filesystem::file_time_type modified;
    };
    std::vector<Candidate> candidates;
    for (const std::filesystem::path& directory : directories) {
        std::error_code filesystem_error;
        std::filesystem::directory_iterator iterator(directory, filesystem_error);
        if (filesystem_error) {
            continue;
        }
        for (const std::filesystem::directory_entry& entry : iterator) {
            if (!entry.is_regular_file(filesystem_error) || filesystem_error) {
                filesystem_error.clear();
                continue;
            }
            const std::string filename = entry.path().filename().u8string();
            if (filename.rfind("benchmark-", 0) != 0 || filename.size() < 17 ||
                filename.compare(filename.size() - 6, 6, ".jsonl") != 0) {
                continue;
            }
            const auto modified = entry.last_write_time(filesystem_error);
            if (filesystem_error) {
                filesystem_error.clear();
                continue;
            }
            candidates.push_back({entry.path(), modified});
        }
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& left, const Candidate& right) {
        if (left.modified != right.modified) {
            return left.modified > right.modified;
        }
        return left.path.u8string() < right.path.u8string();
    });
    for (const Candidate& candidate : candidates) {
        SourceLoadResult loaded = load_grade_source(
            candidate.path.u8string(), options.benchmark_category,
            options.benchmark_case, 1);
        if (loaded.error.ok()) {
            path = candidate.path.u8string();
            return ok_error();
        }
    }
    std::string searched;
    for (const auto& directory : directories) {
        if (!searched.empty()) {
            searched += ", ";
        }
        searched += directory.u8string();
    }
    return {ErrorCode::FileRead,
            "no valid benchmark-*.jsonl grading input matched the requested category/case in " +
                searched + "; use --grade-input FILE for a custom-named result file"};
}

Error render_grading_case_prompt(const cli::BenchmarkGradingPrompts& prompts,
                                 const std::string& benchmark_case_json,
                                 std::string& prompt) {
    Error err = config::validate_benchmark_grading_prompts(prompts);
    if (!err.ok()) {
        return err;
    }
    prompt = prompts.case_prompt;
    const size_t placeholder = prompt.find(kBenchmarkPlaceholder);
    prompt.replace(placeholder, std::char_traits<char>::length(kBenchmarkPlaceholder),
                   benchmark_case_json);
    return ok_error();
}

Error parse_judge_grade(const std::string& response,
                        size_t expected_criterion_count,
                        JudgeGrade& grade) {
    json::ParseResult parsed = json::parse(response);
    if (!parsed.error.ok() || !parsed.value.is_object()) {
        return {ErrorCode::ProviderSchema,
                "judge response must be exactly one JSON object: " +
                    (parsed.error.ok() ? std::string("top-level value is not an object")
                                       : parsed.error.message)};
    }
    static const std::set<std::string> allowed = {
        "score", "verdict", "rationale", "criteria"};
    for (const auto& entry : parsed.value.object) {
        if (allowed.count(entry.first) == 0) {
            return {ErrorCode::ProviderSchema,
                    "judge response contains unknown field '" + entry.first + "'"};
        }
    }
    const json::Value* score = parsed.value.get("score");
    const json::Value* verdict = parsed.value.get("verdict");
    const json::Value* rationale = parsed.value.get("rationale");
    const json::Value* criteria = parsed.value.get("criteria");
    if (score == nullptr || score->type != json::Value::Type::Number ||
        std::floor(score->number) != score->number || score->number < 0.0 ||
        score->number > 100.0) {
        return {ErrorCode::ProviderSchema,
                "judge response 'score' must be an integer from 0 through 100"};
    }
    if (verdict == nullptr || !verdict->is_string() ||
        (verdict->string != "pass" && verdict->string != "partial" &&
         verdict->string != "fail")) {
        return {ErrorCode::ProviderSchema,
                "judge response 'verdict' must be pass, partial, or fail"};
    }
    if (rationale == nullptr || !rationale->is_string() ||
        ascii_trim(rationale->string).empty()) {
        return {ErrorCode::ProviderSchema,
                "judge response 'rationale' must be a non-empty string"};
    }
    if (criteria == nullptr || !criteria->is_array() ||
        criteria->array.size() != expected_criterion_count) {
        return {ErrorCode::ProviderSchema,
                "judge response 'criteria' must contain exactly " +
                    std::to_string(expected_criterion_count) + " finding(s)"};
    }
    JudgeGrade candidate;
    candidate.score = static_cast<int>(score->number);
    candidate.verdict = verdict->string;
    candidate.rationale = rationale->string;
    std::set<size_t> indexes;
    for (const json::Value& finding : criteria->array) {
        if (!finding.is_object()) {
            return {ErrorCode::ProviderSchema,
                    "judge response criterion findings must be objects"};
        }
        static const std::set<std::string> finding_allowed = {
            "index", "verdict", "reason"};
        for (const auto& entry : finding.object) {
            if (finding_allowed.count(entry.first) == 0) {
                return {ErrorCode::ProviderSchema,
                        "judge criterion contains unknown field '" + entry.first + "'"};
            }
        }
        const json::Value* index = finding.get("index");
        const json::Value* finding_verdict = finding.get("verdict");
        const json::Value* reason = finding.get("reason");
        if (index == nullptr || index->type != json::Value::Type::Number ||
            index->number < 0.0 || std::floor(index->number) != index->number ||
            index->number >= static_cast<double>(expected_criterion_count)) {
            return {ErrorCode::ProviderSchema,
                    "judge criterion 'index' is outside the evaluation basis"};
        }
        const size_t criterion_index = static_cast<size_t>(index->number);
        if (!indexes.insert(criterion_index).second) {
            return {ErrorCode::ProviderSchema,
                    "judge response contains a duplicate criterion index"};
        }
        if (finding_verdict == nullptr || !finding_verdict->is_string() ||
            (finding_verdict->string != "met" &&
             finding_verdict->string != "partial" &&
             finding_verdict->string != "not_met")) {
            return {ErrorCode::ProviderSchema,
                    "judge criterion 'verdict' must be met, partial, or not_met"};
        }
        if (reason == nullptr || !reason->is_string() ||
            ascii_trim(reason->string).empty()) {
            return {ErrorCode::ProviderSchema,
                    "judge criterion 'reason' must be a non-empty string"};
        }
        candidate.criteria.push_back(
            {criterion_index, finding_verdict->string, reason->string});
    }
    std::sort(candidate.criteria.begin(), candidate.criteria.end(),
              [](const GradeCriterionFinding& left,
                 const GradeCriterionFinding& right) {
        return left.index < right.index;
    });
    grade = std::move(candidate);
    return ok_error();
}

Error grade(const provider::RequestContext& context,
            const std::string& source_path,
            const cli::Options& options,
            std::ostream& output,
            std::ostream& status,
            const std::function<bool()>& interrupt_requested) {
    const size_t limit = options.benchmark_limit > 0
                             ? static_cast<size_t>(options.benchmark_limit)
                             : 0U;
    SourceLoadResult loaded = load_grade_source(
        source_path, options.benchmark_category, options.benchmark_case, limit);
    if (!loaded.error.ok()) {
        return loaded.error;
    }
    provider::RequestContext request_context = context;
    CancellationMonitor cancellation(interrupt_requested);
    if (request_context.profile.offline) {
        return {ErrorCode::UnsupportedFeature,
                "benchmark grading requires a model provider"};
    }
    if (request_context.options.model.empty()) {
        provider::ModelsResult models;
        Error err = provider::list_models(request_context, models, cancellation.token());
        if (!err.ok()) {
            return err;
        }
        if (!models.model_ids.empty()) {
            request_context.options.model = models.model_ids.front();
        }
    }
    if (request_context.options.model.empty()) {
        return {ErrorCode::BadArgs,
                "benchmark grading requires --model when the provider does not list a model"};
    }
    if (!options.quiet) {
        status << "Grading started: source " << source_path << "; provider "
               << request_context.profile.name << "; model "
               << request_context.options.model << "; case runs "
               << loaded.groups.size() << "; concurrency "
               << options.benchmark_concurrency << "\n";
    }

    std::atomic<size_t> next_group{0};
    std::mutex output_mutex;
    std::mutex statistics_mutex;
    GradeStatistics statistics;
    const size_t worker_count = std::max<size_t>(
        1, std::min(loaded.groups.size(),
                    static_cast<size_t>(options.benchmark_concurrency)));
    std::vector<std::thread> workers;
    workers.reserve(worker_count);
    for (size_t worker = 0; worker < worker_count; ++worker) {
        workers.emplace_back([&] {
            while (!cancellation.token().cancelled()) {
                const size_t index = next_group.fetch_add(1, std::memory_order_relaxed);
                if (index >= loaded.groups.size() || cancellation.token().cancelled()) {
                    break;
                }
                const GradeGroup& group = loaded.groups[index];
                const TranscriptTurn* source_failure = nullptr;
                if (group_has_failed_turn(group, source_failure)) {
                    const Error source_error_record{
                        source_failure->cancelled ? ErrorCode::Cancelled
                                                  : ErrorCode::ProviderSchema,
                        "source benchmark run was not completed" +
                            (source_failure->error.empty()
                                 ? std::string()
                                 : ": " + source_failure->error)};
                    const std::string record = error_grade_record(
                        group, request_context, source_path, source_error_record,
                        source_failure->cancelled);
                    {
                        std::lock_guard<std::mutex> lock(output_mutex);
                        output << record;
                    }
                    std::lock_guard<std::mutex> lock(statistics_mutex);
                    ++statistics.errors;
                    continue;
                }
                if (group.reference_answer.empty() &&
                    group.assessment_criteria.empty()) {
                    const Error basis_error{
                        ErrorCode::ProviderSchema,
                        "source benchmark case has neither reference_answer nor assessment_criteria"};
                    {
                        std::lock_guard<std::mutex> lock(output_mutex);
                        output << error_grade_record(group, request_context, source_path,
                                                     basis_error, false);
                    }
                    std::lock_guard<std::mutex> lock(statistics_mutex);
                    ++statistics.errors;
                    continue;
                }
                const std::string payload =
                    json::stringify(grading_payload_value(group));
                std::string case_prompt;
                Error err = render_grading_case_prompt(
                    options.benchmark_grading_prompts, payload, case_prompt);
                if (!err.ok()) {
                    {
                        std::lock_guard<std::mutex> lock(output_mutex);
                        output << error_grade_record(group, request_context, source_path,
                                                     err, false);
                    }
                    std::lock_guard<std::mutex> lock(statistics_mutex);
                    ++statistics.errors;
                    continue;
                }
                const std::vector<provider::Message> messages = {
                    {"system", options.benchmark_grading_prompts.system_prompt},
                    {"user", std::move(case_prompt)}};
                provider::ChatResult result;
                err = provider::send_chat_messages(
                    request_context, messages,
                    [](const std::string&) { return ok_error(); }, result,
                    cancellation.token());
                if (!err.ok()) {
                    const bool cancelled = err.code == ErrorCode::Cancelled &&
                                           cancellation.token().cancelled();
                    {
                        std::lock_guard<std::mutex> lock(output_mutex);
                        output << error_grade_record(group, request_context, source_path,
                                                     err, cancelled);
                    }
                    std::lock_guard<std::mutex> lock(statistics_mutex);
                    ++statistics.errors;
                    continue;
                }
                const ::ainiux::output::ThinkingChunk separated =
                    ::ainiux::output::split_thinking_traces(result.content);
                JudgeGrade judge_grade;
                err = parse_judge_grade(separated.visible, criterion_count(group),
                                        judge_grade);
                if (!err.ok()) {
                    {
                        std::lock_guard<std::mutex> lock(output_mutex);
                        output << error_grade_record(group, request_context, source_path,
                                                     err, false);
                    }
                    std::lock_guard<std::mutex> lock(statistics_mutex);
                    ++statistics.errors;
                    continue;
                }
                {
                    std::lock_guard<std::mutex> lock(output_mutex);
                    output << successful_grade_record(group, request_context,
                                                      source_path, judge_grade);
                }
                std::lock_guard<std::mutex> lock(statistics_mutex);
                ++statistics.graded;
                statistics.score_sum += judge_grade.score;
                if (judge_grade.verdict == "pass") {
                    ++statistics.passes;
                } else if (judge_grade.verdict == "partial") {
                    ++statistics.partials;
                } else {
                    ++statistics.failures;
                }
            }
        });
    }
    for (std::thread& worker : workers) {
        worker.join();
    }
    const bool interrupted = cancellation.interrupted();
    const double mean_score = statistics.graded == 0
                                  ? 0.0
                                  : static_cast<double>(statistics.score_sum) /
                                        static_cast<double>(statistics.graded);
    output << "{\"type\":\"summary\",\"mode\":\"grade\",\"source_path\":"
           << json::quote(source_path)
           << ",\"judge_provider\":" << json::quote(request_context.profile.name)
           << ",\"judge_model\":" << json::quote(request_context.options.model)
           << ",\"selected_case_runs\":" << loaded.groups.size()
           << ",\"graded_count\":" << statistics.graded
           << ",\"error_count\":" << statistics.errors
           << ",\"pass_count\":" << statistics.passes
           << ",\"partial_count\":" << statistics.partials
           << ",\"fail_count\":" << statistics.failures
           << ",\"mean_score\":" << mean_score
           << ",\"interrupted\":" << (interrupted ? "true" : "false")
           << "}\n";
    if (!output) {
        return {ErrorCode::FileWrite, "could not write benchmark grading JSONL output"};
    }
    if (!options.quiet) {
        std::ostringstream mean_score_text;
        mean_score_text << mean_score;
        const std::vector<std::pair<std::string, std::string>> rows = {
            {"selected_case_runs", std::to_string(loaded.groups.size())},
            {"graded_count", std::to_string(statistics.graded)},
            {"error_count", std::to_string(statistics.errors)},
            {"pass_count", std::to_string(statistics.passes)},
            {"partial_count", std::to_string(statistics.partials)},
            {"fail_count", std::to_string(statistics.failures)},
            {"mean_score", mean_score_text.str()},
            {"interrupted", interrupted ? "true" : "false"},
        };
        if (options.benchmark_summary_format == "csv") {
            status << "metric,value\n";
            for (const auto& row : rows) {
                status << row.first << "," << row.second << "\n";
            }
        } else {
            status << "Grading summary:\n"
                   << "  Metric                       Value\n"
                   << "  ---------------------------  ----------------\n";
            for (const auto& row : rows) {
                std::string label = row.first;
                std::replace(label.begin(), label.end(), '_', ' ');
                if (!label.empty()) {
                    label.front() = static_cast<char>(
                        std::toupper(static_cast<unsigned char>(label.front())));
                }
                status << "  " << std::left << std::setw(27) << label << "  "
                       << row.second << "\n";
            }
            status << std::right;
        }
    }
    if (interrupted) {
        return {ErrorCode::Cancelled, "benchmark grading cancelled by Ctrl+C"};
    }
    if (statistics.errors != 0) {
        return {ErrorCode::ProviderSchema,
                std::to_string(statistics.errors) +
                    " selected benchmark grade(s) failed"};
    }
    return ok_error();
}

}  // namespace ainiux::benchmark
