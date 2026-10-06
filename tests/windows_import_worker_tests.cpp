#include "sketch/windows_import_worker.hpp"
#include "support/noninteractive_errors.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include <array>

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
class ImportedApiPatch final {
public:
    ImportedApiPatch(const char* target, void* replacement) {
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
                if (std::strcmp(reinterpret_cast<const char*>(imported->Name), target) == 0) {
                    slot_ = reinterpret_cast<void**>(&address->u1.Function);
                    break;
                }
            }
        }
        require(slot_ != nullptr, "fixture must find its imported API");
        DWORD protection = 0;
        require(VirtualProtect(slot_, sizeof(*slot_), PAGE_READWRITE, &protection) != FALSE,
                "fixture must access its import slot");
        original = InterlockedExchangePointer(slot_, replacement);
        DWORD ignored = 0;
        VirtualProtect(slot_, sizeof(*slot_), protection, &ignored);
    }
    ~ImportedApiPatch() {
        DWORD protection = 0;
        if (VirtualProtect(slot_, sizeof(*slot_), PAGE_READWRITE, &protection)) {
            InterlockedExchangePointer(slot_, original);
            DWORD ignored = 0;
            VirtualProtect(slot_, sizeof(*slot_), protection, &ignored);
        }
    }
    void* original{};
private:
    void** slot_{};
};

// Observe real native assignment and the last boundary before worker code can
// execute. Neither hook changes the operation or its return value.
class NestedMembershipObservation final {
public:
    explicit NestedMembershipObservation(HANDLE outer)
        : assignment_("AssignProcessToJobObject", reinterpret_cast<void*>(&assign)),
          resume_("ResumeThread", reinterpret_cast<void*>(&resume)) {
        outer_ = outer; own_ = nullptr; process_ = nullptr; verified = false;
        assign_original_ = assignment_.original; resume_original_ = resume_.original;
    }
    inline static bool verified{};
private:
    static BOOL WINAPI assign(HANDLE job, HANDLE process) {
        const auto result = reinterpret_cast<BOOL (WINAPI*)(HANDLE, HANDLE)>(assign_original_)(job, process);
        if (result) { own_ = job; process_ = process; }
        return result;
    }
    static DWORD WINAPI resume(HANDLE thread) {
        BOOL in_outer = FALSE, in_own = FALSE;
        verified = own_ && process_ && own_ != outer_ &&
            IsProcessInJob(process_, outer_, &in_outer) && in_outer &&
            IsProcessInJob(process_, own_, &in_own) && in_own;
        return reinterpret_cast<DWORD (WINAPI*)(HANDLE)>(resume_original_)(thread);
    }
    inline static HANDLE outer_{}, own_{}, process_{};
    inline static void* assign_original_{};
    inline static void* resume_original_{};
    ImportedApiPatch assignment_, resume_;
};

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
        assignment_api_ = original_;
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
        // Ask the real OS API to reject an invalid job handle. No synthetic
        // successful assignment or production failure switch participates.
        return reinterpret_cast<BOOL (WINAPI*)(HANDLE, HANDLE)>(assignment_api_)(nullptr, process);
    }
    inline static HANDLE process_{};
    inline static void* assignment_api_{};
    void** slot_{};
    void* original_{};
};

// Trigger cancellation at an actual worker lifecycle boundary. Like the
// assignment fixture above, patch only this test executable's imported API.
class CancellationAtBoundary final {
public:
    enum class Point { resumed, input_write_started, exit_collected };
    CancellationAtBoundary(Point point, std::shared_ptr<std::atomic_bool> flag) {
        flag_ = std::move(flag);
        const char* target = point == Point::resumed ? "ResumeThread" :
            point == Point::input_write_started ? "WriteFile" : "GetExitCodeProcess";
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
                if (std::strcmp(reinterpret_cast<const char*>(imported->Name), target) == 0) {
                    slot_ = reinterpret_cast<void**>(&address->u1.Function);
                    break;
                }
            }
        }
        require(slot_ != nullptr, "cancellation fixture must find its lifecycle API");
        DWORD protection = 0;
        require(VirtualProtect(slot_, sizeof(*slot_), PAGE_READWRITE, &protection) != FALSE,
                "cancellation fixture must access its import slot");
        auto replacement = point == Point::resumed ? reinterpret_cast<void*>(&resume) :
            point == Point::input_write_started ? reinterpret_cast<void*>(&write) : reinterpret_cast<void*>(&exit_code);
        original_ = InterlockedExchangePointer(slot_, replacement);
        DWORD ignored = 0;
        VirtualProtect(slot_, sizeof(*slot_), protection, &ignored);
    }
    ~CancellationAtBoundary() {
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
        flag_.reset();
    }
    HANDLE process() const { return process_; }
