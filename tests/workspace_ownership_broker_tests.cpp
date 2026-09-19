#include "sketch/workspace_ownership_broker.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <future>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using sketch::CanonicalSha256;
using sketch::WorkspaceOwnershipBackend;
using sketch::WorkspaceOwnershipBroker;
using sketch::WorkspaceOwnershipBundle;
using sketch::WorkspaceOwnershipKey;
using sketch::WorkspaceOwnershipResult;
using sketch::WorkspaceOwnershipStatus;
using sketch::WorkspaceOwnershipTestHooks;
using sketch::WorkspaceInstanceId;
using sketch::WorkspaceOwnershipReservation;

[[noreturn]] void fail(std::string_view message) {
    throw std::runtime_error(std::string(message));
}

void require(bool condition, std::string_view message) {
    if (!condition) fail(message);
}

template <typename Actual, typename Expected>
void require_equal(const Actual& actual, const Expected& expected, std::string_view message) {
    if (!(actual == expected)) fail(message);
}

CanonicalSha256 digest(char last) {
    std::string value(64, '0');
    value.back() = last;
    return CanonicalSha256::from_hex(value);
}

WorkspaceOwnershipKey path_key(char last) {
    return WorkspaceOwnershipKey::path(digest(last));
}

WorkspaceOwnershipKey file_key(char last) {
    return WorkspaceOwnershipKey::file(digest(last));
}

WorkspaceOwnershipBundle bundle(std::initializer_list<WorkspaceOwnershipKey> keys) {
    return WorkspaceOwnershipBundle(std::vector<WorkspaceOwnershipKey>(keys));
}

struct BackendObservation {
    std::atomic<std::size_t> open_handles = 0;
    std::atomic<std::size_t> release_attempts = 0;
    std::atomic<std::size_t> close_attempts = 0;
    std::atomic<std::size_t> terminal_abandonments = 0;
};

class RecordingBackend final : public WorkspaceOwnershipBackend {
public:
    explicit RecordingBackend(std::shared_ptr<BackendObservation> observation = {})
        : observation_(std::move(observation)) {}

    struct Event {
        enum class Kind { create, wait, release, close } kind;
        Handle handle = 0;
        std::string name;
        std::uint32_t timeout_ms = 0;
        std::thread::id thread;
    };

    struct HandleState {
        bool open = true;
        bool owned = false;
        std::thread::id owner_thread;
        std::size_t successful_releases = 0;
        std::size_t successful_closes = 0;
    };

    Handle create(std::string_view name) override {
        const auto handle = next_handle_++;
        events.push_back(Event{Event::Kind::create, handle, std::string(name), 0,
                               std::this_thread::get_id()});
        handles.emplace(handle, HandleState{});
        if (observation_) ++observation_->open_handles;
        return handle;
    }

    WaitResult wait(Handle handle, std::uint32_t timeout_ms) override {
        events.push_back(Event{Event::Kind::wait, handle, {}, timeout_ms,
                               std::this_thread::get_id()});
        auto state = handles.find(handle);
        if (state == handles.end() || !state->second.open)
            throw std::runtime_error("wait used a closed or unknown handle");
        if (state->second.owned)
            throw std::runtime_error("wait recursively acquired an already-owned handle");
        const auto index = wait_count++;
        if (throw_wait_at.has_value() && index == *throw_wait_at)
            throw std::runtime_error("injected wait failure");
        const auto result = wait_plan.empty() ? WaitResult::acquired : wait_plan.front();
        if (!wait_plan.empty()) wait_plan.pop_front();
        if (result == WaitResult::acquired || result == WaitResult::abandoned) {
            state->second.owned = true;
            state->second.owner_thread = std::this_thread::get_id();
        }
        return result;
    }

    void release(Handle handle) override {
        events.push_back(Event{Event::Kind::release, handle, {}, 0,
                               std::this_thread::get_id()});
        if (observation_) ++observation_->release_attempts;
        auto state = handles.find(handle);
        if (state == handles.end() || !state->second.open)
            throw std::runtime_error("release used a closed or unknown handle");
        if (!state->second.owned)
            throw std::runtime_error("release used a handle the owner thread did not acquire");
        const auto index = release_count++;
        if (always_throw_release ||
            (throw_release_at.has_value() && index == *throw_release_at)) {
            throw_release_at.reset();
            throw std::runtime_error("injected release failure");
        }
        state->second.owned = false;
        state->second.owner_thread = {};
        ++state->second.successful_releases;
    }

    void close(Handle handle) override {
        events.push_back(Event{Event::Kind::close, handle, {}, 0,
                               std::this_thread::get_id()});
        if (observation_) ++observation_->close_attempts;
        auto state = handles.find(handle);
        if (state == handles.end() || !state->second.open)
            throw std::runtime_error("close used a closed or unknown handle");
        if (state->second.owned &&
            state->second.owner_thread == std::this_thread::get_id())
            throw std::runtime_error("close discarded an owned mutex without release");
        const auto index = close_count++;
        if (always_throw_close ||
            (throw_close_at.has_value() && index == *throw_close_at)) {
            throw_close_at.reset();
            throw std::runtime_error("injected close failure");
        }
        if (state->second.owned) {
            state->second.owned = false;
            state->second.owner_thread = {};
            if (observation_) ++observation_->terminal_abandonments;
        }
        state->second.open = false;
        ++state->second.successful_closes;
        if (observation_) --observation_->open_handles;
    }

    [[nodiscard]] std::size_t count(Event::Kind kind) const {
        return static_cast<std::size_t>(std::count_if(
            events.begin(), events.end(), [kind](const Event& event) {
                return event.kind == kind;
            }));
    }

