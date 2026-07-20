#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "common.hpp"
#include "json/json.hpp"

namespace ainiux::agent {

// Maximum original-argument text retained in invalid-argument tool results.
inline constexpr std::size_t kToolArgumentsErrorCap = 2000;

enum class ToolArgStage {
    EmptyObject = 0,
    StrictJson,
    ExtractedObject,
    RepairedJson,
    // Stages that require registry/schema context are applied by callers after
    // parse_tool_arguments returns a Value.
};

struct ToolArgParseResult {
    json::Value value;                 // object on success
    Error error;                       // ok on success
    ToolArgStage stage = ToolArgStage::StrictJson;
    std::string original_arguments;    // input as received (uncapped)
    std::string normalized_arguments;  // text that was finally accepted, if any
};

// 8-stage lenient-but-bounded argument pipeline (stages 1-5 here; 6-7 are
// schema/name helpers below; stage 8 is "return error" when still invalid).
// Stop at the first stage that yields a JSON object.
ToolArgParseResult parse_tool_arguments(const std::string& arguments_text);

// Truncate original text for rich error tool-results (UTF-8 safe at byte edges).
std::string truncate_tool_arguments_for_error(const std::string& text,
                                              std::size_t cap = kToolArgumentsErrorCap);

// Stage 6: coerce stringly types only where the tool schema expects them.
// `schema_properties` is a JSON object whose keys are parameter names and whose
// values are JSON Schema fragments with at least "type".
// Never invents values for missing required fields.
Error coerce_tool_arguments(json::Value& args_object,
                            const json::Value& schema_properties);

// Stage 7: exact, then case-insensitive, then snake/camel normalization.
// No fuzzy/edit-distance matching. Returns empty string if unresolved.
std::string repair_tool_name(const std::string& requested,
                             const std::vector<std::string>& known_names);

// Rich invalid-arguments tool-result body (bounded original args). Compatible
// with the existing ok/error envelope used by ReadToolRegistry.
std::string invalid_arguments_tool_result(const std::string& tool_name,
                                          const std::string& message,
                                          const std::string& received_arguments);

// Parse XML-alike single tool call channel:
//   <tool_call><name>...</name><args>{...}</args></tool_call>
// One call per turn; multiple blocks are an error. Arguments still go through
// parse_tool_arguments.
struct XmlToolCallParseResult {
    std::string name;
    std::string arguments_text;
    Error error;
    bool found = false;
};

XmlToolCallParseResult parse_xml_tool_call(const std::string& assistant_text);

}  // namespace ainiux::agent
