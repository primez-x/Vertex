#include "sketch/windows_import_worker.hpp"
#include "support/noninteractive_errors.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
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
    options.arguments = worker.empty() ? std::vector<std::wstring>{L"/d", L"/c", L"type"} :
                                        std::vector<std::wstring>{L"--echo"};
    options.input = {std::byte{'p'}, std::byte{'r'}, std::byte{'o'}, std::byte{'b'},
                     std::byte{'e'}, std::byte{'\r'}, std::byte{'\n'}};
    options.timeout_ms = 5'000;
    options.memory_bytes = 128ULL * 1024 * 1024;
    options.max_output_bytes = 1024;
    return options;
}

#ifdef _WIN32
// Patch only this fixture executable's import slot, never the broker API or
// system DLL. This injects the OS failure deterministically without shipping a
// production environment-variable switch or bypass.
class AssignmentFailure final {
public:
    AssignmentFailure() {
        const auto image = reinterpret_cast<std::byte*>(GetModuleHandleW(nullptr));
        const auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(image);
        const auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(image + dos->e_lfanew);
        const auto directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        auto descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(image + directory.VirtualAddress);
        for (; descriptor->Name && !slot_; ++descriptor) {
            if (!descriptor->OriginalFirstThunk) continue;
            auto name = reinterpret_cast<IMAGE_THUNK_DATA*>(image + descriptor->OriginalFirstThunk);
            auto address = reinterpret_cast<IMAGE_THUNK_DATA*>(image + descriptor->FirstThunk);
            for (; name->u1.AddressOfData; ++name, ++address) {
                if (IMAGE_SNAP_BY_ORDINAL(name->u1.Ordinal)) continue;
                const auto imported = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(image + name->u1.AddressOfData);
                if (std::strcmp(reinterpret_cast<const char*>(imported->Name), "AssignProcessToJobObject") == 0) {
                    slot_ = reinterpret_cast<void**>(&address->u1.Function);
                    break;
                }
            }
        }
        require(slot_ != nullptr, "assignment failure fixture must find the imported API");
        DWORD protection = 0;
        require(VirtualProtect(slot_, sizeof(*slot_), PAGE_READWRITE, &protection) != FALSE,
                "assignment failure fixture must access its import slot");
        original_ = InterlockedExchangePointer(slot_, reinterpret_cast<void*>(&fail));
        DWORD ignored = 0;
        VirtualProtect(slot_, sizeof(*slot_), protection, &ignored);
    }
    ~AssignmentFailure() {
        DWORD protection = 0;
        if (VirtualProtect(slot_, sizeof(*slot_), PAGE_READWRITE, &protection)) {
            InterlockedExchangePointer(slot_, original_);
            DWORD ignored = 0;
            VirtualProtect(slot_, sizeof(*slot_), protection, &ignored);
        }
        if (process_) {
            if (WaitForSingleObject(process_, 0) != WAIT_OBJECT_0) {
                TerminateProcess(process_, 1);
                WaitForSingleObject(process_, 5000);
            }
            CloseHandle(process_);
            process_ = nullptr;
        }
    }
    HANDLE process() const { return process_; }
private:
    static BOOL WINAPI fail(HANDLE, HANDLE process) {
        if (!DuplicateHandle(GetCurrentProcess(), process, GetCurrentProcess(), &process_,
                             SYNCHRONIZE | PROCESS_TERMINATE, FALSE, 0)) return FALSE;
        SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    inline static HANDLE process_{};
    void** slot_{};
    void* original_{};
};

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
    {
        std::ofstream invalid_image(root / "invalid-image.exe", std::ios::binary);
        invalid_image << "not a PE executable";
        require(invalid_image.good(), "malformed executable fixture must be written");
    }
    auto user_storage = current_user_sid_storage();
    auto* user_sid = reinterpret_cast<PTOKEN_USER>(user_storage.data())->User.Sid;
    PSID app_sid = worker_app_container_sid();
    EXPLICIT_ACCESSW entries[2]{};
    entries[0].grfAccessPermissions = DELETE | FILE_DELETE_CHILD | GENERIC_READ | GENERIC_EXECUTE;
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
    file_entries[0].grfAccessPermissions = DELETE | GENERIC_READ | GENERIC_EXECUTE;
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
    const auto report = run_windows_import_worker(base_options(worker));
    std::cout << "attested_echo_round_trip: " << report.to_json().dump() << '\n';
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
    std::cout << "deadline_terminates_whole_job: " << report.to_json().dump() << '\n';
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
    std::cout << "output_limit_rejects_partial_result: " << report.to_json().dump() << '\n';
    require(report.status == WindowsImportWorkerStatus::failed, "oversized output must fail the worker");
    require(!report.completed && report.output.empty(), "partial output must never be returned");
    require(has(report, "output_size_limit"), "output limit diagnostic missing");
#else
    (void)worker;
#endif
}