    std::vector<Event> events;
    std::map<Handle, HandleState> handles;
    std::deque<WaitResult> wait_plan;
    std::optional<std::size_t> throw_wait_at;
    std::optional<std::size_t> throw_release_at;
    std::optional<std::size_t> throw_close_at;
    bool always_throw_release = false;
    bool always_throw_close = false;

private:
    std::shared_ptr<BackendObservation> observation_;
    Handle next_handle_ = 1;
    std::size_t wait_count = 0;
    std::size_t release_count = 0;
    std::size_t close_count = 0;
};

std::unique_ptr<WorkspaceOwnershipBroker> test_broker(
    RecordingBackend*& backend, WorkspaceOwnershipTestHooks hooks = {},
    std::shared_ptr<BackendObservation> observation = {}) {
    auto injected = std::make_unique<RecordingBackend>(std::move(observation));
    backend = injected.get();
    return WorkspaceOwnershipBroker::create_for_testing(std::move(injected), std::move(hooks));
}

template <typename Result>
Result get_bounded(std::future<Result>& future, std::string_view message) {
    if (future.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
        std::cerr << "workspace_ownership_broker_tests: " << message << '\n';
        std::_Exit(EXIT_FAILURE);
    }
    return future.get();
}

template <typename Operation>
auto run_bounded(Operation&& operation, std::string_view message) {
    auto future = std::async(std::launch::async, std::forward<Operation>(operation));
    return get_bounded(future, message);
}

void require_exact_cleanup(const RecordingBackend& backend,
                           std::initializer_list<WorkspaceOwnershipBackend::Handle> handles) {
    for (const auto handle : handles) {
        const auto state = backend.handles.find(handle);
        require(state != backend.handles.end(), "cleanup assertion used an unknown handle");
        require(!state->second.open, "cleanup must close every created handle");
        require(!state->second.owned, "cleanup must not retain mutex ownership");
        require(state->second.successful_releases == 1,
                "every acquired handle must have exactly one successful release");
        require(state->second.successful_closes == 1,
                "every created handle must have exactly one successful close");
    }
}

WorkspaceInstanceId new_instance(WorkspaceOwnershipBroker& broker) {
    const auto instance = broker.new_workspace_instance();
    require(instance.has_value(), "broker should issue a workspace instance while running");
    return *instance;
}

WorkspaceOwnershipReservation acquire(WorkspaceOwnershipBroker& broker,
                                      const WorkspaceInstanceId& instance,
                                      const WorkspaceOwnershipBundle& keys) {
    const auto result = broker.acquire(instance, keys);
    require(result.ok(), "acquisition should succeed");
    require(result.reservation.has_value(), "successful acquisition should return a reservation");
    return *result.reservation;
}

void require_all_owner_thread(const RecordingBackend& backend) {
    require(!backend.events.empty(), "backend should have observed owner operations");
    const auto owner = backend.events.front().thread;
    for (const auto& event : backend.events)
        require(event.thread == owner, "all backend operations must use one owner thread");
}

void test_digest_and_bundle_are_canonical_and_typed() {
    constexpr std::string_view lower =
        "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789";
    constexpr std::string_view upper =
        "ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789";
    constexpr std::string_view mixed =
        "aBcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789";
    require(CanonicalSha256::parse(lower).has_value(),
            "lowercase SHA256 input should satisfy the canonical contract");
    require(!CanonicalSha256::parse(upper).has_value(),
            "uppercase SHA256 input must not be normalized implicitly");
    require(!CanonicalSha256::parse(mixed).has_value(),
            "mixed-case SHA256 input must not be normalized implicitly");
    bool uppercase_threw = false;
    try {
        (void)CanonicalSha256::from_hex(upper);
    } catch (const std::invalid_argument&) {
        uppercase_threw = true;
    }
    require(uppercase_threw, "throwing SHA256 construction must enforce lowercase input");
    require(!CanonicalSha256::parse("not-a-sha256").has_value(),
            "non SHA256 input should be rejected");

    const auto path = path_key('1');
    const auto file = file_key('1');
    require(path.kind() == WorkspaceOwnershipKey::Kind::path,
            "path factory must retain the path key category");
    require(file.kind() == WorkspaceOwnershipKey::Kind::file,
            "file factory must retain the file key category");
    const auto keys = bundle({file, path, path});
    require(keys.keys().size() == 2, "bundles should deduplicate exact typed keys");
    require(keys.keys().front().kind() == WorkspaceOwnershipKey::Kind::path,
            "bundles should order path keys before file keys");
}

void test_registry_rejects_duplicate_workspace_before_recursive_wait() {
    RecordingBackend* backend = nullptr;
    auto broker = test_broker(backend);
    const auto first = new_instance(*broker);
    const auto second = new_instance(*broker);
    const auto key = bundle({path_key('1')});
    const auto reservation = acquire(*broker, first, key);

    const auto duplicate = broker->acquire(second, key);
    require(duplicate.status == WorkspaceOwnershipStatus::conflict,
            "a second workspace should conflict with an owned key");
    require(backend->count(RecordingBackend::Event::Kind::wait) == 1,
            "registry conflict must be rejected before an OS wait");
    require(broker->release(reservation).ok(), "first workspace should release cleanly");
    const auto later = broker->acquire(second, key);
    require(later.ok(), "a later workspace should acquire after release");
    require(backend->count(RecordingBackend::Event::Kind::wait) == 2,
            "later acquisition should perform exactly one new wait");
}

