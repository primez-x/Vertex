#include "sketch/windows_import_worker.hpp"
#include "support/noninteractive_errors.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <Aclapi.h>
#include <Objbase.h>
#include <Sddl.h>
#include <UserEnv.h>
#endif

namespace {
using namespace sketch;

void require(bool value, std::string_view message) {
    if (!value) throw std::runtime_error(std::string(message));
}

bool has(const WindowsImportWorkerReport& report, std::string_view code) {
    return std::find(report.diagnostics.begin(), report.diagnostics.end(), code) != report.diagnostics.end();
}

std::filesystem::path system_root() {
#ifdef _WIN32
    if (const auto* value = _wgetenv(L"SystemRoot")) return value;
#else
    if (const auto* value = std::getenv("SystemRoot")) return value;
#endif
    return std::filesystem::path("C:/Windows");
}

std::filesystem::path command_shell() {
#ifdef _WIN32
    if (const auto* value = _wgetenv(L"ComSpec")) return value;
#else
    if (const auto* value = std::getenv("ComSpec")) return value;
#endif
    return system_root() / "System32" / "cmd.exe";
}

#ifdef _WIN32
std::filesystem::path worker_profile_root();
#endif

WindowsImportWorkerOptions base_options(const std::filesystem::path& worker = {}) {
    WindowsImportWorkerOptions options;
    options.executable = worker.empty() ? command_shell() : worker;
    options.temporary_root = worker.empty() ? std::filesystem::temp_directory_path() : worker_profile_root() / "Temp";
    options.immutable_module_roots = {worker.empty() ? system_root() / "System32" : worker.parent_path()};
    options.arguments = {L"/d", L"/c", L"type"};
    options.input = {std::byte{'p'}, std::byte{'r'}, std::byte{'o'}, std::byte{'b'},
                     std::byte{'e'}, std::byte{'\r'}, std::byte{'\n'}};
    options.timeout_ms = 5'000;
    options.memory_bytes = 128ULL * 1024 * 1024;
    options.max_output_bytes = 1024;
    return options;
}

#ifdef _WIN32
std::vector<std::byte> current_user_sid_storage() {
    HANDLE token = nullptr;
    require(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) != FALSE,
            "worker fixture must query the current user token");
    DWORD bytes = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &bytes);
    require(GetLastError() == ERROR_INSUFFICIENT_BUFFER && bytes > 0, "worker fixture user SID size query failed");
    std::vector<std::byte> result(bytes);
    require(GetTokenInformation(token, TokenUser, result.data(), bytes, &bytes) != FALSE,
            "worker fixture user SID query failed");
    CloseHandle(token);
    return result;
}

PSID worker_app_container_sid() {
    static constexpr wchar_t name[] = L"PropertyStudio.ImportWorker";
    PSID sid = nullptr;
    auto result = CreateAppContainerProfile(name, L"Property Studio import worker",
                                           L"Local worker test profile", nullptr, 0, &sid);
    if (FAILED(result) && result == HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS)) {
        result = DeriveAppContainerSidFromAppContainerName(name, &sid);
    }
    require(SUCCEEDED(result) && sid != nullptr, "worker fixture AppContainer profile must exist");
    return sid;
}

std::filesystem::path worker_profile_root() {
    PSID sid = worker_app_container_sid();
    LPWSTR sid_text = nullptr;
    require(ConvertSidToStringSidW(sid, &sid_text) != FALSE, "worker fixture SID must stringify");
    PWSTR raw_path = nullptr;
    const auto result = GetAppContainerFolderPath(sid_text, &raw_path);
    LocalFree(sid_text);
    FreeSid(sid);
    require(SUCCEEDED(result) && raw_path != nullptr, "worker fixture profile root must resolve");
    const std::filesystem::path path(raw_path);
    CoTaskMemFree(raw_path);
    return path;
}

void make_immutable_module_root(const std::filesystem::path& root, const std::filesystem::path& worker) {
    std::error_code error;
    std::filesystem::remove_all(root, error);
    require(std::filesystem::create_directories(root, error) && !error,
            "worker fixture module root must be created");
    const auto worker_copy = root / "worker-probe.exe";
    std::filesystem::copy_file(worker, worker_copy, std::filesystem::copy_options::overwrite_existing, error);
    require(!error, "worker fixture executable must be copied into the protected module root");
    auto user_storage = current_user_sid_storage();
    auto* user_sid = reinterpret_cast<PTOKEN_USER>(user_storage.data())->User.Sid;
    PSID app_sid = worker_app_container_sid();
    EXPLICIT_ACCESSW entries[2]{};
    entries[0].grfAccessPermissions = DELETE | FILE_DELETE_CHILD | FILE_LIST_DIRECTORY | FILE_TRAVERSE |
        FILE_READ_DATA | FILE_READ_ATTRIBUTES | READ_CONTROL;
    entries[1].grfAccessPermissions = GENERIC_READ | GENERIC_EXECUTE;
    for (auto& entry : entries) {
        entry.grfAccessMode = SET_ACCESS;
        entry.grfInheritance = OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE;
    }
    BuildTrusteeWithSidW(&entries[0].Trustee, user_sid);
    BuildTrusteeWithSidW(&entries[1].Trustee, app_sid);
    PACL acl = nullptr;
    require(SetEntriesInAclW(2, entries, nullptr, &acl) == ERROR_SUCCESS && acl,
            "worker fixture module ACL must be built");
    require(SetNamedSecurityInfoW(const_cast<LPWSTR>(root.c_str()), SE_FILE_OBJECT,
                                  DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                                  nullptr, nullptr, acl, nullptr) == ERROR_SUCCESS,
            "worker fixture module ACL must be applied");
    LocalFree(acl);
    EXPLICIT_ACCESSW file_entries[2]{};
    file_entries[0].grfAccessPermissions = DELETE | FILE_READ_DATA | FILE_READ_ATTRIBUTES | READ_CONTROL;
    file_entries[1].grfAccessPermissions = GENERIC_READ | GENERIC_EXECUTE;
    for (auto& entry : file_entries) entry.grfAccessMode = SET_ACCESS;
    BuildTrusteeWithSidW(&file_entries[0].Trustee, user_sid);
    BuildTrusteeWithSidW(&file_entries[1].Trustee, app_sid);
    PACL file_acl = nullptr;
    require(SetEntriesInAclW(2, file_entries, nullptr, &file_acl) == ERROR_SUCCESS && file_acl,
            "worker fixture file ACL must be built");
    require(SetNamedSecurityInfoW(const_cast<LPWSTR>(worker_copy.c_str()), SE_FILE_OBJECT,
                                  DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                                  nullptr, nullptr, file_acl, nullptr) == ERROR_SUCCESS,
            "worker fixture file ACL must be applied");
    LocalFree(file_acl);
    FreeSid(app_sid);
}
#endif

