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

class ProjectArchiveSnapshot;
class ConstraintAuthoringPreview;
class BoundaryCommitPreview;
struct DecodedRecoveryLedger;

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

// Bind a typed redraw to its exact archived authoring input. Shared by live
// finish preparation and persisted lifecycle validation; this grants no commit
// authority and never changes the source snapshot.
void validate_workspace_boundary_redefinition_input(const DocumentSnapshot&,
    const BoundaryActiveRecovery&, const EditBoundaryGeometry&,
    const BoundaryAuthoringResourcePolicy& = boundary_authoring_default_resource_policy);
// Canonical classification changes shared by redraw UI and history validation.
[[nodiscard]] nlohmann::json boundary_redefinition_classification_properties(
    const DocumentSnapshot&, const Entity& target, std::string_view classification);

// Detached owner-thread capture for background readers. Its document, draft,
// history and counters describe the same workspace state. It grants neither
// publication/acknowledgement authority nor filesystem ownership. Copies own
// their data; readers never receive a reference to the mutable workspace.
class ProjectWorkspaceSnapshot final {
public:
    ProjectWorkspaceSnapshot(const ProjectWorkspaceSnapshot&) = default;
    ProjectWorkspaceSnapshot& operator=(const ProjectWorkspaceSnapshot&) = default;
    ProjectWorkspaceSnapshot(ProjectWorkspaceSnapshot&&) noexcept = default;
    ProjectWorkspaceSnapshot& operator=(ProjectWorkspaceSnapshot&&) noexcept = default;

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
    [[nodiscard]] const nlohmann::json& history_extensions() const noexcept { return history_extensions_; }

private:
    friend class ProjectWorkspace;
    ProjectWorkspaceSnapshot(const DocumentSnapshot&, const WorkspaceDocumentHistory&,
        const std::optional<BoundaryActiveRecovery>&, const std::string&,
        std::uint64_t epoch, std::uint64_t edited_generation, std::uint64_t checkpoint_generation,
        const BoundaryAuthoringResourcePolicy&, const WorkspaceNavigationState&,
        const std::vector<WorkspaceLifecycleEvent>&, const WorkspaceRetiredBoundaries&, const nlohmann::json&);

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
    nlohmann::json history_extensions_;
};

// Validated detached workspace ownership. Capture remains on its preparation
// thread; the owner consumes the move-only token once through adopt_prepared.
class PreparedProjectWorkspace final {
public:
    PreparedProjectWorkspace(PreparedProjectWorkspace&&) noexcept;
    PreparedProjectWorkspace& operator=(PreparedProjectWorkspace&&) noexcept;
    PreparedProjectWorkspace(const PreparedProjectWorkspace&) = delete;
    PreparedProjectWorkspace& operator=(const PreparedProjectWorkspace&) = delete;
    ~PreparedProjectWorkspace();
    // Preparation-thread access only, before transfer to the owner.
    [[nodiscard]] ProjectWorkspaceSnapshot capture() const;
private:
    friend class ProjectWorkspace;
    explicit PreparedProjectWorkspace(std::unique_ptr<ProjectWorkspace>) noexcept;
    std::unique_ptr<ProjectWorkspace> workspace_;
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

// Opaque immutable owner-thread capture for ordinary edit preparation. Copies
// share detached const values, including the original instance/counter fence,
// recovery/navigation state and policy. No live workspace reference escapes.
class WorkspaceEditCapture final {
public:
    WorkspaceEditCapture(const WorkspaceEditCapture&) = default;
    WorkspaceEditCapture& operator=(const WorkspaceEditCapture&) = default;
    WorkspaceEditCapture(WorkspaceEditCapture&&) noexcept = default;
    WorkspaceEditCapture& operator=(WorkspaceEditCapture&&) noexcept = default;
    ~WorkspaceEditCapture();

private:
    friend class ProjectWorkspace;
    struct State;
    explicit WorkspaceEditCapture(std::shared_ptr<const State>) noexcept;
    std::shared_ptr<const State> state_;
};

// Document, active-checkpoint and lifecycle navigation authority.
// Persisted restoration grants no filesystem ownership or save acknowledgement.
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

