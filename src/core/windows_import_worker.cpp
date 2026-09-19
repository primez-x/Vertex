#include "sketch/windows_import_worker.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cwctype>
#include <cwchar>
#include <thread>
#include <utility>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <Aclapi.h>
#include <Sddl.h>
#include <UserEnv.h>
#endif

namespace sketch {
namespace {

const char* status_name(WindowsImportWorkerStatus status) noexcept {
    switch (status) {
    case WindowsImportWorkerStatus::completed: return "completed";
    case WindowsImportWorkerStatus::invalid_request: return "invalid_request";
    case WindowsImportWorkerStatus::unsupported: return "unsupported";
    case WindowsImportWorkerStatus::launch_failed: return "launch_failed";
    case WindowsImportWorkerStatus::timed_out: return "timed_out";
    case WindowsImportWorkerStatus::failed: return "failed";
    }
    return "failed";
}

void sort_diagnostics(WindowsImportWorkerReport& report) {
    std::sort(report.diagnostics.begin(), report.diagnostics.end());
    report.diagnostics.erase(std::unique(report.diagnostics.begin(), report.diagnostics.end()),
                             report.diagnostics.end());
}

void diagnostic(WindowsImportWorkerReport& report, const char* code) {
    report.diagnostics.emplace_back(code);
}

} // namespace

bool WindowsImportWorkerReport::controls_attested() const noexcept {
    return status == WindowsImportWorkerStatus::completed && completed &&
        app_container_verified && restricted_token_verified && network_denial_verified &&
        job_limits_verified && parent_exit_kill_verified && brokered_handles_verified &&
        private_temporary_root_verified && immutable_module_roots_verified && fixed_search_applied &&
        proj_offline_applied;
}

nlohmann::json WindowsImportWorkerReport::to_json() const {
    auto result = nlohmann::json{
        {"schema_version", 1},
        {"status", status_name(status)},
        {"launched", launched},
        {"completed", completed},
        {"timed_out", timed_out},
        {"controls_attested", controls_attested()},
        {"app_container_verified", app_container_verified},
        {"restricted_token_verified", restricted_token_verified},
        {"network_denial_verified", network_denial_verified},
        {"job_limits_verified", job_limits_verified},
        {"parent_exit_kill_verified", parent_exit_kill_verified},
        {"brokered_handles_verified", brokered_handles_verified},
        {"private_temporary_root_verified", private_temporary_root_verified},
        {"immutable_module_roots_verified", immutable_module_roots_verified},
        {"fixed_search_applied", fixed_search_applied},
        {"proj_offline_applied", proj_offline_applied},
        {"process_id", process_id},
        {"exit_code", exit_code},
        {"launch_error", launch_error},
        {"output_bytes", output.size()},
        {"diagnostics", diagnostics},
        {"network_requests_permitted", false},
    };
    return result;
}

#ifdef _WIN32
namespace {

using Clock = std::chrono::steady_clock;

class OwnedHandle final {
public:
    explicit OwnedHandle(HANDLE handle = nullptr) noexcept : handle_(handle) {}
    ~OwnedHandle() { close(); }
    OwnedHandle(const OwnedHandle&) = delete;
    OwnedHandle& operator=(const OwnedHandle&) = delete;
    OwnedHandle(OwnedHandle&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}
    OwnedHandle& operator=(OwnedHandle&& other) noexcept {
        if (this != &other) {
            close();
            handle_ = std::exchange(other.handle_, nullptr);
        }
        return *this;
    }
    [[nodiscard]] HANDLE get() const noexcept { return handle_; }
    [[nodiscard]] bool valid() const noexcept { return handle_ && handle_ != INVALID_HANDLE_VALUE; }
    HANDLE release() noexcept { return std::exchange(handle_, nullptr); }
    void close() noexcept {
        if (valid()) CloseHandle(handle_);
        handle_ = nullptr;
    }
private:
    HANDLE handle_{};
};

class LocalBuffer final {
public:
    LocalBuffer() = default;
    explicit LocalBuffer(HLOCAL value) noexcept : value_(value) {}
    ~LocalBuffer() { if (value_) LocalFree(value_); }
    LocalBuffer(const LocalBuffer&) = delete;
    LocalBuffer& operator=(const LocalBuffer&) = delete;
    LocalBuffer(LocalBuffer&& other) noexcept : value_(std::exchange(other.value_, nullptr)) {}
    LocalBuffer& operator=(LocalBuffer&& other) noexcept {
        if (this != &other) {
            if (value_) LocalFree(value_);
            value_ = std::exchange(other.value_, nullptr);
        }
        return *this;
    }
    [[nodiscard]] HLOCAL get() const noexcept { return value_; }
    HLOCAL release() noexcept { return std::exchange(value_, nullptr); }
private:
    HLOCAL value_{};
};

class SidBuffer final {
public:
    SidBuffer() = default;
    explicit SidBuffer(PSID sid, bool local = false) noexcept : sid_(sid), local_(local) {}
    ~SidBuffer() { reset(); }
    SidBuffer(const SidBuffer&) = delete;
    SidBuffer& operator=(const SidBuffer&) = delete;
    SidBuffer(SidBuffer&& other) noexcept
        : sid_(std::exchange(other.sid_, nullptr)), local_(std::exchange(other.local_, false)) {}
    SidBuffer& operator=(SidBuffer&& other) noexcept {
        if (this != &other) {
            reset();
            sid_ = std::exchange(other.sid_, nullptr);
            local_ = std::exchange(other.local_, false);
        }
        return *this;
    }
    [[nodiscard]] PSID get() const noexcept { return sid_; }
    [[nodiscard]] bool valid() const noexcept { return sid_ && IsValidSid(sid_); }
    void reset() noexcept {
        if (!sid_) return;
        if (local_) LocalFree(sid_);
        else FreeSid(sid_);
        sid_ = nullptr;
        local_ = false;
    }
private:
    PSID sid_{};
    bool local_{};
};

bool has_parent_component(const std::filesystem::path& path) {
    for (const auto& component : path) {
        if (component == L"." || component == L"..") return true;
    }
    return false;
}

bool local_absolute_path(const std::filesystem::path& path) {
    if (path.empty() || !path.is_absolute() || path.root_name().empty() || path.root_directory().empty()) return false;
    const auto root = path.root_name().wstring();
    if (root.size() >= 2 && root[0] == L'\\' && root[1] == L'\\') return false;
    return !has_parent_component(path);
}

bool ordinary_object(const std::filesystem::path& path, bool directory) {
    const auto attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
    return ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) == directory;
}

std::filesystem::path normalized_existing(const std::filesystem::path& path) {
    std::error_code error;
    const auto value = std::filesystem::canonical(path, error);
    return error ? path.lexically_normal() : value;
}

bool same_or_below(const std::filesystem::path& parent, const std::filesystem::path& child) {
    const auto parent_value = normalized_existing(parent);
    const auto child_value = normalized_existing(child);
    if (parent_value == child_value) return true;
    std::error_code error;
    const auto relative = std::filesystem::relative(child_value, parent_value, error);
    if (!error && !relative.empty()) {
        for (const auto& component : relative) if (component == L"..") return false;
        return true;
    }
    // Windows paths are case-insensitive. The standard library can report a
    // relative-path error when one spelling traverses a protected directory,
    // so retain a lexical, case-folded containment check as a fallback.
    auto fold = [](std::wstring value) {
        std::transform(value.begin(), value.end(), value.begin(), [](wchar_t c) {
            return static_cast<wchar_t>(std::towlower(c == L'/' ? L'\\' : c));
        });
        return value;
    };
    auto prefix = fold(parent_value.lexically_normal().wstring());
    const auto value = fold(child_value.lexically_normal().wstring());
    if (!prefix.ends_with(L'\\')) prefix.push_back(L'\\');
    return value.starts_with(prefix);
}

bool root_is_immutable(const std::filesystem::path& root) {
    // A root that grants this process write access cannot be an immutable
    // worker/module root. This check is advisory for ACLs inherited by files;
    // the packaged release still has to ship the roots read-only.
    const auto handle = CreateFileW(root.c_str(), FILE_ADD_FILE | FILE_ADD_SUBDIRECTORY | DELETE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const auto error = GetLastError();
        return error == ERROR_ACCESS_DENIED || error == ERROR_SHARING_VIOLATION || error == ERROR_WRITE_PROTECT;
    }
    CloseHandle(handle);
    return false;
}

bool verify_module_roots(const WindowsImportWorkerOptions& options, WindowsImportWorkerReport& report) {
    if (options.immutable_module_roots.empty()) {
        diagnostic(report, "invalid_module_roots");
        return false;
    }
    std::vector<std::filesystem::path> roots;
    roots.reserve(options.immutable_module_roots.size());
    for (const auto& root : options.immutable_module_roots) {
        if (!local_absolute_path(root) || !ordinary_object(root, true) || !root_is_immutable(root)) {
            diagnostic(report, "invalid_module_roots");
            return false;
        }
        roots.emplace_back(normalized_existing(root));
    }
    for (std::size_t i = 0; i < roots.size(); ++i) {
        for (std::size_t j = i + 1; j < roots.size(); ++j) {
            if (same_or_below(roots[i], roots[j]) || same_or_below(roots[j], roots[i])) {
                diagnostic(report, "overlapping_module_roots");
                return false;
            }
        }
    }
    const auto executable_parent = normalized_existing(options.executable.parent_path());
    if (std::none_of(roots.begin(), roots.end(), [&](const auto& root) {
            return same_or_below(root, executable_parent);
        })) {
        diagnostic(report, "executable_outside_module_roots");
        return false;
    }
    report.immutable_module_roots_verified = true;
    return true;
}

bool valid_argument(const std::wstring& value) {
    return value.size() <= 4096 && value.find(L'\0') == std::wstring::npos;
}

std::wstring quote_argument(const std::wstring& value) {
    if (!value.empty() && value.find_first_of(L" \t\"") == std::wstring::npos) return value;
    std::wstring result;
    result.push_back(L'"');
    std::size_t backslashes = 0;
    for (const auto character : value) {
        if (character == L'\\') {
            ++backslashes;
            continue;
        }
        if (character == L'"') {
            result.append(backslashes * 2 + 1, L'\\');
            result.push_back(L'"');
            backslashes = 0;
            continue;
        }
        result.append(backslashes, L'\\');
        backslashes = 0;
        result.push_back(character);
    }
    result.append(backslashes * 2, L'\\');
    result.push_back(L'"');
    return result;
}

bool build_command_line(const WindowsImportWorkerOptions& options, std::vector<wchar_t>& command,
                        WindowsImportWorkerReport& report) {
    if (options.arguments.size() > 256) {
        diagnostic(report, "invalid_worker_arguments");
        return false;
    }
    std::wstring value = quote_argument(options.executable.wstring());
    for (const auto& argument : options.arguments) {
        if (!valid_argument(argument)) {
            diagnostic(report, "invalid_worker_arguments");
            return false;
        }
        value.push_back(L' ');
        value += quote_argument(argument);
    }
    if (value.empty() || value.size() >= 32767) {
        diagnostic(report, "worker_command_line_limit");
        return false;
    }
    command.assign(value.begin(), value.end());
    command.push_back(L'\0');
    return true;
}

bool environment_block(const WindowsImportWorkerOptions& options,
                       const std::filesystem::path& job_root,
                       std::wstring& result,
                       WindowsImportWorkerReport& report) {
    std::wstring path;
    for (std::size_t i = 0; i < options.immutable_module_roots.size(); ++i) {
        if (i) path.push_back(L';');
        path += options.immutable_module_roots[i].wstring();
    }
    std::array<wchar_t, MAX_PATH> system_directory{};
    const auto system_length = GetSystemWindowsDirectoryW(system_directory.data(),
                                                          static_cast<UINT>(system_directory.size()));
    if (system_length == 0 || system_length >= system_directory.size()) {
        diagnostic(report, "worker_system_directory_unavailable");
        return false;
    }
    const std::wstring system_root(system_directory.data(), system_length);
    // Explicit allowlist: never copy the caller's environment, credentials,
    // plugin settings, proxy configuration, or per-drive working directories.
    // System paths come from Windows, not caller-controlled environment values.
    std::vector<std::wstring> values{
        L"PATH=" + path,
        L"SystemRoot=" + system_root,
        L"WINDIR=" + system_root,
        L"SystemDrive=" + std::filesystem::path(system_root).root_name().wstring(),
        L"TEMP=" + job_root.wstring(),
        L"TMP=" + job_root.wstring(),
        L"LOCALAPPDATA=" + job_root.wstring(),
        L"PROJ_NETWORK=OFF",
        L"PROJ_DEBUG=0",
    };
    // AppContainer process creation requires LOCALAPPDATA even with a custom
    // environment. Keep it broker-owned instead of exposing the caller's path.
    // Retain only the broker-owned drive's current-directory entry.
    const auto root_name = job_root.root_name().wstring();
    if (root_name.size() >= 2 && root_name[1] == L':') {
        const auto prefix = L"=" + root_name + L"=";
        values.emplace_back(prefix + job_root.wstring());
    }

    std::sort(values.begin(), values.end(), [](const auto& left, const auto& right) {
        auto name = [](const std::wstring& value) {
            const auto separator = value.find(L'=');
            std::wstring result = value.substr(0, separator);
            std::transform(result.begin(), result.end(), result.begin(), [](wchar_t c) {
                return static_cast<wchar_t>(std::towlower(c));
            });
            return result;
        };
        const auto left_name = name(left);
        const auto right_name = name(right);
        return left_name == right_name ? left < right : left_name < right_name;
    });
    result.clear();
    for (const auto& entry : values) {
        result += entry;
        result.push_back(L'\0');
    }
    result.push_back(L'\0');
    if (result.size() > 32767) {
        diagnostic(report, "worker_environment_limit");
        result.clear();
        return false;
    }
    return true;
}

bool create_job_directory(const std::filesystem::path& root, std::filesystem::path& result,
                          WindowsImportWorkerReport& report) {
    const auto stamp = static_cast<unsigned long long>(GetTickCount64());
    const auto pid = static_cast<unsigned long long>(GetCurrentProcessId());
    for (unsigned int attempt = 0; attempt < 64; ++attempt) {
        result = root / (L"property-studio-import-" + std::to_wstring(pid) + L"-" +
                         std::to_wstring(stamp) + L"-" + std::to_wstring(attempt));
        if (CreateDirectoryW(result.c_str(), nullptr)) {
            if (!ordinary_object(result, true)) {
                diagnostic(report, "temporary_root_verification_failed");
                RemoveDirectoryW(result.c_str());
                return false;
            }
            return true;
        }
        if (GetLastError() != ERROR_ALREADY_EXISTS) break;
    }
    diagnostic(report, "temporary_root_create_failed");
    return false;
}

bool current_user_sid(std::vector<std::byte>& storage, PSID& result) {
    OwnedHandle token;
    HANDLE raw_token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw_token)) return false;
    token = OwnedHandle(raw_token);
    DWORD bytes = 0;
    GetTokenInformation(token.get(), TokenUser, nullptr, 0, &bytes);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || bytes == 0 || bytes > 1024 * 1024) return false;
    storage.resize(bytes);
    if (!GetTokenInformation(token.get(), TokenUser, storage.data(), bytes, &bytes)) return false;
    result = reinterpret_cast<PTOKEN_USER>(storage.data())->User.Sid;
    return IsValidSid(result) != FALSE;
}

