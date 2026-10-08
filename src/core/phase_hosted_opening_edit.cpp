#include "sketch/phase_hosted_opening_edit.hpp"

#include "sketch/constraint_entity.hpp"
#include "sketch/constraint_integrity.hpp"
#include "sketch/constraint_phase_scope.hpp"
#include "sketch/document_wall.hpp"
#include "sketch/phase_wall_profile_edit.hpp"
#include "sketch/project_organization.hpp"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <set>
#include <stdexcept>
#include <tuple>

namespace sketch {
namespace {
using Json = nlohmann::json;
constexpr std::size_t collection_limit = 4096;
constexpr std::size_t expression_limit = 4096;
constexpr std::size_t proof_limit = 1024 * 1024;

[[noreturn]] void invalid(const char* reason) { throw std::invalid_argument(reason); }

void keys(const Json& value, std::initializer_list<const char*> expected) {
    if (!value.is_object() || value.size() != expected.size())
        invalid("Hosted opening profile fields are invalid");
    for (const auto* field : expected)
        if (!value.contains(field)) invalid("Hosted opening profile field is missing");
}

std::string identity(const Json& value) {
    if (!value.is_string()) invalid("Hosted opening profile identity must be a string");
    const auto& result = value.get_ref<const std::string&>();
    if (result.empty() || result.size() > 128 ||
        !std::all_of(result.begin(), result.end(), [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == ':';
        })) invalid("Hosted opening profile identity is invalid");
    return result;
}

const char* unit_name(Unit value) {
    switch (value) {
    case Unit::metre: return "m";
    case Unit::millimetre: return "mm";
    case Unit::centimetre: return "cm";
    case Unit::foot: return "ft";
    case Unit::inch: return "in";
    }
    invalid("Hosted opening profile quantity unit is unsupported");
}

Quantity quantity(const Json& value, bool zero_allowed) {
    keys(value, {"version", "original_expression", "entered_unit", "exact_metres"});
    keys(value.at("exact_metres"), {"numerator", "denominator"});
    const auto& expression = value.at("original_expression");
    if (!expression.is_string() || expression.get_ref<const std::string&>().size() > expression_limit)
        invalid("Hosted opening profile quantity expression budget exceeded");
    const auto result = decode_constraint_quantity_receipt(value);
    if (!std::isfinite(result.metres) || (zero_allowed ? result.metres < 0 : result.metres <= 0))
        invalid("Hosted opening profile dimension is outside its admitted range");
    return result;
}

Json quantity(const Quantity& value, bool zero_allowed) {
    // The shared constraint encoder is positive-only. Its decoder admits an
    // exact zero, so retain the same receipt dialect for offset and sill.
    Json result{{"version", 1}, {"original_expression", value.original_expression},
        {"entered_unit", unit_name(value.entered_unit)},
        {"exact_metres", {{"numerator", value.exact_metres.numerator},
                          {"denominator", value.exact_metres.denominator}}}};
    const auto parsed = quantity(result, zero_allowed);
    if (parsed.metres != value.metres || parsed.exact_metres != value.exact_metres ||
        parsed.entered_unit != value.entered_unit || parsed.original_expression != value.original_expression)
        invalid("Hosted opening profile quantity is internally inconsistent");
    return result;
}

double dimension(const Entity& entity, const char* canonical, const char* alias, bool zero_allowed) {
    const auto& properties = entity.properties;
    const auto found = properties.find(canonical);
    const auto legacy = properties.find(alias);
    const auto selected = found != properties.end() ? found : legacy;
    if (selected == properties.end() || !selected->is_number())
        invalid("Hosted opening profile dimension is missing or invalid");
    const auto result = selected->get<double>();
    if (!std::isfinite(result) || (zero_allowed ? result < 0 : result <= 0))
        invalid("Hosted opening profile source dimension is outside its admitted range");
    if (legacy != properties.end() && (!legacy->is_number() || legacy->get<double>() != result))
        invalid("Hosted opening profile scalar aliases disagree");
    return result;
}

void admit_receipt(const Entity& entity, const std::string& pointer, double stored_value) {
    const auto entries = entity.properties.find("quantity_entries");
    if (entries == entity.properties.end()) return;
    if (!entries->is_object()) invalid("Hosted opening profile quantity_entries must be an object");
    const auto found = entries->find(pointer);
    if (found == entries->end()) return;
    const auto& expression = found->at("original_expression");
    if (!expression.is_string() || expression.get_ref<const std::string&>().size() > expression_limit ||
        found->dump().size() > proof_limit)
        invalid("Hosted opening profile retained quantity receipt budget exceeded");
    if (decode_constraint_quantity_receipt(*found).metres != stored_value)
        invalid("Hosted opening profile source quantity receipt is stale");
}

void receipt(Entity& entity, const std::string& pointer, const Quantity& value, bool zero_allowed) {
    if (!entity.properties.contains("quantity_entries"))
        entity.properties["quantity_entries"] = Json::object();
    auto& entries = entity.properties.at("quantity_entries");
    const auto encoded = quantity(value, zero_allowed);
    const auto found = entries.find(pointer);
    if (found == entries.end()) {
        entries[pointer] = encoded;
        return;
    }
    // Source admission already proved supported, non-stale authority. Retain
    // unknown receipt fields, including those nested within exact_metres.
    for (const auto* field : {"version", "original_expression", "entered_unit"})
        (*found)[field] = encoded.at(field);
    for (const auto* field : {"numerator", "denominator"})
        (*found)["exact_metres"][field] = encoded.at("exact_metres").at(field);
}

void scalar(Entity& entity, const char* canonical, const char* alias,
            const Quantity& value, double old_value, bool zero_allowed) {
    if (value.metres == old_value) return;
    entity.properties[canonical] = value.metres;
    receipt(entity, "/" + std::string(canonical), value, zero_allowed);
    if (entity.properties.contains(alias)) {
        entity.properties[alias] = value.metres;
        if (entity.properties.at("quantity_entries").contains("/" + std::string(alias)))
            receipt(entity, "/" + std::string(alias), value, zero_allowed);
    }
}

struct Profile {
    HostedOpening cut;
    std::optional<OpeningAssembly> assembly;
    std::optional<DoorOperation> operation;
};

Profile profile(const Entity& entity) {
    if (entity.type != "opening" || !entity.properties.is_object())
        invalid("Hosted opening profile requires an existing opening entity");
    Profile result;
    result.cut = {entity.id,
        dimension(entity, "offset_m", "offset", true), dimension(entity, "width_m", "width", false),
        dimension(entity, "sill_m", "sill", true), dimension(entity, "height_m", "height", false)};
    for (const auto& [canonical, alias, value] : {
            std::tuple{"offset_m", "offset", result.cut.offset},
            std::tuple{"width_m", "width", result.cut.width},
            std::tuple{"sill_m", "sill", result.cut.sill},
            std::tuple{"height_m", "height", result.cut.height}}) {
        if (entity.properties.contains(canonical)) admit_receipt(entity, "/" + std::string(canonical), value);
        else if (const auto entries = entity.properties.find("quantity_entries");
                 entries != entity.properties.end() && entries->is_object() &&
                 entries->contains("/" + std::string(canonical)))
            invalid("Hosted opening profile quantity receipt has no scalar authority");
        if (entity.properties.contains(alias)) admit_receipt(entity, "/" + std::string(alias), value);
        else if (const auto entries = entity.properties.find("quantity_entries");
                 entries != entity.properties.end() && entries->is_object() &&
                 entries->contains("/" + std::string(alias)))
            invalid("Hosted opening profile alias receipt has no scalar authority");
    }
    const auto kind = entity.properties.find("opening_kind");
    std::optional<OpeningAssemblyKind> family;
    if (kind != entity.properties.end()) {
        if (!kind->is_string()) invalid("Hosted opening profile kind must be a string");
        const auto& text = kind->get_ref<const std::string&>();
        family = parse_opening_assembly_kind(text);
        if (!family && text != "opening") invalid("Hosted opening profile family is unsupported");
    }
    if (entity.properties.contains("opening_assembly")) {
        result.assembly = parse_opening_assembly(entity.properties.at("opening_assembly"));
        if (kind != entity.properties.end() &&
            (!family || *family != result.assembly->kind))
            invalid("Hosted opening profile kind and assembly disagree");
    } else if (family) result.assembly = default_opening_assembly(*family);
    if (entity.properties.contains("door_operation")) {
        result.operation = decode_door_operation(entity.properties.at("door_operation"));
        if (!result.assembly || result.assembly->kind != OpeningAssemblyKind::door)
            invalid("Only a door assembly can carry a door operation");
    }
    return result;
}

Wall host_wall(const std::map<std::string, Entity, std::less<>>& entities,
               const Entity& host, const std::vector<const Entity*>& openings) {
    if (host.type != "wall") invalid("Hosted opening profile host must be a wall");
    Wall result;
    std::string error;
    if (!read_document_wall(resolve_vertical_placement(entities, host), openings, result, error))
        throw std::invalid_argument("Hosted opening profile host " + host.id + ": " + error);
    validate_wall_semantics(result);
    return result;
}
} // namespace

nlohmann::json encode_hosted_opening_profile_edit_intent(const HostedOpeningProfileEditIntent& intent) {
    (void)identity(intent.opening_id);
    (void)identity(intent.wall_id);
    if (intent.clear_door_operation && intent.door_operation)
        invalid("Hosted opening profile cannot set and clear the same door operation");
    if (!intent.offset && !intent.width && !intent.sill && !intent.height &&
        !intent.assembly && !intent.door_operation && !intent.clear_door_operation)
        invalid("Hosted opening profile edit requires an authored field");
    Json result{{"version", 1}, {"opening_id", intent.opening_id}, {"wall_id", intent.wall_id},
        {"offset", intent.offset ? quantity(*intent.offset, true) : Json(nullptr)},
        {"width", intent.width ? quantity(*intent.width, false) : Json(nullptr)},
        {"sill", intent.sill ? quantity(*intent.sill, true) : Json(nullptr)},
        {"height", intent.height ? quantity(*intent.height, false) : Json(nullptr)},
        {"assembly", intent.assembly ? opening_assembly_json(*intent.assembly) : Json(nullptr)},
        {"door_operation", intent.door_operation ? encode_door_operation(*intent.door_operation) : Json(nullptr)},
        {"clear_door_operation", intent.clear_door_operation}};
    if (result.dump().size() > proof_limit) invalid("Hosted opening profile proof byte budget exceeded");
    return result;
}

HostedOpeningProfileEditIntent decode_hosted_opening_profile_edit_intent(const nlohmann::json& value) {
    keys(value, {"version", "opening_id", "wall_id", "offset", "width", "sill", "height", "assembly", "door_operation", "clear_door_operation"});
    if (!value.at("version").is_number_integer() || value.at("version") != 1)
        invalid("Hosted opening profile edit version is unsupported");
    if (value.dump().size() > proof_limit) invalid("Hosted opening profile proof byte budget exceeded");
    HostedOpeningProfileEditIntent result;
    result.opening_id = identity(value.at("opening_id"));
    result.wall_id = identity(value.at("wall_id"));
    if (!value.at("offset").is_null()) result.offset = quantity(value.at("offset"), true);
    if (!value.at("width").is_null()) result.width = quantity(value.at("width"), false);
    if (!value.at("sill").is_null()) result.sill = quantity(value.at("sill"), true);
    if (!value.at("height").is_null()) result.height = quantity(value.at("height"), false);
    if (!value.at("assembly").is_null()) result.assembly = parse_opening_assembly(value.at("assembly"));
    if (!value.at("door_operation").is_null()) result.door_operation = decode_door_operation(value.at("door_operation"));
    if (!value.at("clear_door_operation").is_boolean())
        invalid("Hosted opening profile clear operation flag must be a boolean");
    result.clear_door_operation = value.at("clear_door_operation").get<bool>();
    (void)encode_hosted_opening_profile_edit_intent(result);
    return result;
}

Entity replay_hosted_opening_profile_entity(const Entity& source, const HostedOpeningProfileEditIntent& intent) {
    (void)encode_hosted_opening_profile_edit_intent(intent);
    if (source.id != intent.opening_id || source.type != "opening")
        invalid("Hosted opening profile edit requires its actual opening target");
    std::string host, error;
    if (!read_document_wall_id(source, host, error) || host != intent.wall_id)
        invalid("Hosted opening profile edit must retain its actual host");
    const auto original = profile(source);
    auto result = source;
    if (intent.offset) scalar(result, "offset_m", "offset", *intent.offset, original.cut.offset, true);
    if (intent.width) scalar(result, "width_m", "width", *intent.width, original.cut.width, false);
    if (intent.sill) scalar(result, "sill_m", "sill", *intent.sill, original.cut.sill, true);
    if (intent.height) scalar(result, "height_m", "height", *intent.height, original.cut.height, false);
    if (intent.assembly) {
        if (!original.assembly || intent.assembly->kind != original.assembly->kind)
            invalid("Hosted opening profile edit must retain its existing assembly family");
        if (*intent.assembly != *original.assembly)
            result.properties["opening_assembly"] = opening_assembly_json(*intent.assembly);
    }
    if (intent.door_operation) {
        if (!original.assembly || original.assembly->kind != OpeningAssemblyKind::door)
            invalid("Hosted opening profile operation edit requires an existing door family");
        if (!original.operation || *intent.door_operation != *original.operation)
            result.properties["door_operation"] = encode_door_operation(*intent.door_operation);
    }
    if (intent.clear_door_operation) {
        if (!original.assembly || original.assembly->kind != OpeningAssemblyKind::door)
            invalid("Hosted opening profile operation clear requires an existing door family");
        result.properties.erase("door_operation");
    }
    (void)profile(result);
    return result;
}

std::map<std::string, Entity, std::less<>> replay_hosted_opening_profile_entities(
    const std::map<std::string, Entity, std::less<>>& source,
    const std::vector<HostedOpeningProfileEditIntent>& intents, bool validate_final_constraints) {
    if (intents.size() > collection_limit) invalid("Hosted opening profile target budget exceeded");
    if (intents.empty()) return source;
    const auto scope = constraint_phase_scope(source);
    std::set<std::string, std::less<>> targets, hosts;
    auto result = source;
    std::size_t proof_bytes = 0;
    for (const auto& intent : intents) {
        const auto bytes = encode_hosted_opening_profile_edit_intent(intent).dump().size();
        if (bytes > proof_limit - proof_bytes) invalid("Hosted opening profile batch proof byte budget exceeded");
        proof_bytes += bytes;
        if (!targets.insert(intent.opening_id).second) invalid("Hosted opening profile edit contains duplicate targets");
        if (scope.inactive_owner_ids.contains(intent.opening_id) || scope.inactive_owner_ids.contains(intent.wall_id))
            invalid("Hosted opening profile target or host is inactive in the saved design");
        const auto target = source.find(intent.opening_id);
        const auto host = source.find(intent.wall_id);
        if (target == source.end() || target->second.id != target->first ||
            host == source.end() || host->second.id != host->first || host->second.type != "wall")
            invalid("Hosted opening profile target or host is missing or inconsistent");
        result.at(intent.opening_id) = replay_hosted_opening_profile_entity(target->second, intent);
        hosts.insert(intent.wall_id);
    }
    std::map<std::string, std::vector<const Entity*>, std::less<>> openings;
    for (const auto& [id, entity] : result) {
        if (entity.type != "opening") continue;
        const auto host_reference = entity.properties.find("wall_id");
        if (host_reference == entity.properties.end() || !host_reference->is_string() ||
            !hosts.contains(host_reference->get_ref<const std::string&>())) continue;
        std::string host, error;
        if (!read_document_wall_id(entity, host, error))
            throw std::invalid_argument("Hosted opening profile sibling host: " + error);
        if (entity.id != id) invalid("Hosted opening profile sibling identity is inconsistent");
        if (openings[host].size() >= collection_limit) invalid("Hosted opening profile sibling budget exceeded");
        (void)profile(entity);
        openings[host].push_back(&entity);
    }
    for (const auto& host_id : hosts) {
        (void)host_wall(result, result.at(host_id), openings[host_id]);
    }
    validate_active_wall_physical_dependencies(result, hosts);
    if (validate_final_constraints) {
        if (const auto diagnostic = validate_active_phase_constraint_integrity(result))
            throw std::invalid_argument(*diagnostic);
    }
    return result;
}

} // namespace sketch
