#include "sketch/workspace_lifecycle_validation.hpp"
#include "sketch/boundary_commit.hpp"
#include "sketch/document_digest.hpp"

#include <stdexcept>
#include <cmath>
#include <set>
#include <tuple>
#include <unordered_set>
#include <utility>

namespace sketch {
namespace {
void validate_payload(const WorkspaceLifecycleEvent& event,
                      bool target, bool session, bool input) {
    if (event.target.has_value() != target || event.session.has_value() != session ||
        event.input.has_value() != input) {
        throw std::invalid_argument("Workspace lifecycle event payload does not match its kind");
    }
}
}

void validate_workspace_finish_deltas(const DocumentSnapshot& snapshot,
    const std::vector<WorkspaceLifecycleEvent>& events, const BoundaryAuthoringResourcePolicy& policy) {
    try { (void)Document::fork(snapshot); }
    catch (const DocumentError& error) { throw std::invalid_argument(error.what()); }
    for (const auto& event : events) {
        if (event.kind != WorkspaceLifecycleKind::boundary_finish) continue;
        if (!event.input || !event.input->value || event.after_revision >= snapshot.history().size() ||
            event.before_revision >= event.after_revision || event.after_revision - event.before_revision != 1)
            throw std::invalid_argument("Finish delta has invalid input or revision bounds");
        const auto& input = *event.input->value;
        if (input.source.revision != event.before_revision)
            throw std::invalid_argument("Finish delta uses stale input");
        validate_historical_boundary_recovery_source(snapshot, input.source);
        const auto session = BoundaryAuthoringSession::from_recovery_checkpoint(input.checkpoint, policy);
        if (session.phase() != BoundaryAuthoringPhase::completed || session.accepted_chains().empty())
            throw std::invalid_argument("Finish delta input is incomplete");
        const auto& before = snapshot.history()[static_cast<std::size_t>(event.before_revision)];
        const auto& after = snapshot.history()[static_cast<std::size_t>(event.after_revision)];
        if (after.source_revision || after.name || after.action != "Finish boundary" || before.assets != after.assets)
            throw std::invalid_argument("Finish delta changes unrelated document state");
        for (const auto& [id, asset] : before.assets)
            if (asset.metadata.dump() != after.assets.at(id).metadata.dump())
                throw std::invalid_argument("Finish delta changes asset metadata representation");
        const auto prefix = Document::fork_at_revision(snapshot, event.before_revision);
        const auto preview = preview_boundary_commit(prefix.snapshot(),
            {session.options(), session.accepted_chains(), input.source.context, "Finish boundary"});
        if (!preview.accepted() || preview.candidate_digest() != entity_map_digest(after.entities))
            throw std::invalid_argument("Finish geometry does not match its archived input");
    }
}

void validate_workspace_recovery_sources(const DocumentSnapshot& snapshot,
    const std::vector<WorkspaceLifecycleEvent>& events, const std::optional<BoundaryActiveRecovery>& active) {
    using Key = std::tuple<std::string, Revision, std::string, std::string, std::string, std::string, std::string>;
    std::set<Key> checked;
    const auto validate = [&](const BoundaryRecoverySource& source, Revision latest) {
        if (source.revision > latest) throw std::invalid_argument("Boundary source comes from a future revision");
        const auto& context = source.context;
        Key key{source.document_id, source.revision, source.authoring_digest,
            context.property_id, context.building_id, context.floor_id, context.layer_id};
        if (checked.insert(std::move(key)).second) validate_historical_boundary_recovery_source(snapshot, source);
    };
    for (const auto& event : events) {
        if (event.session) {
            if (event.session->source.revision != event.before_revision)
                throw std::invalid_argument("Activation source was not current at activation");
            validate(event.session->source, event.before_revision);
        }
        if (event.input) {
            if (!event.input->value) throw std::invalid_argument("Source input payload is missing");
            validate(event.input->value->source, event.before_revision);
        }
    }
    if (active) validate(active->source, snapshot.revision());
}

void validate_workspace_archival_inputs(const std::vector<WorkspaceLifecycleEvent>& events,
                                         const BoundaryAuthoringResourcePolicy& policy) {
    std::map<std::string, const WorkspaceArchivedInput*, std::less<>> owners;
    std::map<std::string, std::string, std::less<>> finishes;
    std::unordered_set<std::string> ids;
    for (const auto& event : events) {
        if (event.event_id.empty() || event.event_id.size() > 128 ||
            event.event_id.find('\0') != std::string::npos || !ids.insert(event.event_id).second)
            throw std::invalid_argument("Invalid archival event identity");
        if (!event.input) continue;
        const auto& input = *event.input;
        if (!input.value) throw std::invalid_argument("Archived input has no immutable payload");
        if (input.owner_event_id == event.event_id) {
            if (input.pointer_override || input.status != WorkspaceInputStatus::active || input.finish_event_id)
                throw std::invalid_argument("Full input owner has reference-only fields");
            (void)encode_boundary_active_recovery(*input.value, policy);
            owners.emplace(event.event_id, &input);
        } else {
            const auto owner = owners.find(input.owner_event_id);
            if (owner == owners.end()) throw std::invalid_argument("Input reference has no direct preceding owner");
            if (input.value != owner->second->value &&
                encode_boundary_active_recovery(*input.value, policy).dump() !=
                encode_boundary_active_recovery(*owner->second->value, policy).dump())
                throw std::invalid_argument("Input reference substitutes its owner's payload");
        }
        if (input.pointer_override && *input.pointer_override &&
            (!std::isfinite((*input.pointer_override)->x) || !std::isfinite((*input.pointer_override)->y)))
            throw std::invalid_argument("Input pointer override is not finite");
        if (input.status == WorkspaceInputStatus::active) {
            if (input.finish_event_id) throw std::invalid_argument("Active input has retired finish provenance");
        } else if (input.status == WorkspaceInputStatus::retired) {
            const auto finish = input.finish_event_id ? finishes.find(*input.finish_event_id) : finishes.end();
            if (finish == finishes.end() || finish->second != input.value->checkpoint.identity_namespace)
                throw std::invalid_argument("Retired input has no matching preceding finish");
        } else {
            throw std::invalid_argument("Unknown archival input status");
        }
        if (event.kind == WorkspaceLifecycleKind::boundary_finish)
            finishes.emplace(event.event_id, input.value->checkpoint.identity_namespace);
    }
}

void validate_workspace_lifecycle_order(
    const DocumentSnapshot& snapshot, const WorkspaceDocumentHistory& history,
    const std::vector<WorkspaceLifecycleEvent>& events,
    const WorkspaceNavigationState& expected_navigation) {
    validate_workspace_document_history(snapshot, history);
    auto navigation = initialize_workspace_navigation(snapshot, history.baseline);
    auto revision = history.baseline.baseline_revision;
    std::size_t document_index = 0;
    std::unordered_set<std::string> ids;
    for (std::size_t index = 0; index < events.size(); ++index) {
        const auto& event = events[index];
        if (event.event_id.empty() || event.event_id.size() > 128 ||
            event.event_id.find('\0') != std::string::npos ||
            !ids.insert(event.event_id).second) {
            throw std::invalid_argument("Workspace lifecycle event ID is invalid or duplicated");
        }
        if (event.sequence != index + 1 || event.before_revision != revision) {
            throw std::invalid_argument("Workspace lifecycle sequence or revision chain is invalid");
        }

        std::optional<WorkspaceDocumentEventKind> document_kind;
        std::optional<WorkspaceOperationKind> operation_kind;
        switch (event.kind) {
        case WorkspaceLifecycleKind::document_edit:
            validate_payload(event, false, false, false);
            operation_kind = WorkspaceOperationKind::document_edit;
            document_kind = WorkspaceDocumentEventKind::edit;
            break;
        case WorkspaceLifecycleKind::boundary_activate:
            validate_payload(event, false, true, false);
            operation_kind = WorkspaceOperationKind::boundary_activate;
            break;
        case WorkspaceLifecycleKind::boundary_discard:
            validate_payload(event, false, false, true);
            operation_kind = WorkspaceOperationKind::boundary_discard;
            break;
        case WorkspaceLifecycleKind::boundary_finish:
            validate_payload(event, false, false, true);
            operation_kind = WorkspaceOperationKind::boundary_finish;
            document_kind = WorkspaceDocumentEventKind::edit;
            break;
        case WorkspaceLifecycleKind::undo:
        case WorkspaceLifecycleKind::redo: {
            const bool redo = event.kind == WorkspaceLifecycleKind::redo;
            auto transition = navigate_workspace_history(navigation, redo);
            validate_payload(event, true, false,
                transition.kind != WorkspaceOperationKind::document_edit);
            if (*event.target != transition.target) {
                throw std::invalid_argument("Workspace lifecycle navigation target does not match stack head");
            }
            if (transition.kind == WorkspaceOperationKind::document_edit ||
                transition.kind == WorkspaceOperationKind::boundary_finish) {
                document_kind = redo ? WorkspaceDocumentEventKind::redo : WorkspaceDocumentEventKind::undo;
            }
            navigation = std::move(transition.state);
            break;
        }
        case WorkspaceLifecycleKind::clear_redo:
            validate_payload(event, false, false, false);
            navigation = clear_workspace_redo(navigation);
            break;
        default:
            throw std::invalid_argument("Unknown workspace lifecycle event kind");
        }
        if (operation_kind) {
            navigation = record_workspace_operation(navigation, event.event_id, *operation_kind);
        }
        if (document_kind) {
            if (document_index == history.events.size()) {
                throw std::invalid_argument("Workspace lifecycle event has no corresponding Document event");
            }
            const auto& document_event = history.events[document_index++];
            // The two ledgers have independent sequence numbers: lifecycle-only
            // events consume no Document revision or Document-event sequence.
            if (document_event.event_id != event.event_id || document_event.kind != *document_kind ||
                document_event.before_revision != event.before_revision ||
                document_event.after_revision != event.after_revision) {
                throw std::invalid_argument("Workspace lifecycle Document projection does not match");
            }
        } else if (event.after_revision != event.before_revision) {
            throw std::invalid_argument("Lifecycle-only event changes the Document revision");
        }
        revision = event.after_revision;
    }
    if (document_index != history.events.size() || revision != snapshot.revision()) {
        throw std::invalid_argument("Workspace lifecycle history does not cover the Document head");
    }
    if (navigation != expected_navigation) {
        throw std::invalid_argument("Workspace lifecycle history does not match final navigation state");
    }
}
}
