#include "sketch/workspace_ownership_broker.hpp"
#include "support/noninteractive_errors.hpp"

#include <cstdlib>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {
using namespace sketch;

void require(bool value, std::string_view message) {
    if (!value) throw std::runtime_error(std::string(message));
}

class Handle final {
public:
    explicit Handle(HANDLE handle = nullptr) : handle_(handle) {}
    ~Handle() { if (handle_) CloseHandle(handle_); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    HANDLE get() const { return handle_; }
private:
    HANDLE handle_;
};

std::string random_digest() {
    std::random_device random;
    std::string result(64, '0');
    constexpr std::string_view digits = "0123456789abcdef";
    for (auto& digit : result) digit = digits[random() & 15U];
    return result;
}

WorkspaceOwnershipKey path_key(const std::string& digest) {
    return WorkspaceOwnershipKey::path(CanonicalSha256::from_hex(digest));
}

WorkspaceInstanceId instance(WorkspaceOwnershipBroker& broker) {
    const auto result = broker.new_workspace_instance();
    require(result.has_value(), "broker must issue a workspace identity");
    return *result;
}

class Child final {
public:
    explicit Child(const std::string& digest)
        : suffix_(random_digest()),
          ready_name_("Local\\Vertex.OwnershipTest.Ready." + suffix_),
          release_name_("Local\\Vertex.OwnershipTest.Release." + suffix_),
          ready_(CreateEventA(nullptr, TRUE, FALSE, ready_name_.c_str())),
          release_(CreateEventA(nullptr, TRUE, FALSE, release_name_.c_str())) {
        require(ready_.get() && release_.get(), "unique child handshake events must be created");
        wchar_t executable[32768]{};
        const auto length = GetModuleFileNameW(nullptr, executable, 32768);
        require(length > 0 && length < 32768, "test executable path must be available");
        const std::string arguments = " --child " + digest + " " + ready_name_ + " " + release_name_;
        std::wstring command = L"\"" + std::wstring(executable, length) + L"\"";
        command.append(arguments.begin(), arguments.end());
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        require(CreateProcessW(executable, command.data(), nullptr, nullptr, FALSE,
                    CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process) != 0,
                "hidden helper process must launch");
        process_ = process.hProcess;
        CloseHandle(process.hThread);
        const HANDLE waits[] = {ready_.get(), process_};
        if (WaitForMultipleObjects(2, waits, FALSE, 5000) != WAIT_OBJECT_0) {
            terminate();
            CloseHandle(process_);
            process_ = nullptr;
            throw std::runtime_error("child failed before ownership readiness handshake");
        }
    }
    ~Child() {
        if (process_) {
            // This is only our retained helper handle, never a process-name kill.
            if (WaitForSingleObject(process_, 0) == WAIT_TIMEOUT) terminate();
            CloseHandle(process_);
        }
    }
    Child(const Child&) = delete;
    Child& operator=(const Child&) = delete;

