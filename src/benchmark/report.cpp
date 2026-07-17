#include "benchmark/benchmark.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <functional>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>

#include "json/json.hpp"

namespace pkchat::benchmark {
namespace {

constexpr size_t kMaxReportLineBytes = 64U * 1024U * 1024U;

std::string json_compact(const json::Value& value) {
    switch (value.type) {
        case json::Value::Type::Null:
            return "null";
        case json::Value::Type::Bool:
            return value.boolean ? "true" : "false";
        case json::Value::Type::Number: {
            std::ostringstream output;
            output << std::setprecision(15) << value.number;
            return output.str();
        }
        case json::Value::Type::String:
            return json::quote(value.string);
        case json::Value::Type::Array: {
            std::string output = "[";
            for (size_t index = 0; index < value.array.size(); ++index) {
                if (index != 0) {
                    output.push_back(',');
                }
                output += json_compact(value.array[index]);
            }
            output.push_back(']');
            return output;
        }
        case json::Value::Type::Object: {
            std::string output = "{";
            bool first = true;
            for (const auto& entry : value.object) {
                if (!first) {
                    output.push_back(',');
                }
                first = false;
                output += json::quote(entry.first) + ":" + json_compact(entry.second);
            }
            output.push_back('}');
            return output;
        }
    }
    return "null";
}

std::string markdown_table_cell(const std::string& text) {
    std::string output;
    output.reserve(text.size());
    for (char ch : text) {
        if (ch == '&') {
            output += "&amp;";
        } else if (ch == '<') {
            output += "&lt;";
        } else if (ch == '>') {
            output += "&gt;";
        } else if (ch == '|') {
            output += "\\|";
        } else if (ch == '`') {
            output += "\\`";
        } else if (ch == '\n') {
            output += "<br>";
        } else if (ch != '\r') {
            output.push_back(ch);
        }
    }
    return output;
}

std::string markdown_heading_text(const std::string& text) {
    std::string output;
    output.reserve(text.size());
    for (char ch : text) {
        if (ch == '\n' || ch == '\r') {
            output.push_back(' ');
        } else {
            if (ch == '\\' || ch == '`' || ch == '*' || ch == '_' || ch == '[' ||
                ch == ']' || ch == '<' || ch == '>' || ch == '#' || ch == '|') {
                output.push_back('\\');
            }
            output.push_back(ch);
        }
    }
    return output;
}

std::string markdown_link_destination(const std::string& text) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string output;
    output.reserve(text.size());
    for (unsigned char ch : text) {
        if (ch <= 0x20U || ch == '<' || ch == '>' || ch == '\\') {
            output.push_back('%');
            output.push_back(kHex[ch >> 4U]);
            output.push_back(kHex[ch & 0x0FU]);
        } else {
            output.push_back(static_cast<char>(ch));
        }
    }
    return output;
}

void write_fenced_block(std::ostream& output,
                        const std::string& language,
                        const std::string& content) {
    size_t longest_ticks = 0;
    size_t current_ticks = 0;
    for (char ch : content) {
        if (ch == '`') {
            ++current_ticks;
            longest_ticks = std::max(longest_ticks, current_ticks);
        } else {
            current_ticks = 0;
        }
    }
    const std::string fence(std::max<size_t>(3, longest_ticks + 1), '`');
    output << fence << language << "\n" << content;
    if (content.empty() || content.back() != '\n') {
        output << "\n";
    }
    output << fence << "\n\n";
}

void write_markdown_table(std::ostream& output,
                          const json::Value& record,
                          const std::set<std::string>& excluded = {}) {
    output << "| Field | Value |\n|---|---|\n";
    for (const auto& entry : record.object) {
        if (excluded.count(entry.first) != 0) {
            continue;
        }
        std::string value = entry.second.is_string()
                                ? entry.second.string
                                : json_compact(entry.second);
        output << "| " << markdown_table_cell(entry.first) << " | "
               << markdown_table_cell(value) << " |\n";
    }
    output << "\n";
}

Error for_each_report_record(
    const std::string& path,
    const std::function<Error(const json::Value&, size_t)>& callback) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return {ErrorCode::FileRead,
                "could not open benchmark JSONL output for Markdown conversion: " + path};
    }
    std::string line;
    size_t line_number = 0;
    while (std::getline(input, line)) {
        ++line_number;
        if (line.size() > kMaxReportLineBytes) {
            return {ErrorCode::FileRead,
                    path + ":" + std::to_string(line_number) +
                        ": benchmark result line exceeds the 64 MiB Markdown report limit"};
        }
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }
        json::ParseResult parsed = json::parse(line);
        if (!parsed.error.ok() || !parsed.value.is_object()) {
            const std::string detail = parsed.error.ok()
                                           ? "record is not a JSON object"
                                           : parsed.error.message;
            return {ErrorCode::JsonParse,
                    path + ":" + std::to_string(line_number) +
                        ": could not create Markdown benchmark report: " + detail};
        }
        Error err = callback(parsed.value, line_number);
        if (!err.ok()) {
            return err;
        }
    }
    if (input.bad()) {
        return {ErrorCode::FileRead,
                "could not read benchmark JSONL output for Markdown conversion: " + path};
    }
    return ok_error();
}