bool protect_job_directory(const std::filesystem::path& path, PSID app_container_sid,
                           WindowsImportWorkerReport& report) {
    std::vector<std::byte> user_storage;
    PSID user_sid = nullptr;
    if (!current_user_sid(user_storage, user_sid)) {
        diagnostic(report, "temporary_acl_failed");
        return false;
    }
    EXPLICIT_ACCESSW entries[2]{};
    for (auto& entry : entries) {
        entry.grfAccessPermissions = GENERIC_ALL;
        entry.grfAccessMode = SET_ACCESS;
        entry.grfInheritance = OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE;
    }
    BuildTrusteeWithSidW(&entries[0].Trustee, user_sid);
    BuildTrusteeWithSidW(&entries[1].Trustee, app_container_sid);
    PACL acl = nullptr;
    const auto acl_error = SetEntriesInAclW(2, entries, nullptr, &acl);
    LocalBuffer new_acl(reinterpret_cast<HLOCAL>(acl));
    if (acl_error != ERROR_SUCCESS || !acl) {
        diagnostic(report, "temporary_acl_failed");
        return false;
    }
    const auto error = SetNamedSecurityInfoW(const_cast<LPWSTR>(path.c_str()), SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
        nullptr, nullptr, acl, nullptr);
    if (error != ERROR_SUCCESS) {
        diagnostic(report, "temporary_acl_failed");
        return false;
    }
    report.private_temporary_root_verified = true;
    return true;
}

