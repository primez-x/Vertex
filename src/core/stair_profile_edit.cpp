#include "sketch/stair_profile_edit.hpp"

#include "sketch/constraint_entity.hpp"
#include "sketch/project_organization.hpp"
#include "sketch/stair_compound_edit.hpp"
#include "sketch/stair_edit_dependencies.hpp"
#include "sketch/stair_semantics.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>
#include <set>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace sketch {
namespace {
using Json = nlohmann::json;
using Entities = std::map<std::string, Entity, std::less<>>;
constexpr std::size_t proof_limit = 1024 * 1024;

[[noreturn]] void invalid(const std::string& reason) {
    throw std::invalid_argument("Stair profile edit: " + reason);
}
void keys(const Json& value, std::initializer_list<const char*> expected) {
    if (!value.is_object() || value.size() != expected.size()) invalid("unsupported closed fields");
    for (const auto* key : expected) if (!value.contains(key)) invalid("missing required field");
}
std::string identity(const Json& value) {
    if (!value.is_string()) invalid("identity must be a string");
    const auto& id = value.get_ref<const std::string&>();
    if (id.empty() || id.size() > 128 || !std::all_of(id.begin(), id.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == ':';
    })) invalid("invalid actual source identity");
    return id;
}
bool exact(const Entity& a, const Entity& b) {
    return a == b && a.properties.dump() == b.properties.dump() && a.extensions.dump() == b.extensions.dump();
}
void receipt_fields(const Json& value) {
    keys(value, {"version", "original_expression", "entered_unit", "exact_metres"});
    keys(value.at("exact_metres"), {"numerator", "denominator"});
    if (!value.at("version").is_number_integer() || value.at("version") != 1)
        invalid("quantity version is unsupported");
    if (!value.at("original_expression").is_string() ||
        value.at("original_expression").get_ref<const std::string&>().size() > 4096)
        invalid("quantity expression budget exceeded");
    if (!value.at("entered_unit").is_string() ||
        value.at("entered_unit").get_ref<const std::string&>().size() > 2)
        invalid("quantity unit is unsupported");
}
Json quantity(const Quantity& value) {
    if (!std::isfinite(value.metres) || value.metres <= 1e-7 || value.metres > 1e6 ||
        value.original_expression.size() > 4096) invalid("quantity must be positive and bounded");
    auto result = encode_constraint_quantity_receipt(value);
    receipt_fields(result);
    return result;
}
Quantity quantity(const Json& value) {
    receipt_fields(value);
    const auto result = decode_constraint_quantity_receipt(value);
    if (quantity(result).dump() != value.dump()) invalid("quantity must use its exact canonical codec");
    return result;
}
struct Dimension {
    const char* name;
    const char* pointer;
    std::optional<Quantity> StairProfileEditIntent::* member;
    bool stair;
};
constexpr std::array dimensions{
    Dimension{"total_rise", "/total_rise_m", &StairProfileEditIntent::total_rise, true},
    Dimension{"going", "/going_m", &StairProfileEditIntent::going, true},
    Dimension{"width", "/width_m", &StairProfileEditIntent::width, true},
    Dimension{"top_landing_depth", "/top_landing/depth_m", &StairProfileEditIntent::top_landing_depth, true},
    Dimension{"top_landing_thickness", "/top_landing/thickness_m", &StairProfileEditIntent::top_landing_thickness, true},
    Dimension{"length", "/length_m", &StairProfileEditIntent::length, false},
    Dimension{"height", "/height_m", &StairProfileEditIntent::height, false},
    Dimension{"thickness", "/thickness_m", &StairProfileEditIntent::thickness, false},
    Dimension{"post_spacing", "/post_spacing_m", &StairProfileEditIntent::post_spacing, false}};

