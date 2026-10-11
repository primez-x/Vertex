#include "sketch/phase_selection_property_edit.hpp"

#include "sketch/assembly_document_adapter.hpp"
#include "sketch/constraint_phase_scope.hpp"
#include "sketch/document_digest.hpp"
#include "sketch/model_phases.hpp"
#include "sketch/phase_constraint_authoring.hpp"
#include "sketch/stair_property_review.hpp"
#include "sketch/constraint_entity.hpp"
#include "sketch/retained_authoring_identities.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>
#include <tuple>
#include <type_traits>

namespace sketch {
namespace {
using Json = nlohmann::json;
using Entities = std::map<std::string, Entity, std::less<>>;
using Ids = std::set<std::string, std::less<>>;
using Key = std::tuple<PhaseSelectionPropertyLane, std::string, std::string>;
constexpr const char* source_fences[] = {"expected_revision", "source_snapshot_digest", "source_authoring_digest", "source_entities_digest", "source_saved_revision"};
[[noreturn]] void invalid(const std::string& message) {
    throw std::invalid_argument("Phase Selection Properties: " + message);
}
bool exact(const Entity& a, const Entity& b) {
    return a == b && a.properties.dump() == b.properties.dump() && a.extensions.dump() == b.extensions.dump();
}
void fields(const Json& value, std::initializer_list<const char*> names) {
    if (!value.is_object() || value.size() != names.size()) invalid("unsupported closed fields");
    for (const auto* name : names) if (!value.contains(name)) invalid("missing closed field");
}
void bounds(const Json& value, std::size_t& nodes, std::size_t& bytes, std::size_t depth = 0) {
    if (++nodes > 100000 || depth > 64) invalid("wire complexity budget exceeded");
    const auto add = [&](std::size_t n) { if (n > 4 * 1024 * 1024 - bytes) invalid("wire byte budget exceeded"); bytes += n; };
    const auto quoted = [&](const std::string& text) { add(2); for (unsigned char c : text) add(c < 32 ? 6 : c == '"' || c == '\\' ? 2 : 1); };
    if (value.is_discarded() || value.is_binary() || (value.is_number_float() && !std::isfinite(value.get<double>())))
        invalid("nonportable wire value");
    if (value.is_object()) {
        add(2 + (value.empty() ? 0 : value.size() - 1));
        for (const auto& [name, child] : value.items()) { quoted(name); add(1); bounds(child, nodes, bytes, depth + 1); }
    } else if (value.is_array()) {
        add(2 + (value.empty() ? 0 : value.size() - 1));
        for (const auto& child : value) bounds(child, nodes, bytes, depth + 1);
    } else if (value.is_string()) quoted(value.get_ref<const std::string&>());
    else add(value.is_number() ? 32 : value.is_boolean() ? 5 : 4);
}
void bounds(const Json& value) { std::size_t nodes{}, bytes{}; bounds(value, nodes, bytes); }
std::string text(const Json& value, std::size_t limit = 128) {
    if (!value.is_string()) invalid("expected a string");
    const auto result = value.get<std::string>();
    if (result.empty() || result.size() > limit || std::any_of(result.begin(), result.end(), [](unsigned char c) { return c < 32; }))
        invalid("invalid bounded identity or choice");
    return result;
}
std::string fresh_text(const Json& value) {
    auto result = text(value);
    if (!std::all_of(result.begin(), result.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == ':';
    })) invalid("fresh identities require canonical ASCII tokens");
    return result;
}
const char* lane_name(PhaseSelectionPropertyLane lane) {
    switch (lane) {
    case PhaseSelectionPropertyLane::walls: return "walls";
    case PhaseSelectionPropertyLane::slabs: return "slabs";
    case PhaseSelectionPropertyLane::roofs: return "roofs";
    case PhaseSelectionPropertyLane::structural: return "structural";
    case PhaseSelectionPropertyLane::stairs: return "stairs";
    }
    invalid("unsupported family lane");
}
PhaseSelectionPropertyLane lane(const Json& value) {
    for (const auto kind : {PhaseSelectionPropertyLane::walls, PhaseSelectionPropertyLane::slabs,
        PhaseSelectionPropertyLane::roofs, PhaseSelectionPropertyLane::structural, PhaseSelectionPropertyLane::stairs})
        if (value == lane_name(kind)) return kind;
    invalid("unsupported family lane");
}
const char* family_name(SelectionPropertyFamily family) {
    switch (family) {
    case SelectionPropertyFamily::wall: return "wall";
    case SelectionPropertyFamily::slab: return "slab";
    case SelectionPropertyFamily::hosted_opening: return "hosted_opening";
    case SelectionPropertyFamily::roof: return "roof";
    case SelectionPropertyFamily::column: return "column";
    case SelectionPropertyFamily::beam: return "beam";
    case SelectionPropertyFamily::stair: return "stair";
    case SelectionPropertyFamily::railing: return "railing";
    }
    invalid("unsupported selected family");
}
const char* entity_type(SelectionPropertyFamily family) {
    return family == SelectionPropertyFamily::hosted_opening ? "opening" : family_name(family);
}
PhaseSelectionPropertyLane family_lane(SelectionPropertyFamily family) {
    switch (family) {
    case SelectionPropertyFamily::wall: case SelectionPropertyFamily::hosted_opening: return PhaseSelectionPropertyLane::walls;
    case SelectionPropertyFamily::slab: return PhaseSelectionPropertyLane::slabs;
    case SelectionPropertyFamily::roof: return PhaseSelectionPropertyLane::roofs;
    case SelectionPropertyFamily::column: case SelectionPropertyFamily::beam: return PhaseSelectionPropertyLane::structural;
    case SelectionPropertyFamily::stair: case SelectionPropertyFamily::railing: return PhaseSelectionPropertyLane::stairs;
    }
    invalid("unsupported selected family");
}
Json source_fields(const DocumentSnapshot& source) {
    return {{"expected_revision", source.revision()}, {"source_snapshot_digest", document_snapshot_digest(source)},
        {"source_authoring_digest", document_authoring_source_digest_v2(source)},
        {"source_entities_digest", entity_map_digest(source.entities())},
        {"source_saved_revision", source.saved_revision_optional() ? Json(*source.saved_revision_optional()) : Json(nullptr)}};
}
const char* unit_name(Unit value) {
    switch (value) {
    case Unit::metre: return "m"; case Unit::millimetre: return "mm"; case Unit::centimetre: return "cm";
    case Unit::foot: return "ft"; case Unit::inch: return "in";
    }
    invalid("unsupported quantity unit");
}
Unit unit(const Json& value) {
    for (auto candidate : {Unit::metre, Unit::millimetre, Unit::centimetre, Unit::foot, Unit::inch})
        if (value == unit_name(candidate)) return candidate;
    invalid("unsupported quantity unit");
}
Json quantity_wire(const RoofOpeningQuantityInput& input) {
    const auto& q = input.quantity;
    if (q.original_expression.size() > 4096) invalid("quantity expression budget exceeded");
    const auto parsed = parse_quantity(q.original_expression, input.default_unit);
    if (parsed.metres != q.metres || parsed.exact_metres != q.exact_metres || parsed.entered_unit != q.entered_unit)
        invalid("quantity must reproduce its exact entered input");
    return {{"quantity", {{"version", 1}, {"original_expression", q.original_expression}, {"entered_unit", unit_name(q.entered_unit)},
        {"exact_metres", {{"numerator", q.exact_metres.numerator}, {"denominator", q.exact_metres.denominator}}}}},
        {"default_unit", unit_name(input.default_unit)}};
}
std::optional<RoofOpeningQuantityInput> quantity_input(const Json& value, bool positive) {
    if (value.is_null()) return {};
    fields(value, {"quantity", "default_unit"});
    const auto& raw = value.at("quantity");
    fields(raw, {"version", "original_expression", "entered_unit", "exact_metres"});
    fields(raw.at("exact_metres"), {"numerator", "denominator"});
    if (!raw.at("original_expression").is_string() || raw.at("original_expression").get_ref<const std::string&>().size() > 4096)
        invalid("invalid quantity expression");
    RoofOpeningQuantityInput result{decode_constraint_quantity_receipt(raw), unit(value.at("default_unit"))};
    if (!std::isfinite(result.quantity.metres) || (positive ? result.quantity.metres <= 0 : result.quantity.metres < 0) ||
        quantity_wire(result).dump() != value.dump()) invalid("quantity requires canonical exact input and admitted sign");
    return result;
}
Json skylight_wire(const RoofOpeningGroupProperties& request) {
    const auto q = [](const auto& value) { return value ? quantity_wire(*value) : Json(nullptr); };
    return {{"version", 1}, {"width", q(request.width)}, {"depth", q(request.depth)}, {"frame_width", q(request.frame_width)},
        {"curb_height", q(request.curb_height)}, {"glazing_thickness", q(request.glazing_thickness)},
        {"rotation_radians", request.rotation_radians ? Json(*request.rotation_radians) : Json(nullptr)},
        {"retain_invariant_members", request.retain_invariant_members}};
}
RoofOpeningGroupProperties skylight_request(const Json& wire) {
    const auto& patch = wire.at("skylight_properties");
    fields(patch, {"version", "width", "depth", "frame_width", "curb_height", "glazing_thickness", "rotation_radians", "retain_invariant_members"});
    if (!patch.at("version").is_number_integer() || patch.at("version") != 1 || !patch.at("retain_invariant_members").is_boolean())
        invalid("unsupported skylight Properties codec");
    RoofOpeningGroupProperties result;
    result.width = quantity_input(patch.at("width"), true); result.depth = quantity_input(patch.at("depth"), true);
    result.frame_width = quantity_input(patch.at("frame_width"), true); result.curb_height = quantity_input(patch.at("curb_height"), false);
    result.glazing_thickness = quantity_input(patch.at("glazing_thickness"), true);
    if (!patch.at("rotation_radians").is_null()) {
        if (!patch.at("rotation_radians").is_number()) invalid("skylight angle must be numeric");
        result.rotation_radians = patch.at("rotation_radians").get<double>();
        if (!std::isfinite(*result.rotation_radians) || std::abs(*result.rotation_radians) > std::numbers::pi) invalid("skylight angle must be canonical in [-pi,pi]");
    }
    result.retain_invariant_members = patch.at("retain_invariant_members").get<bool>();
    if (!result.width && !result.depth && !result.frame_width && !result.curb_height && !result.glazing_thickness &&
        !result.rotation_radians && !result.retain_invariant_members) invalid("empty skylight patch requires invariant retention");
    if (skylight_wire(result).dump() != patch.dump()) invalid("noncanonical skylight Properties");
    const auto& rows = wire.at("roof_opening_members");
    if (!rows.is_array() || rows.empty() || rows.size() > 1000) invalid("invalid child roster budget");
    std::set<std::pair<std::string, std::string>> seen;
    for (const auto& row : rows) {
        fields(row, {"roof_id", "opening_id"});
        RoofOpeningGroupMember member{text(row.at("roof_id")), text(row.at("opening_id"))};
        if (!seen.emplace(member.roof_id, member.opening_id).second) invalid("duplicate qualified child selection");
        result.members.push_back(std::move(member));
    }
    return result;
}
PreparedRoofOpeningGroupProperties composite_roof_properties(const Entities& actual,
    const RoofOpeningGroupProperties& request, const std::vector<RoofProfileEditIntent>& profiles) {
    auto complete = request;
    // Preserve complete selected-child proof even for no-ops. The caller's
    // invariant policy remains a separate independently checked row contract.
    complete.retain_invariant_members = true;
    auto prepared = prepare_roof_opening_group_properties_composite(actual, complete, profiles);
    if (!request.retain_invariant_members) for (const auto& member : request.members) {
        const auto row = [&](const Entities& entities) -> const Json& {
            const auto& roster = entities.at(member.roof_id).properties.at("roof_openings");
            const auto found = std::find_if(roster.begin(), roster.end(), [&](const auto& child) { return child.at("id") == member.opening_id; });
            if (found == roster.end()) invalid("actual selected child row disappeared");
            return *found;
        };
        if (row(actual).dump() == row(prepared.candidate_entities).dump()) invalid("unchanged selected skylight requires explicit retain policy");
    }
    return prepared;
}
Json scalar_wire(const DocumentSnapshot& source, const SelectionPropertyEditRequest& request) {
    if (request.selected.empty() || request.selected.size() > 1000 || request.walls.size() > 1000 ||
        request.slabs.size() > 1000 || request.hosted_openings.size() > 1000 || request.roofs.size() > 1000 ||
        request.structural_profiles.size() > 1000 || request.stair_profiles.size() > 1000) invalid("typed scalar target budget exceeded");
    const bool vertical = std::any_of(request.walls.begin(), request.walls.end(), [](const auto& wall) { return wall.elevation.has_value(); });
    const bool slab_carry = std::any_of(request.slabs.begin(), request.slabs.end(), [](const auto& slab) { return slab.coordinate_hosted_elevation; });
    Json result{{"version", slab_carry ? 5 : vertical ? 4 : !request.stair_profiles.empty() ? 3 : !request.structural_profiles.empty() ? 2 : 1},
        {"expected_revision", source.revision()}, {"source_snapshot_digest", document_snapshot_digest(source)},
        {"source_authoring_digest", document_authoring_source_digest_v2(source)},
        {"source_entities_digest", entity_map_digest(source.entities())},
        {"source_saved_revision", source.saved_revision_optional() ? Json(*source.saved_revision_optional()) : Json(nullptr)},
        {"selected", Json::array()}, {"walls", Json::array()}, {"slabs", Json::array()},
        {"hosted_openings", Json::array()}, {"roofs", Json::array()},
        {"unchanged_policy", request.unchanged_policy == SelectionPropertyUnchangedPolicy::retain ? "retain" : "reject"}};
    if (request.unchanged_policy != SelectionPropertyUnchangedPolicy::retain && request.unchanged_policy != SelectionPropertyUnchangedPolicy::reject)
        invalid("unsupported unchanged policy");
    for (const auto& target : request.selected) result["selected"].push_back({{"entity_id", target.entity_id}, {"family", family_name(target.family)}});
    const auto append = [&](const auto& inputs, const char* key, auto encoder) { for (const auto& input : inputs) result[key].push_back(encoder(input)); };
    append(request.walls, "walls", encode_wall_profile_edit_intent);
    append(request.slabs, "slabs", encode_slab_profile_edit_intent);
    append(request.hosted_openings, "hosted_openings", encode_hosted_opening_profile_edit_intent);
    append(request.roofs, "roofs", encode_roof_profile_edit_intent);
    if (vertical || slab_carry || !request.structural_profiles.empty() || !request.stair_profiles.empty()) {
        result["structural_profiles"] = Json::array(); append(request.structural_profiles, "structural_profiles", encode_structural_profile_edit_intent);
    }
    if (vertical || slab_carry || !request.stair_profiles.empty()) {
        result["stair_profiles"] = Json::array(); append(request.stair_profiles, "stair_profiles", encode_stair_profile_edit_intent);
    }
    return validate_selection_property_edit_request(result);
}
SelectionPropertyEditRequest scalar_request(const Json& encoded) {
    (void)validate_selection_property_edit_request(encoded);
    SelectionPropertyEditRequest result;
    for (const auto& row : encoded.at("selected")) {
        SelectionPropertyFamily family{};
        bool matched = false;
        for (auto kind : {SelectionPropertyFamily::wall, SelectionPropertyFamily::slab, SelectionPropertyFamily::hosted_opening,
            SelectionPropertyFamily::roof, SelectionPropertyFamily::column, SelectionPropertyFamily::beam,
            SelectionPropertyFamily::stair, SelectionPropertyFamily::railing})
            if (row.at("family") == family_name(kind)) { family = kind; matched = true; break; }
        if (!matched) invalid("unsupported family");
        result.selected.push_back({row.at("entity_id").get<std::string>(), family});
    }
    const auto read = [&](const char* key, auto decoder, auto& output) { if (encoded.contains(key)) for (const auto& row : encoded.at(key)) output.push_back(decoder(row)); };
    read("walls", decode_wall_profile_edit_intent, result.walls);
    read("slabs", decode_slab_profile_edit_intent, result.slabs);
    read("hosted_openings", decode_hosted_opening_profile_edit_intent, result.hosted_openings);
    read("roofs", decode_roof_profile_edit_intent, result.roofs);
    read("structural_profiles", decode_structural_profile_edit_intent, result.structural_profiles);
    read("stair_profiles", decode_stair_profile_edit_intent, result.stair_profiles);
    result.unchanged_policy = encoded.at("unchanged_policy") == "retain" ? SelectionPropertyUnchangedPolicy::retain : SelectionPropertyUnchangedPolicy::reject;
    return result;
}
void authenticate(const DocumentSnapshot& source, const Json& scalar) {
    if (!source.is_editable() || source.history().empty() || source.revision() >= source.history().size() ||
        source.history().at(source.revision()).revision != source.revision() ||
        (source.saved_revision_optional() && *source.saved_revision_optional() >= source.history().size())) invalid("invalid captured source metadata");
    if (scalar_wire(source, scalar_request(scalar)).dump() != scalar.dump()) invalid("complete captured source changed");
}
void authenticate_request(const DocumentSnapshot& source, const Json& wire) {
    if (wire.at("version") == 1) { authenticate(source, wire.at("scalar_request")); return; }
    if (!source.is_editable() || source.history().empty() || source.revision() >= source.history().size() ||
        source.history().at(source.revision()).revision != source.revision() ||
        (source.saved_revision_optional() && *source.saved_revision_optional() >= source.history().size())) invalid("invalid captured source metadata");
    const auto expected = source_fields(source);
    for (const auto* fence : source_fences) if (wire.at(fence).dump() != expected.at(fence).dump()) invalid("complete captured source changed");
    if (!wire.at("scalar_request").is_null()) authenticate(source, wire.at("scalar_request"));
}
Json allocation_wire(const PhaseSelectionPropertyIdentityAllocation& allocation, bool complete_stair_component_dimensions) {
    if (!complete_stair_component_dimensions && !allocation.dimension_identities.empty())
        invalid("dimension identities require explicit stair component dimension authority");
    Json result{{"lane", lane_name(allocation.lane)}, {"registry_id", allocation.registry_id}, {"alternative_id", allocation.alternative_id},
        {"identities", Json::array()}, {"child_identities", Json::array()}, {"hosted_instance_identities", Json::array()}, {"overlay_identities", Json::array()}};
    for (const auto& [old, proposed] : allocation.identities) result["identities"].push_back({{"original_id", old}, {"proposed_id", proposed}});
    for (const auto& [key, proposed] : allocation.child_identities) result["child_identities"].push_back({{"owner_id", key.first}, {"child_id", key.second}, {"proposed_id", proposed}});
    for (const auto& [key, proposed] : allocation.hosted_instance_identities) result["hosted_instance_identities"].push_back({{"catalog_id", key.first}, {"instance_id", key.second}, {"proposed_id", proposed}});
    for (const auto& [key, proposed] : allocation.overlay_identities) result["overlay_identities"].push_back({{"view_id", std::get<0>(key)}, {"owner_id", std::get<1>(key)}, {"overlay_id", std::get<2>(key)}, {"proposed_id", proposed}});
    if (complete_stair_component_dimensions) result["dimension_identities"] = allocation.dimension_identities;
    return result;
}
PhaseSelectionPropertyIdentityAllocation allocation(const Json& wire, bool complete_wall_hosted_instances,
    bool complete_stair_component_dimensions) {
    if (complete_stair_component_dimensions)
        fields(wire, {"lane", "registry_id", "alternative_id", "identities", "child_identities", "hosted_instance_identities", "overlay_identities", "dimension_identities"});
    else fields(wire, {"lane", "registry_id", "alternative_id", "identities", "child_identities", "hosted_instance_identities", "overlay_identities"});
    PhaseSelectionPropertyIdentityAllocation result{lane(wire.at("lane")), text(wire.at("registry_id")), text(wire.at("alternative_id")), {}, {}, {}, {}, {}};
    std::size_t count{};
    for (const auto* key : {"identities", "child_identities", "hosted_instance_identities", "overlay_identities"}) {
        if (!wire.at(key).is_array() || wire.at(key).size() > 4096 || (count += wire.at(key).size()) > 4096) invalid("allocation inventory budget exceeded");
    }
    if (complete_stair_component_dimensions) {
        const auto& dimensions = wire.at("dimension_identities");
        if (!dimensions.is_object() || dimensions.size() > 1000 || dimensions.size() > 4096 - count)
            invalid("combined allocation/dimension inventory budget exceeded");
        for (const auto& [original, proposed] : dimensions.items()) {
            const auto source = text(Json(original));
            const auto destination = fresh_text(proposed);
            if (source == destination) invalid("dimension destination must be fresh");
            result.dimension_identities.emplace(source, destination);
        }
        if (result.lane != PhaseSelectionPropertyLane::stairs && !result.dimension_identities.empty())
            invalid("component dimension allocations are stair-only");
    }
    for (const auto& row : wire.at("identities")) {
        fields(row, {"original_id", "proposed_id"});
        if (!result.identities.emplace(text(row.at("original_id")), fresh_text(row.at("proposed_id"))).second) invalid("duplicate original identity");
    }
    for (const auto& row : wire.at("child_identities")) {
        fields(row, {"owner_id", "child_id", "proposed_id"});
        if (!result.child_identities.emplace(std::pair{text(row.at("owner_id")), text(row.at("child_id"))}, fresh_text(row.at("proposed_id"))).second) invalid("duplicate child identity");
    }
    for (const auto& row : wire.at("hosted_instance_identities")) {
        fields(row, {"catalog_id", "instance_id", "proposed_id"});
        if (!result.hosted_instance_identities.emplace(std::pair{text(row.at("catalog_id")), text(row.at("instance_id"))}, fresh_text(row.at("proposed_id"))).second) invalid("duplicate hosted identity");
    }
    for (const auto& row : wire.at("overlay_identities")) {
        fields(row, {"view_id", "owner_id", "overlay_id", "proposed_id"});
        if (!result.overlay_identities.emplace(std::tuple{text(row.at("view_id")), text(row.at("owner_id")), text(row.at("overlay_id"))}, fresh_text(row.at("proposed_id"))).second) invalid("duplicate overlay identity");
    }
    if (result.lane != PhaseSelectionPropertyLane::stairs && (!result.child_identities.empty() || !result.overlay_identities.empty())) invalid("qualified child/overlay allocations are stair-only");
    if (result.lane == PhaseSelectionPropertyLane::walls && !complete_wall_hosted_instances && !result.hosted_instance_identities.empty()) invalid("historical wall allocations cannot borrow hosted instance slots");
    if (allocation_wire(result, complete_stair_component_dimensions).dump() != wire.dump()) invalid("allocation must be canonical ascending inventory");
    return result;
}
Json decisions_wire(const std::vector<PhaseStairReplacementDependencyDisposition>& decisions) {
    Json rows = Json::array();
    std::string prior;
    if (decisions.size() > 128) invalid("dependency decision budget exceeded");
    for (const auto& decision : decisions) {
        if (decision.rail_id <= prior) invalid("dependency decisions must be ascending and unique");
        prior = decision.rail_id;
        if (decision.action == PhaseStairReplacementDependencyAction::retire) {
            if (!decision.target_key.empty()) invalid("retirement cannot supply a rehost target");
            rows.push_back({{"rail_id", decision.rail_id}, {"action", "retire"}});
        } else if (decision.action == PhaseStairReplacementDependencyAction::rehost)
            rows.push_back({{"rail_id", decision.rail_id}, {"action", "rehost"}, {"target_key", decision.target_key}});
        else invalid("unsupported dependency disposition");
    }
    return rows;
}
std::vector<PhaseStairReplacementDependencyDisposition> decisions(const Json& rows) {
    if (!rows.is_array() || rows.size() > 128) invalid("dependency decision budget exceeded");
    std::vector<PhaseStairReplacementDependencyDisposition> result;
    for (const auto& row : rows) {
        if (!row.is_object() || !row.contains("action")) invalid("invalid dependency decision");
        PhaseStairReplacementDependencyDisposition decision;
        if (row.at("action") == "retire") { fields(row, {"rail_id", "action"}); decision.action = PhaseStairReplacementDependencyAction::retire; }
        else if (row.at("action") == "rehost") { fields(row, {"rail_id", "action", "target_key"}); decision.target_key = text(row.at("target_key"), 4096); }
        else invalid("unsupported dependency disposition");
        decision.rail_id = text(row.at("rail_id")); result.push_back(std::move(decision));
    }
    if (decisions_wire(result).dump() != rows.dump()) invalid("noncanonical dependency decisions");
    return result;
}
PhaseSelectionPropertyEditRequest decode(const Json& wire) {
    bounds(wire);
    if (!wire.is_object() || !wire.contains("version") || !wire.at("version").is_number_integer()) invalid("unsupported request version");
    const bool slab_carry = wire.at("version") == 4;
    const bool vertical = wire.at("version") == 3 || slab_carry;
    const bool wall_hosted_dialect = wire.at("version") == 5;
    const bool complete_stair_component_dimensions = wire.at("version") == 6;
    const bool complete_copy_dialect = wall_hosted_dialect || complete_stair_component_dimensions;
    const bool composite = wire.at("version") == 2 || vertical || complete_copy_dialect;
    if (complete_stair_component_dimensions) fields(wire, {"version", "expected_revision", "source_snapshot_digest", "source_authoring_digest", "source_entities_digest",
        "source_saved_revision", "scalar_request", "allocations", "stair_dependency_dispositions", "message", "skylight_properties", "roof_opening_members", "complete_wall_hosted_instances", "complete_stair_component_dimensions"});
    else if (wall_hosted_dialect) fields(wire, {"version", "expected_revision", "source_snapshot_digest", "source_authoring_digest", "source_entities_digest",
        "source_saved_revision", "scalar_request", "allocations", "stair_dependency_dispositions", "message", "skylight_properties", "roof_opening_members", "complete_wall_hosted_instances"});
    else if (composite) fields(wire, {"version", "expected_revision", "source_snapshot_digest", "source_authoring_digest", "source_entities_digest",
        "source_saved_revision", "scalar_request", "allocations", "stair_dependency_dispositions", "message", "skylight_properties", "roof_opening_members"});
    else {
        fields(wire, {"version", "scalar_request", "allocations", "stair_dependency_dispositions", "message"});
        if (wire.at("version") != 1) invalid("unsupported request version");
    }
    PhaseSelectionPropertyEditRequest result;
    if (complete_copy_dialect) {
        if (!wire.at("complete_wall_hosted_instances").is_boolean())
            invalid("wall-hosted permission requires an explicit Boolean");
        if (wall_hosted_dialect && !wire.at("complete_wall_hosted_instances").get<bool>())
            invalid("complete wall-hosted copies require explicit true authority");
        result.complete_wall_hosted_instances = wire.at("complete_wall_hosted_instances").get<bool>();
    }
    if (complete_stair_component_dimensions) {
        if (!wire.at("complete_stair_component_dimensions").is_boolean() || !wire.at("complete_stair_component_dimensions").get<bool>())
            invalid("complete stair component dimension copies require explicit true authority");
        result.complete_stair_component_dimensions = true;
    }
    if (!wire.at("scalar_request").is_null()) result.properties = scalar_request(wire.at("scalar_request"));
    else if (!composite) invalid("historical request requires scalar authority");
    // V5/V6 add explicit copy permissions. Scalar elevation/carry authority is
    // still determined by the independently validated scalar dialect, never
    // by an ordering comparison against the enclosing request version.
    if (!complete_copy_dialect && (vertical ? (wire.at("scalar_request").is_null() || wire.at("scalar_request").at("version") != (slab_carry ? 5 : 4)) :
        (!wire.at("scalar_request").is_null() && (wire.at("scalar_request").at("version") == 4 || wire.at("scalar_request").at("version") == 5))))
        invalid("wall elevation or hosted slab carry requires its explicit phase and scalar request dialects");
    if (composite) {
        const auto revision = [&](const Json& value) {
            if (!value.is_number_integer() || (value.is_number_unsigned() ? false : value.get<std::int64_t>() < 0)) invalid("invalid source revision");
        };
        revision(wire.at("expected_revision"));
        if (!wire.at("source_saved_revision").is_null()) revision(wire.at("source_saved_revision"));
        for (const auto* key : {"source_snapshot_digest", "source_authoring_digest", "source_entities_digest"}) {
            const auto digest = text(wire.at(key));
            if (digest.size() != 64 || !std::all_of(digest.begin(), digest.end(), [](unsigned char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); })) invalid("invalid source digest");
        }
        if (!wire.at("scalar_request").is_null()) for (const auto* fence : source_fences)
            if (wire.at(fence).dump() != wire.at("scalar_request").at(fence).dump()) invalid("scalar and composite source fences differ");
        if ((vertical || complete_copy_dialect) && wire.at("skylight_properties").is_null()) {
            if (!wire.at("roof_opening_members").is_array() || !wire.at("roof_opening_members").empty())
                invalid("absent skylight contribution requires an exact empty child roster");
        } else result.roof_opening_properties = skylight_request(wire);
        if (complete_copy_dialect && wire.at("scalar_request").is_null() && !result.roof_opening_properties)
            invalid("empty physical roster requires child-only authority");
        if (result.properties.selected.size() + (result.roof_opening_properties ? result.roof_opening_properties->members.size() : 0) > 1000) invalid("combined physical selection budget exceeded");
    }
    if (!wire.at("allocations").is_array() || wire.at("allocations").size() > 1000) invalid("allocation cohort budget exceeded");
    std::optional<Key> previous;
    Ids fresh;
    for (const auto& row : wire.at("allocations")) {
        auto item = allocation(row, result.complete_wall_hosted_instances, complete_stair_component_dimensions); const Key key{item.lane, item.registry_id, item.alternative_id};
        if (previous && key <= *previous) invalid("allocation cohorts must be ascending and unique"); previous = key;
        const auto reserve = [&](const auto& mapping) { for (const auto& [old, proposed] : mapping) { (void)old; if (!fresh.insert(proposed).second || fresh.size() > 4096) invalid("duplicate or excessive fresh identity"); } };
        reserve(item.identities); reserve(item.child_identities); reserve(item.hosted_instance_identities); reserve(item.overlay_identities);
        reserve(item.dimension_identities);
        result.allocations.push_back(std::move(item));
    }
    result.stair_dependency_dispositions = decisions(wire.at("stair_dependency_dispositions"));
    if (!wire.at("message").is_string() || wire.at("message").get_ref<const std::string&>().size() > 4096) invalid("invalid message budget");
    result.message = wire.at("message").get<std::string>();
    return result;
}
template<class Intent, class Id> std::vector<Intent> selected_intents(const std::vector<Intent>& inputs, const Ids& ids, Id id) {
    std::vector<Intent> result; for (const auto& input : inputs) if (ids.contains(id(input))) result.push_back(input); return result;
}
std::vector<Entity> editors(const Entities& entities, const Ids& ids) {
    std::vector<Entity> result; for (const auto& id : ids) result.push_back(entities.at(id)); return result;
}
Ids changed_stairs(const SelectionPropertyEditRequest& request, const Entities& actual, const Entities& edited) {
    Ids result; for (const auto& profile : request.stair_profiles) if (!exact(actual.at(profile.object_id), edited.at(profile.object_id))) result.insert(profile.object_id); return result;
}
std::vector<PhaseStairReplacementDependencyDisposition> cohort_decisions(
    const PhaseStairReplacementDependencyPlan& plan, const std::vector<PhaseStairReplacementDependencyDisposition>& all) {
    Ids ids; for (const auto& dependency : plan.dependencies) ids.insert(dependency.rail_id);
    std::vector<PhaseStairReplacementDependencyDisposition> result;
    for (const auto& decision : all) if (ids.contains(decision.rail_id)) result.push_back(decision);
    return result;
}
void merge_mapping(std::map<std::string, std::string, std::less<>>& output, const auto& input, const Entities& actual) {
    for (const auto& [original, proposed] : input) {
        // Shared catalogs may legitimately have a different derived copy in
        // each family. Their complete mappings stay qualified by cohort.
        const auto source = actual.find(original);
        if (source != actual.end() && source->second.type == "assembly_model") continue;
        const auto [found, added] = output.emplace(original, proposed);
        if (!added && found->second != proposed) invalid("replacement mappings overlap physical owner or child authority");
    }
}
template<class Mapping, class Roster> void exact_roster(const Mapping& mapping, const Roster& roster) {
    using T = typename Mapping::key_type;
    const std::set<T> expected(roster.begin(), roster.end());
    if (mapping.size() != expected.size()) invalid("allocation must exactly match independently discovered identity inventory");
    for (const auto& [key, proposed] : mapping) { (void)proposed; if (!expected.contains(key)) invalid("allocation includes an undiscovered identity slot"); }
}
void validate_inventory(const PhaseSelectionPropertyReplacementInventory& inventory,
    const PhaseSelectionPropertyIdentityAllocation& allocation) {
    std::visit([&](const auto& plan) {
        if (!plan.ready()) invalid("replacement plan has unresolved actual-source dependencies");
        using Plan = std::decay_t<decltype(plan)>;
        auto required = plan.required_entity_ids;
        if constexpr (!std::is_same_v<Plan, PhaseStairReplacementPlan>) required.insert(required.end(), plan.required_child_ids.begin(), plan.required_child_ids.end());
        exact_roster(allocation.identities, required);
        if constexpr (std::is_same_v<Plan, PhaseStairReplacementPlan>) {
            exact_roster(allocation.child_identities, plan.required_child_ids);
            exact_roster(allocation.overlay_identities, plan.required_overlay_ids);
            exact_roster(allocation.dimension_identities, plan.required_dimension_ids);
        }
        exact_roster(allocation.hosted_instance_identities, plan.required_hosted_instance_ids);
    }, inventory.plan);
}
} // namespace

