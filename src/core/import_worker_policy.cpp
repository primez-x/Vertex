#include "sketch/import_worker_policy.hpp"

#include <algorithm>
#include <string_view>

namespace sketch {
namespace {
bool safe_relative(std::string_view path) {
    if (path.empty() || path.size() > 240 || path.front() == '/' || path.front() == '\\') return false;
    std::size_t start = 0;
    while (start < path.size()) {
        const auto end = path.find_first_of("/\\", start);
        const auto part = path.substr(start, end == path.npos ? path.size() - start : end - start);
        if (part.empty() || part == "." || part == ".." || part.back() == '.' || part.back() == ' ') return false;
        for (const unsigned char c : part)
            if (c < 32 || c > 126 || std::string_view(":*?\"<>|").find(static_cast<char>(c)) != std::string_view::npos) return false;
        std::string base(part.substr(0, part.find('.')));
        std::transform(base.begin(), base.end(), base.begin(), [](unsigned char c) {
            return static_cast<char>(c >= 'a' && c <= 'z' ? c - ('a' - 'A') : c);
        });
        if (base == "CON" || base == "PRN" || base == "AUX" || base == "NUL" ||
            (base.size() == 4 && (base.starts_with("COM") || base.starts_with("LPT")) && base[3] >= '1' && base[3] <= '9')) return false;
        if (end == path.npos) return true;
        start = end + 1;
    }
    return false;
}

bool safe_absolute(std::string_view path) {
    return path.size() > 3 && ((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')) &&
        path[1] == ':' && (path[2] == '\\' || path[2] == '/') && safe_relative(path.substr(3));
}
} // namespace

nlohmann::json ImportWorkerDecision::to_json() const {
    return {{"allowed", allowed}, {"diagnostics", diagnostics}, {"network_requests_permitted", false},
            {"schema_version", 1}, {"terminate_worker", terminate_worker}};
}

ImportWorkerDecision evaluate_import_worker(const ImportWorkerPolicy& p,
    const ImportWorkerAttestation& a, const ImportWorkerInput& i) {
    ImportWorkerDecision result;
    const auto reject = [&](const char* code) { result.diagnostics.emplace_back(code); };
    if (!p.app_container || p.network_capabilities || !p.brokered_input_only || !p.kill_job_on_parent_exit ||
        !p.fixed_module_search || p.proj_network || p.proj_custom_network_callbacks || p.max_active_processes != 1)
        reject("unsafe_worker_policy");
    if (!p.max_input_bytes || !p.max_expanded_bytes || !p.max_expansion_ratio || !p.timeout_ms ||
        !p.job_memory_bytes || p.max_expanded_bytes > p.job_memory_bytes)
        reject("invalid_resource_limits");
    if (!safe_absolute(p.temporary_root) || p.module_search_roots.empty() ||
        std::any_of(p.module_search_roots.begin(), p.module_search_roots.end(),
                    [](const auto& path) { return !safe_absolute(path); })) reject("invalid_worker_roots");
    if (!a.restricted_token_verified || !a.network_denial_verified || !a.job_limits_verified ||
        !a.parent_exit_kills_job_verified || !a.brokered_handles_verified || !a.private_temporary_root_verified ||
        !a.immutable_module_roots_verified || !a.fixed_search_applied || !a.proj_offline_applied)
        reject("sandbox_not_attested");
    if (!safe_relative(i.relative_name)) reject("unsafe_input_path");
    if (i.input_bytes > p.max_input_bytes) reject("input_size_limit");
    if (i.expanded_bytes > p.max_expanded_bytes) reject("expanded_size_limit");
    // Division avoids overflow even for hostile UINT64_MAX counters.
    if ((!i.input_bytes && i.expanded_bytes) || (i.input_bytes &&
        (i.expanded_bytes / i.input_bytes > p.max_expansion_ratio ||
         (i.expanded_bytes / i.input_bytes == p.max_expansion_ratio && i.expanded_bytes % i.input_bytes))))
        reject("expansion_ratio_limit");
    if (i.elapsed_ms >= p.timeout_ms) reject("worker_timeout");
    if (i.child_processes) reject("child_process_denied");
    if (i.malformed) reject("malformed_import");
    if (i.crashed) reject("worker_crashed");
    for (const auto& resource : i.bundled_proj_resources)
        if (!safe_relative(resource)) reject("invalid_bundled_proj_resource");
    for (const auto& resource : i.required_proj_resources) {
        if (!safe_relative(resource)) reject("unsafe_proj_resource");
        else if (std::find(i.bundled_proj_resources.begin(), i.bundled_proj_resources.end(), resource) == i.bundled_proj_resources.end())
            reject("missing_local_proj_resource");
    }
    std::sort(result.diagnostics.begin(), result.diagnostics.end());
    result.diagnostics.erase(std::unique(result.diagnostics.begin(), result.diagnostics.end()), result.diagnostics.end());
    result.allowed = result.diagnostics.empty();
    result.terminate_worker = !result.allowed;
    return result;
}
} // namespace sketch
