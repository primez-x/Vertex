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
    const bool slab_carry = std::any_of(request.slabs.begin(), request.slabs.end(), [](const auto& slab) { return slab.coordinate_hosted_elevation; });
    result["version"] = slab_carry ? 5 : vertical ? 4 : !request.stair_profiles.empty() ? 3 : request.structural_profiles.empty() ? 1 : 2;
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
    if (vertical || slab_carry || !request.structural_profiles.empty() || !request.stair_profiles.empty())
        lane("structural_profiles", request.structural_profiles, encode_structural_profile_edit_intent);
    if (vertical || slab_carry || !request.stair_profiles.empty())
        lane("stair_profiles", request.stair_profiles, encode_stair_profile_edit_intent);
    if (request.unchanged_policy != SelectionPropertyUnchangedPolicy::retain &&
        request.unchanged_policy != SelectionPropertyUnchangedPolicy::reject) reject("unsupported unchanged policy");
    result["unchanged_policy"] = request.unchanged_policy == SelectionPropertyUnchangedPolicy::retain ? "retain" : "reject";
    return validate_selection_property_edit_request(result);
}
// Elevation/carry scalar dialects validate their own lanes directly. A historical wrapper cannot
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
        if (assembly.at("version")!=1) reject("historical Properties has no instance profile geometry authority");
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
bool phase_selection_dialect(const Json& value) {
    return value.at("version") == 11 || value.at("version") == 12 ||
        value.at("version") == 13 || value.at("version") == 15 || value.at("version") == 21 ||
        value.at("version") == 24 || value.at("version") == 27;
}
bool phase_room_dialect(const Json& value) {
    if (value.at("version")==43) return true;
    if (value.at("version")==39 || value.at("version")==40 || value.at("version")==41 || value.at("version")==42) return true;
    return phase_selection_dialect(value) || value.at("version") == 14 || value.at("version") == 16 ||
        value.at("version") == 17 || value.at("version") == 22 || value.at("version") == 23 ||
        value.at("version") == 25 || value.at("version") == 26 || value.at("version") == 28 ||
        value.at("version") == 29 || value.at("version") == 32 || value.at("version") == 33 || value.at("version") == 34 || value.at("version") == 35 || value.at("version") == 36 || value.at("version") == 37 || value.at("version") == 38;
}
bool component_dialect(const Json& value) {
    if (value.at("version")==43) return true;
    if (value.at("version")==39 || value.at("version")==40 || value.at("version")==41 || value.at("version")==42) return true;
    return value.at("version") == 14 || value.at("version") == 16 || value.at("version") == 17 ||
        value.at("version") == 22 || value.at("version") == 23 || value.at("version") == 25 ||
        value.at("version") == 26 || value.at("version") == 28 || value.at("version") == 29 || value.at("version") == 32 || value.at("version") == 33 || value.at("version") == 34 || value.at("version") == 35 || value.at("version") == 36 || value.at("version") == 37 || value.at("version") == 38;
}
bool coordinated_component_dialect(const Json& value) {
    return value.at("version") == 16 || value.at("version") == 17 ||
        value.at("version") == 22 || value.at("version") == 23 || value.at("version") == 25 ||
        value.at("version") == 26 || value.at("version") == 28 || value.at("version") == 29 || value.at("version") == 32 || value.at("version") == 33 || value.at("version") == 34 || value.at("version") == 35 || value.at("version") == 36 || value.at("version") == 37 || value.at("version") == 38;
}
bool vertical_scalar(const Json& scalar) {
    return !scalar.is_null() && (scalar.at("version") == 4 || scalar.at("version") == 5);
}
MemberKey component_key(const AssemblyInstancePropertyEditTarget& target) {
    MemberKey key{"assembly", target.catalog_id, target.instance_id, target.document_entity_id.value_or("")};
    key.insert(key.end(), target.part_path.begin(), target.part_path.end());
    return key;
}
Json slab_allocation_json(const std::optional<ComponentSlabDirectionalResizeAllocation>& allocation) {
    if (!allocation) return nullptr;
    Json rows=Json::array();
    for (const auto& [binding,proposed]:allocation->hosted_instance_identities)
        rows.push_back({{"catalog_id",binding.first},{"instance_id",binding.second},{"proposed_instance_id",proposed}});
    return {{"identities",allocation->identities},{"hosted_instance_identities",std::move(rows)},
        {"dimension_identities",allocation->dimension_identities}};
}
std::optional<ComponentSlabDirectionalResizeAllocation> slab_allocation(const Json& value) {
    if (value.is_null()) return std::nullopt;
    fields(value,{"identities","hosted_instance_identities","dimension_identities"});
    ComponentSlabDirectionalResizeAllocation result;
    for (const auto* key:{"identities","dimension_identities"}) {
        const auto& rows=value.at(key);
        if (!rows.is_object() || rows.size()>4096) reject("slab allocation map exceeds its source roster bound");
        auto& map=std::string_view(key)=="identities" ? result.identities : result.dimension_identities;
        for (const auto& [original,proposed]:rows.items()) map.emplace(identity(Json(original)),identity(proposed));
    }
    const auto& rows=value.at("hosted_instance_identities");
    if (!rows.is_array() || rows.size()>4096) reject("slab hosted allocation exceeds its source roster bound");
    std::optional<PhaseSlabReplacementHostedInstanceKey> previous;
    for (const auto& row:rows) {
        fields(row,{"catalog_id","instance_id","proposed_instance_id"});
        const PhaseSlabReplacementHostedInstanceKey key{identity(row.at("catalog_id")),identity(row.at("instance_id"))};
        if (previous && *previous>=key) reject("slab hosted allocations must be sorted unique");
        result.hosted_instance_identities.emplace(key,identity(row.at("proposed_instance_id")));
        previous=key;
    }
    if (slab_allocation_json(result).dump()!=value.dump()) reject("slab allocation must retain its canonical exact fields");
    return result;
}
Json structural_allocation_json(const std::optional<ComponentStructuralDirectionalResizeAllocation>& allocation) {
    if (!allocation) return nullptr;
    Json rows=Json::array();
    for (const auto& [binding,proposed]:allocation->hosted_instance_identities)
        rows.push_back({{"catalog_id",binding.first},{"instance_id",binding.second},{"proposed_instance_id",proposed}});
    return {{"identities",allocation->identities},{"hosted_instance_identities",std::move(rows)},
        {"dimension_identities",allocation->dimension_identities}};
}
std::optional<ComponentStructuralDirectionalResizeAllocation> structural_allocation(const Json& value) {
    // The identity wire shape is shared; only the structural source inventory
    // assigns meaning to these keys. No slab leaves enter structural replay.
    const auto decoded=slab_allocation(value);
    if (!decoded) return std::nullopt;
    if (decoded->dimension_identities.size()>1000 || decoded->identities.size()+
        decoded->hosted_instance_identities.size()+decoded->dimension_identities.size()>4096)
        reject("structural allocation exceeds its combined source roster bound");
    return ComponentStructuralDirectionalResizeAllocation{decoded->identities,
        decoded->hosted_instance_identities,decoded->dimension_identities};
}
std::vector<StructuralDirectionalResizeRequest> directional_structural(const Json& rows,bool allow_empty=false) {
    if (!rows.is_array() || (rows.empty() && !allow_empty) || rows.size()>1000)
        reject("structural resize requires a nonempty bounded complete roster");
    std::vector<StructuralDirectionalResizeRequest> result;
    std::string previous;
    const auto number=[](const Json& value) {
        if (!value.is_number()) reject("structural axis coordinates and factors must be numeric");
        const auto result=value.get<double>();
        if (!std::isfinite(result)) reject("structural axis coordinates and factors must be finite");
        return result;
    };
    for (const auto& row:rows) {
        fields(row,{"object_id","sx","sy","anchor_m","frame_rotation_radians"});
        StructuralDirectionalResizeRequest request;
        request.object_id=identity(row.at("object_id"));
        if (!previous.empty() && previous>=request.object_id) reject("structural axis requests must be sorted unique owners");
        previous=request.object_id;
        request.sx=number(row.at("sx"));request.sy=number(row.at("sy"));
        if (request.sx<=0 || request.sy<=0) reject("structural axis factors must be positive");
        const auto& anchor=row.at("anchor_m");
        if (!anchor.is_array() || anchor.size()!=2) reject("structural axis anchor must have exactly two coordinates");
        request.anchor_m={number(anchor.at(0)),number(anchor.at(1))};
        request.frame_rotation_radians=number(row.at("frame_rotation_radians"));
        result.push_back(std::move(request));
    }
    return result;
}
std::vector<StairDirectionalResizeRequest> directional_stairs(const Json& rows) {
    if (!rows.is_array() || rows.empty() || rows.size()>1000) reject("stair resize requires a nonempty bounded complete roster");
    std::vector<StairDirectionalResizeRequest> result;
    std::string previous;
    const auto number=[](const Json& value) {
        if (!value.is_number() || !std::isfinite(value.get<double>())) reject("stair axis coordinates and factors must be finite numbers");
        return value.get<double>();
    };
    for (const auto& row:rows) {
        fields(row,{"stair_id","sx","sy","anchor_m","frame_rotation_radians"});
        StairDirectionalResizeRequest request;
        request.stair_id=identity(row.at("stair_id"));
        if (!previous.empty() && previous>=request.stair_id) reject("stair axis requests must be sorted unique actual owners");
        previous=request.stair_id;
        request.sx=number(row.at("sx"));request.sy=number(row.at("sy"));
        if (request.sx<=0 || request.sy<=0) reject("stair axis factors must be positive");
        const auto& anchor=row.at("anchor_m");
        if (!anchor.is_array() || anchor.size()!=2) reject("stair axis anchor must have exactly two coordinates");
        request.anchor_m={number(anchor.at(0)),number(anchor.at(1))};
        request.frame_rotation_radians=number(row.at("frame_rotation_radians"));
        result.push_back(std::move(request));
    }
    return result;
}
std::vector<SlabGeometryEditIntent> directional_slabs(const Json& rows,bool allow_empty=false) {
    if (!rows.is_array() || (rows.empty() && !allow_empty) || rows.size()>1000) reject("slab resize requires a nonempty bounded complete roster");
    std::vector<SlabGeometryEditIntent> result;
    std::string previous;
    for (const auto& row:rows) {
        const auto intent=decode_slab_geometry_edit_intent(row);
        if ((row.at("version")!=4 && row.at("version")!=5) || intent.kind!=SlabGeometryEditKind::resize_plan ||
            !intent.coordinate_world_hosted_geometry || (!previous.empty() && previous>=intent.slab_id) ||
            encode_slab_geometry_edit_intent(intent).dump()!=row.dump())
            reject("slab resize requires canonical sorted directional leaves four or five");
        previous=intent.slab_id;
        result.push_back(intent);
    }
    return result;
}
std::vector<RoofOpeningDirectionalResizeRequest> directional_roof_openings(const Json& rows,bool allow_empty=false) {
    if (!rows.is_array() || (rows.empty() && !allow_empty) || rows.size()>1000)
        reject("skylight resize requires a nonempty bounded complete roster");
    std::vector<RoofOpeningDirectionalResizeRequest> result;
    std::optional<std::pair<std::string,std::string>> previous;
    for (const auto& row:rows) {
        fields(row,{"roof_id","opening_id","sx","sy","anchor_m","frame_rotation_radians"});
        RoofOpeningDirectionalResizeRequest request;
        request.target={identity(row.at("roof_id")),identity(row.at("opening_id"))};
        const std::pair key{request.target.roof_id,request.target.opening_id};
        if (previous && *previous>=key) reject("skylight axis requests must be sorted unique qualified children");
        previous=key;
        const auto number=[](const Json& value) {
            if (!value.is_number()) reject("skylight axis coordinates and factors must be numeric");
            const auto result=value.get<double>();
            if (!std::isfinite(result)) reject("skylight axis coordinates and factors must be finite");
            return result;
        };
        request.sx=number(row.at("sx"));request.sy=number(row.at("sy"));
        if (request.sx<=0 || request.sy<=0) reject("skylight axis factors must be positive");
        const auto& anchor=row.at("anchor_m");
        if (!anchor.is_array() || anchor.size()!=2) reject("skylight axis anchor must have exactly two coordinates");
        request.anchor_m={number(anchor.at(0)),number(anchor.at(1))};
        request.frame_rotation_radians=number(row.at("frame_rotation_radians"));
        result.push_back(std::move(request));
    }
    return result;
}
// Read the already validated historical roster without changing its dialect or
// translating its authority into a synthetic ordinary request.
void historical_selected_members(const Json& value, std::set<MemberKey>& selected) {
    const auto select = [&](MemberKey key) {
        if (!selected.insert(std::move(key)).second || selected.size() > 1000)
            reject("duplicate or over-budget selected member");
    };
    const auto& scalar = phase_selection_dialect(value) ? value.at("phase_request").at("scalar_request") : value.at("ordinary_request");
    if (!scalar.is_null()) for (const auto& row : scalar.at("selected"))
        select({"entity", identity(row.at("entity_id")), ""});
    const auto& presentation = value.at("presentation_request");
    if (!presentation.is_null()) for (const auto& row : presentation.at("selected")) {
        if (row.at("family") == "reference") select({"entity", identity(row.at("entity_id")), ""});
        else select({row.at("family").get<std::string>(), identity(row.at("owner_id")), identity(row.at("child_id"))});
    }
    if ((value.at("version") == 9 || value.at("version") == 10) && !value.at("stair_review_request").is_null())
        for (const auto& row : value.at("stair_review_request").at("profiles"))
            select({"entity", identity(row.at("object_id")), ""});
    for (const auto& row : value.at("roof_opening_members"))
        select({"skylight", identity(row.at("roof_id")), identity(row.at("opening_id"))});
}
void validate(const Json& value) {
    Budget budget; budget.visit(value);
    if (value.is_object() && value.contains("version") && (value.at("version")==39 || value.at("version")==40 || value.at("version")==41 || value.at("version")==42 || value.at("version")==43)) {
        const bool stair=value.at("version")==43;
        const bool combined=value.at("version")==42;
        const bool skylight=value.at("version")==40;
        const bool structural=value.at("version")==41;
        if (stair) fields(value,{"version","expected_revision","source_snapshot_digest","source_authoring_digest",
            "source_entities_digest","source_saved_revision","component_request","component_identity_reservations",
            "stair_axis_requests","room_reviews","phase_room_reviews","message"});
        else if (combined) fields(value,{"version","expected_revision","source_snapshot_digest","source_authoring_digest",
            "source_entities_digest","source_saved_revision","component_request","component_identity_reservations",
            "slab_geometry","slab_allocation","structural_axis_requests","structural_allocation",
            "roof_opening_axis_requests","room_reviews","phase_room_reviews","message"});
        else if (structural) fields(value,{"version","expected_revision","source_snapshot_digest","source_authoring_digest",
            "source_entities_digest","source_saved_revision","component_request","component_identity_reservations",
            "structural_axis_requests","structural_allocation","room_reviews","phase_room_reviews","message"});
        else if (skylight) fields(value,{"version","expected_revision","source_snapshot_digest","source_authoring_digest",
            "source_entities_digest","source_saved_revision","component_request","component_identity_reservations",
            "roof_opening_axis_requests","room_reviews","phase_room_reviews","message"});
        else fields(value,{"version","expected_revision","source_snapshot_digest","source_authoring_digest",
            "source_entities_digest","source_saved_revision","component_request","component_identity_reservations",
            "slab_geometry","slab_allocation","room_reviews","phase_room_reviews","message"});
        if (!value.at("version").is_number_integer()) reject("Properties dialect must be an integer");
        (void)revision(value.at("expected_revision"));
        for (const auto* key:{"source_snapshot_digest","source_authoring_digest","source_entities_digest"}) digest(value.at(key));
        if (!value.at("source_saved_revision").is_null()) (void)revision(value.at("source_saved_revision"));
        const auto component=validate_phase_assembly_instance_property_edit_request(value.at("component_request"));
        if (component.at("version")!=7 && component.at("version")!=8 && component.at("version")!=9)
            reject("mixed resize requires component geometry request seven, eight or nine");
        for (const auto* key:{"expected_revision","source_snapshot_digest","source_authoring_digest","source_entities_digest","source_saved_revision"})
            if (component.at(key).dump()!=value.at(key).dump()) reject("component resize fences differ from complete original source");
        const auto authoring=decode_phase_assembly_instance_property_edit_authoring(component.at("authoring"));
        std::size_t physical_count{};
        if (combined) {
            const auto slabs=directional_slabs(value.at("slab_geometry"),true);
            const auto structures=directional_structural(value.at("structural_axis_requests"),true);
            const auto openings=directional_roof_openings(value.at("roof_opening_axis_requests"),true);
            if (static_cast<unsigned>(!slabs.empty())+static_cast<unsigned>(!structures.empty())+
                static_cast<unsigned>(!openings.empty())<2)
                reject("combined resize requires at least two nonempty physical family lists before unit elimination");
            physical_count=slabs.size()+structures.size()+openings.size();
            if ((slabs.empty() && !value.at("slab_allocation").is_null()) ||
                (structures.empty() && !value.at("structural_allocation").is_null()))
                reject("absent physical families cannot allocate identities");
        } else physical_count=stair ? directional_stairs(value.at("stair_axis_requests")).size() :
            structural ? directional_structural(value.at("structural_axis_requests")).size() :
            skylight ? directional_roof_openings(value.at("roof_opening_axis_requests")).size() :
            directional_slabs(value.at("slab_geometry")).size();
        if (authoring.intents.empty() || authoring.intents.size()+physical_count>1000) reject("mixed resize complete selected roster exceeds bound");
        for (const auto& intent:authoring.intents)
            if (!intent.target.document_entity_id || !intent.properties.empty() || !intent.materials.empty() ||
                !intent.quantities.empty() || intent.pose.operation!=AssemblyInstancePropertyPatchOperation::unchanged ||
                intent.pose.x_metres || intent.pose.y_metres || intent.pose.z_metres || intent.pose.rotation_radians ||
                intent.pose.scale || intent.pose.vertical_scale || intent.pose.mirrored_y ||
                intent.profile_geometry.operation==AssemblyInstancePropertyPatchOperation::unchanged)
                reject("mixed resize admits independent document component geometry only");
        if (structural || combined) (void)structural_allocation(value.at("structural_allocation"));
        if ((!stair && !skylight && !structural) || combined) (void)slab_allocation(value.at("slab_allocation"));
        const auto& reservations=value.at("component_identity_reservations");
        if (!reservations.is_array() || reservations.size()>4096) reject("component reservation inventory exceeds bound");
        std::optional<std::string> previous;
        for (const auto& row:reservations) {
            if (!row.is_string()) reject("component reservation must be a string");
            const auto& token=row.get_ref<const std::string&>();
            if (token.empty() || token.size()>4096 || token.find('\0')!=std::string::npos || (previous && *previous>=token))
                reject("component reservations must be portable sorted unique identities");
            previous=token;
        }
        for (const auto* key:{"room_reviews","phase_room_reviews"})
            if (!value.at(key).is_array() || !value.at(key).empty()) reject("mixed directional resize has no room review authority");
        if (!value.at("message").is_string() || value.at("message").get_ref<const std::string&>().size()>1024 ||
            value.at("message").get_ref<const std::string&>().find('\0')!=std::string::npos) reject("invalid message");
        if (value.dump().size()>byte_limit) reject("wire byte budget exceeded");
        return;
    }
    if (value.is_object() && value.contains("version") && component_dialect(value)) {
        const bool reviewed_initial_geometry = value.at("version") == 38;
        const bool initialize_deformed_topology = value.at("version") == 37;
        const bool reviewed_topology = value.at("version") == 36;
        const bool initialize_topology = value.at("version") == 35;
        const bool topology_geometry = value.at("version") == 34;
        const bool profile_geometry = value.at("version") == 32 || topology_geometry || initialize_topology || reviewed_topology || initialize_deformed_topology || reviewed_initial_geometry;
        const bool component_dimensions = value.at("version") == 33;
        const bool complete_component = profile_geometry || component_dimensions;
        const auto base_version = complete_component && value.contains("base_intent") && value.at("base_intent").is_object()
            ? value.at("base_intent").value("version",Json()) : Json();
        const bool stair_dimensions = value.at("version") == 22 || value.at("version") == 23 || (complete_component && base_version == 21);
        const bool family_dimensions = value.at("version") == 28 || value.at("version") == 29 || (complete_component && base_version == 27);
        const bool wall_dimensions = value.at("version") == 25 || value.at("version") == 26 || family_dimensions || (complete_component && base_version == 24);
        const bool presentation_carry = value.at("version") == 17 || value.at("version") == 23 ||
            value.at("version") == 26 || value.at("version") == 29 || complete_component;
        const bool coordinated = coordinated_component_dialect(value);
        if (coordinated) {
            if (component_dimensions) {
                fields(value, {"version", "expected_revision", "source_snapshot_digest", "source_authoring_digest",
                    "source_entities_digest", "source_saved_revision", "component_request", "base_intent",
                    "room_reviews", "phase_room_reviews", "message", "coordinate_component_hosts", "component_identity_reservations",
                    "carry_component_presentation", "complete_component_dimensions"});
                if (!value.at("complete_component_dimensions").is_boolean() || value.at("complete_component_dimensions") != true ||
                    !value.at("carry_component_presentation").is_boolean() || value.at("carry_component_presentation") != true)
                    reject("component dimension Properties requires explicit dimension and presentation completion");
            } else if (profile_geometry) {
                fields(value, {"version", "expected_revision", "source_snapshot_digest", "source_authoring_digest",
                    "source_entities_digest", "source_saved_revision", "component_request", "base_intent",
                    "room_reviews", "phase_room_reviews", "message", "coordinate_component_hosts", "component_identity_reservations",
                    "carry_component_presentation", "complete_instance_profile_geometry"});
                if (value.at("complete_instance_profile_geometry") != true || !value.at("complete_instance_profile_geometry").is_boolean() ||
                    value.at("carry_component_presentation") != true || !value.at("carry_component_presentation").is_boolean())
                    reject("profile geometry Properties requires explicit geometry and presentation completion");
            } else if (presentation_carry) {
                if (family_dimensions) fields(value, {"version", "expected_revision", "source_snapshot_digest", "source_authoring_digest",
                    "source_entities_digest", "source_saved_revision", "component_request", "base_intent",
                    "room_reviews", "phase_room_reviews", "message", "coordinate_component_hosts", "component_identity_reservations",
                    "carry_component_presentation", "complete_stair_component_dimensions", "complete_wall_component_dimensions",
                    "complete_roof_component_dimensions", "complete_slab_component_dimensions", "complete_structural_component_dimensions"});
                else if (wall_dimensions) fields(value, {"version", "expected_revision", "source_snapshot_digest", "source_authoring_digest",
                    "source_entities_digest", "source_saved_revision", "component_request", "base_intent",
                    "room_reviews", "phase_room_reviews", "message", "coordinate_component_hosts", "component_identity_reservations",
                    "carry_component_presentation", "complete_stair_component_dimensions", "complete_wall_component_dimensions"});
                else if (stair_dimensions) fields(value, {"version", "expected_revision", "source_snapshot_digest", "source_authoring_digest",
                    "source_entities_digest", "source_saved_revision", "component_request", "base_intent",
                    "room_reviews", "phase_room_reviews", "message", "coordinate_component_hosts", "component_identity_reservations",
                    "carry_component_presentation", "complete_stair_component_dimensions"});
                else fields(value, {"version", "expected_revision", "source_snapshot_digest", "source_authoring_digest",
                    "source_entities_digest", "source_saved_revision", "component_request", "base_intent",
                    "room_reviews", "phase_room_reviews", "message", "coordinate_component_hosts", "component_identity_reservations",
                    "carry_component_presentation"});
                if (!value.at("carry_component_presentation").is_boolean() || value.at("carry_component_presentation") != true)
                    reject("component presentation Properties requires literal carry_component_presentation:true");
            } else if (family_dimensions) fields(value, {"version", "expected_revision", "source_snapshot_digest", "source_authoring_digest",
                "source_entities_digest", "source_saved_revision", "component_request", "base_intent",
                "room_reviews", "phase_room_reviews", "message", "coordinate_component_hosts", "component_identity_reservations",
                "complete_stair_component_dimensions", "complete_wall_component_dimensions", "complete_roof_component_dimensions",
                "complete_slab_component_dimensions", "complete_structural_component_dimensions"});
            else if (wall_dimensions) fields(value, {"version", "expected_revision", "source_snapshot_digest", "source_authoring_digest",
                "source_entities_digest", "source_saved_revision", "component_request", "base_intent",
                "room_reviews", "phase_room_reviews", "message", "coordinate_component_hosts", "component_identity_reservations",
                "complete_stair_component_dimensions", "complete_wall_component_dimensions"});
            else if (stair_dimensions) fields(value, {"version", "expected_revision", "source_snapshot_digest", "source_authoring_digest",
                "source_entities_digest", "source_saved_revision", "component_request", "base_intent",
                "room_reviews", "phase_room_reviews", "message", "coordinate_component_hosts", "component_identity_reservations",
                "complete_stair_component_dimensions"});
            else fields(value, {"version", "expected_revision", "source_snapshot_digest", "source_authoring_digest",
                "source_entities_digest", "source_saved_revision", "component_request", "base_intent",
                "room_reviews", "phase_room_reviews", "message", "coordinate_component_hosts", "component_identity_reservations"});
            if (!complete_component && stair_dimensions && (!value.at("complete_stair_component_dimensions").is_boolean() ||
                    value.at("complete_stair_component_dimensions") != true))
                reject("stair dimension component Properties requires literal complete_stair_component_dimensions:true");
            if (!complete_component && wall_dimensions && (!value.at("complete_wall_component_dimensions").is_boolean() ||
                    value.at("complete_wall_component_dimensions") != true ||
                    !value.at("complete_stair_component_dimensions").is_boolean()))
                reject("wall dimension component Properties requires literal wall completion and explicit stair permission");
            if (!complete_component && family_dimensions) {
                bool enabled = false;
                for (const auto* key : {"complete_roof_component_dimensions", "complete_slab_component_dimensions",
                        "complete_structural_component_dimensions"}) {
                    if (!value.at(key).is_boolean()) reject("component family dimension permissions must be explicit Booleans");
                    enabled = enabled || value.at(key) == true;
                }
                if (!enabled) reject("component family dimension Properties requires an enabled family permission");
            }
            if (!value.at("coordinate_component_hosts").is_boolean() || value.at("coordinate_component_hosts") != true)
                reject("coordinated component Properties requires literal coordinate_component_hosts:true");
            const auto& reservations = value.at("component_identity_reservations");
            // Allocation codecs bound roots/registries; this receipt also
            // retains actual render aliases, including escaped alias tokens.
            if (!reservations.is_array() || reservations.size() > 4096)
                reject("component reservation inventory exceeds its bounded allocation roster");
            std::optional<std::string> previous;
            for (const auto& id : reservations) {
                if (!id.is_string() || id.get_ref<const std::string&>().empty() ||
                    id.get_ref<const std::string&>().size() > 4096 ||
                    id.get_ref<const std::string&>().find('\0') != std::string::npos)
                    reject("component reservation identity must be an exact nonempty string");
                const auto& token = id.get_ref<const std::string&>();
                if (previous && *previous >= token) reject("component reservations must be sorted unique");
                previous = token;
            }
        } else {
            fields(value, {"version", "expected_revision", "source_snapshot_digest", "source_authoring_digest",
                "source_entities_digest", "source_saved_revision", "component_request", "base_intent",
                "room_reviews", "phase_room_reviews", "message"});
        }
        if (!value.at("version").is_number_integer()) reject("Properties dialect must be an integer");
        const auto component = validate_phase_assembly_instance_property_edit_request(value.at("component_request"));
        if (component.at("version") != (reviewed_initial_geometry ? 9 : initialize_deformed_topology ? 8 : reviewed_topology ? 7 : initialize_topology ? 6 : topology_geometry ? 5 : component_dimensions ? 4 : profile_geometry ? 3 : presentation_carry ? 2 : 1))
            reject("component request authority differs from its explicit Properties dialect");
        const auto fence = [&](const Json& lane) {
            for (const auto* key : {"expected_revision", "source_snapshot_digest", "source_authoring_digest", "source_entities_digest", "source_saved_revision"})
                if (lane.at(key).dump() != value.at(key).dump()) reject("leaf source fences differ from complete original source");
        };
        fence(component);
        if (!value.at("message").is_string() || value.at("message").get_ref<const std::string&>().size() > 1024 ||
            value.at("message").get_ref<const std::string&>().find('\0') != std::string::npos) reject("invalid message");
        const auto& base = value.at("base_intent");
        if (!complete_component && coordinated && (!presentation_carry || stair_dimensions || wall_dimensions) && base.is_null())
            reject("coordinated component Properties requires a sealed base");
        std::set<MemberKey> selected;
        if (!base.is_null()) {
            if (!base.is_object() || !base.contains("version") || !base.at("version").is_number_integer() ||
                (complete_component ? !((base.at("version") >= 5 && base.at("version") <= 13) || base.at("version") == 15 ||
                    base.at("version") == 21 || base.at("version") == 24 || base.at("version") == 27) :
                    family_dimensions ? base.at("version") != 27 : wall_dimensions ? base.at("version") != 24 : stair_dimensions ? base.at("version") != 21 :
                    !((base.at("version") >= 5 && base.at("version") <= 13) || (coordinated && base.at("version") == 15))))
                reject("component base must retain its explicit noncomponent Properties dialect");
            if (base.contains("assembly_request") && !base.at("assembly_request").is_null())
                reject("component request is the sole assembly Properties authority");
            validate(base);
            fence(base);
            if (!complete_component && wall_dimensions && value.at("complete_stair_component_dimensions") !=
                    base.at("phase_request").at("complete_stair_component_dimensions"))
                reject("component stair dimension permission differs from its sealed wall Properties base");
            if (!complete_component && family_dimensions)
                for (const auto* key : {"complete_roof_component_dimensions", "complete_slab_component_dimensions",
                        "complete_structural_component_dimensions"})
                    if (value.at(key) != base.at("phase_request").at(key))
                        reject("component family dimension permission differs from its sealed Properties base");
            if (base.at("message").dump() != value.at("message").dump() || !base.at("room_reviews").empty() ||
                (base.contains("phase_room_reviews") && !base.at("phase_room_reviews").empty()))
                reject("component base must retain the same message and no completed room decisions");
            historical_selected_members(base, selected);
        }
        const auto authoring = decode_phase_assembly_instance_property_edit_authoring(component.at("authoring"));
        if (authoring.intents.empty()) reject("component Properties requires a nonempty selected roster");
        for (const auto& intent : authoring.intents)
            if (!selected.insert(component_key(intent.target)).second || selected.size() > 1000)
                reject("duplicate or over-budget selected member");
        const auto& reviews = value.at("room_reviews");
        if (!reviews.is_array() || reviews.size() > 32) reject("room review group budget exceeded");
        for (const auto& row : reviews) {
            const auto review = decode_physical_wall_room_review_intent(row);
            if (encode_physical_wall_room_review_intent(review).dump() != row.dump()) reject("room review must be canonical");
            if (!review.context_plane_selection || !review.selected_wall_id.empty() ||
                !review.selected_dimension_placements.empty() || review.selected_dimension_source)
                reject("scalar Properties requires context/plane review without dimension movement authority");
        }
        const auto& phase_reviews = value.at("phase_room_reviews");
        if (!phase_reviews.is_array() || phase_reviews.size() > 1000) reject("phase room review budget exceeded");
        if ((base.is_null() || !phase_room_dialect(base)) && !phase_reviews.empty())
            reject("component base has no phase room completion authority");
        std::optional<std::size_t> previous;
        for (const auto& row : phase_reviews) {
            fields(row, {"phase_authoring_index", "wall_replacement"});
            const auto index = revision(row.at("phase_authoring_index"));
            if (index >= 1000 || (previous && index <= *previous)) reject("phase room reviews must be an ordered unique leaf roster");
            previous = static_cast<std::size_t>(index);
            const auto leaf = decode_phase_wall_replacement_authoring(row.at("wall_replacement"));
            const bool qualified_walls = coordinated && (base.at("version") == 15 ||
                (base.at("version") == 21 && base.at("phase_request").at("complete_wall_hosted_instances") == true));
            if (wall_dimensions && (row.at("wall_replacement").at("version") != 12 ||
                    !leaf.complete_hosted_instances || !leaf.complete_component_dimensions))
                reject("wall dimension Properties requires explicit leaf twelve room completion");
            if (qualified_walls &&
                (row.at("wall_replacement").at("version") != 11 || !leaf.complete_hosted_instances))
                reject("qualified wall Properties requires explicit leaf eleven room completion");
            if (coordinated && (base.at("version") == 15 || base.at("version") == 21 || base.at("version") == 24 || base.at("version") == 27) &&
                !vertical_scalar(base.at("phase_request").at("scalar_request")) && leaf.complete_vertical_profiles)
                reject("historical Properties has no elevation room completion authority");
            if (base.at("version") == 11 && leaf.complete_vertical_profiles)
                reject("historical Properties has no elevation room completion authority");
            if (encode_phase_wall_replacement_authoring(leaf).dump() != row.at("wall_replacement").dump() || leaf.room_review_intent.is_null())
                reject("phase room completion requires canonical explicit decisions");
        }
        if (value.dump().size() > byte_limit) reject("wire byte budget exceeded");
        return;
    }
    if (value.is_object() && value.contains("version") && phase_selection_dialect(value)) {
        const bool stair_dimensions = value.at("version") == 21;
        const bool family_dimensions = value.at("version") == 27;
        const bool wall_dimensions = value.at("version") == 24 || family_dimensions;
        const bool slab_carry = value.at("version") == 13;
        const bool historical_vertical = value.at("version") == 12 || slab_carry;
        if (!value.at("version").is_number_integer()) reject("Properties dialect must be an integer");
        fields(value, {"version", "expected_revision", "source_snapshot_digest", "source_authoring_digest",
            "source_entities_digest", "source_saved_revision", "phase_request", "presentation_request",
            "skylight_properties", "roof_opening_members", "room_reviews", "phase_room_reviews", "message", "assembly_request"});
        const auto phase = validate_phase_selection_property_edit_request(value.at("phase_request"));
        if (family_dimensions ? phase.at("version") != 8 : wall_dimensions ? phase.at("version") != 7 : stair_dimensions ? phase.at("version") != 6 :
            value.at("version") == 15 ? phase.at("version") != 5 :
            historical_vertical ? phase.at("version") != (slab_carry ? 4 : 3) :
            (phase.at("version") != 1 && phase.at("version") != 2))
            reject("Properties requires its explicit phase request dialect");
        const bool qualified_walls = value.at("version") == 15 ||
            (stair_dimensions && phase.at("complete_wall_hosted_instances") == true);
        const auto& scalar = phase.at("scalar_request");
        const bool vertical = (value.at("version") == 15 || stair_dimensions || wall_dimensions) ? vertical_scalar(scalar) : historical_vertical;
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
            // Retain the exact ordinary roster authority for scalar one through
            // three and child-only selection, including detached dialects fifteen/twenty-one/twenty-four/twenty-seven.
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
            if (wall_dimensions && (row.at("wall_replacement").at("version") != 12 ||
                    !leaf.complete_hosted_instances || !leaf.complete_component_dimensions))
                reject("wall dimension Properties requires explicit leaf twelve room completion");
            if (qualified_walls && (row.at("wall_replacement").at("version") != 11 || !leaf.complete_hosted_instances))
                reject("qualified wall Properties requires explicit leaf eleven room completion");
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
            if (lane.at("version") == 4 || lane.at("version") == 5) reject("historical Properties has no wall elevation or hosted slab carry authority");
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
        if (lane.at("version")!=1) reject("historical Properties has no instance profile geometry authority");
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
void require_assembly_targets_present(const Entities& candidate,
    const std::vector<AssemblyInstancePropertyEditTarget>& targets, bool final_physical_check,
    bool qualified_phase_policy = false) {
    const auto owner = [&](const std::string& id) -> const Entity& {
        const auto found = candidate.find(id);
        if (found == candidate.end() || found->second.id != id) reject("reviewed retirement removed a selected assembly owner");
        return found->second;
    };
    using RootKey = std::tuple<std::string, std::string, std::optional<std::string>>;
    std::map<RootKey, std::vector<std::vector<std::string>>> selected_roots;
    std::vector<AssemblyInstancePropertyEditIntent> physical_checks;
    for (const auto& target : targets) {
        selected_roots[{target.catalog_id, target.instance_id, target.document_entity_id}].push_back(target.part_path);
        AssemblyInstancePropertyEditIntent retained;
        retained.target = target;
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
            const auto actual = decode_document_assembly_instance(root, budget);
            if (actual.assembly_catalog_id != catalog_id || actual.instance.id != instance_id)
                reject("selected independent assembly lost its exact qualified source identity");
            instance = actual.instance;
        } else {
            const auto found = std::find_if(model.instances().begin(), model.instances().end(), [&](const auto& row) { return row.id == instance_id; });
            if (found == model.instances().end()) reject("selected embedded assembly row disappeared");
            instance = *found;
        }
        if (instance.placement) {
            const auto& host = owner(instance.placement->host_entity_id);
            if (!is_complete_assembly_catalog_host_type(host.type)) reject("selected assembly lost its actual host role");
        }
        const auto expansion = model.expand(instance, budget);
        for (const auto& path : paths)
            if (std::count_if(expansion.nodes.begin(), expansion.nodes.end(), [&](const auto& node) { return node.part_path == path; }) != 1)
                reject("selected assembly lost its exact expanded part path");
    }
    // Recheck the actual combined physical graph through invariant typed
    // targets. This gives no candidate or replacement authoring authority.
    if (final_physical_check) {
        if (qualified_phase_policy) {
            // Fresh catalog/owner/host-copy identities need not preserve the
            // lexical order of the captured source target roster.
            std::sort(physical_checks.begin(), physical_checks.end(), [](const auto& left, const auto& right) {
                return left.target < right.target;
            });
            (void)replay_phase_assembly_instance_property_edits(candidate, physical_checks, {});
        }
        else (void)replay_assembly_instance_property_entities(candidate, physical_checks);
    }
}
void require_selected_members_present(const Json& value, const Entities& candidate,
    const Ids& retired_reviewed_rails = {}, const PhaseSelectionPropertyEditPreparation* phase = nullptr,
    bool final_physical_check = true, const MixedSelectionPropertyEditPreparation::ComponentIdentityMap* component_mapping = nullptr) {
    if (component_dialect(value)) {
        if (!component_mapping) reject("component selected targets require their independently reconstructed mapping");
        const auto base = (value.at("version")==39 || value.at("version")==40 || value.at("version")==41 || value.at("version")==42 || value.at("version")==43) ? Json(nullptr) : value.at("base_intent");
        if (!base.is_null()) require_selected_members_present(base, candidate, retired_reviewed_rails, phase, final_physical_check);
        const auto authoring = decode_phase_assembly_instance_property_edit_authoring(value.at("component_request").at("authoring"));
        const auto& mapping = *component_mapping;
        if (mapping.size() != authoring.intents.size()) reject("component mapping must exactly cover every selected target");
        std::vector<AssemblyInstancePropertyEditTarget> targets;
        for (const auto& intent : authoring.intents) {
            const auto found = mapping.find(intent.target);
            if (found == mapping.end() || found->second.part_path != intent.target.part_path)
                reject("component selected target lost its exact reconstructed part path");
            targets.push_back(found->second);
        }
        require_assembly_targets_present(candidate, targets, final_physical_check, true);
        if (value.at("version")==43) for (const auto& request:directional_stairs(value.at("stair_axis_requests"))) {
            const auto found=candidate.find(request.stair_id);
            if (found==candidate.end() || found->second.id!=request.stair_id || found->second.type!="stair")
                reject("mixed resize lost an independently reconstructed selected stair");
        }
        if (value.at("version")==39 || value.at("version")==42) {
            const auto allocation=slab_allocation(value.at("slab_allocation"));
            for (const auto& slab:directional_slabs(value.at("slab_geometry"),value.at("version")==42)) {
                const auto mapped=allocation ? allocation->identities.find(slab.slab_id) : PhaseSlabReplacementIdentityMap::const_iterator{};
                const auto id=allocation && mapped!=allocation->identities.end() ? mapped->second : slab.slab_id;
                const auto found=candidate.find(id);
                if (found==candidate.end() || found->second.id!=id || found->second.type!="slab")
                    reject("mixed resize lost an independently reconstructed selected slab");
            }
        }
        if (value.at("version")==40 || value.at("version")==42)
            for (const auto& request:directional_roof_openings(value.at("roof_opening_axis_requests"),value.at("version")==42))
                (void)roof_opening_group_member_center_world(candidate,request.target);
        if (value.at("version")==41 || value.at("version")==42) {
            const auto allocation=structural_allocation(value.at("structural_allocation"));
            for (const auto& request:directional_structural(value.at("structural_axis_requests"),value.at("version")==42)) {
                auto id=request.object_id;
                if (allocation && allocation->identities.contains(id)) id=allocation->identities.at(id);
                const auto found=candidate.find(id);
                if (found==candidate.end() || found->second.id!=id ||
                    (found->second.type!="column" && found->second.type!="beam"))
                    reject("mixed resize lost an independently reconstructed structural owner");
                validate_structural_object_source_entity(found->second);
            }
        }
        return;
    }
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
    if ((value.at("version") == 10 || phase_selection_dialect(value)) && !value.at("assembly_request").is_null()) {
        std::vector<AssemblyInstancePropertyEditTarget> targets;
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
            targets.push_back(std::move(target));
        }
        // Original-source leaves cannot certify a host body changed by another
        // lane. Admit the complete composed physical cohort through no-op
        // intents, including actual host copies and retained members. This
        // discards the return map and grants no candidate authoring authority.
        require_assembly_targets_present(candidate, targets, final_physical_check);
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
    std::optional<PhaseAssemblyInstancePropertyEditPreparation> component;
    MixedSelectionPropertyEditPreparation::ComponentIdentityMap component_mapping;
    std::vector<std::string> reserved_component_identity_ids;
    std::vector<std::string> elided_component_identity_ids;
    std::map<std::string,std::string,std::less<>> slab_identity_map;
    std::map<std::string,std::string,std::less<>> slab_selected_identity_map;
    MixedSelectionPropertyEditPreparation::HostedBindingMap slab_hosted_bindings;
    std::vector<Json> physical_phase_authorings;
    std::map<std::pair<std::string,std::string>,RoofOpeningGroupMember> roof_children;
};
Stage derive_phase(const DocumentSnapshot& source, const Json& value) {
    auto phase = replay_phase_selection_property_edit_preparation(source, value.at("phase_request"));
    const bool vertical = vertical_scalar(phase.inspection().scalar_request());
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
    if (vertical) {
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
    auto reviews = requirements(source, candidate, changed_hosts, vertical);
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
    if (value.at("version")==43) {
        const auto requests=directional_stairs(value.at("stair_axis_requests"));
        const auto authoring=decode_phase_assembly_instance_property_edit_authoring(value.at("component_request").at("authoring"));
        preflight_component_stair_directional_resize(source,authoring,requests);
        auto component=replay_phase_assembly_instance_property_edit_preparation(source,value.at("component_request"));
        if (value.at("component_identity_reservations")!=Json(component.result().fresh_identity_ids))
            reject("component reservation receipt differs from actual original-source replay");
        auto composed=compose_component_stair_directional_resize(source,component,requests,value.at("message").get_ref<const std::string&>());
        Stage stage;
        stage.entities=std::move(composed.components.entities);
        stage.component_mapping=std::move(composed.components.original_to_proposed);
        stage.reserved_component_identity_ids=std::move(composed.components.reserved_component_identity_ids);
        stage.elided_component_identity_ids=std::move(composed.components.elided_component_identity_ids);
        stage.slab_selected_identity_map=std::move(composed.stair_selected_original_to_proposed);
        stage.slab_identity_map=stage.slab_selected_identity_map;
        stage.reviewed_erased.insert(component.result().retired_dimension_ids.begin(),component.result().retired_dimension_ids.end());
        stage.component=std::move(component);
        require_selected_members_present(value,stage.entities,{},nullptr,true,&stage.component_mapping);
        return stage;
    }
    if (value.at("version")==42) {
        auto component=replay_phase_assembly_instance_property_edit_preparation(source,value.at("component_request"));
        if (value.at("component_identity_reservations")!=Json(component.result().fresh_identity_ids))
            reject("component reservation receipt differs from actual original-source replay");
        auto composed=compose_component_physical_directional_resize(source,component,
            directional_slabs(value.at("slab_geometry"),true),slab_allocation(value.at("slab_allocation")),
            directional_structural(value.at("structural_axis_requests"),true),structural_allocation(value.at("structural_allocation")),
            directional_roof_openings(value.at("roof_opening_axis_requests"),true),value.at("message").get_ref<const std::string&>());
        Stage stage;
        stage.entities=std::move(composed.components.entities);
        stage.component_mapping=std::move(composed.components.original_to_proposed);
        stage.reserved_component_identity_ids=std::move(composed.components.reserved_component_identity_ids);
        stage.elided_component_identity_ids=std::move(composed.components.elided_component_identity_ids);
        stage.reviewed_erased.insert(component.result().retired_dimension_ids.begin(),component.result().retired_dimension_ids.end());
        stage.component=std::move(component);
        stage.slab_identity_map=std::move(composed.original_to_proposed);
        stage.slab_selected_identity_map=std::move(composed.selected_original_to_proposed);
        stage.slab_hosted_bindings=std::move(composed.hosted_bindings);
        stage.roof_children=std::move(composed.roof_children);
        stage.physical_phase_authorings=std::move(composed.phase_authorings);
        require_selected_members_present(value,stage.entities,{},nullptr,true,&stage.component_mapping);
        return stage;
    }
    if (value.at("version")==41) {
        auto component=replay_phase_assembly_instance_property_edit_preparation(source,value.at("component_request"));
        if (value.at("component_identity_reservations")!=Json(component.result().fresh_identity_ids))
            reject("component reservation receipt differs from actual original-source replay");
        auto composed=compose_component_structural_directional_resize(source,component,
            directional_structural(value.at("structural_axis_requests")),structural_allocation(value.at("structural_allocation")),
            value.at("message").get_ref<const std::string&>());
        Stage stage;
        stage.entities=std::move(composed.components.entities);
        stage.component_mapping=std::move(composed.components.original_to_proposed);
        stage.reserved_component_identity_ids=std::move(composed.components.reserved_component_identity_ids);
        stage.elided_component_identity_ids=std::move(composed.components.elided_component_identity_ids);
        stage.reviewed_erased.insert(component.result().retired_dimension_ids.begin(),component.result().retired_dimension_ids.end());
        stage.component=std::move(component);
        stage.slab_identity_map=std::move(composed.structural_original_to_proposed);
        stage.slab_selected_identity_map=std::move(composed.structural_selected_original_to_proposed);
        stage.slab_hosted_bindings=std::move(composed.structural_hosted_bindings);
        stage.physical_phase_authorings=std::move(composed.structural_phase_authorings);
        require_selected_members_present(value,stage.entities,{},nullptr,true,&stage.component_mapping);
        return stage;
    }
    if (value.at("version")==40) {
        auto component=replay_phase_assembly_instance_property_edit_preparation(source,value.at("component_request"));
        if (value.at("component_identity_reservations")!=Json(component.result().fresh_identity_ids))
            reject("component reservation receipt differs from actual original-source replay");
        auto composed=compose_component_roof_opening_directional_resize(source,component,
            directional_roof_openings(value.at("roof_opening_axis_requests")),value.at("message").get_ref<const std::string&>());
        Stage stage;
        stage.entities=std::move(composed.components.entities);
        stage.component_mapping=std::move(composed.components.original_to_proposed);
        stage.reserved_component_identity_ids=std::move(composed.components.reserved_component_identity_ids);
        stage.elided_component_identity_ids=std::move(composed.components.elided_component_identity_ids);
        stage.reviewed_erased.insert(component.result().retired_dimension_ids.begin(),component.result().retired_dimension_ids.end());
        stage.component=std::move(component);
        stage.physical_phase_authorings=std::move(composed.phase_authorings);
        stage.roof_children=std::move(composed.roof_children);
        require_selected_members_present(value,stage.entities,{},nullptr,true,&stage.component_mapping);
        return stage;
    }
    if (value.at("version")==39) {
        auto component=replay_phase_assembly_instance_property_edit_preparation(source,value.at("component_request"));
        if (value.at("component_identity_reservations")!=Json(component.result().fresh_identity_ids))
            reject("component reservation receipt differs from actual original-source replay");
        auto composed=compose_component_slab_directional_resize(source,component,directional_slabs(value.at("slab_geometry")),
            slab_allocation(value.at("slab_allocation")),value.at("message").get_ref<const std::string&>());
        Stage stage;
        stage.entities=std::move(composed.components.entities);
        stage.component_mapping=std::move(composed.components.original_to_proposed);
        stage.reserved_component_identity_ids=std::move(composed.components.reserved_component_identity_ids);
        stage.elided_component_identity_ids=std::move(composed.components.elided_component_identity_ids);
        stage.reviewed_erased.insert(component.result().retired_dimension_ids.begin(),component.result().retired_dimension_ids.end());
        stage.component=std::move(component);
        stage.slab_identity_map=std::move(composed.slab_original_to_proposed);
        stage.slab_selected_identity_map=std::move(composed.slab_selected_original_to_proposed);
        stage.slab_hosted_bindings=std::move(composed.slab_hosted_bindings);
        stage.physical_phase_authorings=std::move(composed.slab_phase_authorings);
        require_selected_members_present(value,stage.entities,{},nullptr,true,&stage.component_mapping);
        return stage;
    }
    if (component_dialect(value)) {
        auto component = replay_phase_assembly_instance_property_edit_preparation(source, value.at("component_request"));
        if (coordinated_component_dialect(value) &&
            value.at("component_identity_reservations") != Json(component.result().fresh_identity_ids))
            reject("component reservation receipt differs from actual original-source replay");
        const auto& base = value.at("base_intent");
        Stage stage;
        std::optional<MixedSelectionPropertyEditPreparation> prepared_base;
        if (!base.is_null()) {
            // Both retain the historical full-source replay. The Stage owns
            // its exact review/removal metadata; the sealed preparation is the
            // composer's separately authenticated contribution.
            stage = derive(source, base);
            prepared_base = replay_mixed_selection_property_edit_preparation(source, base);
        }
        if (coordinated_component_dialect(value) && prepared_base) {
            // A nonnull sealed base supplies entities, survivor correspondence
            // and allocation elision come from one independently replayed result.
            auto coordinated = compose_coordinated_component_property_dependencies(source, component, *prepared_base);
            stage.entities = std::move(coordinated.entities);
            stage.component_mapping = std::move(coordinated.original_to_proposed);
            stage.reserved_component_identity_ids = std::move(coordinated.reserved_component_identity_ids);
            stage.elided_component_identity_ids = std::move(coordinated.elided_component_identity_ids);
        } else if (value.at("version") == 17 || value.at("version") == 32 || value.at("version") == 33 || value.at("version") == 34 || value.at("version") == 35 || value.at("version") == 36 || value.at("version") == 37 || value.at("version") == 38) {
            stage.entities = component.preview_entities();
            stage.component_mapping = component.result().original_to_proposed;
            stage.reserved_component_identity_ids = component.result().fresh_identity_ids;
        } else {
            stage.entities = compose_phase_assembly_with_mixed_property_selection_edit_candidates(
                source, component, prepared_base ? &*prepared_base : nullptr);
            stage.component_mapping = resolve_phase_assembly_with_mixed_property_correspondence(
                source, component, prepared_base ? &*prepared_base : nullptr);
        }
        stage.reviewed_erased.insert(component.result().retired_dimension_ids.begin(), component.result().retired_dimension_ids.end());
        for (const auto& id : component.result().retired_dimension_ids)
            if (stage.entities.contains(id)) reject("component composition restored an explicitly retired profile dimension");
        stage.component = std::move(component);
        require_selected_members_present(value, stage.entities, stage.retired_reviewed_rails,
            stage.phase ? &*stage.phase : nullptr, false, &stage.component_mapping);
        if (stage.phase) {
            // The final inventories inspect the actual composed stage while
            // retaining the base's exact leaf order and identity metadata.
            stage.phase_reviews.clear();
            for (const auto& wire : stage.phase->phase_authorings()) {
                const auto intent = decode_phase_constraint_authoring_intent(wire);
                if (!intent.wall_replacement.is_null()) {
                    const auto preview = inspect_phase_wall_replacement_rooms_on_composed_stage(source, intent, stage.entities);
                    if (preview.needs_room_review) stage.phase_reviews.push_back(preview.room_inventory);
                }
            }
            std::erase_if(stage.reviews, [&](const auto& review) {
                return std::any_of(stage.phase_reviews.begin(), stage.phase_reviews.end(), [&](const auto& inventory) {
                    return std::any_of(inventory.intent.planes.begin(), inventory.intent.planes.end(), [&](const auto& plane) {
                        return plane.context == review.context && std::abs(plane.effective_elevation_m - review.effective_elevation_m) <=
                            default_geometry_tolerance_metres;
                    });
                });
            });
        }
        return stage;
    }
    if (phase_selection_dialect(value)) return derive_phase(source, value);
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
    std::vector<PhysicalWallRoomPhaseReviewInventory> phase_reviews,
    std::optional<PhaseAssemblyInstancePropertyEditPreparation> component, ComponentIdentityMap component_mapping,
    std::vector<std::string> reserved_component_identity_ids, std::vector<std::string> elided_component_identity_ids)
    : intent_(std::move(intent)), preview_(source), source_digest_(intent_.at("source_snapshot_digest").get<std::string>()),
      room_reviews_(std::move(reviews)), phase_(std::move(phase)), component_(std::move(component)),
      component_identity_map_(std::move(component_mapping)),
      reserved_component_identity_ids_(std::move(reserved_component_identity_ids)),
      elided_component_identity_ids_(std::move(elided_component_identity_ids)),
      phase_room_reviews_(std::move(phase_reviews)) {
    if (phase_) {
        selected_identity_map_ = phase_->selected_original_to_proposed();
        identity_map_ = phase_->original_to_proposed();
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
    if (component_) for (const auto& [original, proposed] : component_identity_map_) {
        if (original.document_entity_id) {
            if (!proposed.document_entity_id) reject("independent component lost its reconstructed document destination");
            for (auto* map : {&selected_identity_map_, &identity_map_}) {
                const auto [found, inserted] = map->emplace(*original.document_entity_id, *proposed.document_entity_id);
                if (!inserted && found->second != *proposed.document_entity_id) reject("selected component has ambiguous document destinations");
            }
        }
        if (!original.document_entity_id) {
            const std::pair source_binding{original.catalog_id, original.instance_id};
            const std::pair destination{proposed.catalog_id, proposed.instance_id};
            const auto [found, inserted] = hosted_bindings_.emplace(source_binding, destination);
            if (!inserted && found->second != destination) reject("selected component has ambiguous qualified destinations");
        }
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
    return selected_identity_map_;
}
const std::map<std::string, std::string, std::less<>>& MixedSelectionPropertyEditPreparation::original_to_proposed() const noexcept {
    return identity_map_;
}
const MixedSelectionPropertyEditPreparation::ComponentIdentityMap& MixedSelectionPropertyEditPreparation::component_original_to_proposed() const noexcept {
    return component_identity_map_;
}
const std::vector<std::string>& MixedSelectionPropertyEditPreparation::reserved_component_identity_ids() const noexcept {
    return reserved_component_identity_ids_;
}
const std::vector<std::string>& MixedSelectionPropertyEditPreparation::elided_component_identity_ids() const noexcept {
    return elided_component_identity_ids_;
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
    return phase_ ? phase_->phase_authorings() : !physical_phase_authorings_.empty() ? physical_phase_authorings_ : empty;
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
    const bool slab_carry = ordinary && std::any_of(ordinary->slabs.begin(), ordinary->slabs.end(), [](const auto& slab) { return slab.coordinate_hosted_elevation; });
    if (ordinary && (slab_carry || std::any_of(ordinary->walls.begin(), ordinary->walls.end(), [](const auto& wall) { return wall.elevation.has_value(); }))) {
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
        value["version"] = slab_carry ? 13 : 12;
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
    const std::vector<PhaseStairReplacementDependencyDisposition>& dispositions,
    bool complete_wall_hosted_instances, bool complete_stair_component_dimensions,
    bool complete_wall_component_dimensions, bool complete_roof_component_dimensions,
    bool complete_slab_component_dimensions, bool complete_structural_component_dimensions) {
    if (!ordinary && !children) {
        if (complete_wall_hosted_instances) reject("qualified wall Properties requires an actual physical or skylight selection");
        if (complete_stair_component_dimensions) reject("stair dimension Properties requires an actual physical or skylight selection");
        if (complete_wall_component_dimensions) reject("wall dimension Properties requires an actual physical or skylight selection");
        if (complete_roof_component_dimensions || complete_slab_component_dimensions || complete_structural_component_dimensions)
            reject("family dimension Properties requires an actual physical or skylight selection");
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
    request.complete_wall_hosted_instances = complete_wall_hosted_instances;
    request.complete_stair_component_dimensions = complete_stair_component_dimensions;
    request.complete_wall_component_dimensions = complete_wall_component_dimensions;
    request.complete_roof_component_dimensions = complete_roof_component_dimensions;
    request.complete_slab_component_dimensions = complete_slab_component_dimensions;
    request.complete_structural_component_dimensions = complete_structural_component_dimensions;
    const auto phase = prepare_phase_selection_property_edit(source, request);
    auto value = source_fields(source);
    value["version"] = phase.request().at("version") == 8 ? 27 : phase.request().at("version") == 7 ? 24 :
        phase.request().at("version") == 6 ? 21 : phase.request().at("version") == 5 ? 15 :
        phase.request().at("version") == 4 ? 13 : phase.request().at("version") == 3 ? 12 : 11;
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
MixedSelectionPropertyEditPreparation prepare_mixed_selection_property_edit(const DocumentSnapshot& source,
    const PhaseAssemblyInstancePropertyEditAuthoring& authoring,
    const MixedSelectionPropertyEditPreparation* base, std::string_view message) {
    if (message.size() > 1024 || message.find('\0') != std::string_view::npos) reject("invalid message");
    const auto component = prepare_phase_assembly_instance_property_edit(source, authoring);
    if (base) require_mixed_selection_property_edit_source_current(source, *base);
    auto value = source_fields(source);
    value["version"] = 14;
    value["component_request"] = component.request();
    value["base_intent"] = base ? base->intent() : Json(nullptr);
    value["room_reviews"] = Json::array();
    value["phase_room_reviews"] = Json::array();
    value["message"] = std::string(message);
    return replay_mixed_selection_property_edit_preparation(source, value);
}
MixedSelectionPropertyEditPreparation prepare_coordinated_component_property_edit(const DocumentSnapshot& source,
    const PhaseAssemblyInstancePropertyEditAuthoring& authoring,
    const MixedSelectionPropertyEditPreparation& base, std::string_view message) {
    if (message.size() > 1024 || message.find('\0') != std::string_view::npos) reject("invalid message");
    require_mixed_selection_property_edit_source_current(source, base);
    const auto replayed_base = replay_mixed_selection_property_edit_preparation(source, base.intent());
    const auto component = prepare_phase_assembly_instance_property_edit(source, authoring);
    auto value = source_fields(source);
    const bool stair_dimensions = replayed_base.intent().at("version") == 21;
    const bool family_dimensions = replayed_base.intent().at("version") == 27;
    const bool wall_dimensions = replayed_base.intent().at("version") == 24 || family_dimensions;
    value["version"] = family_dimensions ? 28 : wall_dimensions ? 25 : stair_dimensions ? 22 : 16;
    if (wall_dimensions) {
        value["complete_wall_component_dimensions"] = true;
        value["complete_stair_component_dimensions"] =
            replayed_base.intent().at("phase_request").at("complete_stair_component_dimensions");
    } else if (stair_dimensions) value["complete_stair_component_dimensions"] = true;
    if (family_dimensions)
        for (const auto* key : {"complete_roof_component_dimensions", "complete_slab_component_dimensions",
                "complete_structural_component_dimensions"})
            value[key] = replayed_base.intent().at("phase_request").at(key);
    value["coordinate_component_hosts"] = true;
    value["component_identity_reservations"] = component.result().fresh_identity_ids;
    value["component_request"] = component.request();
    value["base_intent"] = replayed_base.intent();
    value["room_reviews"] = Json::array();
    value["phase_room_reviews"] = Json::array();
    value["message"] = std::string(message);
    return replay_mixed_selection_property_edit_preparation(source, value);
}
MixedSelectionPropertyEditPreparation prepare_component_presentation_property_edit(const DocumentSnapshot& source,
    const PhaseAssemblyInstancePropertyEditAuthoring& authoring,
    const MixedSelectionPropertyEditPreparation* base, std::string_view message) {
    if (message.size() > 1024 || message.find('\0') != std::string_view::npos) reject("invalid message");
    const auto component = prepare_phase_assembly_instance_property_edit(source, authoring);
    const bool initialize_deformed_topology = component.request().at("version") == 8;
    const bool reviewed_initial_geometry = component.request().at("version") == 9;
    const bool reviewed_topology = component.request().at("version") == 7;
    const bool initialize_topology = component.request().at("version") == 6;
    const bool topology_geometry = component.request().at("version") == 5;
    const bool profile_geometry = component.request().at("version") == 3 || topology_geometry || initialize_topology || reviewed_topology || initialize_deformed_topology || reviewed_initial_geometry;
    const bool component_dimensions = component.request().at("version") == 4;
    const bool complete_component = profile_geometry || component_dimensions;
    if (!complete_component && component.request().at("version") != 2) reject("component presentation Properties requires reference-carry authoring");
    if (base) require_mixed_selection_property_edit_source_current(source, *base);
    auto value = source_fields(source);
    const bool stair_dimensions = !complete_component && base && base->intent().at("version") == 21;
    const bool family_dimensions = !complete_component && base && base->intent().at("version") == 27;
    const bool wall_dimensions = !complete_component && base && (base->intent().at("version") == 24 || family_dimensions);
    value["version"] = reviewed_initial_geometry ? 38 : initialize_deformed_topology ? 37 : reviewed_topology ? 36 : initialize_topology ? 35 : topology_geometry ? 34 : component_dimensions ? 33 : profile_geometry ? 32 : family_dimensions ? 29 : wall_dimensions ? 26 : stair_dimensions ? 23 : 17;
    if (profile_geometry) value["complete_instance_profile_geometry"] = true;
    if (component_dimensions) value["complete_component_dimensions"] = true;
    if (wall_dimensions) {
        value["complete_wall_component_dimensions"] = true;
        value["complete_stair_component_dimensions"] =
            base->intent().at("phase_request").at("complete_stair_component_dimensions");
    } else if (stair_dimensions) value["complete_stair_component_dimensions"] = true;
    if (family_dimensions)
        for (const auto* key : {"complete_roof_component_dimensions", "complete_slab_component_dimensions",
                "complete_structural_component_dimensions"})
            value[key] = base->intent().at("phase_request").at(key);
    value["coordinate_component_hosts"] = true;
    value["carry_component_presentation"] = true;
    value["component_identity_reservations"] = component.result().fresh_identity_ids;
    value["component_request"] = component.request();
    value["base_intent"] = base ? base->intent() : Json(nullptr);
    value["room_reviews"] = Json::array();
    value["phase_room_reviews"] = Json::array();
    value["message"] = std::string(message);
    return replay_mixed_selection_property_edit_preparation(source, value);
}
MixedSelectionPropertyEditPreparation prepare_component_physical_directional_resize(
    const DocumentSnapshot& source,const PhaseAssemblyInstancePropertyEditAuthoring& authoring,
    std::span<const SlabGeometryEditIntent> slab_geometry,
    const std::optional<ComponentSlabDirectionalResizeAllocation>& slab_allocation,
    std::span<const StructuralDirectionalResizeRequest> structural_requests,
    const std::optional<ComponentStructuralDirectionalResizeAllocation>& structural_allocation,
    std::span<const RoofOpeningDirectionalResizeRequest> roof_opening_requests,std::string_view message) {
    const auto families=static_cast<unsigned>(!slab_geometry.empty())+static_cast<unsigned>(!structural_requests.empty())+
        static_cast<unsigned>(!roof_opening_requests.empty());
    if (families<2 || authoring.intents.empty() || slab_geometry.size()>1000 || structural_requests.size()>1000 ||
        roof_opening_requests.size()>1000 || authoring.intents.size()>1000 ||
        authoring.intents.size()+slab_geometry.size()+structural_requests.size()+roof_opening_requests.size()>1000 ||
        message.size()>1024 || message.find('\0')!=std::string_view::npos)
        reject("typed combined physical resize exceeds its complete roster or message bound");
    const auto check_allocation=[](const auto& allocation) {
        if (allocation && (allocation->identities.size()>4096 || allocation->hosted_instance_identities.size()>4096 ||
            allocation->dimension_identities.size()>1000 || allocation->identities.size()+
            allocation->hosted_instance_identities.size()+allocation->dimension_identities.size()>4096))
            reject("typed physical allocation exceeds its complete source roster bound");
    };
    check_allocation(slab_allocation);check_allocation(structural_allocation);
    const auto component=prepare_phase_assembly_instance_property_edit(source,authoring);
    std::vector<SlabGeometryEditIntent> slabs(slab_geometry.begin(),slab_geometry.end());
    for (auto& intent:slabs) {
        const auto found=source.entities().find(intent.slab_id);
        if (found==source.entities().end() || found->second.id!=intent.slab_id || found->second.type!="slab")
            reject("combined slab resize requires actual original source owners");
        intent.preserve_planar_source_geometry=slab_geometry_edit_requires_planar_source(found->second,intent);
    }
    std::sort(slabs.begin(),slabs.end(),[](const auto& left,const auto& right) { return left.slab_id<right.slab_id; });
    std::vector<StructuralDirectionalResizeRequest> structures(structural_requests.begin(),structural_requests.end());
    std::sort(structures.begin(),structures.end(),[](const auto& left,const auto& right) { return left.object_id<right.object_id; });
    std::vector<RoofOpeningDirectionalResizeRequest> openings(roof_opening_requests.begin(),roof_opening_requests.end());
    std::sort(openings.begin(),openings.end(),[](const auto& left,const auto& right) {
        return std::tie(left.target.roof_id,left.target.opening_id)<std::tie(right.target.roof_id,right.target.opening_id);
    });
    auto value=source_fields(source);value["version"]=42;
    value["component_request"]=component.request();
    value["component_identity_reservations"]=component.result().fresh_identity_ids;
    value["slab_geometry"]=Json::array();
    for (const auto& intent:slabs) value["slab_geometry"].push_back(encode_slab_geometry_edit_intent(intent));
    value["slab_allocation"]=slab_allocation_json(slab_allocation);
    value["structural_axis_requests"]=Json::array();
    for (const auto& request:structures)
        value["structural_axis_requests"].push_back({{"object_id",request.object_id},{"sx",request.sx},{"sy",request.sy},
            {"anchor_m",{request.anchor_m.x,request.anchor_m.y}},{"frame_rotation_radians",request.frame_rotation_radians}});
    value["structural_allocation"]=structural_allocation_json(structural_allocation);
    value["roof_opening_axis_requests"]=Json::array();
    for (const auto& request:openings)
        value["roof_opening_axis_requests"].push_back({{"roof_id",request.target.roof_id},{"opening_id",request.target.opening_id},
            {"sx",request.sx},{"sy",request.sy},{"anchor_m",{request.anchor_m.x,request.anchor_m.y}},
            {"frame_rotation_radians",request.frame_rotation_radians}});
    value["room_reviews"]=Json::array();value["phase_room_reviews"]=Json::array();value["message"]=std::string(message);
    return replay_mixed_selection_property_edit_preparation(source,value);
}
MixedSelectionPropertyEditPreparation prepare_component_slab_directional_resize(const DocumentSnapshot& source,
    const PhaseAssemblyInstancePropertyEditAuthoring& authoring,std::span<const SlabGeometryEditIntent> slab_geometry,
    const std::optional<ComponentSlabDirectionalResizeAllocation>& allocation,std::string_view message) {
    if (slab_geometry.empty() || slab_geometry.size()>1000 || message.size()>1024 || message.find('\0')!=std::string_view::npos)
        reject("typed mixed resize request exceeds its complete roster or message bound");
    if (allocation && (allocation->identities.size()>4096 || allocation->hosted_instance_identities.size()>4096 ||
        allocation->dimension_identities.size()>1000 || allocation->identities.size()+
        allocation->hosted_instance_identities.size()+allocation->dimension_identities.size()>4096))
        reject("typed slab allocation exceeds its complete source roster bound");
    const auto component=prepare_phase_assembly_instance_property_edit(source,authoring);
    std::vector<SlabGeometryEditIntent> slabs(slab_geometry.begin(),slab_geometry.end());
    for (auto& intent:slabs) {
        const auto found=source.entities().find(intent.slab_id);
        if (found==source.entities().end() || found->second.id!=intent.slab_id || found->second.type!="slab")
            reject("slab resize requires actual original source owners");
        intent.preserve_planar_source_geometry=slab_geometry_edit_requires_planar_source(found->second,intent);
    }
    std::sort(slabs.begin(),slabs.end(),[](const auto& left,const auto& right) { return left.slab_id<right.slab_id; });
    auto value=source_fields(source);
    value["version"]=39;
    value["component_request"]=component.request();
    value["component_identity_reservations"]=component.result().fresh_identity_ids;
    value["slab_geometry"]=Json::array();
    for (const auto& intent:slabs) value["slab_geometry"].push_back(encode_slab_geometry_edit_intent(intent));
    value["slab_allocation"]=slab_allocation_json(allocation);
    value["room_reviews"]=Json::array();
    value["phase_room_reviews"]=Json::array();
    value["message"]=std::string(message);
    return replay_mixed_selection_property_edit_preparation(source,value);
}
MixedSelectionPropertyEditPreparation prepare_component_stair_directional_resize(const DocumentSnapshot& source,
    const PhaseAssemblyInstancePropertyEditAuthoring& authoring,std::span<const StairDirectionalResizeRequest> requests,
    std::string_view message) {
    if (message.size()>1024 || message.find('\0')!=std::string_view::npos)
        reject("typed mixed stair resize message exceeds its bound");
    preflight_component_stair_directional_resize(source,authoring,requests);
    const auto component=prepare_phase_assembly_instance_property_edit(source,authoring);
    std::vector<StairDirectionalResizeRequest> sorted(requests.begin(),requests.end());
    std::sort(sorted.begin(),sorted.end(),[](const auto& a,const auto& b) { return a.stair_id<b.stair_id; });
    auto value=source_fields(source);value["version"]=43;
    value["component_request"]=component.request();
    value["component_identity_reservations"]=component.result().fresh_identity_ids;
    value["stair_axis_requests"]=Json::array();
    for (const auto& request:sorted) value["stair_axis_requests"].push_back({{"stair_id",request.stair_id},
        {"sx",request.sx},{"sy",request.sy},{"anchor_m",{request.anchor_m.x,request.anchor_m.y}},
        {"frame_rotation_radians",request.frame_rotation_radians}});
    value["room_reviews"]=Json::array();value["phase_room_reviews"]=Json::array();value["message"]=std::string(message);
    return replay_mixed_selection_property_edit_preparation(source,value);
}
MixedSelectionPropertyEditPreparation prepare_component_structural_directional_resize(const DocumentSnapshot& source,
    const PhaseAssemblyInstancePropertyEditAuthoring& authoring,std::span<const StructuralDirectionalResizeRequest> requests,
    const std::optional<ComponentStructuralDirectionalResizeAllocation>& allocation,std::string_view message) {
    if (requests.empty() || requests.size()>1000 || authoring.intents.empty() || authoring.intents.size()+requests.size()>1000 ||
        message.size()>1024 || message.find('\0')!=std::string_view::npos)
        reject("typed mixed structural resize exceeds its complete roster or message bound");
    if (allocation && (allocation->identities.size()>4096 || allocation->hosted_instance_identities.size()>4096 ||
        allocation->dimension_identities.size()>1000 || allocation->identities.size()+
        allocation->hosted_instance_identities.size()+allocation->dimension_identities.size()>4096))
        reject("typed structural allocation exceeds its complete source roster bound");
    const auto component=prepare_phase_assembly_instance_property_edit(source,authoring);
    std::vector<StructuralDirectionalResizeRequest> sorted(requests.begin(),requests.end());
    std::sort(sorted.begin(),sorted.end(),[](const auto& left,const auto& right) { return left.object_id<right.object_id; });
    auto value=source_fields(source);
    value["version"]=41;
    value["component_request"]=component.request();
    value["component_identity_reservations"]=component.result().fresh_identity_ids;
    value["structural_axis_requests"]=Json::array();
    for (const auto& request:sorted)
        value["structural_axis_requests"].push_back({{"object_id",request.object_id},{"sx",request.sx},{"sy",request.sy},
            {"anchor_m",{request.anchor_m.x,request.anchor_m.y}},{"frame_rotation_radians",request.frame_rotation_radians}});
    value["structural_allocation"]=structural_allocation_json(allocation);
    value["room_reviews"]=Json::array();value["phase_room_reviews"]=Json::array();value["message"]=std::string(message);
    return replay_mixed_selection_property_edit_preparation(source,value);
}
MixedSelectionPropertyEditPreparation prepare_component_roof_opening_directional_resize(const DocumentSnapshot& source,
    const PhaseAssemblyInstancePropertyEditAuthoring& authoring,
    std::span<const RoofOpeningDirectionalResizeRequest> requests,std::string_view message) {
    if (requests.empty() || requests.size()>1000 || authoring.intents.empty() ||
        authoring.intents.size()+requests.size()>1000 || message.size()>1024 || message.find('\0')!=std::string_view::npos)
        reject("typed mixed skylight resize request exceeds its complete roster or message bound");
    const auto component=prepare_phase_assembly_instance_property_edit(source,authoring);
    std::vector<RoofOpeningDirectionalResizeRequest> sorted(requests.begin(),requests.end());
    std::sort(sorted.begin(),sorted.end(),[](const auto& left,const auto& right) {
        return std::tie(left.target.roof_id,left.target.opening_id)<std::tie(right.target.roof_id,right.target.opening_id);
    });
    auto value=source_fields(source);
    value["version"]=40;
    value["component_request"]=component.request();
    value["component_identity_reservations"]=component.result().fresh_identity_ids;
    value["roof_opening_axis_requests"]=Json::array();
    for (const auto& request:sorted)
        value["roof_opening_axis_requests"].push_back({{"roof_id",request.target.roof_id},{"opening_id",request.target.opening_id},
            {"sx",request.sx},{"sy",request.sy},{"anchor_m",{request.anchor_m.x,request.anchor_m.y}},
            {"frame_rotation_radians",request.frame_rotation_radians}});
    value["room_reviews"]=Json::array();
    value["phase_room_reviews"]=Json::array();
    value["message"]=std::string(message);
    return replay_mixed_selection_property_edit_preparation(source,value);
}
MixedSelectionPropertyEditPreparation replay_mixed_selection_property_edit_preparation(const DocumentSnapshot& source, const Json& value) {
    (void)validate_mixed_selection_property_edit_intent(value);
    if (!value.at("room_reviews").empty() || (phase_room_dialect(value) && !value.at("phase_room_reviews").empty()))
        reject("preparation cannot accept completed room decisions");
    auto stage = derive(source, value);
    MixedSelectionPropertyEditPreparation prepared{source, value, std::move(stage.entities), std::move(stage.reviews), std::move(stage.phase), std::move(stage.phase_reviews),
        std::move(stage.component), std::move(stage.component_mapping),
        std::move(stage.reserved_component_identity_ids), std::move(stage.elided_component_identity_ids)};
    prepared.identity_map_.insert(stage.slab_identity_map.begin(),stage.slab_identity_map.end());
    prepared.selected_identity_map_.insert(stage.slab_selected_identity_map.begin(),stage.slab_selected_identity_map.end());
    prepared.hosted_bindings_.insert(stage.slab_hosted_bindings.begin(),stage.slab_hosted_bindings.end());
    prepared.physical_phase_authorings_=std::move(stage.physical_phase_authorings);
    prepared.roof_children_.insert(stage.roof_children.begin(),stage.roof_children.end());
    return prepared;
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
void protect_component_review_stage(const Entities& original, const Entities& reconstructed,
    const Entities& candidate, const MixedSelectionPropertyEditPreparation::ComponentIdentityMap& mapping) {
    const auto scope = constraint_phase_scope(original);
    Ids protected_ids = retained_baselines(original);
    protected_ids.insert(scope.inactive_owner_ids.begin(), scope.inactive_owner_ids.end());
    for (const auto& [source, destination] : mapping) {
        protected_ids.insert(source.catalog_id);
        protected_ids.insert(destination.catalog_id);
        if (source.document_entity_id) protected_ids.insert(*source.document_entity_id);
        if (destination.document_entity_id) protected_ids.insert(*destination.document_entity_id);
    }
    // Component catalog edits and fresh destinations are already authorized by
    // actual-source replay. Room decisions cannot rewrite that reconstructed
    // authority or change unrelated protected owners.
    for (const auto& id : protected_ids) {
        const auto expected = reconstructed.find(id), actual = candidate.find(id);
        if ((expected == reconstructed.end()) != (actual == candidate.end()) ||
            (expected != reconstructed.end() && !exact(expected->second, actual->second)))
            reject("room completion changed a reconstructed component or protected owner");
    }
}
void review_phase_stage(const DocumentSnapshot& source, const Json& value, Stage& stage, bool complete) {
    if (!stage.phase) return;
    const std::optional<Entities> component_stage = stage.component ? std::optional<Entities>{stage.entities} : std::nullopt;
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
    if (component_stage) protect_component_review_stage(source.entities(), *component_stage, stage.entities, stage.component_mapping);
    else protect_phase_source(source.entities(), stage.entities);
    require_selected_members_present(value, stage.entities, stage.retired_reviewed_rails, &*stage.phase, false,
        stage.component ? &stage.component_mapping : nullptr);
}
ReplayedPhysicalWallRoomReview review_stage(const DocumentSnapshot& source, const Json& value, Stage stage, bool complete) {
    const std::optional<Entities> component_stage = stage.component ? std::optional<Entities>{stage.entities} : std::nullopt;
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
        if (value.at("source_snapshot_digest") != review.source_snapshot_digest ||
            value.at("source_authoring_digest") != review.source_authoring_digest ||
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
    if (component_stage) protect_component_review_stage(source.entities(), *component_stage, result.entities, stage.component_mapping);
    else if (stage.phase) protect_phase_source(source.entities(), result.entities);
    else protect_source(source.entities(), result.entities, false, stage.reviewed_erased);
    if (stage.component) for (const auto& id : stage.component->result().retired_dimension_ids)
        if (result.entities.contains(id)) reject("room completion restored an explicitly retired profile dimension");
    if (value.at("version") == 9 || value.at("version") == 10)
        require_selected_members_present(value, result.entities, stage.retired_reviewed_rails);
    if (stage.phase || stage.component) {
        require_selected_members_present(value, result.entities, stage.retired_reviewed_rails,
            stage.phase ? &*stage.phase : nullptr, complete, stage.component ? &stage.component_mapping : nullptr);
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
    if (phase_room_dialect(value)) value["phase_room_reviews"] = phase_reviews;
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
    if (!phase_room_dialect(prepared.intent())) reject("phase room stage requires an explicit phase Properties dialect");
    auto value = prepared.intent(); value["phase_room_reviews"] = reviews;
    auto stage = derive(source, value);
    review_phase_stage(source, value, stage, false);
    const MixedSelectionPropertyEditPreparation preview(source, std::move(value), std::move(stage.entities),
        std::move(stage.reviews), std::move(stage.phase), std::move(stage.phase_reviews),
        std::move(stage.component), std::move(stage.component_mapping),
        std::move(stage.reserved_component_identity_ids), std::move(stage.elided_component_identity_ids));
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
    if (phase_room_dialect(value)) value["phase_room_reviews"] = phase_reviews;
    else if (!phase_reviews.empty()) reject("historical Properties has no phase room completion authority");
    (void)replay_mixed_selection_property_edit(source, value);
    return validate_mixed_selection_property_edit_intent(value);
}
bool mixed_selection_property_edit_has_geometry(const Json& value) {
    validate(value);
    return value.at("version") == 9 || value.at("version") == 10 || phase_room_dialect(value) ||
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
    if (phase_room_dialect(value)) for (const auto& row : value.at("phase_room_reviews"))
        result.push_back(decode_phase_wall_replacement_authoring(row.at("wall_replacement")).room_review_intent);
    return result;
}
bool mixed_selection_property_edit_retires_dependencies(const Json& value) {
    validate(value);
    if (component_dialect(value)) {
        if (value.at("version") == 36 || value.at("version") == 37 || value.at("version") == 38 || value.at("version")==39 || value.at("version")==40 || value.at("version")==41 || value.at("version")==42 || value.at("version")==43)
            for (const auto& [id, decision] : value.at("component_request").at("authoring").at("profile_dimension_decisions").items()) {
                (void)id;
                if (decision.at("operation") == "retire") return true;
            }
        return value.at("version")!=39 && value.at("version")!=40 && value.at("version")!=41 && value.at("version")!=42 && value.at("version")!=43 && !value.at("base_intent").is_null() &&
            mixed_selection_property_edit_retires_dependencies(value.at("base_intent"));
    }
    if (phase_selection_dialect(value)) return std::any_of(
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
    if (value.at("version")==43) return !value.at("component_request").at("authoring").at("allocation").at("roots").empty();
    if (value.at("version")==42) return !value.at("component_request").at("authoring").at("allocation").at("roots").empty() ||
        !value.at("slab_allocation").is_null() || !value.at("structural_allocation").is_null();
    if (value.at("version")==41) return !value.at("component_request").at("authoring").at("allocation").at("roots").empty() ||
        !value.at("structural_allocation").is_null();
    if (value.at("version")==40) return !value.at("component_request").at("authoring").at("allocation").at("roots").empty();
    if (value.at("version")==39) return !value.at("component_request").at("authoring").at("allocation").at("roots").empty() ||
        !value.at("slab_allocation").is_null();
    if (component_dialect(value)) return !value.at("component_request").at("authoring").at("allocation").at("roots").empty() ||
        (!value.at("base_intent").is_null() && mixed_selection_property_edit_has_phase_replacements(value.at("base_intent")));
    return phase_selection_dialect(value) && !value.at("phase_request").at("allocations").empty();
}
std::optional<Revision> mixed_selection_property_edit_source_saved_revision(const Json& value) {
    validate(value);
    return value.at("source_saved_revision").is_null() ? std::nullopt :
        std::optional<Revision>{revision(value.at("source_saved_revision"))};
}
} // namespace sketch
