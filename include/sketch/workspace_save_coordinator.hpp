#pragma once

#include "sketch/project_store.hpp"
#include "sketch/project_workspace.hpp"

#include <memory>
#include <string>
#include <string_view>

namespace sketch {

// Correlation supplied by the owner/queue, never evidence of a filesystem
// reservation. Paths are bounded UTF-8 text compared exactly: no resolution,
// normalization, case folding, or filesystem access takes place here.
struct SavePublicationBinding {
    std::string owner_token;
    ArchiveRole role = ArchiveRole::ordinary;
    std::string destination_identity;
    std::string destination_path;
    bool operator==(const SavePublicationBinding&) const = default;
};

class SavePublicationTicket final {
public:
    SavePublicationTicket(SavePublicationTicket&&) noexcept;
    SavePublicationTicket& operator=(SavePublicationTicket&&) noexcept;
    SavePublicationTicket(const SavePublicationTicket&) = delete;
    SavePublicationTicket& operator=(const SavePublicationTicket&) = delete;
    ~SavePublicationTicket();

private:
    friend class WorkspaceSaveCoordinator;
    struct State;
    explicit SavePublicationTicket(std::unique_ptr<State>) noexcept;
    std::unique_ptr<State> state_;
};

enum class SaveAcknowledgementStatus {
    acknowledged,
    consumed_ticket,
    invalid_receipt,
    invalid_current_binding,
    binding_mismatch,
    stale_workspace,
    source_mismatch,
};

struct SaveAcknowledgementResult {
    SaveAcknowledgementStatus status;
    // A structurally valid successful storage receipt for the sealed revision.
    // This trusts the queue to return the actual SaveReceipt from its save job;
    // it does not independently authenticate the archive bytes or destination.
    bool publication_valid = false;
    [[nodiscard]] bool acknowledged() const noexcept {
        return status == SaveAcknowledgementStatus::acknowledged;
    }
};

// Pure owner-thread acknowledgement gate. Ticket operations and completion
// processing must be serialized. The queue must pair each ticket with the
// SaveReceipt from that exact immutable save job: SaveReceipt itself contains
// no owner/path/job identity. No Document saved marker or workspace watermark
// is changed here, including for recovery_copy acknowledgements.
class WorkspaceSaveCoordinator final {
public:
    // Invalid bindings/digests throw std::invalid_argument. The semantic
    // authoring digest is distinct from both the source-file and output hashes.
    [[nodiscard]] static SavePublicationTicket capture(
        const ProjectWorkspaceSnapshot&, const SavePublicationBinding&,
        std::string_view authoring_source_digest);

    // Every attempt consumes the ticket, including malformed/stale completions.
    // A valid stale publication is retained as a file fact but never acknowledges
    // newer state. Counters use equality only; there is no cross-counter ordering.
    // A canonical but different file hash cannot be rejected without a separate
    // trusted output fingerprint; SaveReceipt is the storage result, not a seal.
    [[nodiscard]] static SaveAcknowledgementResult accept(
        SavePublicationTicket&, const SaveReceipt&,
        const ProjectWorkspaceSnapshot& current,
        const SavePublicationBinding& current_binding,
        std::string_view current_authoring_source_digest);
};

}  // namespace sketch
