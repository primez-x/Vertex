#include "sketch/workspace_ownership_broker.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <exception>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <type_traits>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace sketch {
namespace {

std::atomic<std::uint64_t> next_workspace_id{1};
std::atomic<std::uint64_t> next_broker_id{1};

int hex_value(char value) noexcept {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    return -1;
}

std::string status_message(WorkspaceOwnershipStatus status) {
    switch (status) {
    case WorkspaceOwnershipStatus::success:
        return {};
    case WorkspaceOwnershipStatus::invalid_workspace:
        return "workspace instance is invalid";
    case WorkspaceOwnershipStatus::invalid_bundle:
        return "ownership bundle must contain at least one key";
    case WorkspaceOwnershipStatus::invalid_reservation:
        return "reservation identity is invalid or no longer owned";
    case WorkspaceOwnershipStatus::duplicate_workspace:
        return "workspace instance already has a reservation";
    case WorkspaceOwnershipStatus::conflict:
        return "ownership key is already reserved";
    case WorkspaceOwnershipStatus::key_not_owned:
        return "ownership key is not owned by this reservation";
    case WorkspaceOwnershipStatus::backend_failure:
        return "ownership backend failure; acquisition is disabled";
    case WorkspaceOwnershipStatus::owner_failed:
        return "ownership owner thread failed; acquisition is disabled";
    case WorkspaceOwnershipStatus::reentrant_shutdown:
        return "shutdown must be called from outside the ownership owner thread";
    case WorkspaceOwnershipStatus::stopped:
        return "ownership broker is stopped";
    }
    return "unknown ownership status";
}

WorkspaceOwnershipResult result_for(WorkspaceOwnershipStatus status,
                                    std::optional<WorkspaceOwnershipReservation> reservation = {},
                                    bool abandonment_observed = false) {
    return WorkspaceOwnershipResult{status, status_message(status), std::move(reservation),
                                    abandonment_observed};
}

#ifdef _WIN32
class WindowsOwnershipBackend final : public WorkspaceOwnershipBackend {
public:
    Handle create(std::string_view mutex_name) override {
        const std::wstring wide_name(mutex_name.begin(), mutex_name.end());
        const auto handle = ::CreateMutexW(nullptr, FALSE, wide_name.c_str());
        if (handle == nullptr)
            throw std::system_error(static_cast<int>(::GetLastError()), std::system_category(),
                                    "CreateMutexW");
        return reinterpret_cast<Handle>(handle);
    }

    WaitResult wait(Handle handle, std::uint32_t timeout_ms) override {
        const auto result = ::WaitForSingleObject(reinterpret_cast<HANDLE>(handle), timeout_ms);
        switch (result) {
        case WAIT_OBJECT_0:
            return WaitResult::acquired;
        case WAIT_ABANDONED:
            return WaitResult::abandoned;
        case WAIT_TIMEOUT:
            return WaitResult::timeout;
        default:
            throw std::system_error(static_cast<int>(::GetLastError()), std::system_category(),
                                    "WaitForSingleObject");
        }
    }

    void release(Handle handle) override {
        if (!::ReleaseMutex(reinterpret_cast<HANDLE>(handle)))
            throw std::system_error(static_cast<int>(::GetLastError()), std::system_category(),
                                    "ReleaseMutex");
    }

    void close(Handle handle) override {
        if (!::CloseHandle(reinterpret_cast<HANDLE>(handle)))
            throw std::system_error(static_cast<int>(::GetLastError()), std::system_category(),
                                    "CloseHandle");
    }
};
#else
// The non-Windows backend keeps the production API usable for portable unit
// tests. Windows builds use kernel mutexes above; this fallback intentionally
// models only process-local named ownership.
class PortableOwnershipBackend final : public WorkspaceOwnershipBackend {
public:
    Handle create(std::string_view mutex_name) override {
        std::lock_guard lock(mutex_);
        const auto handle = next_handle_++;
        handles_.emplace(handle, std::string(mutex_name));
        objects_.try_emplace(std::string(mutex_name));
        return handle;
    }

    WaitResult wait(Handle handle, std::uint32_t timeout_ms) override {
        (void)timeout_ms;
        std::lock_guard lock(mutex_);
        const auto handle_it = handles_.find(handle);
        if (handle_it == handles_.end()) throw std::runtime_error("unknown ownership handle");
        auto object_it = objects_.find(handle_it->second);
        if (object_it == objects_.end()) throw std::runtime_error("unknown ownership object");
        auto& object = object_it->second;
        const auto owner = std::this_thread::get_id();
        if (object.owner == std::thread::id{}) {
            object.owner = owner;
            object.recursion = 1;
            return WaitResult::acquired;
        }
        if (object.owner == owner) {
            ++object.recursion;
            return WaitResult::acquired;
        }
        return WaitResult::timeout;
    }

    void release(Handle handle) override {
        std::lock_guard lock(mutex_);
        const auto handle_it = handles_.find(handle);
        if (handle_it == handles_.end()) throw std::runtime_error("unknown ownership handle");
        auto object_it = objects_.find(handle_it->second);
        if (object_it == objects_.end()) throw std::runtime_error("unknown ownership object");
        auto& object = object_it->second;
        if (object.owner != std::this_thread::get_id() || object.recursion == 0)
            throw std::runtime_error("ownership mutex released by a non-owner thread");
        --object.recursion;
        if (object.recursion == 0) object.owner = {};
    }

