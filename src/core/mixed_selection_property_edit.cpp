#include "sketch/mixed_selection_property_edit.hpp"

#include "sketch/constraint_entity.hpp"
#include "sketch/constraint_integrity.hpp"
#include "sketch/constraint_phase_scope.hpp"
#include "sketch/document_digest.hpp"
#include "sketch/document_wall.hpp"
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
using MemberKey = std::vector<std::string>;
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
    const bool vertical = std::any_of(request.walls.begin(), request.walls.end(), [](const auto& wall) { return wall.elevation.has_value(); });
    result["version"] = vertical ? 4 : !request.stair_profiles.empty() ? 3 : request.structural_profiles.empty() ? 1 : 2;
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
    if (vertical || !request.structural_profiles.empty() || !request.stair_profiles.empty())
        lane("structural_profiles", request.structural_profiles, encode_structural_profile_edit_intent);
    if (vertical || !request.stair_profiles.empty())
        lane("stair_profiles", request.stair_profiles, encode_stair_profile_edit_intent);
    if (request.unchanged_policy != SelectionPropertyUnchangedPolicy::retain &&
        request.unchanged_policy != SelectionPropertyUnchangedPolicy::reject) reject("unsupported unchanged policy");
    result["unchanged_policy"] = request.unchanged_policy == SelectionPropertyUnchangedPolicy::retain ? "retain" : "reject";
    return validate_selection_property_edit_request(result);
}
// Dialect twelve validates its own lanes directly. A historical wrapper cannot
// stand in for the new scalar/phase authority, even for selected-roster checks.
void validate_vertical_members(const Json& value, const Json& scalar) {
    (void)revision(value.at("expected_revision"));
    for (const auto* key : {"source_snapshot_digest", "source_authoring_digest", "source_entities_digest"}) digest(value.at(key));
    if (!value.at("source_saved_revision").is_null()) (void)revision(value.at("source_saved_revision"));
    if (!value.at("message").is_string() || value.at("message").get_ref<const std::string&>().size() > 1024 ||
        value.at("message").get_ref<const std::string&>().find('\0') != std::string::npos) reject("invalid message");
    std::set<MemberKey> selected;
    const auto select = [&](MemberKey key) {
        if (!selected.insert(std::move(key)).second || selected.size() > 1000) reject("duplicate or over-budget selected member");
    };
    for (const auto& row : scalar.at("selected")) select({"entity", identity(row.at("entity_id")), ""});
    const auto fence = [&](const Json& lane) {
        for (const auto* key : {"expected_revision", "source_snapshot_digest", "source_authoring_digest", "source_entities_digest", "source_saved_revision"})
            if (lane.at(key).dump() != value.at(key).dump()) reject("leaf source fences differ from complete original source");
    };
    const auto& presentation = value.at("presentation_request");
    if (!presentation.is_null()) {
        (void)validate_selection_presentation_property_edit_request(presentation);
        fence(presentation);
        for (const auto& row : presentation.at("selected")) {
            if (row.at("family") == "reference") select({"entity", identity(row.at("entity_id")), ""});
            else select({row.at("family").get<std::string>(), identity(row.at("owner_id")), identity(row.at("child_id"))});
        }
    }
    const auto& assembly = value.at("assembly_request");
    if (!assembly.is_null()) {
        (void)validate_assembly_instance_property_edit_request(assembly);
        fence(assembly);
        for (const auto& row : assembly.at("intents")) {
            const auto target = decode_assembly_instance_property_edit_intent(row).target;
            MemberKey key{"assembly", target.catalog_id, target.instance_id, target.document_entity_id.value_or("")};
            key.insert(key.end(), target.part_path.begin(), target.part_path.end());
            select(std::move(key));
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
}
void validate(const Json& value) {
    Budget budget; budget.visit(value);
    if (value.is_object() && value.contains("version") && (value.at("version") == 11 || value.at("version") == 12)) {
        const bool vertical = value.at("version") == 12;
        if (!value.at("version").is_number_integer()) reject("Properties dialect must be an integer");
        fields(value, {"version", "expected_revision", "source_snapshot_digest", "source_authoring_digest",
            "source_entities_digest", "source_saved_revision", "phase_request", "presentation_request",
            "skylight_properties", "roof_opening_members", "room_reviews", "phase_room_reviews", "message", "assembly_request"});
        const auto phase = validate_phase_selection_property_edit_request(value.at("phase_request"));
        if (vertical ? phase.at("version") != 3 : phase.at("version") == 3)
            reject("elevation Properties requires its explicit phase request three");
        const auto& scalar = phase.at("scalar_request");
        const auto& phase_fences = phase.at("version") != 1 ? phase : scalar;
        for (const auto* fence : {"expected_revision", "source_snapshot_digest", "source_authoring_digest",
                "source_entities_digest", "source_saved_revision"})
            if (phase_fences.at(fence).dump() != value.at(fence).dump()) reject("phase source fences differ from complete original source");
        if (phase.at("version") != 1) {
            if (phase.at("skylight_properties").dump() != value.at("skylight_properties").dump() ||
                phase.at("roof_opening_members").dump() != value.at("roof_opening_members").dump())
                reject("outer selected skylights differ from their sole phase request authority");
        } else if (!value.at("skylight_properties").is_null()) reject("phase request one has no skylight contribution");
        if (vertical) validate_vertical_members(value, scalar);
        else {
            // Retain dialect eleven's historical roster validation exactly.
            auto roster = value;
            roster.erase("phase_request"); roster.erase("phase_room_reviews");
            roster["ordinary_request"] = scalar;
            if (!value.at("assembly_request").is_null()) {
                roster["version"] = 10; roster["stair_review_request"] = nullptr;
            } else {
                roster.erase("assembly_request");
                roster["version"] = !scalar.is_null() && scalar.at("version") == 3 ? 8 : !scalar.is_null() && scalar.at("version") == 2 ? 7 :
                    !value.at("presentation_request").is_null() && value.at("presentation_request").at("version") == 2 ? 6 : 5;
            }
            validate(roster);
        }
        const auto& reviews = value.at("phase_room_reviews");
        if (!reviews.is_array() || reviews.size() > 1000) reject("phase room review budget exceeded");
        std::optional<std::size_t> previous;
        for (const auto& row : reviews) {
            fields(row, {"phase_authoring_index", "wall_replacement"});
            const auto index = revision(row.at("phase_authoring_index"));
            if (index >= 1000 || (previous && index <= *previous)) reject("phase room reviews must be an ordered unique leaf roster");
            previous = static_cast<std::size_t>(index);
            const auto leaf = decode_phase_wall_replacement_authoring(row.at("wall_replacement"));
            if (!vertical && leaf.complete_vertical_profiles) reject("historical Properties has no elevation room completion authority");
            if (encode_phase_wall_replacement_authoring(leaf).dump() != row.at("wall_replacement").dump() || leaf.room_review_intent.is_null())
                reject("phase room completion requires canonical explicit decisions");
        }
        if (value.dump().size() > byte_limit) reject("wire byte budget exceeded");
        return;
    }
    const bool assembly_dialect = value.is_object() && value.contains("version") && value.at("version") == 10;
    const bool reviewed_stair_dialect = value.is_object() && value.contains("version") && value.at("version") == 9;
    if (assembly_dialect)
        fields(value, {"version", "expected_revision", "source_snapshot_digest", "source_authoring_digest",
            "source_entities_digest", "source_saved_revision", "ordinary_request", "presentation_request",
            "skylight_properties", "roof_opening_members", "room_reviews", "message", "stair_review_request", "assembly_request"});
    else if (reviewed_stair_dialect)
        fields(value, {"version", "expected_revision", "source_snapshot_digest", "source_authoring_digest",
            "source_entities_digest", "source_saved_revision", "ordinary_request", "presentation_request",
            "skylight_properties", "roof_opening_members", "room_reviews", "message", "stair_review_request"});
    else
        fields(value, {"version", "expected_revision", "source_snapshot_digest", "source_authoring_digest",
            "source_entities_digest", "source_saved_revision", "ordinary_request", "presentation_request",
            "skylight_properties", "roof_opening_members", "room_reviews", "message"});
    if (!value.at("version").is_number_integer() ||
        (value.at("version") != 5 && value.at("version") != 6 && value.at("version") != 7 && value.at("version") != 8 &&
            !reviewed_stair_dialect && !assembly_dialect))
        reject("unsupported Properties dialect");
    const bool reviewed_stair = reviewed_stair_dialect ||
        (assembly_dialect && !value.at("stair_review_request").is_null());
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
            if (lane.at("version") == 4) reject("historical Properties has no wall elevation authority");
            if (reviewed_stair ? (lane.at("version") != 1 && lane.at("version") != 2) :
                !assembly_dialect && lane.at("version") != (stair_dialect ? 3 : structural_dialect ? 2 : 1))
                reject("ordinary dialect differs from complete Properties authority");
        }
        else {
            (void)validate_selection_presentation_property_edit_request(lane);
            if (!assembly_dialect && !reviewed_stair_dialect && !structural_dialect && !stair_dialect && lane.at("version") != (pose_dialect ? 2 : 1))
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
    if (reviewed_stair) {
        const auto& lane = value.at("stair_review_request");
        const auto request = decode_stair_property_review_request(lane);
        for (const auto* fence : {"expected_revision", "source_snapshot_digest", "source_authoring_digest",
                "source_entities_digest", "source_saved_revision"})
            if (lane.at(fence).dump() != value.at(fence).dump()) reject("stair review source fences differ from complete original source");
        for (const auto& profile : request.profiles) select({"entity", profile.object_id, ""});
    }
    if (assembly_dialect) {
        const auto& lane = value.at("assembly_request");
        (void)validate_assembly_instance_property_edit_request(lane);
        for (const auto* fence : {"expected_revision", "source_snapshot_digest", "source_authoring_digest",
                "source_entities_digest", "source_saved_revision"})
            if (lane.at(fence).dump() != value.at(fence).dump()) reject("assembly source fences differ from complete original source");
        for (const auto& row : lane.at("intents")) {
            const auto target = decode_assembly_instance_property_edit_intent(row).target;
            // An absent entity occupies its own qualified slot; no displayed
            // alias or flattened path can stand in for the actual source key.
            MemberKey key{"assembly", target.catalog_id, target.instance_id, target.document_entity_id.value_or("")};
            key.insert(key.end(), target.part_path.begin(), target.part_path.end());
            select(std::move(key));
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
    const Ids& retired_reviewed_rails = {}, const PhaseSelectionPropertyEditPreparation* phase = nullptr,
    bool final_physical_check = true) {
    const auto owner = [&](const std::string& id) -> const Entity& {
        const auto found = candidate.find(id);
        if (found == candidate.end() || found->second.id != id) reject("reviewed retirement removed a selected owner");
        return found->second;
    };
    const auto& ordinary = phase ? phase->inspection().scalar_request() : value.at("ordinary_request");
    if (!ordinary.is_null()) for (const auto& row : ordinary.at("selected"))
        {
            const auto id = row.at("entity_id").get<std::string>();
            if (phase && std::find(phase->explicitly_retired_selected_ids().begin(), phase->explicitly_retired_selected_ids().end(), id) !=
                    phase->explicitly_retired_selected_ids().end()) continue;
            (void)owner(phase ? phase->selected_original_to_proposed().at(id) : id);
        }
    if ((value.at("version") == 9 || value.at("version") == 10) && !value.at("stair_review_request").is_null())
        for (const auto& row : value.at("stair_review_request").at("profiles")) {
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
        auto roof = row.at("roof_id").get<std::string>();
        auto child = row.at("opening_id").get<std::string>();
        if (phase) {
            const auto& mapped = phase->selected_roof_opening_original_to_proposed().at({roof, child});
            roof = mapped.roof_id; child = mapped.opening_id;
        }
        const auto& members = owner(roof).properties.at("roof_openings");
        if (std::count_if(members.begin(), members.end(), [&](const auto& member) {
                return member.at("id") == child;
            }) != 1) reject("reviewed retirement removed a selected skylight child");
    }
    if ((value.at("version") == 10 || value.at("version") == 11 || value.at("version") == 12) && !value.at("assembly_request").is_null()) {
        using RootKey = std::tuple<std::string, std::string, std::optional<std::string>>;
        std::map<RootKey, std::vector<std::vector<std::string>>> selected_roots;
        std::vector<AssemblyInstancePropertyEditIntent> physical_checks;
        for (const auto& row : value.at("assembly_request").at("intents")) {
            auto target = decode_assembly_instance_property_edit_intent(row).target;
            if (phase) {
                std::optional<std::pair<std::string, std::string>> mapped;
                for (const auto& allocation : phase->validated_identity_allocations()) {
                    const auto binding = allocation.hosted_instance_identities.find({target.catalog_id, target.instance_id});
                    if (binding == allocation.hosted_instance_identities.end()) continue;
                    const auto catalog = allocation.identities.find(target.catalog_id);
                    if (catalog == allocation.identities.end()) reject("qualified copied assembly lacks its validated catalog allocation");
                    const std::pair destination{catalog->second, binding->second};
                    if (mapped && *mapped != destination) reject("selected qualified assembly has ambiguous copied destinations");
                    mapped = destination;
                }
                if (mapped) { target.catalog_id = mapped->first; target.instance_id = mapped->second; }
                if (target.document_entity_id) {
                    const auto found = phase->original_to_proposed().find(*target.document_entity_id);
                    if (found != phase->original_to_proposed().end()) target.document_entity_id = found->second;
                }
            }
            selected_roots[{target.catalog_id, target.instance_id, target.document_entity_id}].push_back(target.part_path);
            AssemblyInstancePropertyEditIntent retained;
            retained.target = std::move(target);
            physical_checks.push_back(std::move(retained));
        }
        std::map<std::string, AssemblyModel, std::less<>> models;
        AssemblyExpansionBudget budget;
        for (const auto& [key, paths] : selected_roots) {
            const auto& [catalog_id, instance_id, entity_id] = key;
            const auto& catalog = owner(catalog_id);
            if (catalog.type != "assembly_model") reject("selected assembly catalog changed its source role");
            if (!models.contains(catalog_id)) models.emplace(catalog_id, AssemblyModel::from_json(catalog.properties.at("model")));
            const auto& model = models.at(catalog_id);
            AssemblyInstance instance;
            if (entity_id) {
                const auto& root = owner(*entity_id);
                if (root.type != "assembly_instance") reject("selected independent assembly changed its source role");
                const auto actual = decode_document_assembly_instance(root);
                if (actual.assembly_catalog_id != catalog_id || actual.instance.id != instance_id)
                    reject("selected independent assembly lost its exact qualified source identity");
                instance = actual.instance;
            } else {
                const auto found = std::find_if(model.instances().begin(), model.instances().end(),
                    [&](const auto& row) { return row.id == instance_id; });
                if (found == model.instances().end()) reject("selected embedded assembly row disappeared");
                instance = *found;
            }
            // A retained hosted row cannot survive by referencing a rail which
            // another lane explicitly retired. Its actual source owner survives.
            if (instance.placement) {
                const auto& host = owner(instance.placement->host_entity_id);
                if (!is_complete_assembly_catalog_host_type(host.type)) reject("selected assembly lost its actual host role");
            }
            const auto expansion = model.expand(instance, budget);
            for (const auto& path : paths)
                if (std::count_if(expansion.nodes.begin(), expansion.nodes.end(), [&](const auto& node) {
                        return node.part_path == path;
                    }) != 1) reject("selected assembly lost its exact expanded part path");
        }
        // Original-source leaves cannot certify a host body changed by another
        // lane. Admit the complete composed physical cohort through no-op
        // intents, including actual host copies and retained members. This
        // discards the return map and grants no candidate authoring authority.
        if (final_physical_check) (void)replay_assembly_instance_property_entities(candidate, physical_checks);
    }
}
std::vector<SelectionGeometryRoomReviewRequirement> requirements(const DocumentSnapshot& source,
    const Entities& candidate, const Ids& changed_hosts, bool vertical = false) {
    if (changed_hosts.empty()) return {};
    const auto organization = organize_project(source);
    struct Room { std::string id; DrawingContext context; PhysicalWallRoomLineageCheck lineage; };
    std::vector<Room> rooms;
    std::vector<SelectionGeometryRoomReviewRequirement> result;
    if (vertical) for (const auto& id : changed_hosts) {
        const auto context = organization.drawing_context(id);
        if (!context || !context->complete()) reject("elevation wall has unresolved original drawing context");
        const auto elevation = [&](const Entities& entities) {
            Wall wall;
            std::string error;
            if (!read_document_wall(resolve_vertical_placement(entities, entities.at(id)), {}, wall, error))
                throw std::invalid_argument("Mixed selection Properties elevation wall " + id + ": " + error);
            return wall.elevation;
        };
        const double before = elevation(source.entities()), after = elevation(candidate);
        if (std::abs(before - after) <= default_geometry_tolerance_metres) continue;
        for (const auto plane : {before, after}) {
            const auto found = std::find_if(result.begin(), result.end(), [&](const auto& group) {
                return group.context == *context && std::abs(group.effective_elevation_m - plane) <= default_geometry_tolerance_metres;
            });
            if (found == result.end()) result.push_back({*context, plane, {}, {id}, {}, {}, {}});
            else found->source_wall_ids.push_back(id);
        }
    }
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
    std::optional<PhaseSelectionPropertyEditPreparation> phase;
    std::vector<PhysicalWallRoomPhaseReviewInventory> phase_reviews;
};
Stage derive_phase(const DocumentSnapshot& source, const Json& value) {
    auto phase = replay_phase_selection_property_edit_preparation(source, value.at("phase_request"));
    const auto scope = constraint_phase_scope(source.entities());
    std::vector<Entities> leaves;
    Ids changed_hosts, erased, retired;
    if (!phase.ordinary_scalar_request().is_null()) {
        const auto ordinary = replay_selection_property_edit_preparation(source, phase.ordinary_scalar_request());
        leaves.push_back(ordinary.preview_snapshot().entities());
        for (const auto& id : ordinary.affected_wall_ids())
            if (!exact(source.entities().at(id), leaves.back().at(id))) changed_hosts.insert(id);
        for (const auto& row : phase.ordinary_scalar_request().at("hosted_openings"))
            if (!exact(source.entities().at(row.at("opening_id").get<std::string>()), leaves.back().at(row.at("opening_id").get<std::string>())))
                changed_hosts.insert(row.at("wall_id").get<std::string>());
    }
    if (!phase.ordinary_roof_edits().empty())
        leaves.push_back(replay_roof_edit_entities(source.entities(), phase.ordinary_roof_edits()));
    if (!value.at("presentation_request").is_null()) {
        for (const auto& row : value.at("presentation_request").at("selected")) {
            const auto id = row.at("family") == "reference" ? row.at("entity_id").get<std::string>() : row.at("owner_id").get<std::string>();
            if (scope.inactive_owner_ids.contains(id)) reject("selected presentation owner is inactive");
        }
        leaves.push_back(replay_selection_presentation_property_edit_preparation(source, value.at("presentation_request")).candidate_entities());
    }
    std::optional<AssemblyInstancePropertyEditPreparation> assemblies;
    if (!value.at("assembly_request").is_null())
        assemblies = replay_assembly_instance_property_edit_preparation(source, value.at("assembly_request"));
    std::optional<StairPropertyReviewPreparation> stairs;
    if (!phase.ordinary_stair_review_request().is_null()) {
        stairs = replay_stair_property_review_preparation(source, phase.ordinary_stair_review_request());
        retired.insert(stairs->retired_rail_ids().begin(), stairs->retired_rail_ids().end());
    }
    auto candidate = compose_phase_property_selection_edit_candidates(source, phase.phase_authorings(), leaves,
        stairs ? &*stairs : nullptr, assemblies ? &*assemblies : nullptr);
    if (value.at("version") == 12) {
        // Admit strict vertical dependencies only after every original-source
        // contribution has been composed, including copied hosts and catalog
        // Properties. No individual leaf certifies that final physical graph.
        Ids hosts;
        const auto final_id = [&](const std::string& id) {
            const auto mapped = phase.original_to_proposed().find(id);
            return mapped == phase.original_to_proposed().end() ? id : mapped->second;
        };
        const auto& scalar = phase.inspection().scalar_request();
        for (const auto& row : scalar.at("walls")) hosts.insert(final_id(row.at("wall_id").get<std::string>()));
        for (const auto& row : scalar.at("hosted_openings")) hosts.insert(final_id(row.at("wall_id").get<std::string>()));
        validate_active_wall_physical_dependencies(candidate, hosts, true, true);
    }
    for (const auto& [id, entity] : source.entities()) { (void)entity; if (!candidate.contains(id)) erased.insert(id); }
    require_selected_members_present(value, candidate, retired, &phase, false);
    std::vector<PhysicalWallRoomPhaseReviewInventory> phase_reviews;
    for (const auto& wire : phase.phase_authorings()) {
        const auto intent = decode_phase_constraint_authoring_intent(wire);
        if (!intent.wall_replacement.is_null()) {
            const auto preview = inspect_phase_wall_replacement_rooms_on_composed_stage(source, intent, candidate);
            if (preview.needs_room_review) phase_reviews.push_back(preview.room_inventory);
        }
    }
    auto reviews = requirements(source, candidate, changed_hosts, value.at("version") == 12);
    // Phase reviews own the entire affected analytical plane, including
    // ordinary wall edits composed there. Historical room codecs cannot grant
    // baseline lineage authority on that same plane.
    std::erase_if(reviews, [&](const auto& review) {
        return std::any_of(phase_reviews.begin(), phase_reviews.end(), [&](const auto& inventory) {
            return std::any_of(inventory.intent.planes.begin(), inventory.intent.planes.end(), [&](const auto& plane) {
                return plane.context == review.context &&
                    std::abs(plane.effective_elevation_m - review.effective_elevation_m) <= default_geometry_tolerance_metres;
            });
        });
    });
    return {std::move(candidate), std::move(reviews), std::move(erased), std::move(retired), std::move(phase), std::move(phase_reviews)};
}
Stage derive(const DocumentSnapshot& source, const Json& value) {
    validate(value); authenticate(source, value);
    const auto& original = source.entities();
    if (const auto error = validate_active_phase_constraint_integrity(original)) throw std::invalid_argument(*error);
    if (value.at("version") == 11 || value.at("version") == 12) return derive_phase(source, value);
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
    std::optional<AssemblyInstancePropertyEditPreparation> assembly_prepared;
    if (value.at("version") == 10) {
        // The producer authenticates its own full original snapshot and derives
        // the complete map. Assembly-only edits acquire no changed-host room
        // authority; the ordinary lane still owns that source-derived roster.
        assembly_prepared = replay_assembly_instance_property_edit_preparation(source, value.at("assembly_request"));
    }
    Ids reviewed_erased;
    Ids retired_reviewed_rails;
    Entities candidate;
    if ((value.at("version") == 9 || value.at("version") == 10) && !value.at("stair_review_request").is_null()) {
        for (const auto& row : value.at("stair_review_request").at("profiles"))
            if (scope.inactive_owner_ids.contains(row.at("object_id").get<std::string>()))
                reject("selected reviewed stair owner is inactive");
        const auto reviewed = replay_stair_property_review_preparation(source, value.at("stair_review_request"));
        reviewed_erased.insert(reviewed.erased_entity_ids().begin(), reviewed.erased_entity_ids().end());
        retired_reviewed_rails.insert(reviewed.retired_rail_ids().begin(), reviewed.retired_rail_ids().end());
        // The sealed compositor separately replays this request and admits only
        // its exact existing-producer closure beside original-source leaves.
        candidate = assembly_prepared
            ? compose_assembly_property_selection_edit_candidates(source, *assembly_prepared, leaves, &reviewed)
            : compose_stair_property_review_selection_edit_candidates(source, reviewed, leaves);
    } else candidate = assembly_prepared
        ? compose_assembly_property_selection_edit_candidates(source, *assembly_prepared, leaves)
        : compose_mixed_selection_edit_candidates(original, leaves);
    if (value.at("version") == 9 || value.at("version") == 10)
        require_selected_members_present(value, candidate, retired_reviewed_rails);
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
    Entities entities, std::vector<SelectionGeometryRoomReviewRequirement> reviews,
    std::optional<PhaseSelectionPropertyEditPreparation> phase,
    std::vector<PhysicalWallRoomPhaseReviewInventory> phase_reviews)
    : intent_(std::move(intent)), preview_(source), source_digest_(intent_.at("source_snapshot_digest").get<std::string>()),
      room_reviews_(std::move(reviews)), phase_(std::move(phase)), phase_room_reviews_(std::move(phase_reviews)) {
    if (phase_) {
        for (std::size_t index = 0; index < phase_->phase_authorings().size(); ++index) {
            const auto intent = decode_phase_constraint_authoring_intent(phase_->phase_authorings()[index]);
            if (intent.wall_replacement.is_null()) continue;
            const auto leaf = decode_phase_wall_replacement_authoring(intent.wall_replacement);
            if (std::any_of(phase_room_reviews_.begin(), phase_room_reviews_.end(), [&](const auto& inventory) {
                    return inventory.intent.registry_id == leaf.registry_id && inventory.intent.alternative_id == leaf.alternative_id;
                })) phase_room_review_indices_.push_back(index);
        }
        if (phase_room_review_indices_.size() != phase_room_reviews_.size()) reject("phase room inventory lost its exact source leaf");
        for (const auto& allocation : phase_->validated_identity_allocations())
            for (const auto& [source_binding, destination_instance] : allocation.hosted_instance_identities) {
                const auto catalog = allocation.identities.find(source_binding.first);
                if (catalog == allocation.identities.end()) reject("copied hosted binding lacks its validated catalog identity");
                const std::pair destination{catalog->second, destination_instance};
                const auto [found, inserted] = hosted_bindings_.emplace(source_binding, destination);
                if (!inserted && found->second != destination) reject("qualified hosted binding has ambiguous phase destinations");
            }
        roof_children_ = phase_->selected_roof_opening_original_to_proposed();
    }
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
const std::map<std::string, std::string, std::less<>>& MixedSelectionPropertyEditPreparation::selected_original_to_proposed() const noexcept {
    static const std::map<std::string, std::string, std::less<>> empty;
    return phase_ ? phase_->selected_original_to_proposed() : empty;
}
const std::map<std::string, std::string, std::less<>>& MixedSelectionPropertyEditPreparation::original_to_proposed() const noexcept {
    static const std::map<std::string, std::string, std::less<>> empty;
    return phase_ ? phase_->original_to_proposed() : empty;
}
const PhaseStairReplacementChildIdentityMap& MixedSelectionPropertyEditPreparation::stair_child_original_to_proposed() const noexcept {
    static const PhaseStairReplacementChildIdentityMap empty;
    return phase_ ? phase_->stair_child_original_to_proposed() : empty;
}
const MixedSelectionPropertyEditPreparation::HostedBindingMap& MixedSelectionPropertyEditPreparation::hosted_binding_original_to_proposed() const noexcept { return hosted_bindings_; }
const std::map<std::pair<std::string, std::string>, RoofOpeningGroupMember>& MixedSelectionPropertyEditPreparation::roof_child_original_to_proposed() const noexcept { return roof_children_; }
const std::vector<PhysicalWallRoomPhaseReviewInventory>& MixedSelectionPropertyEditPreparation::required_phase_room_reviews() const noexcept { return phase_room_reviews_; }
const std::vector<std::size_t>& MixedSelectionPropertyEditPreparation::required_phase_room_review_indices() const noexcept { return phase_room_review_indices_; }
const std::vector<PhaseWallReplacementAuthoringPreview>& MixedSelectionPropertyEditPreparation::phase_wall_previews() const noexcept {
    static const std::vector<PhaseWallReplacementAuthoringPreview> empty;
    return phase_ ? phase_->wall_previews() : empty;
}
const std::vector<Json>& MixedSelectionPropertyEditPreparation::phase_authorings() const noexcept {
    static const std::vector<Json> empty;
    return phase_ ? phase_->phase_authorings() : empty;
}

namespace {
MixedSelectionPropertyEditPreparation prepare_properties(const DocumentSnapshot& source,
    const std::optional<SelectionPropertyEditRequest>& ordinary,
    const std::optional<SelectionPresentationPropertyEditRequest>& presentation,
    const std::optional<RoofOpeningGroupProperties>& children, std::string_view message,
    const StairPropertyReviewRequest* stair_review,
    const AssemblyInstancePropertyEditPreparation* assemblies = nullptr) {
    if (message.size() > 1024 || (children && children->members.size() > 1000)) reject("typed request budget exceeded");
    auto value = source_fields(source);
    if (ordinary && std::any_of(ordinary->walls.begin(), ordinary->walls.end(), [](const auto& wall) { return wall.elevation.has_value(); })) {
        PhaseSelectionPropertyEditRequest request;
        request.properties = *ordinary;
        request.roof_opening_properties = children;
        request.message = std::string(message);
        if (stair_review) {
            const auto encoded = encode_stair_property_review_request(*stair_review);
            const auto scalar = ordinary_request(source, *ordinary);
            if (ordinary->stair_profiles.empty() || encoded.at("profiles").dump() != scalar.at("stair_profiles").dump())
                reject("stair review must exactly cover the complete selected scalar profiles");
            for (const auto* fence : {"expected_revision", "source_snapshot_digest", "source_authoring_digest", "source_entities_digest", "source_saved_revision"})
                if (encoded.at(fence).dump() != value.at(fence).dump()) reject("stair review is not bound to the actual complete source");
            request.stair_dependency_dispositions = stair_review->dependency_dispositions;
        }
        const auto phase = prepare_phase_selection_property_edit(source, request);
        value["version"] = 12;
        value["phase_request"] = phase.request();
        value["presentation_request"] = presentation ? prepare_selection_presentation_property_edit(source, *presentation).request() : Json(nullptr);
        value["assembly_request"] = assemblies ? assemblies->request() : Json(nullptr);
        value["skylight_properties"] = phase.request().at("skylight_properties");
        value["roof_opening_members"] = phase.request().at("roof_opening_members");
        value["room_reviews"] = Json::array();
        value["phase_room_reviews"] = Json::array();
        value["message"] = std::string(message);
        return replay_mixed_selection_property_edit_preparation(source, value);
    }
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
    if (assemblies) {
        value["version"] = 10;
        value["assembly_request"] = assemblies->request();
        if (!stair_review) value["stair_review_request"] = nullptr;
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
MixedSelectionPropertyEditPreparation prepare_mixed_selection_property_edit(const DocumentSnapshot& source,
    const std::optional<SelectionPropertyEditRequest>& ordinary,
    const std::optional<SelectionPresentationPropertyEditRequest>& presentation,
    const std::optional<RoofOpeningGroupProperties>& children, std::string_view message,
    std::span<const AssemblyInstancePropertyEditIntent> assemblies,
    const std::optional<StairPropertyReviewRequest>& reviewed) {
    if (assemblies.empty()) reject("assembly Properties requires a nonempty qualified target roster");
    const auto prepared = prepare_assembly_instance_property_edit(source, assemblies);
    return prepare_properties(source, ordinary, presentation, children, message,
        reviewed ? &*reviewed : nullptr, &prepared);
}
MixedSelectionPropertyEditPreparation prepare_mixed_selection_property_edit(const DocumentSnapshot& source,
    const std::optional<SelectionPropertyEditRequest>& ordinary,
    const std::optional<SelectionPresentationPropertyEditRequest>& presentation,
    const std::optional<RoofOpeningGroupProperties>& children, std::string_view message,
    std::span<const AssemblyInstancePropertyEditIntent> assemblies,
    const std::vector<PhaseSelectionPropertyIdentityAllocation>& allocations,
    const std::vector<PhaseStairReplacementDependencyDisposition>& dispositions) {
    if (!ordinary && !children) {
        if (!allocations.empty() || !dispositions.empty()) reject("phase allocations require an actual physical or skylight selection");
        if (assemblies.empty()) return prepare_properties(source, ordinary, presentation, children, message, nullptr);
        const auto prepared = prepare_assembly_instance_property_edit(source, assemblies);
        return prepare_properties(source, ordinary, presentation, children, message, nullptr, &prepared);
    }
    PhaseSelectionPropertyEditRequest request;
    if (ordinary) request.properties = *ordinary;
    request.allocations = allocations;
    request.stair_dependency_dispositions = dispositions;
    request.message = std::string(message);
    request.roof_opening_properties = children;
    const auto phase = prepare_phase_selection_property_edit(source, request);
    auto value = source_fields(source);
    value["version"] = phase.request().at("version") == 3 ? 12 : 11;
    value["phase_request"] = phase.request();
    value["presentation_request"] = presentation ? prepare_selection_presentation_property_edit(source, *presentation).request() : Json(nullptr);
    value["assembly_request"] = assemblies.empty() ? Json(nullptr) : prepare_assembly_instance_property_edit(source, assemblies).request();
    value["skylight_properties"] = children ? phase.request().at("skylight_properties") : Json(nullptr);
    value["roof_opening_members"] = children ? phase.request().at("roof_opening_members") : Json::array();
    value["room_reviews"] = Json::array();
    value["phase_room_reviews"] = Json::array();
    value["message"] = std::string(message);
    return replay_mixed_selection_property_edit_preparation(source, value);
}
MixedSelectionPropertyEditPreparation replay_mixed_selection_property_edit_preparation(const DocumentSnapshot& source, const Json& value) {
    (void)validate_mixed_selection_property_edit_intent(value);
    if (!value.at("room_reviews").empty() || ((value.at("version") == 11 || value.at("version") == 12) && !value.at("phase_room_reviews").empty()))
        reject("preparation cannot accept completed room decisions");
    auto stage = derive(source, value);
    return {source, value, std::move(stage.entities), std::move(stage.reviews), std::move(stage.phase), std::move(stage.phase_reviews)};
}
void require_mixed_selection_property_edit_source_current(const DocumentSnapshot& source,
    const MixedSelectionPropertyEditPreparation& prepared) {
    (void)validate_mixed_selection_property_edit_intent(prepared.intent());
    authenticate(source, prepared.intent());
}
namespace {
void protect_phase_source(const Entities& original, const Entities& candidate) {
    const auto scope = constraint_phase_scope(original);
    const auto baselines = retained_baselines(original);
    for (const auto& [id, entity] : original)
        if (scope.inactive_owner_ids.contains(id) || (baselines.contains(id) && entity.type != "assembly_model")) {
            const auto found = candidate.find(id);
            if (found == candidate.end() || !exact(entity, found->second)) reject("phase Properties changed a retained original physical owner");
        }
}
void review_phase_stage(const DocumentSnapshot& source, const Json& value, Stage& stage, bool complete) {
    if (!stage.phase) return;
    const auto& rows = value.at("phase_room_reviews");
    Ids pending;
    for (const auto& preview : stage.phase->wall_previews())
        for (const auto& [id, entity] : preview.replacement.deferred_room_constraints) { (void)entity; pending.insert(id); }
    std::size_t consumed{};
    for (std::size_t index = 0; index < stage.phase->phase_authorings().size(); ++index) {
        auto intent = decode_phase_constraint_authoring_intent(stage.phase->phase_authorings()[index]);
        if (intent.wall_replacement.is_null()) continue;
        const auto preview = inspect_phase_wall_replacement_rooms_on_composed_stage(source, intent, stage.entities);
        const bool supplied = consumed < rows.size() && rows[consumed].at("phase_authoring_index") == index;
        if (preview.needs_room_review && !supplied) {
            if (complete) reject("complete every final-stage phase room and relationship review");
            break;
        }
        if (supplied) {
            if (!preview.needs_room_review) reject("unrelated phase room completion");
            auto reviewed = decode_phase_wall_replacement_authoring(rows[consumed].at("wall_replacement"));
            auto immutable = reviewed;
            immutable.room_review_intent = nullptr; immutable.room_constraint_decisions.clear();
            if (encode_phase_wall_replacement_authoring(immutable).dump() != intent.wall_replacement.dump())
                reject("phase room review changed independently derived scalar or identity authority");
            intent.wall_replacement = rows[consumed].at("wall_replacement");
            ++consumed;
        }
        stage.entities = complete_phase_wall_replacement_rooms_on_composed_stage(source, intent, stage.entities, pending);
        for (const auto& [id, entity] : preview.replacement.deferred_room_constraints) { (void)entity; pending.erase(id); }
    }
    if (consumed != rows.size()) reject("phase room decisions must follow the exact required leaf prefix");
    protect_phase_source(source.entities(), stage.entities);
    require_selected_members_present(value, stage.entities, stage.retired_reviewed_rails, &*stage.phase, false);
}
ReplayedPhysicalWallRoomReview review_stage(const DocumentSnapshot& source, const Json& value, Stage stage, bool complete) {
    review_phase_stage(source, value, stage, true);
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
    if (stage.phase) protect_phase_source(source.entities(), result.entities);
    else protect_source(source.entities(), result.entities, false, stage.reviewed_erased);
    if (value.at("version") == 9 || value.at("version") == 10)
        require_selected_members_present(value, result.entities, stage.retired_reviewed_rails);
    if (stage.phase) {
        require_selected_members_present(value, result.entities, stage.retired_reviewed_rails, &*stage.phase, complete);
        for (const auto& [id, entity] : result.entities)
            if (!source.entities().contains(id) && is_physical_wall_room(entity) &&
                std::find(result.created_room_ids.begin(), result.created_room_ids.end(), id) == result.created_room_ids.end())
                result.created_room_ids.push_back(id);
        for (const auto& [id, entity] : source.entities())
            if (!result.entities.contains(id) && is_physical_wall_room(entity) &&
                std::find(result.retired_room_ids.begin(), result.retired_room_ids.end(), id) == result.retired_room_ids.end())
                result.retired_room_ids.push_back(id);
    }
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
    return preview_mixed_selection_property_room_stage(source, prepared, reviews, {});
}
DocumentSnapshot preview_mixed_selection_property_room_stage(const DocumentSnapshot& source,
    const MixedSelectionPropertyEditPreparation& prepared, const std::vector<Json>& reviews,
    const std::vector<Json>& phase_reviews) {
    require_mixed_selection_property_edit_source_current(source, prepared);
    if (reviews.size() > 32) reject("room review prefix budget exceeded");
    auto value = prepared.intent();
    value["room_reviews"] = reviews;
    if (value.at("version") == 11 || value.at("version") == 12) value["phase_room_reviews"] = phase_reviews;
    else if (!phase_reviews.empty()) reject("historical Properties has no phase room completion authority");
    auto stage = derive(source, value);
    auto requirements = stage.reviews;
    auto replayed = review_stage(source, value, std::move(stage), false);
    const MixedSelectionPropertyEditPreparation preview(source, std::move(value),
        std::move(replayed.entities), std::move(requirements));
    return preview.preview_snapshot();
}
DocumentSnapshot preview_mixed_selection_property_phase_room_stage(const DocumentSnapshot& source,
    const MixedSelectionPropertyEditPreparation& prepared, const std::vector<Json>& reviews) {
    require_mixed_selection_property_edit_source_current(source, prepared);
    if (prepared.intent().at("version") != 11 && prepared.intent().at("version") != 12) reject("phase room stage requires an explicit phase Properties dialect");
    auto value = prepared.intent(); value["phase_room_reviews"] = reviews;
    auto stage = derive(source, value);
    review_phase_stage(source, value, stage, false);
    const MixedSelectionPropertyEditPreparation preview(source, std::move(value), std::move(stage.entities),
        std::move(stage.reviews), std::move(stage.phase), std::move(stage.phase_reviews));
    return preview.preview_snapshot();
}
Json complete_mixed_selection_property_edit(const DocumentSnapshot& source,
    const MixedSelectionPropertyEditPreparation& prepared, const std::vector<Json>& reviews) {
    return complete_mixed_selection_property_edit(source, prepared, reviews, {});
}
Json complete_mixed_selection_property_edit(const DocumentSnapshot& source,
    const MixedSelectionPropertyEditPreparation& prepared, const std::vector<Json>& reviews,
    const std::vector<Json>& phase_reviews) {
    require_mixed_selection_property_edit_source_current(source, prepared);
    if (reviews.size() > 32) reject("room review group budget exceeded");
    auto value = prepared.intent();
    value["room_reviews"] = reviews;
    if (value.at("version") == 11 || value.at("version") == 12) value["phase_room_reviews"] = phase_reviews;
    else if (!phase_reviews.empty()) reject("historical Properties has no phase room completion authority");
    (void)replay_mixed_selection_property_edit(source, value);
    return validate_mixed_selection_property_edit_intent(value);
}
bool mixed_selection_property_edit_has_geometry(const Json& value) {
    validate(value);
    return value.at("version") == 9 || value.at("version") == 10 || value.at("version") == 11 || value.at("version") == 12 ||
        !value.at("ordinary_request").is_null() || !value.at("skylight_properties").is_null();
}
bool mixed_selection_property_edit_active_phase_policy(const Json& value) {
    validate(value);
    return true;
}
std::vector<Json> mixed_selection_property_edit_room_reviews(const Json& value) {
    validate(value);
    return value.at("room_reviews").get<std::vector<Json>>();
}
std::vector<Json> mixed_selection_property_edit_phase_room_reviews(const Json& value) {
    validate(value);
    std::vector<Json> result;
    if (value.at("version") == 11 || value.at("version") == 12) for (const auto& row : value.at("phase_room_reviews"))
        result.push_back(decode_phase_wall_replacement_authoring(row.at("wall_replacement")).room_review_intent);
    return result;
}
bool mixed_selection_property_edit_retires_dependencies(const Json& value) {
    validate(value);
    if (value.at("version") == 11 || value.at("version") == 12) return std::any_of(
        value.at("phase_request").at("stair_dependency_dispositions").begin(),
        value.at("phase_request").at("stair_dependency_dispositions").end(), [](const auto& choice) { return choice.at("action") == "retire"; });
    if ((value.at("version") == 9 || value.at("version") == 10) && !value.at("stair_review_request").is_null()) {
        const auto request = decode_stair_property_review_request(value.at("stair_review_request"));
        return std::any_of(request.dependency_dispositions.begin(), request.dependency_dispositions.end(),
            [](const auto& choice) { return choice.action == PhaseStairReplacementDependencyAction::retire; });
    }
    return false;
}
bool mixed_selection_property_edit_has_phase_replacements(const Json& value) {
    validate(value);
    return (value.at("version") == 11 || value.at("version") == 12) && !value.at("phase_request").at("allocations").empty();
}
std::optional<Revision> mixed_selection_property_edit_source_saved_revision(const Json& value) {
    validate(value);
    return value.at("source_saved_revision").is_null() ? std::nullopt :
        std::optional<Revision>{revision(value.at("source_saved_revision"))};
}
} // namespace sketch
