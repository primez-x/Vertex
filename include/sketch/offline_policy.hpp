#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace sketch {

// Unknown declarations are not evidence that a runtime is independent.
enum class RuntimeRequirement { unknown, not_required, required };
enum class LocalResourceStatus { unknown, available, unavailable };
enum class OfflineDiagnosticSeverity { warning, error };

struct OfflinePolicy {
    bool require_no_account = true;
    bool require_no_activation = true;
    bool require_no_subscription = true;
    bool require_no_network = true;
};

struct OptionalLocalResource {
    // Stable identifier only; do not put paths, credentials or user data here.
    std::string id;
    LocalResourceStatus status = LocalResourceStatus::unknown;
};

struct RuntimeCapabilities {
    RuntimeRequirement account = RuntimeRequirement::unknown;
    RuntimeRequirement activation = RuntimeRequirement::unknown;
    RuntimeRequirement subscription = RuntimeRequirement::unknown;
    RuntimeRequirement network = RuntimeRequirement::unknown;
    std::vector<OptionalLocalResource> optional_local_resources;
};

struct OfflineDiagnostic {
    OfflineDiagnosticSeverity severity;
    std::string code;
    std::string capability;
    std::string message;
};

struct OfflinePolicyReport {
    bool startup_allowed = false;
    std::vector<OfflineDiagnostic> diagnostics;
};

// Pure evaluation: never contacts services, reads files or changes runtime state.
[[nodiscard]] OfflinePolicyReport evaluate_offline_policy(
    const RuntimeCapabilities& capabilities, const OfflinePolicy& policy = {});
// Deterministic diagnostic envelope, including the evaluated declarations.
[[nodiscard]] nlohmann::json offline_policy_json(
    const RuntimeCapabilities& capabilities, const OfflinePolicy& policy = {});

} // namespace sketch
