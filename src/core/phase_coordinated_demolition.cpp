#include "sketch/phase_coordinated_demolition.hpp"

#include "sketch/architectural_object_removal.hpp"
#include "sketch/architectural_selection_removal.hpp"
#include "sketch/assembly_model.hpp"
#include "sketch/constraint_phase_scope.hpp"
#include "sketch/phase_opening_demolition.hpp"
#include "sketch/phase_roof_replacement.hpp"
#include "sketch/phase_slab_demolition.hpp"
#include "sketch/phase_stair_demolition.hpp"
#include "sketch/phase_stair_demolition_retirement.hpp"
#include "sketch/phase_structural_replacement.hpp"
#include "sketch/roof_join_semantics.hpp"
#include "sketch/stair_semantics.hpp"

#include <Standard_Failure.hxx>

#include <algorithm>
#include <array>
#include <cmath>
#include <set>
#include <stdexcept>
#include <utility>

namespace sketch {
namespace {
using Json = nlohmann::json;
using Entities = std::map<std::string, Entity, std::less<>>;
constexpr std::size_t proof_limit = 1024 * 1024;
constexpr std::size_t source_limit = 64 * 1024 * 1024;
constexpr std::size_t node_limit = 4 * 1024 * 1024;
constexpr std::array<const char*, 5> families{
    "opening_authoring", "roof_authoring", "slab_authoring", "structural_authoring", "stair_authoring"};

[[noreturn]] void invalid(const std::string& reason) {
    throw std::invalid_argument("Coordinated demolition: " + reason);
}

void identity(const std::string& value) {
    if (value.empty() || value.size() > 128 || !std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == ':';
    })) invalid("identity must contain 1..128 supported ASCII characters");
}

// Validate raw JSON before any dump, recursive family codec or native replay.
// The aggregate counters span all children or all actual source envelopes.
struct JsonBudget {
    std::size_t byte_limit;
    bool proof;
    std::size_t nodes{}, bytes{};

    void text(const std::string& value) {
        if (value.size() > byte_limit - bytes) invalid("aggregate JSON string byte budget exceeded");
        bytes += value.size();
    }
    void read(const Json& value, std::size_t depth = 0) {
        if (depth > 64 || ++nodes > node_limit) invalid("aggregate JSON node/nesting budget exceeded");
        if (value.is_binary() || value.is_discarded()) invalid("raw JSON cannot contain binary or discarded values");
        if (value.is_number_float() && !std::isfinite(value.get<double>())) invalid("raw JSON number must be finite");
        if (value.is_string()) text(value.get_ref<const std::string&>());
        if (proof && value.is_structured() && value.size() > 4096) invalid("proof collection budget exceeded");
        if (value.is_object()) {
            for (const auto& [key, child] : value.items()) {
                text(key);
                // Nested coordination is refused before a historical decoder
                // can enter either current or future coordinated recursion.
                if (proof && (key == "coordinated_replacements" || key == "coordinated_demolition"))
                    invalid("nested coordinated authoring is forbidden");
                read(child, depth + 1);
            }
        } else if (value.is_array()) {
            for (const auto& child : value) read(child, depth + 1);
        }
    }
};

void source_budget(const Entities& source) {
    if (source.size() > 250000) invalid("source entity budget exceeded");
    JsonBudget budget{source_limit, false};
    std::size_t serialized{};
    const auto reserve = [&](std::size_t bytes) {
        if (bytes > source_limit - serialized) invalid("serialized source map byte budget exceeded");
        serialized += bytes;
    };
    for (const auto& [id, entity] : source) {
        identity(id);
        if (entity.id != id || !entity.properties.is_object() || !entity.extensions.is_object())
            invalid("source must contain actual identified entity envelopes: " + id);
        budget.text(id); budget.text(entity.type);
        budget.read(entity.properties); budget.read(entity.extensions);
        // Validate opaque type UTF-8 as well as all raw payloads. Reserve full
        // entity/map framing; numeric arrays also consume serialized bytes.
        reserve(2 * id.size() + Json(entity.type).dump().size() + 128);
        reserve(entity.properties.dump().size());
        reserve(entity.extensions.dump().size());
    }
}

