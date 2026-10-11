#include "sketch/mixed_selection_property_edit.hpp"

#include "sketch/constraint_entity.hpp"
#include "sketch/constraint_integrity.hpp"
#include "sketch/constraint_phase_scope.hpp"
#include "sketch/document_digest.hpp"
#include "sketch/model_phases.hpp"
#include "sketch/phase_constraint_authoring.hpp"
#include "sketch/physical_wall_room.hpp"
#include "sketch/project_store.hpp"

#include <algorithm>
#include <cmath>
#include <memory>
#include <numbers>
#include <set>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace sketch {
namespace {
using Json = nlohmann::json;
using Entities = std::map<std::string, Entity, std::less<>>;
using Ids = std::set<std::string, std::less<>>;
using MemberKey = std::tuple<std::string, std::string, std::string>;
constexpr std::size_t byte_limit = 4 * 1024 * 1024;
[[noreturn]] void reject(const char* reason) {
    throw std::invalid_argument(std::string("Mixed selection Properties: ") + reason);
}
void fields(const Json& value, std::initializer_list<const char*> names) {
    if (!value.is_object() || value.size() != names.size()) reject("unsupported closed fields");
    for (const auto* name : names) if (!value.contains(name)) reject("missing required field");
}
std::string identity(const Json& value) {
    if (!value.is_string()) reject("identity must be a string");
    const auto& id = value.get_ref<const std::string&>();
    if (id.empty() || id.size() > 128 || !std::all_of(id.begin(), id.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == ':';
    })) reject("invalid qualified identity");
    return id;
}
Revision revision(const Json& value) {
    if (value.is_number_unsigned()) return value.get<Revision>();
    if (value.is_number_integer() && value.get<std::int64_t>() >= 0)
        return static_cast<Revision>(value.get<std::int64_t>());
    reject("invalid revision");
}
void digest(const Json& value) {
    if (!value.is_string()) reject("invalid source digest");
    const auto& text = value.get_ref<const std::string&>();
    if (text.size() != 64 || text.find_first_not_of("0123456789abcdef") != std::string::npos)
        reject("source digest must be canonical SHA-256");
}
struct Budget {
    std::size_t bytes{}, nodes{};
    void add(std::size_t count) {
        if (count > byte_limit - bytes) reject("wire byte budget exceeded");
        bytes += count;
    }
    void quoted(std::string_view value) {
        add(2);
        for (const auto c : value) add(c == '"' || c == '\\' ? 2 : static_cast<unsigned char>(c) < 0x20 ? 6 : 1);
    }
    void visit(const Json& value, std::size_t depth = 0) {
        if (depth > 64 || ++nodes > 100000) reject("JSON complexity budget exceeded");
        if (value.is_discarded() || value.is_binary() ||
            (value.is_number_float() && !std::isfinite(value.get<double>()))) reject("nonportable JSON value");
        if (value.is_object()) {
            add(2 + (value.empty() ? 0 : value.size() - 1));
            for (const auto& [key, child] : value.items()) { quoted(key); add(1); visit(child, depth + 1); }
        } else if (value.is_array()) {
            add(2 + (value.empty() ? 0 : value.size() - 1));
            for (const auto& child : value) visit(child, depth + 1);
        } else if (value.is_string()) quoted(value.get_ref<const std::string&>());
        else add(value.is_number() ? 32 : value.is_boolean() ? 5 : 4);
    }
};
bool exact(const Entity& left, const Entity& right) {
    return left == right && left.properties.dump() == right.properties.dump() &&
        left.extensions.dump() == right.extensions.dump();
}
const char* unit_name(Unit value) {
    switch (value) {
    case Unit::metre: return "m";
    case Unit::millimetre: return "mm";
    case Unit::centimetre: return "cm";
    case Unit::foot: return "ft";
    case Unit::inch: return "in";
    }
    reject("unsupported quantity unit");
}
Unit unit(const Json& value) {
    if (value == "m") return Unit::metre;
    if (value == "mm") return Unit::millimetre;
    if (value == "cm") return Unit::centimetre;
    if (value == "ft") return Unit::foot;
    if (value == "in") return Unit::inch;
    reject("unsupported quantity unit");
}
Json quantity(const RoofOpeningQuantityInput& input) {
    const auto& q = input.quantity;
    if (q.original_expression.size() > 4096) reject("quantity expression budget exceeded");
    const auto parsed = parse_quantity(q.original_expression, input.default_unit);
    if (parsed.metres != q.metres || parsed.exact_metres != q.exact_metres || parsed.entered_unit != q.entered_unit)
        reject("quantity must reproduce its exact entered input");
    return {{"quantity", {{"version", 1}, {"original_expression", q.original_expression},
        {"entered_unit", unit_name(q.entered_unit)},
        {"exact_metres", {{"numerator", q.exact_metres.numerator}, {"denominator", q.exact_metres.denominator}}}}},
        {"default_unit", unit_name(input.default_unit)}};
}
std::optional<RoofOpeningQuantityInput> quantity(const Json& value, bool positive) {
    if (value.is_null()) return std::nullopt;
    fields(value, {"quantity", "default_unit"});
    const auto& raw = value.at("quantity");
    fields(raw, {"version", "original_expression", "entered_unit", "exact_metres"});
    fields(raw.at("exact_metres"), {"numerator", "denominator"});
    if (!raw.at("original_expression").is_string() || raw.at("original_expression").get_ref<const std::string&>().size() > 4096)
        reject("invalid quantity expression");
    RoofOpeningQuantityInput result{decode_constraint_quantity_receipt(raw), unit(value.at("default_unit"))};
    if (!std::isfinite(result.quantity.metres) || (positive ? result.quantity.metres <= 0 : result.quantity.metres < 0) ||
        quantity(result).dump() != value.dump()) reject("quantity requires canonical exact input and admitted sign");
    return result;
}
Json skylight_properties(const RoofOpeningGroupProperties& request) {
    const auto q = [](const auto& value) { return value ? quantity(*value) : Json(nullptr); };
    return {{"version", 1}, {"width", q(request.width)}, {"depth", q(request.depth)},
        {"frame_width", q(request.frame_width)}, {"curb_height", q(request.curb_height)},
        {"glazing_thickness", q(request.glazing_thickness)},
        {"rotation_radians", request.rotation_radians ? Json(*request.rotation_radians) : Json(nullptr)},
        {"retain_invariant_members", request.retain_invariant_members}};
}
RoofOpeningGroupProperties skylights(const Json& value) {
    RoofOpeningGroupProperties result;
    const auto& patch = value.at("skylight_properties");
    if (patch.is_null()) return result;
    fields(patch, {"version", "width", "depth", "frame_width", "curb_height", "glazing_thickness",
        "rotation_radians", "retain_invariant_members"});
    if (!patch.at("version").is_number_integer() || patch.at("version") != 1 ||
        !patch.at("retain_invariant_members").is_boolean()) reject("unsupported skylight Properties codec");
    result.width = quantity(patch.at("width"), true);
    result.depth = quantity(patch.at("depth"), true);
    result.frame_width = quantity(patch.at("frame_width"), true);
    result.curb_height = quantity(patch.at("curb_height"), false);
    result.glazing_thickness = quantity(patch.at("glazing_thickness"), true);
    if (!patch.at("rotation_radians").is_null()) {
        if (!patch.at("rotation_radians").is_number()) reject("skylight angle must be numeric");
        result.rotation_radians = patch.at("rotation_radians").get<double>();
        if (!std::isfinite(*result.rotation_radians) || std::abs(*result.rotation_radians) > std::numbers::pi)
            reject("skylight angle must be canonical in [-pi,pi]");
    }
    result.retain_invariant_members = patch.at("retain_invariant_members").get<bool>();
    if (!result.width && !result.depth && !result.frame_width && !result.curb_height &&
        !result.glazing_thickness && !result.rotation_radians && !result.retain_invariant_members)
        reject("empty skylight Properties patch requires explicit invariant retention");
    if (skylight_properties(result).dump() != patch.dump()) reject("skylight Properties must be canonical");
    for (const auto& member : value.at("roof_opening_members"))
        result.members.push_back({identity(member.at("roof_id")), identity(member.at("opening_id"))});
    return result;
}
Json source_fields(const DocumentSnapshot& source) {
    return {{"expected_revision", source.revision()}, {"source_snapshot_digest", document_snapshot_digest(source)},
        {"source_authoring_digest", document_authoring_source_digest_v2(source)},
        {"source_entities_digest", entity_map_digest(source.entities())},
        {"source_saved_revision", source.saved_revision_optional() ? Json(*source.saved_revision_optional()) : Json(nullptr)}};
}
Json ordinary_request(const DocumentSnapshot& source, const SelectionPropertyEditRequest& request) {
    if (request.selected.empty() || request.selected.size() > 1000 || request.walls.size() > 1000 ||
        request.slabs.size() > 1000 || request.hosted_openings.size() > 1000 || request.roofs.size() > 1000 ||
        request.structural_profiles.size() > 1000 || request.stair_profiles.size() > 1000)
        reject("typed ordinary Properties budget exceeded");
    // Encode without admitting profile-only intermediate roofs. The canonical
    // leaf codec authenticates the complete scalar roster before fused replay.
    auto result = source_fields(source);
    result["version"] = !request.stair_profiles.empty() ? 3 : request.structural_profiles.empty() ? 1 : 2;
    result["selected"] = Json::array();
    for (const auto& target : request.selected) {
        const char* name = nullptr;
        switch (target.family) {
        case SelectionPropertyFamily::wall: name = "wall"; break;
        case SelectionPropertyFamily::slab: name = "slab"; break;
        case SelectionPropertyFamily::hosted_opening: name = "hosted_opening"; break;
        case SelectionPropertyFamily::roof: name = "roof"; break;
        case SelectionPropertyFamily::column: name = "column"; break;
        case SelectionPropertyFamily::beam: name = "beam"; break;
        case SelectionPropertyFamily::stair: name = "stair"; break;
        case SelectionPropertyFamily::railing: name = "railing"; break;
        default: reject("unsupported ordinary family");
        }
        result["selected"].push_back({{"entity_id", target.entity_id}, {"family", name}});
    }
    const auto lane = [&](const char* name, const auto& inputs, auto encoder) {
        result[name] = Json::array();
        for (const auto& input : inputs) result[name].push_back(encoder(input));
    };
    lane("walls", request.walls, encode_wall_profile_edit_intent);
    lane("slabs", request.slabs, encode_slab_profile_edit_intent);
    lane("hosted_openings", request.hosted_openings, encode_hosted_opening_profile_edit_intent);
    lane("roofs", request.roofs, encode_roof_profile_edit_intent);
    if (!request.structural_profiles.empty() || !request.stair_profiles.empty())
        lane("structural_profiles", request.structural_profiles, encode_structural_profile_edit_intent);
    if (!request.stair_profiles.empty())
        lane("stair_profiles", request.stair_profiles, encode_stair_profile_edit_intent);
    if (request.unchanged_policy != SelectionPropertyUnchangedPolicy::retain &&
        request.unchanged_policy != SelectionPropertyUnchangedPolicy::reject) reject("unsupported unchanged policy");
    result["unchanged_policy"] = request.unchanged_policy == SelectionPropertyUnchangedPolicy::retain ? "retain" : "reject";
    return validate_selection_property_edit_request(result);
}
void validate(const Json& value) {
    Budget budget; budget.visit(value);
    const bool reviewed_stair_dialect = value.is_object() && value.contains("version") && value.at("version") == 9;
    if (reviewed_stair_dialect)
        fields(value, {"version", "expected_revision", "source_snapshot_digest", "source_authoring_digest",
            "source_entities_digest", "source_saved_revision", "ordinary_request", "presentation_request",
            "skylight_properties", "roof_opening_members", "room_reviews", "message", "stair_review_request"});
    else
        fields(value, {"version", "expected_revision", "source_snapshot_digest", "source_authoring_digest",
            "source_entities_digest", "source_saved_revision", "ordinary_request", "presentation_request",
            "skylight_properties", "roof_opening_members", "room_reviews", "message"});
    if (!value.at("version").is_number_integer() ||
        (value.at("version") != 5 && value.at("version") != 6 && value.at("version") != 7 && value.at("version") != 8 && !reviewed_stair_dialect))
        reject("unsupported Properties dialect");
    const bool pose_dialect = value.at("version") == 6;
    const bool structural_dialect = value.at("version") == 7;
    const bool stair_dialect = value.at("version") == 8;
    const auto& physical = value.at("ordinary_request");
    if (structural_dialect && (physical.is_null() || physical.value("version", Json()) != 2))
        reject("structural Properties requires its explicit ordinary dialect two");
    if (stair_dialect && (physical.is_null() || physical.value("version", Json()) != 3))
        reject("stair Properties requires its explicit ordinary dialect three");
    const auto& presentation = value.at("presentation_request");
    if (pose_dialect && (presentation.is_null() || presentation.value("version", Json()) != 2))
        reject("pose Properties requires its explicit presentation dialect two");
    (void)revision(value.at("expected_revision"));
    for (const auto* key : {"source_snapshot_digest", "source_authoring_digest", "source_entities_digest"}) digest(value.at(key));
    if (!value.at("source_saved_revision").is_null()) (void)revision(value.at("source_saved_revision"));
    if (!value.at("message").is_string() || value.at("message").get_ref<const std::string&>().size() > 1024 ||
        value.at("message").get_ref<const std::string&>().find('\0') != std::string::npos) reject("invalid message");
    std::set<MemberKey> selected;
    const auto select = [&](MemberKey key) {
        if (!selected.insert(std::move(key)).second || selected.size() > 1000) reject("duplicate or over-budget selected member");
    };
    for (const auto* name : {"ordinary_request", "presentation_request"}) {
        const auto& lane = value.at(name);
        if (lane.is_null()) continue;
        if (std::string_view(name) == "ordinary_request") {
            (void)validate_selection_property_edit_request(lane);
            if (reviewed_stair_dialect ? (lane.at("version") != 1 && lane.at("version") != 2) :
                lane.at("version") != (stair_dialect ? 3 : structural_dialect ? 2 : 1))
                reject("ordinary dialect differs from complete Properties authority");
        }
        else {
            (void)validate_selection_presentation_property_edit_request(lane);
            if (!reviewed_stair_dialect && !structural_dialect && !stair_dialect && lane.at("version") != (pose_dialect ? 2 : 1))
                reject("presentation dialect differs from complete Properties authority");
        }
        for (const auto* fence : {"expected_revision", "source_snapshot_digest", "source_authoring_digest",
                "source_entities_digest", "source_saved_revision"})
            if (lane.at(fence).dump() != value.at(fence).dump()) reject("leaf source fences differ from complete original source");
        for (const auto& row : lane.at("selected")) {
            if (std::string_view(name) == "ordinary_request" || row.at("family") == "reference")
                select({"entity", identity(row.at("entity_id")), ""});
            else select({row.at("family").get<std::string>(), identity(row.at("owner_id")), identity(row.at("child_id"))});
        }
    }
    if (reviewed_stair_dialect) {
        const auto& lane = value.at("stair_review_request");
        const auto request = decode_stair_property_review_request(lane);
        for (const auto* fence : {"expected_revision", "source_snapshot_digest", "source_authoring_digest",
                "source_entities_digest", "source_saved_revision"})
            if (lane.at(fence).dump() != value.at(fence).dump()) reject("stair review source fences differ from complete original source");
        for (const auto& profile : request.profiles) select({"entity", profile.object_id, ""});
    }
    const auto& members = value.at("roof_opening_members");
    if (!members.is_array() || members.size() > 1000 || value.at("skylight_properties").is_null() != members.empty())
        reject("skylight Properties must exactly cover a nonempty actual child roster");
    std::pair<std::string, std::string> previous;
    for (const auto& row : members) {
        fields(row, {"roof_id", "opening_id"});
        const std::pair key{identity(row.at("roof_id")), identity(row.at("opening_id"))};
        if (!previous.first.empty() && !(previous < key)) reject("child roster must be sorted and unique");
        previous = key;
        select({"skylight", key.first, key.second});
    }
    (void)skylights(value);
    if (selected.empty()) reject("Properties requires one to 1000 unique selected members");
    const auto& reviews = value.at("room_reviews");
    if (!reviews.is_array() || reviews.size() > 32) reject("room review group budget exceeded");
    for (const auto& row : reviews) {
        const auto review = decode_physical_wall_room_review_intent(row);
        if (encode_physical_wall_room_review_intent(review).dump() != row.dump()) reject("room review must be canonical");
        if (!review.context_plane_selection || !review.selected_wall_id.empty() ||
            !review.selected_dimension_placements.empty() || review.selected_dimension_source)
            reject("scalar Properties requires context/plane review without dimension movement authority");
    }
    // Strict dump validates UTF-8 only after bounded traversal.
    if (value.dump().size() > byte_limit) reject("wire byte budget exceeded");
}
void authenticate(const DocumentSnapshot& source, const Json& value) {
    if (!source.is_editable() || source.history().empty() || source.history().size() > ProjectStore::maximum_revision_count ||
        source.revision() >= source.history().size() || source.history().at(source.revision()).revision != source.revision() ||
        (source.saved_revision_optional() && *source.saved_revision_optional() >= source.history().size()))
        reject("captured source history, save state or editability is invalid");
    const auto actual = source_fields(source);
    for (const auto& [key, field] : actual.items())
        if (field.dump() != value.at(key).dump()) reject("complete original source changed");
}
Ids retained_baselines(const Entities& entities) {
    Ids result;
    for (const auto& [id, entity] : entities) if (entity.type == "model_phases") {
        (void)id;
        const auto model = ModelPhases::from_json(entity.properties.at("model"));
        if (model.active_alternative()) result.insert(model.baseline_ids().begin(), model.baseline_ids().end());
    }
    return result;
}
void protect_source(const Entities& original, const Entities& candidate, bool before_review,
    const Ids& reviewed_erased = {}) {
    const auto scope = constraint_phase_scope(original);
    const auto baselines = retained_baselines(original);
    for (const auto& [id, entity] : original) {
        const auto found = candidate.find(id);
        if (scope.inactive_owner_ids.contains(id) || baselines.contains(id) ||
            (before_review && (entity.type == "room" || is_physical_wall_room(entity))))
            if (found == candidate.end() || !exact(entity, found->second))
                reject("ordinary Properties changed a protected inactive, baseline or retained room owner");
        if (before_review && (found == candidate.end()) != reviewed_erased.contains(id))
            reject("Properties removal must exactly match independently derived stair review closure");
    }
    for (const auto& id : reviewed_erased)
        if (!original.contains(id) || candidate.contains(id)) reject("stair review closure must remain exactly erased");
    if (before_review) for (const auto& [id, entity] : candidate) {
        (void)entity;
        if (!original.contains(id)) reject("Properties cannot add identities");
    }
}
void require_selected_members_present(const Json& value, const Entities& candidate,
    const Ids& retired_reviewed_rails = {}) {
    const auto owner = [&](const std::string& id) -> const Entity& {
        const auto found = candidate.find(id);
        if (found == candidate.end() || found->second.id != id) reject("reviewed retirement removed a selected owner");
        return found->second;
    };
    const auto& ordinary = value.at("ordinary_request");
    if (!ordinary.is_null()) for (const auto& row : ordinary.at("selected"))
        (void)owner(row.at("entity_id").get<std::string>());
    if (value.at("version") == 9) for (const auto& row : value.at("stair_review_request").at("profiles")) {
        const auto id = row.at("object_id").get<std::string>();
        // Only the independently rederived explicit rail disposition can retire
        // a selected scalar member. Other selected owners/children still survive.
        if (!retired_reviewed_rails.contains(id)) (void)owner(id);
    }
    const auto& presentation = value.at("presentation_request");
    if (!presentation.is_null()) for (const auto& row : presentation.at("selected")) {
        if (row.at("family") == "reference") (void)owner(row.at("entity_id").get<std::string>());
        else {
            const auto& state = owner(row.at("owner_id").get<std::string>()).properties.at("state");
            const auto& members = state.at(row.at("family") == "symbol" ? "symbols" : "labels");
            if (std::count_if(members.begin(), members.end(), [&](const auto& member) {
                    return member.at("id") == row.at("child_id");
                }) != 1) reject("reviewed retirement removed a selected annotation child");
        }
    }
    for (const auto& row : value.at("roof_opening_members")) {
        const auto& members = owner(row.at("roof_id").get<std::string>()).properties.at("roof_openings");
        if (std::count_if(members.begin(), members.end(), [&](const auto& member) {
                return member.at("id") == row.at("opening_id");
            }) != 1) reject("reviewed retirement removed a selected skylight child");
    }
}
std::vector<SelectionGeometryRoomReviewRequirement> requirements(const DocumentSnapshot& source,
    const Entities& candidate, const Ids& changed_hosts) {
    if (changed_hosts.empty()) return {};
    const auto organization = organize_project(source);
    struct Room { std::string id; DrawingContext context; PhysicalWallRoomLineageCheck lineage; };
    std::vector<Room> rooms;
    std::vector<SelectionGeometryRoomReviewRequirement> result;
    for (const auto& id : active_physical_wall_room_ids(source.entities())) {
        const auto& entity = source.entities().at(id);
        const auto context = organization.drawing_context(id);
        if (!context || !context->complete()) reject("physical room has unresolved original drawing context");
        auto lineage = validate_retained_physical_wall_room_lineage(entity, *context);
        if (!exact(entity, candidate.at(id))) reject("Properties stage cannot silently repair a physical room");
        if (std::any_of(lineage.source_owner_ids.begin(), lineage.source_owner_ids.end(),
                [&](const auto& wall) { return changed_hosts.contains(wall); })) {
            const auto found = std::find_if(result.begin(), result.end(), [&](const auto& group) {
                return group.context == *context && std::abs(group.effective_elevation_m - lineage.effective_elevation_m) <=
                    default_geometry_tolerance_metres;
            });
            if (found == result.end()) result.push_back({*context, lineage.effective_elevation_m, {}, {}, {}, {}, {}});
        }
        rooms.push_back({id, *context, std::move(lineage)});
    }
    // Require explicit disposition for every active retained room in an affected
    // context/plane, including its unchanged neighbours. No callout operator is
    // invented for scalar profiles.
    for (const auto& room : rooms) for (auto& group : result)
        if (group.context == room.context && std::abs(group.effective_elevation_m - room.lineage.effective_elevation_m) <=
                default_geometry_tolerance_metres) {
            group.retained_room_ids.push_back(room.id);
            group.source_wall_ids.insert(group.source_wall_ids.end(), room.lineage.source_owner_ids.begin(), room.lineage.source_owner_ids.end());
        }
    for (auto& group : result) {
        std::sort(group.source_wall_ids.begin(), group.source_wall_ids.end());
        group.source_wall_ids.erase(std::unique(group.source_wall_ids.begin(), group.source_wall_ids.end()), group.source_wall_ids.end());
    }
    if (result.size() > 32) reject("affected room planes exceed review budget");
    return result;
}
struct Stage {
    Entities entities;
    std::vector<SelectionGeometryRoomReviewRequirement> reviews;
    Ids reviewed_erased;
    Ids retired_reviewed_rails;
};
Stage derive(const DocumentSnapshot& source, const Json& value) {
    validate(value); authenticate(source, value);
    const auto& original = source.entities();
    if (const auto error = validate_active_phase_constraint_integrity(original)) throw std::invalid_argument(*error);
    const auto scope = constraint_phase_scope(original);
    std::vector<Entities> leaves;
    std::vector<RoofProfileEditIntent> profiles;
    Ids changed_hosts;
    const auto& ordinary = value.at("ordinary_request");
    if (!ordinary.is_null()) {
        for (const auto& row : ordinary.at("selected"))
            if (scope.inactive_owner_ids.contains(identity(row.at("entity_id")))) reject("selected ordinary owner is inactive");
        for (const auto& row : ordinary.at("roofs")) {
            auto profile = decode_roof_profile_edit_intent(row);
            const auto found = original.find(profile.roof_id);
            if (found == original.end() || found->second.id != found->first || found->second.type != "roof")
                reject("roof scalar selection requires its actual original owner");
            // BODY invariance is independent of selected child changes. Every
            // scalar intent remains authenticated; none is silently discarded.
            if (exact(found->second, stage_roof_profile_entity(found->second, profile)) && ordinary.at("unchanged_policy") != "retain")
                reject("unchanged selected roof body requires explicit retain policy");
            profiles.push_back(std::move(profile));
        }
        auto remaining = ordinary;
        remaining["roofs"] = Json::array();
        auto& selected = remaining["selected"];
        selected.erase(std::remove_if(selected.begin(), selected.end(), [](const auto& row) { return row.at("family") == "roof"; }), selected.end());
        if (!selected.empty()) {
            const auto prepared = replay_selection_property_edit_preparation(source, remaining);
            leaves.push_back(prepared.preview_snapshot().entities());
            for (const auto& id : prepared.affected_wall_ids())
                if (!exact(original.at(id), leaves.back().at(id))) changed_hosts.insert(id);
            for (const auto& row : remaining.at("hosted_openings")) {
                const auto opening = row.at("opening_id").get<std::string>();
                if (!exact(original.at(opening), leaves.back().at(opening))) changed_hosts.insert(row.at("wall_id").get<std::string>());
            }
        }
    }
    auto children = skylights(value);
    for (const auto& member : children.members)
        if (scope.inactive_owner_ids.contains(member.roof_id) || scope.inactive_owner_ids.contains(member.opening_id))
            reject("selected skylight or actual host is inactive");
    if (!profiles.empty() || !children.members.empty()) {
        // Keep the complete proof even when invariant members are selected.
        const bool retain = children.retain_invariant_members;
        children.retain_invariant_members = true;
        auto prepared_roofs = prepare_roof_opening_group_properties_composite(original, children, profiles);
        auto roof_candidate = std::move(prepared_roofs.candidate_entities);
        if (!retain) for (const auto& member : children.members) {
            const auto row = [&](const Entities& entities) -> const Json& {
                const auto& roster = entities.at(member.roof_id).properties.at("roof_openings");
                const auto found = std::find_if(roster.begin(), roster.end(), [&](const auto& child) { return child.at("id") == member.opening_id; });
                if (found == roster.end()) reject("actual selected skylight row disappeared");
                return *found;
            };
            if (row(original).dump() == row(roof_candidate).dump()) reject("unchanged selected skylight requires explicit retain policy");
        }
        leaves.push_back(std::move(roof_candidate));
    }
    const auto& presentation = value.at("presentation_request");
    if (!presentation.is_null()) {
        for (const auto& row : presentation.at("selected")) {
            const auto id = row.at("family") == "reference" ? row.at("entity_id").get<std::string>() : row.at("owner_id").get<std::string>();
            if (scope.inactive_owner_ids.contains(id)) reject("selected presentation owner is inactive");
        }
        leaves.push_back(replay_selection_presentation_property_edit_preparation(source, presentation).candidate_entities());
    }
    Ids reviewed_erased;
    Ids retired_reviewed_rails;
    Entities candidate;
    if (value.at("version") == 9) {
        for (const auto& row : value.at("stair_review_request").at("profiles"))
            if (scope.inactive_owner_ids.contains(row.at("object_id").get<std::string>()))
                reject("selected reviewed stair owner is inactive");
        const auto reviewed = replay_stair_property_review_preparation(source, value.at("stair_review_request"));
        reviewed_erased.insert(reviewed.erased_entity_ids().begin(), reviewed.erased_entity_ids().end());
        retired_reviewed_rails.insert(reviewed.retired_rail_ids().begin(), reviewed.retired_rail_ids().end());
        // The sealed compositor separately replays this request and admits only
        // its exact existing-producer closure beside original-source leaves.
        candidate = compose_stair_property_review_selection_edit_candidates(source, reviewed, leaves);
        require_selected_members_present(value, candidate, retired_reviewed_rails);
    } else candidate = compose_mixed_selection_edit_candidates(original, leaves);
    protect_source(original, candidate, true, reviewed_erased);
    auto reviews = requirements(source, candidate, changed_hosts);
    return {std::move(candidate), std::move(reviews), std::move(reviewed_erased), std::move(retired_reviewed_rails)};
}
} // namespace