void malformed_image_preserves_launch_error(const std::filesystem::path& worker) {
#ifdef _WIN32
    auto options = base_options(worker.parent_path() / "invalid-image.exe");
    const auto report = run_windows_import_worker(options);
    std::cout << "malformed_image_preserves_launch_error: " << report.to_json().dump() << '\n';
    require(report.status == WindowsImportWorkerStatus::launch_failed && !report.launched &&
                !report.completed && report.output.empty(), "malformed executable must fail before launch");
    require(has(report, "worker_launch_failed"), "malformed executable launch diagnostic missing");
    require((report.launch_error == ERROR_BAD_EXE_FORMAT || report.launch_error == ERROR_EXE_MACHINE_TYPE_MISMATCH) &&
                report.to_json().at("launch_error") == report.launch_error,
            "native launch error must survive cleanup and JSON serialization");
#else
    (void)worker;
#endif
}

void assignment_failure_terminates_suspended_worker(const std::filesystem::path& worker) {
#ifdef _WIN32
    AssignmentFailure failure;
    const auto report = run_windows_import_worker(base_options(worker));
    std::cout << "assignment_failure_terminates_suspended_worker: " << report.to_json().dump() << '\n';
    require(failure.process() != nullptr, "assignment failure must capture the exact suspended process");
    require(WaitForSingleObject(failure.process(), 0) == WAIT_OBJECT_0,
            "failed job assignment must terminate the unassigned suspended worker before returning");
    require(report.status == WindowsImportWorkerStatus::launch_failed && !report.completed && report.output.empty(),
            "job assignment failure must reject all output");
    require(has(report, "worker_job_assignment_failed") && !has(report, "worker_exit_unconfirmed"),
            "assignment failure must report confirmed process cleanup");
#else
    (void)worker;
#endif
}

void caller_secrets_are_not_inherited(const std::filesystem::path& worker) {
#ifdef _WIN32
    constexpr auto sentinel = L"VERTEX_WORKER_SENTINEL_SECRET";
    require(GetEnvironmentVariableW(sentinel, nullptr, 0) == 0 && GetLastError() == ERROR_ENVVAR_NOT_FOUND,
            "secret-inheritance fixture requires an unused sentinel name");
    require(SetEnvironmentVariableW(sentinel, L"fixture-only-value-never-log") != FALSE,
            "secret-inheritance fixture must set its sentinel");
    struct Cleanup { ~Cleanup() { SetEnvironmentVariableW(L"VERTEX_WORKER_SENTINEL_SECRET", nullptr); } } cleanup;
    auto options = base_options(worker);
    options.arguments = {L"--assert-clean-environment"};
    options.input.clear();
    const auto report = run_windows_import_worker(options);
    std::cout << "caller_secrets_are_not_inherited: " << report.to_json().dump() << '\n';
    require(report.completed && report.exit_code == 0 && report.output.empty(),
            "worker must omit caller secrets while retaining required runtime environment");
#else
    (void)worker;
#endif
}

void run(const std::filesystem::path& worker) {
    invalid_requests_fail_closed();
#ifdef _WIN32
    const auto profile_root = worker_profile_root();
    std::error_code profile_error;
    std::filesystem::create_directories(profile_root / "Temp", profile_error);
    require(!profile_error, "worker fixture profile temp root must be created");
    const auto module_root = profile_root /
        ("windows-import-worker-modules-" + std::to_string(GetCurrentProcessId()));
    struct Cleanup {
        std::filesystem::path root;
        ~Cleanup() { std::error_code error; std::filesystem::remove_all(root, error); }
    } cleanup{module_root};
    make_immutable_module_root(module_root, worker);
    const auto worker_copy = module_root / "worker-probe.exe";
    const auto executable_access = CreateFileW(worker_copy.c_str(), GENERIC_READ | GENERIC_EXECUTE,
        FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    require(executable_access != INVALID_HANDLE_VALUE, "protected fixture must remain executable by the broker user");
    CloseHandle(executable_access);
    attested_echo_round_trip(worker_copy);
    deadline_terminates_whole_job(worker_copy);
    output_limit_rejects_partial_result(worker_copy);
    malformed_image_preserves_launch_error(worker_copy);
    assignment_failure_terminates_suspended_worker(worker_copy);
    caller_secrets_are_not_inherited(worker_copy);
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
