#include "sketch/phase_corner_window_edit.hpp"

#include "sketch/constraint_integrity.hpp"
#include "sketch/constraint_phase_scope.hpp"
#include "sketch/corner_window.hpp"
#include "sketch/corner_window_edit.hpp"
#include "sketch/model_phases.hpp"
#include "sketch/phase_wall_profile_edit.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <set>
#include <stdexcept>
#include <string_view>

namespace sketch {
namespace {
using Json = nlohmann::json;
using Entities = std::map<std::string, Entity, std::less<>>;
constexpr std::size_t proof_limit = 1024 * 1024;
constexpr std::size_t collection_limit = 2048;
constexpr std::array pointers{
    "/widths_m/0", "/widths_m/1", "/sill_m", "/height_m",
    "/opening_assembly/frame_width_m", "/opening_assembly/frame_depth_m",
    "/opening_assembly/panel_thickness_m", "/opening_assembly/glazing_thickness_m",
    "/opening_assembly/inset_m"};

[[noreturn]] void invalid(const std::string& reason) {
    throw std::invalid_argument("Corner window profile: " + reason);
}
std::string identity(const Json& value) {
    if (!value.is_string()) invalid("identity must be a string");
    auto result = value.get<std::string>();
    if (result.empty() || result.size() > 128 || !std::all_of(result.begin(), result.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == ':';
    })) invalid("identity is invalid");
    return result;
}
bool exact(const Entity& left, const Entity& right) {
    return left == right && left.properties.dump() == right.properties.dump() &&
        left.extensions.dump() == right.extensions.dump();
}
const Entity& owner(const Entities& source, const std::string& id) {
    const auto found = source.find(id);
    if (found == source.end() || found->second.id != id || found->second.type != "corner_window")
        invalid("target must be its actual corner-window owner");
    (void)parse_corner_window(found->second);
    return found->second;
}
void stage(Entity& target, const CornerWindowProfileEditIntent& intent) {
    for (const auto& [pointer, value] : intent.dimensions) {
        const Json::json_pointer path(pointer);
        // Preserve integer/float storage and signed zero on exact no-ops.
        if (target.properties.at(path).get<double>() != value) target.properties[path] = value;
    }
    if (intent.name && (!target.properties.contains("name") || target.properties.at("name") != *intent.name))
        target.properties["name"] = *intent.name;
}
} // namespace

Json encode_corner_window_profile_edit_intent(const CornerWindowProfileEditIntent& intent) {
    (void)identity(intent.owner_id);
    if (intent.name && intent.name->size() > 4096) invalid("name budget exceeded");
    if (!intent.name && intent.dimensions.empty()) invalid("an authored field is required");
    if (intent.dimensions.size() > pointers.size()) invalid("dimension inventory is invalid");
    Json dimensions = Json::object();
    for (const auto& [pointer, value] : intent.dimensions) {
        if (!std::any_of(pointers.begin(), pointers.end(), [&](const char* allowed) {
                return std::string_view(allowed) == pointer;
            }) ||
            !std::isfinite(value)) invalid("dimension pointer or value is unsupported");
        dimensions[pointer] = value;
    }
    Json result{{"version", 1}, {"owner_id", intent.owner_id},
        {"name", intent.name ? Json(*intent.name) : Json(nullptr)}, {"dimensions", std::move(dimensions)}};
    if (result.dump().size() > proof_limit) invalid("proof byte budget exceeded");
    return result;
}

CornerWindowProfileEditIntent decode_corner_window_profile_edit_intent(const Json& value) {
    if (!value.is_object() || value.size() != 4 || !value.contains("version") ||
        !value.contains("owner_id") || !value.contains("name") || !value.contains("dimensions") ||
        !value.at("version").is_number_integer() || value.at("version") != 1)
        invalid("fields or version are unsupported");
    if (value.dump().size() > proof_limit) invalid("proof byte budget exceeded");
    CornerWindowProfileEditIntent result;
    result.owner_id = identity(value.at("owner_id"));
    if (!value.at("name").is_null()) {
        if (!value.at("name").is_string()) invalid("name must be null or a string");
        result.name = value.at("name").get<std::string>();
    }
    if (!value.at("dimensions").is_object()) invalid("dimensions must be an object");
    for (const auto& [pointer, dimension] : value.at("dimensions").items()) {
        if (!dimension.is_number()) invalid("dimension must be a finite number");
        result.dimensions.emplace(pointer, dimension.get<double>());
    }
    (void)encode_corner_window_profile_edit_intent(result);
    return result;
}