std::string record_string(const json::Value& record, const std::string& key) {
    const json::Value* value = record.get(key);
    return value != nullptr && value->is_string() ? value->string : std::string();
}

long long record_integer(const json::Value& record, const std::string& key) {
    const json::Value* value = record.get(key);
    if (value == nullptr || value->type != json::Value::Type::Number ||
        value->number < 0.0 || value->number > static_cast<double>(
                                               std::numeric_limits<long long>::max())) {
        return -1;
    }
    return static_cast<long long>(value->number);
}

void write_special_markdown_field(std::ostream& output,
                                  const json::Value& record,
                                  const std::string& key,
                                  const std::string& heading,
                                  const std::string& language) {
    const json::Value* value = record.get(key);
    if (value == nullptr || value->is_null()) {
        return;
    }
    output << "#### " << heading << "\n\n";
    write_fenced_block(output, language,
                       value->is_string() ? value->string : json_compact(*value));
}

void write_markdown_external_file(std::ostream& output, const json::Value& record) {
    const json::Value* value = record.get("external_file_url");
    if (value == nullptr || !value->is_string() || value->string.empty()) {
        return;
    }
    output << "#### External File\n\n"
           << "[Open external file](<" << markdown_link_destination(value->string)
           << ">)\n\n";
}

void write_markdown_assessment_criteria(std::ostream& output,
                                        const json::Value& record) {
    const json::Value* value = record.get("assessment_criteria");
    if (value == nullptr || value->is_null()) {
        return;
    }
    output << "#### Assessment Criteria\n\n";
    if (!value->is_array()) {
        write_fenced_block(output, "json", json_compact(*value));
        return;
    }
    for (const json::Value& criterion : value->array) {
        output << "- "
               << markdown_table_cell(criterion.is_string()
                                          ? criterion.string
                                          : json_compact(criterion))
               << "\n";
    }
    output << "\n";
}

void write_grade_transcript(std::ostream& output, const json::Value& record) {
    const json::Value* transcript = record.get("transcript");
    if (transcript == nullptr || !transcript->is_array()) {
        return;
    }
    output << "#### Transcript\n\n";
    size_t message_number = 0;
    for (const json::Value& message : transcript->array) {
        ++message_number;
        const std::string role = message.is_object()
                                     ? record_string(message, "role")
                                     : std::string();
        const std::string content = message.is_object()
                                        ? record_string(message, "content")
                                        : json_compact(message);
        output << "##### "
               << markdown_heading_text(role.empty()
                                            ? "Message " + std::to_string(message_number)
                                            : role == "user" ? "User" : "Assistant")
               << "\n\n";
        write_fenced_block(output, "text", content);
    }
}