    void close(Handle handle) override {
        std::lock_guard lock(mutex_);
        handles_.erase(handle);
    }

private:
    struct Object {
        std::thread::id owner;
        std::size_t recursion = 0;
    };

    std::mutex mutex_;
    Handle next_handle_ = 1;
    std::map<Handle, std::string> handles_;
    std::map<std::string, Object, std::less<>> objects_;
};
#endif

std::unique_ptr<WorkspaceOwnershipBackend> default_backend() {
#ifdef _WIN32
    return std::make_unique<WindowsOwnershipBackend>();
#else
    return std::make_unique<PortableOwnershipBackend>();
#endif
}

}  // namespace

CanonicalSha256::CanonicalSha256(std::array<std::uint8_t, 32> bytes) noexcept
    : bytes_(bytes) {}

std::optional<CanonicalSha256> CanonicalSha256::parse(std::string_view value) noexcept {
    if (value.size() != 64) return std::nullopt;
    std::array<std::uint8_t, 32> bytes{};
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        const auto high = hex_value(value[index * 2]);
        const auto low = hex_value(value[index * 2 + 1]);
        if (high < 0 || low < 0) return std::nullopt;
        bytes[index] = static_cast<std::uint8_t>((high << 4) | low);
    }
    return CanonicalSha256(bytes);
}

CanonicalSha256 CanonicalSha256::from_hex(std::string_view value) {
    const auto parsed = parse(value);
    if (!parsed.has_value())
        throw std::invalid_argument("SHA256 digest must contain 64 lowercase hex digits");
    return *parsed;
}

std::string CanonicalSha256::hex() const {
    static constexpr char digits[] = "0123456789abcdef";
    std::string result(64, '0');
    for (std::size_t index = 0; index < bytes_.size(); ++index) {
        result[index * 2] = digits[bytes_[index] >> 4];
        result[index * 2 + 1] = digits[bytes_[index] & 0x0f];
    }
    return result;
}

bool operator<(const CanonicalSha256& lhs, const CanonicalSha256& rhs) noexcept {
    return lhs.bytes_ < rhs.bytes_;
}

WorkspaceOwnershipKey::WorkspaceOwnershipKey(Kind kind, CanonicalSha256 digest) noexcept
    : kind_(kind), digest_(std::move(digest)) {}

WorkspaceOwnershipKey WorkspaceOwnershipKey::path(CanonicalSha256 digest) {
    return WorkspaceOwnershipKey(Kind::path, std::move(digest));
}

WorkspaceOwnershipKey WorkspaceOwnershipKey::file(CanonicalSha256 digest) {
    return WorkspaceOwnershipKey(Kind::file, std::move(digest));
}

WorkspaceOwnershipKey::Kind WorkspaceOwnershipKey::kind() const noexcept {
    return kind_;
}

const CanonicalSha256& WorkspaceOwnershipKey::digest() const noexcept {
    return digest_;
}

std::string WorkspaceOwnershipKey::canonical_sha256() const {
    return digest_.hex();
}

bool operator<(const WorkspaceOwnershipKey& lhs, const WorkspaceOwnershipKey& rhs) noexcept {
    if (lhs.kind_ != rhs.kind_)
        return static_cast<std::uint8_t>(lhs.kind_) < static_cast<std::uint8_t>(rhs.kind_);
    return lhs.digest_ < rhs.digest_;
}

WorkspaceOwnershipBundle::WorkspaceOwnershipBundle(std::vector<WorkspaceOwnershipKey> keys)
    : keys_(std::move(keys)) {
    std::sort(keys_.begin(), keys_.end());
    keys_.erase(std::unique(keys_.begin(), keys_.end()), keys_.end());
}

const std::vector<WorkspaceOwnershipKey>& WorkspaceOwnershipBundle::keys() const noexcept {
    return keys_;
}

bool WorkspaceOwnershipBundle::empty() const noexcept {
    return keys_.empty();
}

WorkspaceInstanceId::WorkspaceInstanceId(std::uint64_t value) noexcept : value_(value) {}

bool WorkspaceInstanceId::valid() const noexcept {
    return value_ != 0;
}

WorkspaceOwnershipReservation::WorkspaceOwnershipReservation(
    std::uint64_t broker_id, std::uint64_t reservation_id, std::uint64_t workspace_id) noexcept
    : broker_id_(broker_id), reservation_id_(reservation_id), workspace_id_(workspace_id) {}

bool WorkspaceOwnershipReservation::valid() const noexcept {
    return broker_id_ != 0 && reservation_id_ != 0 && workspace_id_ != 0;
}

namespace detail {

class WorkspaceOwnershipBrokerImpl final {
public:
    explicit WorkspaceOwnershipBrokerImpl(
        std::unique_ptr<WorkspaceOwnershipBackend> backend,
        std::optional<WorkspaceOwnershipTestHooks> test_hooks)
        : backend_(std::move(backend)), broker_id_(next_broker_id.fetch_add(1)),
          test_hooks_(std::move(test_hooks)) {
        if (!backend_) throw std::invalid_argument("ownership backend is required");
        if (broker_id_ == 0) throw std::overflow_error("ownership broker ID exhausted");
        owner_thread_ = std::thread([this] { owner_loop(); });
    }