    void release_normally() {
        require(SetEvent(release_.get()) != 0, "child release handshake must signal");
        require(WaitForSingleObject(process_, 5000) == WAIT_OBJECT_0,
                "released child must exit within the deadline");
        DWORD code{};
        require(GetExitCodeProcess(process_, &code) && code == 0,
                "child must release its reservation and shut down successfully");
    }
    void crash() {
        require(TerminateProcess(process_, 99) != 0, "owned crash fixture must terminate");
        require(WaitForSingleObject(process_, 5000) == WAIT_OBJECT_0,
                "crashed child exit must be confirmed");
    }
private:
    void terminate() noexcept {
        if (process_ && WaitForSingleObject(process_, 0) == WAIT_TIMEOUT) {
            (void)TerminateProcess(process_, 98);
            (void)WaitForSingleObject(process_, 5000);
        }
    }
    std::string suffix_;
    std::string ready_name_;
    std::string release_name_;
    Handle ready_;
    Handle release_;
    HANDLE process_{};
};

int child_main(char** argv) {
    const auto key = path_key(argv[2]);
    const Handle ready(OpenEventA(EVENT_MODIFY_STATE, FALSE, argv[3]));
    const Handle release(OpenEventA(SYNCHRONIZE, FALSE, argv[4]));
    require(ready.get() && release.get(), "parent handshake events must exist");
    auto& broker = WorkspaceOwnershipBroker::instance();
    const auto acquired = broker.acquire(instance(broker), WorkspaceOwnershipBundle({key}));
    require(acquired.ok() && acquired.reservation && !acquired.abandonment_observed,
            "child must obtain normal exclusive ownership");
    require(SetEvent(ready.get()) != 0, "child must publish readiness after acquisition");
    require(WaitForSingleObject(release.get(), 15000) == WAIT_OBJECT_0,
            "child release handshake must arrive before deadline");
    require(broker.release(*acquired.reservation).ok(), "child must release ownership");
    require(broker.shutdown().ok(), "child broker must join before exit");
    return 0;
}

void contention_and_namespace(WorkspaceOwnershipBroker& broker) {
    const auto digest = random_digest();
    Child owner(digest);
    const auto conflict = broker.acquire(instance(broker), WorkspaceOwnershipBundle({path_key(digest)}));
    require(conflict.status == WorkspaceOwnershipStatus::conflict &&
                !conflict.reservation && !conflict.abandonment_observed,
            "another process holding the same path must cause ordinary contention");
    const auto independent = broker.acquire(instance(broker), WorkspaceOwnershipBundle({
        WorkspaceOwnershipKey::file(CanonicalSha256::from_hex(digest))}));
    require(independent.ok() && independent.reservation && !independent.abandonment_observed,
            "typed file and path namespaces must remain independent across processes");
    require(broker.release(*independent.reservation).ok(), "independent file lease must release");
    owner.release_normally();
    const auto retried = broker.acquire(instance(broker), WorkspaceOwnershipBundle({path_key(digest)}));
    require(retried.ok() && retried.reservation && !retried.abandonment_observed,
            "normal child release must permit a clean retry without abandonment");
    require(broker.release(*retried.reservation).ok(), "retried lease must release");
}

void partial_bundle_rollback(WorkspaceOwnershipBroker& broker) {
    auto held_digest = random_digest();
    held_digest.front() = '8';
    auto free_digest = random_digest();
    free_digest.front() = '0';
    Child owner(held_digest);
    const auto conflict = broker.acquire(instance(broker), WorkspaceOwnershipBundle({
        path_key(held_digest), path_key(free_digest)}));
    require(conflict.status == WorkspaceOwnershipStatus::conflict && !conflict.reservation,
            "later conflicting bundle key must reject the whole acquisition");
    Child rollback_witness(free_digest);
    rollback_witness.release_normally();
    owner.release_normally();
}

void owner_crash(WorkspaceOwnershipBroker& broker) {
    const auto digest = random_digest();
    Child owner(digest);
    // Preserve the kernel object while its owning process exits. This handle
    // is never waited on or owned by the test's requesting thread.
    const std::string mutex_name = "Global\\Vertex.Owner.Path." + digest;
    const Handle retained(OpenMutexA(SYNCHRONIZE, FALSE, mutex_name.c_str()));
    require(retained.get() != nullptr, "crash fixture must retain the existing mutex object");
    owner.crash();
    const auto acquired = broker.acquire(instance(broker), WorkspaceOwnershipBundle({path_key(digest)}));
    require(acquired.ok() && acquired.reservation && acquired.abandonment_observed,
            "a crashed owner must produce actual kernel abandonment on the broker thread");
    require(broker.release(*acquired.reservation).ok(), "abandoned ownership must release exactly once");
}
}  // namespace

int main(int argc, char** argv) {
    sketch::testing::noninteractive_errors();
    try {
        if (argc == 5 && std::string_view(argv[1]) == "--child") return child_main(argv);
        require(argc == 1, "unrecognized subprocess test arguments");
        auto& broker = WorkspaceOwnershipBroker::instance();
        contention_and_namespace(broker);
        partial_bundle_rollback(broker);
        owner_crash(broker);
        require(broker.shutdown().ok(), "parent broker must drain and join");
        std::cout << "Cross-process contention, typed namespaces, rollback and abandonment passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "workspace_ownership_process_tests: " << error.what() << '\n';
        return 1;
    }
}