void message_only(const ConstraintAuthoringIntent& intent) {
    if (intent.wall_resize || intent.wall_geometry_move || intent.wall_curve_construction ||
        intent.boundary_resize || intent.boundary_vertex_move || intent.exterior_corner_move ||
        intent.exterior_segment_resize || intent.exterior_segment_arc || intent.measured_stroke_resize ||
        intent.measured_stroke_vertex_move || intent.measured_stroke_transform || intent.joint_translation ||
        !intent.relation_mutations.empty() || intent.relation_anchor || !intent.relation_move_connected_walls)
        invalid("demolition permits only a semantic message without geometry or relationship authority");
    if (intent.message.size() > 4096 || intent.message.find('\0') != std::string::npos)
        invalid("semantic message budget exceeded");
    (void)Json(intent.message).dump();
}

void same_binding(const PhaseConstraintAuthoringIntent& child, const PhaseConstraintAuthoringIntent& enclosing) {
    if (child.expected_revision != enclosing.expected_revision ||
        child.source_snapshot_digest != enclosing.source_snapshot_digest ||
        child.source_authoring_digest != enclosing.source_authoring_digest ||
        child.source_entities_digest != enclosing.source_entities_digest ||
        child.source_saved_revision != enclosing.source_saved_revision ||
        child.phase_selections.dump() != enclosing.phase_selections.dump())
        invalid("child differs from the enclosing actual source binding and raw saved choices");
}

struct DemolitionChoice {
    std::string registry_id;
    std::string alternative_id;
    std::vector<std::string> roots;
};

DemolitionChoice choice(const PhaseConstraintAuthoringIntent& child, std::size_t family, const Json& wire) {
    const auto& version = wire.at("version");
    if (family == 0 && version == 3) {
        const auto leaf = decode_phase_opening_demolition_intent(child.opening_demolition);
        return {leaf.registry_id, leaf.alternative_id, leaf.opening_ids};
    }
    if (family == 1 && version == 4) {
        const auto leaf = decode_phase_roof_replacement_authoring(child.roof_replacement);
        if (!leaf.demolition || !leaf.roof_profiles.empty() || !leaf.roof_opening_edits.empty() ||
            !leaf.roof_edits.empty() || !leaf.ordinary_roof_edits.empty())
            invalid("roof child requires demolition without body/profile/opening/edit authority");
        return {leaf.registry_id, leaf.alternative_id, leaf.seed_roof_ids};
    }
    if (family == 2 && version == 6) {
        const auto leaf = decode_slab_demolition_intent(child.slab_demolition);
        return {leaf.registry_id, leaf.alternative_id, leaf.slab_ids};
    }
    if (family == 3 && version == 9) {
        const auto leaf = decode_phase_structural_replacement_authoring(child.structural_replacement);
        if (!leaf.demolition || !leaf.edits.empty() || !leaf.identities.empty() ||
            leaf.complete_hosted || !leaf.hosted_instance_identities.empty())
            invalid("structural child requires demolition without replacement or edit authority");
        return {leaf.registry_id, leaf.alternative_id, leaf.seed_object_ids};
    }
    if (family == 4 && version == 11) {
        const auto leaf = decode_stair_demolition_intent(child.stair_demolition);
        return {leaf.registry_id, leaf.alternative_id, leaf.selected_object_ids};
    }
    if (family == 4 && version == 13) {
        const auto leaf = decode_stair_demolition_retirement_intent(child.stair_demolition_retirement);
        return {leaf.registry_id, leaf.alternative_id, leaf.selected_object_ids};
    }
    invalid("family slot contains a foreign historical authoring dialect");
}

