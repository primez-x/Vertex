#include "sketch/phase_wall_profile_edit.hpp"

#include "sketch/architecture.hpp"
#include "sketch/boundary_integrity.hpp"
#include "sketch/constraint_entity.hpp"
#include "sketch/constraint_integrity.hpp"
#include "sketch/constraint_phase_scope.hpp"
#include "sketch/document_wall.hpp"
#include "sketch/door_operation.hpp"
#include "sketch/opening_assembly.hpp"
#include "sketch/project_organization.hpp"
#include "sketch/wall_measurement.hpp"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <set>
#include <stdexcept>

namespace sketch {
namespace {
using Json = nlohmann::json;
constexpr std::size_t collection_limit = 4096;
constexpr std::size_t expression_limit = 4096;
constexpr std::size_t proof_limit = 1024 * 1024;
constexpr double tolerance = default_geometry_tolerance_metres;

[[noreturn]] void invalid(const char* reason) { throw std::invalid_argument(reason); }

void keys(const Json& value, std::initializer_list<const char*> expected) {
    if (!value.is_object() || value.size() != expected.size())
        invalid("Wall profile edit fields are invalid");
    for (const auto* field : expected)
        if (!value.contains(field)) invalid("Wall profile edit field is missing");
}

std::string identity(const Json& value) {
    if (!value.is_string()) invalid("Wall profile identity must be a string");
    const auto& result = value.get_ref<const std::string&>();
    if (result.empty() || result.size() > 128 ||
        !std::all_of(result.begin(), result.end(), [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == ':';
        })) invalid("Wall profile identity is invalid");
    return result;
}

Quantity quantity(const Json& value) {
    keys(value, {"version", "original_expression", "entered_unit", "exact_metres"});
    keys(value.at("exact_metres"), {"numerator", "denominator"});
    const auto& expression = value.at("original_expression");
    if (!expression.is_string() || expression.get_ref<const std::string&>().size() > expression_limit)
        invalid("Wall profile quantity expression budget exceeded");
    const auto result = decode_constraint_quantity_receipt(value);
    if (!std::isfinite(result.metres) || result.metres <= tolerance)
        invalid("Wall profile dimensions must be finite and positive");
    return result;
}

Json quantity(const Quantity& value) {
    if (value.original_expression.size() > expression_limit)
        invalid("Wall profile quantity expression budget exceeded");
    const auto result = encode_constraint_quantity_receipt(value);
    (void)quantity(result);
    return result;
}

Wall wall(const Entity& entity, const std::vector<const Entity*>& openings = {}) {
    Wall result;
    std::string diagnostic;
    if (!read_document_wall(entity, openings, result, diagnostic))
        throw std::invalid_argument("Wall profile source " + entity.id + ": " + diagnostic);
    validate_wall_semantics(result);
    return result;
}

void receipt(Entity& entity, const std::string& pointer, const Quantity& value, double old_value) {
    auto entries = entity.properties.find("quantity_entries");
    if (entries == entity.properties.end()) {
        entity.properties["quantity_entries"] = Json::object();
        entries = entity.properties.find("quantity_entries");
    }
    if (!entries->is_object()) invalid("Wall profile quantity_entries must be an object");
    const auto encoded = quantity(value);
    auto previous = entries->find(pointer);
    if (previous == entries->end()) {
        (*entries)[pointer] = encoded;
        return;
    }
    // Existing opaque fields are retained, including fields in exact_metres.
    // Unsupported or stale receipts cannot silently be rewritten as authority.
    const auto parsed = decode_constraint_quantity_receipt(*previous);
    if (parsed.metres != old_value) invalid("Wall profile source quantity receipt is stale");
    for (const auto* field : {"version", "original_expression", "entered_unit"})
        (*previous)[field] = encoded.at(field);
    for (const auto* field : {"numerator", "denominator"})
        (*previous)["exact_metres"][field] = encoded.at("exact_metres").at(field);
}

void scalar(Entity& entity, const char* canonical, const char* alias,
            const Quantity& value, double old_value) {
    if (entity.properties.contains(alias)) {
        const auto& retained = entity.properties.at(alias);
        if (!retained.is_number() || retained.get<double>() != old_value)
            invalid("Wall profile scalar aliases disagree");
    }
    if (value.metres == old_value) return;
    entity.properties[canonical] = value.metres;
    receipt(entity, "/" + std::string(canonical), value, old_value);
    if (entity.properties.contains(alias)) {
        entity.properties[alias] = value.metres;
        const auto& entries = entity.properties.at("quantity_entries");
        if (entries.contains("/" + std::string(alias)))
            receipt(entity, "/" + std::string(alias), value, old_value);
    }
}

void assembly_fit(const Entity& entity, const Wall& host, const HostedOpening& cut) {
    std::optional<OpeningAssembly> assembly;
    if (const auto found = entity.properties.find("opening_assembly"); found != entity.properties.end())
        assembly = parse_opening_assembly(*found);
    else if (const auto kind = entity.properties.find("opening_kind");
             kind != entity.properties.end() && kind->is_string()) {
        if (const auto parsed = parse_opening_assembly_kind(kind->get_ref<const std::string&>()))
            assembly = default_opening_assembly(*parsed);
    }
    if (!assembly) return; // Bare cuts retain their existing semantics.
    const auto& value = *assembly;
    const bool window = value.kind == OpeningAssemblyKind::window;
    if (const auto kind = entity.properties.find("opening_kind"); kind != entity.properties.end() &&
        (!kind->is_string() || kind->get_ref<const std::string&>() != opening_assembly_kind_name(value.kind)))
        invalid("Wall profile opening kind and assembly disagree");
    std::optional<DoorOperation> operation;
    if (entity.properties.contains("door_operation"))
        operation = decode_door_operation(entity.properties.at("door_operation"));
    if (window && operation) invalid("Window assembly cannot carry a door operation");
    // Reuse the authoritative physical factory, including double-door meeting
    // clearance and the solid fit of curved/sloped/bay and moving assemblies.
    // Its derived solid is discarded; source and assembly records stay intact.
    (void)make_opening_assembly(host, cut, value, operation);
}

Wall hosted_wall(const std::map<std::string, Entity, std::less<>>& entities,
    const Entity& host, const std::vector<const Entity*>& openings) {
    for (const auto* opening : openings) {
        const auto found = entities.find(opening->id);
        if (found == entities.end() || &found->second != opening)
            invalid("Wall profile hosted opening identity is inconsistent");
    }
    return wall(resolve_vertical_placement(entities, host), openings);
}

void admit_affected_joins(const std::map<std::string, Entity, std::less<>>& entities,
    const std::set<std::string, std::less<>>& targets, const ConstraintPhaseScope& scope,
    std::map<std::string, std::vector<const Entity*>, std::less<>>& openings) {
    std::map<std::string, Wall, std::less<>> members;
    for (const auto& [id, entity] : entities) {
        if (entity.type != "wall_join" || scope.inactive_owner_ids.contains(id)) continue;
        const auto join = parse_wall_join(entity.properties, id);
        // Same saved-active relationship policy as physical wall replacement:
        // retained joins containing parked/demolished originals are inactive.
        if (std::any_of(join.wall_ids.begin(), join.wall_ids.end(), [&](const auto& member) {
                return scope.inactive_owner_ids.contains(member);
            }) || std::none_of(join.wall_ids.begin(), join.wall_ids.end(), [&](const auto& member) {
                return targets.contains(member);
            })) continue;
        if (entity.id != id) invalid("Wall profile join identity is inconsistent");
        for (const auto& member : join.wall_ids) {
            if (members.contains(member)) continue;
            const auto found = entities.find(member);
            if (found == entities.end() || found->second.type != "wall" || found->second.id != member)
                invalid("Wall profile active join member is missing or inconsistent");
            members.emplace(member, hosted_wall(entities, found->second, openings[member]));
        }
        std::vector<Wall> actual_members;
        actual_members.reserve(join.wall_ids.size());
        for (const auto& member : join.wall_ids) actual_members.push_back(members.at(member));
        // Retain exact IDs/order and all hosted cuts. The native baseline and
        // actual solid-contact gates include placement, thickness and top plane.
        // Never serialize this derived union or replace its source members.
        (void)make_wall_join(join, actual_members);
    }
}
} // namespace

nlohmann::json encode_wall_profile_edit_intent(const WallProfileEditIntent& intent) {
    (void)identity(intent.wall_id);
    if (!intent.thickness && !intent.height && !intent.layer_thicknesses)
        invalid("Wall profile edit requires an authored dimension");
    Json result{{"version", 1}, {"wall_id", intent.wall_id},
        {"thickness", intent.thickness ? quantity(*intent.thickness) : Json(nullptr)},
        {"height", intent.height ? quantity(*intent.height) : Json(nullptr)}, {"layer_thicknesses", nullptr}};
    if (intent.layer_thicknesses) {
        if (intent.layer_thicknesses->empty() || intent.layer_thicknesses->size() > collection_limit)
            invalid("Wall profile layer inventory budget is invalid");
        auto rows = Json::array();
        std::set<std::string, std::less<>> ids;
        for (const auto& layer : *intent.layer_thicknesses) {
            if (!ids.insert(identity(layer.layer_id)).second)
                invalid("Wall profile layer inventory contains duplicate identities");
            rows.push_back({{"layer_id", layer.layer_id}, {"thickness", quantity(layer.thickness)}});
        }
        result["layer_thicknesses"] = std::move(rows);
    }
    if (result.dump().size() > proof_limit) invalid("Wall profile proof byte budget exceeded");
    return result;
}

WallProfileEditIntent decode_wall_profile_edit_intent(const nlohmann::json& value) {
    keys(value, {"version", "wall_id", "thickness", "height", "layer_thicknesses"});
    if (!value.at("version").is_number_integer() || value.at("version") != 1)
        invalid("Wall profile edit version is unsupported");
    WallProfileEditIntent result;
    result.wall_id = identity(value.at("wall_id"));
    if (!value.at("thickness").is_null()) result.thickness = quantity(value.at("thickness"));
    if (!value.at("height").is_null()) result.height = quantity(value.at("height"));
    const auto& layers = value.at("layer_thicknesses");
    if (!layers.is_null()) {
        if (!layers.is_array() || layers.empty() || layers.size() > collection_limit)
            invalid("Wall profile layer inventory budget is invalid");
        result.layer_thicknesses.emplace();
        for (const auto& row : layers) {
            keys(row, {"layer_id", "thickness"});
            result.layer_thicknesses->push_back({identity(row.at("layer_id")), quantity(row.at("thickness"))});
        }
    }
    (void)encode_wall_profile_edit_intent(result);
    return result;
}

Entity replay_wall_profile_entity(const Entity& source, const WallProfileEditIntent& intent) {
    (void)encode_wall_profile_edit_intent(intent);
    if (source.type != "wall" || source.id != intent.wall_id)
        invalid("Wall profile edit requires its actual wall target");
    const auto original = wall(source);
    auto result = source;
    if (intent.thickness) scalar(result, "thickness_m", "thickness", *intent.thickness, original.thickness);
    if (intent.height) scalar(result, "height_m", "height", *intent.height, original.height);
    if (intent.layer_thicknesses) {
        if (intent.layer_thicknesses->size() != original.layers.size())
            invalid("Wall profile layer inventory must include every existing layer");
        for (std::size_t i = 0; i < original.layers.size(); ++i) {
            const auto& replacement = intent.layer_thicknesses->at(i);
            const auto& retained = original.layers[i];
            if (replacement.layer_id != retained.id)
                invalid("Wall profile layer inventory must retain its existing order and identities");
            if (replacement.thickness.metres == retained.thickness) continue;
            result.properties.at("layers").at(i).at("thickness_m") = replacement.thickness.metres;
            receipt(result, "/layers/" + std::to_string(i) + "/thickness_m", replacement.thickness, retained.thickness);
        }
    }
    // Includes the established exact layer-total policy. A total-only edit of
    // a layered wall fails unless its retained layer sum still agrees.
    (void)wall(result);
    return result;
}

void validate_active_wall_physical_dependencies(
    const std::map<std::string, Entity, std::less<>>& entities,
    const std::set<std::string, std::less<>>& targets) {
    if (targets.empty()) return;
    if (targets.size() > collection_limit) invalid("Wall physical dependency target budget exceeded");
    const auto scope = constraint_phase_scope(entities);
    std::map<std::string, std::vector<const Entity*>, std::less<>> openings;
    for (const auto& [id, entity] : entities) {
        (void)id;
        if (entity.type != "opening" || !entity.properties.is_object()) continue;
        const auto host = entity.properties.find("wall_id");
        if (host != entity.properties.end() && host->is_string())
            openings[host->get_ref<const std::string&>()].push_back(&entity);
    }
    for (const auto& target : targets) {
        const auto found = entities.find(target);
        if (found == entities.end() || found->second.id != target || found->second.type != "wall" ||
            scope.inactive_owner_ids.contains(target))
            invalid("Wall physical dependency target must be an actual saved-active wall");
        const auto checked = hosted_wall(entities, found->second, openings[target]);
        (void)make_wall(checked);
        for (std::size_t i = 0; i < checked.openings.size(); ++i)
            assembly_fit(*openings[target][i], checked, checked.openings[i]);
    }
    admit_affected_joins(entities, targets, scope, openings);
}

std::map<std::string, Entity, std::less<>> replay_wall_profile_entities(
    const std::map<std::string, Entity, std::less<>>& source,
    const std::vector<WallProfileEditIntent>& intents, bool validate_final_constraints) {
    if (intents.size() > collection_limit) invalid("Wall profile target budget exceeded");
    if (intents.empty()) return source;
    const auto scope = constraint_phase_scope(source);
    std::set<std::string, std::less<>> targets;
    auto result = source;
    bool depth_changed = false;
    std::size_t proof_bytes = 0;
    for (const auto& intent : intents) {
        const auto bytes = encode_wall_profile_edit_intent(intent).dump().size();
        if (bytes > proof_limit - proof_bytes) invalid("Wall profile batch proof byte budget exceeded");
        proof_bytes += bytes;
        if (!targets.insert(intent.wall_id).second) invalid("Wall profile edit contains duplicate targets");
        if (scope.inactive_owner_ids.contains(intent.wall_id)) invalid("Wall profile target is inactive in the saved design");
        const auto found = source.find(intent.wall_id);
        if (found == source.end() || found->second.id != found->first)
            invalid("Wall profile target is missing or has inconsistent identity");
        auto edited = replay_wall_profile_entity(found->second, intent);
        depth_changed = depth_changed || wall(found->second).thickness != wall(edited).thickness;
        result.at(intent.wall_id) = std::move(edited);
    }
    if (result == source) return source;
    validate_active_wall_physical_dependencies(result, targets);
    if (depth_changed) {
        const auto edits = exterior_wall_measurement_source_updates_active_phase(source, result, false);
        result = edited_boundary_entities_batch(result, edits);
    }
    for (const auto& [id, entity] : source) {
        if (entity.type != "room" && !scope.inactive_owner_ids.contains(id)) continue;
        const auto retained = result.find(id);
        if (retained == result.end() || retained->second != entity ||
            retained->second.properties.dump() != entity.properties.dump() ||
            retained->second.extensions.dump() != entity.extensions.dump())
            invalid("Wall profile completion changed a physical room or inactive owner");
    }
    if (validate_final_constraints) {
        if (const auto diagnostic = validate_active_phase_constraint_integrity(result))
            throw std::invalid_argument(*diagnostic);
    }
    return result;
}

} // namespace sketch