    ~WorkspaceOwnershipBrokerImpl() {
        shutdown_and_join_terminal();
    }

    [[nodiscard]] std::optional<WorkspaceInstanceId> new_workspace_instance() {
        std::lock_guard lock(queue_mutex_);
        if (lifecycle_.load() != Lifecycle::running) return std::nullopt;
        const auto value = next_workspace_id.fetch_add(1);
        if (value == 0) return std::nullopt;
        return WorkspaceInstanceId(value);
    }

    [[nodiscard]] WorkspaceOwnershipResult acquire(const WorkspaceInstanceId& workspace,
                                                    const WorkspaceOwnershipBundle& bundle) {
        return invoke(
            [this, workspace, bundle] { return acquire_on_owner(workspace, bundle); }, false);
    }

    [[nodiscard]] WorkspaceOwnershipResult ensure(
        const WorkspaceOwnershipReservation& reservation,
        const WorkspaceOwnershipBundle& bundle) {
        return invoke(
            [this, reservation, bundle] { return ensure_on_owner(reservation, bundle); }, false);
    }

    [[nodiscard]] WorkspaceOwnershipResult release_keys(
        const WorkspaceOwnershipReservation& reservation,
        const WorkspaceOwnershipBundle& bundle) {
        return invoke(
            [this, reservation, bundle] { return release_keys_on_owner(reservation, bundle); },
            true);
    }

    [[nodiscard]] WorkspaceOwnershipResult release(
        const WorkspaceOwnershipReservation& reservation) {
        return invoke([this, reservation] { return release_on_owner(reservation); }, true);
    }

    [[nodiscard]] WorkspaceOwnershipResult observe_abandonment(
        const WorkspaceOwnershipReservation& reservation) {
        return invoke([this, reservation] { return observe_abandonment_on_owner(reservation); },
                      false);
    }

    [[nodiscard]] WorkspaceOwnershipResult shutdown_and_join() {
        // Joining the current thread is impossible. Leave lifecycle and queue
        // state untouched so an external caller can perform the shutdown.
        if (is_owner_thread())
            return result_for(WorkspaceOwnershipStatus::reentrant_shutdown);

        std::lock_guard shutdown_lock(shutdown_mutex_);
        if (!owner_thread_.joinable()) {
            auto status = completed_shutdown_status_.value_or(
                failure_seen_.load() ? WorkspaceOwnershipStatus::owner_failed
                                     : WorkspaceOwnershipStatus::stopped);
            if (!finalize_after_owner_exit()) status = WorkspaceOwnershipStatus::owner_failed;
            completed_shutdown_status_ = status;
            return result_for(status);
        }

        WorkspaceOwnershipStatus status = WorkspaceOwnershipStatus::owner_failed;
        bool join_owner = false;
        {
            std::unique_lock queue_lock(queue_mutex_);
            const auto lifecycle = lifecycle_.load();
            if (lifecycle == Lifecycle::stopped) {
                join_owner = true;
            } else {
                if (lifecycle != Lifecycle::stopping) {
                    shutdown_attempt_complete_ = false;
                    shutdown_requested_ = true;
                    terminal_shutdown_requested_ = false;
                }
                lifecycle_.store(Lifecycle::stopping);
                queue_cv_.notify_one();
                queue_cv_.wait(queue_lock, [this] {
                    return shutdown_attempt_complete_ ||
                           lifecycle_.load() == Lifecycle::stopped;
                });
                if (shutdown_attempt_complete_)
                    status = shutdown_attempt_status_;
                join_owner = lifecycle_.load() == Lifecycle::stopped;
            }
        }

        if (join_owner) {
            owner_thread_.join();
            if (status == WorkspaceOwnershipStatus::owner_failed &&
                !failure_seen_.load())
                status = WorkspaceOwnershipStatus::success;
            if (!finalize_after_owner_exit()) status = WorkspaceOwnershipStatus::owner_failed;
            completed_shutdown_status_ = status;
        }
        return result_for(status);
    }

    void shutdown_and_join_terminal() noexcept {
        try {
            std::lock_guard shutdown_lock(shutdown_mutex_);
            if (owner_thread_.joinable()) {
                {
                    std::unique_lock queue_lock(queue_mutex_);
                    if (lifecycle_.load() != Lifecycle::stopped) {
                        shutdown_attempt_complete_ = false;
                        shutdown_requested_ = true;
                        terminal_shutdown_requested_ = true;
                        lifecycle_.store(Lifecycle::stopping);
                        queue_cv_.notify_one();
                        queue_cv_.wait(queue_lock, [this] {
                            return lifecycle_.load() == Lifecycle::stopped;
                        });
                    }
                }
                owner_thread_.join();
            }
            (void)finalize_after_owner_exit();
        } catch (...) {
            // All backend failures are handled below the join boundary. With a
            // valid external destructor call, mutex waits and join do not throw.
            failure_seen_.store(true);
        }
    }

private:
    // State transitions are published while queue_mutex_ is held:
    // running -> failed, running|failed -> stopping, and * -> stopped.
    enum class Lifecycle : std::uint8_t { running, stopping, stopped, failed };

    struct HeldKey {
        WorkspaceOwnershipKey key;
        WorkspaceOwnershipBackend::Handle handle = 0;
    };

    struct HeldHandle {
        WorkspaceOwnershipBackend::Handle handle = 0;
        bool mutex_owned = true;
    };

