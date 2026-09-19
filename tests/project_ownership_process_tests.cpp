#include "sketch/project_ownership.hpp"
#include "support/noninteractive_errors.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <filesystem>
#include <cwchar>
#include <fstream>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {

using sketch::ProjectOwnershipSession;
using sketch::ProjectOwnershipStatus;

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

#ifdef _WIN32
class UniqueHandle final {
public:
    explicit UniqueHandle(HANDLE handle = nullptr) : handle_(handle) {}
    ~UniqueHandle() { if (handle_) CloseHandle(handle_); }
    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;
    HANDLE get() const noexcept { return handle_; }
    HANDLE release() noexcept { return std::exchange(handle_, nullptr); }

private:
    HANDLE handle_{};
};

std::string suffix() {
    std::random_device random;
    std::string value(24, '0');
    constexpr std::string_view digits = "0123456789abcdef";
    for (auto& character : value) character = digits[random() & 15U];
    return value;
}

std::wstring quote(std::wstring value) {
    std::wstring result = L"\"";
    result += value;
    result += L"\"";
    return result;
}

std::string narrow_ascii(const wchar_t* value) {
    std::string result;
    for (const auto* cursor = value; *cursor != L'\0'; ++cursor) {
        require(*cursor <= 0x7f, "process fixture event names must remain ASCII");
        result.push_back(static_cast<char>(*cursor));
    }
    return result;
}

int child_main(const wchar_t* path_argument, const char* ready_name,
               const char* release_name) {
    UniqueHandle ready(OpenEventA(EVENT_MODIFY_STATE, FALSE, ready_name));
    UniqueHandle release(OpenEventA(SYNCHRONIZE, FALSE, release_name));
    require(ready.get() && release.get(), "parent handshake events must open");

    ProjectOwnershipSession owner;
    require(owner.acquire(std::filesystem::path(path_argument)).status ==
                ProjectOwnershipStatus::acquired,
            "child process should acquire the project lease");
    require(SetEvent(ready.get()) != 0, "child should publish lease readiness");
    require(WaitForSingleObject(release.get(), 15000) == WAIT_OBJECT_0,
            "child should receive the release handshake");
    require(owner.release().status == ProjectOwnershipStatus::released,
            "child should release the project lease");
    return 0;
}

void cross_process_contention() {
    const auto path = std::filesystem::temp_directory_path() /
        ("vertex-project-ownership-" + suffix() + ".bldproj");
    struct RemoveOnExit final {
        std::filesystem::path path;
        ~RemoveOnExit() {
            std::error_code error;
            std::filesystem::remove(path, error);
        }
    } cleanup{path};
    {
        std::ofstream output(path, std::ios::binary);
        require(output.good(), "process fixture should create a project file");
        output << "project ownership process fixture";
        require(output.good(), "process fixture should write project bytes");
    }

    const auto token = suffix();
    const auto ready_name = "Local\\Vertex.ProjectOwnership.Ready." + token;
    const auto release_name = "Local\\Vertex.ProjectOwnership.Release." + token;
    UniqueHandle ready(CreateEventA(nullptr, TRUE, FALSE, ready_name.c_str()));
    UniqueHandle release(CreateEventA(nullptr, TRUE, FALSE, release_name.c_str()));
    require(ready.get() && release.get(), "process fixture events should be created");

    wchar_t executable[32768]{};
    const auto executable_length = GetModuleFileNameW(nullptr, executable, 32768);
    require(executable_length > 0 && executable_length < 32768,
            "process fixture executable path should be available");
    std::wstring command = quote(std::wstring(executable, executable_length));
    command += L" --child ";
    command += quote(path.wstring());
    command += L" ";
    command += std::wstring(ready_name.begin(), ready_name.end());
    command += L" ";
    command += std::wstring(release_name.begin(), release_name.end());

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    require(CreateProcessW(executable, command.data(), nullptr, nullptr, FALSE,
                            CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process) != 0,
            "process fixture child should launch hidden");
    UniqueHandle process_handle(process.hProcess);
    UniqueHandle thread_handle(process.hThread);
    const HANDLE waits[] = {ready.get(), process_handle.get()};
    require(WaitForMultipleObjects(2, waits, FALSE, 15000) == WAIT_OBJECT_0,
            "child should acquire before parent contention check");

    ProjectOwnershipSession parent;
    require(parent.acquire(path).status == ProjectOwnershipStatus::conflict,
            "a second process should be blocked by the project lease");
    auto case_variant = path;
    case_variant.replace_extension(L".BLDPROJ");
    ProjectOwnershipSession case_variant_parent;
    require(case_variant_parent.acquire(case_variant).status == ProjectOwnershipStatus::conflict,
            "a second process should honor Windows case-insensitive path identity");

    require(SetEvent(release.get()) != 0, "parent should release the child handshake");
    require(WaitForSingleObject(process_handle.get(), 15000) == WAIT_OBJECT_0,
            "child should exit after releasing the lease");
    DWORD exit_code = 1;
    require(GetExitCodeProcess(process_handle.get(), &exit_code) != 0 && exit_code == 0,
            "child should exit successfully after releasing the lease");

    require(parent.acquire(path).status == ProjectOwnershipStatus::acquired,
            "the next process should acquire after the child releases");
    require(parent.release().status == ProjectOwnershipStatus::released,
            "the parent should release its retry lease");
}
#endif

}  // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    sketch::testing::noninteractive_errors();
    try {
        if (argc == 5 && std::wstring_view(argv[1]) == L"--child") {
            const auto ready = narrow_ascii(argv[3]);
            const auto release = narrow_ascii(argv[4]);
            return child_main(argv[2], ready.c_str(), release.c_str());
        }
        require(argc == 1, "unrecognized process fixture arguments");
        cross_process_contention();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "project_ownership_process_tests: " << error.what() << '\n';
        return 1;
    }
}
#else
int main() { return 77; }
#endif
