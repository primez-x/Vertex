#pragma once

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace sketch {

// The broker accepts an executable that is already part of the locally
// installed worker package. It never accepts a source/project handle or a
// command line assembled from imported document text.
struct WindowsImportWorkerOptions {
    std::filesystem::path executable;
    std::vector<std::wstring> arguments;
    std::filesystem::path temporary_root;
    std::vector<std::filesystem::path> immutable_module_roots;
    std::vector<std::byte> input;
    std::uint64_t timeout_ms{30'000};
    std::uint64_t memory_bytes{512ULL * 1024 * 1024};
    std::uint32_t max_active_processes{1};
    std::uint64_t max_output_bytes{256ULL * 1024 * 1024};
    bool proj_offline_required{true};
};

enum class WindowsImportWorkerStatus {
    completed,
    invalid_request,
    unsupported,
    launch_failed,
    timed_out,
    failed,
};

struct WindowsImportWorkerReport {
    WindowsImportWorkerStatus status{WindowsImportWorkerStatus::failed};
    bool launched{};
    bool completed{};
    bool timed_out{};
    bool app_container_verified{};
    bool restricted_token_verified{};
    bool network_denial_verified{};
    bool job_limits_verified{};
    bool parent_exit_kill_verified{};
    bool brokered_handles_verified{};
    bool private_temporary_root_verified{};
    bool immutable_module_roots_verified{};
    bool fixed_search_applied{};
    bool proj_offline_applied{};
    std::uint32_t process_id{};
    std::uint32_t exit_code{};
    // Win32 error captured immediately when CreateProcessW fails; zero otherwise.
    std::uint32_t launch_error{};
    std::vector<std::byte> output;
    // Codes are stable and deliberately exclude paths, command lines and
    // worker-provided text.
    std::vector<std::string> diagnostics;

    [[nodiscard]] bool controls_attested() const noexcept;
    [[nodiscard]] nlohmann::json to_json() const;
};

// Launch and supervise one local import worker. The call is synchronous so a
// caller can publish output only after this report says completed. On
// non-Windows hosts it always returns unsupported; the product target is
// Windows and therefore does not silently fall back to an unsandboxed launch.
[[nodiscard]] WindowsImportWorkerReport run_windows_import_worker(
    const WindowsImportWorkerOptions& options);

} // namespace sketch
