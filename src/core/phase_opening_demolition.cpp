#include "sketch/phase_opening_demolition.hpp"

#include "sketch/constraint_integrity.hpp"
#include "sketch/constraint_phase_scope.hpp"
#include "sketch/door_operation.hpp"
#include "sketch/document_wall.hpp"
#include "sketch/model_phases.hpp"
#include "sketch/phase_constraint_authoring.hpp"
#include "sketch/opening_assembly.hpp"
#include "sketch/phase_wall_profile_edit.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>
#include <utility>

namespace sketch {
namespace {
constexpr std::size_t target_limit = 1000;
using Entities = std::map<std::string,Entity,std::less<>>;
struct DemolitionDerivation {
    PhaseOpeningDemolitionIntent intent;
    Entities entities;
};

[[noreturn]] void invalid(const std::string& reason) {
    throw std::invalid_argument("Opening demolition: " + reason);
}

// The shared physical factories admit fit, assemblies and joins. Admit retained
// aliases too, so removing a cut cannot conceal conflicting source authority.
void admit_descriptor(const Entity& opening) {
    for (const auto& [canonical, alias] : {
            std::pair{"offset_m", "offset"}, std::pair{"width_m", "width"},
            std::pair{"sill_m", "sill"}, std::pair{"height_m", "height"}}) {
        const auto first = opening.properties.find(canonical);
        const auto second = opening.properties.find(alias);
        const auto chosen = first != opening.properties.end() ? first : second;
        if (chosen == opening.properties.end() || !chosen->is_number() ||
            !std::isfinite(chosen->get<double>()))
            invalid("opening " + opening.id + " has a missing or invalid " + canonical);
        if (second != opening.properties.end() &&
            (!second->is_number() || second->get<double>() != chosen->get<double>()))
            invalid("opening " + opening.id + " has conflicting " + canonical + " aliases");
    }
    const auto kind = opening.properties.find("opening_kind");
    std::optional<OpeningAssemblyKind> family;
    if (kind != opening.properties.end()) {
        if (!kind->is_string()) invalid("opening " + opening.id + " kind must be a string");
        const auto& text = kind->get_ref<const std::string&>();
        family = parse_opening_assembly_kind(text);
        if (!family && text != "opening") invalid("opening " + opening.id + " kind is unsupported");
    }
    if (opening.properties.contains("opening_assembly")) {
        const auto assembly = parse_opening_assembly(opening.properties.at("opening_assembly"));
        if (kind != opening.properties.end() && (!family || *family != assembly.kind))
            invalid("opening " + opening.id + " kind and assembly disagree");
        family = assembly.kind;
    }
    if (opening.properties.contains("door_operation")) {
        (void)decode_door_operation(opening.properties.at("door_operation"));
        if (!family || *family != OpeningAssemblyKind::door)
            invalid("opening " + opening.id + " operation requires an existing door assembly");
    }
}
std::optional<DemolitionDerivation> derive_demolition(
    const Entities& entities, const std::vector<std::string>& selected_opening_ids) {
    if (selected_opening_ids.size() > target_limit)
        invalid("selected opening count exceeds the target budget");
    if (selected_opening_ids.empty()) return std::nullopt;

    const auto scope = constraint_phase_scope(entities);
    std::map<std::string, const PhysicalWallPhaseState*, std::less<>> memberships;
    for (const auto& registry : scope.registries)
        for (const auto& id : registry.registered_entity_ids)
            if (!memberships.emplace(id, &registry).second)
                invalid("selected source has overlapping actual phase ownership");

    std::set<std::string, std::less<>> selected, hosts, demolitions;
    const PhysicalWallPhaseState* destination = nullptr;
    std::size_t unqualified = 0;
    for (const auto& id : selected_opening_ids) {
        if (id.empty() || !selected.insert(id).second)
            invalid("selection contains a blank or duplicate opening identity");
        const auto found = entities.find(id);
        if (found == entities.end() || found->second.type != "opening") {
            ++unqualified;
            continue;
        }
        const auto& opening = found->second;
        if (opening.id != id) invalid("opening identity differs from its actual entity key: " + id);
        std::string host_id, diagnostic;
        if (!read_document_wall_id(opening, host_id, diagnostic))
            invalid("opening " + id + " has no exact existing host: " + diagnostic);
        const auto host = entities.find(host_id);
        if (host == entities.end() || host->second.id != host_id || host->second.type != "wall")
            invalid("opening " + id + " host is missing or inconsistent: " + host_id);
        const auto membership = memberships.find(id);
        if (membership == memberships.end()) {
            const auto host_membership = memberships.find(host_id);
            if (host_membership != memberships.end() && host_membership->second->alternative_id) {
                const auto model = ModelPhases::from_json(
                    entities.at(host_membership->second->registry_id).properties.at("model"));
                if (std::binary_search(model.baseline_ids().begin(), model.baseline_ids().end(), host_id))
                    invalid("opening " + id + " is unregistered although host " + host_id +
                        " is shared baseline in registry " + host_membership->second->registry_id +
                        "; explicitly register the original opening as baseline before demolition");
            }
            ++unqualified;
            continue;
        }
        const auto* registry = membership->second;
        const auto model = ModelPhases::from_json(entities.at(registry->registry_id).properties.at("model"));
        if (!registry->alternative_id ||
            !std::binary_search(model.baseline_ids().begin(), model.baseline_ids().end(), id)) {
            ++unqualified;
            continue;
        }
        if (scope.inactive_owner_ids.contains(id) || scope.inactive_owner_ids.contains(host_id))
            invalid("opening " + id + " and its host must both be active in the actual saved design");
        const auto state = registry->states.find(id);
        if (state == registry->states.end() || state->second != ModelPhase::existing ||
            model.active_alternative() != registry->alternative_id)
            invalid("opening " + id + " no longer has its captured active baseline membership");
        if (destination && destination != registry)
            invalid("demolish openings from one actual phase registry at a time");
        admit_descriptor(opening);
        destination = registry;
        hosts.insert(host_id);
        demolitions.insert(id);
    }
    if (!destination) return std::nullopt;
    if (unqualified) invalid("select only active baseline openings for this demolition; delete other objects separately");

    // Admit the actual source with every still-active sibling cut before the
    // removed opening becomes inactive and could otherwise escape fit checks.
    validate_active_wall_physical_dependencies(entities, hosts);
    const auto& original_registry = entities.at(destination->registry_id);
    const auto phases = ModelPhases::from_json(original_registry.properties.at("model"));
    const auto alternative = std::find_if(phases.alternatives().begin(), phases.alternatives().end(),
        [&](const auto& value) { return value.id == *destination->alternative_id; });
    if (alternative == phases.alternatives().end()) invalid("saved active alternative is missing");
    demolitions.insert(alternative->demolished_ids.begin(), alternative->demolished_ids.end());
    auto amended_alternative = *alternative;
    amended_alternative.demolished_ids.assign(demolitions.begin(), demolitions.end());
    const auto admitted_model = phases.with_updated_alternative(std::move(amended_alternative)).to_json();
    auto replacement = original_registry;
    // ModelPhases canonicalizes vectors. Retain the exact source representation
    // of every other field/alternative, changing only this demolition list.
    replacement.properties["model"] = original_registry.properties.at("model");
    for (auto& row : replacement.properties.at("model").at("alternatives"))
        if (row.at("id") == alternative->id) row["demolished_ids"] = demolitions;
    if (ModelPhases::from_json(replacement.properties.at("model")).to_json() != admitted_model)
        invalid("registry patch differs from its typed alternative update");

    auto stage = entities;
    stage.at(destination->registry_id) = replacement;
    const auto after_scope = constraint_phase_scope(stage);
    for (const auto& id : selected)
        if (!after_scope.inactive_owner_ids.contains(id))
            invalid("demolition failed to park the selected actual opening membership");
    validate_active_wall_physical_dependencies(stage, hosts);
    if (const auto unsupported = validate_active_phase_constraint_integrity(stage)) invalid(*unsupported);
    PhaseOpeningDemolitionIntent intent{destination->registry_id, *destination->alternative_id,
        std::vector<std::string>(selected.begin(), selected.end())};
    return DemolitionDerivation{std::move(intent), std::move(stage)};
}
} // namespace

nlohmann::json encode_phase_opening_demolition_intent(const PhaseOpeningDemolitionIntent& intent) {
    const auto identity = [](const std::string& value) {
        if (value.empty() || value.size() > 128 || !std::all_of(value.begin(), value.end(), [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                c == '-' || c == '_' || c == '.' || c == ':';
        })) invalid("proof identity is invalid");
    };
    identity(intent.registry_id); identity(intent.alternative_id);
    if (intent.opening_ids.empty() || intent.opening_ids.size() > target_limit ||
        !std::is_sorted(intent.opening_ids.begin(), intent.opening_ids.end()) ||
        std::adjacent_find(intent.opening_ids.begin(), intent.opening_ids.end()) != intent.opening_ids.end())
        invalid("proof opening inventory must be nonempty, unique and sorted");
    for (const auto& id : intent.opening_ids) identity(id);
    return {{"version", 1}, {"registry_id", intent.registry_id},
        {"alternative_id", intent.alternative_id}, {"opening_ids", intent.opening_ids}};
}

PhaseOpeningDemolitionIntent decode_phase_opening_demolition_intent(const nlohmann::json& value) {
    if (!value.is_object() || value.size() != 4 || !value.contains("version") ||
        !value.at("version").is_number_integer() || value.at("version") != 1 ||
        !value.contains("registry_id") || !value.at("registry_id").is_string() ||
        !value.contains("alternative_id") || !value.at("alternative_id").is_string() ||
        !value.contains("opening_ids") || !value.at("opening_ids").is_array() ||
        value.at("opening_ids").size() > target_limit || value.dump().size() > 256 * 1024)
        invalid("proof fields or resource bounds are invalid");
    PhaseOpeningDemolitionIntent result{value.at("registry_id").get<std::string>(),
        value.at("alternative_id").get<std::string>(), {}};
    for (const auto& id : value.at("opening_ids")) {
        if (!id.is_string()) invalid("proof opening identity must be a string");
        result.opening_ids.push_back(id.get<std::string>());
    }
    if (encode_phase_opening_demolition_intent(result) != value) invalid("proof is not canonical");
    return result;
}

Entities replay_phase_opening_demolition_entities(const Entities& source,
    const PhaseOpeningDemolitionIntent& intent) {
    (void)encode_phase_opening_demolition_intent(intent);
    auto derived = derive_demolition(source, intent.opening_ids);
    if (!derived || derived->intent != intent)
        invalid("proof differs from actual saved active baseline membership");
    return std::move(derived->entities);
}

std::optional<ApplyBoundaryConstraintChanges> phase_opening_demolition_command(
    const DocumentSnapshot& source, const std::vector<std::string>& selected_opening_ids) {
    if (!source.is_editable())
        throw DocumentError(DocumentErrorCode::read_only, source.read_only_reason());
    const auto derived = derive_demolition(source.entities(), selected_opening_ids);
    if (!derived) return std::nullopt;
    ConstraintAuthoringIntent semantic;
    semantic.message = "Demolish hosted openings in active alternative";
    auto intent = make_phase_constraint_authoring_intent(source, semantic);
    intent.opening_demolition = encode_phase_opening_demolition_intent(derived->intent);
    ApplyBoundaryConstraintChanges command;
    command.expected_revision = source.revision();
    command.message = semantic.message;
    command.phase_constraint_authoring_completion = true;
    command.phase_constraint_authoring_intent = encode_phase_constraint_authoring_intent(intent);
    // The actual captured history grants final schema/relationship/room and
    // policy admission. Detached entity replay never fabricates a Snapshot.
    const auto preview = Document::preview_command(source, Command{command});
    if (preview.entities() != derived->entities || preview.assets() != source.assets() || !preview.is_editable())
        invalid("final preview changed unrelated state or made the project read-only");
    return command;
}

} // namespace sketch