// Projection matches the existing full profile codec; opaque siblings stay in
// the actual source envelope and are merged by that producer, never supplied.
Json record(const Json& source, std::initializer_list<const char*> fields) {
    auto result = Json::object();
    for (const auto* key : fields) result[key] = source.contains(key) ? source.at(key) : Json(nullptr);
    return result;
}
Json profile(const Entity& source) {
    const auto& p = source.properties;
    auto result = source.type == "stair" ? record(p, {"version", "form", "base_position_m",
        "orientation_rad", "riser_count", "total_rise_m", "going_m", "width_m", "top_landing",
        "level_connection", "flights", "landings", "vertical_placement"}) :
        record(p, {"version", "form", "base_position_m", "orientation_rad", "length_m", "height_m",
            "thickness_m", "post_spacing_m", "host", "vertical_placement"});
    if (result.contains("top_landing") && result.at("top_landing").is_object())
        result["top_landing"] = record(p.at("top_landing"), {"depth_m", "thickness_m"});
    if (result.contains("level_connection") && result.at("level_connection").is_object())
        result["level_connection"] = record(p.at("level_connection"),
            {"version", "graph_id", "link_id", "lower_level_id", "upper_level_id"});
    if (result.at("vertical_placement").is_object())
        result["vertical_placement"] = record(p.at("vertical_placement"), {"version", "mode", "offset_m"});
    if (source.type == "stair") {
        if (result.at("flights").is_array()) for (auto& row : result["flights"])
            row = record(row, {"id", "riser_count", "going_m", "width_m"});
        if (result.at("landings").is_array()) for (auto& row : result["landings"])
            row = record(row, {"id", "depth_m", "thickness_m", "turn", "return_gap_m", "straight_alignment"});
    } else if (result.at("host").is_object()) {
        const auto& host = p.at("host");
        if (p.at("version") == 2)
            result["host"] = record(host, {"stair_id", "flight_id", "side", "start_fraction", "end_fraction"});
        else if (host.at("role") == "top") result["host"] = record(host,
            {"stair_id", "role", "incoming_flight_id", "edge_index", "start_fraction", "end_fraction"});
        else result["host"] = record(host, {"stair_id", "role", "landing_id", "incoming_flight_id",
            "outgoing_flight_id", "edge_index", "start_fraction", "end_fraction"});
    }
    return result;
}
std::set<std::string, std::less<>> receipt_pointers(const Entity& source) {
    std::set<std::string, std::less<>> result;
    for (const auto& dimension : dimensions) if (dimension.stair == (source.type == "stair") &&
        source.properties.contains(Json::json_pointer(dimension.pointer))) result.insert(dimension.pointer);
    if (source.properties.contains("base_position_m")) for (int i = 0; i < 3; ++i)
        result.insert("/base_position_m/" + std::to_string(i));
    if (source.properties.contains("vertical_placement")) result.insert("/vertical_placement/offset_m");
    if (source.type == "stair") for (const auto* key : {"flights", "landings"})
        if (source.properties.contains(key)) {
            const auto& rows = source.properties.at(key);
            for (std::size_t i = 0; i < rows.size(); ++i)
                for (const auto* field : {"going_m", "width_m", "depth_m", "thickness_m", "return_gap_m"}) {
                    const bool flight = std::string_view(key) == "flights";
                    const bool flight_field = std::string_view(field) == "going_m" || std::string_view(field) == "width_m";
                    if (flight == flight_field && rows.at(i).contains(field))
                        result.insert("/" + std::string(key) + "/" + std::to_string(i) + "/" + field);
                }
        }
    return result;
}
void replace_receipt(Json& entries, const std::string& pointer, const Quantity& entered) {
    const auto encoded = quantity(entered);
    const auto old = entries.find(pointer);
    if (old == entries.end()) {
        if (entries.size() >= 4096) invalid("quantity entry budget exceeded");
        entries[pointer] = encoded;
    } else {
        if (!old->is_object() || !old->contains("version") ||
            !old->at("version").is_number_integer() || old->at("version") != 1)
            invalid("cannot overwrite an opaque future receipt");
        for (const auto* key : {"version", "original_expression", "entered_unit"}) (*old)[key] = encoded.at(key);
        for (const auto* key : {"numerator", "denominator"})
            (*old)["exact_metres"][key] = encoded.at("exact_metres").at(key);
    }
}
void apply_dimension(Entity& edited, const std::string& pointer, const Quantity& entered) {
    const Json::json_pointer path(pointer);
    if (edited.properties.at(path).get<double>() == entered.metres) return;
    edited.properties[path] = entered.metres;
    if (!edited.properties.contains("quantity_entries")) edited.properties["quantity_entries"] = Json::object();
    replace_receipt(edited.properties["quantity_entries"], pointer, entered);
}
Entity prepare(const Entity& source, const StairProfileEditIntent& intent) {
    (void)encode_stair_profile_edit_intent(intent);
    if (source.id != intent.object_id) invalid("target differs from its actual source owner");
    validate_stair_profile_source_entity(source);
    auto result = source;
    for (const auto& dimension : dimensions) {
        const auto& entered = intent.*(dimension.member);
        if (!entered) continue;
        const Json::json_pointer pointer(dimension.pointer);
        if (std::string_view(dimension.name) == "length" && source.type == "railing" &&
            source.properties.contains("host") && source.properties.at("host").is_object())
            invalid("hosted rail length is derived from its actual host");
        if (dimension.stair != (source.type == "stair") || !source.properties.contains(pointer))
            invalid("quantity is inapplicable to the actual source form");
        apply_dimension(result, dimension.pointer, *entered);
        if ((std::string_view(dimension.name) == "going" || std::string_view(dimension.name) == "width") &&
            source.properties.contains("flights")) {
            const auto& rows = source.properties.at("flights");
            const auto field = std::string(dimension.pointer + 1);
            for (std::size_t i = 0; i < rows.size(); ++i) if (rows.at(i).contains(field))
                apply_dimension(result, "/flights/" + std::to_string(i) + "/" + field, *entered);
        }
    }
    // Capture validates scalar/layout semantics and complete opaque retention,
    // but does not invoke actual-map native replay or choose rail dispositions.
    (void)capture_stair_object_edit(source, result);
    return result;
}
std::vector<StairTransformIntent> identity_placements(const Entities& actual,
    const std::vector<StairProfileEditIntent>& intents, bool include_hosts = false) {
    std::vector<StairTransformIntent> result;
    std::set<std::string, std::less<>> targets;
    for (const auto& intent : intents) {
        const auto& entity = actual.at(intent.object_id);
        const auto& owner = entity.type == "railing" && entity.properties.contains("host") ?
            actual.at(entity.properties.at("host").at("stair_id").get<std::string>()) : entity;
        const auto resolved = resolve_vertical_placement(actual, owner);
        const auto& base = resolved.properties.at("base_position_m");
        // Admission of a real host includes its complete active attached rail
        // cohort. Collapse repeated host/rail identity checks rather than
        // double the explicit producer roster at the selected-target limit.
        const auto& id = include_hosts ? owner.id : intent.object_id;
        if (targets.insert(id).second)
            result.push_back({id, {{base.at(0).get<double>(), base.at(1).get<double>(), base.at(2).get<double>()}}});
    }
    // Host closure is admission-only. Scalar compound authoring below retains
    // exactly the selected owners, independently of this physical inventory.
    return result;
}
void admit_identity(const Entities& actual, const std::vector<StairTransformIntent>& placements) {
    const auto admitted = replay_stair_transform_entities(actual, placements);
    if (admitted.size() != actual.size()) invalid("identity admission changed owner inventory");
    for (const auto& [id, entity] : actual)
        if (!exact(entity, admitted.at(id))) invalid("identity admission changed retained source: " + id);
}
} // namespace