PhaseSelectionPropertyEditInspection::PhaseSelectionPropertyEditInspection(Json scalar, Entities edited,
    std::vector<PhaseSelectionPropertyReplacementInventory> replacements,
    std::vector<PhaseStairReplacementDependencyPlan> dependencies, std::vector<std::string> invariant)
    : scalar_(std::move(scalar)), editors_(std::move(edited)), replacements_(std::move(replacements)), dependencies_(std::move(dependencies)), invariant_(std::move(invariant)) {}
const Json& PhaseSelectionPropertyEditInspection::scalar_request() const noexcept { return scalar_; }
const Entities& PhaseSelectionPropertyEditInspection::selected_editor_entities() const noexcept { return editors_; }
const std::vector<PhaseSelectionPropertyReplacementInventory>& PhaseSelectionPropertyEditInspection::replacements() const noexcept { return replacements_; }
const std::vector<PhaseStairReplacementDependencyPlan>& PhaseSelectionPropertyEditInspection::stair_dependency_plans() const noexcept { return dependencies_; }
const std::vector<std::string>& PhaseSelectionPropertyEditInspection::invariant_selected_ids() const noexcept { return invariant_; }

PhaseSelectionPropertyEditInspection inspect_phase_selection_property_edit(const DocumentSnapshot& source, const SelectionPropertyEditRequest& request,
    const std::vector<PhaseStairReplacementDependencyDisposition>& reviewed_stair_dispositions,
    const std::optional<RoofOpeningGroupProperties>& roof_opening_properties, bool complete_wall_hosted_instances,
    bool complete_stair_component_dimensions) {
    (void)decisions(decisions_wire(reviewed_stair_dispositions));
    if (request.selected.empty() && (!roof_opening_properties || !request.walls.empty() || !request.slabs.empty() ||
        !request.hosted_openings.empty() || !request.roofs.empty() || !request.structural_profiles.empty() || !request.stair_profiles.empty()))
        invalid("empty physical roster requires child-only authority");
    const Json wire = request.selected.empty() ? Json(nullptr) : scalar_wire(source, request);
    if (!wire.is_null()) authenticate(source, wire);
    else {
        auto captured = source_fields(source); captured["version"] = 2; captured["scalar_request"] = nullptr;
        authenticate_request(source, captured);
    }
    if (roof_opening_properties) {
        Json children{{"skylight_properties", skylight_wire(*roof_opening_properties)}, {"roof_opening_members", Json::array()}};
        for (const auto& member : roof_opening_properties->members) children["roof_opening_members"].push_back({{"roof_id", member.roof_id}, {"opening_id", member.opening_id}});
        (void)skylight_request(children);
        if (request.selected.size() + roof_opening_properties->members.size() > 1000) invalid("combined physical selection budget exceeded");
    }
    const auto& actual = source.entities(); const auto scope = constraint_phase_scope(actual);
    std::optional<PreparedRoofOpeningGroupProperties> composite_roofs;
    if (roof_opening_properties) composite_roofs = composite_roof_properties(actual, *roof_opening_properties, request.roofs);
    if (roof_opening_properties) for (const auto& member : roof_opening_properties->members)
        if (scope.inactive_owner_ids.contains(member.roof_id) || scope.inactive_owner_ids.contains(member.opening_id)) invalid("selected child or host is inactive in the saved design");
    Entities edited;
    for (const auto& target : request.selected) {
        const auto found = actual.find(target.entity_id);
        if (found == actual.end() || found->first != found->second.id || found->second.type != entity_type(target.family)) invalid("selected family differs from its actual source owner");
        if (scope.inactive_owner_ids.contains(target.entity_id)) invalid("selected owner is inactive in the saved design");
        edited.emplace(found->first, found->second);
    }
    for (const auto& profile : request.walls) edited.at(profile.wall_id) = replay_wall_profile_entity(actual.at(profile.wall_id), profile);
    const bool slab_carry = std::any_of(request.slabs.begin(), request.slabs.end(), [](const auto& slab) { return slab.coordinate_hosted_elevation; });
    if (slab_carry) {
        // Inspect the complete original-source consequence, including actual
        // hosted placement rows. Only selected slab bodies enter this editor
        // inventory; the full producer independently owns catalog changes.
        const auto slab_candidate = replay_slab_profile_entities(actual, request.slabs);
        for (const auto& profile : request.slabs) edited.at(profile.slab_id) = slab_candidate.at(profile.slab_id);
    } else for (const auto& profile : request.slabs)
        edited.at(profile.slab_id) = replay_slab_profile_entity(actual.at(profile.slab_id), profile);
    for (const auto& profile : request.hosted_openings) edited.at(profile.opening_id) = replay_hosted_opening_profile_entity(actual.at(profile.opening_id), profile);
    Ids invariant_roof_bodies;
    for (const auto& profile : request.roofs) {
        const auto body = stage_roof_profile_entity(actual.at(profile.roof_id), profile);
        if (exact(actual.at(profile.roof_id), body)) invariant_roof_bodies.insert(profile.roof_id);
        edited.at(profile.roof_id) = composite_roofs ? composite_roofs->candidate_entities.at(profile.roof_id) : body;
    }
    for (const auto& profile : request.structural_profiles) edited.at(profile.object_id) = prepare_structural_profile_edit_entity(actual.at(profile.object_id), profile);
    for (const auto& entity : prepare_stair_profile_edit_entities(actual, request.stair_profiles)) edited.at(entity.id) = entity;
    std::map<std::string, std::string, std::less<>> memberships;
    std::map<std::string, ModelPhases, std::less<>> registries;
    for (const auto& [registry, entity] : actual) if (entity.type == "model_phases") {
        const auto model = ModelPhases::from_json(entity.properties.at("model"));
        for (const auto& id : model.entity_ids()) {
            const auto owner = actual.find(id);
            if (owner == actual.end() || owner->second.id != id || !is_model_phase_entity_type(owner->second.type) ||
                !memberships.emplace(id, registry).second) invalid("saved phase membership is missing, invalid or overlapping");
        }
        registries.emplace(registry, model);
    }
    const auto baseline_key = [&](const std::string& id, PhaseSelectionPropertyLane kind) -> std::optional<Key> {
        const auto found = memberships.find(id); if (found == memberships.end()) return {};
        const auto& registry = found->second; const auto& model = registries.at(registry);
        if (!model.active_alternative() || std::find(model.baseline_ids().begin(), model.baseline_ids().end(), id) == model.baseline_ids().end()) return {};
        return Key{kind, registry, *model.active_alternative()};
    };
    struct Group { Ids selected, seeds, roof_hosts; };
    std::map<Key, Group> groups;
    std::vector<std::string> invariant;
    for (const auto& target : request.selected) {
        if (invariant_roof_bodies.contains(target.entity_id) || exact(actual.at(target.entity_id), edited.at(target.entity_id))) {
            if (request.unchanged_policy != SelectionPropertyUnchangedPolicy::retain) invalid("unchanged selected owner requires retain policy");
            invariant.push_back(target.entity_id); continue;
        }
        const auto kind = family_lane(target.family);
        auto key = baseline_key(target.entity_id, kind);
        std::string seed = target.entity_id;
        if (target.family == SelectionPropertyFamily::hosted_opening) {
            const auto& opening = actual.at(target.entity_id);
            const auto host = opening.properties.at("wall_id").get<std::string>();
            if (!actual.contains(host) || actual.at(host).type != "wall" || scope.inactive_owner_ids.contains(host)) invalid("opening requires an actual active wall host");
            const auto host_key = baseline_key(host, kind);
            if (key && (!host_key || *host_key != *key)) invalid("baseline opening and host require the same saved phase authority");
            if (host_key) key = host_key;
            seed = host;
        }
        if (key) { groups[*key].selected.insert(target.entity_id); groups[*key].seeds.insert(seed); }
    }
    // Qualified child selections derive host phase authority from actual saved
    // membership. Hosts remain internal producers, never physical selections.
    if (composite_roofs) for (const auto& edit : composite_roofs->intents) {
        if (scope.inactive_owner_ids.contains(edit.roof_id)) invalid("selected child host is inactive in the saved design");
        if (exact(actual.at(edit.roof_id), composite_roofs->candidate_entities.at(edit.roof_id))) continue;
        if (const auto key = baseline_key(edit.roof_id, PhaseSelectionPropertyLane::roofs)) {
            groups[*key].seeds.insert(edit.roof_id); groups[*key].roof_hosts.insert(edit.roof_id);
        }
    }
    // Coordinate ordinary members with their selected replacement family when
    // its existing producer admits that role. This is essential for selected
    // rails/openings fitting a concurrently edited baseline host. Foreign
    // ordinary registries retain their separate original-source contribution.
    for (const auto& target : request.selected) {
        if (invariant_roof_bodies.contains(target.entity_id) || exact(actual.at(target.entity_id), edited.at(target.entity_id))) continue;
        const auto kind = family_lane(target.family);
        if (kind == PhaseSelectionPropertyLane::slabs || std::any_of(groups.begin(), groups.end(),
            [&](const auto& item) { return item.second.selected.contains(target.entity_id); })) continue;
        const auto membership = memberships.find(target.entity_id);
        for (auto& [key, group] : groups) if (std::get<0>(key) == kind &&
            (kind == PhaseSelectionPropertyLane::walls || membership == memberships.end() || membership->second == std::get<1>(key))) {
            group.selected.insert(target.entity_id); break;
        }
    }
    if (composite_roofs) for (const auto& edit : composite_roofs->intents) {
        if (exact(actual.at(edit.roof_id), composite_roofs->candidate_entities.at(edit.roof_id))) continue;
        if (std::any_of(groups.begin(), groups.end(), [&](const auto& item) { return item.second.roof_hosts.contains(edit.roof_id); })) continue;
        const auto membership = memberships.find(edit.roof_id);
        for (auto& [key, group] : groups) if (std::get<0>(key) == PhaseSelectionPropertyLane::roofs &&
            (membership == memberships.end() || membership->second == std::get<1>(key))) {
            group.roof_hosts.insert(edit.roof_id); break;
        }
    }
    std::vector<PhaseSelectionPropertyReplacementInventory> replacements;
    std::vector<PhaseStairReplacementDependencyPlan> dependencies;
    Ids phase_stairs;
    for (const auto& [key, group] : groups) {
        const auto& [kind, registry, alternative] = key;
        const std::vector<std::string> seeds(group.seeds.begin(), group.seeds.end());
        PhaseSelectionPropertyPlan plan;
        switch (kind) {
        case PhaseSelectionPropertyLane::walls: plan = inspect_phase_wall_replacement_plan(actual, seeds, registry, alternative, true, true, complete_wall_hosted_instances); break;
        case PhaseSelectionPropertyLane::slabs: plan = inspect_phase_slab_replacement_plan(actual, seeds, registry, alternative); break;
        case PhaseSelectionPropertyLane::roofs: plan = inspect_phase_roof_replacement_plan(actual, seeds, registry, alternative, true, true); break;
        case PhaseSelectionPropertyLane::structural: plan = inspect_phase_structural_replacement_plan(actual, seeds, registry, alternative, true); break;
        case PhaseSelectionPropertyLane::stairs: {
            const auto rows = editors(edited, group.selected);
            const auto dependency = inspect_phase_stair_replacement_dependencies(actual, rows, true);
            dependencies.push_back(dependency);
            auto intents = selected_intents(request.stair_profiles, group.selected, [](const auto& p) { return p.object_id; });
            const auto choices = cohort_decisions(dependency, reviewed_stair_dispositions);
            if (!choices.empty() || dependency.dependencies.empty()) {
                validate_phase_stair_dependency_dispositions(actual, rows, choices, true);
                PhaseStairReplacementAuthoring authoring; authoring.registry_id = registry; authoring.alternative_id = alternative;
                authoring.complete_scalar_dependencies = true;
                authoring.complete_component_dimensions = complete_stair_component_dimensions;
                authoring.compound_edits = capture_phase_stair_replacement_compound_edits(actual, rows, choices, true);
                authoring.dependency_dispositions = choices;
                for (const auto& choice : choices) authoring.dependency_rail_ids.push_back(choice.rail_id);
                plan = inspect_phase_stair_replacement_plan(actual, authoring);
            } else {
                std::vector<StairObjectEditIntent> profiles;
                for (const auto& p : intents) profiles.push_back(stair_object_edit_from_profile(actual.at(p.object_id), p));
                if (complete_stair_component_dimensions) {
                    PhaseStairReplacementAuthoring authoring; authoring.registry_id = registry; authoring.alternative_id = alternative;
                    authoring.edits = std::move(profiles);
                    authoring.complete_scalar_dependencies = true;
                    authoring.complete_component_dimensions = true;
                    plan = inspect_phase_stair_replacement_plan(actual, authoring);
                } else plan = inspect_phase_stair_replacement_plan(actual, profiles, registry, alternative);
            }
            phase_stairs.insert(group.selected.begin(), group.selected.end()); break;
        }
        }
        std::vector<RoofEditIntent> roof_edits;
        if (composite_roofs && kind == PhaseSelectionPropertyLane::roofs) {
            const auto& roof_plan = std::get<PhaseRoofReplacementPlan>(plan);
            const Ids closure(roof_plan.seed_roof_ids.begin(), roof_plan.seed_roof_ids.end());
            for (const auto& edit : composite_roofs->intents) if ((group.roof_hosts.contains(edit.roof_id) || closure.contains(edit.roof_id)) &&
                !exact(actual.at(edit.roof_id), composite_roofs->candidate_entities.at(edit.roof_id))) roof_edits.push_back(edit);
        }
        replacements.push_back({kind, registry, alternative, {group.selected.begin(), group.selected.end()}, std::move(plan), std::move(roof_edits)});
    }
    auto ordinary_stairs = changed_stairs(request, actual, edited);
    for (const auto& id : phase_stairs) ordinary_stairs.erase(id);
    if (!ordinary_stairs.empty()) dependencies.push_back(inspect_ordinary_stair_edit_dependencies(actual, editors(edited, ordinary_stairs)));
    return {wire, std::move(edited), std::move(replacements), std::move(dependencies), std::move(invariant)};
}

