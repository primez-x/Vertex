#include "sketch/project_workspace.hpp"

#include "sketch/document_digest.hpp"
#include "sketch/boundary_commit.hpp"

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
    WorkspaceRetiredBoundaries retired;
    nlohmann::json history_extensions = nlohmann::json::object();
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
                                     const BoundaryActiveRecovery& input, const std::string& owner) {
    for (auto it = state.lifecycle.rbegin(); it != state.lifecycle.rend(); ++it) {
        if (it->input && same_semantics(*it->input->value, input)) {
            auto reference = *it->input;
            reference.pointer_override.emplace(input.checkpoint.pointer);
            reference.status = WorkspaceInputStatus::active;
            reference.finish_event_id.reset();
            return reference;
        }
    }
    return {owner, std::make_shared<const BoundaryActiveRecovery>(input), std::nullopt};
}
void restore_input(detail::WorkspaceDocumentState& state, const WorkspaceArchivedInput& input) {
    const auto& identity_namespace = input.value->checkpoint.identity_namespace;
    if (state.retired.contains(identity_namespace))
        throw std::invalid_argument("restoration would overwrite retired boundary input");
    if (input.status == WorkspaceInputStatus::retired) {
        if (!input.finish_event_id || (state.active && state.active->checkpoint.identity_namespace == identity_namespace))
            throw std::invalid_argument("retired restoration has inconsistent session status");
        state.retired.emplace(identity_namespace, input);
        return;
    }
    if (input.status != WorkspaceInputStatus::active || input.finish_event_id)
        throw std::invalid_argument("active restoration has inconsistent session status");
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
    const std::vector<WorkspaceLifecycleEvent>& lifecycle, const WorkspaceRetiredBoundaries& retired,
    const nlohmann::json& history_extensions)
    : document_(document), history_(history), active_(active), identity_(identity),
      epoch_(epoch), edited_generation_(edited_generation), checkpoint_generation_(checkpoint_generation),
      resource_policy_(policy), navigation_(navigation), lifecycle_history_(lifecycle), retired_(retired),
      history_extensions_(history_extensions) {}

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

std::unique_ptr<ProjectWorkspace> ProjectWorkspace::restore_components(
    const DocumentSnapshot& document, const WorkspaceDocumentHistory& history,
    const std::optional<BoundaryActiveRecovery>& active, const WorkspaceNavigationState& navigation,
    const std::vector<WorkspaceLifecycleEvent>& lifecycle, const WorkspaceRetiredBoundaries& retired,
    const nlohmann::json& history_extensions, std::uint64_t epoch,
    std::uint64_t edited_generation, std::uint64_t checkpoint_generation) {
    // All validation precedes state installation. Document::fork recomputes
    // editability from retained content and preserves existing saved markers.
    auto restored = std::make_unique<ProjectWorkspace>(document);
    restored->state_->history = history;
    restored->state_->active = active;
    restored->state_->navigation = navigation;
    restored->state_->lifecycle = lifecycle;
    restored->state_->retired = retired;
    restored->state_->history_extensions = history_extensions;
    restored->epoch_ = epoch;
    restored->edited_generation_ = edited_generation;
    restored->checkpoint_generation_ = checkpoint_generation;
    return restored;
}

const std::string& ProjectWorkspace::identity() const noexcept { return identity_; }
std::uint64_t ProjectWorkspace::epoch() const noexcept { return epoch_; }
std::uint64_t ProjectWorkspace::edited_generation() const noexcept { return edited_generation_; }
std::uint64_t ProjectWorkspace::checkpoint_generation() const noexcept { return checkpoint_generation_; }
DocumentSnapshot ProjectWorkspace::snapshot() const { return state_->document->snapshot(); }
ProjectWorkspaceSnapshot ProjectWorkspace::capture() const {
    return ProjectWorkspaceSnapshot(state_->document->snapshot(), state_->history, state_->active,
        identity_, epoch_, edited_generation_, checkpoint_generation_, resource_policy_,
        state_->navigation, state_->lifecycle, state_->retired, state_->history_extensions);
}
WorkspaceDocumentHistory ProjectWorkspace::document_history() const { return state_->history; }
std::optional<BoundaryActiveRecovery> ProjectWorkspace::active_boundary() const { return state_->active; }
std::optional<BoundaryActiveRecovery> ProjectWorkspace::retired_boundary(std::string_view identity_namespace) const {
    const auto found = state_->retired.find(identity_namespace);
    if (found == state_->retired.end()) return std::nullopt;
    std::optional<BoundaryActiveRecovery> result{*found->second.value};
    if (found->second.pointer_override) result->checkpoint.pointer = *found->second.pointer_override;
    return std::optional<BoundaryActiveRecovery>(result);
}

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
    state->candidate->retired = state_->retired;
    state->candidate->history_extensions = state_->history_extensions;
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
    event.input = archive_input(candidate, *candidate.active, event.event_id);
    candidate.navigation = record_workspace_operation(candidate.navigation, event.event_id,
        WorkspaceOperationKind::boundary_discard);
    candidate.active.reset();
    candidate.lifecycle.push_back(std::move(event));
    return PreparedWorkspaceEdit(std::move(state));
}

