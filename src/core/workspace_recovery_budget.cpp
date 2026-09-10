#include "sketch/workspace_recovery_budget.hpp"
#include "sketch/boundary_authoring_recovery_resource.hpp"
#include <set>
#include <stdexcept>

namespace sketch {
namespace {
void add(std::size_t& total, std::size_t amount, std::size_t limit) {
    if (!boundary_authoring_recovery_checked_add(total, amount, total) || total > limit)
        throw std::invalid_argument("Workspace recovery resource budget exceeded");
}
std::size_t multiply(std::size_t a, std::size_t b) {
    std::size_t result;
    if (!boundary_authoring_recovery_checked_multiply(a, b, result))
        throw std::invalid_argument("Workspace recovery accounting overflow");
    return result;
}
}
WorkspaceRecoveryUsage preflight_workspace_recovery(const DocumentSnapshot& document,
    const WorkspaceDocumentHistory& history, const std::vector<WorkspaceLifecycleEvent>& events,
    const WorkspaceNavigationState& navigation, const std::optional<BoundaryActiveRecovery>& active,
    const WorkspaceRetiredBoundaries& retired, const BoundaryAuthoringResourcePolicy& policy,
    const WorkspaceRecoveryLimits& limits) {
    WorkspaceRecoveryUsage u;
    add(u.events, events.size(), limits.max_events);
    add(u.document_revisions, document.history().size(), limits.max_document_revisions);
    if (history.events.size() > limits.max_events || navigation.operations.size() > limits.max_events ||
        navigation.undo_stack.size() > limits.max_events || navigation.redo_stack.size() > limits.max_events ||
        retired.size() > limits.max_inputs)
        throw std::invalid_argument("Workspace recovery collection budget exceeded");
    if (history.baseline.undo_stack.size() > limits.max_document_revisions ||
        history.baseline.redo_stack.size() > limits.max_document_revisions)
        throw std::invalid_argument("Workspace recovery baseline stack budget exceeded");
    // Reserve bounded schema/container overhead before walking payloads.
    add(u.encoded_bytes, multiply(events.size(), 1024), limits.max_encoded_bytes);
    add(u.json_values, multiply(events.size(), 64), limits.max_json_values);
    add(u.encoded_bytes, multiply(history.events.size(), 256), limits.max_encoded_bytes);
    add(u.encoded_bytes, multiply(document.history().size(), 256), limits.max_encoded_bytes);
    for (const auto count : {history.baseline.undo_stack.size(), history.baseline.redo_stack.size(),
                            navigation.undo_stack.size(), navigation.redo_stack.size()}) {
        add(u.encoded_bytes, multiply(count, 21), limits.max_encoded_bytes);
        add(u.json_values, count, limits.max_json_values);
    }
    const auto wire = [&](const BoundaryAuthoringResourceUsage& delta) {
        add(u.encoded_bytes, delta.encoded_bytes, limits.max_encoded_bytes);
        add(u.json_values, delta.json_values, limits.max_json_values);
        add(u.string_bytes, delta.string_bytes, limits.max_string_bytes);
    };
    const auto text = [&](const std::string& value) {
        add(u.string_bytes, value.size(), limits.max_string_bytes);
        add(u.encoded_bytes, multiply(value.size(), 6), limits.max_encoded_bytes);
        add(u.encoded_bytes, 2, limits.max_encoded_bytes);
        add(u.json_values, 1, limits.max_json_values);
    };
    const auto source = [&](const BoundaryRecoverySource& s) {
        text(s.document_id); text(s.authoring_digest); text(s.context.property_id);
        text(s.context.building_id); text(s.context.floor_id); text(s.context.layer_id);
    };
    std::set<const BoundaryActiveRecovery*> inputs;
    const auto input = [&](const BoundaryActiveRecovery* value) {
        if (!value) throw std::invalid_argument("Missing recovery input payload");
        if (!inputs.insert(value).second) return;
        add(u.inputs, 1, limits.max_inputs);
        add(u.encoded_bytes, 1024, limits.max_encoded_bytes);
        add(u.json_values, 64, limits.max_json_values);
        source(value->source);
        wire(detail::measure_authoring_recovery_json(value->extensions, policy));
        const auto usage = detail::measure_authoring_checkpoint_raw(value->checkpoint, policy);
        wire(usage);
        add(u.actions, usage.action_count, limits.max_actions);
        add(u.replay_work, usage.replay_work, limits.max_replay_work);
    };
    const auto archived = [&](const WorkspaceArchivedInput& value) {
        text(value.owner_event_id);
        if (value.finish_event_id) text(*value.finish_event_id);
        input(value.value.get());
    };
    text(history.baseline.document_id); text(history.baseline.source_digest);
    for (const auto& event : events) {
        text(event.event_id);
        if (event.target) if (const auto* id = std::get_if<std::string>(&event.target->identity)) text(*id);
        if (event.session) {
            source(event.session->source); text(event.session->identity_namespace);
            if (event.session->revised_from_namespace) text(*event.session->revised_from_namespace);
            if (event.session->revised_from_finish_event_id) text(*event.session->revised_from_finish_event_id);
        }
        if (event.input) archived(*event.input);
    }
    if (active) input(&*active);
    for (const auto& [name, value] : retired) {
        add(u.encoded_bytes, 256, limits.max_encoded_bytes);
        add(u.json_values, 16, limits.max_json_values);
        text(name); archived(value);
    }
    for (const auto& event : history.events) text(event.event_id);
    for (const auto& [id, kind] : navigation.operations) { (void)kind; text(id); }
    for (const auto* stack : {&navigation.undo_stack, &navigation.redo_stack})
        for (const auto& target : *stack) if (const auto* id = std::get_if<std::string>(&target.identity)) text(*id);
    // Bound the retained document before validators fork it. JSON preflight is
    // non-replaying; asset bytes are charged separately from JSON accounting.
    auto json_policy = policy;
    json_policy.max_encoded_bytes = limits.max_encoded_bytes;
    json_policy.max_json_values = limits.max_json_values;
    json_policy.max_string_bytes = limits.max_string_bytes;
    text(document.document_id()); text(document.read_only_reason());
    for (const auto& [name, revision] : document.named_revisions()) { (void)revision; text(name); }
    for (const auto& record : document.history()) {
        add(u.entity_rows, record.entities.size(), limits.max_entity_rows);
        add(u.asset_rows, record.assets.size(), limits.max_asset_rows);
        add(u.encoded_bytes, multiply(record.entities.size(), 256), limits.max_encoded_bytes);
        add(u.encoded_bytes, multiply(record.assets.size(), 256), limits.max_encoded_bytes);
        text(record.action); if (record.name) text(*record.name);
        add(u.encoded_bytes, multiply(record.undo_stack.size(), 21), limits.max_encoded_bytes);
        add(u.encoded_bytes, multiply(record.redo_stack.size(), 21), limits.max_encoded_bytes);
        add(u.json_values, record.undo_stack.size(), limits.max_json_values);
        add(u.json_values, record.redo_stack.size(), limits.max_json_values);
        for (const auto& [id, entity] : record.entities) {
            text(id); text(entity.id); text(entity.type);
            wire(detail::measure_authoring_recovery_json(entity.properties, json_policy));
            wire(detail::measure_authoring_recovery_json(entity.extensions, json_policy));
        }
        for (const auto& [id, asset] : record.assets) {
            text(id); text(asset.id); text(asset.media_type); text(asset.sha256);
            add(u.asset_bytes, asset.bytes.size(), limits.max_asset_bytes);
            wire(detail::measure_authoring_recovery_json(asset.metadata, json_policy));
        }
    }
    std::size_t base = u.entity_rows;
    add(base, u.asset_rows, limits.max_validation_work);
    add(base, u.replay_work, limits.max_validation_work);
    add(base, u.events, limits.max_validation_work);
    add(base, u.json_values, limits.max_validation_work);
    add(base, u.asset_bytes / 1024, limits.max_validation_work);
    std::size_t passes = u.events;
    add(passes, 8, limits.max_validation_work);
    add(u.validation_work, multiply(base, passes), limits.max_validation_work);
    return u;
}
void validate_workspace_recovery(const DocumentSnapshot& document,
    const WorkspaceDocumentHistory& history, const std::vector<WorkspaceLifecycleEvent>& events,
    const WorkspaceNavigationState& navigation, const std::optional<BoundaryActiveRecovery>& active,
    const WorkspaceRetiredBoundaries& retired, const BoundaryAuthoringResourcePolicy& policy,
    const WorkspaceRecoveryLimits& limits) {
    (void)preflight_workspace_recovery(document, history, events, navigation, active, retired, policy, limits);
    validate_workspace_lifecycle_slots(document, history, events, navigation, active, retired, policy);
}
}
