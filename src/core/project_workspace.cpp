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
    WorkspaceNavigationState navigation;
    std::vector<WorkspaceLifecycleEvent> lifecycle;
};
}

namespace {
bool same_semantics(const BoundaryActiveRecovery& left, const BoundaryActiveRecovery& right) {
    const auto& a = left.checkpoint;
    const auto& b = right.checkpoint;
    // JSON numeric equality intentionally equates integer 1 and float 1.0.
    // Opaque extension representation must survive archival input sharing.
    return left.source == right.source && left.extensions.dump() == right.extensions.dump() &&
        a.version == b.version && a.replay_version == b.replay_version && a.mode == b.mode &&
        a.identity_namespace == b.identity_namespace && a.options == b.options &&
        a.actions == b.actions && a.history_position == b.history_position &&
        a.counters == b.counters && a.extensions.dump() == b.extensions.dump();
}
WorkspaceLifecycleEvent next_event(const detail::WorkspaceDocumentState& state,
                                  WorkspaceLifecycleKind kind) {
    if (state.lifecycle.size() == std::numeric_limits<std::uint64_t>::max())
        throw std::overflow_error("workspace lifecycle sequence is exhausted");
    WorkspaceLifecycleEvent event;
    event.event_id = make_stable_id();
    event.sequence = static_cast<std::uint64_t>(state.lifecycle.size()) + 1;
    event.kind = kind;
    event.before_revision = event.after_revision = state.document->revision();
    return event;
}
const WorkspaceArchivedInput* last_input(const detail::WorkspaceDocumentState& state,
                                       const WorkspaceNavigationTarget& target) {
    const auto* id = std::get_if<std::string>(&target.identity);
    for (auto it = state.lifecycle.rbegin(); it != state.lifecycle.rend(); ++it) {
        if (it->input && ((it->target && *it->target == target) || (id && it->event_id == *id)))
            return &*it->input;
    }
    return nullptr;
}
WorkspaceArchivedInput archive_input(const detail::WorkspaceDocumentState& state,
                                     const std::string& owner) {
    if (!state.active) throw std::invalid_argument("there is no active boundary to archive");
    for (auto it = state.lifecycle.rbegin(); it != state.lifecycle.rend(); ++it) {
        if (it->input && same_semantics(*it->input->value, *state.active)) {
            auto reference = *it->input;
            reference.pointer_override.emplace(state.active->checkpoint.pointer);
            return reference;
        }
    }
    return {owner, std::make_shared<const BoundaryActiveRecovery>(*state.active), std::nullopt};
}
void restore_input(detail::WorkspaceDocumentState& state, const WorkspaceArchivedInput& input) {
    if (state.active) throw std::invalid_argument("restoration would overwrite an active boundary");
    state.active.emplace(*input.value);
    if (input.pointer_override) state.active->checkpoint.pointer = *input.pointer_override;
}
void append_document_event(detail::WorkspaceDocumentState& state,
                           const WorkspaceLifecycleEvent& event, WorkspaceDocumentEventKind kind) {
    auto& events = state.history.events;
    if (events.size() == std::numeric_limits<std::uint64_t>::max())
        throw std::overflow_error("workspace document sequence is exhausted");
    events.push_back({event.event_id, static_cast<std::uint64_t>(events.size()) + 1,
        kind, event.before_revision, event.after_revision});
}
}

struct PreparedWorkspaceEdit::State {
    std::string workspace_identity;
    std::uint64_t expected_epoch = 0;
    std::string source_digest;
    std::unique_ptr<detail::WorkspaceDocumentState> candidate;
    bool consumed = false;
    bool advances_edited_generation = true;
};