    // Validate and allocate entirely on the preparation thread. The token is
    // the only transfer boundary; no live workspace can be rebound. After the
    // owner has validated its original fence and full source proof, adoption
    // consumes the token once and publishes without allocation or validation.
    [[nodiscard]] static PreparedProjectWorkspace prepare_detached(const DocumentSnapshot&);
    [[nodiscard]] static std::unique_ptr<ProjectWorkspace> adopt_prepared(PreparedProjectWorkspace&&) noexcept;

    // Revalidates the archive and supplied decoded aggregate before restoration.
    // Archives do not persist instance identity or resource policy: restoration
    // creates a fresh workspace identity and uses the default resource policy.
    [[nodiscard]] static std::unique_ptr<ProjectWorkspace> restore_archive(
        const ProjectArchiveSnapshot&, const DecodedRecoveryLedger&);

    [[nodiscard]] const std::string& identity() const noexcept;
    [[nodiscard]] const std::string& document_id() const noexcept;
    [[nodiscard]] Revision revision() const noexcept;
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
    [[nodiscard]] PreparedWorkspaceEdit prepare_finish_boundary(const EditBoundaryGeometry&) const;
    // Explicitly starts fresh input from a retained retired view, bound to the
    // current document and original context. The retired view stays available.
    [[nodiscard]] PreparedWorkspaceEdit prepare_revise_boundary(std::string_view identity_namespace) const;
    [[nodiscard]] bool can_undo() const noexcept;
    [[nodiscard]] bool can_redo() const noexcept;
    [[nodiscard]] PreparedWorkspaceEdit prepare(const Command& command) const;
    // Capture on the owner, prepare on a worker using only this detached value,
    // then transfer the ticket after completion for ordinary owner commit.
    // A ticket remains bound to the original unchanged workspace instance.
    [[nodiscard]] WorkspaceEditCapture capture_edit_source() const;
    [[nodiscard]] static PreparedWorkspaceEdit prepare_captured(
        const WorkspaceEditCapture& source, const Command& command);
    // Revalidate sealed previews on an isolated fork, then stage one ordinary
    // document edit. Boundary commit does not finish an active session.
    [[nodiscard]] PreparedWorkspaceEdit prepare_constraint_authoring(
        const ConstraintAuthoringPreview& preview) const;
    [[nodiscard]] PreparedWorkspaceEdit prepare_boundary_commit(
        const BoundaryCommitPreview& preview) const;
    [[nodiscard]] PreparedWorkspaceEdit prepare_undo() const;
    [[nodiscard]] PreparedWorkspaceEdit prepare_redo() const;
    Revision commit(PreparedWorkspaceEdit& edit);

private:
    [[nodiscard]] static std::unique_ptr<ProjectWorkspace> restore_components(
        const DocumentSnapshot&, const WorkspaceDocumentHistory&,
        const std::optional<BoundaryActiveRecovery>&, const WorkspaceNavigationState&,
        const std::vector<WorkspaceLifecycleEvent>&, const WorkspaceRetiredBoundaries&,
        const nlohmann::json&, std::uint64_t epoch, std::uint64_t edited_generation,
        std::uint64_t checkpoint_generation);
    [[nodiscard]] std::unique_ptr<PreparedWorkspaceEdit::State> prepare_state() const;
    [[nodiscard]] static std::unique_ptr<PreparedWorkspaceEdit::State> prepare_state(
        const WorkspaceEditCapture&);
    [[nodiscard]] PreparedWorkspaceEdit prepare_document_edit(const Command&) const;
    [[nodiscard]] static PreparedWorkspaceEdit prepare_document_edit(
        std::unique_ptr<PreparedWorkspaceEdit::State>, const Command&);
    [[nodiscard]] PreparedWorkspaceEdit prepare_finish_boundary_impl(const EditBoundaryGeometry*) const;
    [[nodiscard]] PreparedWorkspaceEdit prepare_navigation(bool redo) const;

    std::unique_ptr<detail::WorkspaceDocumentState> state_;
    const std::string identity_;
    const BoundaryAuthoringResourcePolicy resource_policy_;
    std::uint64_t epoch_ = 0;
    std::uint64_t edited_generation_ = 0;
    std::uint64_t checkpoint_generation_ = 0;
};

}  // namespace sketch