void write_grade_evaluation_basis(std::ostream& output,
                                  const json::Value& record) {
    const json::Value* basis = record.get("evaluation_basis");
    if (basis == nullptr) {
        return;
    }
    output << "#### Evaluation Basis\n\n";
    if (!basis->is_object()) {
        write_fenced_block(output, "json", json_compact(*basis));
        return;
    }
    const json::Value* reference = basis->get("reference_answer");
    if (reference != nullptr && !reference->is_null()) {
        output << "##### Reference Answer\n\n";
        write_fenced_block(output, "text",
                           reference->is_string() ? reference->string
                                                  : json_compact(*reference));
    }
    const json::Value* items = basis->get("evaluation_items");
    if (items != nullptr && items->is_array()) {
        output << "##### Evaluation Items\n\n";
        for (const json::Value& item : items->array) {
            const json::Value* index = item.is_object() ? item.get("index") : nullptr;
            const json::Value* criterion =
                item.is_object() ? item.get("criterion") : nullptr;
            const json::Value* kind = item.is_object() ? item.get("kind") : nullptr;
            output << "- ";
            if (index != nullptr) {
                output << markdown_table_cell(json_compact(*index)) << ": ";
            }
            if (criterion != nullptr && criterion->is_string()) {
                output << markdown_table_cell(criterion->string);
            } else if (kind != nullptr && kind->is_string()) {
                output << markdown_table_cell(kind->string);
            } else {
                output << markdown_table_cell(json_compact(item));
            }
            output << "\n";
        }
        output << "\n";
    }
}

void write_grade_findings(std::ostream& output, const json::Value& record) {
    const json::Value* findings = record.get("criteria");
    if (findings == nullptr || !findings->is_array()) {
        return;
    }
    output << "#### Criterion Findings\n\n"
           << "| Index | Verdict | Reason |\n|---:|---|---|\n";
    for (const json::Value& finding : findings->array) {
        const json::Value* index = finding.is_object() ? finding.get("index") : nullptr;
        output << "| "
               << markdown_table_cell(index == nullptr ? std::string()
                                                        : json_compact(*index))
               << " | " << markdown_table_cell(record_string(finding, "verdict"))
               << " | " << markdown_table_cell(record_string(finding, "reason"))
               << " |\n";
    }
    output << "\n";
}

}  // namespace

std::string markdown_report_path(const std::string& jsonl_path) {
    const size_t separator = jsonl_path.find_last_of("/\\");
    const size_t dot = jsonl_path.find_last_of('.');
    if (dot != std::string::npos &&
        (separator == std::string::npos || dot > separator)) {
        std::string extension = jsonl_path.substr(dot);
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](unsigned char ch) {
                           return static_cast<char>(std::tolower(ch));
                       });
        if (extension == ".jsonl") {
            return jsonl_path.substr(0, dot) + ".md";
        }
    }
    return jsonl_path + ".md";
}

