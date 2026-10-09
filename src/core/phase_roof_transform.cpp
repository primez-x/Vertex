#include "sketch/phase_roof_transform.hpp"
#include "roof_derivation_cache.hpp"

#include "sketch/architecture.hpp"
#include "sketch/constraint_entity.hpp"
#include "sketch/constraint_phase_scope.hpp"
#include "sketch/phase_roof_profile_edit.hpp"
#include "sketch/project_organization.hpp"
#include "sketch/roof_entity_codec.hpp"
#include "sketch/roof_join_phase_ownership.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>
#include <numbers>
#include <set>
#include <stdexcept>
#include <utility>

namespace sketch {
namespace {
using Json = nlohmann::json;
using Entities = std::map<std::string, Entity, std::less<>>;
using Ids = std::set<std::string, std::less<>>;
constexpr std::size_t collection_limit = 4096;
constexpr std::size_t opening_limit = 256;
constexpr std::size_t operation_limit = 256;
constexpr std::size_t proof_limit = 1024 * 1024;
constexpr double scalar_limit = 1.0e12;

[[noreturn]] void invalid(const char* reason) { throw std::invalid_argument(reason); }
void keys(const Json& value, std::initializer_list<const char*> expected) {
    if (!value.is_object() || value.size() != expected.size()) invalid("Roof rigid transform fields are invalid");
    for (const auto* key : expected)
        if (!value.contains(key)) invalid("Roof rigid transform field is missing");
}
const Json* field(const Json& object, const std::string& key) {
    const auto found = object.find(key);
    return found == object.end() ? nullptr : &*found;
}
bool exact(const Json& a, const Json& b) { return a == b && a.dump() == b.dump(); }
bool exact(const Entity& a, const Entity& b) {
    return a == b && exact(a.properties, b.properties) && exact(a.extensions, b.extensions);
}
bool version_one(const Json& value) {
    return (value.is_number_integer() || value.is_number_unsigned()) && value == 1;
}
std::string identity(const Json& value) {
    if (!value.is_string()) invalid("Roof rigid transform identity must be a string");
    const auto& result = value.get_ref<const std::string&>();
    if (result.empty() || result.size() > 128 ||
        !std::all_of(result.begin(), result.end(), [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == ':';
        })) invalid("Roof rigid transform identity is invalid");
    return result;
}
double scalar(double value) {
    if (!std::isfinite(value) || std::abs(value) > scalar_limit)
        invalid("Roof rigid transform exceeds the finite scalar range");
    return value;
}
double scalar(const Json& value) {
    if (!value.is_number()) invalid("Roof rigid transform scalar must be numeric");
    return scalar(value.get<double>());
}
Vec3 point(const Json& value) {
    if (!value.is_array() || value.size() != 3) invalid("Roof rigid transform point must have three scalars");
    return {scalar(value.at(0)), scalar(value.at(1)), scalar(value.at(2))};
}
void budget_nodes(const Json& value, std::size_t& nodes, std::size_t depth = 0) {
    if (depth > 32 || ++nodes > 65536) invalid("Roof rigid transform proof structural budget exceeded");
    if (value.is_number_float() && !std::isfinite(value.get<double>()))
        invalid("Roof rigid transform proof contains a nonfinite scalar");
    if (value.is_array() || value.is_object())
        for (const auto& child : value) budget_nodes(child, nodes, depth + 1);
}
std::size_t budget(const Json& value) {
    std::size_t nodes = 0;
    budget_nodes(value, nodes);
    const auto bytes = value.dump().size();
    if (bytes > proof_limit) invalid("Roof rigid transform proof byte budget exceeded");
    return bytes;
}
std::array<double, 2> rotation(double angle) {
    if (std::abs(angle) == std::numbers::pi) return {-1.0, 0.0};
    return {std::cos(angle), std::sin(angle)};
}
struct Normalized {
    double x{}, y{}, z{}, angle{};
    bool horizontal{}, vertical{}, identity{};
};
Normalized normalize(const ArchitecturalGroupTransform& value) {
    for (auto n : {value.pivot.x, value.pivot.y, value.pivot.z,
             value.offset.x, value.offset.y, value.offset.z, value.rotation_z_radians}) (void)scalar(n);
    if (value.scale != 1.0) invalid("Roof rigid transform requires an exact unit scale");
    const bool reflected = value.flip_horizontal != value.flip_vertical;
    const double angle = std::remainder(std::remainder(value.rotation_z_radians,
        2.0 * std::numbers::pi) + (value.flip_horizontal && value.flip_vertical
            ? std::numbers::pi : 0.0), 2.0 * std::numbers::pi);
    const auto [c, s] = rotation(angle);
    const bool horizontal = reflected && value.flip_horizontal;
    const bool vertical = reflected && value.flip_vertical;
    const double hx = horizontal ? -1.0 : 1.0, hy = vertical ? -1.0 : 1.0;
    // Same difference-form normalization as architectural_group_transform_command.
    const double x = scalar(value.offset.x + (1.0 - hx) * value.pivot.x +
        hx * ((1.0 - c) * value.pivot.x + s * value.pivot.y));
    const double y = scalar(value.offset.y + (1.0 - hy) * value.pivot.y +
        hy * ((1.0 - c) * value.pivot.y - s * value.pivot.x));
    return {x, y, value.offset.z, angle, horizontal, vertical,
        value.offset.x == 0.0 && value.offset.y == 0.0 && value.offset.z == 0.0 && angle == 0.0 && !reflected};
}

// Closed mathematical frame, extracted only from an admitted actual roof.
// Opaque owner/child fields never acquire authority through the proof wire.
Json frame(const Entity& source) {
    const auto& p = source.properties;
    Json cuts = Json::array();
    if (const auto rows = field(p, "roof_openings"))
        for (const auto& row : *rows)
            cuts.push_back({{"id", row.at("id")}, {"y_m", row.at("y_m")}, {"depth_m", row.at("depth_m")}});
    return {{"form", p.at("form")}, {"base_position_m", p.at("base_position_m")},
        {"orientation_rad", p.at("orientation_rad")}, {"span_m", p.at("span_m")}, {"cuts", std::move(cuts)}};
}
void admit_frame(const Json& value) {
    keys(value, {"form", "base_position_m", "orientation_rad", "span_m", "cuts"});
    if (!value.at("form").is_string() || (value.at("form") != "sloped_roof_panel" &&
        value.at("form") != "gable_roof" && value.at("form") != "hip_roof"))
        invalid("Roof rigid transform proof form is unsupported");
    (void)point(value.at("base_position_m"));
    (void)scalar(value.at("orientation_rad"));
    if (!(scalar(value.at("span_m")) > 0.0)) invalid("Roof rigid transform proof span must be positive");
    const auto& cuts = value.at("cuts");
    if (!cuts.is_array() || cuts.size() > opening_limit) invalid("Roof rigid transform proof cut budget exceeded");
    Ids ids;
    for (const auto& cut : cuts) {
        keys(cut, {"id", "y_m", "depth_m"});
        if (!ids.insert(identity(cut.at("id"))).second) invalid("Roof rigid transform proof has duplicate cut IDs");
        (void)scalar(cut.at("y_m"));
        if (!(scalar(cut.at("depth_m")) > 0.0)) invalid("Roof rigid transform cut depth must be positive");
    }
}
void assign_if_changed(Json& target, double value) {
    (void)scalar(value);
    if (scalar(target) != value) target = value;
}
Json derive(const Json& before, const ArchitecturalGroupTransform& transform) {
    admit_frame(before);
    const auto t = normalize(transform);
    if (t.identity) return before;
    auto result = before;
    const auto p = point(before.at("base_position_m"));
    const auto [c, s] = rotation(t.angle);
    const double hx = t.horizontal ? -1.0 : 1.0, hy = t.vertical ? -1.0 : 1.0;
    double x = hx * (c * p.x - s * p.y) + t.x;
    double y = hy * (s * p.x + c * p.y) + t.y;
    const bool reflected = t.horizontal != t.vertical;
    const double yaw = scalar(before.at("orientation_rad"));
    double heading = yaw + t.angle;
    if (reflected) {
        heading = std::atan2(hy * (s * std::cos(yaw) + c * std::sin(yaw)),
            hx * (c * std::cos(yaw) - s * std::sin(yaw)));
        if (before.at("form") == "sloped_roof_panel") {
            const double span = scalar(before.at("span_m"));
            x += span * std::sin(heading);
            y -= span * std::cos(heading);
        }
        for (auto& cut : result.at("cuts")) {
            const double old_y = scalar(cut.at("y_m")), depth = scalar(cut.at("depth_m"));
            assign_if_changed(cut.at("y_m"), before.at("form") == "sloped_roof_panel"
                ? scalar(before.at("span_m")) - old_y - depth : -old_y - depth);
        }
    }
    auto& position = result.at("base_position_m");
    assign_if_changed(position.at(0), x);
    assign_if_changed(position.at(1), y);
    assign_if_changed(position.at(2), p.z + t.z);
    assign_if_changed(result.at("orientation_rad"), heading);
    admit_frame(result);
    return result;
}
using Changes = std::map<std::string, std::pair<double, double>, std::less<>>;
Changes changes(const Json& before, const Json& after) {
    Changes result;
    const auto add = [&](const std::string& path, const Json& a, const Json& b) {
        if (scalar(a) != scalar(b)) result.emplace(path, std::pair{scalar(a), scalar(b)});
    };
    for (std::size_t i = 0; i < 3; ++i)
        add("/base_position_m/" + std::to_string(i), before.at("base_position_m").at(i), after.at("base_position_m").at(i));
    add("/orientation_rad", before.at("orientation_rad"), after.at("orientation_rad"));
    for (std::size_t i = 0; i < before.at("cuts").size(); ++i)
        add("/roof_openings/" + std::to_string(i) + "/y_m", before.at("cuts").at(i).at("y_m"), after.at("cuts").at(i).at("y_m"));
    return result;
}
bool descendant(std::string_view path, std::string_view parent) {
    return path.size() > parent.size() && path.starts_with(parent) && path[parent.size()] == '/';
}
bool known_raw(const Json& value) {
    const auto version = field(value, "version");
    return value.is_object() && (!version || version_one(*version));
}
void receipt_budget(const Json& receipt) {
    const auto expression = field(receipt, "original_expression");
    if (!expression || !expression->is_string() || expression->get_ref<const std::string&>().size() > 4096)
        invalid("Roof rigid transform receipt expression budget exceeded");
    (void)budget(receipt);
}
void admit_quantity_receipt(const Json& receipt, double actual) {
    receipt_budget(receipt);
    const auto version = field(receipt, "version");
    if (!receipt.is_object() || !version || !version_one(*version))
        invalid("Roof rigid transform affects an opaque quantity receipt");
    if (decode_constraint_quantity_receipt(receipt).metres != actual)
        invalid("Roof rigid transform quantity receipt is stale");
}
Unit unit(const Json& value) {
    if (!value.is_string()) invalid("Roof rigid transform receipt unit is invalid");
    const auto& name = value.get_ref<const std::string&>();
    if (name == "m") return Unit::metre;
    if (name == "mm") return Unit::millimetre;
    if (name == "cm") return Unit::centimetre;
    if (name == "ft") return Unit::foot;
    if (name == "in") return Unit::inch;
    invalid("Roof rigid transform receipt unit is unsupported");
}
const char* unit_name(Unit value) {
    switch (value) {
    case Unit::metre: return "m";
    case Unit::millimetre: return "mm";
    case Unit::centimetre: return "cm";
    case Unit::foot: return "ft";
    case Unit::inch: return "in";
    }
    invalid("Roof rigid transform receipt unit is unsupported");
}
void admit_opening_receipt(const Json& raw, double actual) {
    if (!known_raw(raw)) invalid("Roof rigid transform affects an opaque child receipt");
    receipt_budget(raw);
    const auto default_unit = field(raw, "default_unit"), rational = field(raw, "exact_metres");
    if (!default_unit || !rational) invalid("Roof rigid transform opening receipt core is missing");
    const auto parsed = parse_quantity(raw.at("original_expression").get_ref<const std::string&>(), unit(*default_unit));
    const Json core{{"version", 1}, {"original_expression", parsed.original_expression},
        {"entered_unit", unit_name(parsed.entered_unit)}, {"exact_metres", *rational}};
    admit_quantity_receipt(core, actual);
}
Json archive_receipts(const Entity& source, Entity& result, const Json& before, const Changes& affected) {
    Json indexed = Json::object(), children = Json::object();
    if (const auto entries = field(source.properties, "quantity_entries")) {
        if (!entries->is_object() || entries->size() > collection_limit)
            invalid("Roof rigid transform quantity_entries budget exceeded");
        for (const auto& [pointer, receipt] : entries->items())
            for (const auto& [changed, values] : affected) {
                if (pointer != changed && !descendant(pointer, changed) && !descendant(changed, pointer)) continue;
                if (pointer != changed || changed == "/orientation_rad")
                    invalid("Roof rigid transform affects an unsupported quantity binding");
                admit_quantity_receipt(receipt, values.first);
                indexed[pointer] = receipt;
                result.properties.at("quantity_entries").erase(pointer);
            }
    }
    const auto extension = field(source.extensions, "roof_opening_input");
    for (std::size_t i = 0; i < before.at("cuts").size(); ++i) {
        if (!affected.contains("/roof_openings/" + std::to_string(i) + "/y_m") || !extension) continue;
        const auto version = field(*extension, "version");
        if (!extension->is_object() || !version || !version_one(*version))
            invalid("Roof rigid transform affects an opaque opening input envelope");
        const auto entries = field(*extension, "entries");
        if (!entries || !entries->is_object() || entries->size() > collection_limit)
            invalid("Roof rigid transform opening input entries budget exceeded");
        const auto id = identity(before.at("cuts").at(i).at("id"));
        const auto child = field(*entries, id);
        if (!child) continue;
        if (!known_raw(*child)) invalid("Roof rigid transform affects an opaque child input envelope");
        if (const auto receipt = field(*child, "y_m")) {
            admit_opening_receipt(*receipt, scalar(before.at("cuts").at(i).at("y_m")));
            children[id] = {{"y_m", *receipt}};
            // Preserve every child/envelope sibling, including empty objects.
            result.extensions.at("roof_opening_input").at("entries").at(id).erase("y_m");
        }
    }
    return {{"quantity_entries", std::move(indexed)}, {"roof_opening_input", std::move(children)}};
}
void admit_record(const Json& record) {
    keys(record, {"operation", "source", "result", "receipts"});
    const auto intent = decode_roof_rigid_transform_intent(record.at("operation"));
    const auto& before = record.at("source"), after = record.at("result");
    if (!exact(derive(before, intent.transform), after) || exact(before, after))
        invalid("Roof rigid transform derivation does not reproduce its result");
    const auto affected = changes(before, after);
    const auto& receipts = record.at("receipts");
    keys(receipts, {"quantity_entries", "roof_opening_input"});
    const auto& indexed = receipts.at("quantity_entries"), children = receipts.at("roof_opening_input");
    if (!indexed.is_object() || !children.is_object() || indexed.size() > collection_limit || children.size() > opening_limit)
        invalid("Roof rigid transform archived receipt budget exceeded");
    for (const auto& [pointer, receipt] : indexed.items()) {
        const auto changed = affected.find(pointer);
        if (changed == affected.end() || pointer == "/orientation_rad")
            invalid("Roof rigid transform archive contains an unaffected or unsupported binding");
        admit_quantity_receipt(receipt, changed->second.first);
    }
    for (const auto& [id, values] : children.items()) {
        (void)identity(id);
        keys(values, {"y_m"});
        bool found = false;
        for (std::size_t i = 0; i < before.at("cuts").size(); ++i) {
            const auto& cut = before.at("cuts").at(i);
            if (cut.at("id") != id) continue;
            if (!affected.contains("/roof_openings/" + std::to_string(i) + "/y_m"))
                invalid("Roof rigid transform archived cut receipt is unaffected");
            admit_opening_receipt(values.at("y_m"), scalar(cut.at("y_m")));
            found = true;
        }
        if (!found) invalid("Roof rigid transform archive has a dangling cut receipt");
    }
}
Json receipt_opaque_remainder(Json receipt, bool opening) {
    // Only admitted current-schema fields have understood receipt meaning.
    // Copy/erase retains every unknown sibling and its exact nested JSON.
    for (const auto* key : {"version", "original_expression"}) receipt.erase(key);
    receipt.erase(opening ? "default_unit" : "entered_unit");
    auto& rational = receipt.at("exact_metres");
    rational.erase("numerator");
    rational.erase("denominator");
    return receipt;
}
TopoDS_Shape resolved_shape(const Entities& source, const std::string& id) {
    const auto found = source.find(id);
    if (found == source.end() || found->first != found->second.id || found->second.type != "roof")
        invalid("Roof rigid transform actual source roof is missing or inconsistent");
    validate_roof_rigid_transform_source_entity(found->second);
    return make_roof_shape(decode_roof_entity(resolve_vertical_placement(source, found->second)));
}
std::vector<RoofJoin> affected_joins(const Entities& source, const Ids& targets, const ConstraintPhaseScope& scope) {
    const auto qualified_cohorts = phase_qualified_roof_join_cohort_ids(source);
    std::vector<RoofJoin> result;
    std::map<std::string, std::string, std::less<>> owners;
    for (const auto& [id, entity] : source) {
        if (entity.id != id) invalid("Roof rigid transform actual map contains inconsistent identities");
        if (entity.type != "roof_join") continue;
        const auto join = parse_roof_join(entity.properties, id);
        if (scope.inactive_owner_ids.contains(id) && qualified_cohorts.contains(id)) continue;
        const bool affected = std::any_of(join.roof_ids.begin(), join.roof_ids.end(),
            [&](const auto& member) { return targets.contains(member); });
        for (const auto& member : join.roof_ids) {
            const auto roof = source.find(member);
            if (roof == source.end() || roof->second.type != "roof" || roof->second.id != member)
                invalid("Roof rigid transform source has a dangling join member");
            if (!owners.emplace(member, id).second) invalid("Roof rigid transform source has overlapping joins");
            if (affected && scope.inactive_owner_ids.contains(member))
                invalid("Roof rigid transform affected join member is inactive");
        }
        if (!affected) continue;
        if (scope.inactive_owner_ids.contains(id)) invalid("Roof rigid transform affected join is inactive");
        if (result.size() == collection_limit) invalid("Roof rigid transform affected join budget exceeded");
        result.push_back(join);
    }
    return result;
}
void admit_joins(const Entities& source, const std::vector<RoofJoin>& joins) {
    validate_roof_join_ownership(source);
    for (const auto& join : joins) {
        std::vector<TopoDS_Shape> members;
        for (const auto& id : join.roof_ids) members.push_back(resolved_shape(source, id));
        (void)make_roof_join(join, members);
    }
}
} // namespace

nlohmann::json encode_roof_rigid_transform_intent(const RoofRigidTransformIntent& intent) {
    (void)identity(intent.roof_id);
    (void)normalize(intent.transform);
    const auto& t = intent.transform;
    Json result{{"version", 1}, {"roof_id", intent.roof_id}, {"transform", {
        {"pivot_m", {t.pivot.x, t.pivot.y, t.pivot.z}}, {"offset_m", {t.offset.x, t.offset.y, t.offset.z}},
        {"rotation_z_radians", t.rotation_z_radians}, {"scale", t.scale},
        {"flip_horizontal", t.flip_horizontal}, {"flip_vertical", t.flip_vertical}}}};
    (void)budget(result);
    return result;
}
RoofRigidTransformIntent decode_roof_rigid_transform_intent(const nlohmann::json& value) {
    (void)budget(value);
    keys(value, {"version", "roof_id", "transform"});
    if (!version_one(value.at("version"))) invalid("Roof rigid transform version is unsupported");
    const auto& t = value.at("transform");
    keys(t, {"pivot_m", "offset_m", "rotation_z_radians", "scale", "flip_horizontal", "flip_vertical"});
    if (!t.at("flip_horizontal").is_boolean() || !t.at("flip_vertical").is_boolean())
        invalid("Roof rigid transform reflection flags must be booleans");
    RoofRigidTransformIntent result{identity(value.at("roof_id")),
        {point(t.at("pivot_m")), point(t.at("offset_m")), scalar(t.at("rotation_z_radians")),
            scalar(t.at("scale")), t.at("flip_horizontal").get<bool>(), t.at("flip_vertical").get<bool>()}};
    (void)normalize(result.transform);
    return result;
}
void validate_roof_rigid_transform_derivations(const Entity& source) {
    if (!source.extensions.is_object()) invalid("Roof rigid transform extensions must be an object");
    const auto extension = field(source.extensions, std::string(roof_rigid_transform_derivations_key));
    if (!extension) return;
    (void)budget(*extension);
    keys(*extension, {"version", "operations"});
    if (!version_one(extension->at("version"))) invalid("Roof rigid transform derivation namespace collision");
    const auto& operations = extension->at("operations");
    if (!operations.is_array() || operations.empty() || operations.size() > operation_limit)
        invalid("Roof rigid transform derivation operation budget exceeded");
    validate_roof_derivation_cached("roof-rigid-transform-v1", extension->dump(), [&] {
        for (const auto& record : operations) admit_record(record);
    });
}
nlohmann::json roof_rigid_transform_opaque_remainder(const Entity& source) {
    validate_roof_rigid_transform_derivations(source);
    const auto extension = field(source.extensions, std::string(roof_rigid_transform_derivations_key));
    if (!extension) return Json::object();
    Json operations = Json::array();
    for (const auto& record : extension->at("operations")) {
        const auto& receipts = record.at("receipts");
        Json indexed = Json::object(), children = Json::array();
        for (const auto& [pointer, receipt] : receipts.at("quantity_entries").items())
            indexed[pointer] = receipt_opaque_remainder(receipt, false);
        for (const auto& [id, values] : receipts.at("roof_opening_input").items()) {
            (void)id; // The outer key is the admitted historical child identity.
            children.push_back({{"y_m", receipt_opaque_remainder(values.at("y_m"), true)}});
        }
        // Operation and frame envelopes are closed; their only strings are
        // admitted historical IDs and known form tokens. They leave no opaque
        // fields. Receipt containers are closed too, except for the full raw
        // receipts whose unknown payload remains below.
        operations.push_back({{"receipts", {{"quantity_entries", std::move(indexed)},
            {"roof_opening_input", std::move(children)}}}});
    }
    return {{"operations", std::move(operations)}};
}
void validate_roof_rigid_transform_source_entity(const Entity& source) {
    validate_roof_profile_source_entity(source);
    admit_frame(frame(source));
    validate_roof_rigid_transform_derivations(source);
}
Entity stage_roof_rigid_transform_entity(const Entity& source, const RoofRigidTransformIntent& intent) {
    const auto operation = encode_roof_rigid_transform_intent(intent);
    if (source.id != intent.roof_id) invalid("Roof rigid transform differs from its actual source owner");
    validate_roof_rigid_transform_source_entity(source);
    const auto before = frame(source), after = derive(before, intent.transform);
    if (exact(before, after)) return source;
    auto result = source;
    result.properties.at("base_position_m") = after.at("base_position_m");
    result.properties.at("orientation_rad") = after.at("orientation_rad");
    if (result.properties.contains("roof_openings"))
        for (std::size_t i = 0; i < after.at("cuts").size(); ++i)
            result.properties.at("roof_openings").at(i).at("y_m") = after.at("cuts").at(i).at("y_m");
    const auto receipts = archive_receipts(source, result, before, changes(before, after));
    const auto key = std::string(roof_rigid_transform_derivations_key);
    if (!result.extensions.contains(key)) result.extensions[key] = {{"version", 1}, {"operations", Json::array()}};
    auto& operations = result.extensions.at(key).at("operations");
    if (operations.size() == operation_limit) invalid("Roof rigid transform derivation operation budget exceeded");
    operations.push_back({{"operation", operation}, {"source", before}, {"result", after}, {"receipts", receipts}});
    validate_roof_rigid_transform_derivations(result);
    return result;
}
Entity replay_roof_rigid_transform_entity(const Entity& source, const RoofRigidTransformIntent& intent) {
    auto result = stage_roof_rigid_transform_entity(source, intent);
    validate_roof_rigid_transform_source_entity(result);
    return result;
}
std::map<std::string, Entity, std::less<>> replay_roof_rigid_transform_entities(
    const std::map<std::string, Entity, std::less<>>& source,
    const std::vector<RoofRigidTransformIntent>& intents) {
    if (intents.size() > maximum_architectural_group_targets) invalid("Roof rigid transform target budget exceeded");
    if (intents.empty()) return source;
    const auto scope = constraint_phase_scope(source);
    Ids targets;
    std::size_t operation_bytes = 0, archive_bytes = 0;
    for (const auto& intent : intents) {
        const auto bytes = budget(encode_roof_rigid_transform_intent(intent));
        if (bytes > proof_limit - operation_bytes) invalid("Roof rigid transform batch operation budget exceeded");
        operation_bytes += bytes;
        if (!targets.insert(intent.roof_id).second) invalid("Roof rigid transform contains duplicate targets");
        if (scope.inactive_owner_ids.contains(intent.roof_id)) invalid("Roof rigid transform target is inactive");
        (void)resolved_shape(source, intent.roof_id);
    }
    const auto joins = affected_joins(source, targets, scope);
    admit_joins(source, joins);
    auto result = source;
    for (const auto& intent : intents) {
        auto roof = replay_roof_rigid_transform_entity(source.at(intent.roof_id), intent);
        if (const auto archive = field(roof.extensions, std::string(roof_rigid_transform_derivations_key))) {
            const auto bytes = budget(*archive);
            if (bytes > proof_limit - archive_bytes) invalid("Roof rigid transform batch archive budget exceeded");
            archive_bytes += bytes;
        }
        result.at(intent.roof_id) = std::move(roof);
    }
    for (const auto& id : targets) (void)resolved_shape(result, id);
    admit_joins(result, joins);
    return result;
}
std::optional<RoofRigidTransformIntent> capture_roof_rigid_transform(
    const Entity& original, const Entity& candidate, const ArchitecturalGroupTransform& actual_transform) {
    const RoofRigidTransformIntent intent{original.id, actual_transform};
    const auto expected = replay_roof_rigid_transform_entity(original, intent);
    if (!exact(expected, candidate)) invalid("Roof rigid transform candidate differs from complete actual-source replay");
    if (exact(original, expected)) return std::nullopt;
    return intent;
}
} // namespace sketch