void test_ensure_is_idempotent_and_orders_new_keys() {
    RecordingBackend* backend = nullptr;
    auto broker = test_broker(backend);
    const auto instance = new_instance(*broker);
    const auto reservation = acquire(*broker, instance, bundle({path_key('1')}));

    require(broker->ensure(reservation, bundle({path_key('1')})).ok(),
            "ensuring an existing key should succeed");
    require(backend->count(RecordingBackend::Event::Kind::wait) == 1,
            "repeated ensure should not recursively wait");

    require(broker->ensure(reservation, bundle({file_key('2'), path_key('2')})).ok(),
            "ensure should add a typed key bundle");
    require(backend->count(RecordingBackend::Event::Kind::wait) == 3,
            "ensure should wait once for each new key");

    const auto waits = std::vector<std::string>{
        backend->events[0].name,
        backend->events[2].name,
        backend->events[4].name,
    };
    require(waits[0].find("Owner.Path.") != std::string::npos,
            "path mutex name should use the owner path namespace");
    require(waits[1].find("Owner.Path.") != std::string::npos,
            "ordered path key should be waited before file key");
    require(waits[2].find("Owner.File.") != std::string::npos,
            "file mutex name should use the owner file namespace");
    require(broker->ensure(reservation, bundle({path_key('1'), path_key('2'), file_key('2')})).ok(),
            "repeating the complete bundle should remain idempotent");
    require(backend->count(RecordingBackend::Event::Kind::wait) == 3,
            "idempotent complete ensure should not add waits");
}

void test_partial_acquisition_rolls_back_only_new_keys() {
    RecordingBackend* backend = nullptr;
    auto broker = test_broker(backend);
    backend->wait_plan = {WorkspaceOwnershipBackend::WaitResult::acquired,
                          WorkspaceOwnershipBackend::WaitResult::timeout};
    const auto instance = new_instance(*broker);
    const auto result = broker->acquire(instance, bundle({path_key('1'), file_key('2')}));
    require(result.status == WorkspaceOwnershipStatus::conflict,
            "a timeout in a bundle should report a conflict");
    require(!result.reservation.has_value(), "failed acquisition should not return a reservation");
    require(backend->count(RecordingBackend::Event::Kind::release) == 1,
            "partial acquisition should release only its acquired key");
    require(backend->count(RecordingBackend::Event::Kind::close) == 2,
            "partial acquisition should close acquired and conflicting handles");

    const auto retry = broker->acquire(instance, bundle({path_key('1')}));
    require(retry.ok(), "rolled back keys should be available for a later acquisition");
}

void test_failed_extension_preserves_original_reservation() {
    RecordingBackend* backend = nullptr;
    auto broker = test_broker(backend);
    const auto instance = new_instance(*broker);
    const auto reservation = acquire(*broker, instance, bundle({path_key('1')}));
    backend->wait_plan = {WorkspaceOwnershipBackend::WaitResult::timeout};

    const auto extension = broker->ensure(reservation, bundle({path_key('1'), file_key('2')}));
    require(extension.status == WorkspaceOwnershipStatus::conflict,
            "a failed extension should report the new-key conflict");
    require(backend->count(RecordingBackend::Event::Kind::release) == 0,
            "a failed extension must not release the original key");
    require(broker->ensure(reservation, bundle({path_key('1')})).ok(),
            "the original reservation should remain valid after failed extension");
    require(backend->count(RecordingBackend::Event::Kind::wait) == 2,
            "failed extension should wait only for the new key");
}

void test_explicit_key_release_preserves_reservation_and_allows_transfer() {
    RecordingBackend* backend = nullptr;
    auto broker = test_broker(backend);
    const auto original_instance = new_instance(*broker);
    const auto transfer_instance = new_instance(*broker);
    const auto reservation = acquire(*broker, original_instance,
                                     bundle({path_key('1'), file_key('2')}));

    require(broker->release_keys(reservation, bundle({path_key('1')})).ok(),
            "validated transfer should release only the selected key");
    const auto transferred = broker->acquire(transfer_instance, bundle({path_key('1')}));
    require(transferred.ok(), "a released key should be available for later acquisition");
    require(broker->ensure(reservation, bundle({file_key('2')})).ok(),
            "the original reservation should retain keys not released for transfer");
    require(broker->release(*transferred.reservation).ok(),
            "the transferred reservation should release cleanly");
    require(broker->release(reservation).ok(), "the original reservation should release cleanly");
    require(backend->count(RecordingBackend::Event::Kind::wait) == 3,
            "transfer should perform one wait for the later owner");
}

void test_backend_exception_fails_closed_after_rollback() {
    RecordingBackend* backend = nullptr;
    auto broker = test_broker(backend);
    const auto instance = new_instance(*broker);
    const auto later_instance = new_instance(*broker);
    const auto reservation = acquire(*broker, instance, bundle({path_key('1')}));
    backend->throw_wait_at = 1;

    const auto extension = broker->ensure(reservation, bundle({file_key('2')}));
    require(extension.status == WorkspaceOwnershipStatus::backend_failure,
            "backend exception should be reported as a backend failure");
    require(backend->handles.at(2).successful_releases == 0,
            "a wait exception before acquisition must not release the new handle");
    require(backend->handles.at(2).successful_closes == 1,
            "a wait exception before acquisition must close the new handle");
    const auto later = broker->acquire(later_instance, bundle({path_key('3')}));
    require(later.status == WorkspaceOwnershipStatus::owner_failed,
            "backend failure must disable later acquisition");
    const auto repeated = broker->ensure(reservation, bundle({path_key('1')}));
    require(repeated.status == WorkspaceOwnershipStatus::owner_failed,
            "backend failure must disable later ensure");
    require(broker->release(reservation).ok(),
            "release remains available to balance a failed broker");
}

