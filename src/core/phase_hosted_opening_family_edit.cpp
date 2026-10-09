#include "sketch/phase_hosted_opening_family_edit.hpp"

#include "sketch/constraint_integrity.hpp"
#include "sketch/constraint_phase_scope.hpp"
#include "sketch/document_wall.hpp"
#include "sketch/model_phases.hpp"
#include "sketch/phase_hosted_opening_edit.hpp"
#include "sketch/phase_wall_profile_edit.hpp"

#include <algorithm>
#include <initializer_list>
#include <set>
#include <stdexcept>

namespace sketch {
namespace {
using Json = nlohmann::json;
constexpr std::size_t collection_limit = 4096;
constexpr std::size_t proof_limit = 1024 * 1024;

[[noreturn]] void invalid(const char* reason) { throw std::invalid_argument(reason); }

void keys(const Json& value, std::initializer_list<const char*> expected) {
    if (!value.is_object() || value.size() != expected.size())
        invalid("Hosted opening family edit fields are invalid");
    for (const auto* field : expected)
        if (!value.contains(field)) invalid("Hosted opening family edit field is missing");
}

std::string identity(const Json& value) {
    if (!value.is_string()) invalid("Hosted opening family identity must be a string");
    const auto& result = value.get_ref<const std::string&>();
    if (result.empty() || result.size() > 128 ||
        !std::all_of(result.begin(), result.end(), [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == ':';
        })) invalid("Hosted opening family identity is invalid");
    return result;
}

const char* family_name(HostedOpeningFamily value) {
    switch (value) {
    case HostedOpeningFamily::door: return "door";
    case HostedOpeningFamily::window: return "window";
    case HostedOpeningFamily::opening: return "opening";
    }
    invalid("Hosted opening family is unsupported");
}

HostedOpeningFamily family(const Json& value) {
    if (!value.is_string()) invalid("Hosted opening family must be a string");
    const auto& text = value.get_ref<const std::string&>();
    if (text == "door") return HostedOpeningFamily::door;
    if (text == "window") return HostedOpeningFamily::window;
    if (text == "opening") return HostedOpeningFamily::opening;
    invalid("Hosted opening family is unsupported");
}

HostedOpeningFamily family(OpeningAssemblyKind value) {
    switch (value) {
    case OpeningAssemblyKind::door: return HostedOpeningFamily::door;
    case OpeningAssemblyKind::window: return HostedOpeningFamily::window;
    case OpeningAssemblyKind::passage: return HostedOpeningFamily::opening;
    }
    invalid("Hosted opening assembly family is unsupported");
}

void admit_final_profile(const HostedOpeningFamilyEditIntent& intent) {
    (void)family_name(intent.target_family);
    if (intent.framed_passage_transition && intent.target_family != HostedOpeningFamily::opening)
        invalid("Passage framing conversion requires the opening family");
    if (intent.target_family == HostedOpeningFamily::opening) {
        if (intent.door_operation)
            invalid("An opening cannot carry a door operation");
        if (intent.assembly) {
            if (!intent.framed_passage_transition || intent.assembly->kind != OpeningAssemblyKind::passage)
                invalid("Only an explicit passage transition can frame an opening");
            (void)opening_assembly_json(*intent.assembly);
        }
        return;
    }
    if (!intent.assembly || family(intent.assembly->kind) != intent.target_family)
        invalid("Hosted opening family conversion requires an explicit matching final assembly");
    (void)opening_assembly_json(*intent.assembly);
    if (intent.door_operation) {
        if (intent.target_family != HostedOpeningFamily::door)
            invalid("Only the final door family can carry a door operation");
        (void)encode_door_operation(*intent.door_operation);
    }
}

struct Profile {
    HostedOpeningFamily family{HostedOpeningFamily::opening};
    std::optional<OpeningAssembly> assembly;
    std::optional<DoorOperation> operation;
};

// Called only after independent existing-profile validation admitted all dimensions,
// aliases, known receipts, family/profile agreement and operation authority.
Profile source_profile(const Entity& source) {
    Profile result;
    const auto kind = source.properties.find("opening_kind");
    if (kind != source.properties.end()) result.family = family(*kind);
    if (const auto assembly = source.properties.find("opening_assembly");
        assembly != source.properties.end()) {
        result.assembly = parse_opening_assembly(*assembly);
        const auto assembly_family = family(result.assembly->kind);
        if (kind != source.properties.end() && result.family != assembly_family)
            invalid("Hosted opening source family and assembly disagree");
        result.family = assembly_family;
    } else if (result.family != HostedOpeningFamily::opening) {
        result.assembly = default_opening_assembly(result.family == HostedOpeningFamily::door
            ? OpeningAssemblyKind::door : OpeningAssemblyKind::window);
    }
    if (const auto operation = source.properties.find("door_operation");
        operation != source.properties.end()) {
        result.operation = decode_door_operation(*operation);
        if (result.family != HostedOpeningFamily::door)
            invalid("Hosted opening source operation requires a door family");
    }
    return result;
}

bool exact(const Entity& left, const Entity& right) {
    return left == right && left.properties.dump() == right.properties.dump() &&
        left.extensions.dump() == right.extensions.dump();
}

using Entities = std::map<std::string, Entity, std::less<>>;
using RegistryMemberships = std::map<std::string, const PhysicalWallPhaseState*, std::less<>>;

const Entity& owner(const Entities& entities, const std::string& id, const char* type) {
    const auto found = entities.find(id);
    if (found == entities.end() || found->second.id != id || found->second.type != type ||
        !found->second.properties.is_object())
        invalid("Hosted opening family owner is missing, inconsistent or has the wrong type");
    return found->second;
}

RegistryMemberships family_memberships(const ConstraintPhaseScope& scope) {
    RegistryMemberships result;
    for (const auto& registry : scope.registries)
        for (const auto& id : registry.registered_entity_ids)
            if (!result.emplace(id, &registry).second)
                invalid("Hosted opening family has overlapping model registry membership");
    return result;
}

void admit_family_memberships(const RegistryMemberships& memberships,
                             const HostedOpeningFamilyEditIntent& intent) {
    const auto opening = memberships.find(intent.opening_id);
    const auto host = memberships.find(intent.wall_id);
    if (opening != memberships.end() && host != memberships.end() && opening->second != host->second)
        invalid("Opening family target and host must belong to the same design registry");
}
} // namespace

nlohmann::json encode_hosted_opening_family_edit_intent(const HostedOpeningFamilyEditIntent& intent) {
    (void)identity(intent.opening_id);
    (void)identity(intent.wall_id);
    admit_final_profile(intent);
    Json result{{"version", intent.framed_passage_transition ? 2 : 1}, {"opening_id", intent.opening_id}, {"wall_id", intent.wall_id},
        {"target_family", family_name(intent.target_family)},
        {"assembly", intent.assembly ? opening_assembly_json(*intent.assembly) : Json(nullptr)},
        {"door_operation", intent.door_operation ? encode_door_operation(*intent.door_operation) : Json(nullptr)}};
    if (intent.framed_passage_transition) result["framed_passage_transition"] = true;
    if (result.dump().size() > proof_limit) invalid("Hosted opening family proof byte budget exceeded");
    return result;
}

HostedOpeningFamilyEditIntent decode_hosted_opening_family_edit_intent(const nlohmann::json& value) {
    if (!value.is_object() || !value.contains("version") || !value.at("version").is_number_integer() ||
        (value.at("version") != 1 && value.at("version") != 2))
        invalid("Hosted opening family edit version is unsupported");
    const bool passage_transition = value.at("version") == 2;
    if (passage_transition) {
        keys(value, {"version", "opening_id", "wall_id", "target_family", "assembly", "door_operation", "framed_passage_transition"});
        if (!value.at("framed_passage_transition").is_boolean() || value.at("framed_passage_transition") != true)
            invalid("Passage transition flag must be true");
    } else keys(value, {"version", "opening_id", "wall_id", "target_family", "assembly", "door_operation"});
    if (value.dump().size() > proof_limit) invalid("Hosted opening family proof byte budget exceeded");
    HostedOpeningFamilyEditIntent result;
    result.opening_id = identity(value.at("opening_id"));
    result.wall_id = identity(value.at("wall_id"));
    result.target_family = family(value.at("target_family"));
    result.framed_passage_transition = passage_transition;
    if (!value.at("assembly").is_null()) result.assembly = parse_opening_assembly(value.at("assembly"));
    if (!value.at("door_operation").is_null()) result.door_operation = decode_door_operation(value.at("door_operation"));
    if (encode_hosted_opening_family_edit_intent(result).dump() != value.dump())
        invalid("Hosted opening family proof is not canonical");
    return result;
}

Entity replay_hosted_opening_family_entity(const Entity& source, const HostedOpeningFamilyEditIntent& intent) {
    (void)encode_hosted_opening_family_edit_intent(intent);
    if (source.id != intent.opening_id || source.type != "opening" || !source.properties.is_object())
        invalid("Hosted opening family conversion requires its actual existing opening");
    std::string host, diagnostic;
    if (!read_document_wall_id(source, host, diagnostic) || host != intent.wall_id)
        invalid("Hosted opening family conversion must retain its actual existing host");

    validate_hosted_opening_profile_entity(source);
    const auto original = source_profile(source);
    if (intent.target_family == original.family) {
        if (intent.assembly == original.assembly && intent.door_operation == original.operation)
            return source;
        // Bare cut <-> framed passage is the only same-family conversion. A
        // change between two framed profiles still uses the profile-edit API.
        if (!intent.framed_passage_transition || original.family != HostedOpeningFamily::opening ||
            original.assembly.has_value() == intent.assembly.has_value())
            invalid("Same-family opening changes must use the existing hosted opening profile edit command");
    }

    auto result = source;
    result.properties["opening_kind"] = family_name(intent.target_family);
    if (intent.assembly) result.properties["opening_assembly"] = opening_assembly_json(*intent.assembly);
    else result.properties.erase("opening_assembly");
    if (intent.door_operation) result.properties["door_operation"] = encode_door_operation(*intent.door_operation);
    else result.properties.erase("door_operation");
    // This checks the complete final profile against its retained scalar authority;
    // no source dimensions, representation or opaque data are reconstructed.
    validate_hosted_opening_profile_entity(result);
    const auto final = source_profile(result);
    if (final.family != intent.target_family || final.assembly != intent.assembly ||
        final.operation != intent.door_operation)
        invalid("Hosted opening final family differs from independently admitted intent");
    return result;
}

std::optional<PhaseWallReplacementRequest> phase_hosted_opening_family_replacement_request(
    const Entities& source, const std::vector<HostedOpeningFamilyEditIntent>& intents) {
    if (intents.size() > 2048) invalid("Proposed opening family target budget exceeded");
    if (intents.empty()) return std::nullopt;
    const auto scope = constraint_phase_scope(source);
    const auto memberships = family_memberships(scope);
    RegistryMemberships shared_hosts, shared_openings;
    for (const auto& registry : scope.registries) {
        if (!registry.alternative_id) continue;
        const auto model = ModelPhases::from_json(owner(source, registry.registry_id, "model_phases").properties.at("model"));
        for (const auto& id : model.baseline_ids()) {
            const auto found = source.find(id);
            if (found == source.end() || found->second.id != id)
                invalid("Proposed opening family baseline owner is missing or inconsistent");
            if (scope.inactive_owner_ids.contains(id)) continue;
            if (found->second.type == "wall") shared_hosts.emplace(id, &registry);
            else if (found->second.type == "opening") shared_openings.emplace(id, &registry);
        }
    }
    std::set<std::string, std::less<>> openings, seeds;
    const PhysicalWallPhaseState* selected = nullptr;
    std::size_t proof_bytes = 0;
    for (const auto& intent : intents) {
        const auto bytes = encode_hosted_opening_family_edit_intent(intent).dump().size();
        if (bytes > proof_limit - proof_bytes) invalid("Proposed opening family proof byte budget exceeded");
        proof_bytes += bytes;
        if (!openings.insert(intent.opening_id).second) invalid("Proposed opening family contains duplicate targets");
        admit_family_memberships(memberships, intent);
        const auto& opening = owner(source, intent.opening_id, "opening");
        (void)owner(source, intent.wall_id, "wall");
        if (scope.inactive_owner_ids.contains(intent.opening_id) || scope.inactive_owner_ids.contains(intent.wall_id))
            invalid("Proposed opening family target or host is inactive in the saved design");
        std::string actual_host, error;
        if (!read_document_wall_id(opening, actual_host, error) || actual_host != intent.wall_id)
            invalid("Proposed opening family must retain its actual original host");

        // Pure entity admission recognizes an exact no-op without solving any
        // original host or fabricating a map. Only a real edit needs copies.
        if (exact(replay_hosted_opening_family_entity(opening, intent), opening)) continue;
        const auto baseline_opening = shared_openings.find(intent.opening_id);
        const auto host = shared_hosts.find(intent.wall_id);
        if (baseline_opening != shared_openings.end() &&
            (host == shared_hosts.end() || host->second != baseline_opening->second))
            invalid("A retained baseline opening on a nonbaseline host requires unsupported replacement ownership");
        if (host == shared_hosts.end()) continue;
        if (selected && selected != host->second)
            invalid("Proposed opening family reaches several saved-active design registries");
        selected = host->second;
    }
    if (!selected) return std::nullopt;
    for (const auto& intent : intents) {
        const auto host = shared_hosts.find(intent.wall_id);
        if (host == shared_hosts.end() || host->second != selected)
            invalid("Each proposed opening family row requires its own actual baseline host in the selected registry");
        for (const auto* id : {&intent.opening_id, &intent.wall_id}) {
            const auto member = memberships.find(*id);
            if (member == memberships.end() || member->second != selected)
                invalid("Proposed opening family target and host require actual membership in the selected design registry");
        }
        seeds.insert(intent.wall_id);
    }
    return PhaseWallReplacementRequest{selected->registry_id, *selected->alternative_id,
        std::vector<std::string>(seeds.begin(), seeds.end())};
}

std::map<std::string, Entity, std::less<>> replay_hosted_opening_family_entities(
    const std::map<std::string, Entity, std::less<>>& source,
    const std::vector<HostedOpeningFamilyEditIntent>& intents, bool validate_final_constraints) {
    if (intents.size() > collection_limit) invalid("Hosted opening family target budget exceeded");
    if (intents.empty()) return source;
    const auto scope = constraint_phase_scope(source);
    const auto memberships = family_memberships(scope);
    std::set<std::string, std::less<>> targets, hosts;
    auto result = source;
    std::size_t proof_bytes = 0;
    for (const auto& intent : intents) {
        const auto bytes = encode_hosted_opening_family_edit_intent(intent).dump().size();
        if (bytes > proof_limit - proof_bytes)
            invalid("Hosted opening family batch proof byte budget exceeded");
        proof_bytes += bytes;
        if (!targets.insert(intent.opening_id).second)
            invalid("Hosted opening family conversion contains duplicate targets");
        admit_family_memberships(memberships, intent);
        if (scope.inactive_owner_ids.contains(intent.opening_id) || scope.inactive_owner_ids.contains(intent.wall_id))
            invalid("Hosted opening family target or host is inactive in the saved design");
        const auto target = source.find(intent.opening_id);
        const auto host = source.find(intent.wall_id);
        if (target == source.end() || target->second.id != target->first ||
            host == source.end() || host->second.id != host->first || host->second.type != "wall")
            invalid("Hosted opening family target or host is missing or inconsistent");
        result.at(intent.opening_id) = replay_hosted_opening_family_entity(target->second, intent);
        hosts.insert(intent.wall_id);
    }
    // Actual full saved-active maps establish cut/assembly fit and joined native
    // host geometry. A valid final profile cannot legitimize an invalid source.
    validate_active_wall_physical_dependencies(source, hosts,true);
    validate_active_wall_physical_dependencies(result, hosts,true);
    if (validate_final_constraints) {
        if (const auto diagnostic = validate_active_phase_constraint_integrity(result))
            throw std::invalid_argument(*diagnostic);
    }
    return result;
}

} // namespace sketch
