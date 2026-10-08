#include "sketch/phase_hosted_opening_rehost.hpp"

#include "sketch/constraint_entity.hpp"
#include "sketch/constraint_integrity.hpp"
#include "sketch/constraint_phase_scope.hpp"
#include "sketch/document_wall.hpp"
#include "sketch/model_phases.hpp"
#include "sketch/phase_hosted_opening_edit.hpp"
#include "sketch/phase_wall_profile_edit.hpp"
#include "sketch/project_organization.hpp"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <set>
#include <stdexcept>
#include <utility>

namespace sketch {
namespace {
using Json = nlohmann::json;
using Entities = std::map<std::string, Entity, std::less<>>;
constexpr std::size_t collection_limit = 4096;
constexpr std::size_t expression_limit = 4096;
constexpr std::size_t proof_limit = 1024 * 1024;

[[noreturn]] void invalid(const char* reason) { throw std::invalid_argument(reason); }

void keys(const Json& value, std::initializer_list<const char*> expected) {
    if (!value.is_object() || value.size() != expected.size())
        invalid("Hosted opening rehost fields are invalid");
    for (const auto* field : expected)
        if (!value.contains(field)) invalid("Hosted opening rehost field is missing");
}

std::string identity(const Json& value) {
    if (!value.is_string()) invalid("Hosted opening rehost identity must be a string");
    const auto& result = value.get_ref<const std::string&>();
    if (result.empty() || result.size() > 128 ||
        !std::all_of(result.begin(), result.end(), [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == ':';
        })) invalid("Hosted opening rehost identity is invalid");
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
    invalid("Hosted opening rehost quantity unit is unsupported");
}

Quantity quantity(const Json& value) {
    keys(value, {"version", "original_expression", "entered_unit", "exact_metres"});
    keys(value.at("exact_metres"), {"numerator", "denominator"});
    const auto& expression = value.at("original_expression");
    if (!expression.is_string() || expression.get_ref<const std::string&>().size() > expression_limit)
        invalid("Hosted opening rehost quantity expression budget exceeded");
    const auto result = decode_constraint_quantity_receipt(value);
    if (!std::isfinite(result.metres) || result.metres < 0)
        invalid("Hosted opening rehost station must be finite and nonnegative");
    return result;
}

Json quantity(const Quantity& value) {
    Json result{{"version", 1}, {"original_expression", value.original_expression},
        {"entered_unit", unit_name(value.entered_unit)},
        {"exact_metres", {{"numerator", value.exact_metres.numerator},
                          {"denominator", value.exact_metres.denominator}}}};
    const auto parsed = quantity(result);
    if (parsed.metres != value.metres || parsed.exact_metres != value.exact_metres ||
        parsed.entered_unit != value.entered_unit || parsed.original_expression != value.original_expression)
        invalid("Hosted opening rehost quantity is internally inconsistent");
    return result;
}

const Entity& owner(const Entities& entities, const std::string& id, const char* type) {
    const auto found = entities.find(id);
    if (found == entities.end() || found->second.id != id || found->second.type != type ||
        !found->second.properties.is_object())
        invalid("Hosted opening rehost owner is missing, inconsistent or has the wrong type");
    return found->second;
}

DrawingContext context(const Entities& entities, const ProjectOrganization& organization,
                       const std::string& id) {
    const auto result = organization.drawing_context(id);
    if (!result || !result->complete())
        invalid("Hosted opening rehost owner has unresolved drawing context");
    // The derived resolver does not establish map-key/entity-ID agreement for
    // its ancestor references. Admit the actual context owners independently.
    (void)owner(entities, result->property_id, "property");
    (void)owner(entities, result->building_id, "building");
    (void)owner(entities, result->floor_id, "floor");
    (void)owner(entities, result->layer_id, "layer");
    return *result;
}

bool same_floor(const DrawingContext& left, const DrawingContext& right) {
    return left.property_id == right.property_id && left.building_id == right.building_id &&
        left.floor_id == right.floor_id && left.level_id == right.level_id;
}

Wall wall(const Entities& entities, const Entity& host) {
    Wall result;
    std::string error;
    if (!read_document_wall(resolve_vertical_placement(entities, host), {}, result, error))
        throw std::invalid_argument("Hosted opening rehost wall " + host.id + ": " + error);
    validate_wall_semantics(result);
    return result;
}

void station_receipt(Entity& opening, const Quantity& offset) {
    opening.properties["offset_m"] = offset.metres;
    if (!opening.properties.contains("quantity_entries"))
        opening.properties["quantity_entries"] = Json::object();
    auto& entries = opening.properties.at("quantity_entries");
    const auto encoded = quantity(offset);
    const auto write = [&](const std::string& pointer) {
        const auto found = entries.find(pointer);
        if (found == entries.end()) { entries[pointer] = encoded; return; }
        // Profile replay admitted retained receipts. Preserve their opaque
        // fields while replacing only this operation's exact station authority.
        for (const auto* field : {"version", "original_expression", "entered_unit"})
            (*found)[field] = encoded.at(field);
        for (const auto* field : {"numerator", "denominator"})
            (*found)["exact_metres"][field] = encoded.at("exact_metres").at(field);
    };
    write("/offset_m");
    if (opening.properties.contains("offset") && entries.contains("/offset")) write("/offset");
}

using RegistryMemberships = std::map<std::string, const PhysicalWallPhaseState*, std::less<>>;
RegistryMemberships rehost_memberships(const ConstraintPhaseScope& scope) {
    RegistryMemberships result;
    for (const auto& registry : scope.registries)
        for (const auto& id : registry.registered_entity_ids)
            if (!result.emplace(id, &registry).second)
                invalid("Hosted opening rehost has overlapping model registry membership");
    return result;
}
void admit_rehost_memberships(const RegistryMemberships& memberships, const HostedOpeningRehostIntent& intent) {
    const PhysicalWallPhaseState* actual = nullptr;
    for (const auto* id : {&intent.opening_id, &intent.original_wall_id, &intent.target_wall_id}) {
        const auto member = memberships.find(*id);
        if (member == memberships.end()) continue;
        if (actual && actual != member->second)
            invalid("Opening and both hosts must belong to the same design registry");
        actual = member->second;
    }
}
} // namespace

nlohmann::json encode_hosted_opening_rehost_intent(const HostedOpeningRehostIntent& intent) {
    (void)identity(intent.opening_id);
    (void)identity(intent.original_wall_id);
    (void)identity(intent.target_wall_id);
    Json result{{"version", 1}, {"opening_id", intent.opening_id},
        {"original_wall_id", intent.original_wall_id}, {"target_wall_id", intent.target_wall_id},
        {"offset", quantity(intent.offset)}};
    if (result.dump().size() > proof_limit) invalid("Hosted opening rehost proof byte budget exceeded");
    return result;
}

HostedOpeningRehostIntent decode_hosted_opening_rehost_intent(const nlohmann::json& value) {
    keys(value, {"version", "opening_id", "original_wall_id", "target_wall_id", "offset"});
    if (!value.at("version").is_number_integer() || value.at("version") != 1)
        invalid("Hosted opening rehost version is unsupported");
    if (value.dump().size() > proof_limit) invalid("Hosted opening rehost proof byte budget exceeded");
    HostedOpeningRehostIntent result{identity(value.at("opening_id")),
        identity(value.at("original_wall_id")), identity(value.at("target_wall_id")),
        quantity(value.at("offset"))};
    if (encode_hosted_opening_rehost_intent(result).dump() != value.dump())
        invalid("Hosted opening rehost proof is not canonical");
    return result;
}

std::optional<PhaseWallReplacementRequest> phase_hosted_opening_rehost_replacement_request(
    const Entities& source, const std::vector<HostedOpeningRehostIntent>& intents) {
    if (intents.size() > 2048) invalid("Proposed opening rehost target budget exceeded");
    if (intents.empty()) return std::nullopt;
    const auto scope = constraint_phase_scope(source);
    const auto memberships = rehost_memberships(scope);
    RegistryMemberships shared_hosts, shared_openings;
    for (const auto& registry : scope.registries) {
        if (!registry.alternative_id) continue;
        const auto model = ModelPhases::from_json(owner(source, registry.registry_id, "model_phases").properties.at("model"));
        for (const auto& id : model.baseline_ids()) {
            const auto found = source.find(id);
            if (found == source.end() || found->second.id != id)
                invalid("Proposed opening rehost baseline owner is missing or inconsistent");
            if (scope.inactive_owner_ids.contains(id)) continue;
            if (found->second.type == "wall") shared_hosts.emplace(id, &registry);
            else if (found->second.type == "opening") shared_openings.emplace(id, &registry);
        }
    }
    std::set<std::string, std::less<>> openings, seeds;
    const PhysicalWallPhaseState* selected = nullptr;
    std::size_t proof_bytes = 0;
    for (const auto& intent : intents) {
        const auto bytes = encode_hosted_opening_rehost_intent(intent).dump().size();
        if (bytes > proof_limit - proof_bytes) invalid("Proposed opening rehost proof byte budget exceeded");
        proof_bytes += bytes;
        if (!openings.insert(intent.opening_id).second) invalid("Proposed opening rehost contains duplicate targets");
        admit_rehost_memberships(memberships, intent);
        const auto& opening = owner(source, intent.opening_id, "opening");
        (void)owner(source, intent.original_wall_id, "wall");
        (void)owner(source, intent.target_wall_id, "wall");
        for (const auto* id : {&intent.opening_id, &intent.original_wall_id, &intent.target_wall_id})
            if (scope.inactive_owner_ids.contains(*id)) invalid("Proposed opening rehost owner is inactive in the saved design");
        std::string actual_host, error;
        if (!read_document_wall_id(opening, actual_host, error) || actual_host != intent.original_wall_id)
            invalid("Proposed opening rehost must name its actual original host");
        const auto baseline_opening = shared_openings.find(intent.opening_id);
        const auto old_host = shared_hosts.find(intent.original_wall_id);
        if (baseline_opening != shared_openings.end() &&
            (old_host == shared_hosts.end() || old_host->second != baseline_opening->second))
            invalid("A retained baseline opening on a nonbaseline host requires unsupported replacement ownership");
        for (const auto* id : {&intent.original_wall_id, &intent.target_wall_id}) {
            const auto shared = shared_hosts.find(*id);
            if (shared == shared_hosts.end()) continue;
            if (selected && selected != shared->second)
                invalid("Proposed opening rehost reaches several saved-active design registries");
            selected = shared->second;
            seeds.insert(*id);
        }
    }
    if (!selected) return std::nullopt;
    for (const auto& intent : intents) {
        if (!shared_hosts.contains(intent.original_wall_id) && !shared_hosts.contains(intent.target_wall_id))
            invalid("Each proposed opening rehost must qualify through its own actual old or target baseline host");
        for (const auto* id : {&intent.opening_id, &intent.original_wall_id, &intent.target_wall_id}) {
            const auto registered = memberships.find(*id);
            if (registered != memberships.end() && registered->second != selected)
                invalid("Proposed opening rehost owner belongs to another design registry");
        }
    }
    return PhaseWallReplacementRequest{selected->registry_id, *selected->alternative_id,
        std::vector<std::string>(seeds.begin(), seeds.end())};
}

std::map<std::string, Entity, std::less<>> replay_hosted_opening_rehost_entities(
    const std::map<std::string, Entity, std::less<>>& source,
    const std::vector<HostedOpeningRehostIntent>& intents, bool validate_final_constraints) {
    if (intents.size() > collection_limit) invalid("Hosted opening rehost target budget exceeded");
    if (intents.empty()) return source;
    const auto scope = constraint_phase_scope(source);
    const auto memberships = rehost_memberships(scope);
    const auto organization = organize_project(source);
    std::set<std::string, std::less<>> targets, hosts;
    std::map<std::string, DrawingContext, std::less<>> retained_contexts;
    auto result = source;
    std::size_t proof_bytes = 0;
    for (const auto& intent : intents) {
        const auto bytes = encode_hosted_opening_rehost_intent(intent).dump().size();
        if (bytes > proof_limit - proof_bytes)
            invalid("Hosted opening rehost batch proof byte budget exceeded");
        proof_bytes += bytes;
        if (!targets.insert(intent.opening_id).second)
            invalid("Hosted opening rehost contains duplicate targets");
        admit_rehost_memberships(memberships, intent);
        for (const auto* id : {&intent.opening_id, &intent.original_wall_id, &intent.target_wall_id})
            if (scope.inactive_owner_ids.contains(*id))
                invalid("Hosted opening rehost opening or host is inactive in the saved design");
        const auto& opening = owner(source, intent.opening_id, "opening");
        const auto& original = owner(source, intent.original_wall_id, "wall");
        const auto& target = owner(source, intent.target_wall_id, "wall");
        const auto opening_context = context(source, organization, intent.opening_id);
        const auto original_context = context(source, organization, intent.original_wall_id);
        const auto target_context = context(source, organization, intent.target_wall_id);
        if (!same_floor(opening_context, original_context) || !same_floor(original_context, target_context))
            invalid("Hosted opening rehost requires the same resolved property, building and floor");
        if (wall(source, original).elevation != wall(source, target).elevation)
            invalid("Hosted opening rehost host base planes differ");
        // Validate any retained opening placement; level-driven opening placement
        // is unsupported because actual physical Z belongs to the host wall.
        (void)resolve_vertical_placement(source, opening);
        HostedOpeningProfileEditIntent profile;
        profile.opening_id = intent.opening_id;
        profile.wall_id = intent.original_wall_id;
        profile.offset = intent.offset;
        // This independently admits the original complete profile, aliases and
        // retained receipts before applying only the exact authored station.
        auto replacement = replay_hosted_opening_profile_entity(opening, profile);
        if (intent.target_wall_id != intent.original_wall_id) {
            replacement.properties.at("wall_id") = intent.target_wall_id;
            // A real rehost retains the authored exact station even when its
            // numeric value equals the previous host's station. Same-host
            // unchanged station continues to preserve the exact source record.
            station_receipt(replacement, intent.offset);
        }
        result.at(intent.opening_id) = std::move(replacement);
        retained_contexts.emplace(intent.opening_id, opening_context);
        hosts.insert(intent.original_wall_id);
        hosts.insert(intent.target_wall_id);
        if (hosts.size() > collection_limit) invalid("Hosted opening rehost host budget exceeded");
    }
    // Never let a valid final arrangement legitimize a malformed original cut
    // or assembly. Both checks use the actual full maps and saved-active scope.
    validate_active_wall_physical_dependencies(source, hosts);
    const auto completed_organization = organize_project(result);
    for (const auto& [id, retained] : retained_contexts)
        if (context(result, completed_organization, id) != retained)
            invalid("Hosted opening rehost must retain its opening drawing context and layer");
    validate_active_wall_physical_dependencies(result, hosts);
    if (validate_final_constraints) {
        if (const auto diagnostic = validate_active_phase_constraint_integrity(result))
            throw std::invalid_argument(*diagnostic);
    }
    return result;
}

} // namespace sketch
