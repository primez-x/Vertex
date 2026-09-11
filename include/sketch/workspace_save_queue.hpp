#pragma once

#include "sketch/workspace_save_coordinator.hpp"

#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace sketch {

// A serialized owner-independent save worker. Save operations must capture
// immutable ProjectWorkspaceSnapshot data before enqueueing; the queue never
// touches a mutable workspace or GUI object. Completion records are drained
// by the owner thread and may then be passed to WorkspaceSaveCoordinator.
class WorkspaceSaveQueue final {
public:
    using SaveOperation = std::function<SaveReceipt()>;

    enum class CompletionKind { save, barrier };

    struct Completion final {
        std::uint64_t sequence{};
        CompletionKind kind{CompletionKind::save};
        std::optional<SavePublicationTicket> ticket;
        std::optional<SaveReceipt> receipt;
        std::exception_ptr error;

        [[nodiscard]] bool succeeded() const noexcept {
            return !error && (kind == CompletionKind::barrier || receipt.has_value());
        }
    };

    WorkspaceSaveQueue();
    WorkspaceSaveQueue(const WorkspaceSaveQueue&) = delete;
    WorkspaceSaveQueue& operator=(const WorkspaceSaveQueue&) = delete;
    ~WorkspaceSaveQueue();

    // Enqueues one save operation and returns its FIFO sequence. The ticket is
    // retained until the owner drains its matching completion. An empty
    // operation is rejected before the ticket is moved into the queue.
    [[nodiscard]] std::uint64_t enqueue(SavePublicationTicket ticket, SaveOperation operation);

    // A barrier is ordered after all prior work and completes only after that
    // work has finished (successfully or with captured errors). It is useful
    // for explicit Save/close/shutdown coordination.
    [[nodiscard]] std::uint64_t enqueue_barrier();

    // Nonblocking owner-thread drain. Completion order is the queue order.
    [[nodiscard]] std::vector<Completion> take_completed();

    // Stops accepting work. With drain=true, queued operations finish before
    // the worker exits; false discards queued work and retains only completions
    // already produced. Calling shutdown more than once is harmless.
    void shutdown(bool drain = true);

private:
    struct State;
    std::unique_ptr<State> state_;
};

}  // namespace sketch