Json encode_phase_selection_property_edit_request(const DocumentSnapshot& source, const PhaseSelectionPropertyEditRequest& request) {
    if (request.allocations.size() > 1000) invalid("allocation cohort budget exceeded");
    if (request.properties.selected.empty() && (!request.roof_opening_properties || !request.properties.walls.empty() || !request.properties.slabs.empty() ||
        !request.properties.hosted_openings.empty() || !request.properties.roofs.empty() || !request.properties.structural_profiles.empty() || !request.properties.stair_profiles.empty())) invalid("empty physical roster requires child-only authority");
    const bool vertical = std::any_of(request.properties.walls.begin(), request.properties.walls.end(), [](const auto& wall) { return wall.elevation.has_value(); });
    const bool slab_carry = std::any_of(request.properties.slabs.begin(), request.properties.slabs.end(), [](const auto& slab) { return slab.coordinate_hosted_elevation; });
    Json wire{{"version", request.complete_stair_component_dimensions ? 6 : request.complete_wall_hosted_instances ? 5 : slab_carry ? 4 : vertical ? 3 : request.roof_opening_properties ? 2 : 1}, {"scalar_request", request.properties.selected.empty() ? Json(nullptr) : scalar_wire(source, request.properties)}, {"allocations", Json::array()},
        {"stair_dependency_dispositions", decisions_wire(request.stair_dependency_dispositions)}, {"message", request.message}};
    if (request.complete_stair_component_dimensions) {
        wire["complete_stair_component_dimensions"] = true;
        wire["complete_wall_hosted_instances"] = request.complete_wall_hosted_instances;
    } else if (request.complete_wall_hosted_instances) wire["complete_wall_hosted_instances"] = true;
    if (request.complete_stair_component_dimensions || request.complete_wall_hosted_instances || vertical || slab_carry || request.roof_opening_properties) {
        const auto captured = source_fields(source); for (const auto* fence : source_fences) wire[fence] = captured.at(fence);
        wire["skylight_properties"] = request.roof_opening_properties ? skylight_wire(*request.roof_opening_properties) : Json(nullptr); wire["roof_opening_members"] = Json::array();
        if (request.roof_opening_properties) for (const auto& member : request.roof_opening_properties->members) wire["roof_opening_members"].push_back({{"roof_id", member.roof_id}, {"opening_id", member.opening_id}});
    }
    for (const auto& item : request.allocations) wire["allocations"].push_back(allocation_wire(item, request.complete_stair_component_dimensions));
    (void)decode(wire); return wire;
}
Json validate_phase_selection_property_edit_request(const Json& wire) { (void)decode(wire); return wire; }