PhaseCoordinatedOrdinaryRemoval ordinary_removal(const Json& value, bool roof_selection) {
    const int version = roof_selection ? 2 : 1;
    if (!value.is_object() || value.size() != (roof_selection ? 4u : 3u) || !value.contains("version") ||
        !value.at("version").is_number_integer() || value.at("version") != version ||
        !value.contains("object_ids") || !value.at("object_ids").is_array() ||
        !value.contains("components") || !value.at("components").is_array() ||
        (roof_selection && (!value.contains("roof_additional_identities") ||
            !value.at("roof_additional_identities").is_object())))
        invalid(roof_selection ? "ordinary removal v2 requires exactly version, object_ids, components and roof_additional_identities" :
            "ordinary removal requires exactly version one, object_ids and components");
    const auto& objects = value.at("object_ids");
    const auto& rows = value.at("components");
    if (objects.size() > 1000 || rows.size() > 1000 - objects.size() || (objects.empty() && rows.empty()))
        invalid("ordinary removal requires 1..1000 aggregate actual roots/components");
    PhaseCoordinatedOrdinaryRemoval result;
    result.version = version;
    for (const auto& row : objects) {
        if (!row.is_string()) invalid("ordinary object identity must be a string");
        auto id = row.get<std::string>(); identity(id);
        if (!result.object_ids.empty() && !(result.object_ids.back() < id))
            invalid("ordinary object identities must be ascending and unique");
        result.object_ids.push_back(std::move(id));
    }
    for (const auto& row : rows) {
        if (!row.is_object() || row.size() != 2 || !row.contains("catalog_id") ||
            !row.at("catalog_id").is_string() || !row.contains("instance_id") || !row.at("instance_id").is_string())
            invalid("ordinary component requires exactly catalog_id and instance_id");
        auto key = std::pair{row.at("catalog_id").get<std::string>(), row.at("instance_id").get<std::string>()};
        identity(key.first); identity(key.second);
        if (!result.components.empty() && !(result.components.back() < key))
            invalid("ordinary qualified component keys must be ascending and unique");
        result.components.push_back(std::move(key));
    }
    if (roof_selection) {
        const auto& destinations = value.at("roof_additional_identities");
        if (destinations.size() > 4096) invalid("roof destination key budget exceeded");
        std::size_t count = result.object_ids.size();
        std::set<std::string, std::less<>> fresh;
        for (const auto& [old_id, rows] : destinations.items()) {
            identity(old_id);
            if (!rows.is_array() || rows.empty() || rows.size() > 4096 - count)
                invalid("roof destinations require bounded nonempty fresh identity arrays");
            count += rows.size();
            auto& ids = result.roof_additional_identities[old_id];
            for (const auto& row : rows) {
                if (!row.is_string()) invalid("roof destination identity must be a string");
                auto id = row.get<std::string>(); identity(id);
                if (!fresh.insert(id).second) invalid("roof destination identities must be unique");
                ids.push_back(std::move(id));
            }
        }
    }
    return result;
}

struct Decoded {
    std::vector<PhaseConstraintAuthoringIntent> leaves;
    std::optional<PhaseCoordinatedOrdinaryRemoval> ordinary;
    std::pair<std::string, std::string> saved_choice;
};