void test_wait_failure_after_an_earlier_acquisition_rolls_back_exactly() {
    RecordingBackend* backend = nullptr;
    auto broker = test_broker(backend);
    backend->throw_wait_at = 1;

    const auto result = broker->acquire(new_instance(*broker),
                                        bundle({path_key('1'), file_key('2')}));
    require(result.status == WorkspaceOwnershipStatus::backend_failure,
            "a later wait exception should fail the complete bundle");
    require_exact_cleanup(*backend, {1});
    require(!backend->handles.at(2).open,
            "the handle whose wait threw must be closed");
    require(backend->handles.at(2).successful_releases == 0,
            "the handle whose wait threw must never be released");
    require(backend->handles.at(2).successful_closes == 1,
            "the handle whose wait threw must be closed exactly once");
}

void test_post_acquisition_allocation_and_rollback_failure_remain_tracked() {
    WorkspaceOwnershipTestHooks hooks;
    hooks.after_wait_acquired = [] {
        throw std::bad_alloc();
    };
    RecordingBackend* backend = nullptr;
    auto broker = test_broker(backend, std::move(hooks));
    backend->throw_release_at = 0;

    const auto result = broker->acquire(new_instance(*broker), bundle({path_key('1')}));
    require(result.status == WorkspaceOwnershipStatus::backend_failure,
            "post-acquisition allocation failure should fail closed");
    require(backend->handles.at(1).open && backend->handles.at(1).owned,
            "failed rollback must retain the open owned handle in reserved storage");
    const auto shutdown = run_bounded([&] { return broker->shutdown(); },
                                      "tracked rollback cleanup hung during shutdown");
    require(shutdown.status == WorkspaceOwnershipStatus::owner_failed,
            "successful retry must retain the earlier broker failure status");
    require_exact_cleanup(*backend, {1});
}

void test_release_failure_retains_open_owned_handles_for_full_retry() {
    for (std::size_t failure_position = 0; failure_position < 3; ++failure_position) {
        RecordingBackend* backend = nullptr;
        auto broker = test_broker(backend);
        const auto all_keys = bundle({path_key('1'), path_key('2'), file_key('3')});
        const auto reservation = acquire(*broker, new_instance(*broker), all_keys);
        backend->throw_release_at = failure_position;

        const auto partial = broker->release_keys(reservation, all_keys);
        require(partial.status == WorkspaceOwnershipStatus::backend_failure,
                "a partial key-release failure must fail closed");
        const auto failed_handle = static_cast<WorkspaceOwnershipBackend::Handle>(
            failure_position + 1);
        require(backend->handles.at(failed_handle).open,
                "a release failure must retain the still-owned handle open");
        require(backend->handles.at(failed_handle).owned,
                "a release failure must retain mutex ownership for retry");
        require(broker->release_keys(reservation, bundle({path_key('1')})).status ==
                    WorkspaceOwnershipStatus::owner_failed,
                "partial release failure must require full-reservation cleanup");
        require(broker->release(reservation).ok(),
                "full-reservation cleanup should retry the retained owned handle");
        require_exact_cleanup(*backend, {1, 2, 3});
    }
}

void test_close_failure_retains_only_a_close_obligation_for_full_retry() {
    for (std::size_t failure_position = 0; failure_position < 3; ++failure_position) {
        RecordingBackend* backend = nullptr;
        auto broker = test_broker(backend);
        const auto all_keys = bundle({path_key('1'), path_key('2'), file_key('3')});
        const auto reservation = acquire(*broker, new_instance(*broker), all_keys);
        backend->throw_close_at = failure_position;

        const auto partial = broker->release_keys(reservation, all_keys);
        require(partial.status == WorkspaceOwnershipStatus::backend_failure,
                "a partial key-close failure must fail closed");
        const auto failed_handle = static_cast<WorkspaceOwnershipBackend::Handle>(
            failure_position + 1);
        require(backend->handles.at(failed_handle).open,
                "a failed close must retain an open close obligation");
        require(!backend->handles.at(failed_handle).owned,
                "a failed close must not retain mutex ownership");
        require(broker->release_keys(reservation, all_keys).status ==
                    WorkspaceOwnershipStatus::owner_failed,
                "partial close failure must require full-reservation cleanup");
        require(broker->release(reservation).ok(),
                "full-reservation cleanup should retry close without another release");
        require_exact_cleanup(*backend, {1, 2, 3});
    }
}

void test_full_release_retries_owned_and_close_pending_handles() {
    for (std::size_t failure_position = 0; failure_position < 3; ++failure_position) {
        RecordingBackend* backend = nullptr;
        auto broker = test_broker(backend);
        const auto reservation = acquire(
            *broker, new_instance(*broker),
            bundle({path_key('1'), path_key('2'), file_key('3')}));
        backend->throw_release_at = failure_position;

        require(broker->release(reservation).status ==
                    WorkspaceOwnershipStatus::backend_failure,
                "full release must report an injected release failure");
        const auto failed_handle = static_cast<WorkspaceOwnershipBackend::Handle>(
            failure_position + 1);
        require(backend->handles.at(failed_handle).open &&
                    backend->handles.at(failed_handle).owned,
                "full release must retain a failed owned handle for retry");
        require(broker->release(reservation).ok(),
                "full release should retry its retained owned handle");
        require_exact_cleanup(*backend, {1, 2, 3});
    }

    for (std::size_t failure_position = 0; failure_position < 3; ++failure_position) {
        RecordingBackend* backend = nullptr;
        auto broker = test_broker(backend);
        const auto reservation = acquire(
            *broker, new_instance(*broker),
            bundle({path_key('1'), path_key('2'), file_key('3')}));
        backend->throw_close_at = failure_position;

        require(broker->release(reservation).status ==
                    WorkspaceOwnershipStatus::backend_failure,
                "full release must report an injected close failure");
        const auto failed_handle = static_cast<WorkspaceOwnershipBackend::Handle>(
            failure_position + 1);
        require(backend->handles.at(failed_handle).open &&
                    !backend->handles.at(failed_handle).owned,
                "full release must retain only a close obligation after release");
        require(broker->release(reservation).ok(),
                "full release should retry close without another release");
        require_exact_cleanup(*backend, {1, 2, 3});
    }
}

