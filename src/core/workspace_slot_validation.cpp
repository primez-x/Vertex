#include "sketch/workspace_slot_validation.hpp"

#include <stdexcept>
#include <utility>

namespace sketch {
namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::invalid_argument(message);
}
bool semantics(const BoundaryActiveRecovery& a, const BoundaryActiveRecovery& b) {
    const auto& x = a.checkpoint;
    const auto& y = b.checkpoint;
    return a.source == b.source && a.extensions.dump() == b.extensions.dump() &&
        x.version == y.version && x.replay_version == y.replay_version && x.mode == y.mode &&
        x.identity_namespace == y.identity_namespace && x.options == y.options &&
        x.actions == y.actions && x.history_position == y.history_position &&
        x.counters == y.counters && x.extensions.dump() == y.extensions.dump();
}
bool counters_at_least(const BoundaryAuthoringCounters& a, const BoundaryAuthoringCounters& b) {
    return a.next_boundary_id >= b.next_boundary_id && a.next_vertex_id >= b.next_vertex_id &&
        a.next_segment_id >= b.next_segment_id && a.next_dimension_id >= b.next_dimension_id;
}
BoundaryActiveRecovery effective(const WorkspaceArchivedInput& input) {
    require(bool(input.value), "Slot input payload is missing");
    auto result = *input.value;
    if (input.pointer_override) result.checkpoint.pointer = *input.pointer_override;
    return result;
}
bool pointer_equal(const std::optional<Vec2>& a, const std::optional<Vec2>& b) {
    return a.has_value() == b.has_value() && (!a || (a->x == b->x && a->y == b->y));
}
bool exact(const WorkspaceArchivedInput& a, const WorkspaceArchivedInput& b) {
    return a.value && b.value && a.owner_event_id == b.owner_event_id &&
        a.status == b.status && a.finish_event_id == b.finish_event_id &&
        a.pointer_override.has_value() == b.pointer_override.has_value() &&
        (!a.pointer_override || pointer_equal(*a.pointer_override, *b.pointer_override)) &&
        semantics(*a.value, *b.value) && pointer_equal(a.value->checkpoint.pointer, b.value->checkpoint.pointer);
}
struct Session {
    WorkspaceSessionIdentity identity;
    BoundaryAuthoringCounters floor;
};
struct Active {
    std::string name;
    std::optional<BoundaryActiveRecovery> observed;
    bool semantic_changes = false;
    bool pointer_changes = false;
};
struct Slots {
    std::map<std::string, Session, std::less<>> sessions;
    std::optional<Active> active;
    WorkspaceRetiredBoundaries retired;

    void window(Revision revision, bool redo_empty) {
        if (active && sessions.at(active->name).identity.source.revision == revision) {
            active->pointer_changes = true;
            active->semantic_changes |= redo_empty;
        }
    }
    void observe(const BoundaryActiveRecovery& value) {
        require(active.has_value(), "Input removal has no active slot");
        auto& slot = *active;
        auto& session = sessions.at(slot.name);
        const auto& c = value.checkpoint;
        require(c.identity_namespace == slot.name && c.mode == session.identity.mode &&
            value.source == session.identity.source, "Active input changes session identity");
        require(counters_at_least(c.counters, session.floor), "Session input rewinds counters");
        if (slot.observed) {
            require(slot.semantic_changes || semantics(value, *slot.observed),
                "Active semantics changed without a permitted checkpoint update");
            require(slot.pointer_changes || pointer_equal(c.pointer, slot.observed->checkpoint.pointer),
                "Active pointer changed without a permitted checkpoint update");
        }
        session.floor = c.counters;
        slot.observed = value;
        slot.semantic_changes = slot.pointer_changes = false;
    }
    void restore(const WorkspaceArchivedInput& input) {
        auto value = effective(input);
        const auto& name = value.checkpoint.identity_namespace;
        const auto session = sessions.find(name);
        require(session != sessions.end(), "Restored input has no activation");
        require(!retired.contains(name), "Restoration overwrites retired input");
        require(value.source == session->second.identity.source &&
            value.checkpoint.mode == session->second.identity.mode &&
            counters_at_least(value.checkpoint.counters, session->second.floor),
            "Restored input violates session identity or counter floor");
        session->second.floor = value.checkpoint.counters;
        if (input.status == WorkspaceInputStatus::retired) {
            require(input.finish_event_id.has_value() && (!active || active->name != name),
                "Retired restoration conflicts with active input");
            retired.emplace(name, input);
        } else {
            require(input.status == WorkspaceInputStatus::active && !input.finish_event_id && !active,
                "Active restoration conflicts with slot state");
            active = Active{name, std::move(value)};
        }
    }
};
}