    struct ReservationRecord {
        std::uint64_t workspace_id = 0;
        std::map<WorkspaceOwnershipKey, HeldHandle> keys;
        bool abandonment_observed = false;
        bool requires_full_release = false;
    };

    struct Request {
        std::function<void()> run;
        std::function<void()> fail;
    };

    struct CleanupOutcome {
        bool complete = false;
        WorkspaceOwnershipStatus status = WorkspaceOwnershipStatus::owner_failed;
    };

    template <typename Operation>
    [[nodiscard]] WorkspaceOwnershipResult invoke(Operation&& operation, bool allow_failed) {
        const bool owner = is_owner_thread();
        if (owner) {
            Lifecycle lifecycle;
            {
                std::lock_guard queue_lock(queue_mutex_);
                lifecycle = lifecycle_.load();
            }
            if (lifecycle == Lifecycle::stopping || lifecycle == Lifecycle::stopped)
                return result_for(WorkspaceOwnershipStatus::stopped);
            if (!allow_failed && lifecycle == Lifecycle::failed)
                return result_for(WorkspaceOwnershipStatus::owner_failed);
            return execute(std::forward<Operation>(operation), allow_failed);
        }

        try {
            std::promise<WorkspaceOwnershipResult> promise;
            auto future = promise.get_future();
            {
                std::lock_guard queue_lock(queue_mutex_);
                const auto lifecycle = lifecycle_.load();
                const bool rejected = lifecycle == Lifecycle::stopping ||
                                      lifecycle == Lifecycle::stopped ||
                                      (!allow_failed && lifecycle == Lifecycle::failed);
                if (rejected) {
                    return result_for(lifecycle == Lifecycle::failed
                                          ? WorkspaceOwnershipStatus::owner_failed
                                          : WorkspaceOwnershipStatus::stopped);
                }

                auto operation_holder =
                    std::make_shared<std::decay_t<Operation>>(std::forward<Operation>(operation));
                auto promise_holder =
                    std::make_shared<std::promise<WorkspaceOwnershipResult>>(std::move(promise));
                if (test_hooks_ && test_hooks_->before_request_enqueue)
                    test_hooks_->before_request_enqueue();
                requests_.push_back(Request{
                    [this, operation_holder, promise_holder, allow_failed]() mutable {
                        try {
                            promise_holder->set_value(execute(*operation_holder, allow_failed));
                        } catch (...) {
                            mark_failure();
                            try {
                                promise_holder->set_value(result_for(
                                    WorkspaceOwnershipStatus::owner_failed));
                            } catch (...) {
                            }
                        }
                    },
                    [promise_holder]() mutable {
                        try {
                            promise_holder->set_value(
                                result_for(WorkspaceOwnershipStatus::owner_failed));
                        } catch (...) {
                        }
                    }});
            }
            queue_cv_.notify_one();
            return future.get();
        } catch (...) {
            mark_failure();
            return result_for(WorkspaceOwnershipStatus::owner_failed);
        }
    }

    template <typename Operation>
    [[nodiscard]] WorkspaceOwnershipResult execute(Operation&& operation, bool allow_failed) {
        // Shutdown drains already-enqueued work under Lifecycle::stopping.
        // That lifecycle must not mask a fault raised by an earlier request.
        if (!allow_failed && failure_seen_.load())
            return result_for(WorkspaceOwnershipStatus::owner_failed);
        try {
            return std::forward<Operation>(operation)();
        } catch (...) {
            mark_failure();
            return result_for(WorkspaceOwnershipStatus::owner_failed);
        }
    }

    [[nodiscard]] bool is_owner_thread() const {
        std::lock_guard lock(queue_mutex_);
        return owner_thread_id_ != std::thread::id{} &&
               owner_thread_id_ == std::this_thread::get_id();
    }

    void mark_failure() noexcept {
        failure_seen_.store(true);
        std::lock_guard lock(queue_mutex_);
        if (lifecycle_.load() == Lifecycle::running)
            lifecycle_.store(Lifecycle::failed);
    }