void invalid_requests_fail_closed() {
    auto options = base_options();
    options.executable = "relative-worker.exe";
    const auto report = run_windows_import_worker(options);
    require(!report.launched && !report.completed, "relative worker must never launch");
    require(report.status == WindowsImportWorkerStatus::invalid_request ||
                report.status == WindowsImportWorkerStatus::unsupported,
            "invalid worker request must fail closed");
    if (report.status == WindowsImportWorkerStatus::invalid_request)
        require(has(report, "invalid_worker_executable"), "invalid executable diagnostic missing");
}

void attested_echo_round_trip(const std::filesystem::path& worker) {
#ifdef _WIN32
    const auto profile_root = worker_profile_root();
    std::error_code profile_error;
    std::filesystem::create_directories(profile_root / "Temp", profile_error);
    require(!profile_error, "worker fixture profile temp root must be created");
    const auto module_root = profile_root /
        ("windows-import-worker-modules-" + std::to_string(GetCurrentProcessId()));
    make_immutable_module_root(module_root, worker);
    const auto worker_copy = module_root / "worker-probe.exe";
    const auto report = run_windows_import_worker(base_options(worker_copy));
    std::error_code cleanup_error;
    std::filesystem::remove_all(module_root, cleanup_error);
    require(report.status == WindowsImportWorkerStatus::completed,
            "protected worker probe must complete in the AppContainer broker");
    require(report.launched && report.completed && !report.timed_out, "completed worker flags are inconsistent");
    require(report.controls_attested(), "completed worker must attest every control");
    const std::vector<std::byte> expected = {std::byte{'p'}, std::byte{'r'}, std::byte{'o'}, std::byte{'b'},
                                             std::byte{'e'}, std::byte{'\r'}, std::byte{'\n'}};
    require(report.output == expected, "brokered worker output must round-trip exactly");
    require(report.to_json().at("network_requests_permitted") == false,
            "worker report must remain offline by construction");
#else
    (void)worker;
#endif
}

void deadline_terminates_whole_job(const std::filesystem::path& worker) {
#ifdef _WIN32
    auto options = base_options(worker);
    options.arguments = {L"--sleep-ms", L"2000"};
    options.input.clear();
    options.timeout_ms = 100;
    const auto report = run_windows_import_worker(options);
    require(report.status == WindowsImportWorkerStatus::timed_out,
            "deadline probe must terminate the worker job");
    require(report.timed_out && report.launched && !report.completed, "timeout flags are inconsistent");
    require(has(report, "worker_timeout"), "timeout diagnostic missing");
#else
    (void)worker;
#endif
}

void output_limit_rejects_partial_result(const std::filesystem::path& worker) {
#ifdef _WIN32
    auto options = base_options(worker);
    options.arguments = {L"--echo"};
    options.max_output_bytes = 3;
    const auto report = run_windows_import_worker(options);
    require(report.status == WindowsImportWorkerStatus::failed, "oversized output must fail the worker");
    require(!report.completed && report.output.empty(), "partial output must never be returned");
    require(has(report, "output_size_limit"), "output limit diagnostic missing");
#else
    (void)worker;
#endif
}

void run(const std::filesystem::path& worker) {
    invalid_requests_fail_closed();
#ifdef _WIN32
    attested_echo_round_trip(worker);
    deadline_terminates_whole_job(worker);
    output_limit_rejects_partial_result(worker);
#endif
}

#ifdef _WIN32
bool fixture_host_is_job_constrained() {
    BOOL in_job = FALSE;
    if (!IsProcessInJob(GetCurrentProcess(), nullptr, &in_job)) return true;
    // AppContainer launch can be denied when the test runner itself is in a
    // parent job. A production desktop process is expected to run outside
    // that harness job, so report a CTest skip rather than a false pass/fail.
    return in_job != FALSE;
}
#endif
} // namespace

int main(int argc, char** argv) {
    sketch::testing::noninteractive_errors();
    try {
        require(argc <= 2, "unexpected worker test arguments");
#ifdef _WIN32
        if (fixture_host_is_job_constrained()) {
            std::cout << "windows import worker runtime fixture skipped: host process is in a parent job\n";
            return 77;
        }
#endif
        run(argc == 2 ? std::filesystem::path(argv[1]) : std::filesystem::path{});
        std::cout << "windows import worker tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "windows_import_worker_tests: " << error.what() << '\n';
        return 1;
    }
}