PhaseSelectionPropertyEditPreparation::PhaseSelectionPropertyEditPreparation(Json request, PhaseSelectionPropertyEditInspection inspection,
    Entities entities, std::vector<Json> authorings, Json ordinary, Json ordinary_stair,
    std::map<std::string, std::string, std::less<>> selected, std::map<std::string, std::string, std::less<>> mapping,
    std::vector<std::string> retired_selected, PhaseStairReplacementChildIdentityMap children, std::vector<PhaseWallReplacementAuthoringPreview> wall_previews,
    std::vector<RoofEditIntent> ordinary_roofs, std::map<std::pair<std::string, std::string>, RoofOpeningGroupMember> roof_opening_mapping)
    : request_(std::move(request)), inspection_(std::move(inspection)), entities_(std::move(entities)), authorings_(std::move(authorings)),
      ordinary_(std::move(ordinary)), ordinary_stair_(std::move(ordinary_stair)), selected_mapping_(std::move(selected)), mapping_(std::move(mapping)),
      allocations_(decode(request_).allocations), retired_selected_(std::move(retired_selected)), children_(std::move(children)), wall_previews_(std::move(wall_previews)),
      ordinary_roofs_(std::move(ordinary_roofs)), roof_opening_mapping_(std::move(roof_opening_mapping)) {}
