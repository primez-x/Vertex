#include "sketch/phase_roof_uniform_transform.hpp"
#include "roof_derivation_cache.hpp"

#include "sketch/architecture.hpp"
#include "sketch/constraint_entity.hpp"
#include "sketch/constraint_phase_scope.hpp"
#include "sketch/phase_roof_profile_edit.hpp"
#include "sketch/phase_roof_resize.hpp"
#include "sketch/phase_roof_transform.hpp"
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
using Changes = std::map<std::string, std::pair<double, double>, std::less<>>;
constexpr std::size_t collection_limit = 4096;
constexpr std::size_t opening_limit = 256;
constexpr std::size_t operation_limit = 256;
constexpr std::size_t proof_limit = 1024 * 1024;
constexpr std::size_t string_limit = 16384;
constexpr double scalar_limit = 1.0e12;
constexpr std::array opening_scalars{"x_m", "y_m", "width_m", "depth_m"};
constexpr std::array skylight_scalars{"frame_width_m", "curb_height_m", "glazing_thickness_m"};

[[noreturn]] void invalid(const char* reason) { throw std::invalid_argument(reason); }
void keys(const Json& value, std::initializer_list<const char*> expected) {
    if (!value.is_object() || value.size() != expected.size()) invalid("Roof uniform transform fields are invalid");
    for (const auto* key : expected)
        if (!value.contains(key)) invalid("Roof uniform transform field is missing");
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
bool version_two(const Json& value) {
    return (value.is_number_integer() || value.is_number_unsigned()) && value == 2;
}
std::string identity(const Json& value) {
    if (!value.is_string()) invalid("Roof uniform transform identity must be a string");
    const auto& result = value.get_ref<const std::string&>();
    if (result.empty() || result.size() > 128 ||
        !std::all_of(result.begin(), result.end(), [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == ':';
        })) invalid("Roof uniform transform identity is invalid");
    return result;
}
double scalar(double value) {
    if (!std::isfinite(value) || std::abs(value) > scalar_limit)
        invalid("Roof uniform transform exceeds the finite scalar range");
    return value;
}
double scalar(const Json& value) {
    if (!value.is_number()) invalid("Roof uniform transform scalar must be numeric");
    return scalar(value.get<double>());
}
Vec3 point(const Json& value) {
    if (!value.is_array() || value.size() != 3) invalid("Roof uniform transform point must have three scalars");
    return {scalar(value.at(0)), scalar(value.at(1)), scalar(value.at(2))};
}
void budget_nodes(const Json& value, std::size_t& nodes, std::size_t depth = 0) {
    if (depth > 32 || ++nodes > 65536) invalid("Roof uniform transform proof structural budget exceeded");
    if ((value.is_array() || value.is_object()) && value.size() > collection_limit)
        invalid("Roof uniform transform proof collection budget exceeded");
    if (value.is_number_float() && !std::isfinite(value.get<double>()))
        invalid("Roof uniform transform proof contains a nonfinite scalar");
    if (value.is_string() && value.get_ref<const std::string&>().size() > string_limit)
        invalid("Roof uniform transform proof string budget exceeded");
    if (value.is_object())
        for (const auto& [key, child] : value.items()) {
            if (key.size() > string_limit) invalid("Roof uniform transform proof key budget exceeded");
            budget_nodes(child, nodes, depth + 1);
        }
    else if (value.is_array())
        for (const auto& child : value) budget_nodes(child, nodes, depth + 1);
}
std::size_t budget(const Json& value) {
    std::size_t nodes = 0;
    budget_nodes(value, nodes);
    const auto bytes = value.dump().size();
    if (bytes > proof_limit) invalid("Roof uniform transform proof byte budget exceeded");
    return bytes;
}
std::array<double, 2> rotation(double angle) {
    if (std::abs(angle) == std::numbers::pi) return {-1.0, 0.0};
    return {std::cos(angle), std::sin(angle)};
}
struct Normalized {
    double x{}, y{}, angle{}, scale{};
    bool horizontal{}, vertical{};
};
Normalized normalize(const ArchitecturalGroupTransform& value) {
    for (auto n : {value.pivot.x, value.pivot.y, value.pivot.z,
             value.offset.x, value.offset.y, value.offset.z, value.rotation_z_radians}) (void)scalar(n);
    if (!(scalar(value.scale) > 0.0) || value.scale == 1.0)
        invalid("Roof uniform transform requires a positive non-unit scale");
    const bool reflected = value.flip_horizontal != value.flip_vertical;
    const double angle = std::remainder(std::remainder(value.rotation_z_radians,
        2.0 * std::numbers::pi) + (value.flip_horizontal && value.flip_vertical
            ? std::numbers::pi : 0.0), 2.0 * std::numbers::pi);
    const auto [c, s] = rotation(angle);
    const bool horizontal = reflected && value.flip_horizontal;
    const bool vertical = reflected && value.flip_vertical;
    const double hx = horizontal ? -1.0 : 1.0, hy = vertical ? -1.0 : 1.0;
    // Preserve the shared command's difference-form arithmetic and original G.
    const double x = scalar(value.offset.x + (1.0 - value.scale * hx) * value.pivot.x +
        value.scale * hx * ((1.0 - c) * value.pivot.x + s * value.pivot.y));
    const double y = scalar(value.offset.y + (1.0 - value.scale * hy) * value.pivot.y +
        value.scale * hy * ((1.0 - c) * value.pivot.y - s * value.pivot.x));
    return {x, y, angle, value.scale, horizontal, vertical};
}

// Closed source-only physical frame plus the historical actual-map datum.
Json frame(const Entity& source, double datum) {
    const auto& p = source.properties;
    Json result;
    for (const auto* key : {"version", "form", "base_position_m", "orientation_rad", "span_m",
             "rise_m", "pitch_rad", "overhang_m", "thickness_m"}) result[key] = p.at(key);
    const auto* length = p.at("form") == "sloped_roof_panel" ? "run_m" : "length_m";
    result[length] = p.at(length);
    result["vertical_datum_m"] = scalar(datum);
    if (p.contains("roof_openings")) {
        result["roof_openings"] = Json::array();
        for (const auto& row : p.at("roof_openings")) {
            Json cut{{"id", row.at("id")}};
            for (const auto* key : opening_scalars) cut[key] = row.at(key);
            if (p.at("version") >= 3 && row.contains("skylight")) cut["skylight"] = row.at("skylight");
            if (p.at("version") == 4 && row.contains("rotation_rad")) cut["rotation_rad"] = row.at("rotation_rad");
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
    result.properties.erase("vertical_datum_m");
    return result;
}
void admit_frame(const Json& value, const std::string& id, bool skylights = true, bool rotations = true) {
    (void)budget(value);
    if (!value.is_object() || !value.contains("form") || !value.at("form").is_string())
        invalid("Roof uniform transform proof form is missing");
    const bool panel = value.at("form") == "sloped_roof_panel";
    if (!panel && value.at("form") != "gable_roof" && value.at("form") != "hip_roof")
        invalid("Roof uniform transform proof form is unsupported");
    const auto schema = field(value, "version");
    if (!schema || (!schema->is_number_integer() && !schema->is_number_unsigned()) ||
        (*schema != 1 && *schema != 2 && (!skylights || *schema != 3) && (!rotations || *schema != 4)))
        invalid("Roof uniform transform proof schema is unsupported");
    const bool openings = *schema != 1;
    if (panel && openings)
        keys(value, {"version", "form", "base_position_m", "orientation_rad", "run_m", "span_m",
            "rise_m", "pitch_rad", "overhang_m", "thickness_m", "vertical_datum_m", "roof_openings"});
    else if (panel)
        keys(value, {"version", "form", "base_position_m", "orientation_rad", "run_m", "span_m",
            "rise_m", "pitch_rad", "overhang_m", "thickness_m", "vertical_datum_m"});
    else if (openings)
        keys(value, {"version", "form", "base_position_m", "orientation_rad", "length_m", "span_m",
            "rise_m", "pitch_rad", "overhang_m", "thickness_m", "vertical_datum_m", "roof_openings"});
    else
        keys(value, {"version", "form", "base_position_m", "orientation_rad", "length_m", "span_m",
            "rise_m", "pitch_rad", "overhang_m", "thickness_m", "vertical_datum_m"});
    (void)point(value.at("base_position_m"));
    for (const auto* key : {"orientation_rad", "span_m", "rise_m", "pitch_rad", "overhang_m", "thickness_m", "vertical_datum_m"})
        (void)scalar(value.at(key));
    (void)scalar(value.at(panel ? "run_m" : "length_m"));
    if (openings) {
        const auto& rows = value.at("roof_openings");
        if (!rows.is_array() || rows.size() > opening_limit) invalid("Roof uniform transform proof cut budget exceeded");
        Ids ids;
        for (const auto& cut : rows) {
            auto core = cut;
            if (*schema == 4 && core.contains("rotation_rad")) {
                const double angle = scalar(core.at("rotation_rad"));
                if (std::abs(angle) > std::acos(-1.0) || (angle != 0.0 && !core.contains("skylight")))
                    invalid("Roof uniform transform opening rotation is invalid");
                core.erase("rotation_rad");
            }
            if (*schema >= 3 && cut.contains("skylight")) {
                keys(core, {"id", "x_m", "y_m", "width_m", "depth_m", "skylight"});
                const auto& profile = cut.at("skylight");
                keys(profile, {"version", "frame_width_m", "curb_height_m", "glazing_thickness_m"});
                if (!version_one(profile.at("version"))) invalid("Roof uniform transform skylight profile is unsupported");
                for (const auto* key : skylight_scalars)
                    if (scalar(profile.at(key)) < 0.0 ||
                        (std::string_view(key) != "curb_height_m" && scalar(profile.at(key)) == 0.0))
                        invalid("Roof uniform transform skylight dimensions are invalid");
            } else keys(core, {"id", "x_m", "y_m", "width_m", "depth_m"});
            if (!ids.insert(identity(cut.at("id"))).second) invalid("Roof uniform transform proof has duplicate cut IDs");
            for (const auto* key : opening_scalars) (void)scalar(cut.at(key));
        }
    }
    (void)make_roof_shape(decode_roof_entity(physical_entity(value, id)));
    auto resolved = physical_entity(value, id);
    const auto p = point(value.at("base_position_m"));
    resolved.properties.at("base_position_m").at(2) = scalar(p.z + scalar(value.at("vertical_datum_m")));
    (void)make_roof_shape(decode_roof_entity(resolved));
}
void assign_if_changed(Json& target, double value) {
    (void)scalar(value);
    if (scalar(target) != value) target = value;
}
Json derive(const Json& before, const RoofUniformTransformIntent& intent, bool skylights = true, bool rotations = true) {
    admit_frame(before, intent.roof_id, skylights, rotations);
    const auto t = normalize(intent.transform);
    auto result = before;
    const auto p = point(before.at("base_position_m"));
    const auto [c, s] = rotation(t.angle);
    const double hx = t.horizontal ? -1.0 : 1.0, hy = t.vertical ? -1.0 : 1.0;
    const double scaled_x = p.x * t.scale, scaled_y = p.y * t.scale;
    double x = hx * (c * scaled_x - s * scaled_y) + t.x;
    double y = hy * (s * scaled_x + c * scaled_y) + t.y;
    const bool reflected = t.horizontal != t.vertical;
    const double yaw = scalar(before.at("orientation_rad"));
    double heading = yaw + t.angle;
    if (reflected) {
        heading = std::atan2(hy * (s * std::cos(yaw) + c * std::sin(yaw)),
            hx * (c * std::cos(yaw) - s * std::sin(yaw)));
        if (before.at("form") == "sloped_roof_panel") {
            const double span = scalar(before.at("span_m")) * t.scale;
            x += span * std::sin(heading);
            y -= span * std::cos(heading);
        }
    }
    for (const auto* key : {"run_m", "length_m", "span_m", "rise_m", "overhang_m", "thickness_m"})
        if (before.contains(key)) assign_if_changed(result.at(key), scalar(before.at(key)) * t.scale);
    if (before.contains("roof_openings"))
        for (std::size_t i = 0; i < before.at("roof_openings").size(); ++i) {
            const auto& old_cut = before.at("roof_openings").at(i);
            auto& cut = result.at("roof_openings").at(i);
            for (const auto* key : opening_scalars) assign_if_changed(cut.at(key), scalar(old_cut.at(key)) * t.scale);
            if (old_cut.contains("skylight"))
                for (const auto* key : skylight_scalars)
                    assign_if_changed(cut.at("skylight").at(key), scalar(old_cut.at("skylight").at(key)) * t.scale);
            if (reflected) assign_if_changed(cut.at("y_m"), (before.at("form") == "sloped_roof_panel"
                ? scalar(before.at("span_m")) - scalar(old_cut.at("y_m")) - scalar(old_cut.at("depth_m"))
                : -scalar(old_cut.at("y_m")) - scalar(old_cut.at("depth_m"))) * t.scale);
            if (reflected && old_cut.contains("rotation_rad") && scalar(old_cut.at("rotation_rad")) != 0.0)
                assign_if_changed(cut.at("rotation_rad"), -scalar(old_cut.at("rotation_rad")));
        }
    auto& position = result.at("base_position_m");
    assign_if_changed(position.at(0), x);
    assign_if_changed(position.at(1), y);
    const auto& g = intent.transform;
    assign_if_changed(position.at(2), t.scale * p.z + g.offset.z + (1.0 - t.scale) * g.pivot.z +
        (t.scale - 1.0) * scalar(before.at("vertical_datum_m")));
    assign_if_changed(result.at("orientation_rad"), heading);
    admit_frame(result, intent.roof_id, skylights, rotations);
    return result;
}
Changes changes(const Json& before, const Json& after) {
    Changes result;
    const auto add = [&](const std::string& path, const Json& a, const Json& b) {
        if (scalar(a) != scalar(b)) result.emplace(path, std::pair{scalar(a), scalar(b)});
    };
    for (std::size_t i = 0; i < 3; ++i)
        add("/base_position_m/" + std::to_string(i), before.at("base_position_m").at(i), after.at("base_position_m").at(i));
    for (const auto* key : {"run_m", "length_m", "span_m", "rise_m", "overhang_m", "thickness_m", "orientation_rad"})
        if (before.contains(key)) add("/" + std::string(key), before.at(key), after.at(key));
    if (before.contains("roof_openings"))
        for (std::size_t i = 0; i < before.at("roof_openings").size(); ++i) {
            for (const auto* key : opening_scalars)
                add("/roof_openings/" + std::to_string(i) + "/" + key,
                    before.at("roof_openings").at(i).at(key), after.at("roof_openings").at(i).at(key));
            if (before.at("roof_openings").at(i).contains("rotation_rad"))
                add("/roof_openings/" + std::to_string(i) + "/rotation_rad",
                    before.at("roof_openings").at(i).at("rotation_rad"), after.at("roof_openings").at(i).at("rotation_rad"));
            if (before.at("roof_openings").at(i).contains("skylight"))
                for (const auto* key : skylight_scalars)
                    add("/roof_openings/" + std::to_string(i) + "/skylight/" + key,
                        before.at("roof_openings").at(i).at("skylight").at(key),
                        after.at("roof_openings").at(i).at("skylight").at(key));
        }
    return result;
}
bool descendant(std::string_view path, std::string_view parent) {
    return path.size() > parent.size() && path.starts_with(parent) && path[parent.size()] == '/';
}
bool known_raw(const Json& value) {
    const auto version = field(value, "version");
    return value.is_object() && (!version || version_one(*version));
}
bool receipt_core(const Json& value, bool raw = false) {
    if (!value.is_object()) return true;
    for (const auto* key : {"version", "original_expression", raw ? "default_unit" : "entered_unit"})
        if (value.contains(key)) return true;
    const auto rational = field(value, "exact_metres");
    return rational && (!rational->is_object() || rational->contains("numerator") || rational->contains("denominator"));
}
void receipt_budget(const Json& receipt) {
    const auto expression = field(receipt, "original_expression");
    if (!expression || !expression->is_string() || expression->get_ref<const std::string&>().size() > 4096)
        invalid("Roof uniform transform receipt expression budget exceeded");
    (void)budget(receipt);
}
void admit_quantity_receipt(const Json& receipt, double actual) {
    receipt_budget(receipt);
    const auto version = field(receipt, "version");
    if (!receipt.is_object() || !version || !version_one(*version))
        invalid("Roof uniform transform affects an opaque quantity receipt");
    if (decode_constraint_quantity_receipt(receipt).metres != actual)
        invalid("Roof uniform transform quantity receipt is stale");
}
Unit unit(const Json& value) {
    if (!value.is_string()) invalid("Roof uniform transform receipt unit is invalid");
    const auto& name = value.get_ref<const std::string&>();
    if (name == "m") return Unit::metre;
    if (name == "mm") return Unit::millimetre;
    if (name == "cm") return Unit::centimetre;
    if (name == "ft") return Unit::foot;
    if (name == "in") return Unit::inch;
    invalid("Roof uniform transform receipt unit is unsupported");
}
const char* unit_name(Unit value) {
    switch (value) {
    case Unit::metre: return "m";
    case Unit::millimetre: return "mm";
    case Unit::centimetre: return "cm";
    case Unit::foot: return "ft";
    case Unit::inch: return "in";
    }
    invalid("Roof uniform transform receipt unit is unsupported");
}
void admit_opening_receipt(const Json& raw, double actual) {
    if (!known_raw(raw)) invalid("Roof uniform transform affects an opaque child receipt");
    receipt_budget(raw);
    const auto default_unit = field(raw, "default_unit"), rational = field(raw, "exact_metres");
    if (!default_unit || !rational) invalid("Roof uniform transform opening receipt core is missing");
    const auto parsed = parse_quantity(raw.at("original_expression").get_ref<const std::string&>(), unit(*default_unit));
    const Json core{{"version", 1}, {"original_expression", parsed.original_expression},
        {"entered_unit", unit_name(parsed.entered_unit)}, {"exact_metres", *rational}};
    admit_quantity_receipt(core, actual);
}
Json archive_receipts(const Entity& source, Entity& result, const Json& before, const Changes& affected) {
    Json indexed = Json::object(), children = Json::object();
    if (const auto entries = field(source.properties, "quantity_entries")) {
        if (!entries->is_object() || entries->size() > collection_limit)
            invalid("Roof uniform transform quantity_entries budget exceeded");
        for (const auto& [pointer, receipt] : entries->items())
            for (const auto& [changed, values] : affected) {
                if (pointer != changed && !descendant(pointer, changed) && !descendant(changed, pointer)) continue;
                if (pointer != changed || changed == "/orientation_rad" || changed.ends_with("/rotation_rad"))
                    invalid("Roof uniform transform affects an unsupported quantity binding");
                const bool skylight = changed.starts_with("/roof_openings/") &&
                    std::any_of(skylight_scalars.begin(), skylight_scalars.end(), [&](const auto* key) {
                        return changed.ends_with("/skylight/" + std::string(key));
                    });
                if (skylight && !receipt_core(receipt)) continue;
                admit_quantity_receipt(receipt, values.first);
                indexed[pointer] = receipt;
                result.properties.at("quantity_entries").erase(pointer);
            }
    }
    const auto extension = field(source.extensions, "roof_opening_input");
    if (before.contains("roof_openings"))
        for (std::size_t i = 0; i < before.at("roof_openings").size(); ++i) {
            if (extension && affected.contains("/roof_openings/" + std::to_string(i) + "/rotation_rad")) {
                const auto version = field(*extension,"version");
                if (!extension->is_object() || !version || !version_one(*version))
                    invalid("Roof uniform transform affects an opaque opening input envelope");
                const auto entries = field(*extension,"entries");
                if (!entries || !entries->is_object() || entries->size() > collection_limit)
                    invalid("Roof uniform transform opening input entries budget exceeded");
                const auto child = field(*entries,identity(before.at("roof_openings").at(i).at("id")));
                if (child && !known_raw(*child))
                    invalid("Roof uniform transform affects an opaque child input envelope");
                if (child && child->contains("rotation_rad"))
                    invalid("Roof uniform transform affects an unsupported opening angle binding");
            }
            const auto archive = [&](const char* key, bool skylight) {
                const auto path = "/roof_openings/" + std::to_string(i) + "/" + (skylight ? "skylight/" : "") + key;
                const auto changed = affected.find(path);
                if (changed == affected.end() || !extension) return;
                const auto version = field(*extension, "version");
                if (!extension->is_object() || !version || !version_one(*version))
                    invalid("Roof uniform transform affects an opaque opening input envelope");
                const auto entries = field(*extension, "entries");
                if (!entries || !entries->is_object() || entries->size() > collection_limit)
                    invalid("Roof uniform transform opening input entries budget exceeded");
                const auto id = identity(before.at("roof_openings").at(i).at("id"));
                const auto child = field(*entries, id);
                if (!child) return;
                if (!known_raw(*child)) invalid("Roof uniform transform affects an opaque child input envelope");
                const auto input = skylight ? field(*child, "skylight") : child;
                if (!input) return;
                if (skylight && !known_raw(*input))
                    invalid("Roof uniform transform affects an opaque skylight input envelope");
                if (const auto receipt = field(*input, key)) {
                    if (skylight && !receipt_core(*receipt, true)) return;
                    admit_opening_receipt(*receipt, changed->second.first);
                    auto& remaining = result.extensions.at("roof_opening_input").at("entries").at(id);
                    if (skylight) {
                        children[id]["skylight"][key] = *receipt;
                        remaining.at("skylight").erase(key);
                    } else {
                        children[id][key] = *receipt;
                        remaining.erase(key);
                    }
                }
            };
            for (const auto* key : opening_scalars) archive(key, false);
            if (before.at("roof_openings").at(i).contains("skylight"))
                for (const auto* key : skylight_scalars) archive(key, true);
        }
    return {{"quantity_entries", std::move(indexed)}, {"roof_opening_input", std::move(children)}};
}
void admit_record(const Json& record, bool skylights, bool rotations) {
    keys(record, {"operation", "source", "result", "receipts"});
    const auto intent = decode_roof_uniform_transform_intent(record.at("operation"));
    const auto& before = record.at("source"), after = record.at("result");
    if (!exact(derive(before, intent, skylights, rotations), after) || exact(before, after))
        invalid("Roof uniform transform derivation does not reproduce its result");
    const auto affected = changes(before, after);
    const auto& receipts = record.at("receipts");
    keys(receipts, {"quantity_entries", "roof_opening_input"});
    const auto& indexed = receipts.at("quantity_entries"), children = receipts.at("roof_opening_input");
    if (!indexed.is_object() || !children.is_object() || indexed.size() > collection_limit || children.size() > opening_limit)
        invalid("Roof uniform transform archived receipt budget exceeded");
    for (const auto& [pointer, receipt] : indexed.items()) {
        const auto changed = affected.find(pointer);
        if (changed == affected.end() || pointer == "/orientation_rad" || pointer.ends_with("/rotation_rad"))
            invalid("Roof uniform transform archive contains an unaffected or unsupported binding");
        admit_quantity_receipt(receipt, changed->second.first);
    }
    for (const auto& [id, values] : children.items()) {
        (void)identity(id);
        if (!values.is_object() || values.empty() || values.size() > opening_scalars.size() + (skylights ? 1 : 0))
            invalid("Roof uniform transform archived cut fields are invalid");
        const Json* cut = nullptr;
        std::size_t index = 0;
        if (before.contains("roof_openings"))
            for (std::size_t i = 0; i < before.at("roof_openings").size(); ++i)
                if (before.at("roof_openings").at(i).at("id") == id) {
                    cut = &before.at("roof_openings").at(i);
                    index = i;
                }
        if (!cut) invalid("Roof uniform transform archive has a dangling cut receipt");
        for (const auto& [key, receipt] : values.items()) {
            if (skylights && key == "skylight" && cut->contains("skylight")) {
                if (!receipt.is_object() || receipt.empty() || receipt.size() > skylight_scalars.size())
                    invalid("Roof uniform transform archived skylight fields are invalid");
                for (const auto& [dimension, input] : receipt.items()) {
                    if (std::find(skylight_scalars.begin(), skylight_scalars.end(), dimension) == skylight_scalars.end() ||
                        !affected.contains("/roof_openings/" + std::to_string(index) + "/skylight/" + dimension))
                        invalid("Roof uniform transform archived skylight receipt is unaffected or unsupported");
                    admit_opening_receipt(input, scalar(cut->at("skylight").at(dimension)));
                }
                continue;
            }
            if (std::find(opening_scalars.begin(), opening_scalars.end(), key) == opening_scalars.end() ||
                !affected.contains("/roof_openings/" + std::to_string(index) + "/" + key))
                invalid("Roof uniform transform archived cut receipt is unaffected or unsupported");
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
        invalid("Roof uniform transform actual source roof is missing or inconsistent");
    validate_roof_uniform_transform_source_entity(found->second);
    const auto object = decode_roof_entity(resolve_vertical_placement(source, found->second));
    (void)make_roof_shape(object);
    return make_roof_structure_shape(object);
}
std::vector<RoofJoin> affected_joins(const Entities& source, const Ids& targets, const ConstraintPhaseScope& scope) {
    const auto qualified_cohorts = phase_qualified_roof_join_cohort_ids(source);
    std::vector<RoofJoin> result;
    std::map<std::string, std::string, std::less<>> owners;
    for (const auto& [id, entity] : source) {
        if (entity.id != id) invalid("Roof uniform transform actual map contains inconsistent identities");
        if (entity.type != "roof_join") continue;
        const auto join = parse_roof_join(entity.properties, id);
        if (scope.inactive_owner_ids.contains(id) && qualified_cohorts.contains(id)) continue;
        const bool affected = std::any_of(join.roof_ids.begin(), join.roof_ids.end(),
            [&](const auto& member) { return targets.contains(member); });
        for (const auto& member : join.roof_ids) {
            const auto roof = source.find(member);
            if (roof == source.end() || roof->second.type != "roof" || roof->second.id != member)
                invalid("Roof uniform transform source has a dangling join member");
            if (!owners.emplace(member, id).second) invalid("Roof uniform transform source has overlapping joins");
            if (affected && scope.inactive_owner_ids.contains(member))
                invalid("Roof uniform transform affected join member is inactive");
        }
        if (!affected) continue;
        if (scope.inactive_owner_ids.contains(id)) invalid("Roof uniform transform affected join is inactive");
        if (result.size() == collection_limit) invalid("Roof uniform transform affected join budget exceeded");
        result.push_back(join);
    }
    return result;
}
void admit_joins(const Entities& source, const std::vector<RoofJoin>& joins) {
    validate_roof_join_ownership(source);
    for (const auto& join : joins) {
        std::vector<TopoDS_Shape> members;
        for (const auto& id : join.roof_ids) members.push_back(resolved_shape(source, id));
        validate_roof_join_skylights(join, source);
        (void)make_roof_join(join, members);
    }
}
} // namespace

nlohmann::json encode_roof_uniform_transform_intent(const RoofUniformTransformIntent& intent) {
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
RoofUniformTransformIntent decode_roof_uniform_transform_intent(const nlohmann::json& value) {
    (void)budget(value);
    keys(value, {"version", "roof_id", "transform"});
    if (!version_one(value.at("version"))) invalid("Roof uniform transform version is unsupported");
    const auto& t = value.at("transform");
    keys(t, {"pivot_m", "offset_m", "rotation_z_radians", "scale", "flip_horizontal", "flip_vertical"});
    if (!t.at("flip_horizontal").is_boolean() || !t.at("flip_vertical").is_boolean())
        invalid("Roof uniform transform reflection flags must be booleans");
    RoofUniformTransformIntent result{identity(value.at("roof_id")),
        {point(t.at("pivot_m")), point(t.at("offset_m")), scalar(t.at("rotation_z_radians")),
            scalar(t.at("scale")), t.at("flip_horizontal").get<bool>(), t.at("flip_vertical").get<bool>()}};
    (void)normalize(result.transform);
    return result;
}
void validate_roof_uniform_transform_derivations(const Entity& source) {
    if (!source.extensions.is_object()) invalid("Roof uniform transform extensions must be an object");
    const auto extension = field(source.extensions, std::string(roof_uniform_transform_derivations_key));
    if (!extension) return;
    (void)budget(*extension);
    keys(*extension, {"version", "operations"});
    const bool rotations = extension->at("version").is_number_integer() && extension->at("version") == 3;
    const bool skylights = version_two(extension->at("version")) || rotations;
    if (!version_one(extension->at("version")) && !skylights)
        invalid("Roof uniform transform derivation namespace collision");
    const auto& operations = extension->at("operations");
    if (!operations.is_array() || operations.empty() || operations.size() > operation_limit)
        invalid("Roof uniform transform derivation operation budget exceeded");
    validate_roof_derivation_cached(rotations ? "roof-uniform-transform-v3" : skylights ? "roof-uniform-transform-v2" : "roof-uniform-transform-v1", extension->dump(), [&] {
        for (const auto& record : operations) admit_record(record, skylights, rotations);
    });
}
nlohmann::json roof_uniform_transform_opaque_remainder(const Entity& source) {
    validate_roof_uniform_transform_derivations(source);
    const auto extension = field(source.extensions, std::string(roof_uniform_transform_derivations_key));
    if (!extension) return Json::object();
    Json operations = Json::array();
    for (const auto& record : extension->at("operations")) {
        const auto& receipts = record.at("receipts");
        Json indexed = Json::object(), children = Json::array();
        for (const auto& [pointer, receipt] : receipts.at("quantity_entries").items())
            indexed[pointer] = receipt_opaque_remainder(receipt, false);
        for (const auto& [id, values] : receipts.at("roof_opening_input").items()) {
            (void)id; // Historical child identities are qualified ordered slots.
            Json child = Json::object();
            for (const auto& [key, receipt] : values.items()) {
                if (key == "skylight") {
                    for (const auto& [dimension, input] : receipt.items())
                        child[key][dimension] = receipt_opaque_remainder(input, true);
                } else child[key] = receipt_opaque_remainder(receipt, true);
            }
            children.push_back(std::move(child));
        }
        operations.push_back({{"receipts", {{"quantity_entries", std::move(indexed)},
            {"roof_opening_input", std::move(children)}}}});
    }
    return {{"operations", std::move(operations)}};
}
void validate_roof_uniform_transform_source_entity(const Entity& source) {
    (void)budget(source.properties);
    (void)budget(source.extensions);
    validate_roof_profile_source_entity(source);
    validate_roof_rigid_transform_derivations(source);
    validate_roof_plan_resize_derivations(source);
    admit_frame(frame(source, 0.0), source.id);
    validate_roof_uniform_transform_derivations(source);
}
Entity stage_roof_uniform_transform_entity(const Entities& actual_source, const RoofUniformTransformIntent& intent) {
    const auto operation = encode_roof_uniform_transform_intent(intent);
    const auto found = actual_source.find(intent.roof_id);
    if (found == actual_source.end() || found->second.id != found->first || found->second.type != "roof")
        invalid("Roof uniform transform actual source roof is missing or inconsistent");
    const auto& source = found->second;
    validate_roof_uniform_transform_source_entity(source);
    if (constraint_phase_scope(actual_source).inactive_owner_ids.contains(intent.roof_id))
        invalid("Roof uniform transform target is inactive");
    const auto resolved = resolve_vertical_placement(actual_source, source);
    (void)make_roof_shape(decode_roof_entity(resolved));
    const double datum = scalar(point(resolved.properties.at("base_position_m")).z -
        point(source.properties.at("base_position_m")).z);
    const auto before = frame(source, datum), after = derive(before, intent);
    if (exact(before, after)) return source;
    auto result = source;
    for (const auto* key : {"base_position_m", "orientation_rad", "run_m", "length_m", "span_m", "rise_m", "overhang_m", "thickness_m"})
        if (before.contains(key)) result.properties.at(key) = after.at(key);
    if (before.contains("roof_openings"))
        for (std::size_t i = 0; i < before.at("roof_openings").size(); ++i) {
            for (const auto* key : opening_scalars)
                result.properties.at("roof_openings").at(i).at(key) = after.at("roof_openings").at(i).at(key);
            if (before.at("roof_openings").at(i).contains("rotation_rad"))
                result.properties.at("roof_openings").at(i).at("rotation_rad") = after.at("roof_openings").at(i).at("rotation_rad");
            if (before.at("roof_openings").at(i).contains("skylight"))
                for (const auto* key : skylight_scalars)
                    result.properties.at("roof_openings").at(i).at("skylight").at(key) =
                        after.at("roof_openings").at(i).at("skylight").at(key);
        }
    const auto receipts = archive_receipts(source, result, before, changes(before, after));
    const auto key = std::string(roof_uniform_transform_derivations_key);
    if (!result.extensions.contains(key))
        result.extensions[key] = {{"version", before.at("version") == 4 ? 3 : before.at("version") == 3 ? 2 : 1}, {"operations", Json::array()}};
    else if (before.at("version") == 4) result.extensions.at(key).at("version") = 3;
    else if (before.at("version") == 3 && result.extensions.at(key).at("version") == 1)
        result.extensions.at(key).at("version") = 2;
    auto& operations = result.extensions.at(key).at("operations");
    if (operations.size() == operation_limit) invalid("Roof uniform transform derivation operation budget exceeded");
    operations.push_back({{"operation", operation}, {"source", before}, {"result", after}, {"receipts", receipts}});
    validate_roof_uniform_transform_source_entity(result);
    return result;
}
std::map<std::string, Entity, std::less<>> replay_roof_uniform_transform_entities(
    const Entities& source, const std::vector<RoofUniformTransformIntent>& intents) {
    if (intents.size() > maximum_architectural_group_targets) invalid("Roof uniform transform target budget exceeded");
    if (intents.empty()) return source;
    const auto scope = constraint_phase_scope(source);
    Ids targets;
    std::size_t operation_bytes = 0, archive_bytes = 0;
    for (const auto& intent : intents) {
        const auto bytes = budget(encode_roof_uniform_transform_intent(intent));
        if (bytes > proof_limit - operation_bytes) invalid("Roof uniform transform batch operation budget exceeded");
        operation_bytes += bytes;
        if (!targets.insert(intent.roof_id).second) invalid("Roof uniform transform contains duplicate targets");
        if (scope.inactive_owner_ids.contains(intent.roof_id)) invalid("Roof uniform transform target is inactive");
        (void)resolved_shape(source, intent.roof_id);
    }
    const auto joins = affected_joins(source, targets, scope);
    admit_joins(source, joins);
    auto result = source;
    for (const auto& intent : intents) {
        auto roof = stage_roof_uniform_transform_entity(source, intent);
        if (const auto archive = field(roof.extensions, std::string(roof_uniform_transform_derivations_key))) {
            const auto bytes = budget(*archive);
            if (bytes > proof_limit - archive_bytes) invalid("Roof uniform transform batch archive budget exceeded");
            archive_bytes += bytes;
        }
        result.at(intent.roof_id) = std::move(roof);
    }
    for (const auto& id : targets) (void)resolved_shape(result, id);
    admit_joins(result, joins);
    return result;
}
std::optional<RoofUniformTransformIntent> capture_roof_uniform_transform(
    const Entities& source, const Entity& candidate, const RoofUniformTransformIntent& actual_intent) {
    (void)identity(candidate.id);
    if (candidate.id != actual_intent.roof_id || candidate.type != "roof")
        invalid("Roof uniform transform candidate must retain the actual source owner");
    (void)budget(candidate.properties);
    (void)budget(candidate.extensions);
    const auto expected = stage_roof_uniform_transform_entity(source, actual_intent);
    if (!exact(expected, candidate))
        invalid("Roof uniform transform candidate differs from complete actual-source replay");
    if (exact(source.at(actual_intent.roof_id), candidate)) return std::nullopt;
    return actual_intent;
}
} // namespace sketch
