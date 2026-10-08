#include "sketch/phase_wall_canvas_proposal.hpp"

#include "sketch/constraint_phase_scope.hpp"
#include "sketch/document_wall.hpp"
#include "sketch/model_phases.hpp"
#include "sketch/phase_wall_replacement_request.hpp"

#include <map>
#include <set>
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

std::optional<PhaseWallCanvasProposal> prepare_phase_hosted_opening_canvas_proposal(
    const DocumentSnapshot& source,
    const std::vector<HostedOpeningProfileEditIntent>& profiles,
    const std::function<std::string(std::string_view original_id)>& allocate_fresh_identity) {
    if (profiles.empty()) return std::nullopt;
    if (profiles.size() > 2048)
        throw std::invalid_argument("The proposed opening edit exceeds its target budget.");
    const auto& entities = source.entities();
    const auto scope = constraint_phase_scope(entities);
    std::map<std::string, const PhysicalWallPhaseState*, std::less<>> shared_hosts;
    for (const auto& registry : scope.registries) {
        if (!registry.alternative_id) continue;
        const auto model = ModelPhases::from_json(entities.at(registry.registry_id).properties.at("model"));
        for (const auto& id : model.baseline_ids()) {
            if (entities.at(id).type == "wall" && !scope.inactive_owner_ids.contains(id))
                shared_hosts.emplace(id, &registry);
        }
    }
    PhaseWallReplacementAuthoring replacement;
    std::set<std::string, std::less<>> openings, hosts;
    bool nonshared = false;
    for (const auto& profile : profiles) {
        // Admit only the typed request here. Do not replay a profile or solve
        // original geometry before discovering its actual baseline authority.
        (void)encode_hosted_opening_profile_edit_intent(profile);
        if (!openings.insert(profile.opening_id).second)
            throw std::invalid_argument("The proposed opening edit contains duplicate targets.");
        const auto opening = entities.find(profile.opening_id);
        const auto wall = entities.find(profile.wall_id);
        if (opening == entities.end() || opening->second.id != opening->first ||
            opening->second.type != "opening" || wall == entities.end() ||
            wall->second.id != wall->first || wall->second.type != "wall")
            throw std::invalid_argument("The proposed opening edit requires its actual opening and host wall.");
        std::string actual_host, error;
        if (!read_document_wall_id(opening->second, actual_host, error) || actual_host != profile.wall_id)
            throw std::invalid_argument("The proposed opening edit must retain its exact original host.");
        if (scope.inactive_owner_ids.contains(profile.opening_id) || scope.inactive_owner_ids.contains(profile.wall_id))
            throw std::invalid_argument("The proposed opening and host must be active in the saved design.");

        const auto shared = shared_hosts.find(profile.wall_id);
        if (shared == shared_hosts.end()) {
            nonshared = true;
            continue;
        }
        const auto* shared_registry = shared->second;
        if (!replacement.registry_id.empty() && replacement.registry_id != shared_registry->registry_id)
            throw std::invalid_argument("This opening edit replaces baseline walls in several design registries. "
                "Edit each building's alternative separately.");
        replacement.registry_id = shared_registry->registry_id;
        replacement.alternative_id = *shared_registry->alternative_id;
        hosts.insert(profile.wall_id);
    }
    if (hosts.empty()) return std::nullopt;
    if (nonshared)
        throw std::invalid_argument("Edit shared-baseline and other openings in separate operations.");
    replacement.seed_wall_ids.assign(hosts.begin(), hosts.end());
    const auto plan = inspect_phase_wall_replacement_plan(entities, replacement.seed_wall_ids,
        replacement.registry_id, replacement.alternative_id);
    if (!plan.ready()) {
        std::string reason = "The proposed opening replacement has unsupported dependencies.";
        for (const auto& diagnostic : plan.diagnostics) {
            if (!diagnostic.blocking) continue;
            reason += "\n";
            if (!diagnostic.entity_id.empty()) reason += diagnostic.entity_id + ": ";
            reason += diagnostic.reason;
        }
        throw std::invalid_argument(reason);
    }

    // Every authority field comes from the original actual capture. The empty
    // semantic record cannot author the original opening or original wall.
    auto intent = make_phase_constraint_authoring_intent(source, ConstraintAuthoringIntent{});
    if (!allocate_fresh_identity)
        throw std::invalid_argument("The proposed opening replacement needs a fresh identity allocator.");
    const auto allocate = [&](const std::string& original_id) {
        if (!replacement.identities.contains(original_id))
            replacement.identities.emplace(original_id, allocate_fresh_identity(original_id));
    };
    for (const auto& id : plan.required_entity_ids) allocate(id);
    for (const auto& id : plan.required_child_ids) allocate(id);
    replacement.opening_profiles = profiles;
    intent.wall_replacement = encode_phase_wall_replacement_authoring(replacement);
    (void)encode_phase_constraint_authoring_intent(intent);
    auto physical = inspect_phase_wall_replacement_authoring(source, intent);
    std::set<std::string, std::less<>> proposed_walls;
    for (const auto& [original, proposed] : physical.replacement.original_to_proposed) {
        const auto owner = entities.find(original);
        if (owner != entities.end() && owner->second.type == "wall") proposed_walls.insert(proposed);
    }
    // Use the entire copied closure, including hosts/dependencies outside the
    // visible canvas; shared factories also admit sibling cuts and active joins.
    validate_active_wall_physical_dependencies(physical.edited_entities, proposed_walls);
    return PhaseWallCanvasProposal{std::move(intent), std::move(physical)};
}

} // namespace sketch