const Json& PhaseSelectionPropertyEditPreparation::request() const noexcept { return request_; }
const Entities& PhaseSelectionPropertyEditPreparation::entities() const noexcept { return entities_; }
const std::vector<Json>& PhaseSelectionPropertyEditPreparation::phase_authorings() const noexcept { return authorings_; }
const Json& PhaseSelectionPropertyEditPreparation::ordinary_scalar_request() const noexcept { return ordinary_; }
const Json& PhaseSelectionPropertyEditPreparation::ordinary_stair_review_request() const noexcept { return ordinary_stair_; }
const std::vector<RoofEditIntent>& PhaseSelectionPropertyEditPreparation::ordinary_roof_edits() const noexcept { return ordinary_roofs_; }
const std::map<std::pair<std::string, std::string>, RoofOpeningGroupMember>& PhaseSelectionPropertyEditPreparation::selected_roof_opening_original_to_proposed() const noexcept { return roof_opening_mapping_; }
const std::map<std::string, std::string, std::less<>>& PhaseSelectionPropertyEditPreparation::selected_original_to_proposed() const noexcept { return selected_mapping_; }
const std::vector<std::string>& PhaseSelectionPropertyEditPreparation::explicitly_retired_selected_ids() const noexcept { return retired_selected_; }
const std::vector<PhaseSelectionPropertyIdentityAllocation>& PhaseSelectionPropertyEditPreparation::validated_identity_allocations() const noexcept { return allocations_; }
const std::map<std::string, std::string, std::less<>>& PhaseSelectionPropertyEditPreparation::original_to_proposed() const noexcept { return mapping_; }
const PhaseStairReplacementChildIdentityMap& PhaseSelectionPropertyEditPreparation::stair_child_original_to_proposed() const noexcept { return children_; }
const std::vector<PhaseWallReplacementAuthoringPreview>& PhaseSelectionPropertyEditPreparation::wall_previews() const noexcept { return wall_previews_; }
const PhaseSelectionPropertyEditInspection& PhaseSelectionPropertyEditPreparation::inspection() const noexcept { return inspection_; }

