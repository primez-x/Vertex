#pragma once

#include <nlohmann/json.hpp>
#include <cstdint>
#include <string>
#include <vector>

namespace sketch {

enum class InterchangeFormat { ifc, dxf, pdf, proj };
enum class UnsupportedEntityPolicy { reject, preserve_reference_and_report };

// A declaration is not registration of a parser or proof of runtime support.
struct InterchangeProfile {
    InterchangeFormat format{InterchangeFormat::ifc};
    std::string adapter_id;
    std::string adapter_version;
    std::string format_target;
    std::vector<std::string> capabilities;
    std::vector<std::string> module_allowlist;
    std::vector<std::string> required_resources;
    std::string license_expression;
    std::string license_review_id;
    UnsupportedEntityPolicy unsupported_entities{UnsupportedEntityPolicy::preserve_reference_and_report};
    std::uint64_t max_input_bytes{64ULL * 1024 * 1024};
    std::uint64_t max_output_bytes{256ULL * 1024 * 1024};
    std::uint64_t timeout_ms{30'000};
    [[nodiscard]] nlohmann::json to_json() const;
};

struct InterchangeProfileAttestation {
    std::string adapter_id;
    std::string adapter_version;
    std::string license_review_id;
    std::vector<std::string> loaded_modules;
    std::vector<std::string> verified_local_resources;
    bool license_review_verified{};
    bool offline_build_verified{};
    bool network_denial_verified{};
    bool isolated_worker_verified{};
    bool limits_enforced{};
    bool failure_preserves_project_verified{};
    bool proj_network_disabled{};
    bool proj_network_callbacks_disabled{};
};

struct InterchangeProfileDecision {
    bool allowed{};
    std::vector<std::string> diagnostics;
    [[nodiscard]] nlohmann::json to_json() const;
};

// Baseline subset with no component/version/license attested. Callers must fill
// reviewed metadata and verify the real worker before readiness is allowed.
[[nodiscard]] InterchangeProfile declared_interchange_profile(InterchangeFormat);
[[nodiscard]] InterchangeProfileDecision validate_interchange_profile(
    const InterchangeProfile&, const InterchangeProfileAttestation&);

} // namespace sketch
