#include "sketch/field_adapter_contract.hpp"
#include <algorithm>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
using namespace sketch;
void check(bool result) { if (!result) throw std::runtime_error("field adapter contract check failed"); }
template<class F> void rejects(F action) {
  try { action(); } catch (const std::invalid_argument &) { return; }
  throw std::runtime_error("invalid contract accepted");
}
void run() {
  DistoMeasurementRecord r{{1, 0}, "reading-1", "wall.length", 12.5, "ft",
    "2024-02-29T23:59:59Z", "synthetic-disto", "fixture-1", "offline-fixture", "Synthetic reading"};
  const auto json = disto_measurement_json(r);
  const auto decoded = parse_disto_measurement_json(json);
  check(disto_measurement_json(decoded) == json && decoded.unit == "ft" && decoded.value == 12.5);
  std::optional<DistoMeasurementRecord> field;
  rejects([&] { assign_disto_measurement("other", field, r); });
  check(!field);
  assign_disto_measurement("wall.length", field, r);
  auto next = r; next.value = 5;
  rejects([&] { assign_disto_measurement("wall.length", field, next); });
  check(field->value == 12.5);
  for (int fault = 0; fault < 9; ++fault) {
    auto bad = r;
    switch (fault) {
    case 0: bad.protocol_version.minor = 1; break;
    case 1: bad.value = std::numeric_limits<double>::infinity(); break;
    case 2: bad.value = -1; break;
    case 3: bad.unit = "yards"; break;
    case 4: bad.captured_at = "2023-02-29T23:59:59Z"; break;
    case 5: bad.captured_at = "2024-02-29T24:59:59Z"; break;
    case 6: bad.provenance.clear(); break;
    case 7: bad.target_field = "other\nfield"; break;
    case 8: bad.model = std::string(129, 'x'); break;
    }
    rejects([&] { (void)disto_measurement_json(bad); });
  }
  rejects([&] { (void)parse_disto_measurement_json("{}"); });
  rejects([&] { (void)parse_disto_measurement_json("{\"reading_id\":1,\"reading_id\":2}"); });
  auto unknown = json; unknown.insert(1, "\"unknown\":0,");
  rejects([&] { (void)parse_disto_measurement_json(unknown); });
  auto fractional_version = json;
  const auto major = fractional_version.find("\"major\":1");
  fractional_version.replace(major, 9, "\"major\":1.5");
  rejects([&] { (void)parse_disto_measurement_json(fractional_version); });

  AppraisalMappingDescriptor d{{1, 0}, "fixture.appraisal", "Synthetic Appraisal", "fixture-1",
    "fixture.text", "Synthetic mapping; no vendor compatibility claim",
    {{"area", "gross_area", true}, {"comment", "notes", false}}, 5000};
  const auto descriptor_json = appraisal_mapping_json(d);
  check(appraisal_mapping_json(parse_appraisal_mapping_json(descriptor_json)) == descriptor_json);
  check(appraisal_contract_compatible(d, {1, 0}, d.application, d.application_version, d.exchange_format));
  check(!appraisal_contract_compatible(d, {1, 1}, d.application, d.application_version, d.exchange_format));
  check(!appraisal_contract_compatible(d, {1, 0}, d.application, "other", d.exchange_format));
  const auto payload = appraisal_payload_json(d, {{"area", "120.5"}});
  check(payload.find("\"gross_area\":\"120.5\"") != std::string::npos);
  check(payload.find("\"notes\":") == std::string::npos);
  std::reverse(d.fields.begin(), d.fields.end());
  check(appraisal_mapping_json(d) == descriptor_json);
  check(appraisal_payload_json(d, {{"area", "120.5"}}) == payload);
  rejects([&] { (void)appraisal_payload_json(d, {}); });
  rejects([&] { (void)appraisal_payload_json(d, {{"area", ""}}); });
  rejects([&] { (void)appraisal_payload_json(d, {{"area", "1"}, {"unknown", "2"}}); });
  for (int fault = 0; fault < 6; ++fault) {
    auto bad = d;
    switch (fault) {
    case 0: bad.timeout_ms = 0; break;
    case 1: bad.timeout_ms = 120001; break;
    case 2: bad.fields.push_back(bad.fields.front()); break;
    case 3: bad.fields[0].target_field = bad.fields[1].target_field; break;
    case 4: bad.application_version.clear(); break;
    case 5: bad.fields.clear(); break;
    }
    rejects([&] { (void)appraisal_mapping_json(bad); });
  }
  auto malformed = descriptor_json;
  malformed.insert(1, "\"timeout_ms\":1,");
  rejects([&] { (void)parse_appraisal_mapping_json(malformed); });
  rejects([&] { (void)parse_appraisal_mapping_json(std::string(1024 * 1024 + 1, ' ')); });
}
}
int main() {
  try { run(); } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
  return 0;
}