void test_abandonment_is_a_pure_sticky_kernel_observation() {
    RecordingBackend* backend = nullptr;
    auto broker = test_broker(backend);
    const auto reservation = acquire(*broker, new_instance(*broker), bundle({file_key('1')}));
    const auto initially_observed = broker->observe_abandonment(reservation);
    require(initially_observed.ok() && !initially_observed.abandonment_observed,
            "querying a normal acquisition must not fabricate abandonment");
    const auto repeated_query = broker->observe_abandonment(reservation);
    require(repeated_query.ok() && !repeated_query.abandonment_observed,
            "repeated abandonment queries must remain pure");

    backend->wait_plan = {WorkspaceOwnershipBackend::WaitResult::abandoned};
    const auto observed = broker->ensure(reservation, bundle({file_key('2')}));
    require(observed.status == WorkspaceOwnershipStatus::success,
            "an abandoned extension should still acquire its key");
    require(observed.abandonment_observed,
            "an abandoned wait should be surfaced to the caller");
    const auto sticky = broker->observe_abandonment(reservation);
    require(sticky.ok() && sticky.abandonment_observed,
            "an actual abandoned wait should remain sticky for the reservation");
}

void test_abandonment_survives_a_later_bundle_conflict() {
    RecordingBackend* backend = nullptr;
    auto broker = test_broker(backend);
    const auto reservation = acquire(*broker, new_instance(*broker), bundle({path_key('1')}));
    backend->wait_plan = {WorkspaceOwnershipBackend::WaitResult::abandoned,
                          WorkspaceOwnershipBackend::WaitResult::timeout};

    const auto extension = broker->ensure(reservation,
                                          bundle({path_key('2'), file_key('3')}));
    require(extension.status == WorkspaceOwnershipStatus::conflict,
            "a later bundle timeout should still report conflict");
    require(extension.abandonment_observed,
            "an abandoned early member must be surfaced when a later member conflicts");
    const auto sticky = broker->observe_abandonment(reservation);
    require(sticky.ok() && sticky.abandonment_observed,
            "an abandoned early member must remain sticky after rollback");
    require_exact_cleanup(*backend, {2});
    require(backend->handles.at(3).successful_releases == 0,
            "a conflicting handle was never acquired and must not be released");
    require(backend->handles.at(3).successful_closes == 1,
            "a conflicting handle must be closed exactly once");
}

void test_requester_concurrency_and_thread_affine_cleanup() {
    RecordingBackend* backend = nullptr;
    auto broker = test_broker(backend);
    const auto one = new_instance(*broker);
    const auto two = new_instance(*broker);
    const auto first_key = bundle({path_key('1')});
    const auto second_key = bundle({file_key('2')});
    std::optional<WorkspaceOwnershipResult> first_result;
    std::optional<WorkspaceOwnershipResult> second_result;
    std::mutex results_mutex;
    std::thread first([&] {
        auto result = broker->acquire(one, first_key);
        std::lock_guard lock(results_mutex);
        first_result = std::move(result);
    });
    std::thread second([&] {
        auto result = broker->acquire(two, second_key);
        std::lock_guard lock(results_mutex);
        second_result = std::move(result);
    });
    first.join();
    second.join();
    require(first_result.has_value() && first_result->ok(),
            "first concurrent requester should acquire");
    require(second_result.has_value() && second_result->ok(),
            "second concurrent requester should acquire");
    require_all_owner_thread(*backend);
    require(backend->events.front().timeout_ms == 0 ||
                backend->events.front().kind != RecordingBackend::Event::Kind::wait,
            "backend events should retain operation details");
    for (const auto& event : backend->events) {
        if (event.kind == RecordingBackend::Event::Kind::wait)
            require(event.timeout_ms == 0, "all waits must use a zero timeout");
    }
    require(broker->release(*first_result->reservation).ok(), "first release should succeed");
    require(broker->release(*second_result->reservation).ok(), "second release should succeed");
}

void test_unexpected_owner_stop_allows_bounded_shutdown_and_destruction() {
    std::atomic<std::size_t> owner_waits = 0;
    WorkspaceOwnershipTestHooks stop_hooks;
    stop_hooks.before_owner_wait = [&] {
        if (owner_waits.fetch_add(1) != 0)
            throw std::runtime_error("injected unexpected owner-loop stop");
    };
    RecordingBackend* backend = nullptr;
    auto broker = test_broker(backend, std::move(stop_hooks));
    (void)acquire(*broker, new_instance(*broker), bundle({path_key('1')}));
    const auto shutdown = run_bounded([&] { return broker->shutdown(); },
                                      "shutdown hung after unexpected owner-loop stop");
    require(shutdown.status == WorkspaceOwnershipStatus::owner_failed,
            "unexpected owner-loop stop should fail closed without blocking join");
    require_exact_cleanup(*backend, {1});
    require_all_owner_thread(*backend);
    run_bounded([owned = std::move(broker)]() mutable {
        owned.reset();
        return true;
    }, "destruction hung after joining an unexpectedly stopped owner thread");

    WorkspaceOwnershipTestHooks destructor_hooks;
    destructor_hooks.before_owner_wait = [] {
        throw std::runtime_error("injected owner-loop stop before destruction");
    };
    RecordingBackend* second_backend = nullptr;
    auto destroy_directly = test_broker(second_backend, std::move(destructor_hooks));
    run_bounded([owned = std::move(destroy_directly)]() mutable {
        owned.reset();
        return true;
    }, "destruction hung on a stopped but still-joinable owner thread");
}

