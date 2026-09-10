#pragma once

#include "sketch/document.hpp"
#include "sketch/workspace_document_history.hpp"
#include "sketch/boundary_active_recovery.hpp"
#include "sketch/workspace_navigation.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace sketch {
namespace detail { struct WorkspaceDocumentState; }

class ProjectWorkspace;

enum class WorkspaceLifecycleKind { document_edit, boundary_activate, boundary_discard, boundary_finish, undo, redo, clear_redo };
enum class WorkspaceInputStatus { active, retired };
struct WorkspaceSessionIdentity {
    BoundaryRecoverySource source;
    std::string identity_namespace;
    BoundaryAuthoringMode mode{};
    BoundaryAuthoringCounters initial_counters;
    std::optional<std::string> revised_from_namespace;
    std::optional<std::string> revised_from_finish_event_id;
};
// Only the owner event contains a full immutable input. Repeat executions can
// share that owner and override its pointer, including explicitly clearing it.
struct WorkspaceArchivedInput {
    std::string owner_event_id;
    std::shared_ptr<const BoundaryActiveRecovery> value;
    std::optional<std::optional<Vec2>> pointer_override;
    WorkspaceInputStatus status{WorkspaceInputStatus::active};
    std::optional<std::string> finish_event_id;
};
using WorkspaceRetiredBoundaries = std::map<std::string, WorkspaceArchivedInput, std::less<>>;
struct WorkspaceLifecycleEvent {
    std::string event_id;
    std::uint64_t sequence{};
    WorkspaceLifecycleKind kind{};
    Revision before_revision{}, after_revision{};
    std::optional<WorkspaceNavigationTarget> target;
    std::optional<WorkspaceSessionIdentity> session;
    std::optional<WorkspaceArchivedInput> input;
};

// Detached owner-thread capture for background readers. Its document, draft,
// history and counters describe the same workspace state. It grants neither
// publication/acknowledgement authority nor filesystem ownership. Copies own
// their data; readers never receive a reference to the mutable workspace.
class ProjectWorkspaceSnapshot final {
public:
    ProjectWorkspaceSnapshot(const ProjectWorkspaceSnapshot&) = default;
    ProjectWorkspaceSnapshot& operator=(const ProjectWorkspaceSnapshot&) = default;

    [[nodiscard]] const DocumentSnapshot& document() const noexcept { return document_; }
    [[nodiscard]] const WorkspaceDocumentHistory& document_history() const noexcept { return history_; }
    [[nodiscard]] const std::optional<BoundaryActiveRecovery>& active_boundary() const noexcept { return active_; }
    [[nodiscard]] const std::string& identity() const noexcept { return identity_; }
    [[nodiscard]] std::uint64_t epoch() const noexcept { return epoch_; }
    [[nodiscard]] std::uint64_t edited_generation() const noexcept { return edited_generation_; }
    [[nodiscard]] std::uint64_t checkpoint_generation() const noexcept { return checkpoint_generation_; }
    [[nodiscard]] const BoundaryAuthoringResourcePolicy& resource_policy() const noexcept { return resource_policy_; }
    [[nodiscard]] const WorkspaceNavigationState& navigation() const noexcept { return navigation_; }
    [[nodiscard]] const std::vector<WorkspaceLifecycleEvent>& lifecycle_history() const noexcept { return lifecycle_history_; }
    [[nodiscard]] const WorkspaceRetiredBoundaries& retired_boundaries() const noexcept { return retired_; }

private:
    friend class ProjectWorkspace;
    ProjectWorkspaceSnapshot(const DocumentSnapshot&, const WorkspaceDocumentHistory&,
        const std::optional<BoundaryActiveRecovery>&, const std::string&,
        std::uint64_t epoch, std::uint64_t edited_generation, std::uint64_t checkpoint_generation,
        const BoundaryAuthoringResourcePolicy&, const WorkspaceNavigationState&,
        const std::vector<WorkspaceLifecycleEvent>&, const WorkspaceRetiredBoundaries&);

    DocumentSnapshot document_;
    WorkspaceDocumentHistory history_;
    std::optional<BoundaryActiveRecovery> active_;
    std::string identity_;
    std::uint64_t epoch_;
    std::uint64_t edited_generation_;
    std::uint64_t checkpoint_generation_;
    BoundaryAuthoringResourcePolicy resource_policy_;
    WorkspaceNavigationState navigation_;
    std::vector<WorkspaceLifecycleEvent> lifecycle_history_;
    WorkspaceRetiredBoundaries retired_;
};

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

// Document, active-checkpoint and lifecycle navigation authority.
// Persisted-ledger validation and storage coordination remain pending.
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
    [[nodiscard]] ProjectWorkspaceSnapshot capture() const;
    [[nodiscard]] WorkspaceDocumentHistory document_history() const;
    [[nodiscard]] std::optional<BoundaryActiveRecovery> active_boundary() const;
    // A detached view only. Retired input is never an active finish source.
    [[nodiscard]] std::optional<BoundaryActiveRecovery> retired_boundary(std::string_view identity_namespace) const;
    // Stages a current-source checkpoint without changing document geometry.
    // Replacement preserves session identity and monotonic ID counters. Exact
    // repeats are rejected; pointer-only changes do not advance edited_generation.
    // This does not finish, discard, rebind, or load a stale recovered session.
    [[nodiscard]] PreparedWorkspaceEdit prepare_boundary_checkpoint(
        const BoundaryActiveRecovery& checkpoint) const;
    [[nodiscard]] PreparedWorkspaceEdit prepare_discard_boundary() const;
    // Requires an already completed, classified current-source checkpoint.
    // Does not synthesize closure, classification, or replacement identities.
    [[nodiscard]] PreparedWorkspaceEdit prepare_finish_boundary() const;
    // Explicitly starts fresh input from a retained retired view, bound to the
    // current document and original context. The retired view stays available.
    [[nodiscard]] PreparedWorkspaceEdit prepare_revise_boundary(std::string_view identity_namespace) const;
    [[nodiscard]] bool can_undo() const noexcept;
    [[nodiscard]] bool can_redo() const noexcept;
    [[nodiscard]] PreparedWorkspaceEdit prepare(const Command& command) const;
    [[nodiscard]] PreparedWorkspaceEdit prepare_undo() const;
    [[nodiscard]] PreparedWorkspaceEdit prepare_redo() const;
    Revision commit(PreparedWorkspaceEdit& edit);

private:
    [[nodiscard]] std::unique_ptr<PreparedWorkspaceEdit::State> prepare_state() const;
    [[nodiscard]] PreparedWorkspaceEdit prepare_document_edit(const Command&) const;
    [[nodiscard]] PreparedWorkspaceEdit prepare_navigation(bool redo) const;

    std::unique_ptr<detail::WorkspaceDocumentState> state_;
    const std::string identity_;
    const BoundaryAuthoringResourcePolicy resource_policy_;
    std::uint64_t epoch_ = 0;
    std::uint64_t edited_generation_ = 0;
    std::uint64_t checkpoint_generation_ = 0;
};

}  // namespace sketch
