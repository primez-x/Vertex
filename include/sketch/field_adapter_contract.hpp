#pragma once

#include "sketch/integration_adapter.hpp"
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sketch {

// These are local exchange contracts, not discovered vendor protocols.
struct DistoMeasurementRecord {
  IntegrationVersion protocol_version;
  std::string reading_id;
  std::string target_field;
  double value{};
  std::string unit; // m, mm, cm, ft, in; never silently converted
  std::string captured_at; // UTC YYYY-MM-DDTHH:MM:SSZ
  std::string model;
  std::string firmware;
  std::string transport;
  std::string provenance;
};

void validate_disto_measurement(const DistoMeasurementRecord &record);
[[nodiscard]] std::string disto_measurement_json(const DistoMeasurementRecord &record);
[[nodiscard]] DistoMeasurementRecord parse_disto_measurement_json(std::string_view payload);
// Strong failure guarantee: existing or differently selected fields are unchanged.
void assign_disto_measurement(std::string_view selected_field,
                             std::optional<DistoMeasurementRecord> &destination,
                             const DistoMeasurementRecord &record);

struct AppraisalFieldMapping {
  std::string source_field;
  std::string target_field;
  bool required{true};
};

struct AppraisalMappingDescriptor {
  IntegrationVersion protocol_version;
  std::string adapter_id;
  std::string application;
  std::string application_version;
  std::string exchange_format;
  std::string provenance;
  std::vector<AppraisalFieldMapping> fields;
  std::uint32_t timeout_ms{5000};
};

void validate_appraisal_mapping(const AppraisalMappingDescriptor &descriptor);
[[nodiscard]] std::string appraisal_mapping_json(const AppraisalMappingDescriptor &descriptor);
[[nodiscard]] AppraisalMappingDescriptor parse_appraisal_mapping_json(std::string_view payload);
// Exact version and application identity must match before a caller exchanges data.
[[nodiscard]] bool appraisal_contract_compatible(const AppraisalMappingDescriptor &descriptor,
    IntegrationVersion version, std::string_view application, std::string_view application_version,
    std::string_view exchange_format);
// One-way text mapping; rejects unknown fields, missing/empty required values.
// Output includes identity/version and sorted mapped values; no external calls occur.
[[nodiscard]] std::string appraisal_payload_json(const AppraisalMappingDescriptor &descriptor,
    const std::map<std::string, std::string> &source_values);

} // namespace sketch