void validate_workspace_lifecycle_slots(const DocumentSnapshot& document,
    const WorkspaceDocumentHistory& history, const std::vector<WorkspaceLifecycleEvent>& events,
    const WorkspaceNavigationState& expected_navigation,
    const std::optional<BoundaryActiveRecovery>& expected_active,
    const WorkspaceRetiredBoundaries& expected_retired,
    const BoundaryAuthoringResourcePolicy& policy) {
    validate_workspace_lifecycle_order(document, history, events, expected_navigation);
    validate_workspace_archival_inputs(events, policy);
    validate_workspace_recovery_sources(document, events, expected_active);
    validate_workspace_finish_deltas(document, events, policy);
    if (expected_active) (void)encode_boundary_active_recovery(*expected_active, policy);

    Slots slots;
    auto navigation = initialize_workspace_navigation(document, history.baseline);
    std::map<std::string, WorkspaceArchivedInput, std::less<>> last;
    std::map<std::string, std::string, std::less<>> activations;
    const auto previous = [&](const std::string& id) -> const WorkspaceArchivedInput& {
        const auto found = last.find(id);
        require(found != last.end(), "Navigation restoration input is missing");
        return found->second;
    };
    const auto remove_active = [&](const WorkspaceArchivedInput& input) {
        require(input.status == WorkspaceInputStatus::active && !input.finish_event_id,
            "Active removal records retired input");
        slots.observe(effective(input));
        slots.active.reset();
    };
    for (const auto& event : events) {
        slots.window(event.before_revision, navigation.redo_stack.empty());
        if (event.kind == WorkspaceLifecycleKind::boundary_activate) {
            const auto& identity = *event.session;
            require(!slots.active && !slots.sessions.contains(identity.identity_namespace),
                "Activation occupies an active slot or reuses a session namespace");
            require(!identity.identity_namespace.empty() && identity.identity_namespace.size() <= 128 &&
                identity.identity_namespace.find('\0') == std::string::npos &&
                counters_at_least(identity.initial_counters, BoundaryAuthoringCounters{}),
                "Activation identity or initial counters are invalid");
            require(identity.mode == BoundaryAuthoringMode::draw_first ||
                identity.mode == BoundaryAuthoringMode::define_first, "Activation mode is invalid");
            require(identity.revised_from_namespace.has_value() ==
                identity.revised_from_finish_event_id.has_value(), "Revise provenance is incomplete");
            if (identity.revised_from_namespace) {
                const auto origin = slots.retired.find(*identity.revised_from_namespace);
                require(origin != slots.retired.end(), "Revise origin is not retired");
                const auto& old = *origin->second.value;
                require(origin->second.finish_event_id == identity.revised_from_finish_event_id &&
                    identity.mode == old.checkpoint.mode && identity.source.context == old.source.context &&
                    identity.initial_counters == old.checkpoint.counters,
                    "Revise activation disagrees with retired provenance");
            }
            slots.sessions.emplace(identity.identity_namespace, Session{identity, identity.initial_counters});
            slots.active = Active{identity.identity_namespace, std::nullopt};
            activations.emplace(event.event_id, identity.identity_namespace);
        } else if (event.kind == WorkspaceLifecycleKind::boundary_discard ||
                   event.kind == WorkspaceLifecycleKind::boundary_finish) {
            if (event.kind == WorkspaceLifecycleKind::boundary_finish) {
                require(event.input->value->source.revision == event.before_revision,
                    "Finish input source is stale");
                const auto session = BoundaryAuthoringSession::from_recovery_checkpoint(
                    event.input->value->checkpoint, policy);
                require(session.phase() == BoundaryAuthoringPhase::completed && !session.accepted_chains().empty(),
                    "Finish input is not completed");
            }
            remove_active(*event.input);
            last.insert_or_assign(event.event_id, *event.input);
        } else if (event.kind == WorkspaceLifecycleKind::clear_redo) {
            require(slots.active && !navigation.redo_stack.empty() &&
                slots.sessions.at(slots.active->name).identity.source.revision == event.before_revision,
                "Redo barrier has no permitted semantic checkpoint update");
            slots.active->semantic_changes = true;
        } else if (event.kind == WorkspaceLifecycleKind::undo || event.kind == WorkspaceLifecycleKind::redo) {
            const bool redo = event.kind == WorkspaceLifecycleKind::redo;
            const auto transition = navigate_workspace_history(navigation, redo);
            if (transition.kind != WorkspaceOperationKind::document_edit) {
                const auto& id = std::get<std::string>(transition.target.identity);
                const auto& input = *event.input;
                if (transition.kind == WorkspaceOperationKind::boundary_activate) {
                    if (redo) {
                        require(exact(input, previous(id)), "Activation redo substitutes restoration input");
                        slots.restore(input);
                    } else {
                        const auto& name = activations.at(id);
                        require(input.value->checkpoint.identity_namespace == name,
                            "Activation undo removes another session");
                        if (slots.active && slots.active->name == name) remove_active(input);
                        else {
                            const auto found = slots.retired.find(name);
                            require(found != slots.retired.end() && exact(input, found->second),
                                "Activation undo does not match retired slot");
                            slots.retired.erase(found);
                        }
                    }
                } else if (transition.kind == WorkspaceOperationKind::boundary_discard) {
                    if (redo) {
                        require(slots.active && slots.active->observed &&
                            semantics(effective(input), effective(previous(id))),
                            "Discard redo changes restored semantics");
                        remove_active(input);
                    } else {
                        require(exact(input, previous(id)) && input.status == WorkspaceInputStatus::active,
                            "Discard undo substitutes restoration input");
                        slots.restore(input);
                    }
                } else {
                    auto expected = previous(id);
                    expected.status = WorkspaceInputStatus::retired;
                    expected.finish_event_id = id;
                    require(exact(input, expected), "Finish navigation substitutes retired input");
                    if (redo) {
                        const auto found = slots.retired.find(input.value->checkpoint.identity_namespace);
                        require(found != slots.retired.end() && exact(found->second, input),
                            "Finish redo has no matching retired slot");
                        slots.retired.erase(found);
                    } else slots.restore(input);
                }
                last.insert_or_assign(id, input);
            }
            navigation = transition.state;
            continue;
        }
        switch (event.kind) {
        case WorkspaceLifecycleKind::document_edit:
            navigation = record_workspace_operation(navigation, event.event_id, WorkspaceOperationKind::document_edit); break;
        case WorkspaceLifecycleKind::boundary_activate:
            navigation = record_workspace_operation(navigation, event.event_id, WorkspaceOperationKind::boundary_activate); break;
        case WorkspaceLifecycleKind::boundary_discard:
            navigation = record_workspace_operation(navigation, event.event_id, WorkspaceOperationKind::boundary_discard); break;
        case WorkspaceLifecycleKind::boundary_finish:
            navigation = record_workspace_operation(navigation, event.event_id, WorkspaceOperationKind::boundary_finish); break;
        case WorkspaceLifecycleKind::clear_redo: navigation = clear_workspace_redo(navigation); break;
        default: break;
        }
    }
    slots.window(document.revision(), navigation.redo_stack.empty());
    require(slots.active.has_value() == expected_active.has_value(), "Final active slot presence disagrees");
    if (expected_active) slots.observe(*expected_active);
    require(slots.retired.size() == expected_retired.size(), "Final retired slot count disagrees");
    for (const auto& [name, input] : expected_retired) {
        const auto found = slots.retired.find(name);
        require(found != slots.retired.end() && exact(input, found->second) &&
            input.value->checkpoint.identity_namespace == name,
            "Final retired slot disagrees with replay");
    }
    // No hypothetical checkpoint updates during this traversal: all currently
    // reachable redo operations must be executable from the supplied slots.
    while (!navigation.redo_stack.empty()) {
        auto transition = navigate_workspace_history(navigation, true);
        if (transition.kind != WorkspaceOperationKind::document_edit) {
            const auto& id = std::get<std::string>(transition.target.identity);
            const auto& input = previous(id);
            if (transition.kind == WorkspaceOperationKind::boundary_activate) slots.restore(input);
            else if (transition.kind == WorkspaceOperationKind::boundary_discard) {
                require(slots.active && slots.active->observed && input.status == WorkspaceInputStatus::active &&
                    semantics(*slots.active->observed, effective(input)),
                    "Reachable discard redo conflicts with active slot");
                slots.active.reset();
            } else {
                const auto found = slots.retired.find(input.value->checkpoint.identity_namespace);
                require(found != slots.retired.end() && found->second.finish_event_id == id &&
                    exact(found->second, input), "Reachable finish redo conflicts with retired slot");
                slots.retired.erase(found);
            }
        }
        navigation = std::move(transition.state);
    }
}
}
