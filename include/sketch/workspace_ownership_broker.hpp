#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sketch {

namespace detail {
class WorkspaceOwnershipBrokerImpl;
}

// A lowercase, exactly 64-character SHA-256 digest. The broker accepts only
// canonical digests; it never normalizes or resolves paths itself.
class CanonicalSha256 final {
public:
    static std::optional<CanonicalSha256> parse(std::string_view value) noexcept;
    static CanonicalSha256 from_hex(std::string_view value);

    [[nodiscard]] std::string hex() const;

    friend bool operator==(const CanonicalSha256&, const CanonicalSha256&) = default;
    friend bool operator<(const CanonicalSha256&, const CanonicalSha256&) noexcept;

private:
    explicit CanonicalSha256(std::array<std::uint8_t, 32> bytes) noexcept;

    std::array<std::uint8_t, 32> bytes_{};
};

class WorkspaceOwnershipKey final {
public:
    enum class Kind : std::uint8_t { path, file };

    // These factories are for digests produced by the future handle-based
    // resolver. An arbitrary caller-provided path string or hash is not proof
    // of path ownership and must never be treated as one.
    [[nodiscard]] static WorkspaceOwnershipKey path(CanonicalSha256 digest);
    [[nodiscard]] static WorkspaceOwnershipKey file(CanonicalSha256 digest);

    [[nodiscard]] Kind kind() const noexcept;
    [[nodiscard]] const CanonicalSha256& digest() const noexcept;
    [[nodiscard]] std::string canonical_sha256() const;

    friend bool operator==(const WorkspaceOwnershipKey&, const WorkspaceOwnershipKey&) = default;
    friend bool operator<(const WorkspaceOwnershipKey&, const WorkspaceOwnershipKey&) noexcept;

private:
    WorkspaceOwnershipKey(Kind kind, CanonicalSha256 digest) noexcept;

    Kind kind_;
    CanonicalSha256 digest_;
};

class WorkspaceOwnershipBundle final {
public:
    WorkspaceOwnershipBundle() = default;
    explicit WorkspaceOwnershipBundle(std::vector<WorkspaceOwnershipKey> keys);

    [[nodiscard]] const std::vector<WorkspaceOwnershipKey>& keys() const noexcept;
    [[nodiscard]] bool empty() const noexcept;

private:
    std::vector<WorkspaceOwnershipKey> keys_;
};

// Opaque process-local identity allocated by the broker. A workspace instance
// can acquire at most one lifetime reservation; create a new instance for an
// independent workspace after the old reservation is released.
class WorkspaceInstanceId final {
public:
    WorkspaceInstanceId() = default;
    [[nodiscard]] bool valid() const noexcept;

    friend bool operator==(const WorkspaceInstanceId&, const WorkspaceInstanceId&) = default;

private:
    explicit WorkspaceInstanceId(std::uint64_t value) noexcept;

    std::uint64_t value_ = 0;
    friend class WorkspaceOwnershipBroker;
    friend class detail::WorkspaceOwnershipBrokerImpl;
};

// Opaque reservation identity. Native mutex handles are held only by the
// broker's owner thread and are never exposed through this type.
class WorkspaceOwnershipReservation final {
public:
    WorkspaceOwnershipReservation() = default;
    [[nodiscard]] bool valid() const noexcept;

    friend bool operator==(const WorkspaceOwnershipReservation&,
                           const WorkspaceOwnershipReservation&) = default;

private:
    WorkspaceOwnershipReservation(std::uint64_t broker_id, std::uint64_t reservation_id,
                                  std::uint64_t workspace_id) noexcept;

    std::uint64_t broker_id_ = 0;
    std::uint64_t reservation_id_ = 0;
    std::uint64_t workspace_id_ = 0;
    friend class WorkspaceOwnershipBroker;
    friend class detail::WorkspaceOwnershipBrokerImpl;
};

// This interface exists solely for deterministic broker tests. Production
// callers use WorkspaceOwnershipBroker::instance() and never see a Handle.
class WorkspaceOwnershipBackend {
public:
    using Handle = std::uintptr_t;

    enum class WaitResult : std::uint8_t { acquired, abandoned, timeout, failed };

    virtual ~WorkspaceOwnershipBackend() = default;
    [[nodiscard]] virtual Handle create(std::string_view mutex_name) = 0;
    [[nodiscard]] virtual WaitResult wait(Handle handle, std::uint32_t timeout_ms) = 0;
    virtual void release(Handle handle) = 0;
    virtual void close(Handle handle) = 0;
};