Json encode_stair_profile_edit_intent(const StairProfileEditIntent& intent) {
    (void)identity(intent.object_id);
    Json result{{"version", 1}, {"object_id", intent.object_id}};
    for (const auto& dimension : dimensions) {
        const auto& entered = intent.*(dimension.member);
        result[dimension.name] = entered ? quantity(*entered) : Json(nullptr);
    }
    return result;
}
StairProfileEditIntent decode_stair_profile_edit_intent(const Json& value) {
    keys(value, {"version", "object_id", "total_rise", "going", "width", "top_landing_depth",
        "top_landing_thickness", "length", "height", "thickness", "post_spacing"});
    if (!value.at("version").is_number_integer() || value.at("version") != 1) invalid("unsupported version");
    StairProfileEditIntent result;
    result.object_id = identity(value.at("object_id"));
    for (const auto& dimension : dimensions)
        if (!value.at(dimension.name).is_null()) result.*(dimension.member) = quantity(value.at(dimension.name));
    if (encode_stair_profile_edit_intent(result).dump() != value.dump()) invalid("intent must use its exact canonical codec");
    return result;
}
void validate_stair_profile_source_entity(const Entity& source) {
    // Bound unknown source metadata before family codecs or JSON serialization.
    stair_transform_detail::source_bounds(Entities{{source.id, source}});
    (void)identity(source.id);
    if (source.type != "stair" && source.type != "railing") invalid("source is not an actual stair or railing");
    (void)capture_stair_object_edit(source, source);
    StairObjectEditIntent descriptor{source.id, profile(source), nullptr};
    (void)encode_stair_object_edit_intent(descriptor);
    if (!source.properties.contains("quantity_entries")) return;
    const auto& entries = source.properties.at("quantity_entries");
    if (!entries.is_object() || entries.size() > 4096) invalid("source quantities must be a bounded object");
    const auto understood = receipt_pointers(source);
    for (const auto& [pointer, raw] : entries.items()) {
        if (!understood.contains(pointer) || !raw.is_object() || !raw.contains("version") ||
            !raw.at("version").is_number_integer() || raw.at("version") != 1) continue;
        const auto entered = decode_constraint_quantity_receipt(raw);
        if (entered.original_expression.size() > 4096 ||
            entered.metres != source.properties.at(Json::json_pointer(pointer)).get<double>())
            invalid("source quantity receipt is stale");
    }
}
StairObjectEditIntent stair_object_edit_from_profile(const Entity& source, const StairProfileEditIntent& intent) {
    const auto edited = prepare(source, intent);
    if (const auto result = capture_stair_object_edit(source, edited)) return *result;
    StairObjectEditIntent result{source.id, profile(source), nullptr};
    if (source.properties.contains("quantity_entries")) result.quantity_entries = source.properties.at("quantity_entries");
    (void)encode_stair_object_edit_intent(result);
    return result;
}
std::vector<Entity> prepare_stair_profile_edit_entities(const Entities& source,
    const std::vector<StairProfileEditIntent>& intents) {
    if (intents.size() > maximum_architectural_group_targets) invalid("target budget exceeded");
    if (intents.empty()) return {};
    stair_transform_detail::source_bounds(source);
    std::size_t bytes = 0;
    std::set<std::string, std::less<>> targets;
    std::vector<Entity> result;
    for (const auto& intent : intents) {
        const auto count = encode_stair_profile_edit_intent(intent).dump().size();
        if (count > proof_limit - bytes) invalid("batch proof byte budget exceeded");
        bytes += count;
        if (!targets.insert(intent.object_id).second) invalid("duplicate targets");
        const auto found = source.find(intent.object_id);
        if (found == source.end() || found->first != found->second.id) invalid("actual target is missing or inconsistent");
        result.push_back(prepare(found->second, intent));
    }
    return result;
}
Entities replay_stair_profile_entities(const Entities& source, const std::vector<StairProfileEditIntent>& intents) {
    const auto edited = prepare_stair_profile_edit_entities(source, intents);
    if (edited.empty()) return source;
    std::vector<Entity> changed;
    for (const auto& entity : edited)
        if (!exact(source.at(entity.id), entity)) changed.push_back(entity);
    // A retained member does not request ordinary authoring of its baseline.
    // It still participates in complete original/result physical admission.
    const auto dependencies = changed.empty() ? PhaseStairReplacementDependencyPlan{} :
        inspect_ordinary_stair_edit_dependencies(source, changed);
    if (!dependencies.ready()) invalid(dependencies.diagnostics.front().entity_id + ": " + dependencies.diagnostics.front().reason);
    if (!dependencies.dependencies.empty()) {
        auto descriptors = source;
        std::set<std::string, std::less<>> selected;
        for (const auto& entity : edited) { descriptors.at(entity.id) = entity; selected.insert(entity.id); }
        for (const auto& dependency : dependencies.dependencies) {
            // The general topology inspector evaluates an original dependent
            // against the edited stair. A concurrently selected scalar rail
            // may retain that exact host with a newly fitting thickness/spacing.
            // Authenticate only this retained binding, never infer a disposition.
            if (!selected.contains(dependency.rail_id))
                invalid("affected railings require explicit reviewed dispositions");
            const auto& rail = descriptors.at(dependency.rail_id);
            if (rail.properties.at("host").dump() != source.at(rail.id).properties.at("host").dump())
                invalid("scalar rail changed its actual host");
            const auto host = resolve_vertical_placement(descriptors, descriptors.at(dependency.stair_id));
            (void)derive_hosted_railing_layout(decode_railing_properties(rail.id, rail.properties),
                decode_stair_properties(host.id, host.properties));
        }
    }
    const auto placements = identity_placements(source, intents);
    admit_identity(source, identity_placements(source, intents, true));
    std::vector<StairCompoundEditIntent> compound;
    for (std::size_t i = 0; i < intents.size(); ++i)
        compound.push_back({stair_object_edit_from_profile(source.at(intents[i].object_id), intents[i]), placements[i], true});
    auto result = replay_stair_compound_edit_entities(source, compound);
    // Scalar edits have no placement operation. Identity producer admission
    // also covers hosted catalogs which vertical-only coordination can skip.
    admit_identity(result, identity_placements(result, intents, true));
    return result;
}

} // namespace sketch
