#include "sketch/phase_wall_canvas_proposal.hpp"

#include "sketch/phase_wall_replacement_request.hpp"

#include <stdexcept>
#include <utility>

namespace sketch {

std::optional<PhaseWallCanvasProposal> prepare_phase_wall_canvas_proposal(
    const DocumentSnapshot& source,
    const ConstraintAuthoringIntent& semantic,
    const std::function<std::string(std::string_view original_id)>& allocate_fresh_identity) {
    const auto requests = phase_wall_replacement_requests(source.entities(), semantic);
    if (requests.empty()) return std::nullopt;
    if (requests.size() != 1)
        throw std::invalid_argument("This edit replaces baseline walls in several design registries. "
            "Edit each building's alternative separately.");

    const auto& request = requests.front();
    const auto plan = inspect_phase_wall_replacement_plan(source.entities(), request.seed_wall_ids,
        request.registry_id, request.alternative_id);
    if (!plan.ready()) {
        std::string reason = "The proposed wall replacement has unsupported dependencies.";
        for (const auto& diagnostic : plan.diagnostics) {
            if (!diagnostic.blocking) continue;
            reason += "\n";
            if (!diagnostic.entity_id.empty()) reason += diagnostic.entity_id + ": ";
            reason += diagnostic.reason;
        }
        throw std::invalid_argument(reason);
    }

    // Bind only the actual captured source, before allocating any preview IDs.
    // Encoding admits semantic receipts without solving the original geometry.
    auto intent = make_phase_constraint_authoring_intent(source, semantic);
    if (!allocate_fresh_identity)
        throw std::invalid_argument("The proposed wall replacement needs a fresh identity allocator.");
    PhaseWallReplacementAuthoring replacement;
    replacement.registry_id = request.registry_id;
    replacement.alternative_id = request.alternative_id;
    replacement.seed_wall_ids = request.seed_wall_ids;
    const auto allocate = [&](const std::string& original_id) {
        // Discovery admits disjoint entity/child inventories. Keep the callback
        // at most once per identity even if that contract changes in the future.
        if (!replacement.identities.contains(original_id))
            replacement.identities.emplace(original_id, allocate_fresh_identity(original_id));
    };
    for (const auto& id : plan.required_entity_ids) allocate(id);
    for (const auto& id : plan.required_child_ids) allocate(id);
    intent.wall_replacement = encode_phase_wall_replacement_authoring(replacement);
    // The outer encoder delegates to the strict replacement decoder and applies
    // the canonical semantic/source envelope and resource admission checks.
    (void)encode_phase_constraint_authoring_intent(intent);
    auto physical = inspect_phase_wall_replacement_authoring(source, intent);
    std::set<std::string, std::less<>> proposed_walls;
    for (const auto& [original, proposed] : physical.replacement.original_to_proposed) {
        const auto owner = source.entities().find(original);
        if (owner != source.entities().end() && owner->second.type == "wall") proposed_walls.insert(proposed);
    }
    validate_active_wall_physical_dependencies(physical.edited_entities, proposed_walls);
    return PhaseWallCanvasProposal{std::move(intent), std::move(physical)};
}

} // namespace sketch