Json validate_mixed_selection_property_edit_intent(const Json& value) {
    try { validate(value); return value; }
    catch (const Json::exception&) { reject("malformed intent"); }
}
MixedSelectionPropertyEditPreparation::MixedSelectionPropertyEditPreparation(const DocumentSnapshot& source, Json intent,
    Entities entities, std::vector<SelectionGeometryRoomReviewRequirement> reviews)
    : intent_(std::move(intent)), preview_(source), source_digest_(intent_.at("source_snapshot_digest").get<std::string>()),
      room_reviews_(std::move(reviews)) {
    auto history = std::make_shared<std::vector<RevisionRecord>>(source.history());
    history->at(source.revision()).entities = std::move(entities);
    preview_.history_ = std::move(history);
    preview_.editable_ = false;
    preview_.read_only_reason_ = "Mixed Properties drawing preview; room review and final admission pending";
    preview_.admission_.reset();
}
const Json& MixedSelectionPropertyEditPreparation::intent() const noexcept { return intent_; }
const DocumentSnapshot& MixedSelectionPropertyEditPreparation::preview_snapshot() const noexcept { return preview_; }
const std::string& MixedSelectionPropertyEditPreparation::source_snapshot_digest() const noexcept { return source_digest_; }
const std::vector<SelectionGeometryRoomReviewRequirement>& MixedSelectionPropertyEditPreparation::required_room_reviews() const noexcept { return room_reviews_; }