private:
    static DWORD WINAPI resume(HANDLE thread) {
        process_ = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION,
                               FALSE, GetProcessIdOfThread(thread));
        const auto result = reinterpret_cast<DWORD (WINAPI*)(HANDLE)>(original_)(thread);
        if (result != static_cast<DWORD>(-1)) flag_->store(true);
        return result;
    }
    static BOOL WINAPI exit_code(HANDLE process, LPDWORD code) {
        const auto result = reinterpret_cast<BOOL (WINAPI*)(HANDLE, LPDWORD)>(original_)(process, code);
        if (result) flag_->store(true);
        return result;
    }
    static BOOL WINAPI write(HANDLE handle, LPCVOID data, DWORD count, LPDWORD written, LPOVERLAPPED overlapped) {
        flag_->store(true);
        return reinterpret_cast<BOOL (WINAPI*)(HANDLE, LPCVOID, DWORD, LPDWORD, LPOVERLAPPED)>(original_)(
            handle, data, count, written, overlapped);
    }
    inline static std::shared_ptr<std::atomic_bool> flag_;
    inline static HANDLE process_{};
    inline static void* original_{};
    void** slot_{};
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
    static constexpr wchar_t name[] = L"Vertex.ImportWorker";
    PSID sid = nullptr;
    auto result = CreateAppContainerProfile(name, L"Vertex import worker",
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

class ScopedDeleteChildDeny {
public:
    explicit ScopedDeleteChildDeny(const std::filesystem::path& root) : root_(root) {
        require(GetNamedSecurityInfoW(const_cast<LPWSTR>(root_.c_str()), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
            nullptr, nullptr, &original_acl_, nullptr, &descriptor_) == ERROR_SUCCESS,
            "fixture holder ACL must be read");
        DWORD revision = 0;
        require(GetSecurityDescriptorControl(descriptor_, &control_, &revision) != FALSE,
                "fixture holder inheritance must be read");
        auto user_storage = current_user_sid_storage();
        auto* user_sid = reinterpret_cast<PTOKEN_USER>(user_storage.data())->User.Sid;
        EXPLICIT_ACCESSW entry{};
        entry.grfAccessPermissions = FILE_DELETE_CHILD;
        entry.grfAccessMode = DENY_ACCESS;
        entry.grfInheritance = NO_INHERITANCE;
        BuildTrusteeWithSidW(&entry.Trustee, user_sid);
        PACL acl = nullptr;
        require(SetEntriesInAclW(1, &entry, original_acl_, &acl) == ERROR_SUCCESS,
                "fixture holder deny ACL must be built");
        const auto result = SetNamedSecurityInfoW(const_cast<LPWSTR>(root_.c_str()), SE_FILE_OBJECT,
            DACL_SECURITY_INFORMATION, nullptr, nullptr, acl, nullptr);
        LocalFree(acl);
        require(result == ERROR_SUCCESS, "fixture holder delete-child deny must be applied");
    }
    ~ScopedDeleteChildDeny() {
        SetNamedSecurityInfoW(const_cast<LPWSTR>(root_.c_str()), SE_FILE_OBJECT,
            DACL_SECURITY_INFORMATION | ((control_ & SE_DACL_PROTECTED) ?
                PROTECTED_DACL_SECURITY_INFORMATION : UNPROTECTED_DACL_SECURITY_INFORMATION),
            nullptr, nullptr, original_acl_, nullptr);
        LocalFree(descriptor_);
    }
    ScopedDeleteChildDeny(const ScopedDeleteChildDeny&) = delete;
    ScopedDeleteChildDeny& operator=(const ScopedDeleteChildDeny&) = delete;
private:
    std::filesystem::path root_;
    PACL original_acl_{};
    PSECURITY_DESCRIPTOR descriptor_{};
    SECURITY_DESCRIPTOR_CONTROL control_{};
};

void make_immutable_module_root(const std::filesystem::path& root, const std::filesystem::path& worker,
                                DWORD additional_right = 0) {
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
    entries[0].grfAccessPermissions = GENERIC_READ | GENERIC_EXECUTE | additional_right;
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
    for (const auto& file : {worker_copy, root / "invalid-image.exe"})
        require(SetNamedSecurityInfoW(const_cast<LPWSTR>(file.c_str()), SE_FILE_OBJECT,
                                      DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                                      nullptr, nullptr, file_acl, nullptr) == ERROR_SUCCESS,
                "worker fixture file ACL must be applied");
    LocalFree(file_acl);
    FreeSid(app_sid);
}

void partial_module_rights_fail_closed(const std::filesystem::path& holder,
                                      const std::filesystem::path& worker) {
    for (const auto right : {DWORD(FILE_ADD_FILE), DWORD(FILE_ADD_SUBDIRECTORY), DWORD(DELETE)}) {
        const auto root = holder / ("partial-right-" + std::to_string(right));
        make_immutable_module_root(root, worker, right);
        const auto allowed = CreateFileW(root.c_str(), right,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        require(allowed != INVALID_HANDLE_VALUE, "partial-right fixture must grant its individual right");
        CloseHandle(allowed);
        const auto combined = CreateFileW(root.c_str(), FILE_ADD_FILE | FILE_ADD_SUBDIRECTORY | DELETE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        require(combined == INVALID_HANDLE_VALUE && GetLastError() == ERROR_ACCESS_DENIED,
                "combined probe must mask the partial-right fixture");
        const auto report = run_windows_import_worker(base_options(root / "worker-probe.exe"));
        std::cout << "partial_module_rights_fail_closed " << right << ": " << report.to_json().dump() << '\n';
        require(!report.launched && !report.immutable_module_roots_verified && has(report, "invalid_module_roots"),
                "a partially writable or deletable module root must fail before launch");
    }
}

void sharing_conflicts_do_not_attest_immutable_roots(const std::filesystem::path& holder,
                                                   const std::filesystem::path& worker) {
    // Keep a directory with writable rights open exclusively. Permission
    // checks alone would permit the broker's ADD_FILE probe, but sharing
    // prevents the open and must not be mistaken for immutable permissions.
    const auto root = holder / "sharing-conflict";
    make_immutable_module_root(root, worker, FILE_ADD_FILE | FILE_ADD_SUBDIRECTORY | DELETE);
    const auto handle = CreateFileW(root.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING,
                                   FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    require(handle != INVALID_HANDLE_VALUE, "sharing fixture must hold its directory exclusively");
    struct Close { HANDLE handle; ~Close() { CloseHandle(handle); } } close{handle};
    const auto blocked = CreateFileW(root.c_str(), FILE_ADD_FILE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    require(blocked == INVALID_HANDLE_VALUE && GetLastError() == ERROR_SHARING_VIOLATION,
            "sharing fixture must produce a sharing conflict on the write probe");
    const auto report = run_windows_import_worker(base_options(root / "worker-probe.exe"));
    require(!report.launched && !report.immutable_module_roots_verified && has(report, "invalid_module_roots"),
            "a sharing conflict must not attest immutable module roots");
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

void precancelled_request_does_not_launch() {
    auto options = base_options();
    options.cancellation_requested = std::make_shared<std::atomic_bool>(true);
    const auto report = run_windows_import_worker(options);
    require(report.status == WindowsImportWorkerStatus::cancelled && !report.launched &&
                !report.completed && !report.timed_out && report.output.empty() && !report.controls_attested(),
            "pre-cancelled requests must not launch or publish output");
    require(has(report, "worker_cancelled") && report.to_json().at("status") == "cancelled" &&
                report.to_json().at("controls_attested") == false && report.to_json().at("output_bytes") == 0,
            "cancelled report JSON must remain consistent and non-publishable");
}

void attested_echo_round_trip(const std::filesystem::path& worker);

void cancellation_terminates_only_the_invocation(const std::filesystem::path& worker) {
#ifdef _WIN32
    auto options = base_options(worker);
    options.temporary_root /= "cancel-test-" + std::to_string(GetCurrentProcessId()) + "-" +
                              std::to_string(GetTickCount64());
    require(std::filesystem::create_directory(options.temporary_root), "cancellation fixture temp root must be fresh");
    struct Cleanup {
        std::filesystem::path root;
        ~Cleanup() { std::error_code error; std::filesystem::remove_all(root, error); }
    } cleanup{options.temporary_root};
    options.arguments = {L"--sleep-ms", L"2000"};
    // More than a pipe buffer: the second cancellation below occurs after
    // the writer's flag check, before its blocking write to a sleeping child.
    options.input.assign(1024 * 1024, std::byte{'x'});
    auto flag = std::make_shared<std::atomic_bool>(false);
    options.cancellation_requested = flag;
    {
        CancellationAtBoundary cancellation(CancellationAtBoundary::Point::resumed, flag);
        const auto report = run_windows_import_worker(options);
        require(cancellation.process() != nullptr && WaitForSingleObject(cancellation.process(), 0) == WAIT_OBJECT_0,
                "cancelled invocation must confirm termination of its exact worker");
        require(report.status == WindowsImportWorkerStatus::cancelled && report.launched &&
                    !report.completed && !report.timed_out && report.output.empty() && !report.controls_attested(),
                "live cancellation must reject all worker output and attestation");
        require(report.exit_code == 1 && has(report, "worker_cancelled") &&
                    !has(report, "worker_exit_unconfirmed") && !has(report, "temporary_cleanup_failed") &&
                    std::filesystem::is_empty(options.temporary_root),
                "live cancellation must capture exit and clean the private job root");
    }
    flag->store(false);
    {
        CancellationAtBoundary cancellation(CancellationAtBoundary::Point::input_write_started, flag);
        const auto report = run_windows_import_worker(options);
        require(report.launched && report.status == WindowsImportWorkerStatus::cancelled && report.exit_code == 1 &&
                    !report.completed && report.output.empty() && !report.controls_attested() &&
                    !has(report, "worker_exit_unconfirmed") && std::filesystem::is_empty(options.temporary_root),
                "cancellation must unblock this invocation's input writer and clean its job root");
    }
    // The cancellation flag belongs to one call; another worker remains usable.
    attested_echo_round_trip(worker);
#else
    (void)worker;
#endif
}

void completion_race_cannot_publish_cancelled_output(const std::filesystem::path& worker) {
#ifdef _WIN32
    auto options = base_options(worker);
    auto flag = std::make_shared<std::atomic_bool>(false);
    options.cancellation_requested = flag;
    CancellationAtBoundary cancellation(CancellationAtBoundary::Point::exit_collected, flag);
    const auto report = run_windows_import_worker(options);
    require(report.launched && report.exit_code == 0 && report.status == WindowsImportWorkerStatus::cancelled &&
                !report.completed && report.output.empty() && !report.controls_attested(),
            "cancellation concurrent with normal exit must discard completed output");
    require(has(report, "worker_cancelled") && !has(report, "temporary_cleanup_failed"),
            "completion cancellation must preserve private-root cleanup");
#else
    (void)worker;
#endif
}

void attested_echo_round_trip(const std::filesystem::path& worker) {
#ifdef _WIN32
    const auto report = run_windows_import_worker(base_options(worker));
    std::cout << "attested_echo_round_trip: " << report.to_json().dump() << '\n';
    require(report.status == WindowsImportWorkerStatus::completed,
            "protected worker probe must complete in the AppContainer broker");
    require(report.launched && report.completed && !report.timed_out, "completed worker flags are inconsistent");
    require(report.controls_attested(), "completed worker must attest every control");
    require(report.job_membership_verified && report.to_json().at("job_membership_verified") == true,
            "completed worker must attest membership in its exact broker-owned job");
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
    auto options = base_options(worker);
    options.temporary_root /= "assignment-failure-" + std::to_string(GetCurrentProcessId());
    require(std::filesystem::create_directory(options.temporary_root), "assignment fixture root must be fresh");
    struct Cleanup { std::filesystem::path root; ~Cleanup() { std::error_code error; std::filesystem::remove_all(root, error); } } cleanup{options.temporary_root};
    bool resumed = false;
    // A failed assignment must never reach ResumeThread, even if the worker
    // would otherwise emit a valid echo result.
    static bool* resumed_flag = nullptr;
    resumed_flag = &resumed;
    ImportedApiPatch resume("ResumeThread", reinterpret_cast<void*>(+[](HANDLE) -> DWORD {
        *resumed_flag = true;
        SetLastError(ERROR_ACCESS_DENIED);
        return static_cast<DWORD>(-1);
    }));
    AssignmentFailure failure;
    const auto report = run_windows_import_worker(options);
    std::cout << "assignment_failure_terminates_suspended_worker: " << report.to_json().dump() << '\n';
    require(failure.process() != nullptr, "assignment failure must capture the exact suspended process");
    require(WaitForSingleObject(failure.process(), 0) == WAIT_OBJECT_0,
            "failed job assignment must terminate the unassigned suspended worker before returning");
    require(!resumed && report.status == WindowsImportWorkerStatus::launch_failed && !report.completed && report.output.empty(),
            "job assignment failure must reject all output");
    require(report.job_assignment_error == ERROR_INVALID_HANDLE &&
                report.to_json().at("job_assignment_error") == ERROR_INVALID_HANDLE &&
                has(report, "worker_job_assignment_failed") && !has(report, "worker_exit_unconfirmed") &&
                !has(report, "temporary_cleanup_failed") && std::filesystem::is_empty(options.temporary_root),
            "assignment failure must report confirmed process cleanup");
#else
    (void)worker;
#endif
}

void unverified_membership_never_resumes_worker(const std::filesystem::path& worker) {
#ifdef _WIN32
    for (const bool api_failure : {true, false}) {
        auto options = base_options(worker);
        options.temporary_root /= "membership-refusal-" + std::to_string(GetCurrentProcessId()) +
            (api_failure ? "-api" : "-false");
        require(std::filesystem::create_directory(options.temporary_root), "membership fixture root must be fresh");
        HANDLE process = nullptr;
        struct Close { HANDLE& value; ~Close() { if (value) CloseHandle(value); } } close{process};
        bool resumed = false;
        static HANDLE* observed_process = nullptr;
        static bool* observed_resume = nullptr;
        static bool failure = false;
        static void* membership_api = nullptr;
        observed_process = &process; observed_resume = &resumed; failure = api_failure;
        WindowsImportWorkerReport report;
        {
            ImportedApiPatch membership("IsProcessInJob", reinterpret_cast<void*>(+[](HANDLE child, HANDLE, PBOOL in_job) -> BOOL {
                require(DuplicateHandle(GetCurrentProcess(), child, GetCurrentProcess(), observed_process,
                    SYNCHRONIZE, FALSE, 0), "membership fixture must capture the real suspended child");
                if (failure) return reinterpret_cast<BOOL (WINAPI*)(HANDLE, HANDLE, PBOOL)>(membership_api)(
                    child, reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(1)), in_job);
                *in_job = FALSE;
                return TRUE;
            }));
            membership_api = membership.original;
            ImportedApiPatch resume("ResumeThread", reinterpret_cast<void*>(+[](HANDLE) -> DWORD {
                *observed_resume = true;
                return static_cast<DWORD>(-1);
            }));
            report = run_windows_import_worker(options);
        }
        std::cout << "unverified_membership_never_resumes_worker: " << report.to_json().dump() << '\n';
        require(process && WaitForSingleObject(process, 0) == WAIT_OBJECT_0,
            "membership refusal must terminate the exact suspended worker");
        require(report.launched && !resumed && !report.completed && report.output.empty() &&
            !report.job_membership_verified && !report.controls_attested() &&
            report.status == WindowsImportWorkerStatus::launch_failed &&
            has(report, "worker_job_membership_unverified") && !has(report, "worker_exit_unconfirmed") &&
            std::filesystem::is_empty(options.temporary_root),
            "membership API failure or false membership must refuse before resume and clean only confirmed storage");
        std::filesystem::remove(options.temporary_root);
    }
#else
    (void)worker;
#endif
}

void unconfirmed_assignment_exit_preserves_private_storage(const std::filesystem::path& worker) {
#ifdef _WIN32
    auto options = base_options(worker);
    options.temporary_root /= "unconfirmed-assignment-" + std::to_string(GetCurrentProcessId());
    require(std::filesystem::create_directory(options.temporary_root), "unconfirmed-exit fixture root must be fresh");
    struct Cleanup { std::filesystem::path root; ~Cleanup() { std::error_code error; std::filesystem::remove_all(root, error); } } cleanup{options.temporary_root};
    AssignmentFailure failure;
    WindowsImportWorkerReport report;
    {
        // Simulate an unavailable exit observation, rather than pretending a
        // suspended worker ran. The real exact-process termination still runs.
        ImportedApiPatch wait("WaitForSingleObject", reinterpret_cast<void*>(+[](HANDLE, DWORD) -> DWORD {
            return WAIT_TIMEOUT;
        }));
        report = run_windows_import_worker(options);
    }
    require(failure.process() && WaitForSingleObject(failure.process(), 5000) == WAIT_OBJECT_0,
            "fixture must independently confirm its exact worker exited after restoring the wait API");
    require(report.status == WindowsImportWorkerStatus::launch_failed && report.output.empty() &&
                !report.completed && !report.controls_attested() && has(report, "worker_exit_unconfirmed") &&
                !std::filesystem::is_empty(options.temporary_root),
            "broker must preserve private storage when its exit observation is unconfirmed");
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

void project_failure_markers_are_bounded(const std::filesystem::path& worker) {
#ifdef _WIN32
    struct Fixture {
        std::vector<std::wstring> arguments;
        std::string marker;
        const char* diagnostic;
        char exit_code{'4'};
    };
    const std::vector<Fixture> fixtures{
        {{L"ifc", L"0"}, "VERTEX_PROJECT_FAILURE_V1:core\n", "worker_project_failed_core"},
        {{L"dxf", L"0"}, "VERTEX_PROJECT_FAILURE_V1:library\r\n", "worker_project_failed_library"},
        {{L"ifc", L"0"}, "VERTEX_PROJECT_FAILURE_V1:merge\n", "worker_project_failed_merge"},
        {{L"dxf", L"0"}, "VERTEX_PROJECT_FAILURE_V1:candidate\n", "worker_project_failed_candidate"},
        {{L"ifc", L"0"}, "VERTEX_PROJECT_FAILURE_V1:unknown\n", nullptr},
        {{L"ifc", L"0"}, "VERTEX_PROJECT_FAILURE_V1:core", nullptr},
        {{L"ifc", L"0"}, "VERTEX_PROJECT_FAILURE_V1:core\nprivate fixture text\n", nullptr},
        {{L"ifc", L"0"}, "VERTEX_PROJECT_FAILURE_V1:core:5\n", nullptr},
        {{L"ifc", L"0"}, std::string("VERTEX_PROJECT_FAILURE_V1:core") + '\0' + '\n', nullptr},
        {{L"ifc", L"0"}, "untrusted prefix\nVERTEX_PROJECT_FAILURE_V1:core\n", nullptr},
        {{L"ifc", L"0"}, std::string(129, 'x') + "\n", nullptr},
        {{L"ifc", L"1"}, "VERTEX_PROJECT_FAILURE_V1:core\n", nullptr},
        {{L"dxf", L"00"}, "VERTEX_PROJECT_FAILURE_V1:core\n", nullptr},
        {{L"ocr", L"0"}, "VERTEX_PROJECT_FAILURE_V1:core\n", nullptr},
        {{L"unknown", L"0"}, "VERTEX_PROJECT_FAILURE_V1:core\n", nullptr},
        {{L"ifc", L"0"}, "VERTEX_PROJECT_FAILURE_V1:core\n", nullptr, '5'},
        {{L"ifc", L"0"}, "VERTEX_PROJECT_FAILURE_V1:core\n", nullptr, '0'},
    };
    for (const auto& fixture : fixtures) {
        auto options = base_options(worker);
        options.arguments = fixture.arguments;
        const auto input = fixture.exit_code + fixture.marker;
        options.input.assign(reinterpret_cast<const std::byte*>(input.data()),
                             reinterpret_cast<const std::byte*>(input.data() + input.size()));
        const auto report = run_windows_import_worker(options);
        require(report.launched && report.exit_code == static_cast<unsigned>(fixture.exit_code - '0'),
                "marker fixture must observe the real child exit");
        if (fixture.exit_code == '0') {
            require(report.completed && report.diagnostics.empty() && report.output.size() == fixture.marker.size(),
                    "a failure marker cannot turn a zero-exit reply into a reported failure");
        } else {
            require(report.status == WindowsImportWorkerStatus::failed && !report.completed &&
                        !report.controls_attested() && report.output.empty(),
                    "negative worker replies must remain failed and discard all raw output");
            require(fixture.diagnostic ? report.diagnostics == std::vector<std::string>{fixture.diagnostic} :
                                         report.diagnostics.empty(),
                    "only an exact known stage for exact project arguments and exit four may survive");
        }
    }
#else
    (void)worker;
#endif
}

void run(const std::filesystem::path& worker) {
    invalid_requests_fail_closed();
    precancelled_request_does_not_launch();
#ifdef _WIN32
    const auto profile_root = worker_profile_root();
    std::error_code profile_error;
    std::filesystem::create_directories(profile_root / "Temp", profile_error);
    require(!profile_error, "worker fixture profile temp root must be created");
    const auto holder = profile_root /
        ("windows-import-worker-holder-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
    require(std::filesystem::create_directories(holder), "worker fixture holder must be created");
    const auto module_root = holder / "modules";
    struct Cleanup {
        std::filesystem::path root;
        ~Cleanup() { std::error_code error; std::filesystem::remove_all(root, error); }
    } cleanup{holder};
    ScopedDeleteChildDeny parent_deny(holder);
    partial_module_rights_fail_closed(holder, worker);
    sharing_conflicts_do_not_attest_immutable_roots(holder, worker);
    make_immutable_module_root(module_root, worker);
    const auto worker_copy = module_root / "worker-probe.exe";
    const auto executable_access = CreateFileW(worker_copy.c_str(), GENERIC_READ | GENERIC_EXECUTE,
        FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    require(executable_access != INVALID_HANDLE_VALUE, "protected fixture must remain executable by the broker user");
    CloseHandle(executable_access);
    attested_echo_round_trip(worker_copy);
    project_failure_markers_are_bounded(worker_copy);
    cancellation_terminates_only_the_invocation(worker_copy);
    completion_race_cannot_publish_cancelled_output(worker_copy);
    deadline_terminates_whole_job(worker_copy);
    output_limit_rejects_partial_result(worker_copy);
    malformed_image_preserves_launch_error(worker_copy);
    assignment_failure_terminates_suspended_worker(worker_copy);
    unverified_membership_never_resumes_worker(worker_copy);
    unconfirmed_assignment_exit_preserves_private_storage(worker_copy);
    caller_secrets_are_not_inherited(worker_copy);
#endif
}

#ifdef _WIN32
void nested_no_breakaway_host(const std::filesystem::path& worker) {
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    const auto job = CreateJobObjectW(&security, nullptr);
    require(job != nullptr, "nested fixture must create its outer job");
    struct Close { HANDLE value; ~Close() { CloseHandle(value); } } close_job{job};
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    require(SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)),
            "nested fixture must configure its no-breakaway outer job");
    std::array<wchar_t, 32768> executable{};
    require(GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size())) > 0,
            "nested fixture must resolve its host executable");
    std::wstring command = L"\"" + std::wstring(executable.data()) + L"\" --nested-job-child " +
        std::to_wstring(reinterpret_cast<std::uintptr_t>(job)) + L" \"" + worker.wstring() + L"\"";
    SIZE_T bytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
    std::vector<std::byte> storage(bytes);
    const auto attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    require(InitializeProcThreadAttributeList(attributes, 1, 0, &bytes), "nested fixture attributes must initialize");
    struct Delete { LPPROC_THREAD_ATTRIBUTE_LIST value; ~Delete() { DeleteProcThreadAttributeList(value); } } delete_attributes{attributes};
    require(UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                     const_cast<HANDLE*>(&job), sizeof(job), nullptr, nullptr),
            "nested fixture must inherit only its outer job query handle");
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup); startup.lpAttributeList = attributes;
    PROCESS_INFORMATION process{};
    require(CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE,
                          CREATE_SUSPENDED | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT,
                          nullptr, nullptr, &startup.StartupInfo, &process), "nested fixture host must launch suspended");
    Close close_process{process.hProcess}, close_thread{process.hThread};
    struct Terminate { HANDLE process; ~Terminate() {
        if (WaitForSingleObject(process, 0) != WAIT_OBJECT_0) { TerminateProcess(process, 1); WaitForSingleObject(process, 5000); }
    } } terminate{process.hProcess};
    require(AssignProcessToJobObject(job, process.hProcess), "nested fixture host must join its no-breakaway job");
    require(ResumeThread(process.hThread) != static_cast<DWORD>(-1), "nested fixture host must resume");
    require(WaitForSingleObject(process.hProcess, 30000) == WAIT_OBJECT_0, "nested fixture host must exit within its deadline");
    DWORD code = 1;
    require(GetExitCodeProcess(process.hProcess, &code) && code == 0, "no-breakaway nested worker fixture must pass");
    std::cout << "nested_no_breakaway_host: retained parent membership and exact broker membership verified before resume\n";
}
#endif
} // namespace