// Fault-injection points used only by create_for_testing(). Production brokers
// never execute these callbacks.
struct WorkspaceOwnershipTestHooks final {
    std::function<void()> before_owner_wait;
    std::function<void()> before_request_enqueue;
    std::function<void()> after_wait_acquired;
    std::function<void()> before_reservation_registration;
};

enum class WorkspaceOwnershipStatus : std::uint8_t {
    success,
    invalid_workspace,
    invalid_bundle,
    invalid_reservation,
    duplicate_workspace,
    conflict,
    key_not_owned,
    backend_failure,
    owner_failed,
    reentrant_shutdown,
    stopped,
};

struct WorkspaceOwnershipResult final {
    WorkspaceOwnershipStatus status = WorkspaceOwnershipStatus::backend_failure;
    std::string message;
    std::optional<WorkspaceOwnershipReservation> reservation;
    bool abandonment_observed = false;

    [[nodiscard]] bool ok() const noexcept {
        return status == WorkspaceOwnershipStatus::success;
    }
};

class WorkspaceOwnershipBroker final {
public:
    // The one production broker owns every lifetime mutex operation on its
    // joinable dedicated owner thread. The singleton cannot be replaced by a
    // second production broker.
    [[nodiscard]] static WorkspaceOwnershipBroker& instance();

    // Tests inject a backend so owner-thread affinity and rollback can be
    // checked without exposing native handles to application code.
    [[nodiscard]] static std::unique_ptr<WorkspaceOwnershipBroker>
    create_for_testing(std::unique_ptr<WorkspaceOwnershipBackend> backend,
                       WorkspaceOwnershipTestHooks hooks = {});

    WorkspaceOwnershipBroker(const WorkspaceOwnershipBroker&) = delete;
    WorkspaceOwnershipBroker& operator=(const WorkspaceOwnershipBroker&) = delete;
    WorkspaceOwnershipBroker(WorkspaceOwnershipBroker&&) = delete;
    WorkspaceOwnershipBroker& operator=(WorkspaceOwnershipBroker&&) = delete;
    ~WorkspaceOwnershipBroker();

    [[nodiscard]] std::optional<WorkspaceInstanceId> new_workspace_instance();

    [[nodiscard]] WorkspaceOwnershipResult acquire(const WorkspaceInstanceId& workspace,
                                                    const WorkspaceOwnershipBundle& bundle);
    [[nodiscard]] WorkspaceOwnershipResult ensure(
        const WorkspaceOwnershipReservation& reservation,
        const WorkspaceOwnershipBundle& bundle);
    // A partial backend failure fails the broker closed. The reservation keeps
    // every remaining owned or close-pending handle, and only release() may be
    // used to retry complete reservation cleanup afterward.
    [[nodiscard]] WorkspaceOwnershipResult release_keys(
        const WorkspaceOwnershipReservation& reservation,
        const WorkspaceOwnershipBundle& bundle);
    [[nodiscard]] WorkspaceOwnershipResult release(
        const WorkspaceOwnershipReservation& reservation);
    // Pure query for the reservation's sticky WAIT_ABANDONED observation. This
    // call never creates or acknowledges an abandonment observation.
    [[nodiscard]] WorkspaceOwnershipResult observe_abandonment(
        const WorkspaceOwnershipReservation& reservation);

    // Shutdown rejects new acquisitions and ensures, drains already queued
    // requests, releases all remaining owned handles on the owner thread, and
    // joins that thread once cleanup succeeds. A failed cleanup keeps the owner
    // thread and exact handle state available for a later shutdown or release
    // retry. External calls are serialized and idempotent. A call from the
    // owner thread is rejected so an external caller can perform the join.
    // Destruction uses an allocation-free terminal stop: cleanup is attempted
    // on the owner thread, then that thread is joined. A mutex whose release
    // persistently fails is abandoned by owner-thread exit and only its handle
    // is closed by the joining thread; the joining thread never releases it.
    // Close failures remain tracked and owner_failed through public retries;
    // terminal destruction makes one final close attempt after the join.
    // Lifecycle transitions are running -> failed -> stopping -> stopped (with
    // running -> stopping allowed), and stop states are published only while
    // holding the request-queue mutex.
    [[nodiscard]] WorkspaceOwnershipResult shutdown();

private:
    explicit WorkspaceOwnershipBroker(std::unique_ptr<WorkspaceOwnershipBackend> backend,
                                      bool production,
                                      WorkspaceOwnershipTestHooks hooks);

    std::unique_ptr<detail::WorkspaceOwnershipBrokerImpl> impl_;
};

}  // namespace sketch