Decoded components(const Json& value, const PhaseConstraintAuthoringIntent& enclosing) {
    JsonBudget budget{proof_limit, true};
    budget.read(value);
    if (value.dump().size() > proof_limit) invalid("proof byte budget exceeded");
    if (!value.is_object() || !value.contains("version") || !value.at("version").is_number_integer())
        invalid("inner demolition requires a known integer version");
    const bool roof_selection = value.at("version") == 3;
    const bool mixed = value.at("version") == 2 || roof_selection;
    if ((!mixed && (value.at("version") != 1 || value.size() != 6)) ||
        (mixed && (value.size() != 7 || !value.contains("ordinary_removal"))))
        invalid("inner demolition requires exactly version, five family fields and v2/v3 ordinary_removal");
    for (const auto* key : families) if (!value.contains(key)) invalid("required family field is missing");
    message_only(enclosing.intent);
    // Root enforces outer exclusivity too; this public codec cannot validate a
    // mixed enclosure by ignoring a separately supplied historical operation.
    if (!enclosing.wall_replacement.is_null() || !enclosing.opening_demolition.is_null() ||
        !enclosing.roof_replacement.is_null() || !enclosing.slab_replacement.is_null() ||
        !enclosing.slab_demolition.is_null() || !enclosing.coordinated_replacements.is_null() ||
        !enclosing.structural_replacement.is_null() || !enclosing.stair_demolition.is_null() ||
        !enclosing.stair_replacement.is_null() || !enclosing.stair_demolition_retirement.is_null())
        invalid("enclosing coordinated demolition cannot borrow another operation");
    // Bind raw enclosing choices only after guarding their shape/number types.
    JsonBudget selections_budget{proof_limit, true};
    selections_budget.read(enclosing.phase_selections);
    if (enclosing.phase_selections.dump().size() > proof_limit) invalid("saved choice byte budget exceeded");

    Decoded result;
    if (mixed) result.ordinary = ordinary_removal(value.at("ordinary_removal"), roof_selection);
    std::optional<std::pair<std::string, std::string>> saved_choice;
    std::set<std::string, std::less<>> targets;
    for (std::size_t family = 0; family < families.size(); ++family) {
        const auto& wire = value.at(families[family]);
        if (wire.is_null()) continue;
        // Inspect the outer tag BEFORE entering the full historical codec.
        if (!wire.is_object() || !wire.contains("version") || !wire.at("version").is_number_integer() ||
            (family == 0 && wire.at("version") != 3) ||
            (family == 1 && wire.at("version") != 4) ||
            (family == 2 && wire.at("version") != 6) ||
            (family == 3 && wire.at("version") != 9) ||
            (family == 4 && wire.at("version") != 11 && wire.at("version") != 13))
            invalid("family slot requires its full historical demolition authoring envelope");
        auto child = decode_phase_constraint_authoring_intent(wire);
        if (encode_phase_constraint_authoring_intent(child).dump() != wire.dump())
            invalid("historical child envelope is not canonical");
        message_only(child.intent);
        same_binding(child, enclosing);
        const auto typed = choice(child, family, wire);
        identity(typed.registry_id); identity(typed.alternative_id);
        const auto current_choice = std::pair{typed.registry_id, typed.alternative_id};
        if (saved_choice && *saved_choice != current_choice)
            invalid("all families must name the same typed registry and alternative");
        saved_choice = current_choice;
        if (typed.roots.empty()) invalid("present demolition family cannot be empty");
        for (const auto& root : typed.roots) {
            identity(root);
            if (!targets.insert(root).second) invalid("demolition roots must be unique across all families");
            if (targets.size() > 4096) invalid("aggregate demolition root budget exceeded");
        }
        result.leaves.push_back(std::move(child));
    }
    if (result.leaves.size() < (mixed ? 1u : 2u))
        invalid(mixed ? "mixed demolition requires at least one historical family" :
            "coordinated demolition requires at least two distinct families");
    if (result.ordinary) for (const auto& id : result.ordinary->object_ids)
        if (targets.contains(id)) invalid("ordinary and historical demolition roots must be disjoint");
    if (roof_selection && result.ordinary->object_ids.size() + result.ordinary->components.size() > 4096 - targets.size())
        invalid("aggregate mixed demolition root/component budget exceeded");
    result.saved_choice = *saved_choice;
    return result;
}