ProjectWorkspaceSnapshot::ProjectWorkspaceSnapshot(
    const DocumentSnapshot& document, const WorkspaceDocumentHistory& history,
    const std::optional<BoundaryActiveRecovery>& active, const std::string& identity,
    std::uint64_t epoch, std::uint64_t edited_generation, std::uint64_t checkpoint_generation,
    const BoundaryAuthoringResourcePolicy& policy, const WorkspaceNavigationState& navigation,
    const std::vector<WorkspaceLifecycleEvent>& lifecycle)
    : document_(document), history_(history), active_(active), identity_(identity),
      epoch_(epoch), edited_generation_(edited_generation), checkpoint_generation_(checkpoint_generation),
      resource_policy_(policy), navigation_(navigation), lifecycle_history_(lifecycle) {}

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
    state_->navigation = initialize_workspace_navigation(source, state_->history.baseline);
}

ProjectWorkspace::~ProjectWorkspace() = default;

const std::string& ProjectWorkspace::identity() const noexcept { return identity_; }
std::uint64_t ProjectWorkspace::epoch() const noexcept { return epoch_; }
std::uint64_t ProjectWorkspace::edited_generation() const noexcept { return edited_generation_; }
std::uint64_t ProjectWorkspace::checkpoint_generation() const noexcept { return checkpoint_generation_; }
DocumentSnapshot ProjectWorkspace::snapshot() const { return state_->document->snapshot(); }
ProjectWorkspaceSnapshot ProjectWorkspace::capture() const {
    return ProjectWorkspaceSnapshot(state_->document->snapshot(), state_->history, state_->active,
        identity_, epoch_, edited_generation_, checkpoint_generation_, resource_policy_,
        state_->navigation, state_->lifecycle);
}
WorkspaceDocumentHistory ProjectWorkspace::document_history() const { return state_->history; }
std::optional<BoundaryActiveRecovery> ProjectWorkspace::active_boundary() const { return state_->active; }

PreparedWorkspaceEdit ProjectWorkspace::prepare(const Command& command) const {
    return prepare_document_edit(command);
}

PreparedWorkspaceEdit ProjectWorkspace::prepare_undo() const {
    return prepare_navigation(false);
}

PreparedWorkspaceEdit ProjectWorkspace::prepare_redo() const {
    return prepare_navigation(true);
}
bool ProjectWorkspace::can_undo() const noexcept { return !state_->navigation.undo_stack.empty(); }
bool ProjectWorkspace::can_redo() const noexcept { return !state_->navigation.redo_stack.empty(); }

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
    state->candidate->navigation = state_->navigation;
    state->candidate->lifecycle = state_->lifecycle;
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
    if (!state_->active) {
        for (const auto& event : state_->lifecycle) {
            if (event.session && event.session->identity_namespace == checkpoint.checkpoint.identity_namespace)
                throw std::invalid_argument("session identity is already retained; restore through lifecycle navigation");
        }
    }
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
        if (previous == checkpoint && same_semantics(previous, checkpoint)) {
            throw std::invalid_argument("boundary checkpoint is unchanged");
        }
        // Compare every semantic field directly, avoiding a full checkpoint
        // copy merely to clear the pointer. Envelope extensions are content.
        semantic_change = !same_semantics(previous, checkpoint);
    }
    auto state = prepare_state();
    auto& candidate = *state->candidate;
    if (!candidate.active) {
        auto event = next_event(candidate, WorkspaceLifecycleKind::boundary_activate);
        event.session = WorkspaceSessionIdentity{checkpoint.source, checkpoint.checkpoint.identity_namespace,
            checkpoint.checkpoint.mode, checkpoint.checkpoint.counters};
        candidate.navigation = record_workspace_operation(candidate.navigation, event.event_id,
            WorkspaceOperationKind::boundary_activate);
        candidate.lifecycle.push_back(std::move(event));
    } else if (semantic_change && !candidate.navigation.redo_stack.empty()) {
        auto event = next_event(candidate, WorkspaceLifecycleKind::clear_redo);
        candidate.navigation = clear_workspace_redo(candidate.navigation);
        candidate.lifecycle.push_back(std::move(event));
    }
    state->candidate->active = checkpoint;
    state->advances_edited_generation = semantic_change;
    return PreparedWorkspaceEdit(std::move(state));
}