void test_concurrent_and_repeated_shutdown_is_idempotent() {
    RecordingBackend* backend = nullptr;
    auto broker = test_broker(backend);
    (void)acquire(*broker, new_instance(*broker), bundle({path_key('1')}));

    auto first = std::async(std::launch::async, [&] { return broker->shutdown(); });
    auto second = std::async(std::launch::async, [&] { return broker->shutdown(); });
    const auto first_result = get_bounded(first, "first concurrent shutdown hung");
    const auto second_result = get_bounded(second, "second concurrent shutdown hung");
    require(first_result.status == WorkspaceOwnershipStatus::success &&
                second_result.status == WorkspaceOwnershipStatus::success,
            "concurrent shutdown callers should receive the same successful result");
    const auto repeated = run_bounded([&] { return broker->shutdown(); },
                                      "repeated shutdown hung after owner join");
    require(repeated.status == WorkspaceOwnershipStatus::success,
            "repeated shutdown should return the completed result");
    require_exact_cleanup(*backend, {1});
}

void test_shutdown_drain_keeps_acquisition_fail_closed() {
    std::promise<void> release_owner;
    const auto owner_gate = release_owner.get_future().share();
    std::atomic<std::size_t> owner_waits = 0;
    std::atomic<bool> track_requests = false;
    std::atomic<std::size_t> queued = 0;
    std::promise<void> queued_first, queued_second, queued_cleanup;
    auto first_ready = queued_first.get_future();
    auto second_ready = queued_second.get_future();
    auto cleanup_ready = queued_cleanup.get_future();
    WorkspaceOwnershipTestHooks hooks;
    hooks.before_owner_wait = [&] {
        // Allow one initial reservation, then stop before any queued work.
        if (owner_waits.fetch_add(1) == 1) owner_gate.wait();
    };
    hooks.before_request_enqueue = [&] {
        if (!track_requests.load()) return;
        const auto index = queued.fetch_add(1);
        if (index == 0) queued_first.set_value();
        if (index == 1) queued_second.set_value();
        if (index == 2) queued_cleanup.set_value();
    };
    RecordingBackend* backend = nullptr;
    auto broker = test_broker(backend, std::move(hooks));
    const auto existing = acquire(*broker, new_instance(*broker), bundle({path_key('1')}));
    const auto first_id = new_instance(*broker);
    const auto second_id = new_instance(*broker);
    backend->wait_plan = {WorkspaceOwnershipBackend::WaitResult::failed};
    track_requests.store(true);
    auto first = std::async(std::launch::async, [&] {
        return broker->acquire(first_id, bundle({path_key('2')}));
    });
    get_bounded(first_ready, "first request was not enqueued");
    auto second = std::async(std::launch::async, [&] {
        return broker->acquire(second_id, bundle({path_key('3')}));
    });
    get_bounded(second_ready, "second request was not enqueued");
    auto cleanup = std::async(std::launch::async, [&] { return broker->release(existing); });
    get_bounded(cleanup_ready, "cleanup request was not enqueued");
    auto shutdown = std::async(std::launch::async, [&] { return broker->shutdown(); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    bool stopping = false;
    while (std::chrono::steady_clock::now() < deadline) {
        // No owner operation can fail while paused. Closing this public
        // admission gate therefore witnesses shutdown's stopping transition.
        if (!broker->new_workspace_instance()) { stopping = true; break; }
        std::this_thread::yield();
    }
    release_owner.set_value();
    const auto failed = get_bounded(first, "first queued acquisition hung");
    const auto refused = get_bounded(second, "second queued acquisition hung");
    const auto released = get_bounded(cleanup, "queued cleanup hung");
    const auto stopped = get_bounded(shutdown, "shutdown drain hung");
    require(stopping, "test must observe shutdown before releasing the owner");
    require(failed.status == WorkspaceOwnershipStatus::backend_failure,
            "first queued acquisition must exercise its injected backend failure");
    require(refused.status == WorkspaceOwnershipStatus::owner_failed && !refused.reservation,
            "shutdown must not mask sticky failure for a later queued acquisition");
    require(released.ok(), "cleanup must still execute after a queued failure during shutdown");
    require(stopped.status == WorkspaceOwnershipStatus::owner_failed,
            "shutdown must retain its failure diagnostic");
    require(backend->count(RecordingBackend::Event::Kind::create) == 2,
            "the refused acquisition must not create or acquire a backend handle");
    require_exact_cleanup(*backend, {1});
    require(backend->handles.at(2).successful_releases == 0 &&
                backend->handles.at(2).successful_closes == 1,
            "the failed wait must be closed without an unowned release");
}

void test_registration_failure_preserves_abandonment() {
    for (const bool extension : {false, true}) {
        std::atomic<bool> fail_registration = false;
        WorkspaceOwnershipTestHooks hooks;
        hooks.before_reservation_registration = [&] {
            if (fail_registration.load()) throw std::bad_alloc();
        };
        RecordingBackend* backend = nullptr;
        auto broker = test_broker(backend, std::move(hooks));
        const auto instance = new_instance(*broker);
        std::optional<WorkspaceOwnershipReservation> reservation;
        if (extension) reservation = acquire(*broker, instance, bundle({path_key('1')}));
        backend->wait_plan = {WorkspaceOwnershipBackend::WaitResult::abandoned};
        fail_registration.store(true);
        const auto result = extension
            ? broker->ensure(*reservation, bundle({file_key('2')}))
            : broker->acquire(instance, bundle({file_key('2')}));
        require(result.status == WorkspaceOwnershipStatus::backend_failure &&
                    result.abandonment_observed,
                "registration failure must preserve an actual abandoned wait in its result");
        if (extension) {
            const auto released = broker->release(*reservation);
            require(released.ok() && released.abandonment_observed,
                    "reservation cleanup must retain abandonment after failed registration");
        }
        const auto stopped = broker->shutdown();
        require(stopped.status == WorkspaceOwnershipStatus::owner_failed,
                "registration failure must remain visible at shutdown");
        if (extension) require_exact_cleanup(*backend, {1, 2});
        else require_exact_cleanup(*backend, {1});
    }
}

void test_shutdown_retries_one_shot_release_and_close_failures_exactly() {
    for (std::size_t failure_position = 0; failure_position < 3; ++failure_position) {
        RecordingBackend* backend = nullptr;
        auto broker = test_broker(backend);
        (void)acquire(*broker, new_instance(*broker),
                      bundle({path_key('1'), path_key('2'), file_key('3')}));
        backend->throw_release_at = failure_position;

        const auto failed = run_bounded([&] { return broker->shutdown(); },
                                        "shutdown release failure blocked its caller");
        require(failed.status == WorkspaceOwnershipStatus::owner_failed,
                "shutdown release failure must fail closed and preserve the owner thread");
        const auto failed_handle = static_cast<WorkspaceOwnershipBackend::Handle>(
            failure_position + 1);
        require(backend->handles.at(failed_handle).open &&
                    backend->handles.at(failed_handle).owned,
                "shutdown must retain a failed release for owner-thread retry");
        const auto retried = run_bounded([&] { return broker->shutdown(); },
                                         "shutdown release retry hung");
        require(retried.status == WorkspaceOwnershipStatus::owner_failed,
                "successful cleanup retry must retain the earlier failure status");
        require_exact_cleanup(*backend, {1, 2, 3});
    }

    for (std::size_t failure_position = 0; failure_position < 3; ++failure_position) {
        RecordingBackend* backend = nullptr;
        auto broker = test_broker(backend);
        (void)acquire(*broker, new_instance(*broker),
                      bundle({path_key('1'), path_key('2'), file_key('3')}));
        backend->throw_close_at = failure_position;

        const auto failed = run_bounded([&] { return broker->shutdown(); },
                                        "shutdown close failure blocked its caller");
        require(failed.status == WorkspaceOwnershipStatus::owner_failed,
                "shutdown close failure must fail closed and preserve the owner thread");
        const auto failed_handle = static_cast<WorkspaceOwnershipBackend::Handle>(
            failure_position + 1);
        require(backend->handles.at(failed_handle).open &&
                    !backend->handles.at(failed_handle).owned,
                "shutdown must retain only a close obligation after successful release");
        const auto retried = run_bounded([&] { return broker->shutdown(); },
                                         "shutdown close retry hung");
        require(retried.status == WorkspaceOwnershipStatus::owner_failed,
                "successful close retry must retain the earlier failure status");
        require_exact_cleanup(*backend, {1, 2, 3});
    }
}

void test_persistent_release_failure_has_a_bounded_terminal_policy() {
    auto observation = std::make_shared<BackendObservation>();
    RecordingBackend* backend = nullptr;
    auto broker = test_broker(backend, {}, observation);
    (void)acquire(*broker, new_instance(*broker), bundle({path_key('1')}));
    backend->always_throw_release = true;

    const auto failed = run_bounded([&] { return broker->shutdown(); },
                                    "persistent release failure blocked public shutdown");
    require(failed.status == WorkspaceOwnershipStatus::owner_failed,
            "persistent release failure must be reported while the handle remains tracked");
    require(backend->handles.at(1).open && backend->handles.at(1).owned,
            "public shutdown must preserve a persistently failed owned handle for retry");
    run_bounded([owned = std::move(broker)]() mutable {
        owned.reset();
        return true;
    }, "persistent release failure blocked terminal destruction");
    require(observation->release_attempts.load() == 2,
            "terminal destruction must make one final owner-thread release attempt");
    require(observation->close_attempts.load() == 1 &&
                observation->open_handles.load() == 0,
            "joining thread must close the handle after owner-thread exit");
    require(observation->terminal_abandonments.load() == 1,
            "persistent release failure must terminate as explicit mutex abandonment");
}

void test_reentrant_shutdown_is_rejected_without_stopping_the_owner() {
    WorkspaceOwnershipBroker* broker_address = nullptr;
    std::optional<WorkspaceOwnershipResult> reentrant_result;
    WorkspaceOwnershipTestHooks hooks;
    hooks.after_wait_acquired = [&] {
        require(broker_address != nullptr, "test broker must exist before its first wait");
        reentrant_result = broker_address->shutdown();
    };
    RecordingBackend* backend = nullptr;
    auto broker = test_broker(backend, std::move(hooks));
    broker_address = broker.get();

    const auto acquired = run_bounded(
        [&] { return broker->acquire(new_instance(*broker), bundle({path_key('1')})); },
        "owner-thread reentrant shutdown blocked acquisition");
    require(acquired.ok() && acquired.reservation.has_value(),
            "rejecting reentrant shutdown must allow the active acquisition to finish");
    require(reentrant_result.has_value() &&
                reentrant_result->status == WorkspaceOwnershipStatus::reentrant_shutdown,
            "owner-thread shutdown must be rejected explicitly");
    const auto external = run_bounded([&] { return broker->shutdown(); },
                                      "external shutdown hung after reentrant rejection");
    require(external.ok(), "external shutdown should still stop and join the owner thread");
    require_exact_cleanup(*backend, {1});
}

void test_persistent_request_queue_failure_cannot_block_terminal_destruction() {
    std::atomic<bool> fail_enqueue = false;
    WorkspaceOwnershipTestHooks hooks;
    hooks.before_request_enqueue = [&] {
        if (fail_enqueue.load()) throw std::bad_alloc();
    };
    auto observation = std::make_shared<BackendObservation>();
    RecordingBackend* backend = nullptr;
    auto broker = test_broker(backend, std::move(hooks), observation);
    const auto reservation = acquire(*broker, new_instance(*broker), bundle({path_key('1')}));

    fail_enqueue.store(true);
    const auto rejected = broker->ensure(reservation, bundle({path_key('1')}));
    require(rejected.status == WorkspaceOwnershipStatus::owner_failed,
            "persistent request allocation failure should fail closed");
    require(backend->handles.at(1).open && backend->handles.at(1).owned,
            "failed request enqueue must leave ownership with the owner thread");
    run_bounded([owned = std::move(broker)]() mutable {
        owned.reset();
        return true;
    }, "persistent queue allocation failure blocked terminal destruction");
    require(observation->open_handles.load() == 0,
            "allocation-free terminal shutdown must close the retained handle");
    require(observation->release_attempts.load() == 1 &&
                observation->close_attempts.load() == 1,
            "terminal shutdown must perform exact owner-thread cleanup");
    require(observation->terminal_abandonments.load() == 0,
            "successful terminal cleanup must not abandon a mutex");
}

void test_identity_validation_shutdown_and_namespace() {
    RecordingBackend* first_backend = nullptr;
    auto first = test_broker(first_backend);
    const auto first_reservation = acquire(*first, new_instance(*first), bundle({path_key('1')}));

    RecordingBackend* second_backend = nullptr;
    auto second = test_broker(second_backend);
    require(second->ensure(first_reservation, bundle({file_key('2')})).status ==
                WorkspaceOwnershipStatus::invalid_reservation,
            "a reservation from another broker must be rejected before mutation");

    for (const auto& event : first_backend->events) {
        if (event.kind == RecordingBackend::Event::Kind::create) {
            require(event.name.rfind("Global\\Vertex.Owner.", 0) == 0,
                    "lifetime mutexes must use the owner namespace");
            require(event.name.find("Save.") == std::string::npos,
                    "lifetime mutexes must not use the Save namespace");
        }
    }

    require(first->shutdown().ok(), "normal shutdown should drain and release reservations");
    require(first_backend->count(RecordingBackend::Event::Kind::release) == 1,
            "shutdown should balance the reservation wait with a release");
    require(first_backend->count(RecordingBackend::Event::Kind::close) == 1,
            "shutdown should close the reservation handle on the owner thread");
    require(!first->new_workspace_instance().has_value(),
            "shutdown should reject new workspace instances");
    require(second->acquire(new_instance(*second), bundle({path_key('3')})).status ==
                WorkspaceOwnershipStatus::success,
            "an independent broker should continue after the first broker stops");
    require(first->acquire(WorkspaceInstanceId{}, bundle({path_key('4')})).status ==
                WorkspaceOwnershipStatus::stopped,
            "shutdown should reject acquisition requests");
}

#ifdef _WIN32
void test_real_windows_mutex_with_unique_key() {
    auto& broker = WorkspaceOwnershipBroker::instance();
    const auto instance = broker.new_workspace_instance();
    require(instance.has_value(), "production broker should issue a workspace instance");
    std::random_device random;
    std::string unique_digest(64, '0');
    constexpr std::string_view digits = "0123456789abcdef";
    for (auto& digit : unique_digest) digit = digits[random() & 0x0fU];
    const auto unique_key = WorkspaceOwnershipKey::path(CanonicalSha256::from_hex(unique_digest));
    const auto result = broker.acquire(*instance, bundle({unique_key}));
    require(result.ok() && result.reservation.has_value(),
            "production broker should acquire a unique Windows mutex");
    require(broker.release(*result.reservation).ok(),
            "production broker should release its Windows mutex");
    require(broker.shutdown().ok(), "production broker should shut down cleanly");
}
#endif

}  // namespace

int main() {
    try {
        test_digest_and_bundle_are_canonical_and_typed();
        test_registry_rejects_duplicate_workspace_before_recursive_wait();
        test_ensure_is_idempotent_and_orders_new_keys();
        test_partial_acquisition_rolls_back_only_new_keys();
        test_failed_extension_preserves_original_reservation();
        test_explicit_key_release_preserves_reservation_and_allows_transfer();
        test_backend_exception_fails_closed_after_rollback();
        test_wait_failure_after_an_earlier_acquisition_rolls_back_exactly();
        test_post_acquisition_allocation_and_rollback_failure_remain_tracked();
        test_release_failure_retains_open_owned_handles_for_full_retry();
        test_close_failure_retains_only_a_close_obligation_for_full_retry();
        test_full_release_retries_owned_and_close_pending_handles();
        test_abandonment_is_a_pure_sticky_kernel_observation();
        test_abandonment_survives_a_later_bundle_conflict();
        test_requester_concurrency_and_thread_affine_cleanup();
        test_unexpected_owner_stop_allows_bounded_shutdown_and_destruction();
        test_concurrent_and_repeated_shutdown_is_idempotent();
        test_shutdown_drain_keeps_acquisition_fail_closed();
        test_registration_failure_preserves_abandonment();
        test_shutdown_retries_one_shot_release_and_close_failures_exactly();
        test_persistent_release_failure_has_a_bounded_terminal_policy();
        test_reentrant_shutdown_is_rejected_without_stopping_the_owner();
        test_persistent_request_queue_failure_cannot_block_terminal_destruction();
        test_identity_validation_shutdown_and_namespace();
#ifdef _WIN32
        test_real_windows_mutex_with_unique_key();
#endif
    } catch (const std::exception& error) {
        std::cerr << "workspace_ownership_broker_tests: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
