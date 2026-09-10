#include "sketch/project_workspace.hpp"

#include "sketch/document_digest.hpp"

#include <limits>
#include <stdexcept>
#include <utility>

namespace sketch {
namespace detail {
struct WorkspaceDocumentState {
    std::unique_ptr<Document> document;
    WorkspaceDocumentHistory history;
    std::optional<BoundaryActiveRecovery> active;
};
}

struct PreparedWorkspaceEdit::State {
    std::string workspace_identity;
    std::uint64_t expected_epoch = 0;
    std::string source_digest;
    std::unique_ptr<detail::WorkspaceDocumentState> candidate;
    bool consumed = false;
    bool advances_edited_generation = true;
};

PreparedWorkspaceEdit::PreparedWorkspaceEdit(std::unique_ptr<State> state) noexcept
    : state_(std::move(state)) {}

PreparedWorkspaceEdit::PreparedWorkspaceEdit(PreparedWorkspaceEdit&&) noexcept = default;
PreparedWorkspaceEdit& PreparedWorkspaceEdit::operator=(PreparedWorkspaceEdit&&) noexcept = default;
PreparedWorkspaceEdit::~PreparedWorkspaceEdit() = default;

DocumentSnapshot PreparedWorkspaceEdit::preview() const {
    if (!state_ || state_->consumed || !state_->candidate) {
        throw std::invalid_argument("workspace edit is consumed or moved from");
    }
    return state_->candidate->document->snapshot();
}

ProjectWorkspace::ProjectWorkspace(const DocumentSnapshot& source, BoundaryAuthoringResourcePolicy policy)
    : state_(std::make_unique<detail::WorkspaceDocumentState>()), identity_(make_stable_id()),
      resource_policy_(policy) {
    state_->document.reset(new Document(Document::fork(source)));
    state_->history = capture_workspace_document_history(source);
}

ProjectWorkspace::~ProjectWorkspace() = default;

const std::string& ProjectWorkspace::identity() const noexcept { return identity_; }
std::uint64_t ProjectWorkspace::epoch() const noexcept { return epoch_; }
std::uint64_t ProjectWorkspace::edited_generation() const noexcept { return edited_generation_; }
std::uint64_t ProjectWorkspace::checkpoint_generation() const noexcept { return checkpoint_generation_; }
DocumentSnapshot ProjectWorkspace::snapshot() const { return state_->document->snapshot(); }
WorkspaceDocumentHistory ProjectWorkspace::document_history() const { return state_->history; }
std::optional<BoundaryActiveRecovery> ProjectWorkspace::active_boundary() const { return state_->active; }

PreparedWorkspaceEdit ProjectWorkspace::prepare(const Command& command) const {
    return prepare_impl(Operation::apply, &command);
}

PreparedWorkspaceEdit ProjectWorkspace::prepare_undo() const {
    return prepare_impl(Operation::undo, nullptr);
}

PreparedWorkspaceEdit ProjectWorkspace::prepare_redo() const {
    return prepare_impl(Operation::redo, nullptr);
}

std::unique_ptr<PreparedWorkspaceEdit::State> ProjectWorkspace::prepare_state() const {
    const auto source = state_->document->snapshot();
    auto state = std::make_unique<PreparedWorkspaceEdit::State>();
    state->workspace_identity = identity_;
    state->expected_epoch = epoch_;
    state->source_digest = document_snapshot_digest(source);
    state->candidate = std::make_unique<detail::WorkspaceDocumentState>();
    state->candidate->document.reset(new Document(Document::fork(source)));
    state->candidate->history = state_->history;
    state->candidate->active = state_->active;
    return state;
}

PreparedWorkspaceEdit ProjectWorkspace::prepare_boundary_checkpoint(
    const BoundaryActiveRecovery& checkpoint) const {
    // Canonical replay and enclosing-record resource checks finish before any
    // publication. They validate borrowed input before we retain its copy.
    (void)encode_boundary_active_recovery(checkpoint, resource_policy_);
    if (inspect_boundary_recovery_source(state_->document->snapshot(), checkpoint.source) !=
        BoundaryRecoverySourceStatus::current) {
        throw std::invalid_argument("boundary checkpoint source is not current");
    }
    bool semantic_change = true;
    if (state_->active) {
        const auto& previous = *state_->active;
        const auto& old = previous.checkpoint;
        const auto& next = checkpoint.checkpoint;
        if (previous.source != checkpoint.source || old.identity_namespace != next.identity_namespace ||
            old.mode != next.mode) {
            throw std::invalid_argument("boundary checkpoint replacement changes session identity or source");
        }
        if (next.counters.next_boundary_id < old.counters.next_boundary_id ||
            next.counters.next_vertex_id < old.counters.next_vertex_id ||
            next.counters.next_segment_id < old.counters.next_segment_id ||
            next.counters.next_dimension_id < old.counters.next_dimension_id) {
            throw std::invalid_argument("boundary checkpoint replacement rewinds allocated identities");
        }
        if (previous == checkpoint) {
            throw std::invalid_argument("boundary checkpoint is unchanged");
        }
        // Compare every semantic field directly, avoiding a full checkpoint
        // copy merely to clear the pointer. Envelope extensions are content.
        semantic_change = old.version != next.version || old.replay_version != next.replay_version ||
            old.options != next.options || old.actions != next.actions ||
            old.history_position != next.history_position || old.counters != next.counters ||
            old.extensions != next.extensions || previous.extensions != checkpoint.extensions;
    }
    auto state = prepare_state();
    state->candidate->active = checkpoint;
    state->advances_edited_generation = semantic_change;
    return PreparedWorkspaceEdit(std::move(state));
}

PreparedWorkspaceEdit ProjectWorkspace::prepare_impl(
    Operation operation, const Command* command) const {
    auto state = prepare_state();
    const auto source_revision = state->candidate->document->revision();
    auto kind = WorkspaceDocumentEventKind::edit;
    switch (operation) {
    case Operation::apply:
        state->candidate->document->apply(*command);
        break;
    case Operation::undo:
        state->candidate->document->undo(source_revision);
        kind = WorkspaceDocumentEventKind::undo;
        break;
    case Operation::redo:
        state->candidate->document->redo(source_revision);
        kind = WorkspaceDocumentEventKind::redo;
        break;
    }
    auto& events = state->candidate->history.events;
    if (events.size() == std::numeric_limits<std::uint64_t>::max())
        throw std::overflow_error("workspace history sequence is exhausted");
    events.push_back({make_stable_id(), static_cast<std::uint64_t>(events.size()) + 1,
                      kind, source_revision, state->candidate->document->revision()});
    return PreparedWorkspaceEdit(std::move(state));
}

Revision ProjectWorkspace::commit(PreparedWorkspaceEdit& edit) {
    if (!edit.state_ || edit.state_->consumed || !edit.state_->candidate) {
        throw std::invalid_argument("workspace edit is consumed or moved from");
    }
    auto& state = *edit.state_;
    if (state.workspace_identity != identity_) {
        throw std::invalid_argument("workspace edit belongs to another workspace");
    }
    if (state.expected_epoch != epoch_) {
        throw std::invalid_argument("workspace edit epoch is stale");
    }
    if (epoch_ == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("workspace epoch is exhausted");
    }
    if ((state.advances_edited_generation && edited_generation_ == std::numeric_limits<std::uint64_t>::max()) ||
        checkpoint_generation_ == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("workspace content generation is exhausted");
    }
    {
        // Complete all allocating validation and dispose its JSON snapshots
        // before publication. Full history and saved markers are part of CAS.
        const auto current = state_->document->snapshot();
        if (document_snapshot_digest(current) != state.source_digest) {
            throw std::invalid_argument("workspace edit source digest is stale");
        }
    }
    const Revision revision = state.candidate->document->revision();
    state_.swap(state.candidate);
    ++epoch_;
    if (state.advances_edited_generation) ++edited_generation_;
    ++checkpoint_generation_;
    state.consumed = true;
    // The consumed ticket retains the retired Document. Its destruction must
    // not run here: JSON disposal can allocate even after successful mutation.
    return revision;
}

}  // namespace sketch
