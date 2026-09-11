#include "sketch/integration_adapter.hpp"
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
using namespace sketch;
template<class Error, class F> void rejected(F operation) {
  try { operation(); } catch (const Error &) { return; }
  throw std::runtime_error("expected adapter rejection");
}
IntegrationAdapterDefinition fixture() {
  return {"test.exchange", IntegrationDomain::apex_exchange, {1, 0}, "test.format", {2, 3},
          {IntegrationOperation::import_data}, "Synthetic test fixture", "MIT"};
}
void check() {
  IntegrationAdapterRegistry registry;
  IntegrationAdapterRequest request{IntegrationDomain::apex_exchange,
      IntegrationOperation::import_data, "test.format", {2, 2}, {1, 0}, {}};
  rejected<std::runtime_error>([&] { (void)registry.resolve(request); });
  auto definition = fixture();
  registry.register_adapter(definition);
  definition.provenance = "Changed caller copy";
  auto retained = registry.resolve(request);
  if (retained->definition().provenance != "Synthetic test fixture")
    throw std::runtime_error("descriptor aliases caller metadata");
  rejected<std::invalid_argument>([&] { registry.register_adapter(fixture()); });
  for (int fault = 0; fault < 9; ++fault) {
    auto bad = fixture();
    bad.id = "bad";
    switch (fault) {
    case 0: bad.id = " "; break;
    case 1: bad.format_id.clear(); break;
    case 2: bad.api_version = {1, 1}; break;
    case 3: bad.format_version.major = 0; break;
    case 4: bad.license = "\n"; break;
    case 5: bad.provenance.clear(); break;
    case 6: bad.operations.clear(); break;
    case 7: bad.operations.push_back(bad.operations.front()); break;
    case 8: bad.domain = static_cast<IntegrationDomain>(999); break;
    }
    rejected<std::invalid_argument>([&] { registry.register_adapter(bad); });
  }
  auto unavailable = request;
  unavailable.minimum_format_version = {2, 4};
  rejected<std::runtime_error>([&] { (void)registry.resolve(unavailable); });
  unavailable.minimum_format_version = {1, 0};
  rejected<std::runtime_error>([&] { (void)registry.resolve(unavailable); });
  unavailable = request; unavailable.minimum_api_version = {1, 1};
  rejected<std::invalid_argument>([&] { (void)registry.resolve(unavailable); });
  unavailable = request; unavailable.adapter_id = "unknown";
  rejected<std::runtime_error>([&] { (void)registry.resolve(unavailable); });
  unavailable = request; unavailable.operation = IntegrationOperation::export_data;
  rejected<std::runtime_error>([&] { (void)registry.resolve(unavailable); });
  auto second = fixture(); second.id = "second";
  registry.register_adapter(second);
  rejected<std::runtime_error>([&] { (void)registry.resolve(request); });
  request.adapter_id = "test.exchange";
  if (registry.resolve(request) != retained) throw std::runtime_error("explicit selection failed");
  bool resolve_rejected = false, register_rejected = false;
  std::thread other([&] {
    try { (void)registry.resolve(request); } catch (const std::logic_error &) { resolve_rejected = true; }
    try { registry.register_adapter(fixture()); } catch (const std::logic_error &) { register_rejected = true; }
  });
  other.join();
  if (!resolve_rejected || !register_rejected) throw std::runtime_error("owner thread not enforced");
  for (auto domain : {IntegrationDomain::cad_exchange, IntegrationDomain::devices,
                     IntegrationDomain::georeferencing, IntegrationDomain::appraisal}) {
    IntegrationAdapterRegistry independent;
    auto d = fixture(); d.domain = domain;
    independent.register_adapter(d);
    auto r = request; r.domain = domain;
    (void)independent.resolve(r);
  }
}
}
int main() {
  try { check(); } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
  return 0;
}