PhaseSelectionPropertyEditPreparation prepare_phase_selection_property_edit(const DocumentSnapshot& source, const PhaseSelectionPropertyEditRequest& request) {
    return replay_phase_selection_property_edit_preparation(source, encode_phase_selection_property_edit_request(source, request));
}

PhaseSelectionPropertyEditPreparation replay_phase_selection_property_edit_preparation(const DocumentSnapshot& source, const Json& wire) {
    const auto request = decode(wire); authenticate_request(source, wire);
    auto inspection = inspect_phase_selection_property_edit(source, request.properties, request.stair_dependency_dispositions, request.roof_opening_properties,
        request.complete_wall_hosted_instances, request.complete_stair_component_dimensions);
    const auto& actual = source.entities();
    if (request.allocations.size() != inspection.replacements().size()) invalid("allocation cohorts must exactly cover actual replacement inventory");
    for (std::size_t index = 0; index < request.allocations.size(); ++index) {
        const auto& inventory = inspection.replacements()[index]; const auto& ids = request.allocations[index];
        if (Key{inventory.lane, inventory.registry_id, inventory.alternative_id} != Key{ids.lane, ids.registry_id, ids.alternative_id}) invalid("allocation cohort differs from original-source inventory");
        validate_inventory(inventory, ids);
    }
    Ids fresh;
    for (const auto& item : request.allocations) {
        const auto reserve = [&](const auto& mapping) { for (const auto& [old, proposed] : mapping) { (void)old; if (!fresh.insert(proposed).second) invalid("fresh identity allocations overlap"); } };
        reserve(item.identities); reserve(item.child_identities); reserve(item.hosted_instance_identities); reserve(item.overlay_identities);
        reserve(item.dimension_identities);
    }
    // Reserve every actual retained revision before producers derive native
    // geometry. Document independently owns final lifetime/admission as well.
    validate_retained_authoring_fresh_identities(source.history(), source.history().size(), fresh);
    for (const auto& revision : source.history()) validate_selection_edit_fresh_identity_tokens(revision.entities, fresh);
    std::vector<Json> authorings; std::vector<Entities> ordinary_candidates;
    std::vector<PhaseWallReplacementAuthoringPreview> wall_previews;
    std::map<std::string, std::string, std::less<>> mapping;
    PhaseStairReplacementChildIdentityMap children;
    Ids phase_selected, consumed_decisions, phase_roof_hosts;
    for (std::size_t index = 0; index < inspection.replacements().size(); ++index) {
        const auto& inventory = inspection.replacements()[index]; const auto& ids = request.allocations[index];
        const Ids selected(inventory.changed_selected_ids.begin(), inventory.changed_selected_ids.end());
        phase_selected.insert(selected.begin(), selected.end());
        ConstraintAuthoringIntent semantic; semantic.message = request.message;
        auto intent = make_phase_constraint_authoring_intent(source, semantic);
        switch (inventory.lane) {
        case PhaseSelectionPropertyLane::walls: {
            const auto& plan = std::get<PhaseWallReplacementPlan>(inventory.plan);
            PhaseWallReplacementAuthoring leaf; leaf.registry_id = ids.registry_id; leaf.alternative_id = ids.alternative_id;
            leaf.seed_wall_ids = plan.seed_wall_ids; leaf.identities = ids.identities;
            leaf.complete_presentations = true; leaf.complete_corner_windows = true;
            leaf.complete_hosted_instances = request.complete_wall_hosted_instances;
            leaf.hosted_instance_identities = ids.hosted_instance_identities;
            leaf.wall_profiles = selected_intents(request.properties.walls, selected, [](const auto& p) { return p.wall_id; });
            leaf.opening_profiles = selected_intents(request.properties.hosted_openings, selected, [](const auto& p) { return p.opening_id; });
            // The current combined scalar leaf is root-owned. Its closed codec
            // must admit both lists as one original-source/room-review stage.
            leaf.complete_scalar_profiles = true;
            leaf.complete_vertical_profiles = std::any_of(leaf.wall_profiles.begin(), leaf.wall_profiles.end(), [](const auto& wall) { return wall.elevation.has_value(); });
            intent.wall_replacement = encode_phase_wall_replacement_authoring(leaf);
            auto preview = inspect_phase_wall_replacement_authoring(source, intent);
            merge_mapping(mapping, preview.replacement.original_to_proposed, actual);
            wall_previews.push_back(std::move(preview)); break;
        }
        case PhaseSelectionPropertyLane::slabs: {
            const auto& plan = std::get<PhaseSlabReplacementPlan>(inventory.plan);
            PhaseSlabReplacementAuthoring leaf; leaf.registry_id = ids.registry_id; leaf.alternative_id = ids.alternative_id;
            leaf.seed_slab_ids = plan.seed_slab_ids; leaf.identities = ids.identities; leaf.hosted_instance_identities = ids.hosted_instance_identities;
            leaf.slab_profiles = selected_intents(request.properties.slabs, selected, [](const auto& p) { return p.slab_id; });
            leaf.coordinate_hosted_profiles = std::any_of(leaf.slab_profiles.begin(), leaf.slab_profiles.end(), [](const auto& slab) { return slab.coordinate_hosted_elevation; });
            intent.slab_replacement = encode_phase_slab_replacement_authoring(leaf);
            merge_mapping(mapping, ids.identities, actual); break;
        }
        case PhaseSelectionPropertyLane::roofs: {
            const auto& plan = std::get<PhaseRoofReplacementPlan>(inventory.plan);
            PhaseRoofReplacementAuthoring leaf; leaf.registry_id = ids.registry_id; leaf.alternative_id = ids.alternative_id;
            leaf.seed_roof_ids = plan.seed_roof_ids; leaf.identities = ids.identities; leaf.hosted_instance_identities = ids.hosted_instance_identities;
            leaf.phase_qualified_joins = true; leaf.include_hosted_instances = true;
            const Ids seeds(plan.seed_roof_ids.begin(), plan.seed_roof_ids.end());
            if (request.roof_opening_properties) {
                for (const auto& edit : inventory.source_roof_edits) {
                    if (!phase_roof_hosts.insert(edit.roof_id).second) invalid("combined roof producer authority overlaps phase cohorts");
                    (seeds.contains(edit.roof_id) ? leaf.roof_edits : leaf.ordinary_roof_edits).push_back(edit);
                }
            } else for (const auto& profile : selected_intents(request.properties.roofs, selected, [](const auto& p) { return p.roof_id; })) {
                RoofEditIntent edit; edit.roof_id = profile.roof_id; edit.profile = profile;
                (seeds.contains(profile.roof_id) ? leaf.roof_edits : leaf.ordinary_roof_edits).push_back(std::move(edit));
            }
            intent.roof_replacement = encode_phase_roof_replacement_authoring(leaf);
            merge_mapping(mapping, ids.identities, actual); break;
        }
        case PhaseSelectionPropertyLane::structural: {
            const auto& plan = std::get<PhaseStructuralReplacementPlan>(inventory.plan);
            PhaseStructuralReplacementAuthoring leaf; leaf.registry_id = ids.registry_id; leaf.alternative_id = ids.alternative_id;
            leaf.seed_object_ids = plan.seed_object_ids; leaf.identities = ids.identities; leaf.hosted_instance_identities = ids.hosted_instance_identities;
            leaf.complete_hosted = true; leaf.scalar_profiles = selected_intents(request.properties.structural_profiles, selected, [](const auto& p) { return p.object_id; });
            intent.structural_replacement = encode_phase_structural_replacement_authoring(leaf);
            merge_mapping(mapping, ids.identities, actual); break;
        }
        case PhaseSelectionPropertyLane::stairs: {
            const auto edited = editors(inspection.selected_editor_entities(), selected);
            const auto dependency = inspect_phase_stair_replacement_dependencies(actual, edited, true);
            const auto choices = cohort_decisions(dependency, request.stair_dependency_dispositions);
            validate_phase_stair_dependency_dispositions(actual, edited, choices, true);
            for (const auto& choice : choices) if (!consumed_decisions.insert(choice.rail_id).second) invalid("dependency decisions overlap family cohorts");
            PhaseStairReplacementAuthoring leaf; leaf.registry_id = ids.registry_id; leaf.alternative_id = ids.alternative_id;
            leaf.complete_scalar_dependencies = true;
            leaf.complete_component_dimensions = request.complete_stair_component_dimensions;
            leaf.compound_edits = capture_phase_stair_replacement_compound_edits(actual, edited, choices, true);
            leaf.dependency_dispositions = choices; for (const auto& choice : choices) leaf.dependency_rail_ids.push_back(choice.rail_id);
            const auto plan = inspect_phase_stair_replacement_plan(actual, leaf);
            leaf.preserved_inactive_rail_ids = plan.preserved_inactive_rail_ids;
            leaf.identities = ids.identities; leaf.child_identities = ids.child_identities;
            leaf.hosted_instance_identities = ids.hosted_instance_identities; leaf.overlay_identities = ids.overlay_identities;
            leaf.dimension_identities = ids.dimension_identities;
            intent.stair_replacement = encode_phase_stair_replacement_authoring(leaf);
            merge_mapping(mapping, ids.identities, actual);
            merge_mapping(mapping, ids.dimension_identities, actual);
            for (const auto& [original, proposed] : ids.child_identities) if (!children.emplace(original, proposed).second) invalid("stair child mappings overlap");
            break;
        }
        }
        authorings.push_back(encode_phase_constraint_authoring_intent(intent));
    }
    // The ordinary contribution keeps all selected no-ops and ordinary/proposed
    // physical members. It reads the original snapshot, never a replacement map.
    auto ordinary = request.properties;
    std::erase_if(ordinary.selected, [&](const auto& target) { return phase_selected.contains(target.entity_id); });
    std::erase_if(ordinary.walls, [&](const auto& p) { return phase_selected.contains(p.wall_id); });
    std::erase_if(ordinary.slabs, [&](const auto& p) { return phase_selected.contains(p.slab_id); });
    std::erase_if(ordinary.hosted_openings, [&](const auto& p) { return phase_selected.contains(p.opening_id); });
    std::erase_if(ordinary.roofs, [&](const auto& p) { return phase_selected.contains(p.roof_id); });
    std::erase_if(ordinary.structural_profiles, [&](const auto& p) { return phase_selected.contains(p.object_id); });
    std::erase_if(ordinary.stair_profiles, [&](const auto& p) { return phase_selected.contains(p.object_id); });
    std::vector<RoofEditIntent> ordinary_roofs;
    if (request.roof_opening_properties) {
        const auto combined = composite_roof_properties(actual, *request.roof_opening_properties, request.properties.roofs);
        for (const auto& edit : combined.intents) if (!phase_roof_hosts.contains(edit.roof_id)) ordinary_roofs.push_back(edit);
        if (!ordinary_roofs.empty()) ordinary_candidates.push_back(replay_roof_edit_entities(actual, ordinary_roofs));
        ordinary.roofs.clear();
        std::erase_if(ordinary.selected, [](const auto& target) { return target.family == SelectionPropertyFamily::roof; });
    }
    Json ordinary_wire = nullptr, ordinary_stair_wire = nullptr;
    std::optional<StairPropertyReviewPreparation> ordinary_stair_review;
    if (!ordinary.stair_profiles.empty()) {
        const auto dependency = inspect_stair_property_review_dependencies(source, ordinary.stair_profiles);
        const auto choices = cohort_decisions(dependency, request.stair_dependency_dispositions);
        for (const auto& choice : choices) if (!consumed_decisions.insert(choice.rail_id).second) invalid("dependency decisions overlap phase and ordinary cohorts");
        ordinary_stair_review = prepare_stair_property_review(source, ordinary.stair_profiles, choices, request.message);
        ordinary_stair_wire = ordinary_stair_review->request();
        ordinary.stair_profiles.clear();
        std::erase_if(ordinary.selected, [](const auto& target) {
            return target.family == SelectionPropertyFamily::stair || target.family == SelectionPropertyFamily::railing;
        });
    }
    if (!ordinary.selected.empty()) {
        auto prepared = prepare_selection_property_edit(source, ordinary);
        ordinary_wire = prepared.request();
        ordinary_candidates.push_back(prepared.preview_snapshot().entities());
    }
    if (consumed_decisions.size() != request.stair_dependency_dispositions.size())
        invalid("dependency decisions must exactly cover actual affected rails");
    // The enclosing current composer independently reconstructs every phase
    // child from this same source. Our detached phase maps supply no authority
    // for fresh rows, explicit retirement or pending room decisions.
    auto candidate = compose_phase_property_selection_edit_candidates(source, authorings, ordinary_candidates,
        ordinary_stair_review ? &*ordinary_stair_review : nullptr);
    // Reservations include computed hosted render aliases, not merely the
    // caller's destination list. History remains a separate Document guard.
    for (const auto& [id, entity] : candidate) if (!actual.contains(id)) { (void)entity; fresh.insert(id); }
    const auto original_aliases = embedded_assembly_presentation_ids(actual);
    for (const auto& [binding, alias] : embedded_assembly_presentation_ids(candidate))
        if (!original_aliases.contains(binding)) fresh.insert(alias);
    for (const auto& revision : source.history()) validate_selection_edit_fresh_identity_tokens(revision.entities, fresh);
    std::map<std::string, std::string, std::less<>> selected_mapping;
    std::vector<std::string> retired_selected;
    for (const auto& target : request.properties.selected) {
        const auto mapped = mapping.find(target.entity_id);
        const auto final = mapped == mapping.end() ? target.entity_id : mapped->second;
        if (!candidate.contains(final)) {
            const bool explicitly_retired = target.family == SelectionPropertyFamily::railing && std::any_of(
                request.stair_dependency_dispositions.begin(), request.stair_dependency_dispositions.end(), [&](const auto& choice) {
                    return choice.rail_id == target.entity_id && choice.action == PhaseStairReplacementDependencyAction::retire;
                });
            if (!explicitly_retired) invalid("complete preparation lost a selected physical owner");
            retired_selected.push_back(target.entity_id); continue;
        }
        selected_mapping.emplace(target.entity_id, final);
    }
    std::map<std::pair<std::string, std::string>, RoofOpeningGroupMember> roof_opening_mapping;
    if (request.roof_opening_properties) for (const auto& member : request.roof_opening_properties->members) {
        const auto& source_host = actual.at(member.roof_id);
        if (source_host.type != "roof" || source_host.id != member.roof_id) invalid("selected child has no actual qualified roof host");
        RoofOpeningGroupMember destination = member;
        bool inventoried = false;
        for (std::size_t i = 0; i < inspection.replacements().size(); ++i) {
            const auto& inventory = inspection.replacements()[i];
            if (inventory.lane != PhaseSelectionPropertyLane::roofs) continue;
            const auto& plan = std::get<PhaseRoofReplacementPlan>(inventory.plan);
            const auto& ids = request.allocations[i].identities;
            if (!ids.contains(member.roof_id)) continue;
            if (inventoried || std::find(plan.required_entity_ids.begin(), plan.required_entity_ids.end(), member.roof_id) == plan.required_entity_ids.end() ||
                std::find(plan.required_child_ids.begin(), plan.required_child_ids.end(), member.opening_id) == plan.required_child_ids.end() || !ids.contains(member.opening_id))
                invalid("selected child replacement lacks one actual inventoried host/child pair");
            inventoried = true; destination = {ids.at(member.roof_id), ids.at(member.opening_id)};
        }
        const auto final = candidate.find(destination.roof_id);
        if (final == candidate.end() || final->second.id != destination.roof_id || final->second.type != "roof") invalid("selected child destination lost its actual qualified roof");
        const auto& roster = final->second.properties.at("roof_openings");
        if (std::count_if(roster.begin(), roster.end(), [&](const auto& row) { return row.at("id") == destination.opening_id && row.contains("skylight") && !row.at("skylight").is_null(); }) != 1)
            invalid("complete preparation lost a selected qualified skylight");
        // Query validates the actual qualified original child even when it was
        // retained and cloned only through its host's replacement closure.
        (void)roof_opening_group_member_center_world(actual, member);
        (void)roof_opening_group_member_center_world(candidate, destination);
        if (!roof_opening_mapping.emplace(std::pair{member.roof_id, member.opening_id}, std::move(destination)).second) invalid("duplicate selected child mapping");
    }
    // Every original protected physical owner remains exact. Known additive
    // presentation completion is separately admitted by the family producers.
    const auto scope = constraint_phase_scope(actual);
    for (const auto& [registry, entity] : actual) if (entity.type == "model_phases") {
        (void)registry; const auto model = ModelPhases::from_json(entity.properties.at("model"));
        if (!model.active_alternative()) continue;
        for (const auto& id : model.baseline_ids()) if (actual.at(id).type != "assembly_model" &&
            (!candidate.contains(id) || !exact(actual.at(id), candidate.at(id)))) invalid("preparation changed an original baseline physical owner");
    }
    for (const auto& id : scope.inactive_owner_ids) if (!candidate.contains(id) || !exact(actual.at(id), candidate.at(id))) invalid("preparation changed an inactive physical owner");
    return {wire, std::move(inspection), std::move(candidate), std::move(authorings), std::move(ordinary_wire), std::move(ordinary_stair_wire),
        std::move(selected_mapping), std::move(mapping), std::move(retired_selected), std::move(children), std::move(wall_previews), std::move(ordinary_roofs), std::move(roof_opening_mapping)};
}
void require_phase_selection_property_edit_source_current(const DocumentSnapshot& source, const PhaseSelectionPropertyEditPreparation& prepared) {
    (void)decode(prepared.request()); authenticate_request(source, prepared.request());
}
} // namespace sketch