namespace {
MixedSelectionPropertyEditPreparation prepare_properties(const DocumentSnapshot& source,
    const std::optional<SelectionPropertyEditRequest>& ordinary,
    const std::optional<SelectionPresentationPropertyEditRequest>& presentation,
    const std::optional<RoofOpeningGroupProperties>& children, std::string_view message,
    const StairPropertyReviewRequest* stair_review) {
    if (message.size() > 1024 || (children && children->members.size() > 1000)) reject("typed request budget exceeded");
    auto value = source_fields(source);
    value["version"] = 5;
    value["ordinary_request"] = ordinary ? ordinary_request(source, *ordinary) : Json(nullptr);
    value["presentation_request"] = presentation ? prepare_selection_presentation_property_edit(source, *presentation).request() : Json(nullptr);
    if (!value.at("presentation_request").is_null() && value.at("presentation_request").at("version") == 2)
        value["version"] = 6;
    if (!value.at("ordinary_request").is_null() && value.at("ordinary_request").at("version") == 2)
        value["version"] = 7;
    if (!value.at("ordinary_request").is_null() && value.at("ordinary_request").at("version") == 3)
        value["version"] = 8;
    if (stair_review) {
        if (!ordinary || ordinary->stair_profiles.empty()) reject("reviewed Properties requires a complete ordinary stair scalar roster");
        for (const auto& target : ordinary->selected)
            if (target.family == SelectionPropertyFamily::stair || target.family == SelectionPropertyFamily::railing) {
                const auto found = source.entities().find(target.entity_id);
                const auto* type = target.family == SelectionPropertyFamily::stair ? "stair" : "railing";
                if (found == source.entities().end() || found->second.id != target.entity_id || found->second.type != type)
                    reject("reviewed scalar selection requires its actual declared stair or railing family");
            }
        auto encoded = encode_stair_property_review_request(*stair_review);
        if (encoded.at("profiles").dump() != value.at("ordinary_request").at("stair_profiles").dump())
            reject("stair review must exactly cover the complete selected scalar profiles");
        for (const auto* fence : {"expected_revision", "source_snapshot_digest", "source_authoring_digest",
                "source_entities_digest", "source_saved_revision"})
            if (encoded.at(fence).dump() != value.at(fence).dump()) reject("stair review is not bound to the actual complete source");
        auto remaining = *ordinary;
        remaining.stair_profiles.clear();
        remaining.selected.erase(std::remove_if(remaining.selected.begin(), remaining.selected.end(), [](const auto& target) {
            return target.family == SelectionPropertyFamily::stair || target.family == SelectionPropertyFamily::railing;
        }), remaining.selected.end());
        value["ordinary_request"] = remaining.selected.empty() ? Json(nullptr) : ordinary_request(source, remaining);
        value["version"] = 9;
        value["stair_review_request"] = std::move(encoded);
    }
    value["skylight_properties"] = children ? skylight_properties(*children) : Json(nullptr);
    value["roof_opening_members"] = Json::array();
    if (children) for (const auto& member : children->members)
        value["roof_opening_members"].push_back({{"roof_id", member.roof_id}, {"opening_id", member.opening_id}});
    value["room_reviews"] = Json::array();
    value["message"] = std::string(message);
    return replay_mixed_selection_property_edit_preparation(source, value);
}
} // namespace
MixedSelectionPropertyEditPreparation prepare_mixed_selection_property_edit(const DocumentSnapshot& source,
    const std::optional<SelectionPropertyEditRequest>& ordinary,
    const std::optional<SelectionPresentationPropertyEditRequest>& presentation,
    const std::optional<RoofOpeningGroupProperties>& children, std::string_view message) {
    return prepare_properties(source, ordinary, presentation, children, message, nullptr);
}
MixedSelectionPropertyEditPreparation prepare_mixed_selection_property_edit(const DocumentSnapshot& source,
    const std::optional<SelectionPropertyEditRequest>& ordinary,
    const std::optional<SelectionPresentationPropertyEditRequest>& presentation,
    const std::optional<RoofOpeningGroupProperties>& children, std::string_view message,
    const StairPropertyReviewRequest& stair_review) {
    return prepare_properties(source, ordinary, presentation, children, message, &stair_review);
}
MixedSelectionPropertyEditPreparation replay_mixed_selection_property_edit_preparation(const DocumentSnapshot& source, const Json& value) {
    (void)validate_mixed_selection_property_edit_intent(value);
    if (!value.at("room_reviews").empty()) reject("preparation cannot accept completed room decisions");
    auto stage = derive(source, value);
    return {source, value, std::move(stage.entities), std::move(stage.reviews)};
}
void require_mixed_selection_property_edit_source_current(const DocumentSnapshot& source,
    const MixedSelectionPropertyEditPreparation& prepared) {
    (void)validate_mixed_selection_property_edit_intent(prepared.intent());
    authenticate(source, prepared.intent());
}
namespace {
ReplayedPhysicalWallRoomReview review_stage(const DocumentSnapshot& source, const Json& value, Stage stage, bool complete) {
    const auto& reviews = value.at("room_reviews");
    if (reviews.size() > stage.reviews.size() || (complete && reviews.size() != stage.reviews.size()))
        reject("room decisions must cover every required context/plane exactly once");
    const bool saved_phases = std::any_of(source.entities().begin(), source.entities().end(), [](const auto& entry) {
        return entry.second.type == "model_phases";
    });
    std::vector<bool> covered(stage.reviews.size(), false);
    std::vector<Json> intents;
    for (const auto& encoded : reviews) {
        const auto review = decode_physical_wall_room_review_intent(encoded);
        if (review.source_snapshot_digest != value.at("source_snapshot_digest") ||
            review.source_authoring_digest != value.at("source_authoring_digest") ||
            review.source_saved_revision != source.saved_revision_optional() || review.active_phase_room_scope != saved_phases)
            reject("room review must bind complete original source and actual active scope");
        const auto found = std::find_if(stage.reviews.begin(), stage.reviews.end(), [&](const auto& required) {
            return required.context == review.context && required.effective_elevation_m == review.effective_elevation_m;
        });
        if (found == stage.reviews.end()) reject("unrelated room review context/plane");
        const auto index = static_cast<std::size_t>(found - stage.reviews.begin());
        if (!complete && index != intents.size()) reject("room stage must follow the exact required review prefix");
        if (covered[index]) reject("duplicate room review context/plane");
        covered[index] = true;
        Ids retained;
        for (const auto& decision : review.retained)
            if (!retained.insert(decision.room_id).second) reject("duplicate retained room disposition");
        if (retained != Ids(found->retained_room_ids.begin(), found->retained_room_ids.end()))
            reject("room decisions must exactly cover all affected active retained rooms");
        intents.push_back(encoded);
    }
    if (complete && std::any_of(covered.begin(), covered.end(), [](bool value) { return !value; })) reject("required room review omitted");
    ReplayedPhysicalWallRoomReview result;
    const bool active_policy = saved_phases || source.uses_active_phase_constraints();
    if (intents.empty()) result.entities = std::move(stage.entities);
    else if (intents.size() == 1) result = replay_physical_wall_room_review(stage.entities, intents.front(), active_policy, &source.entities());
    else result = replay_physical_wall_room_review_batch(stage.entities, intents, active_policy, &source.entities());
    // Pure room replay checks each actual preceding entity-map digest and
    // rederives correspondences/transitions. The source metadata above never
    // substitutes a caller-reviewed map for that proof.
    protect_source(source.entities(), result.entities, false, stage.reviewed_erased);
    if (value.at("version") == 9) require_selected_members_present(value, result.entities, stage.retired_reviewed_rails);
    if (complete)
        if (const auto error = validate_active_phase_constraint_integrity(result.entities)) throw std::invalid_argument(*error);
    return result;
}
} // namespace
ReplayedPhysicalWallRoomReview replay_mixed_selection_property_edit(const DocumentSnapshot& source, const Json& value) {
    return review_stage(source, value, derive(source, value), true);
}
DocumentSnapshot preview_mixed_selection_property_room_stage(const DocumentSnapshot& source,
    const MixedSelectionPropertyEditPreparation& prepared, const std::vector<Json>& reviews) {
    require_mixed_selection_property_edit_source_current(source, prepared);
    if (reviews.size() > 32) reject("room review prefix budget exceeded");
    auto value = prepared.intent();
    value["room_reviews"] = reviews;
    auto stage = derive(source, value);
    auto requirements = stage.reviews;
    auto replayed = review_stage(source, value, std::move(stage), false);
    const MixedSelectionPropertyEditPreparation preview(source, std::move(value),
        std::move(replayed.entities), std::move(requirements));
    return preview.preview_snapshot();
}
Json complete_mixed_selection_property_edit(const DocumentSnapshot& source,
    const MixedSelectionPropertyEditPreparation& prepared, const std::vector<Json>& reviews) {
    require_mixed_selection_property_edit_source_current(source, prepared);
    if (reviews.size() > 32) reject("room review group budget exceeded");
    auto value = prepared.intent();
    value["room_reviews"] = reviews;
    (void)replay_mixed_selection_property_edit(source, value);
    return validate_mixed_selection_property_edit_intent(value);
}
bool mixed_selection_property_edit_has_geometry(const Json& value) {
    validate(value);
    return value.at("version") == 9 || !value.at("ordinary_request").is_null() || !value.at("skylight_properties").is_null();
}
bool mixed_selection_property_edit_active_phase_policy(const Json& value) {
    validate(value);
    return true;
}
std::vector<Json> mixed_selection_property_edit_room_reviews(const Json& value) {
    validate(value);
    return value.at("room_reviews").get<std::vector<Json>>();
}
std::optional<Revision> mixed_selection_property_edit_source_saved_revision(const Json& value) {
    validate(value);
    return value.at("source_saved_revision").is_null() ? std::nullopt :
        std::optional<Revision>{revision(value.at("source_saved_revision"))};
}
} // namespace sketch