PreparedWorkspaceEdit ProjectWorkspace::prepare_document_edit(const Command& command) const {
    auto state = prepare_state();
    auto& candidate = *state->candidate;
    auto event = next_event(candidate, WorkspaceLifecycleKind::document_edit);
    event.after_revision = candidate.document->apply(command);
    append_document_event(candidate, event, WorkspaceDocumentEventKind::edit);
    candidate.navigation = record_workspace_operation(candidate.navigation, event.event_id,
        WorkspaceOperationKind::document_edit);
    candidate.lifecycle.push_back(std::move(event));
    return PreparedWorkspaceEdit(std::move(state));
}

PreparedWorkspaceEdit ProjectWorkspace::prepare_discard_boundary() const {
    if (!state_->active) throw std::invalid_argument("there is no active boundary to discard");
    auto state = prepare_state();
    auto& candidate = *state->candidate;
    auto event = next_event(candidate, WorkspaceLifecycleKind::boundary_discard);
    event.input = archive_input(candidate, event.event_id);
    candidate.navigation = record_workspace_operation(candidate.navigation, event.event_id,
        WorkspaceOperationKind::boundary_discard);
    candidate.active.reset();
    candidate.lifecycle.push_back(std::move(event));
    return PreparedWorkspaceEdit(std::move(state));
}

PreparedWorkspaceEdit ProjectWorkspace::prepare_navigation(bool redo) const {
    if (redo ? !can_redo() : !can_undo()) {
        throw DocumentError(redo ? DocumentErrorCode::no_redo : DocumentErrorCode::no_undo,
            "there is no workspace operation to navigate");
    }
    auto state = prepare_state();
    auto& candidate = *state->candidate;
    auto transition = navigate_workspace_history(candidate.navigation, redo);
    auto event = next_event(candidate, redo ? WorkspaceLifecycleKind::redo : WorkspaceLifecycleKind::undo);
    event.target = transition.target;
    switch (transition.kind) {
    case WorkspaceOperationKind::document_edit:
        event.after_revision = redo ? candidate.document->redo(event.before_revision) :
            candidate.document->undo(event.before_revision);
        append_document_event(candidate, event,
            redo ? WorkspaceDocumentEventKind::redo : WorkspaceDocumentEventKind::undo);
        break;
    case WorkspaceOperationKind::boundary_activate:
        if (redo) {
            const auto* input = last_input(candidate, transition.target);
            if (!input) throw std::invalid_argument("activation restoration input is missing");
            event.input = *input;
            restore_input(candidate, *event.input);
        } else {
            const auto& id = std::get<std::string>(transition.target.identity);
            const WorkspaceSessionIdentity* session = nullptr;
            for (const auto& origin : candidate.lifecycle)
                if (origin.event_id == id && origin.session) { session = &*origin.session; break; }
            if (!session || !candidate.active || candidate.active->source != session->source ||
                candidate.active->checkpoint.identity_namespace != session->identity_namespace ||
                candidate.active->checkpoint.mode != session->mode)
                throw std::invalid_argument("activation navigation does not match the active session");
            event.input = archive_input(candidate, event.event_id);
            candidate.active.reset();
        }
        break;
    case WorkspaceOperationKind::boundary_discard: {
        const auto* input = last_input(candidate, transition.target);
        if (!input) throw std::invalid_argument("discard restoration input is missing");
        if (redo) {
            if (!candidate.active || !same_semantics(*input->value, *candidate.active))
                throw std::invalid_argument("discard redo does not match the restored session");
            event.input = archive_input(candidate, event.event_id);
            candidate.active.reset();
        } else {
            event.input = *input;
            restore_input(candidate, *event.input);
        }
        break;
    }
    case WorkspaceOperationKind::boundary_finish:
        throw std::logic_error("finish lifecycle is not yet implemented");
    }
    candidate.navigation = std::move(transition.state);
    candidate.lifecycle.push_back(std::move(event));
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
