#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace sketch {

enum class IntegrationDomain { apex_exchange, cad_exchange, devices, georeferencing, appraisal };
enum class IntegrationOperation { import_data, export_data, read_device, transform_coordinates, evaluate };

struct IntegrationVersion {
  std::uint32_t major{1};
  std::uint32_t minor{0};
  bool operator==(const IntegrationVersion &) const = default;
};

struct IntegrationAdapterDefinition {
  std::string id;
  IntegrationDomain domain{IntegrationDomain::apex_exchange};
  IntegrationVersion api_version;
  std::string format_id;
  IntegrationVersion format_version;
  std::vector<IntegrationOperation> operations;
  std::string provenance;
  std::string license;
};

// A validated, owned snapshot; callers cannot mutate registered metadata.
class IntegrationAdapterDescriptor final {
public:
  explicit IntegrationAdapterDescriptor(IntegrationAdapterDefinition definition);
  [[nodiscard]] const IntegrationAdapterDefinition &definition() const noexcept;

private:
  const IntegrationAdapterDefinition definition_;
};

struct IntegrationAdapterRequest {
  IntegrationDomain domain{IntegrationDomain::apex_exchange};
  IntegrationOperation operation{IntegrationOperation::import_data};
  std::string format_id;
  IntegrationVersion minimum_format_version;
  IntegrationVersion minimum_api_version;
  // Empty selects by capability; multiple matches are always an error.
  std::string adapter_id;
};

// Owner-thread confined. Cross-thread calls throw before accessing registry data.
// Descriptors returned by value-owned shared pointers may be read on any thread.
class IntegrationAdapterRegistry final {
public:
  IntegrationAdapterRegistry();
  IntegrationAdapterRegistry(const IntegrationAdapterRegistry &) = delete;
  IntegrationAdapterRegistry &operator=(const IntegrationAdapterRegistry &) = delete;
  void register_adapter(IntegrationAdapterDefinition definition);
  [[nodiscard]] std::shared_ptr<const IntegrationAdapterDescriptor>
  resolve(const IntegrationAdapterRequest &request) const;

private:
  void require_owner() const;
  const std::thread::id owner_;
  std::vector<std::shared_ptr<const IntegrationAdapterDescriptor>> adapters_;
};

} // namespace sketch