    void owner_loop() noexcept {
        {
            std::lock_guard lock(queue_mutex_);
            owner_thread_id_ = std::this_thread::get_id();
        }
        queue_cv_.notify_all();

        try {
            for (;;) {
                if (test_hooks_ && test_hooks_->before_owner_wait)
                    test_hooks_->before_owner_wait();
                Request request;
                bool have_request = false;
                bool run_shutdown = false;
                bool terminal_shutdown = false;
                {
                    std::unique_lock lock(queue_mutex_);
                    queue_cv_.wait(lock, [this] {
                        return shutdown_requested_ || !requests_.empty();
                    });
                    if (!requests_.empty()) {
                        request = std::move(requests_.front());
                        requests_.pop_front();
                        have_request = true;
                    } else if (shutdown_requested_) {
                        shutdown_requested_ = false;
                        run_shutdown = true;
                        terminal_shutdown = terminal_shutdown_requested_;
                    }
                }

                if (have_request) {
                    try {
                        request.run();
                    } catch (...) {
                        mark_failure();
                        try {
                            request.fail();
                        } catch (...) {
                        }
                    }
                    continue;
                }

                if (run_shutdown) {
                    const auto cleanup = cleanup_all_on_owner();
                    const auto stop = cleanup.complete || terminal_shutdown;
                    {
                        std::lock_guard lock(queue_mutex_);
                        shutdown_attempt_status_ = cleanup.status;
                        shutdown_attempt_complete_ = true;
                        if (stop) {
                            lifecycle_.store(Lifecycle::stopped);
                            owner_thread_id_ = {};
                        } else {
                            lifecycle_.store(Lifecycle::failed);
                        }
                    }
                    queue_cv_.notify_all();
                    if (stop) return;
                }
            }
        } catch (...) {
            mark_failure();
            {
                std::lock_guard lock(queue_mutex_);
                lifecycle_.store(Lifecycle::stopping);
                shutdown_attempt_complete_ = false;
                shutdown_requested_ = false;
                terminal_shutdown_requested_ = true;
            }
            for (;;) {
                Request pending;
                {
                    std::lock_guard lock(queue_mutex_);
                    if (requests_.empty()) break;
                    pending = std::move(requests_.front());
                    requests_.pop_front();
                }
                try {
                    pending.fail();
                } catch (...) {
                }
            }
            // The unexpected failure is already on the owner thread. Attempt
            // exact cleanup here before publishing the terminal state.
            const auto cleanup = cleanup_all_on_owner();
            (void)cleanup;
            {
                std::lock_guard lock(queue_mutex_);
                shutdown_attempt_status_ = WorkspaceOwnershipStatus::owner_failed;
                shutdown_attempt_complete_ = true;
                lifecycle_.store(Lifecycle::stopped);
                owner_thread_id_ = {};
            }
            queue_cv_.notify_all();
        }
    }

    [[nodiscard]] std::string mutex_name(const WorkspaceOwnershipKey& key) const {
        std::string name = "Global\\PropertyStudio.Owner.";
        name += key.kind() == WorkspaceOwnershipKey::Kind::path ? "Path." : "File.";
        name += key.canonical_sha256();
        return name;
    }

    [[nodiscard]] WorkspaceOwnershipResult acquire_on_owner(
        const WorkspaceInstanceId& workspace, const WorkspaceOwnershipBundle& bundle) {
        if (!workspace.valid()) return result_for(WorkspaceOwnershipStatus::invalid_workspace);
        if (bundle.empty()) return result_for(WorkspaceOwnershipStatus::invalid_bundle);
        if (used_workspace_ids_.contains(workspace.value_))
            return result_for(WorkspaceOwnershipStatus::duplicate_workspace);
        for (const auto& key : bundle.keys()) {
            if (owners_.contains(key)) return result_for(WorkspaceOwnershipStatus::conflict);
        }

        std::vector<HeldKey> fresh;
        bool abandoned = false;
        const auto lock_status = lock_new_keys(bundle.keys(), fresh, abandoned);
        if (lock_status != WorkspaceOwnershipStatus::success)
            return result_for(lock_status, {}, abandoned);

        const auto reservation_id = next_reservation_id_;
        try {
            if (test_hooks_ && test_hooks_->before_reservation_registration)
                test_hooks_->before_reservation_registration();
            std::vector<WorkspaceOwnershipKey> inserted;
            inserted.reserve(fresh.size());
            ReservationRecord record;
            record.workspace_id = workspace.value_;
            for (const auto& held : fresh)
                record.keys.emplace(held.key, HeldHandle{held.handle, true});
            record.abandonment_observed = abandoned;

            auto [record_it, inserted_record] =
                reservations_.emplace(reservation_id, std::move(record));
            if (!inserted_record) throw std::runtime_error("reservation ID collision");
            try {
                for (const auto& held : fresh) {
                    const auto [owner_it, inserted_owner] = owners_.emplace(held.key, reservation_id);
                    (void)owner_it;
                    if (!inserted_owner) throw std::runtime_error("ownership registry collision");
                    inserted.push_back(held.key);
                }
                used_workspace_ids_.insert(workspace.value_);
            } catch (...) {
                for (const auto& key : inserted) owners_.erase(key);
                reservations_.erase(record_it);
                throw;
            }
            ++next_reservation_id_;
            const WorkspaceOwnershipReservation reservation(broker_id_, reservation_id,
                                                             workspace.value_);
            fresh.clear();
            return result_for(WorkspaceOwnershipStatus::success, reservation, abandoned);
        } catch (...) {
            mark_failure();
            (void)rollback(fresh);
            return result_for(WorkspaceOwnershipStatus::backend_failure, {}, abandoned);
        }
    }

