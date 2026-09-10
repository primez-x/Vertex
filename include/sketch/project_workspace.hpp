#pragma once

#include "sketch/document.hpp"
#include "sketch/workspace_document_history.hpp"
#include "sketch/boundary_active_recovery.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace sketch {
namespace detail { struct WorkspaceDocumentState; }

class ProjectWorkspace;

// A sealed, instance-bound candidate. Returned snapshots are detached values.
// Ticket access, moves and destruction must be serialized with commit on the
// owning thread. A consumed ticket retains the full retired Document until
// destruction/replacement; release it promptly rather than storing it as history.
class PreparedWorkspaceEdit final {
public:
    PreparedWorkspaceEdit(PreparedWorkspaceEdit&&) noexcept;
    PreparedWorkspaceEdit& operator=(PreparedWorkspaceEdit&&) noexcept;
    PreparedWorkspaceEdit(const PreparedWorkspaceEdit&) = delete;
    PreparedWorkspaceEdit& operator=(const PreparedWorkspaceEdit&) = delete;
    ~PreparedWorkspaceEdit();

    [[nodiscard]] DocumentSnapshot preview() const;

private:
    friend class ProjectWorkspace;
    struct State;
    explicit PreparedWorkspaceEdit(std::unique_ptr<State> state) noexcept;
    std::unique_ptr<State> state_;
};

// Document and active-checkpoint mutation authority. Recovery ledger and persistence
// coordination are separate, pending layers of the workspace contract.
// Confined to its owning application thread: member calls must not overlap.
// Storage workers receive detached snapshots, never the workspace itself.
class ProjectWorkspace final {
public:
    explicit ProjectWorkspace(const DocumentSnapshot& source,
        BoundaryAuthoringResourcePolicy policy = boundary_authoring_default_resource_policy);
    ProjectWorkspace(const ProjectWorkspace&) = delete;
    ProjectWorkspace& operator=(const ProjectWorkspace&) = delete;
    ProjectWorkspace(ProjectWorkspace&&) = delete;
    ProjectWorkspace& operator=(ProjectWorkspace&&) = delete;
    ~ProjectWorkspace();

    [[nodiscard]] const std::string& identity() const noexcept;
    [[nodiscard]] std::uint64_t epoch() const noexcept;
    // Independent monotonic content counters. Navigation advances them too;
    // imported document revisions are baseline state, not new workspace edits.
    [[nodiscard]] std::uint64_t edited_generation() const noexcept;
    [[nodiscard]] std::uint64_t checkpoint_generation() const noexcept;
    [[nodiscard]] DocumentSnapshot snapshot() const;
    [[nodiscard]] WorkspaceDocumentHistory document_history() const;
    [[nodiscard]] std::optional<BoundaryActiveRecovery> active_boundary() const;
    // Stages a current-source checkpoint without changing document geometry.
    // Replacement preserves session identity and monotonic ID counters. Exact
    // repeats are rejected; pointer-only changes do not advance edited_generation.
    // This does not finish, discard, rebind, or load a stale recovered session.
    [[nodiscard]] PreparedWorkspaceEdit prepare_boundary_checkpoint(
        const BoundaryActiveRecovery& checkpoint) const;
    [[nodiscard]] PreparedWorkspaceEdit prepare(const Command& command) const;
    [[nodiscard]] PreparedWorkspaceEdit prepare_undo() const;
    [[nodiscard]] PreparedWorkspaceEdit prepare_redo() const;
    Revision commit(PreparedWorkspaceEdit& edit);

private:
    enum class Operation { apply, undo, redo };
    [[nodiscard]] std::unique_ptr<PreparedWorkspaceEdit::State> prepare_state() const;
    [[nodiscard]] PreparedWorkspaceEdit prepare_impl(
        Operation operation, const Command* command) const;

    std::unique_ptr<detail::WorkspaceDocumentState> state_;
    const std::string identity_;
    const BoundaryAuthoringResourcePolicy resource_policy_;
    std::uint64_t epoch_ = 0;
    std::uint64_t edited_generation_ = 0;
    std::uint64_t checkpoint_generation_ = 0;
};

}  // namespace sketch
