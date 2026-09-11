#include "sketch/offline_policy.hpp"

#include <algorithm>
#include <array>
#include <set>
#include <string_view>
#include <utility>

namespace sketch {
namespace {
const char* name(RuntimeRequirement value) {
    switch (value) {
    case RuntimeRequirement::unknown: return "unknown";
    case RuntimeRequirement::not_required: return "not_required";
    case RuntimeRequirement::required: return "required";
    }
    return "invalid";
}
const char* name(LocalResourceStatus value) {
    switch (value) {
    case LocalResourceStatus::unknown: return "unknown";
    case LocalResourceStatus::available: return "available";
    case LocalResourceStatus::unavailable: return "unavailable";
    }
    return "invalid";
}
bool valid_id(std::string_view id) {
    return !id.empty() && id.size() <= 64 && std::all_of(id.begin(), id.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
    });
}
}

OfflinePolicyReport evaluate_offline_policy(const RuntimeCapabilities& capabilities,
                                          const OfflinePolicy& policy) {
    OfflinePolicyReport report;
    auto emit = [&](OfflineDiagnosticSeverity severity, std::string code,
                    std::string capability, std::string message) {
        report.diagnostics.push_back({severity, std::move(code), std::move(capability), std::move(message)});
    };
    struct Dependency { const char* id; bool enforced; RuntimeRequirement requirement; };
    const std::array dependencies{
        Dependency{"account", policy.require_no_account, capabilities.account},
        Dependency{"activation", policy.require_no_activation, capabilities.activation},
        Dependency{"subscription", policy.require_no_subscription, capabilities.subscription},
        Dependency{"network", policy.require_no_network, capabilities.network}};
    for (const auto& dependency : dependencies) {
        if (!dependency.enforced)
            emit(OfflineDiagnosticSeverity::error, "policy_weakened", dependency.id,
                 "Offline independence must be required.");
        switch (dependency.requirement) {
        case RuntimeRequirement::not_required: break;
        case RuntimeRequirement::required:
            emit(OfflineDiagnosticSeverity::error, "external_dependency_required", dependency.id,
                 "Startup requires an external dependency.");
            break;
        case RuntimeRequirement::unknown:
            emit(OfflineDiagnosticSeverity::error, "requirement_unknown", dependency.id,
                 "Runtime independence has not been declared.");
            break;
        default:
            emit(OfflineDiagnosticSeverity::error, "requirement_invalid", dependency.id,
                 "Runtime requirement is invalid.");
        }
    }
    std::set<std::string> ids;
    for (const auto& resource : capabilities.optional_local_resources) {
        if (!valid_id(resource.id)) {
            emit(OfflineDiagnosticSeverity::error, "resource_id_invalid", "optional_local_resource",
                 "Resource identifiers must contain 1 to 64 lowercase letters, digits, underscores or hyphens.");
            continue;
        }
        if (!ids.insert(resource.id).second)
            emit(OfflineDiagnosticSeverity::error, "resource_id_duplicate", resource.id,
                 "Optional resource identifiers must be unique.");
        switch (resource.status) {
        case LocalResourceStatus::available: break;
        case LocalResourceStatus::unavailable:
            emit(OfflineDiagnosticSeverity::warning, "optional_resource_unavailable", resource.id,
                 "Optional local resource is unavailable; its feature can remain disabled.");
            break;
        case LocalResourceStatus::unknown:
            emit(OfflineDiagnosticSeverity::warning, "optional_resource_unknown", resource.id,
                 "Optional local resource has not been checked; its feature must remain disabled.");
            break;
        default:
            emit(OfflineDiagnosticSeverity::error, "resource_status_invalid", resource.id,
                 "Optional resource status is invalid.");
        }
    }
    report.startup_allowed = std::none_of(report.diagnostics.begin(), report.diagnostics.end(),
        [](const auto& diagnostic) { return diagnostic.severity == OfflineDiagnosticSeverity::error; });
    return report;
}

nlohmann::json offline_policy_json(const RuntimeCapabilities& capabilities, const OfflinePolicy& policy) {
    const auto report = evaluate_offline_policy(capabilities, policy);
    auto diagnostics = nlohmann::json::array();
    for (const auto& diagnostic : report.diagnostics)
        diagnostics.push_back({{"severity", diagnostic.severity == OfflineDiagnosticSeverity::error ? "error" : "warning"},
            {"code", diagnostic.code}, {"capability", diagnostic.capability}, {"message", diagnostic.message}});
    auto resources = nlohmann::json::array();
    for (const auto& resource : capabilities.optional_local_resources)
        resources.push_back({{"id", valid_id(resource.id) ? resource.id : "invalid"}, {"status", name(resource.status)}});
    return {{"schema_version", 1}, {"startup_allowed", report.startup_allowed},
        {"policy", {{"require_no_account", policy.require_no_account}, {"require_no_activation", policy.require_no_activation},
                    {"require_no_subscription", policy.require_no_subscription}, {"require_no_network", policy.require_no_network}}},
        {"requirements", {{"account", name(capabilities.account)}, {"activation", name(capabilities.activation)},
                          {"subscription", name(capabilities.subscription)}, {"network", name(capabilities.network)}}},
        {"optional_local_resources", std::move(resources)}, {"diagnostics", std::move(diagnostics)}};
}
} // namespace sketch