bool cleanup_job_directory(const std::filesystem::path& path) {
    if (!ordinary_object(path, true)) return false;
    std::error_code error;
    std::filesystem::recursive_directory_iterator iterator(
        path, std::filesystem::directory_options::skip_permission_denied, error);
    const std::filesystem::recursive_directory_iterator end;
    if (error) return false;
    for (; iterator != end; iterator.increment(error)) {
        if (error) return false;
        const auto attributes = GetFileAttributesW(iterator->path().c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
    }
    std::filesystem::remove_all(path, error);
    return !error && !std::filesystem::exists(path, error);
}

bool parent_job_allows_breakaway(bool& parent_in_job, WindowsImportWorkerReport& report) {
    parent_in_job = false;
    BOOL in_job = FALSE;
    if (!IsProcessInJob(GetCurrentProcess(), nullptr, &in_job)) {
        diagnostic(report, "parent_job_query_failed");
        return false;
    }
    if (!in_job) return true;

    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    if (!QueryInformationJobObject(nullptr, JobObjectExtendedLimitInformation, &limits,
                                   sizeof(limits), nullptr)) {
        diagnostic(report, "parent_job_query_failed");
        return false;
    }
    const auto breakaway_flags = limits.BasicLimitInformation.LimitFlags &
        (JOB_OBJECT_LIMIT_BREAKAWAY_OK | JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK);
    if (!breakaway_flags) {
        diagnostic(report, "parent_job_breakaway_unavailable");
        return false;
    }
    parent_in_job = true;
    return true;
}

bool app_container_sid(SidBuffer& sid, WindowsImportWorkerReport& report) {
    static constexpr wchar_t profile_name[] = L"PropertyStudio.ImportWorker";
    PSID value = nullptr;
    auto result = CreateAppContainerProfile(profile_name, L"Vertex import worker",
                                       L"Local worker for bounded drawing-file interchange", nullptr, 0, &value);
    if (SUCCEEDED(result) && value) {
        sid = SidBuffer(value);
        return true;
    }
    // CreateAppContainerProfile reports an existing profile as an error and
    // does not return a SID. Derive it only after the profile has been
    // confirmed to exist; deriving a SID alone does not create the profile.
    value = nullptr;
    if (result == HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS)) {
        result = DeriveAppContainerSidFromAppContainerName(profile_name, &value);
        if (SUCCEEDED(result) && value) {
            sid = SidBuffer(value);
            return true;
        }
    }
    diagnostic(report, "app_container_profile_unavailable");
    return false;
}

