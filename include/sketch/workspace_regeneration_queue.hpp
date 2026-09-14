#pragma once

#include <atomic>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace sketch {

// A cancellation signal owned by one regeneration operation.  Geometry
// workers must poll this token at bounded points and must never mutate the
// live document; the owner applies only a completed receipt whose source
// revision still matches the current workspace.
class RegenerationCancellationToken final {
public:
    RegenerationCancellationToken(const RegenerationCancellationToken&) = default;
    RegenerationCancellationToken& operator=(const RegenerationCancellationToken&) = default;

    [[nodiscard]] bool is_cancelled() const noexcept;

private:
    friend class WorkspaceRegenerationQueue;
    struct State;
    explicit RegenerationCancellationToken(std::shared_ptr<State> state) noexcept;
    std::shared_ptr<State> state_;
};

// A derived result is explicitly tied to the immutable source revision that
// produced it.  The queue treats this as opaque data and never installs it in
// a document; callers perform the revision/fingerprint check on their owner
// thread before publishing a view.
struct RegenerationReceipt final {
    std::uint64_t source_revision{};
    std::string output_digest;
};

enum class RegenerationCompletionKind { completed, cancelled, failed };

struct RegenerationCompletion final {
    std::uint64_t sequence{};
    RegenerationCompletionKind kind{RegenerationCompletionKind::failed};
    std::optional<RegenerationReceipt> receipt;
    std::exception_ptr error;

    [[nodiscard]] bool succeeded() const noexcept {
        return kind == RegenerationCompletionKind::completed && receipt.has_value() && !error;
    }
};

// Owner-independent worker boundary for expensive derived geometry.  Work is
// FIFO and runs off the caller thread.  Cancelling a queued or running job
// guarantees that its receipt is discarded even if the operation returns
// after observing the cancellation.  Completion records are drained by the
// owner thread, where source-revision validation and presentation publication
// must occur.
class WorkspaceRegenerationQueue final {
public:
    using RegenerationOperation =
        std::function<RegenerationReceipt(const RegenerationCancellationToken&)>;
    using Completion = RegenerationCompletion;
    using CompletionKind = RegenerationCompletionKind;

    WorkspaceRegenerationQueue();
    WorkspaceRegenerationQueue(const WorkspaceRegenerationQueue&) = delete;
    WorkspaceRegenerationQueue& operator=(const WorkspaceRegenerationQueue&) = delete;
    ~WorkspaceRegenerationQueue();

    [[nodiscard]] std::uint64_t enqueue(RegenerationOperation operation);

    // Returns false when the sequence is no longer queued or running.  A true
    // result means the token is now cancelled; the operation may still need to
    // reach its next bounded cancellation point before completion is delivered.
    [[nodiscard]] bool cancel(std::uint64_t sequence);

    // Nonblocking owner-thread drain.  Records retain FIFO sequence order.
    [[nodiscard]] std::vector<RegenerationCompletion> take_completed();

    // With drain=true, accepted work completes.  With drain=false, running and
    // queued work is cancelled, queued operations are never invoked, and
    // ordered cancellation records are retained for the owner to drain.
    void shutdown(bool drain = true);

private:
    struct State;
    std::unique_ptr<State> state_;
};

}  // namespace sketch
