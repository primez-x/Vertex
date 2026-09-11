#pragma once

#include <nlohmann/json.hpp>
#include <cstdint>
#include <string>
#include <vector>

namespace sketch {

// Portable contract only. The Windows broker must establish and attest every
// control before resuming a worker; configuration alone is not attestation.
struct ImportWorkerPolicy {
    bool app_container{true};
    bool network_capabilities{false};
    bool brokered_input_only{true};
    bool kill_job_on_parent_exit{true};
    bool fixed_module_search{true};
    bool proj_network{false};
    bool proj_custom_network_callbacks{false};
    std::uint64_t max_input_bytes{64ULL * 1024 * 1024};
    std::uint64_t max_expanded_bytes{256ULL * 1024 * 1024};
    std::uint64_t max_expansion_ratio{100};
    std::uint64_t timeout_ms{30'000};
    std::uint64_t job_memory_bytes{512ULL * 1024 * 1024};
    std::uint32_t max_active_processes{1};
    std::string temporary_root;
    std::vector<std::string> module_search_roots;
};

struct ImportWorkerAttestation {
    bool restricted_token_verified{};
    bool network_denial_verified{};
    bool job_limits_verified{};
    bool parent_exit_kills_job_verified{};
    bool brokered_handles_verified{};
    bool private_temporary_root_verified{};
    bool immutable_module_roots_verified{};
    bool fixed_search_applied{};
    bool proj_offline_applied{};
};

struct ImportWorkerInput {
    // Archive member, never an absolute host path. Broker maps it under temp.
    std::string relative_name;
    std::uint64_t input_bytes{};
    std::uint64_t expanded_bytes{};
    std::uint64_t elapsed_ms{};
    std::uint32_t child_processes{};
    bool malformed{};
    bool crashed{};
    std::vector<std::string> required_proj_resources;
    std::vector<std::string> bundled_proj_resources;
};

struct ImportWorkerDecision {
    bool allowed{};
    bool terminate_worker{true};
    // Stable local codes; reports never include untrusted paths or payloads.
    std::vector<std::string> diagnostics;
    [[nodiscard]] nlohmann::json to_json() const;
};

[[nodiscard]] ImportWorkerDecision evaluate_import_worker(
    const ImportWorkerPolicy&, const ImportWorkerAttestation&, const ImportWorkerInput&);

} // namespace sketch