bool token_attestation(HANDLE process, PSID expected_sid, WindowsImportWorkerReport& report) {
    OwnedHandle token;
    HANDLE raw_token = nullptr;
    if (!OpenProcessToken(process, TOKEN_QUERY, &raw_token)) {
        diagnostic(report, "worker_token_query_failed");
        return false;
    }
    token = OwnedHandle(raw_token);
    BOOL app_container = FALSE;
    DWORD bytes = 0;
    if (!GetTokenInformation(token.get(), TokenIsAppContainer, &app_container, sizeof(app_container), &bytes)) {
        diagnostic(report, "worker_token_query_failed");
        return false;
    }
    if (!app_container) {
        diagnostic(report, "worker_not_app_container");
        return false;
    }
    report.app_container_verified = true;
    BOOL has_restrictions = FALSE;
    if (!GetTokenInformation(token.get(), TokenHasRestrictions, &has_restrictions,
                             sizeof(has_restrictions), &bytes)) {
        diagnostic(report, "worker_restriction_query_failed");
        return false;
    }
    // AppContainer tokens are restricted by construction. Some Windows
    // releases report TokenHasRestrictions as false for a regular
    // AppContainer, so the AppContainer identity is the authoritative
    // restriction signal while the query is retained for diagnostics.
    report.restricted_token_verified = has_restrictions != FALSE || report.app_container_verified;
    if (!report.restricted_token_verified) diagnostic(report, "worker_token_not_restricted");

    DWORD container_bytes = 0;
    GetTokenInformation(token.get(), TokenAppContainerSid, nullptr, 0, &container_bytes);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER ||
        container_bytes < sizeof(TOKEN_APPCONTAINER_INFORMATION) || container_bytes > 1024 * 1024) {
        diagnostic(report, "worker_app_container_identity_query_failed");
        return false;
    }
    std::vector<std::byte> container_storage(container_bytes);
    if (!GetTokenInformation(token.get(), TokenAppContainerSid, container_storage.data(),
                             container_bytes, &container_bytes)) {
        diagnostic(report, "worker_app_container_identity_query_failed");
        return false;
    }
    const auto* container = reinterpret_cast<const TOKEN_APPCONTAINER_INFORMATION*>(container_storage.data());
    if (!container->TokenAppContainer || !IsValidSid(container->TokenAppContainer) ||
        !EqualSid(container->TokenAppContainer, expected_sid)) {
        diagnostic(report, "worker_app_container_identity_mismatch");
        return false;
    }

    DWORD required = 0;
    GetTokenInformation(token.get(), TokenCapabilities, nullptr, 0, &required);
    const auto capability_size_error = GetLastError();
    if (capability_size_error != ERROR_INSUFFICIENT_BUFFER && capability_size_error != ERROR_SUCCESS) {
        diagnostic(report, "worker_capability_query_failed");
        return false;
    }
    std::vector<std::byte> capability_storage(required);
    PTOKEN_GROUPS capabilities = required ? reinterpret_cast<PTOKEN_GROUPS>(capability_storage.data()) : nullptr;
    if (required && !GetTokenInformation(token.get(), TokenCapabilities, capabilities, required, &required)) {
        diagnostic(report, "worker_capability_query_failed");
        return false;
    }
    // These are the three network capabilities defined by Windows. Other
    // capabilities do not grant network access, so a future non-network
    // capability can be reviewed without weakening this denial check.
    static constexpr const wchar_t* network_sids[] = {
        L"S-1-15-3-1", L"S-1-15-3-2", L"S-1-15-3-3"
    };
    bool network_capability = false;
    for (const auto* sid_text : network_sids) {
        PSID network_sid = nullptr;
        if (ConvertStringSidToSidW(sid_text, &network_sid)) {
            LocalBuffer owned(reinterpret_cast<HLOCAL>(network_sid));
            if (capabilities) {
                for (DWORD index = 0; index < capabilities->GroupCount; ++index) {
                    if (EqualSid(network_sid, capabilities->Groups[index].Sid)) network_capability = true;
                }
            }
        }
    }
    report.network_denial_verified = !network_capability;
    if (network_capability) diagnostic(report, "worker_network_capability_present");
    return report.restricted_token_verified && report.network_denial_verified;
}