    [[nodiscard]] WorkspaceOwnershipResult ensure_on_owner(
        const WorkspaceOwnershipReservation& reservation,
        const WorkspaceOwnershipBundle& bundle) {
        auto record_it = reservations_.find(reservation.reservation_id_);
        if (!reservation_matches(reservation, record_it))
            return result_for(WorkspaceOwnershipStatus::invalid_reservation);
        if (bundle.empty())
            return result_for(WorkspaceOwnershipStatus::success, {},
                              record_it->second.abandonment_observed);

        auto& record = record_it->second;
        std::vector<WorkspaceOwnershipKey> new_keys;
        new_keys.reserve(bundle.keys().size());
        for (const auto& key : bundle.keys()) {
            const auto record_key = record.keys.find(key);
            if (record_key != record.keys.end()) {
                const auto owner = owners_.find(key);
                if (owner == owners_.end() || owner->second != reservation.reservation_id_)
                    return result_for(WorkspaceOwnershipStatus::invalid_reservation);
                continue;
            }
            if (owners_.contains(key)) return result_for(WorkspaceOwnershipStatus::conflict);
            new_keys.push_back(key);
        }

        if (new_keys.empty())
            return result_for(WorkspaceOwnershipStatus::success, {},
                              record.abandonment_observed);

        std::vector<HeldKey> fresh;
        bool abandoned = false;
        const auto lock_status = lock_new_keys(new_keys, fresh, abandoned);
        // The kernel observation survives any later registry allocation or
        // rollback failure, even when no new reservation key is published.
        record.abandonment_observed = record.abandonment_observed || abandoned;
        if (lock_status != WorkspaceOwnershipStatus::success) {
            return result_for(lock_status, {}, record.abandonment_observed);
        }

        std::vector<WorkspaceOwnershipKey> record_inserted;
        std::vector<WorkspaceOwnershipKey> owner_inserted;
        try {
            if (test_hooks_ && test_hooks_->before_reservation_registration)
                test_hooks_->before_reservation_registration();
            record_inserted.reserve(fresh.size());
            owner_inserted.reserve(fresh.size());
            for (const auto& held : fresh) {
                const auto [key_it, inserted_key] =
                    record.keys.emplace(held.key, HeldHandle{held.handle, true});
                (void)key_it;
                if (!inserted_key) throw std::runtime_error("reservation key collision");
                record_inserted.push_back(held.key);
            }
            for (const auto& held : fresh) {
                const auto [owner_it, inserted_owner] = owners_.emplace(
                    held.key, reservation.reservation_id_);
                (void)owner_it;
                if (!inserted_owner) throw std::runtime_error("ownership registry collision");
                owner_inserted.push_back(held.key);
            }
            fresh.clear();
            return result_for(WorkspaceOwnershipStatus::success, {},
                              record.abandonment_observed);
        } catch (...) {
            for (const auto& key : owner_inserted) owners_.erase(key);
            for (const auto& key : record_inserted) record.keys.erase(key);
            mark_failure();
            (void)rollback(fresh);
            return result_for(WorkspaceOwnershipStatus::backend_failure,
                              {}, record.abandonment_observed);
        }
    }

    [[nodiscard]] WorkspaceOwnershipResult release_keys_on_owner(
        const WorkspaceOwnershipReservation& reservation,
        const WorkspaceOwnershipBundle& bundle) {
        auto record_it = reservations_.find(reservation.reservation_id_);
        if (!reservation_matches(reservation, record_it))
            return result_for(WorkspaceOwnershipStatus::invalid_reservation);
        auto& record = record_it->second;
        if (record.requires_full_release)
            return result_for(WorkspaceOwnershipStatus::owner_failed, {},
                              record.abandonment_observed);
        for (const auto& key : bundle.keys()) {
            const auto record_key = record.keys.find(key);
            const auto owner = owners_.find(key);
            if (record_key == record.keys.end() || owner == owners_.end() ||
                owner->second != reservation.reservation_id_ ||
                !record_key->second.mutex_owned)
                return result_for(WorkspaceOwnershipStatus::key_not_owned);
        }

        bool failed = false;
        for (const auto& key : bundle.keys()) {
            const auto record_key = record.keys.find(key);
            if (record_key == record.keys.end()) continue;
            const auto cleaned = cleanup_handle(record_key->second);
            if (!record_key->second.mutex_owned) owners_.erase(key);
            if (cleaned) {
                record.keys.erase(key);
            } else {
                failed = true;
            }
        }
        if (failed) {
            record.requires_full_release = true;
            return result_for(WorkspaceOwnershipStatus::backend_failure, {},
                              record.abandonment_observed);
        }
        return result_for(WorkspaceOwnershipStatus::success, {}, record.abandonment_observed);
    }

    [[nodiscard]] WorkspaceOwnershipResult release_on_owner(
        const WorkspaceOwnershipReservation& reservation) {
        auto record_it = reservations_.find(reservation.reservation_id_);
        if (!reservation_matches(reservation, record_it))
            return result_for(WorkspaceOwnershipStatus::invalid_reservation);
        auto& record = record_it->second;
        bool failed = false;
        for (auto record_key = record.keys.begin(); record_key != record.keys.end();) {
            const auto key = record_key->first;
            const auto cleaned = cleanup_handle(record_key->second);
            if (!record_key->second.mutex_owned) owners_.erase(key);
            if (cleaned) {
                record_key = record.keys.erase(record_key);
            } else {
                failed = true;
                ++record_key;
            }
        }
        if (record.keys.empty()) {
            const auto abandoned = record.abandonment_observed;
            reservations_.erase(record_it);
            return result_for(WorkspaceOwnershipStatus::success, {}, abandoned);
        }
        if (failed) record.requires_full_release = true;
        return result_for(WorkspaceOwnershipStatus::backend_failure, {},
                          record.abandonment_observed);
    }

    [[nodiscard]] WorkspaceOwnershipResult observe_abandonment_on_owner(
        const WorkspaceOwnershipReservation& reservation) {
        auto record_it = reservations_.find(reservation.reservation_id_);
        if (!reservation_matches(reservation, record_it))
            return result_for(WorkspaceOwnershipStatus::invalid_reservation);
        return result_for(WorkspaceOwnershipStatus::success, {},
                          record_it->second.abandonment_observed);
    }

