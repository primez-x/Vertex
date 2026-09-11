#include "sketch/field_adapter_contract.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <set>
#include <stdexcept>

namespace sketch {
namespace {
using Json = nlohmann::json;
constexpr std::size_t max_payload = 1024 * 1024;
void require(bool condition) {
  if (!condition) throw std::invalid_argument("invalid or unsupported field adapter contract");
}
bool text(std::string_view s, std::size_t limit = 4096) {
  return !s.empty() && s.size() <= limit &&
    std::any_of(s.begin(), s.end(), [](unsigned char c) { return c > 32; }) &&
    std::none_of(s.begin(), s.end(), [](unsigned char c) { return c < 32 || c == 127; });
}
bool id(std::string_view s) {
  return !s.empty() && s.size() <= 128 && std::all_of(s.begin(), s.end(), [](unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
      (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
  });
}
void version(IntegrationVersion v) { require(v == IntegrationVersion{1, 0}); }
bool timestamp(const std::string &s) {
  if (s.size() != 20 || s[4] != '-' || s[7] != '-' || s[10] != 'T' ||
      s[13] != ':' || s[16] != ':' || s[19] != 'Z') return false;
  for (std::size_t i = 0; i < s.size(); ++i)
    if (i != 4 && i != 7 && i != 10 && i != 13 && i != 16 && i != 19 &&
        (s[i] < '0' || s[i] > '9')) return false;
  auto n = [&](std::size_t start, std::size_t count) { return std::stoi(s.substr(start, count)); };
  const std::chrono::year_month_day date{std::chrono::year{n(0, 4)},
    std::chrono::month{static_cast<unsigned>(n(5, 2))}, std::chrono::day{static_cast<unsigned>(n(8, 2))}};
  return n(0, 4) > 0 && date.ok() && n(11, 2) < 24 && n(14, 2) < 60 && n(17, 2) < 60;
}
void keys(const Json &j, std::initializer_list<const char *> expected) {
  require(j.is_object() && j.size() == expected.size());
  for (auto key : expected) require(j.contains(key));
}
Json parse(std::string_view payload) {
  require(!payload.empty() && payload.size() <= max_payload);
  // Reject duplicate keys instead of accepting the parser's last-value-wins behavior.
  std::vector<std::set<std::string>> seen;
  auto callback = [&](int depth, Json::parse_event_t event, Json &value) {
    require(depth <= 16);
    if (event == Json::parse_event_t::object_start) seen.emplace_back();
    if (event == Json::parse_event_t::key) require(seen.back().insert(value.get<std::string>()).second);
    if (event == Json::parse_event_t::object_end) seen.pop_back();
    return true;
  };
  try { return Json::parse(payload, callback); }
  catch (const Json::exception &) { throw std::invalid_argument("malformed field adapter JSON"); }
}
IntegrationVersion read_version(const Json &j) {
  keys(j, {"major", "minor"});
  require(j.at("major").is_number_integer() && j.at("minor").is_number_integer());
  require(j.at("major") == 1 && j.at("minor") == 0);
  return {1, 0};
}
Json version_json() { return {{"major", 1}, {"minor", 0}}; }
Json mapping_object(const AppraisalMappingDescriptor &d) {
  validate_appraisal_mapping(d);
  auto fields = d.fields;
  std::sort(fields.begin(), fields.end(), [](const auto &a, const auto &b) { return a.source_field < b.source_field; });
  Json rows = Json::array();
  for (const auto &f : fields) rows.push_back({{"source_field", f.source_field},
      {"target_field", f.target_field}, {"required", f.required}});
  return {{"protocol_version", version_json()}, {"adapter_id", d.adapter_id},
    {"application", d.application}, {"application_version", d.application_version},
    {"exchange_format", d.exchange_format}, {"provenance", d.provenance},
    {"timeout_ms", d.timeout_ms}, {"fields", rows}};
}
}

void validate_disto_measurement(const DistoMeasurementRecord &r) {
  version(r.protocol_version);
  require(id(r.reading_id) && id(r.target_field) && std::isfinite(r.value) && r.value > 0 &&
    (r.unit == "m" || r.unit == "mm" || r.unit == "cm" || r.unit == "ft" || r.unit == "in") &&
    timestamp(r.captured_at) && text(r.model, 128) && text(r.firmware, 128) &&
    text(r.transport, 128) && text(r.provenance));
}
std::string disto_measurement_json(const DistoMeasurementRecord &r) {
  validate_disto_measurement(r);
  return Json{{"protocol_version", version_json()}, {"reading_id", r.reading_id},
    {"target_field", r.target_field}, {"value", r.value}, {"unit", r.unit},
    {"captured_at", r.captured_at}, {"model", r.model}, {"firmware", r.firmware},
    {"transport", r.transport}, {"provenance", r.provenance}}.dump();
}
DistoMeasurementRecord parse_disto_measurement_json(std::string_view payload) {
  try {
    const auto j = parse(payload);
    keys(j, {"protocol_version", "reading_id", "target_field", "value", "unit", "captured_at",
             "model", "firmware", "transport", "provenance"});
    require(j.at("value").is_number());
    DistoMeasurementRecord r{read_version(j.at("protocol_version")), j.at("reading_id"),
      j.at("target_field"), j.at("value"), j.at("unit"), j.at("captured_at"), j.at("model"),
      j.at("firmware"), j.at("transport"), j.at("provenance")};
    validate_disto_measurement(r);
    return r;
  } catch (const Json::exception &) { throw std::invalid_argument("malformed DISTO measurement JSON"); }
}
void assign_disto_measurement(std::string_view selected, std::optional<DistoMeasurementRecord> &destination,
                             const DistoMeasurementRecord &r) {
  validate_disto_measurement(r);
  require(selected == r.target_field && !destination.has_value());
  destination.emplace(r);
}
void validate_appraisal_mapping(const AppraisalMappingDescriptor &d) {
  version(d.protocol_version);
  require(id(d.adapter_id) && text(d.application, 128) && text(d.application_version, 128) &&
    id(d.exchange_format) && text(d.provenance) && !d.fields.empty() && d.fields.size() <= 256 &&
    d.timeout_ms > 0 && d.timeout_ms <= 120000);
  std::set<std::string> sources, targets;
  for (const auto &f : d.fields)
    require(id(f.source_field) && id(f.target_field) && sources.insert(f.source_field).second &&
            targets.insert(f.target_field).second);
}
std::string appraisal_mapping_json(const AppraisalMappingDescriptor &d) { return mapping_object(d).dump(); }
AppraisalMappingDescriptor parse_appraisal_mapping_json(std::string_view payload) {
  try {
    const auto j = parse(payload);
    keys(j, {"protocol_version", "adapter_id", "application", "application_version", "exchange_format",
             "provenance", "timeout_ms", "fields"});
    require(j.at("timeout_ms").is_number_integer() && j.at("timeout_ms") > 0 && j.at("timeout_ms") <= 120000);
    require(j.at("fields").is_array() && j.at("fields").size() <= 256);
    AppraisalMappingDescriptor d{read_version(j.at("protocol_version")), j.at("adapter_id"),
      j.at("application"), j.at("application_version"), j.at("exchange_format"), j.at("provenance"),
      {}, j.at("timeout_ms")};
    for (const auto &f : j.at("fields")) {
      keys(f, {"source_field", "target_field", "required"});
      require(f.at("required").is_boolean());
      d.fields.push_back({f.at("source_field"), f.at("target_field"), f.at("required")});
    }
    validate_appraisal_mapping(d);
    return d;
  } catch (const Json::exception &) { throw std::invalid_argument("malformed appraisal mapping JSON"); }
}
bool appraisal_contract_compatible(const AppraisalMappingDescriptor &d, IntegrationVersion v,
    std::string_view application, std::string_view application_version, std::string_view format) {
  validate_appraisal_mapping(d);
  return d.protocol_version == v && d.application == application &&
    d.application_version == application_version && d.exchange_format == format;
}
std::string appraisal_payload_json(const AppraisalMappingDescriptor &d,
                                  const std::map<std::string, std::string> &values) {
  validate_appraisal_mapping(d);
  require(values.size() <= d.fields.size());
  Json mapped = Json::object();
  std::set<std::string> consumed;
  for (const auto &f : d.fields) {
    const auto it = values.find(f.source_field);
    require(!f.required || it != values.end());
    if (it == values.end()) continue;
    require(text(it->second, 2048));
    mapped[f.target_field] = it->second;
    consumed.insert(it->first);
  }
  require(consumed.size() == values.size());
  return Json{{"contract", mapping_object(d)}, {"values", mapped}}.dump();
}
} // namespace sketch