bool configure_job(HANDLE job, std::uint64_t memory, std::uint32_t active,
                  WindowsImportWorkerReport& report) {
    if (active != 1 || memory == 0 || memory > 4ULL * 1024 * 1024 * 1024) {
        diagnostic(report, "invalid_job_limits");
        return false;
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE |
        JOB_OBJECT_LIMIT_ACTIVE_PROCESS | JOB_OBJECT_LIMIT_PROCESS_MEMORY;
    limits.BasicLimitInformation.ActiveProcessLimit = active;
    limits.ProcessMemoryLimit = static_cast<SIZE_T>(memory);
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
        diagnostic(report, "job_limits_failed");
        return false;
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION observed{};
    if (!QueryInformationJobObject(job, JobObjectExtendedLimitInformation, &observed,
                                   sizeof(observed), nullptr) ||
        (observed.BasicLimitInformation.LimitFlags & limits.BasicLimitInformation.LimitFlags) !=
            limits.BasicLimitInformation.LimitFlags ||
        observed.BasicLimitInformation.ActiveProcessLimit != active ||
        observed.ProcessMemoryLimit != limits.ProcessMemoryLimit) {
        diagnostic(report, "job_limits_unverified");
        return false;
    }
    report.job_limits_verified = true;
    report.parent_exit_kill_verified =
        (observed.BasicLimitInformation.LimitFlags & JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE) != 0;
    return report.parent_exit_kill_verified;
}

bool pipe_read(HANDLE pipe, std::vector<std::byte>& output, std::uint64_t limit,
               WindowsImportWorkerReport& report) {
    DWORD available = 0;
    if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) {
        const auto error = GetLastError();
        if (error == ERROR_BROKEN_PIPE || error == ERROR_PIPE_NOT_CONNECTED) return true;
        diagnostic(report, "worker_output_pipe_failed");
        return false;
    }
    while (available) {
        std::array<std::byte, 64 * 1024> buffer{};
        const DWORD requested = (std::min)(available, static_cast<DWORD>(buffer.size()));
        DWORD received = 0;
        if (!ReadFile(pipe, buffer.data(), requested, &received, nullptr)) {
            const auto error = GetLastError();
            if (error == ERROR_BROKEN_PIPE || error == ERROR_PIPE_NOT_CONNECTED) return true;
            diagnostic(report, "worker_output_pipe_failed");
            return false;
        }
        if (received == 0) break;
        if (output.size() > limit || received > limit - output.size()) {
            diagnostic(report, "output_size_limit");
            return false;
        }
        output.insert(output.end(), buffer.begin(), buffer.begin() + received);
        available -= received;
        if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) {
            const auto error = GetLastError();
            if (error == ERROR_BROKEN_PIPE || error == ERROR_PIPE_NOT_CONNECTED) return true;
            diagnostic(report, "worker_output_pipe_failed");
            return false;
        }
    }
    return true;
}

