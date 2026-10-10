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
    // Transient version-two authoring equality includes every retained typed
    // history proof while excluding saved markers and derived editability.
    // Persisted version-one recovery receipts keep their original contract.
    class SourceProof final {
    public:
        [[nodiscard]] static SourceProof capture(const DocumentSnapshot&);
        [[nodiscard]] const DocumentSnapshot& source() const noexcept { return source_; }
        [[nodiscard]] const std::string& digest() const noexcept { return digest_; }
    private:
        SourceProof(DocumentSnapshot, std::string) noexcept;
        DocumentSnapshot source_;
        std::string digest_;
    };

    // A publication can only be created by publish(), which seals the ticket,
    // candidate and full captured source before invoking the actual storage
    // operation. Fields cannot be swapped by completion consumers.
    class PreparedSave final {
    public:
        PreparedSave(PreparedSave&&) noexcept = default;
        PreparedSave& operator=(PreparedSave&&) noexcept = default;
        PreparedSave(const PreparedSave&) = delete;
        PreparedSave& operator=(const PreparedSave&) = delete;
        [[nodiscard]] static PreparedSave publish(const ProjectWorkspaceSnapshot&,
            const SavePublicationBinding&, SourceProof,
            SaveOperation, std::optional<PreparedProjectWorkspace> = std::nullopt);
        [[nodiscard]] bool publication_valid() const;
        [[nodiscard]] SaveAcknowledgementResult acknowledge(const SaveAcknowledgementDescriptor&,
            const SavePublicationBinding&, std::string_view source_digest);
        [[nodiscard]] const SaveReceipt& receipt() const noexcept { return receipt_; }
        [[nodiscard]] const DocumentSnapshot& source() const noexcept { return source_; }
        [[nodiscard]] const std::string& source_digest() const noexcept { return source_digest_; }
        [[nodiscard]] const SaveAcknowledgementDescriptor& descriptor() const noexcept { return descriptor_; }
        [[nodiscard]] bool has_workspace() const noexcept { return workspace_.has_value(); }
        [[nodiscard]] std::unique_ptr<ProjectWorkspace> adopt_workspace() noexcept;
    private:
        PreparedSave(SavePublicationTicket, SaveReceipt, DocumentSnapshot, std::string,
            SaveAcknowledgementDescriptor, std::optional<PreparedProjectWorkspace>) noexcept;
        SavePublicationTicket ticket_;
        SaveReceipt receipt_;
        DocumentSnapshot source_;
        std::string source_digest_;
        SaveAcknowledgementDescriptor descriptor_;
        std::optional<PreparedProjectWorkspace> workspace_;
        bool acknowledged_ = false;
    };
    using PreparedOperation = std::function<PreparedSave()>;
    using TaskOperation = std::function<void()>;

    enum class CompletionKind { save, prepared_save, task, barrier };

    struct Completion final {
        std::uint64_t sequence{};
        CompletionKind kind{CompletionKind::save};
        std::optional<SavePublicationTicket> ticket;
        std::optional<SaveReceipt> receipt;
        std::optional<PreparedSave> prepared;
        std::exception_ptr error;

        [[nodiscard]] bool succeeded() const noexcept {
            return !error && (kind == CompletionKind::barrier || kind == CompletionKind::task ||
                (kind == CompletionKind::prepared_save ? prepared.has_value() : ticket && receipt));
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
    [[nodiscard]] std::uint64_t enqueue_prepared(PreparedOperation);
    // Detached immutable proof/disposal work participates in the same FIFO.
    [[nodiscard]] std::uint64_t enqueue_task(TaskOperation);

    // A barrier is ordered after all prior work and completes only after that
    // work has finished (successfully or with captured errors). It is useful
    // for explicit Save/close/shutdown coordination.
    [[nodiscard]] std::uint64_t enqueue_barrier();

    // Nonblocking owner-thread drain. Completion order is the queue order.
    [[nodiscard]] std::vector<Completion> take_completed();
    // Single-record owner drain avoids allocating a temporary completion batch.
    [[nodiscard]] std::optional<Completion> take_completed_one();

    // Stops accepting work. With drain=true, queued operations finish before
    // the worker exits; false discards queued work and retains only completions
    // already produced. Calling shutdown more than once is harmless.
    void shutdown(bool drain = true);

private:
    struct State;
    std::unique_ptr<State> state_;
};

}  // namespace sketch