Error write_markdown_report(const std::string& jsonl_path,
                            const std::string& markdown_path) {
    if (jsonl_path.empty() || markdown_path.empty()) {
        return {ErrorCode::BadArgs,
                "benchmark Markdown conversion requires non-empty input and output paths"};
    }
    if (jsonl_path == markdown_path) {
        return {ErrorCode::BadArgs,
                "benchmark Markdown report path must differ from the JSONL output path: " +
                    jsonl_path};
    }

    json::Value overview;
    bool have_overview = false;
    size_t record_count = 0;
    Error err = for_each_report_record(
        jsonl_path, [&](const json::Value& record, size_t line_number) {
            ++record_count;
            const std::string type = record_string(record, "type");
            if (type != "summary" && type != "dataset") {
                return ok_error();
            }
            if (have_overview) {
                return Error{ErrorCode::JsonParse,
                             jsonl_path + ":" + std::to_string(line_number) +
                                 ": benchmark JSONL contains more than one summary/overview record"};
            }
            overview = record;
            have_overview = true;
            return ok_error();
        });
    if (!err.ok()) {
        return err;
    }
    if (record_count == 0) {
        return {ErrorCode::JsonParse,
                "cannot create Markdown report from empty benchmark JSONL output: " +
                    jsonl_path};
    }

    std::ofstream output(markdown_path, std::ios::binary | std::ios::trunc);
    if (!output) {
        return {ErrorCode::FileWrite,
                "could not open benchmark Markdown report for writing: " + markdown_path};
    }
    const bool grading_report = have_overview &&
                                record_string(overview, "mode") == "grade";
    output << (grading_report ? "# pkchat Benchmark Grading Report\n\n"
                              : "# pkchat Benchmark Report\n\n")
           << "**JSONL source:** " << markdown_heading_text(jsonl_path) << "\n\n";
    if (have_overview) {
        output << "## "
               << (record_string(overview, "type") == "summary" ? "Summary" : "Dataset")
               << "\n\n";
        write_markdown_table(output, overview);
    }

    bool wrote_results_heading = false;
    bool wrote_grades_heading = false;
    bool wrote_cases_heading = false;
    size_t detail_number = 0;
    err = for_each_report_record(
        jsonl_path, [&](const json::Value& record, size_t) {
            const std::string type = record_string(record, "type");
            if (type == "summary" || type == "dataset") {
                return ok_error();
            }
            ++detail_number;
            if (type == "grade") {
                if (!wrote_grades_heading) {
                    output << "## Grades\n\n";
                    wrote_grades_heading = true;
                }
                const std::string id = record_string(record, "id");
                const long long run = record_integer(record, "run");
                output << "### "
                       << markdown_heading_text(id.empty()
                                                    ? "Grade " + std::to_string(detail_number)
                                                    : id);
                if (run >= 0) {
                    output << " - Run " << run;
                }
                output << "\n\n";
                write_markdown_table(output, record,
                                     {"transcript", "evaluation_basis", "criteria",
                                      "rationale", "error"});
                write_grade_transcript(output, record);
                write_grade_evaluation_basis(output, record);
                write_grade_findings(output, record);
                write_special_markdown_field(output, record, "rationale",
                                             "Rationale", "text");
                write_special_markdown_field(output, record, "error", "Error",
                                             "text");
                return ok_error();
            }
            if (type == "result") {
                if (!wrote_results_heading) {
                    output << "## Results\n\n";
                    wrote_results_heading = true;
                }
                const std::string id = record_string(record, "id");
                const long long run = record_integer(record, "run");
                const long long turn = record_integer(record, "turn");
                output << "### "
                       << markdown_heading_text(id.empty()
                                                    ? "Result " + std::to_string(detail_number)
                                                    : id);
                if (run >= 0 || turn >= 0) {
                    output << " -";
                    if (run >= 0) {
                        output << " Run " << run;
                    }
                    if (turn >= 0) {
                        output << ", Turn " << turn;
                    }
                }
                output << "\n\n";
                write_markdown_table(output, record,
                                     {"prompt", "external_file_url", "reference_answer",
                                      "assessment_criteria", "response", "error",
                                      "provider_usage"});
                write_special_markdown_field(output, record, "prompt", "Prompt", "text");
                write_markdown_external_file(output, record);
                write_special_markdown_field(output, record, "reference_answer",
                                             "Correct Answer", "text");
                write_markdown_assessment_criteria(output, record);
                write_special_markdown_field(output, record, "provider_usage",
                                             "Provider Usage", "json");
                write_special_markdown_field(output, record, "response", "Response", "text");
                write_special_markdown_field(output, record, "error", "Error", "text");
                return ok_error();
            }

            if (!wrote_cases_heading) {
                output << "## Cases\n\n";
                wrote_cases_heading = true;
            }
            const std::string id = record_string(record, "id");
            output << "### "
                   << markdown_heading_text(id.empty()
                                                ? "Record " + std::to_string(detail_number)
                                                : id)
                   << "\n\n";
            write_markdown_table(output, record);
            return ok_error();
        });
    if (!err.ok()) {
        output.close();
        return err;
    }
    output.flush();
    if (!output) {
        return {ErrorCode::FileWrite,
                "could not write benchmark Markdown report: " + markdown_path};
    }
    return ok_error();
}

}  // namespace pkchat::benchmark