bool terminate_job(HANDLE job, HANDLE process, WindowsImportWorkerReport& report, const char* code) {
    diagnostic(report, code);
    if (!TerminateJobObject(job, 1)) {
        diagnostic(report, "worker_termination_failed");
        return false;
    }
    const auto wait = WaitForSingleObject(process, 5000);
    if (wait != WAIT_OBJECT_0) {
        diagnostic(report, "worker_exit_unconfirmed");
        return false;
    }
    return true;
}

} // namespace

WindowsImportWorkerReport run_windows_import_worker(const WindowsImportWorkerOptions& options) {
    WindowsImportWorkerReport report;
    report.status = WindowsImportWorkerStatus::invalid_request;
    if (!local_absolute_path(options.executable) || !ordinary_object(options.executable, false))
        diagnostic(report, "invalid_worker_executable");
    if (!local_absolute_path(options.temporary_root) || !ordinary_object(options.temporary_root, true))
        diagnostic(report, "invalid_temporary_root");
    if (options.input.size() > 64ULL * 1024 * 1024) diagnostic(report, "input_size_limit");
    if (options.timeout_ms == 0 || options.timeout_ms > 30'000) diagnostic(report, "invalid_timeout");
    if (options.memory_bytes == 0 || options.memory_bytes > 4ULL * 1024 * 1024 * 1024)
        diagnostic(report, "invalid_memory_limit");
    if (options.max_active_processes != 1) diagnostic(report, "invalid_active_process_limit");
    if (options.max_output_bytes == 0 || options.max_output_bytes > 256ULL * 1024 * 1024)
        diagnostic(report, "invalid_output_limit");
    if (!report.diagnostics.empty()) {
        sort_diagnostics(report);
        return report;
    }

    std::vector<wchar_t> command;
    if (!build_command_line(options, command, report) || !verify_module_roots(options, report)) {
        sort_diagnostics(report);
        return report;
    }
    if (same_or_below(options.temporary_root, options.executable.parent_path()) ||
        std::any_of(options.immutable_module_roots.begin(), options.immutable_module_roots.end(),
                    [&](const auto& root) { return same_or_below(options.temporary_root, root) ||
                        same_or_below(root, options.temporary_root); })) {
        diagnostic(report, "temporary_root_overlaps_worker_roots");
        sort_diagnostics(report);
        return report;
    }

    SidBuffer app_sid;
    if (!app_container_sid(app_sid, report)) {
        sort_diagnostics(report);
        return report;
    }
    std::filesystem::path job_root;
    if (!create_job_directory(options.temporary_root, job_root, report) ||
        !protect_job_directory(job_root, app_sid.get(), report)) {
        if (!job_root.empty()) (void)cleanup_job_directory(job_root);
        sort_diagnostics(report);
        return report;
    }

    OwnedHandle job(CreateJobObjectW(nullptr, nullptr));
    if (!job.valid() || !configure_job(job.get(), options.memory_bytes, options.max_active_processes, report)) {
        if (!job.valid()) diagnostic(report, "job_create_failed");
        (void)cleanup_job_directory(job_root);
        sort_diagnostics(report);
        return report;
    }

    SECURITY_ATTRIBUTES pipe_attributes{};
    pipe_attributes.nLength = sizeof(pipe_attributes);
    pipe_attributes.bInheritHandle = TRUE;
    OwnedHandle child_input_read;
    OwnedHandle parent_input_write;
    OwnedHandle parent_output_read;
    OwnedHandle child_output_write;
    HANDLE child_read = nullptr;
    HANDLE parent_write = nullptr;
    HANDLE parent_read = nullptr;
    HANDLE child_write = nullptr;
    if (!CreatePipe(&child_read, &parent_write, &pipe_attributes, 0) ||
        !CreatePipe(&parent_read, &child_write, &pipe_attributes, 0)) {
        if (child_read) CloseHandle(child_read);
        if (parent_write) CloseHandle(parent_write);
        if (parent_read) CloseHandle(parent_read);
        if (child_write) CloseHandle(child_write);
        diagnostic(report, "worker_pipe_create_failed");
        (void)cleanup_job_directory(job_root);
        sort_diagnostics(report);
        return report;
    }
    child_input_read = OwnedHandle(child_read);
    parent_input_write = OwnedHandle(parent_write);
    parent_output_read = OwnedHandle(parent_read);
    child_output_write = OwnedHandle(child_write);
    if (!SetHandleInformation(parent_input_write.get(), HANDLE_FLAG_INHERIT, 0) ||
        !SetHandleInformation(parent_output_read.get(), HANDLE_FLAG_INHERIT, 0)) {
        diagnostic(report, "worker_pipe_inheritance_failed");
        (void)cleanup_job_directory(job_root);
        sort_diagnostics(report);
        return report;
    }

    SIZE_T attribute_bytes = 0;
    (void)InitializeProcThreadAttributeList(nullptr, 2, 0, &attribute_bytes);
    if (!attribute_bytes) {
        diagnostic(report, "worker_attribute_list_failed");
        (void)cleanup_job_directory(job_root);
        sort_diagnostics(report);
        return report;
    }
    std::vector<std::byte> attribute_storage(attribute_bytes);
    auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attribute_storage.data());
    if (!InitializeProcThreadAttributeList(attributes, 2, 0, &attribute_bytes)) {
        diagnostic(report, "worker_attribute_list_failed");
        (void)cleanup_job_directory(job_root);
        sort_diagnostics(report);
        return report;
    }
    HANDLE inherited[] = {child_input_read.get(), child_output_write.get()};
    if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                   inherited, sizeof(inherited), nullptr, nullptr)) {
        DeleteProcThreadAttributeList(attributes);
        diagnostic(report, "worker_handle_list_attribute_failed");
        (void)cleanup_job_directory(job_root);
        sort_diagnostics(report);
        return report;
    }
    SECURITY_CAPABILITIES capabilities{};
    capabilities.AppContainerSid = app_sid.get();
    if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES,
                                   &capabilities, sizeof(capabilities), nullptr, nullptr)) {
        DeleteProcThreadAttributeList(attributes);
        diagnostic(report, "worker_app_container_attribute_failed");
        (void)cleanup_job_directory(job_root);
        sort_diagnostics(report);
        return report;
    }
    std::wstring environment;
    if (!environment_block(options, job_root, environment, report)) {
        DeleteProcThreadAttributeList(attributes);
        (void)cleanup_job_directory(job_root);
        sort_diagnostics(report);
        return report;
    }
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = child_input_read.get();
    startup.StartupInfo.hStdOutput = child_output_write.get();
    startup.StartupInfo.hStdError = child_output_write.get();
    startup.lpAttributeList = attributes;
    PROCESS_INFORMATION process_information{};
    bool parent_in_job = false;
    if (!parent_job_allows_breakaway(parent_in_job, report)) {
        DeleteProcThreadAttributeList(attributes);
        (void)cleanup_job_directory(job_root);
        sort_diagnostics(report);
        return report;
    }
    DWORD flags = CREATE_SUSPENDED | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT |
        CREATE_UNICODE_ENVIRONMENT;
    if (parent_in_job) flags |= CREATE_BREAKAWAY_FROM_JOB;
    const BOOL launched = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE,
                                         flags, environment.data(), job_root.c_str(), &startup.StartupInfo,
                                         &process_information);
    const auto launch_error = launched ? ERROR_SUCCESS : GetLastError();
    DeleteProcThreadAttributeList(attributes);
    if (!launched) {
        report.launch_error = launch_error;
        if (launch_error == ERROR_NOT_SUPPORTED || launch_error == ERROR_CALL_NOT_IMPLEMENTED)
            diagnostic(report, "app_container_launch_not_supported");
        else if (launch_error == ERROR_ACCESS_DENIED)
            diagnostic(report, "app_container_launch_access_denied");
        diagnostic(report, "worker_launch_failed");
        report.status = WindowsImportWorkerStatus::launch_failed;
        (void)cleanup_job_directory(job_root);
        sort_diagnostics(report);
        return report;
    }
    OwnedHandle process(process_information.hProcess);
    OwnedHandle thread(process_information.hThread);
    report.launched = true;
    report.process_id = process_information.dwProcessId;
    child_input_read.close();
    child_output_write.close();

    if (!AssignProcessToJobObject(job.get(), process.get())) {
        diagnostic(report, "worker_job_assignment_failed");
        // The suspended process never joined our job; terminating that empty
        // job cannot stop it. Retain its handle until direct termination is
        // confirmed, before returning or removing its temporary directory.
        if (!TerminateProcess(process.get(), 1)) diagnostic(report, "worker_termination_failed");
        if (WaitForSingleObject(process.get(), 5000) != WAIT_OBJECT_0)
            diagnostic(report, "worker_exit_unconfirmed");
        report.status = WindowsImportWorkerStatus::launch_failed;
        (void)cleanup_job_directory(job_root);
        sort_diagnostics(report);
        return report;
    }
    if (!token_attestation(process.get(), app_sid.get(), report)) {
        (void)terminate_job(job.get(), process.get(), report, "worker_sandbox_attestation_failed");
        report.status = WindowsImportWorkerStatus::launch_failed;
        (void)cleanup_job_directory(job_root);
        sort_diagnostics(report);
        return report;
    }
    report.brokered_handles_verified = true;
    report.fixed_search_applied = true;
    report.proj_offline_applied = options.proj_offline_required;
    if (options.proj_offline_required == false) {
        diagnostic(report, "proj_offline_required");
        (void)terminate_job(job.get(), process.get(), report, "unsafe_proj_network_policy");
        report.status = WindowsImportWorkerStatus::launch_failed;
        (void)cleanup_job_directory(job_root);
        sort_diagnostics(report);
        return report;
    }
    if (ResumeThread(thread.get()) == static_cast<DWORD>(-1)) {
        (void)terminate_job(job.get(), process.get(), report, "worker_resume_failed");
        report.status = WindowsImportWorkerStatus::launch_failed;
        (void)cleanup_job_directory(job_root);
        sort_diagnostics(report);
        return report;
    }

    std::atomic<bool> input_failed{false};
    std::thread input_thread([&] {
        std::size_t offset = 0;
        while (offset < options.input.size()) {
            const auto requested = static_cast<DWORD>((std::min)(options.input.size() - offset,
                                                                 static_cast<std::size_t>(64 * 1024)));
            DWORD written = 0;
            if (!WriteFile(parent_input_write.get(), options.input.data() + offset, requested, &written, nullptr) ||
                written == 0) {
                input_failed.store(true);
                break;
            }
            offset += written;
        }
        parent_input_write.close();
    });

    const auto deadline = Clock::now() + std::chrono::milliseconds(options.timeout_ms);
    bool output_ok = true;
    bool timed_out = false;
    for (;;) {
        output_ok = pipe_read(parent_output_read.get(), report.output, options.max_output_bytes, report);
        if (!output_ok) {
            (void)terminate_job(job.get(), process.get(), report, "worker_output_rejected");
            report.output.clear();
            break;
        }
        const auto wait = WaitForSingleObject(process.get(), 5);
        if (wait == WAIT_OBJECT_0) break;
        if (wait == WAIT_FAILED) {
            diagnostic(report, "worker_wait_failed");
            (void)terminate_job(job.get(), process.get(), report, "worker_wait_failed");
            report.output.clear();
            break;
        }
        if (Clock::now() >= deadline) {
            timed_out = true;
            (void)terminate_job(job.get(), process.get(), report, "worker_timeout");
            report.output.clear();
            break;
        }
        Sleep(2);
    }
    if (input_thread.joinable()) input_thread.join();
    parent_input_write.close();
    // Drain output after normal exit. A failed/terminated worker's bytes are
    // discarded below, so a partial parse can never reach the document model.
    if (!timed_out && output_ok && WaitForSingleObject(process.get(), 0) == WAIT_OBJECT_0) {
        for (unsigned int pass = 0; pass < 1000; ++pass) {
            DWORD before = static_cast<DWORD>(report.output.size());
            if (!pipe_read(parent_output_read.get(), report.output, options.max_output_bytes, report)) {
                output_ok = false;
                break;
            }
            if (before == report.output.size()) {
                DWORD available = 0;
                if (!PeekNamedPipe(parent_output_read.get(), nullptr, 0, nullptr, &available, nullptr)) break;
                if (!available) break;
            }
            Sleep(1);
        }
    }
    parent_output_read.close();
    if (!GetExitCodeProcess(process.get(), reinterpret_cast<LPDWORD>(&report.exit_code))) {
        diagnostic(report, "worker_exit_code_failed");
        report.exit_code = 1;
    }
    if (input_failed.load()) diagnostic(report, "worker_input_pipe_failed");
    if (timed_out) {
        report.status = WindowsImportWorkerStatus::timed_out;
        report.timed_out = true;
        report.completed = false;
        report.output.clear();
    } else if (!output_ok || report.exit_code != 0 || !report.diagnostics.empty()) {
        report.status = WindowsImportWorkerStatus::failed;
        report.completed = false;
        report.output.clear();
    } else {
        report.status = WindowsImportWorkerStatus::completed;
        report.completed = true;
    }
    if (!cleanup_job_directory(job_root)) {
        diagnostic(report, "temporary_cleanup_failed");
        report.status = WindowsImportWorkerStatus::failed;
        report.completed = false;
        report.output.clear();
    }
    sort_diagnostics(report);
    return report;
}

#else

WindowsImportWorkerReport run_windows_import_worker(const WindowsImportWorkerOptions&) {
    WindowsImportWorkerReport report;
    report.status = WindowsImportWorkerStatus::unsupported;
    diagnostic(report, "windows_only");
    sort_diagnostics(report);
    return report;
}

#endif

} // namespace sketch
