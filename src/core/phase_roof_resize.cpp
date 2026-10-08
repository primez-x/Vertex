#include "sketch/phase_roof_resize.hpp"
#include "roof_derivation_cache.hpp"

#include "sketch/architecture.hpp"
#include "sketch/constraint_entity.hpp"
#include "sketch/constraint_phase_scope.hpp"
#include "sketch/phase_roof_transform.hpp"
#include "sketch/plan_axis_resize.hpp"
#include "sketch/project_organization.hpp"
#include "sketch/roof_entity_codec.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>
#include <set>
#include <stdexcept>
#include <utility>

namespace sketch {
namespace {
using Json = nlohmann::json;
using Entities = std::map<std::string, Entity, std::less<>>;
using Ids = std::set<std::string, std::less<>>;
using Changes = std::map<std::string, std::pair<double, double>, std::less<>>;
constexpr std::size_t collection_limit = 4096;
constexpr std::size_t opening_limit = 256;
constexpr std::size_t operation_limit = 256;
constexpr std::size_t proof_limit = 1024 * 1024;
constexpr std::size_t string_limit = 16384;
constexpr double scalar_limit = 1.0e12;
constexpr std::array opening_scalars{"x_m", "y_m", "width_m", "depth_m"};

[[noreturn]] void invalid(const char* reason) { throw std::invalid_argument(reason); }
void keys(const Json& value, std::initializer_list<const char*> expected) {
    if (!value.is_object() || value.size() != expected.size()) invalid("Roof plan resize fields are invalid");
    for (const auto* key : expected)
        if (!value.contains(key)) invalid("Roof plan resize field is missing");
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
    if (!value.is_string()) invalid("Roof plan resize identity must be a string");
    const auto& result = value.get_ref<const std::string&>();
    if (result.empty() || result.size() > 128 ||
        !std::all_of(result.begin(), result.end(), [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == ':';
        })) invalid("Roof plan resize identity is invalid");
    return result;
}
double scalar(double value) {
    if (!std::isfinite(value) || std::abs(value) > scalar_limit)
        invalid("Roof plan resize exceeds the finite scalar range");
    return value;
}
double scalar(const Json& value) {
    if (!value.is_number()) invalid("Roof plan resize scalar must be numeric");
    return scalar(value.get<double>());
}
Vec2 anchor(const Json& value) {
    if (!value.is_array() || value.size() != 2) invalid("Roof plan resize anchor must have two scalars");
    return {scalar(value.at(0)), scalar(value.at(1))};
}
void admit_intent(const RoofPlanResizeIntent& value) {
    (void)identity(value.roof_id);
    if (!(scalar(value.scale_x) > 0.0) || !(scalar(value.scale_y) > 0.0))
        invalid("Roof plan resize factors must be positive");
    (void)scalar(value.anchor.x);
    (void)scalar(value.anchor.y);
    (void)scalar(value.frame_rotation_radians);
}
void budget_nodes(const Json& value, std::size_t& nodes, std::size_t depth = 0) {
    if (depth > 32 || ++nodes > 65536) invalid("Roof plan resize proof structural budget exceeded");
    if ((value.is_array() || value.is_object()) && value.size() > collection_limit)
        invalid("Roof plan resize proof collection budget exceeded");
    if (value.is_number_float() && !std::isfinite(value.get<double>()))
        invalid("Roof plan resize proof contains a nonfinite scalar");
    if (value.is_string() && value.get_ref<const std::string&>().size() > string_limit)
        invalid("Roof plan resize proof string budget exceeded");
    if (value.is_object())
        for (const auto& [key, child] : value.items()) {
            if (key.size() > string_limit) invalid("Roof plan resize proof key budget exceeded");
            budget_nodes(child, nodes, depth + 1);
        }
    else if (value.is_array())
        for (const auto& child : value) budget_nodes(child, nodes, depth + 1);
}
std::size_t budget(const Json& value) {
    std::size_t nodes = 0;
    budget_nodes(value, nodes);
    const auto bytes = value.dump().size();
    if (bytes > proof_limit) invalid("Roof plan resize proof byte budget exceeded");
    return bytes;
}

// A closed physical frame is historical evidence only. Opaque entity data is
// excluded; it never obtains live replacement or command authority here.
Json frame(const Entity& source) {
    const auto& p = source.properties;
    Json result;
    for (const auto* key : {"version", "form", "base_position_m", "orientation_rad", "span_m",
             "rise_m", "pitch_rad", "overhang_m", "thickness_m"}) result[key] = p.at(key);
    const auto* length = p.at("form") == "sloped_roof_panel" ? "run_m" : "length_m";
    result[length] = p.at(length);
    if (p.contains("roof_openings")) {
        result["roof_openings"] = Json::array();
        for (const auto& row : p.at("roof_openings")) {
            Json cut{{"id", row.at("id")}};
            for (const auto* key : opening_scalars) cut[key] = row.at(key);
            result.at("roof_openings").push_back(std::move(cut));
        }
    }
    return result;
}
Entity physical_entity(const Json& value, const std::string& id) {
    Entity result;
    result.id = id;
    result.type = "roof";
    result.properties = value;
    return result;
}
void admit_frame(const Json& value, const std::string& id) {
    (void)budget(value);
    if (!value.is_object() || !value.contains("form") || !value.at("form").is_string())
        invalid("Roof plan resize proof form is missing");
    const bool panel = value.at("form") == "sloped_roof_panel";
    if (!panel && value.at("form") != "gable_roof" && value.at("form") != "hip_roof")
        invalid("Roof plan resize proof form is unsupported");
    const auto schema = field(value, "version");
    if (!schema || (!schema->is_number_integer() && !schema->is_number_unsigned()) ||
        (*schema != 1 && *schema != 2)) invalid("Roof plan resize proof schema is unsupported");
    const bool openings = *schema == 2;
    if (panel && openings)
        keys(value, {"version", "form", "base_position_m", "orientation_rad", "run_m", "span_m",
            "rise_m", "pitch_rad", "overhang_m", "thickness_m", "roof_openings"});
    else if (panel)
        keys(value, {"version", "form", "base_position_m", "orientation_rad", "run_m", "span_m",
            "rise_m", "pitch_rad", "overhang_m", "thickness_m"});
    else if (openings)
        keys(value, {"version", "form", "base_position_m", "orientation_rad", "length_m", "span_m",
            "rise_m", "pitch_rad", "overhang_m", "thickness_m", "roof_openings"});
    else
        keys(value, {"version", "form", "base_position_m", "orientation_rad", "length_m", "span_m",
            "rise_m", "pitch_rad", "overhang_m", "thickness_m"});
    const auto& position = value.at("base_position_m");
    if (!position.is_array() || position.size() != 3) invalid("Roof plan resize proof position is invalid");
    for (const auto& coordinate : position) (void)scalar(coordinate);
    for (const auto* key : {"orientation_rad", "span_m", "rise_m", "pitch_rad", "overhang_m", "thickness_m"})
        (void)scalar(value.at(key));
    (void)scalar(value.at(panel ? "run_m" : "length_m"));
    if (openings) {
        const auto& rows = value.at("roof_openings");
        if (!rows.is_array() || rows.size() > opening_limit) invalid("Roof plan resize proof cut budget exceeded");
        Ids ids;
        for (const auto& cut : rows) {
            keys(cut, {"id", "x_m", "y_m", "width_m", "depth_m"});
            if (!ids.insert(identity(cut.at("id"))).second) invalid("Roof plan resize proof has duplicate cut IDs");
            for (const auto* key : opening_scalars) (void)scalar(cut.at(key));
        }
    }
    (void)make_roof_shape(decode_roof_entity(physical_entity(value, id)));
}
Json derive(const Json& before, const RoofPlanResizeIntent& intent) {
    admit_intent(intent);
    admit_frame(before, intent.roof_id);
    const auto result = frame(stage_roof_plan_axis_resize_entity(physical_entity(before, intent.roof_id),
        intent.scale_x, intent.scale_y, intent.anchor, intent.frame_rotation_radians));
    admit_frame(result, intent.roof_id);
    return result;
}
Changes changes(const Json& before, const Json& after) {
    Changes result;
    const auto add = [&](const std::string& path, const Json& a, const Json& b) {
        if (scalar(a) != scalar(b)) result.emplace(path, std::pair{scalar(a), scalar(b)});
    };
    for (std::size_t i = 0; i < 2; ++i)
        add("/base_position_m/" + std::to_string(i), before.at("base_position_m").at(i), after.at("base_position_m").at(i));
    for (const auto* key : {"run_m", "length_m", "span_m", "pitch_rad"})
        if (before.contains(key)) add("/" + std::string(key), before.at(key), after.at(key));
    if (before.contains("roof_openings"))
        for (std::size_t i = 0; i < before.at("roof_openings").size(); ++i)
            for (const auto* key : opening_scalars)
                add("/roof_openings/" + std::to_string(i) + "/" + key,
                    before.at("roof_openings").at(i).at(key), after.at("roof_openings").at(i).at(key));
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
        invalid("Roof plan resize receipt expression budget exceeded");
    (void)budget(receipt);
}
void admit_quantity_receipt(const Json& receipt, double actual) {
    receipt_budget(receipt);
    const auto version = field(receipt, "version");
    if (!receipt.is_object() || !version || !version_one(*version))
        invalid("Roof plan resize affects an opaque quantity receipt");
    if (decode_constraint_quantity_receipt(receipt).metres != actual)
        invalid("Roof plan resize quantity receipt is stale");
}
Unit unit(const Json& value) {
    if (!value.is_string()) invalid("Roof plan resize receipt unit is invalid");
    const auto& name = value.get_ref<const std::string&>();
    if (name == "m") return Unit::metre;
    if (name == "mm") return Unit::millimetre;
    if (name == "cm") return Unit::centimetre;
    if (name == "ft") return Unit::foot;
    if (name == "in") return Unit::inch;
    invalid("Roof plan resize receipt unit is unsupported");
}
const char* unit_name(Unit value) {
    switch (value) {
    case Unit::metre: return "m";
    case Unit::millimetre: return "mm";
    case Unit::centimetre: return "cm";
    case Unit::foot: return "ft";
    case Unit::inch: return "in";
    }
    invalid("Roof plan resize receipt unit is unsupported");
}
void admit_opening_receipt(const Json& raw, double actual) {
    if (!known_raw(raw)) invalid("Roof plan resize affects an opaque child receipt");
    receipt_budget(raw);
    const auto default_unit = field(raw, "default_unit"), rational = field(raw, "exact_metres");
    if (!default_unit || !rational) invalid("Roof plan resize opening receipt core is missing");
    const auto parsed = parse_quantity(raw.at("original_expression").get_ref<const std::string&>(), unit(*default_unit));
    const Json core{{"version", 1}, {"original_expression", parsed.original_expression},
        {"entered_unit", unit_name(parsed.entered_unit)}, {"exact_metres", *rational}};
    admit_quantity_receipt(core, actual);
}
Json archive_receipts(const Entity& source, Entity& result, const Json& before, const Changes& affected) {
    Json indexed = Json::object(), children = Json::object();
    if (const auto entries = field(source.properties, "quantity_entries")) {
        if (!entries->is_object() || entries->size() > collection_limit)
            invalid("Roof plan resize quantity_entries budget exceeded");
        for (const auto& [pointer, receipt] : entries->items())
            for (const auto& [changed, values] : affected) {
                if (pointer != changed && !descendant(pointer, changed) && !descendant(changed, pointer)) continue;
                if (pointer != changed || changed == "/pitch_rad")
                    invalid("Roof plan resize affects an unsupported quantity binding");
                admit_quantity_receipt(receipt, values.first);
                indexed[pointer] = receipt;
                result.properties.at("quantity_entries").erase(pointer);
            }
    }
    const auto extension = field(source.extensions, "roof_opening_input");
    if (before.contains("roof_openings"))
        for (std::size_t i = 0; i < before.at("roof_openings").size(); ++i) {
            for (const auto* key : opening_scalars) {
                const auto changed = affected.find("/roof_openings/" + std::to_string(i) + "/" + key);
                if (changed == affected.end() || !extension) continue;
                const auto version = field(*extension, "version");
                if (!extension->is_object() || !version || !version_one(*version))
                    invalid("Roof plan resize affects an opaque opening input envelope");
                const auto entries = field(*extension, "entries");
                if (!entries || !entries->is_object() || entries->size() > collection_limit)
                    invalid("Roof plan resize opening input entries budget exceeded");
                const auto id = identity(before.at("roof_openings").at(i).at("id"));
                const auto child = field(*entries, id);
                if (!child) continue;
                if (!known_raw(*child)) invalid("Roof plan resize affects an opaque child input envelope");
                if (const auto receipt = field(*child, key)) {
                    admit_opening_receipt(*receipt, changed->second.first);
                    children[id][key] = *receipt;
                    // Keep every envelope/child sibling, even empty objects.
                    result.extensions.at("roof_opening_input").at("entries").at(id).erase(key);
                }
            }
        }
    return {{"quantity_entries", std::move(indexed)}, {"roof_opening_input", std::move(children)}};
}
void admit_record(const Json& record) {
    keys(record, {"operation", "source", "result", "receipts"});
    const auto intent = decode_roof_plan_resize_intent(record.at("operation"));
    const auto& before = record.at("source"), after = record.at("result");
    if (!exact(derive(before, intent), after) || exact(before, after))
        invalid("Roof plan resize derivation does not reproduce its result");
    const auto affected = changes(before, after);
    const auto& receipts = record.at("receipts");
    keys(receipts, {"quantity_entries", "roof_opening_input"});
    const auto& indexed = receipts.at("quantity_entries"), children = receipts.at("roof_opening_input");
    if (!indexed.is_object() || !children.is_object() || indexed.size() > collection_limit || children.size() > opening_limit)
        invalid("Roof plan resize archived receipt budget exceeded");
    for (const auto& [pointer, receipt] : indexed.items()) {
        const auto changed = affected.find(pointer);
        if (changed == affected.end() || pointer == "/pitch_rad")
            invalid("Roof plan resize archive contains an unaffected or unsupported binding");
        admit_quantity_receipt(receipt, changed->second.first);
    }
    for (const auto& [id, values] : children.items()) {
        (void)identity(id);
        if (!values.is_object() || values.empty() || values.size() > opening_scalars.size())
            invalid("Roof plan resize archived cut fields are invalid");
        const Json* cut = nullptr;
        std::size_t index = 0;
        if (before.contains("roof_openings"))
            for (std::size_t i = 0; i < before.at("roof_openings").size(); ++i)
                if (before.at("roof_openings").at(i).at("id") == id) {
                    cut = &before.at("roof_openings").at(i);
                    index = i;
                }
        if (!cut) invalid("Roof plan resize archive has a dangling cut receipt");
        for (const auto& [key, receipt] : values.items()) {
            if (std::find(opening_scalars.begin(), opening_scalars.end(), key) == opening_scalars.end() ||
                !affected.contains("/roof_openings/" + std::to_string(index) + "/" + key))
                invalid("Roof plan resize archived cut receipt is unaffected or unsupported");
            admit_opening_receipt(receipt, scalar(cut->at(key)));
        }
    }
}
Json receipt_opaque_remainder(Json receipt, bool opening) {
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
        invalid("Roof plan resize actual source roof is missing or inconsistent");
    validate_roof_plan_resize_source_entity(found->second);
    return make_roof_shape(decode_roof_entity(resolve_vertical_placement(source, found->second)));
}
std::vector<RoofJoin> affected_joins(const Entities& source, const Ids& targets, const ConstraintPhaseScope& scope) {
    std::vector<RoofJoin> result;
    std::map<std::string, std::string, std::less<>> owners;
    for (const auto& [id, entity] : source) {
        if (entity.id != id) invalid("Roof plan resize actual map contains inconsistent identities");
        if (entity.type != "roof_join") continue;
        const auto join = parse_roof_join(entity.properties, id);
        const bool affected = std::any_of(join.roof_ids.begin(), join.roof_ids.end(),
            [&](const auto& member) { return targets.contains(member); });
        for (const auto& member : join.roof_ids) {
            const auto roof = source.find(member);
            if (roof == source.end() || roof->second.type != "roof" || roof->second.id != member)
                invalid("Roof plan resize source has a dangling join member");
            if (!owners.emplace(member, id).second) invalid("Roof plan resize source has overlapping joins");
            if (affected && scope.inactive_owner_ids.contains(member))
                invalid("Roof plan resize affected join member is inactive");
        }
        if (!affected) continue;
        if (scope.inactive_owner_ids.contains(id)) invalid("Roof plan resize affected join is inactive");
        if (result.size() == collection_limit) invalid("Roof plan resize affected join budget exceeded");
        result.push_back(join);
    }
    return result;
}
void admit_joins(const Entities& source, const std::vector<RoofJoin>& joins) {
    for (const auto& join : joins) {
        std::vector<TopoDS_Shape> members;
        for (const auto& id : join.roof_ids) members.push_back(resolved_shape(source, id));
        (void)make_roof_join(join, members);
    }
}
} // namespace

nlohmann::json encode_roof_plan_resize_intent(const RoofPlanResizeIntent& intent) {
    admit_intent(intent);
    Json result{{"version", 1}, {"roof_id", intent.roof_id}, {"scale_x", intent.scale_x},
        {"scale_y", intent.scale_y}, {"anchor_m", {intent.anchor.x, intent.anchor.y}},
        {"frame_rotation_radians", intent.frame_rotation_radians}};
    (void)budget(result);
    return result;
}
RoofPlanResizeIntent decode_roof_plan_resize_intent(const nlohmann::json& value) {
    (void)budget(value);
    keys(value, {"version", "roof_id", "scale_x", "scale_y", "anchor_m", "frame_rotation_radians"});
    if (!version_one(value.at("version"))) invalid("Roof plan resize version is unsupported");
    RoofPlanResizeIntent result{identity(value.at("roof_id")), scalar(value.at("scale_x")),
        scalar(value.at("scale_y")), anchor(value.at("anchor_m")), scalar(value.at("frame_rotation_radians"))};
    admit_intent(result);
    return result;
}
void validate_roof_plan_resize_derivations(const Entity& source) {
    if (!source.extensions.is_object()) invalid("Roof plan resize extensions must be an object");
    const auto extension = field(source.extensions, std::string(roof_plan_resize_derivations_key));
    if (!extension) return;
    (void)budget(*extension);
    keys(*extension, {"version", "operations"});
    if (!version_one(extension->at("version"))) invalid("Roof plan resize derivation namespace collision");
    const auto& operations = extension->at("operations");
    if (!operations.is_array() || operations.empty() || operations.size() > operation_limit)
        invalid("Roof plan resize derivation operation budget exceeded");
    validate_roof_derivation_cached("roof-plan-resize-v1", extension->dump(), [&] {
        for (const auto& record : operations) admit_record(record);
    });
}
nlohmann::json roof_plan_resize_opaque_remainder(const Entity& source) {
    validate_roof_plan_resize_derivations(source);
    const auto extension = field(source.extensions, std::string(roof_plan_resize_derivations_key));
    if (!extension) return Json::object();
    Json operations = Json::array();
    for (const auto& record : extension->at("operations")) {
        const auto& receipts = record.at("receipts");
        Json indexed = Json::object(), children = Json::array();
        for (const auto& [pointer, receipt] : receipts.at("quantity_entries").items())
            indexed[pointer] = receipt_opaque_remainder(receipt, false);
        for (const auto& [id, values] : receipts.at("roof_opening_input").items()) {
            (void)id; // Qualified historical cut-map keys become ordered slots.
            Json child = Json::object();
            for (const auto& [key, receipt] : values.items()) child[key] = receipt_opaque_remainder(receipt, true);
            children.push_back(std::move(child));
        }
        // Both physical frames and intent are closed; their only strings are
        // qualified historical identities/form tokens. Raw opaque receipt
        // siblings and rational payload remain in the residual without edits.
        operations.push_back({{"receipts", {{"quantity_entries", std::move(indexed)},
            {"roof_opening_input", std::move(children)}}}});
    }
    return {{"operations", std::move(operations)}};
}
void validate_roof_plan_resize_source_entity(const Entity& source) {
    (void)budget(source.properties);
    (void)budget(source.extensions);
    validate_roof_rigid_transform_source_entity(source);
    admit_frame(frame(source), source.id);
    validate_roof_plan_resize_derivations(source);
}
Entity stage_roof_plan_resize_entity(const Entity& source, const RoofPlanResizeIntent& intent) {
    const auto operation = encode_roof_plan_resize_intent(intent);
    if (source.id != intent.roof_id) invalid("Roof plan resize differs from its actual source owner");
    validate_roof_plan_resize_source_entity(source);
    const auto before = frame(source);
    auto result = stage_roof_plan_axis_resize_entity(source, intent.scale_x, intent.scale_y,
        intent.anchor, intent.frame_rotation_radians);
    const auto after = frame(result);
    // Admit and compare historical frame replay independently of opaque source
    // data, then constrain the geometry stage to its declared scalar ownership.
    if (!exact(derive(before, intent), after)) invalid("Roof plan resize geometry depends on unsupported source data");
    auto permitted = source;
    for (const auto* key : {"run_m", "length_m", "span_m", "pitch_rad"})
        if (before.contains(key)) permitted.properties.at(key) = after.at(key);
    for (std::size_t i = 0; i < 2; ++i)
        permitted.properties.at("base_position_m").at(i) = after.at("base_position_m").at(i);
    if (before.contains("roof_openings")) {
        if (!after.contains("roof_openings") || after.at("roof_openings").size() != before.at("roof_openings").size())
            invalid("Roof plan resize cannot change the source opening roster");
        for (std::size_t i = 0; i < before.at("roof_openings").size(); ++i)
            for (const auto* key : opening_scalars)
                permitted.properties.at("roof_openings").at(i).at(key) = after.at("roof_openings").at(i).at(key);
    }
    if (!exact(result, permitted)) invalid("Roof plan resize geometry changed retained source data");
    if (exact(before, after)) return source;
    const auto receipts = archive_receipts(source, result, before, changes(before, after));
    const auto key = std::string(roof_plan_resize_derivations_key);
    if (!result.extensions.contains(key)) result.extensions[key] = {{"version", 1}, {"operations", Json::array()}};
    auto& operations = result.extensions.at(key).at("operations");
    if (operations.size() == operation_limit) invalid("Roof plan resize derivation operation budget exceeded");
    operations.push_back({{"operation", operation}, {"source", before}, {"result", after}, {"receipts", receipts}});
    validate_roof_plan_resize_derivations(result);
    return result;
}
Entity replay_roof_plan_resize_entity(const Entity& source, const RoofPlanResizeIntent& intent) {
    auto result = stage_roof_plan_resize_entity(source, intent);
    validate_roof_plan_resize_source_entity(result);
    return result;
}
std::map<std::string, Entity, std::less<>> replay_roof_plan_resize_entities(
    const std::map<std::string, Entity, std::less<>>& source,
    const std::vector<RoofPlanResizeIntent>& intents) {
    if (intents.size() > maximum_architectural_group_targets) invalid("Roof plan resize target budget exceeded");
    if (intents.empty()) return source;
    const auto scope = constraint_phase_scope(source);
    Ids targets;
    std::size_t operation_bytes = 0, archive_bytes = 0;
    for (const auto& intent : intents) {
        const auto bytes = budget(encode_roof_plan_resize_intent(intent));
        if (bytes > proof_limit - operation_bytes) invalid("Roof plan resize batch operation budget exceeded");
        operation_bytes += bytes;
        if (!targets.insert(intent.roof_id).second) invalid("Roof plan resize contains duplicate targets");
        if (scope.inactive_owner_ids.contains(intent.roof_id)) invalid("Roof plan resize target is inactive");
        (void)resolved_shape(source, intent.roof_id);
    }
    const auto joins = affected_joins(source, targets, scope);
    admit_joins(source, joins);
    auto result = source;
    for (const auto& intent : intents) {
        auto roof = replay_roof_plan_resize_entity(source.at(intent.roof_id), intent);
        if (const auto archive = field(roof.extensions, std::string(roof_plan_resize_derivations_key))) {
            const auto bytes = budget(*archive);
            if (bytes > proof_limit - archive_bytes) invalid("Roof plan resize batch archive budget exceeded");
            archive_bytes += bytes;
        }
        result.at(intent.roof_id) = std::move(roof);
    }
    for (const auto& id : targets) (void)resolved_shape(result, id);
    admit_joins(result, joins);
    return result;
}
std::optional<RoofPlanResizeIntent> capture_roof_plan_resize(
    const Entity& original, const Entity& candidate, const RoofPlanResizeIntent& actual_intent) {
    (void)identity(candidate.id);
    if (candidate.type != "roof") invalid("Roof plan resize candidate must retain the source roof type");
    (void)budget(candidate.properties);
    (void)budget(candidate.extensions);
    const auto expected = replay_roof_plan_resize_entity(original, actual_intent);
    if (!exact(expected, candidate)) invalid("Roof plan resize candidate differs from complete actual-source replay");
    if (exact(original, expected)) return std::nullopt;
    return actual_intent;
}
} // namespace sketch
