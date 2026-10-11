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
        request.slabs.size() > 1000 || request.hosted_openings.size() > 1000 || request.roofs.size() > 1000)
        reject("typed ordinary Properties budget exceeded");
    // Encode without admitting profile-only intermediate roofs. The canonical
    // leaf codec authenticates the complete scalar roster before fused replay.
    auto result = source_fields(source);
    result["version"] = 1;
    result["selected"] = Json::array();
    for (const auto& target : request.selected) {
        const char* name = nullptr;
        switch (target.family) {
        case SelectionPropertyFamily::wall: name = "wall"; break;
        case SelectionPropertyFamily::slab: name = "slab"; break;
        case SelectionPropertyFamily::hosted_opening: name = "hosted_opening"; break;
        case SelectionPropertyFamily::roof: name = "roof"; break;
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
    if (request.unchanged_policy != SelectionPropertyUnchangedPolicy::retain &&
        request.unchanged_policy != SelectionPropertyUnchangedPolicy::reject) reject("unsupported unchanged policy");
    result["unchanged_policy"] = request.unchanged_policy == SelectionPropertyUnchangedPolicy::retain ? "retain" : "reject";
    return validate_selection_property_edit_request(result);
}
void validate(const Json& value) {
    Budget budget; budget.visit(value);
    fields(value, {"version", "expected_revision", "source_snapshot_digest", "source_authoring_digest",
        "source_entities_digest", "source_saved_revision", "ordinary_request", "presentation_request",
        "skylight_properties", "roof_opening_members", "room_reviews", "message"});
    if (!value.at("version").is_number_integer() || value.at("version") != 5) reject("unsupported Properties dialect");
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
        if (std::string_view(name) == "ordinary_request") (void)validate_selection_property_edit_request(lane);
        else (void)validate_selection_presentation_property_edit_request(lane);
        for (const auto* fence : {"expected_revision", "source_snapshot_digest", "source_authoring_digest",
                "source_entities_digest", "source_saved_revision"})
            if (lane.at(fence).dump() != value.at(fence).dump()) reject("leaf source fences differ from complete original source");
        for (const auto& row : lane.at("selected")) {
            if (std::string_view(name) == "ordinary_request" || row.at("family") == "reference")
                select({"entity", identity(row.at("entity_id")), ""});
            else select({row.at("family").get<std::string>(), identity(row.at("owner_id")), identity(row.at("child_id"))});
        }
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
void protect_source(const Entities& original, const Entities& candidate, bool before_review) {
    const auto scope = constraint_phase_scope(original);
    const auto baselines = retained_baselines(original);
    for (const auto& [id, entity] : original) {
        const auto found = candidate.find(id);
        if (scope.inactive_owner_ids.contains(id) || baselines.contains(id) ||
            (before_review && (entity.type == "room" || is_physical_wall_room(entity))))
            if (found == candidate.end() || !exact(entity, found->second))
                reject("ordinary Properties changed a protected inactive, baseline or retained room owner");
        if (before_review && found == candidate.end()) reject("Properties cannot remove original identities");
    }
    if (before_review && candidate.size() != original.size()) reject("Properties cannot add identities");
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
struct Stage { Entities entities; std::vector<SelectionGeometryRoomReviewRequirement> reviews; };
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
    auto candidate = compose_mixed_selection_edit_candidates(original, leaves);
    protect_source(original, candidate, true);
    auto reviews = requirements(source, candidate, changed_hosts);
    return {std::move(candidate), std::move(reviews)};
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

MixedSelectionPropertyEditPreparation prepare_mixed_selection_property_edit(const DocumentSnapshot& source,
    const std::optional<SelectionPropertyEditRequest>& ordinary,
    const std::optional<SelectionPresentationPropertyEditRequest>& presentation,
    const std::optional<RoofOpeningGroupProperties>& children, std::string_view message) {
    if (message.size() > 1024 || (children && children->members.size() > 1000)) reject("typed request budget exceeded");
    auto value = source_fields(source);
    value["version"] = 5;
    value["ordinary_request"] = ordinary ? ordinary_request(source, *ordinary) : Json(nullptr);
    value["presentation_request"] = presentation ? prepare_selection_presentation_property_edit(source, *presentation).request() : Json(nullptr);
    value["skylight_properties"] = children ? skylight_properties(*children) : Json(nullptr);
    value["roof_opening_members"] = Json::array();
    if (children) for (const auto& member : children->members)
        value["roof_opening_members"].push_back({{"roof_id", member.roof_id}, {"opening_id", member.opening_id}});
    value["room_reviews"] = Json::array();
    value["message"] = std::string(message);
    return replay_mixed_selection_property_edit_preparation(source, value);
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
    protect_source(source.entities(), result.entities, false);
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
    return !value.at("ordinary_request").is_null() || !value.at("skylight_properties").is_null();
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