// Inspect only actual producer consequences. Physical removals include attached
// rails; catalog row retirement includes derived hosted components. Their actual
// carriers and hosts must belong to the historical choice when registered.
// Unregistered owners remain ordinary. Render aliases never establish authority.
void ordinary_phase_authority(const Entities& source, const Entities& candidate,
    const std::pair<std::string, std::string>& saved_choice) {
    const auto scope = constraint_phase_scope(source);
    std::map<std::string, const PhysicalWallPhaseState*, std::less<>> owners;
    const PhysicalWallPhaseState* selected = nullptr;
    for (const auto& registry : scope.registries) {
        if (registry.registry_id == saved_choice.first) selected = &registry;
        for (const auto& id : registry.registered_entity_ids)
            if (!owners.emplace(id, &registry).second) invalid("overlapping actual phase ownership: " + id);
    }
    if (!selected || selected->alternative_id != std::optional<std::string>{saved_choice.second})
        invalid("ordinary removal requires the historical actual saved registry/alternative");
    const auto compatible = [&](const std::string& id) {
        if (!source.contains(id)) invalid("ordinary authority owner is absent from actual source: " + id);
        if (scope.inactive_owner_ids.contains(id)) invalid("ordinary authority owner is inactive: " + id);
        const auto member = owners.find(id);
        if (member == owners.end()) return;
        const auto* registry = member->second;
        if (registry != selected) invalid("ordinary authority owner belongs to foreign registry: " + id);
        const auto state = registry->states.find(id);
        if (state == registry->states.end() || state->second == ModelPhase::demolished)
            invalid("ordinary authority owner is absent or demolished in actual saved choice: " + id);
    };
    for (const auto& [id, entity] : source) {
        if (!candidate.contains(id) && (entity.type == "stair" || entity.type == "railing" ||
            entity.type == "slab" || entity.type == "column" || entity.type == "beam")) {
            compatible(id);
            if (entity.type == "railing") {
                const auto rail = decode_railing_properties(id, entity.properties);
                if (rail.host) compatible(rail.host->stair_id);
                if (rail.landing_host) compatible(rail.landing_host->stair_id);
            }
        }
        if (entity.type != "assembly_model") continue;
        const auto retained = candidate.find(id);
        if (retained == candidate.end()) invalid("ordinary removal cannot erase its actual catalog carrier");
        // Equal raw catalogs carry no consequence and require no additional
        // authority. Decode affected catalogs through their actual typed codec.
        const auto& before = entity.properties.at("model");
        const auto& after = retained->second.properties.at("model");
        if (before == after) continue;
        const auto original = AssemblyModel::from_json(before);
        const auto changed = AssemblyModel::from_json(after);
        std::set<std::string, std::less<>> remaining;
        for (const auto& row : changed.instances()) remaining.insert(row.id);
        for (const auto& row : original.instances()) if (!remaining.contains(row.id)) {
            compatible(id);
            if (row.placement) compatible(row.placement->host_entity_id);
        }
    }
}