    [[nodiscard]] CleanupOutcome cleanup_all_on_owner() noexcept {
        bool failed = false;
        for (auto reservation = reservations_.begin(); reservation != reservations_.end();) {
            auto& record = reservation->second;
            for (auto key = record.keys.begin(); key != record.keys.end();) {
                const auto owned_key = key->first;
                const auto cleaned = cleanup_handle(key->second);
                if (!key->second.mutex_owned) owners_.erase(owned_key);
                if (cleaned) {
                    key = record.keys.erase(key);
                } else {
                    failed = true;
                    ++key;
                }
            }
            if (record.keys.empty()) {
                reservation = reservations_.erase(reservation);
            } else {
                record.requires_full_release = true;
                ++reservation;
            }
        }

        for (auto held = cleanup_obligations_.begin();
             held != cleanup_obligations_.end();) {
            if (cleanup_handle(*held)) {
                held = cleanup_obligations_.erase(held);
            } else {
                failed = true;
                ++held;
            }
        }

        const auto complete = reservations_.empty() && cleanup_obligations_.empty();
        if (complete) owners_.clear();
        return CleanupOutcome{
            complete,
            (failed || failure_seen_.load()) ? WorkspaceOwnershipStatus::owner_failed
                                             : WorkspaceOwnershipStatus::success};
    }

    [[nodiscard]] bool finalize_after_owner_exit() noexcept {
        bool complete = true;
        owners_.clear();
        for (auto reservation = reservations_.begin(); reservation != reservations_.end();) {
            auto& record = reservation->second;
            for (auto key = record.keys.begin(); key != record.keys.end();) {
                auto& held = key->second;
                // Owner-thread termination abandons any still-owned mutex. The
                // joining thread performs close only; it never calls release.
                held.mutex_owned = false;
                try {
                    backend_->close(held.handle);
                    held.handle = 0;
                    key = record.keys.erase(key);
                } catch (...) {
                    failure_seen_.store(true);
                    complete = false;
                    ++key;
                }
            }
            if (record.keys.empty()) {
                reservation = reservations_.erase(reservation);
            } else {
                ++reservation;
            }
        }

        for (auto held = cleanup_obligations_.begin();
             held != cleanup_obligations_.end();) {
            held->mutex_owned = false;
            try {
                backend_->close(held->handle);
                held = cleanup_obligations_.erase(held);
            } catch (...) {
                failure_seen_.store(true);
                complete = false;
                ++held;
            }
        }
        return complete && reservations_.empty() && cleanup_obligations_.empty();
    }

    [[nodiscard]] bool reservation_matches(
        const WorkspaceOwnershipReservation& reservation,
        std::map<std::uint64_t, ReservationRecord>::iterator record_it) const noexcept {
        return reservation.valid() && reservation.broker_id_ == broker_id_ &&
               record_it != reservations_.end() &&
               record_it->second.workspace_id == reservation.workspace_id_;
    }

    [[nodiscard]] WorkspaceOwnershipStatus lock_new_keys(
        const std::vector<WorkspaceOwnershipKey>& keys, std::vector<HeldKey>& fresh,
        bool& abandoned) {
        // All storage needed to retain failed rollback obligations is obtained
        // before the first OS handle exists. HeldHandle is trivially movable,
        // so push_back cannot throw while this reserved capacity remains.
        try {
            if (keys.size() > cleanup_obligations_.max_size() -
                                  cleanup_obligations_.size())
                throw std::length_error("ownership cleanup capacity exhausted");
            cleanup_obligations_.reserve(cleanup_obligations_.size() + keys.size());
            fresh.reserve(keys.size());
        } catch (...) {
            mark_failure();
            return WorkspaceOwnershipStatus::backend_failure;
        }

        for (const auto& key : keys) {
            WorkspaceOwnershipBackend::Handle handle = 0;
            bool retained = false;
            bool owns_handle = false;
            try {
                handle = backend_->create(mutex_name(key));
                if (handle == 0) throw std::runtime_error("ownership backend returned null handle");
                const auto wait = backend_->wait(handle, 0);
                switch (wait) {
                case WorkspaceOwnershipBackend::WaitResult::acquired:
                    owns_handle = true;
                    break;
                case WorkspaceOwnershipBackend::WaitResult::abandoned:
                    owns_handle = true;
                    abandoned = true;
                    break;
                case WorkspaceOwnershipBackend::WaitResult::timeout: {
                    HeldHandle pending{handle, false};
                    bool cleanup_ok = cleanup_handle(pending);
                    if (!cleanup_ok) retain_cleanup_obligation(pending);
                    cleanup_ok = rollback(fresh) && cleanup_ok;
                    return cleanup_ok ? WorkspaceOwnershipStatus::conflict
                                      : WorkspaceOwnershipStatus::backend_failure;
                }
                case WorkspaceOwnershipBackend::WaitResult::failed: {
                    mark_failure();
                    HeldHandle pending{handle, false};
                    bool cleanup_ok = cleanup_handle(pending);
                    if (!cleanup_ok) retain_cleanup_obligation(pending);
                    cleanup_ok = rollback(fresh) && cleanup_ok;
                    (void)cleanup_ok;
                    return WorkspaceOwnershipStatus::backend_failure;
                }
                }
                if (test_hooks_ && test_hooks_->after_wait_acquired)
                    test_hooks_->after_wait_acquired();
                fresh.push_back(HeldKey{key, handle});
                retained = true;
            } catch (...) {
                mark_failure();
                if (handle != 0 && !retained) {
                    HeldHandle pending{handle, owns_handle};
                    if (!cleanup_handle(pending)) retain_cleanup_obligation(pending);
                }
                (void)rollback(fresh);
                return WorkspaceOwnershipStatus::backend_failure;
            }
        }
        return WorkspaceOwnershipStatus::success;
    }