int main(int argc, char** argv) {
    sketch::testing::noninteractive_errors();
    try {
#ifdef _WIN32
        if (argc == 4 && std::string_view(argv[1]) == "--nested-job-child") {
            const auto outer = reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(std::stoull(argv[2])));
            BOOL member = FALSE;
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
            require(IsProcessInJob(GetCurrentProcess(), outer, &member) && member &&
                        QueryInformationJobObject(outer, JobObjectExtendedLimitInformation, &limits, sizeof(limits), nullptr) &&
                        !(limits.BasicLimitInformation.LimitFlags & (JOB_OBJECT_LIMIT_BREAKAWAY_OK | JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK)),
                    "nested fixture host must retain its exact outer job without breakaway permission");
            NestedMembershipObservation observation(outer);
            run(std::filesystem::path(argv[3]));
            require(NestedMembershipObservation::verified, "worker must retain outer and exact broker membership before resume");
            CloseHandle(outer);
            return 0;
        }
#endif
        require(argc <= 2, "unexpected worker test arguments");
        run(argc == 2 ? std::filesystem::path(argv[1]) : std::filesystem::path{});
#ifdef _WIN32
        nested_no_breakaway_host(argc == 2 ? std::filesystem::path(argv[1]) : std::filesystem::path{});
#endif
        std::cout << "windows import worker tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "windows_import_worker_tests: " << error.what() << '\n';
        return 1;
    }
}