// Child v2 alone carries roof authority. Bind consequences to the actual saved
// source choice, including registry-only demolition of a physically exact roof.
// Child v1 deliberately continues through the historical guard above unchanged.
void roof_selection_phase_authority(const Entities& source, const Entities& candidate,
    const PhaseCoordinatedOrdinaryRemoval& intent,
    const std::pair<std::string, std::string>& saved_choice) {
    const auto scope = constraint_phase_scope(source);
    std::map<std::string, const PhysicalWallPhaseState*, std::less<>> owners;
    const PhysicalWallPhaseState* selected = nullptr;
    for (const auto& registry : scope.registries) {
        if (registry.registry_id == saved_choice.first) selected = &registry;
        for (const auto& id : registry.registered_entity_ids)
            if (!owners.emplace(id, &registry).second) invalid("overlapping actual phase ownership: " + id);
    }
    if (!selected || selected->alternative_id != std::optional<std::string>{saved_choice.second})
        invalid("roof selection requires the historical actual saved registry/alternative");
    const auto original_model = ModelPhases::from_json(source.at(saved_choice.first).properties.at("model"));
    const std::set<std::string, std::less<>> baseline(original_model.baseline_ids().begin(), original_model.baseline_ids().end());
    std::set<std::string, std::less<>> protected_choices;
    for (const auto& alternative : original_model.alternatives()) if (alternative.id != saved_choice.second) {
        protected_choices.insert(alternative.proposed_ids.begin(), alternative.proposed_ids.end());
        protected_choices.insert(alternative.demolished_ids.begin(), alternative.demolished_ids.end());
    }
    std::set<std::string, std::less<>> roofs;
    for (const auto& id : intent.object_ids) {
        const auto row = source.find(id);
        if (row != source.end() && row->second.type == "roof") roofs.insert(id);
    }
    if (roofs.empty()) invalid("ordinary child v2 requires an actual selected roof root");
    const auto compatible = [&](const std::string& id, bool retained = false) {
        const auto row = source.find(id);
        if (row == source.end()) invalid("roof authority owner is absent from actual source: " + id);
        if (scope.inactive_owner_ids.contains(id)) invalid("roof authority owner is inactive: " + id);
        const auto owner = owners.find(id);
        if (owner == owners.end()) return;
        if (owner->second != selected) invalid("roof authority owner belongs to foreign registry: " + id);
        const auto state = selected->states.find(id);
        if (state == selected->states.end() || state->second == ModelPhase::demolished)
            invalid("roof authority owner is absent or demolished in actual saved choice: " + id);
        if (!retained && !original_model.alternatives().empty() && baseline.contains(id))
            invalid("changed roof authority owner is shared with preserved alternatives: " + id);
        if (protected_choices.contains(id)) invalid("roof authority owner has protected other-alternative usage: " + id);
    };
    const auto selected_after = candidate.find(saved_choice.first);
    if (selected_after == candidate.end() || selected_after->second.type != "model_phases")
        invalid("roof selection cannot erase its actual saved registry");
    const auto changed_model = ModelPhases::from_json(selected_after->second.properties.at("model"));
    if (changed_model.active_alternative() != original_model.active_alternative())
        invalid("roof selection changed the actual saved alternative");
    const auto changed_states = changed_model.active_state();
    std::set<std::string, std::less<>> retained_roofs;
    for (const auto& roof : roofs) {
        const auto after = candidate.find(roof);
        const auto state = changed_states.find(roof);
        if (after != candidate.end() && after->second == source.at(roof) &&
            state != changed_states.end() && state->second == ModelPhase::demolished)
            retained_roofs.insert(roof);
        compatible(roof, retained_roofs.contains(roof));
    }
    for (const auto& [id, entity] : source) {
        const auto after = candidate.find(id);
        if ((entity.type == "roof" || entity.type == "roof_join") &&
            (after == candidate.end() || after->second != entity)) {
            compatible(id);
            if (entity.type == "roof_join") {
                const auto join = parse_roof_join(entity.properties, id);
                for (const auto& roof : join.roof_ids) {
                    const auto member = candidate.find(roof);
                    // Qualified joins may retain unchanged shared-baseline
                    // members. Their actual owner still binds the same choice.
                    const bool preserved = member != candidate.end() && source.contains(roof) && member->second == source.at(roof);
                    compatible(roof, preserved);
                }
            }
        }
        if (entity.type != "model_phases") continue;
        if (after == candidate.end()) invalid("roof selection cannot erase an actual phase registry");
        if (entity == after->second) continue;
        if (id != saved_choice.first) invalid("roof selection changed a foreign phase registry: " + id);
        const auto before = ModelPhases::from_json(entity.properties.at("model"));
        const auto changed = ModelPhases::from_json(after->second.properties.at("model"));
        if (changed.active_alternative() != before.active_alternative() ||
            changed.alternatives().size() != before.alternatives().size())
            invalid("roof selection changed saved alternative inventory");
        for (std::size_t index = 0; index < before.alternatives().size(); ++index) {
            const auto& old = before.alternatives()[index];
            const auto& current = changed.alternatives()[index];
            if (old.id != current.id || old.name != current.name ||
                (old.id != saved_choice.second && old != current))
                invalid("roof selection changed a preserved alternative");
        }
        // New baseline enrollment is permitted only for an actual retained
        // selected roof; a fresh join may copy an existing baseline membership.
        for (const auto& member : changed.baseline_ids())
            if (!baseline.contains(member) && source.contains(member) && !retained_roofs.contains(member))
                invalid("roof selection enrolled an unrelated actual baseline owner: " + member);
    }
}
} // namespace

