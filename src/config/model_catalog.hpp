#pragma once

#include <string>
#include <vector>

#include "ainiux/model_setting.hpp"
#include "common.hpp"

namespace ainiux::config {

Error parse_reasoning_selection(const std::string& text,
                                ReasoningSelection& selection,
                                bool allow_auto = true);
std::string reasoning_selection_value(const ReasoningSelection& selection);
std::string reasoning_selection_display(const ReasoningSelection& selection);
bool reasoning_selection_disables(const ReasoningSelection& selection);
Error resolve_reasoning_off(const ModelCatalog& catalog,
                            const std::string& provider,
                            const std::string& api,
                            const std::string& model,
                            ReasoningSelection& selection);

bool parse_reasoning_protocol(const std::string& text, ReasoningProtocol& protocol);
const char* reasoning_protocol_name(ReasoningProtocol protocol);
std::string reasoning_protocol_names();

bool parse_temperature_support(const std::string& text, TemperatureSupport& support);
const char* temperature_support_name(TemperatureSupport support);

bool model_regex_matches(const std::string& expression, const std::string& model);
const ModelCapability* resolve_model_capability(const ModelCatalog& catalog,
                                                const std::string& provider,
                                                const std::string& api,
                                                const std::string& model);
std::string reasoning_catalog_warning(const ModelCatalog& catalog,
                                      const std::string& provider,
                                      const std::string& api,
                                      const std::string& model,
                                      const ReasoningSelection& selection);
const ModelSetting* find_model_preset(const ModelCatalog& catalog,
                                      const ModelCapability& capability,
                                      const std::string& purpose);
bool temperature_supported_for(const ModelCapability& capability,
                               const ReasoningSelection& selection);
// Default 1.0 when the catalog record omits temperature_max.
double temperature_max_for(const ModelCapability* capability);
std::string temperature_advisory(const ModelCapability* capability,
                                 const ReasoningSelection& selection,
                                 bool explicit_temperature);
struct ReasoningSelectorData {
    std::vector<ReasoningSelection> values;
    std::vector<std::string> labels;
    std::string guidance;
};
ReasoningSelectorData reasoning_selector_data(const ModelCatalog& catalog,
                                              const std::string& provider,
                                              const std::string& api,
                                              const std::string& model);
bool next_reasoning_selection(const ModelCatalog& catalog,
                              const std::string& provider,
                              const std::string& api,
                              const std::string& model,
                              const ReasoningSelection& current,
                              ReasoningSelection& next);
std::string reasoning_selector_text(const ModelCatalog& catalog,
                                    const std::string& provider,
                                    const std::string& api,
                                    const std::string& model,
                                    size_t selected,
                                    std::vector<ReasoningSelection>* selections = nullptr);

}  // namespace ainiux::config