PreparedWorkspaceEdit ProjectWorkspace::prepare_revise_boundary(std::string_view identity_namespace) const {
    if (state_->active) throw std::invalid_argument("revise requires resolving the active boundary first");
    const auto found = state_->retired.find(identity_namespace);
    if (found == state_->retired.end()) throw std::invalid_argument("retired boundary input is missing");
    auto revised = *retired_boundary(identity_namespace);
    revised.checkpoint = BoundaryAuthoringSession::revise_recovery_checkpoint(
        revised.checkpoint, resource_policy_).recovery_checkpoint();
    revised.source = capture_boundary_recovery_source(state_->document->snapshot(), revised.source.context);
    auto ticket = prepare_boundary_checkpoint(revised);
    auto& session = *ticket.state_->candidate->lifecycle.back().session;
    session.revised_from_namespace = std::string(identity_namespace);
    session.revised_from_finish_event_id = found->second.finish_event_id;
    return ticket;
}

PreparedWorkspaceEdit ProjectWorkspace::prepare_finish_boundary() const {
    if (!state_->active) throw std::invalid_argument("there is no active boundary to finish");
    const auto source = state_->document->snapshot();
    if (inspect_boundary_recovery_source(source, state_->active->source) != BoundaryRecoverySourceStatus::current)
        throw std::invalid_argument("boundary finish source is not current");
    const auto session = BoundaryAuthoringSession::from_recovery_checkpoint(
        state_->active->checkpoint, resource_policy_);
    if (session.phase() != BoundaryAuthoringPhase::completed || session.accepted_chains().empty())
        throw std::invalid_argument("finish requires a completed boundary checkpoint");
    auto state = prepare_state();
    auto& candidate = *state->candidate;
    const BoundaryCommitIntent intent{session.options(), session.accepted_chains(),
        candidate.active->source.context, "Finish boundary"};
    const auto preview = preview_boundary_commit(candidate.document->snapshot(), intent);
    if (!preview.accepted())
        throw std::invalid_argument(preview.diagnostics().empty() ? "boundary finish was rejected" : preview.diagnostics().front());
    auto event = next_event(candidate, WorkspaceLifecycleKind::boundary_finish);
    event.input = archive_input(candidate, *candidate.active, event.event_id);
    event.after_revision = apply_boundary_commit(*candidate.document, preview);
    append_document_event(candidate, event, WorkspaceDocumentEventKind::edit);
    candidate.navigation = record_workspace_operation(candidate.navigation, event.event_id,
        WorkspaceOperationKind::boundary_finish);
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
            if (!session) throw std::invalid_argument("activation origin is missing");
            if (candidate.active && candidate.active->source == session->source &&
                candidate.active->checkpoint.identity_namespace == session->identity_namespace &&
                candidate.active->checkpoint.mode == session->mode) {
                event.input = archive_input(candidate, *candidate.active, event.event_id);
                candidate.active.reset();
            } else {
                const auto retired = candidate.retired.find(session->identity_namespace);
                if (retired == candidate.retired.end() || retired->second.value->source != session->source ||
                    retired->second.value->checkpoint.mode != session->mode)
                    throw std::invalid_argument("activation navigation does not match a retained session");
                event.input = retired->second;
                candidate.retired.erase(retired);
            }
        }
        break;
    case WorkspaceOperationKind::boundary_discard: {
        const auto* input = last_input(candidate, transition.target);
        if (!input) throw std::invalid_argument("discard restoration input is missing");
        if (redo) {
            if (!candidate.active || !same_semantics(*input->value, *candidate.active))
                throw std::invalid_argument("discard redo does not match the restored session");
            event.input = archive_input(candidate, *candidate.active, event.event_id);
            candidate.active.reset();
        } else {
            event.input = *input;
            restore_input(candidate, *event.input);
        }
        break;
    }
    case WorkspaceOperationKind::boundary_finish: {
        const auto* input = last_input(candidate, transition.target);
        if (!input) throw std::invalid_argument("finish restoration input is missing");
        const auto& finish_id = std::get<std::string>(transition.target.identity);
        const auto& identity_namespace = input->value->checkpoint.identity_namespace;
        if (redo) {
            const auto retired = candidate.retired.find(identity_namespace);
            if (retired == candidate.retired.end() || retired->second.finish_event_id != finish_id)
                throw std::invalid_argument("finish redo does not match retired input");
            event.input = retired->second;
            event.after_revision = candidate.document->redo(event.before_revision);
            candidate.retired.erase(retired);
        } else {
            event.input = *input;
            event.input->status = WorkspaceInputStatus::retired;
            event.input->finish_event_id = finish_id;
            event.after_revision = candidate.document->undo(event.before_revision);
            restore_input(candidate, *event.input);
        }
        append_document_event(candidate, event,
            redo ? WorkspaceDocumentEventKind::redo : WorkspaceDocumentEventKind::undo);
        break;
    }
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