Json encode_phase_coordinated_demolition(const Json& value, const PhaseConstraintAuthoringIntent& enclosing) {
    try {
        (void)components(value, enclosing);
        return value;
    } catch (const Json::exception& error) {
        invalid(std::string("malformed proof: ") + error.what());
    }
}

std::vector<PhaseConstraintAuthoringIntent> phase_coordinated_demolition_components(
    const Json& value, const PhaseConstraintAuthoringIntent& enclosing) {
    try {
        return components(value, enclosing).leaves;
    } catch (const Json::exception& error) {
        invalid(std::string("malformed proof: ") + error.what());
    }
}

std::optional<PhaseCoordinatedOrdinaryRemoval> phase_coordinated_demolition_ordinary_removal(
    const Json& value, const PhaseConstraintAuthoringIntent& enclosing) {
    try {
        return components(value, enclosing).ordinary;
    } catch (const Json::exception& error) {
        invalid(std::string("malformed proof: ") + error.what());
    }
}

Entities replay_phase_coordinated_demolition(const Entities& source, const PhaseConstraintAuthoringIntent& enclosing) {
    try {
        source_budget(source);
        const auto decoded = components(enclosing.coordinated_demolition, enclosing);
        std::vector<Entities> candidates;
        candidates.reserve(decoded.leaves.size() + (decoded.ordinary ? 1 : 0));
        for (const auto& leaf : decoded.leaves) {
            auto candidate = replay_phase_constraint_authoring(source, encode_phase_constraint_authoring_intent(leaf));
            source_budget(candidate);
            candidates.push_back(std::move(candidate));
        }
        if (decoded.ordinary) {
            const bool roof_selection = decoded.ordinary->version == 2;
            if (roof_selection && std::none_of(decoded.ordinary->object_ids.begin(), decoded.ordinary->object_ids.end(),
                [&](const auto& id) { const auto found = source.find(id); return found != source.end() && found->second.type == "roof"; }))
                invalid("ordinary child v2 requires an actual selected roof root");
            auto candidate = roof_selection ? replay_architectural_selection_removal(source,
                ArchitecturalSelectionRemovalIntent{decoded.ordinary->object_ids, decoded.ordinary->components,
                    decoded.ordinary->roof_additional_identities}) :
                replay_architectural_object_removal(source, decoded.ordinary->object_ids, decoded.ordinary->components);
            source_budget(candidate);
            ordinary_phase_authority(source, candidate, decoded.saved_choice);
            if (roof_selection) roof_selection_phase_authority(source, candidate, *decoded.ordinary, decoded.saved_choice);
            candidates.push_back(std::move(candidate));
        }
        auto result = compose_phase_demolition_candidates(source, candidates, decoded.ordinary.has_value(),
            decoded.ordinary && decoded.ordinary->version == 2);
        source_budget(result);
        return result;
    } catch (const Json::exception& error) {
        invalid(std::string("malformed actual replay source/proof: ") + error.what());
    } catch (const Standard_Failure& error) {
        const auto* message = error.GetMessageString();
        invalid(std::string("native admission failed: ") + (message ? message : "Open CASCADE failure"));
    }
}
} // namespace sketch