    [[nodiscard]] bool rollback(std::vector<HeldKey>& fresh) noexcept {
        bool success = true;
        for (auto held = fresh.rbegin(); held != fresh.rend(); ++held) {
            HeldHandle pending{held->handle, true};
            if (!cleanup_handle(pending)) {
                retain_cleanup_obligation(pending);
                success = false;
            }
        }
        fresh.clear();
        return success;
    }

    [[nodiscard]] bool cleanup_handle(HeldHandle& held) noexcept {
        if (held.handle == 0) return true;
        if (held.mutex_owned) {
            try {
                backend_->release(held.handle);
                held.mutex_owned = false;
            } catch (...) {
                mark_failure();
                return false;
            }
        }
        try {
            backend_->close(held.handle);
            held.handle = 0;
            return true;
        } catch (...) {
            mark_failure();
            return false;
        }
    }

    void retain_cleanup_obligation(const HeldHandle& held) noexcept {
        if (held.handle == 0) return;
        // lock_new_keys() reserves one slot for every handle that can be added
        // by the current operation before creating any of those handles.
        cleanup_obligations_.push_back(held);
    }

    std::unique_ptr<WorkspaceOwnershipBackend> backend_;
    const std::uint64_t broker_id_;
    const std::optional<WorkspaceOwnershipTestHooks> test_hooks_;
    std::atomic<Lifecycle> lifecycle_{Lifecycle::running};
    std::atomic<bool> failure_seen_{false};

    mutable std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::deque<Request> requests_;
    std::thread::id owner_thread_id_;
    std::thread owner_thread_;
    bool shutdown_requested_ = false;
    bool terminal_shutdown_requested_ = false;
    bool shutdown_attempt_complete_ = false;
    WorkspaceOwnershipStatus shutdown_attempt_status_ =
        WorkspaceOwnershipStatus::owner_failed;

    std::mutex shutdown_mutex_;
    std::optional<WorkspaceOwnershipStatus> completed_shutdown_status_;

    std::uint64_t next_reservation_id_ = 1;
    std::map<std::uint64_t, ReservationRecord> reservations_;
    std::map<WorkspaceOwnershipKey, std::uint64_t> owners_;
    std::set<std::uint64_t> used_workspace_ids_;
    std::vector<HeldHandle> cleanup_obligations_;
};

}  // namespace detail

WorkspaceOwnershipBroker& WorkspaceOwnershipBroker::instance() {
    static WorkspaceOwnershipBroker broker(default_backend(), true, {});
    return broker;
}

std::unique_ptr<WorkspaceOwnershipBroker> WorkspaceOwnershipBroker::create_for_testing(
    std::unique_ptr<WorkspaceOwnershipBackend> backend,
    WorkspaceOwnershipTestHooks hooks) {
    return std::unique_ptr<WorkspaceOwnershipBroker>(
        new WorkspaceOwnershipBroker(std::move(backend), false, std::move(hooks)));
}

WorkspaceOwnershipBroker::WorkspaceOwnershipBroker(
    std::unique_ptr<WorkspaceOwnershipBackend> backend, bool production,
    WorkspaceOwnershipTestHooks hooks)
    : impl_(std::make_unique<detail::WorkspaceOwnershipBrokerImpl>(
          std::move(backend),
          production ? std::optional<WorkspaceOwnershipTestHooks>{}
                     : std::optional<WorkspaceOwnershipTestHooks>{std::move(hooks)})) {}

WorkspaceOwnershipBroker::~WorkspaceOwnershipBroker() = default;

std::optional<WorkspaceInstanceId> WorkspaceOwnershipBroker::new_workspace_instance() {
    return impl_->new_workspace_instance();
}

WorkspaceOwnershipResult WorkspaceOwnershipBroker::acquire(
    const WorkspaceInstanceId& workspace, const WorkspaceOwnershipBundle& bundle) {
    return impl_->acquire(workspace, bundle);
}

WorkspaceOwnershipResult WorkspaceOwnershipBroker::ensure(
    const WorkspaceOwnershipReservation& reservation, const WorkspaceOwnershipBundle& bundle) {
    return impl_->ensure(reservation, bundle);
}

WorkspaceOwnershipResult WorkspaceOwnershipBroker::release_keys(
    const WorkspaceOwnershipReservation& reservation, const WorkspaceOwnershipBundle& bundle) {
    return impl_->release_keys(reservation, bundle);
}

WorkspaceOwnershipResult WorkspaceOwnershipBroker::release(
    const WorkspaceOwnershipReservation& reservation) {
    return impl_->release(reservation);
}

WorkspaceOwnershipResult WorkspaceOwnershipBroker::observe_abandonment(
    const WorkspaceOwnershipReservation& reservation) {
    return impl_->observe_abandonment(reservation);
}

WorkspaceOwnershipResult WorkspaceOwnershipBroker::shutdown() {
    return impl_->shutdown_and_join();
}

}  // namespace sketch
