#include "sketch/integration_adapter.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace sketch {
namespace {
bool valid(IntegrationDomain domain) {
  switch (domain) {
  case IntegrationDomain::apex_exchange: case IntegrationDomain::cad_exchange:
  case IntegrationDomain::devices: case IntegrationDomain::georeferencing:
  case IntegrationDomain::appraisal: return true;
  }
  return false;
}
bool valid(IntegrationOperation operation) {
  switch (operation) {
  case IntegrationOperation::import_data: case IntegrationOperation::export_data:
  case IntegrationOperation::read_device: case IntegrationOperation::transform_coordinates:
  case IntegrationOperation::evaluate: return true;
  }
  return false;
}
bool identifier(const std::string &value) {
  return !value.empty() && std::all_of(value.begin(), value.end(), [](unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_';
  });
}
bool text(const std::string &value) {
  return !value.empty() && std::any_of(value.begin(), value.end(), [](unsigned char c) { return c > 32; }) &&
         std::none_of(value.begin(), value.end(), [](unsigned char c) { return c < 32 || c == 127; });
}
bool supports(IntegrationVersion offered, IntegrationVersion minimum) {
  return offered.major == minimum.major && offered.minor >= minimum.minor;
}
}

IntegrationAdapterDescriptor::IntegrationAdapterDescriptor(IntegrationAdapterDefinition definition)
    : definition_(std::move(definition)) {
  const auto &d = definition_;
  if (!identifier(d.id) || !identifier(d.format_id) || !valid(d.domain) ||
      d.api_version.major != 1 || d.api_version.minor != 0 || d.format_version.major == 0 ||
      !text(d.provenance) || !text(d.license) || d.operations.empty())
    throw std::invalid_argument("malformed or unsupported integration adapter descriptor");
  for (std::size_t i = 0; i < d.operations.size(); ++i) {
    if (!valid(d.operations[i]) || std::find(d.operations.begin(), d.operations.begin() + i,
        d.operations[i]) != d.operations.begin() + i)
      throw std::invalid_argument("invalid or duplicate integration operation");
  }
}

const IntegrationAdapterDefinition &IntegrationAdapterDescriptor::definition() const noexcept {
  return definition_;
}

IntegrationAdapterRegistry::IntegrationAdapterRegistry() : owner_(std::this_thread::get_id()) {}

void IntegrationAdapterRegistry::require_owner() const {
  if (std::this_thread::get_id() != owner_)
    throw std::logic_error("integration registry requires its owner thread");
}

void IntegrationAdapterRegistry::register_adapter(IntegrationAdapterDefinition definition) {
  require_owner();
  auto descriptor = std::make_shared<const IntegrationAdapterDescriptor>(std::move(definition));
  for (const auto &existing : adapters_)
    if (existing->definition().id == descriptor->definition().id)
      throw std::invalid_argument("duplicate integration adapter id");
  adapters_.push_back(std::move(descriptor));
}

std::shared_ptr<const IntegrationAdapterDescriptor>
IntegrationAdapterRegistry::resolve(const IntegrationAdapterRequest &request) const {
  require_owner();
  if (!valid(request.domain) || !valid(request.operation) || !identifier(request.format_id) ||
      (!request.adapter_id.empty() && !identifier(request.adapter_id)) ||
      request.minimum_api_version.major != 1 || request.minimum_api_version.minor != 0 ||
      request.minimum_format_version.major == 0)
    throw std::invalid_argument("malformed or unsupported integration request");
  std::shared_ptr<const IntegrationAdapterDescriptor> result;
  for (const auto &adapter : adapters_) {
    const auto &d = adapter->definition();
    if ((!request.adapter_id.empty() && request.adapter_id != d.id) || d.domain != request.domain ||
        d.format_id != request.format_id || !supports(d.api_version, request.minimum_api_version) ||
        !supports(d.format_version, request.minimum_format_version) ||
        std::find(d.operations.begin(), d.operations.end(), request.operation) == d.operations.end())
      continue;
    if (result) throw std::runtime_error("ambiguous integration adapter request");
    result = adapter;
  }
  if (!result) throw std::runtime_error("no supported integration adapter");
  return result;
}

} // namespace sketch