Entities replay_corner_window_profile_entities(const Entities& source,
    const std::vector<CornerWindowProfileEditIntent>& intents, bool validate_final_constraints) {
    if (intents.size() > collection_limit) invalid("target budget exceeded");
    if (intents.empty()) return source;
    const auto scope = constraint_phase_scope(source);
    auto result = source;
    std::set<std::string, std::less<>> targets, hosts;
    std::size_t bytes = 0;
    for (const auto& intent : intents) {
        const auto count = encode_corner_window_profile_edit_intent(intent).dump().size();
        if (count > proof_limit - bytes) invalid("batch proof byte budget exceeded");
        bytes += count;
        if (!targets.insert(intent.owner_id).second) invalid("duplicate target");
        const auto corner = parse_corner_window(owner(source, intent.owner_id));
        for (const auto& id : {corner.id, corner.wall_ids[0], corner.wall_ids[1],
                corner.opening_ids[0], corner.opening_ids[1]})
            if (scope.inactive_owner_ids.contains(id)) invalid("target cohort is inactive in the saved design");
        stage(result.at(intent.owner_id), intent);
        hosts.insert(corner.wall_ids.begin(), corner.wall_ids.end());
    }
    // Complete actual full-map source admission precedes all derived cuts and
    // receipt updates. Failure cannot mutate the captured source.
    complete_corner_window_geometry(source, result);
    validate_corner_window_state(result);
    validate_active_wall_physical_dependencies(source, hosts, true);
    validate_active_wall_physical_dependencies(result, hosts, true);
    if (validate_final_constraints)
        if (const auto error = validate_active_phase_constraint_integrity(result)) invalid(*error);
    return result;
}

std::optional<CornerWindowProfileEditIntent> make_corner_window_profile_edit_intent(
    const Entities& source, const Entity& replacement) {
    const auto& original = owner(source, replacement.id);
    auto candidate = source;
    candidate.at(replacement.id) = replacement;
    complete_corner_window_geometry(source, candidate);
    const auto& completed = candidate.at(replacement.id);
    CornerWindowProfileEditIntent intent;
    intent.owner_id = replacement.id;
    for (const auto* pointer : pointers) {
        const Json::json_pointer path(pointer);
        if (original.properties.at(path).get<double>() != completed.properties.at(path).get<double>())
            intent.dimensions.emplace(pointer, completed.properties.at(path).get<double>());
    }
    if (completed.properties.contains("name") &&
        (!original.properties.contains("name") || original.properties.at("name") != completed.properties.at("name"))) {
        if (!completed.properties.at("name").is_string()) invalid("authored name must be a string");
        intent.name = completed.properties.at("name").get<std::string>();
    }
    auto expected = source;
    if (intent.name || !intent.dimensions.empty())
        expected = replay_corner_window_profile_entities(source, {intent}, false);
    // Exact whole-map comparison catches newly added unknown fields as well as
    // erased names, envelopes, identities, topology and receipt substitutions.
    for (const auto& [id, entity] : candidate)
        if (!exact(entity, expected.at(id))) invalid("replacement contains an unsupported edit");
    if (!intent.name && intent.dimensions.empty()) return std::nullopt;
    return intent;
}

std::optional<PhaseWallReplacementRequest> phase_corner_window_profile_replacement_request(
    const Entities& source, const std::vector<CornerWindowProfileEditIntent>& intents) {
    if (intents.empty()) return std::nullopt;
    const auto replayed = replay_corner_window_profile_entities(source, intents, false);
    const auto scope = constraint_phase_scope(source);
    std::map<std::string, const PhysicalWallPhaseState*, std::less<>> memberships, baselines;
    for (const auto& registry : scope.registries) {
        for (const auto& id : registry.registered_entity_ids)
            if (!memberships.emplace(id, &registry).second) invalid("overlapping registry membership");
        if (!registry.alternative_id) continue;
        const auto model = ModelPhases::from_json(source.at(registry.registry_id).properties.at("model"));
        for (const auto& id : model.baseline_ids()) baselines.emplace(id, &registry);
    }
    const PhysicalWallPhaseState* selected = nullptr;
    bool ordinary = false;
    std::set<std::string, std::less<>> seeds;
    for (const auto& intent : intents) {
        const auto& original = owner(source, intent.owner_id);
        if (exact(original, replayed.at(intent.owner_id))) continue;
        const auto corner = parse_corner_window(original);
        const std::array ids{corner.id, corner.wall_ids[0], corner.wall_ids[1],
            corner.opening_ids[0], corner.opening_ids[1]};
        // Proposed corner owners may legitimately use retained baseline hosts.
        // Host membership alone never lends baseline replacement authority.
        const auto baseline_owner = baselines.find(corner.id);
        if (baseline_owner == baselines.end()) { ordinary = true; continue; }
        const auto* cohort = baseline_owner->second;
        for (const auto& id : ids) {
            const auto member = memberships.find(id), baseline = baselines.find(id);
            if (member == memberships.end() || member->second != cohort ||
                baseline == baselines.end() || baseline->second != cohort)
                invalid("owner, both hosts and cuts require the same actual baseline membership");
        }
        if (selected && selected != cohort) invalid("edits reach several saved-active registries");
        selected = cohort;
        seeds.insert(corner.wall_ids.begin(), corner.wall_ids.end());
    }
    if (selected && ordinary) invalid("edits mix baseline and ordinary/proposed cohorts");
    if (!selected) return std::nullopt;
    return PhaseWallReplacementRequest{selected->registry_id, *selected->alternative_id,
        std::vector<std::string>(seeds.begin(), seeds.end())};
}

} // namespace sketch
