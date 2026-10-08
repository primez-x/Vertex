#include "sketch/slab_geometry_edit.hpp"

#include "sketch/assembly_model.hpp"
#include "sketch/constraint_entity.hpp"
#include "sketch/constraint_phase_scope.hpp"
#include "sketch/document_solid.hpp"
#include "sketch/phase_slab_profile_edit.hpp"
#include "sketch/project_organization.hpp"

#include <algorithm>
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
constexpr std::size_t operation_limit = 256;
constexpr std::size_t proof_limit = 1024 * 1024;
constexpr double scalar_limit = 1.0e12;

[[noreturn]] void invalid(const std::string& reason) {
    throw std::invalid_argument("Slab geometry edit: " + reason);
}
const Json* field(const Json& value, const std::string& name) {
    if (!value.is_object()) return nullptr;
    const auto found = value.find(name);
    return found == value.end() ? nullptr : &*found;
}
void keys(const Json& value, std::initializer_list<const char*> names) {
    if (!value.is_object() || value.size() != names.size()) invalid("invalid closed fields");
    for (const auto* name : names) if (!value.contains(name)) invalid("missing closed field");
}
bool version_one(const Json& value) {
    return (value.is_number_integer() || value.is_number_unsigned()) && value == 1;
}
bool version_two(const Json& value) {
    return (value.is_number_integer() || value.is_number_unsigned()) && value == 2;
}
std::string identity(const Json& value) {
    if (!value.is_string()) invalid("identity must be a string");
    const auto& id = value.get_ref<const std::string&>();
    if (id.empty() || id.size() > 128 || !std::all_of(id.begin(), id.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == ':';
    })) invalid("identity must contain 1..128 supported ASCII characters");
    return id;
}
double scalar(double value) {
    if (!std::isfinite(value) || std::abs(value) > scalar_limit) invalid("finite scalar range exceeded");
    return value;
}
double scalar(const Json& value) {
    if (!value.is_number()) invalid("scalar must be numeric");
    return scalar(value.get<double>());
}
Vec2 point(const Json& value) {
    if (!value.is_array() || value.size() != 2) invalid("point must contain two scalars");
    return {scalar(value.at(0)), scalar(value.at(1))};
}
Json point(Vec2 value) { return Json::array({scalar(value.x), scalar(value.y)}); }
Vec3 point3(const Json& value) {
    if (!value.is_array() || value.size() != 3) invalid("model point must contain three scalars");
    return {scalar(value.at(0)), scalar(value.at(1)), scalar(value.at(2))};
}
Json point3(Vec3 value) { return Json::array({scalar(value.x), scalar(value.y), scalar(value.z)}); }
std::size_t index(const Json& value) {
    if ((!value.is_number_integer() && !value.is_number_unsigned()) ||
        (value.is_number_integer() && !value.is_number_unsigned() && value.get<std::int64_t>() < 0) ||
        value.get<std::uint64_t>() >= collection_limit) invalid("index is outside the bounded inventory");
    return value.get<std::size_t>();
}

// Structural and string allocation bounds precede dump(), codecs and native
// work. Full-map/source bounds do not reinterpret any opaque payload.
struct Budget {
    std::size_t nodes{}, bytes{};
    std::size_t node_limit{4 * 1024 * 1024}, byte_limit{64 * 1024 * 1024};
    void text(const std::string& value) {
        if (value.size() > byte_limit - bytes) invalid("JSON string budget exceeded");
        bytes += value.size();
    }
    void read(const Json& value, std::size_t depth = 0) {
        if (depth > 64 || ++nodes > node_limit) invalid("JSON node/nesting budget exceeded");
        if (value.is_number_float() && !std::isfinite(value.get<double>())) invalid("nonfinite JSON scalar");
        if (value.is_string()) text(value.get_ref<const std::string&>());
        else if (value.is_array()) for (const auto& child : value) read(child, depth + 1);
        else if (value.is_object()) for (const auto& [key, child] : value.items()) {
            text(key); read(child, depth + 1);
        }
    }
};
std::size_t proof_budget(const Json& value) {
    Budget budget; budget.node_limit = 65536; budget.byte_limit = proof_limit; budget.read(value);
    const auto bytes = value.dump().size();
    if (bytes > proof_limit) invalid("proof byte budget exceeded");
    return bytes;
}
bool exact(const Json& a, const Json& b) { return a == b && a.dump() == b.dump(); }
void assign(Json& target, double value) {
    if (scalar(target) != scalar(value)) target = value;
}
void assign_point(Json& target, Vec2 value) {
    assign(target.at(0), value.x); assign(target.at(1), value.y);
}
Segment segment(const Json& value) {
    keys(value, {"start", "end", "sweep_radians"});
    return {point(value.at("start")), point(value.at("end")), scalar(value.at("sweep_radians"))};
}
Boundary ring(const Json& value, std::size_t& count) {
    if (!value.is_array() || value.empty() || value.size() > collection_limit - count)
        invalid("geometry segment budget exceeded");
    count += value.size();
    Boundary result; result.reserve(value.size());
    for (const auto& raw : value) result.push_back(segment(raw));
    return result;
}
void admit_frame(const Json& value) {
    (void)proof_budget(value);
    keys(value, {"boundary", "holes"});
    std::size_t count = 0;
    const auto outer = ring(value.at("boundary"), count);
    const auto& raw_holes = value.at("holes");
    if (!raw_holes.is_array() || raw_holes.size() > collection_limit) invalid("hole budget exceeded");
    std::vector<Boundary> holes; holes.reserve(raw_holes.size());
    for (const auto& hole : raw_holes) holes.push_back(ring(hole, count));
    if (const auto error = validate_boundary_holes(outer, holes)) invalid(*error);
}
Json frame(const Entity& source) {
    // Select actual geometry fields, preserving their numeric representations.
    // Segment metadata and live layers remain only in the unchanged source.
    const auto copy_ring = [](const Json& raw) {
        if (!raw.is_array() || raw.empty() || raw.size() > collection_limit) invalid("invalid source ring");
        Json result = Json::array();
        for (const auto& item : raw) {
            if (!item.is_object()) invalid("invalid source segment");
            result.push_back({{"start", item.at("start")}, {"end", item.at("end")},
                {"sweep_radians", item.at("sweep_radians")}});
        }
        return result;
    };
    const auto& p = source.properties;
    if (!p.contains("boundary") || !p.contains("holes") || !p.at("holes").is_array() ||
        p.at("holes").size() > collection_limit) invalid("actual slab footprint is missing or unbounded");
    std::size_t count = p.at("boundary").is_array() ? p.at("boundary").size() : 0;
    for (const auto& hole : p.at("holes")) {
        if (!hole.is_array() || hole.size() > collection_limit - std::min(count, collection_limit))
            invalid("actual geometry segment budget exceeded");
        count += hole.size();
    }
    if (count > collection_limit) invalid("actual geometry segment budget exceeded");
    Json holes = Json::array();
    for (const auto& hole : p.at("holes")) holes.push_back(copy_ring(hole));
    Json result{{"boundary", copy_ring(p.at("boundary"))}, {"holes", std::move(holes)}};
    admit_frame(result);
    return result;
}
bool rigid_identity(const PlanarTransform& t) {
    if (t.offset.x != 0 || t.offset.y != 0 || t.flip_horizontal != t.flip_vertical) return false;
    const auto angle = std::remainder(t.rotation_radians, 2 * std::numbers::pi);
    return std::remainder(angle + (t.flip_horizontal ? std::numbers::pi : 0),
        2 * std::numbers::pi) == 0;
}
Vec2 resize_point(Vec2 p, const SlabPlanAxisResize& r) {
    if (r.scale_x == 1 && r.scale_y == 1) return p;
    const auto dx = p.x - r.anchor_m.x, dy = p.y - r.anchor_m.y;
    // An exactly uniform scale is independent of frame angle. Avoid rotation
    // roundoff and never approximate distinct scales to retain circular arcs.
    if (r.scale_x == r.scale_y)
        return {scalar(r.anchor_m.x + dx * r.scale_x), scalar(r.anchor_m.y + dy * r.scale_y)};
    const auto angle = std::remainder(r.frame_rotation_radians, 2 * std::numbers::pi);
    const auto c = std::cos(angle), s = std::sin(angle);
    const auto u = (c * dx + s * dy) * r.scale_x, v = (-s * dx + c * dy) * r.scale_y;
    return {scalar(r.anchor_m.x + c * u - s * v), scalar(r.anchor_m.y + s * u + c * v)};
}
Json derive(const Json& before, const SlabGeometryEditIntent& intent) {
    admit_frame(before);
    auto after = before;
    if (intent.kind == SlabGeometryEditKind::move_vertex) {
        const auto& v = *intent.vertex;
        auto* selected = &after.at("boundary");
        if (v.hole_index) {
            if (*v.hole_index >= after.at("holes").size()) invalid("hole index is outside actual source");
            selected = &after.at("holes").at(*v.hole_index);
        }
        if (v.vertex_index >= selected->size()) invalid("vertex index is outside actual source");
        const auto captured = point(selected->at(v.vertex_index).at("start"));
        // Closure admission permits tiny retained endpoint differences. Match
        // the existing vertex stager's exact start-position no-op, rather than
        // silently repairing its preceding end during an unchanged gesture.
        if (captured.x == v.proposed_position.x && captured.y == v.proposed_position.y) return before;
        const auto previous = v.vertex_index == 0 ? selected->size() - 1 : v.vertex_index - 1;
        assign_point(selected->at(v.vertex_index).at("start"), v.proposed_position);
        assign_point(selected->at(previous).at("end"), v.proposed_position);
    } else {
        if (intent.kind == SlabGeometryEditKind::transform_plan && intent.uniform_scale == 1 &&
            rigid_identity(*intent.transform)) return before;
        if (intent.kind == SlabGeometryEditKind::resize_plan &&
            intent.resize->scale_x == 1 && intent.resize->scale_y == 1) return before;
        const auto update = [&](Json& raw_ring) {
            for (auto& raw : raw_ring) {
                auto value = segment(raw);
                if (intent.kind == SlabGeometryEditKind::transform_plan) {
                    if (intent.uniform_scale != 1) {
                        const auto pivot = intent.transform->pivot;
                        const auto scaled = [&](Vec2 p) {
                            return Vec2{scalar(pivot.x + (p.x - pivot.x) * intent.uniform_scale),
                                scalar(pivot.y + (p.y - pivot.y) * intent.uniform_scale)};
                        };
                        value.start = scaled(value.start); value.end = scaled(value.end);
                    }
                    value = transform_segment(value, *intent.transform);
                } else {
                    const auto& r = *intent.resize;
                    if (value.sweep_radians != 0 && r.scale_x != r.scale_y)
                        invalid("unequal plan scales would require an unsupported elliptical arc");
                    value.start = resize_point(value.start, r); value.end = resize_point(value.end, r);
                }
                assign_point(raw.at("start"), value.start); assign_point(raw.at("end"), value.end);
                assign(raw.at("sweep_radians"), value.sweep_radians);
            }
        };
        update(after.at("boundary"));
        for (auto& hole : after.at("holes")) update(hole);
    }
    admit_frame(after);
    return after;
}
PlanarTransform plan_projection(const SlabModelTransform& t) {
    return {{t.pivot_m.x, t.pivot_m.y}, t.rotation_radians,
        t.flip_horizontal, t.flip_vertical, {t.offset_m.x, t.offset_m.y}};
}
double profile_scalar(const Json& profile, const char* canonical, const char* alias, bool positive) {
    const auto& a = profile.at(canonical); const auto& b = profile.at(alias);
    if (a.is_null() && b.is_null()) invalid("model profile scalar is missing");
    const auto value = scalar(a.is_null() ? b : a);
    if (!b.is_null() && scalar(b) != value) invalid("model profile aliases disagree");
    if (positive && value <= default_geometry_tolerance_metres) invalid("model profile thickness must be positive");
    return value;
}
void admit_model_frame(const Json& value) {
    (void)proof_budget(value);
    keys(value, {"boundary", "holes", "profile", "elevation_shift_m"});
    const Json geometry{{"boundary", value.at("boundary")}, {"holes", value.at("holes")}};
    admit_frame(geometry);
    const auto& profile = value.at("profile");
    keys(profile, {"thickness_m", "thickness", "elevation_m", "elevation", "layers"});
    const auto thickness = profile_scalar(profile, "thickness_m", "thickness", true);
    const auto elevation = profile_scalar(profile, "elevation_m", "elevation", false);
    const auto shift = scalar(value.at("elevation_shift_m"));
    const auto& layers = profile.at("layers");
    if (!layers.is_null()) {
        if (!layers.is_array() || layers.size() > 1024) invalid("model frame layer budget exceeded");
        double total = 0;
        for (const auto& layer : layers) {
            const auto amount = scalar(layer);
            if (amount <= default_geometry_tolerance_metres) invalid("model frame layer thickness must be positive");
            total = scalar(total + amount);
        }
        if (!layers.empty() && std::abs(total - thickness) >
            std::max(default_geometry_tolerance_metres, std::abs(thickness) * 1e-9))
            invalid("model frame layers must sum to total thickness");
    }
    std::size_t count = 0;
    Slab slab; slab.boundary = ring(value.at("boundary"), count);
    for (const auto& hole : value.at("holes")) slab.holes.push_back(ring(hole, count));
    slab.thickness = thickness; slab.elevation = scalar(elevation + shift);
    // Historical mathematical frames have no identity/material authority.
    // Positive ordered layers are checked above; native admission checks the
    // complete analytical footprint extruded by its actual profile total.
    (void)make_slab(slab);
}
Json model_frame(const Entity& source, double actual_shift) {
    auto result = frame(source);
    Json profile = Json::object();
    for (const auto* name : {"thickness_m", "thickness", "elevation_m", "elevation"}) {
        const auto value = field(source.properties, name);
        profile[name] = value ? *value : Json(nullptr);
    }
    profile["layers"] = nullptr;
    if (const auto layers = field(source.properties, "layers")) {
        if (!layers->is_array() || layers->size() > 1024) invalid("model frame layer budget exceeded");
        profile["layers"] = Json::array();
        for (const auto& layer : *layers) profile.at("layers").push_back(layer.at("thickness_m"));
    }
    result["profile"] = std::move(profile);
    result["elevation_shift_m"] = scalar(actual_shift);
    admit_model_frame(result);
    return result;
}
Json derive_model(const Json& before, const SlabGeometryEditIntent& intent) {
    admit_model_frame(before);
    const auto& t = *intent.model_transform;
    const auto planar = plan_projection(t);
    if (t.uniform_scale == 1 && t.offset_m.z == 0 && rigid_identity(planar)) return before;
    // Reuse the exact v1 analytical plan operation without changing its wire
    // or semantics. Only the model lane also scales the physical profile.
    SlabGeometryEditIntent plan;
    plan.slab_id = intent.slab_id; plan.kind = SlabGeometryEditKind::transform_plan;
    plan.transform = planar; plan.uniform_scale = t.uniform_scale;
    const auto projected = derive({{"boundary", before.at("boundary")}, {"holes", before.at("holes")}}, plan);
    auto after = before;
    after.at("boundary") = projected.at("boundary"); after.at("holes") = projected.at("holes");
    auto& profile = after.at("profile");
    for (const auto* name : {"thickness_m", "thickness"})
        if (!profile.at(name).is_null() && t.uniform_scale != 1)
            assign(profile.at(name), scalar(profile.at(name)) * t.uniform_scale);
    if (!profile.at("layers").is_null() && t.uniform_scale != 1)
        for (auto& amount : profile.at("layers")) assign(amount, scalar(amount) * t.uniform_scale);
    const auto raw_elevation = profile_scalar(before.at("profile"), "elevation_m", "elevation", false);
    const auto shift = scalar(before.at("elevation_shift_m"));
    // Avoid cancellation for pure XYZ moves, including exact identity. For
    // scale, the source world coordinate comes solely from actual placement.
    const auto new_elevation = t.uniform_scale == 1 ? scalar(raw_elevation + t.offset_m.z) :
        scalar(t.pivot_m.z + (scalar(raw_elevation + shift) - t.pivot_m.z) *
            t.uniform_scale + t.offset_m.z - shift);
    for (const auto* name : {"elevation_m", "elevation"})
        if (!profile.at(name).is_null()) assign(profile.at(name), new_elevation);
    admit_model_frame(after);
    return after;
}
void apply_frame(Entity& target, const Json& value) {
    const auto update = [](Json& raw, const Json& derived) {
        for (std::size_t i = 0; i < raw.size(); ++i)
            for (const auto* name : {"start", "end", "sweep_radians"})
                raw.at(i).at(name) = derived.at(i).at(name);
    };
    update(target.properties.at("boundary"), value.at("boundary"));
    for (std::size_t i = 0; i < value.at("holes").size(); ++i)
        update(target.properties.at("holes").at(i), value.at("holes").at(i));
}
void apply_model_frame(Entity& target, const Json& value) {
    apply_frame(target, value);
    const auto& profile = value.at("profile");
    for (const auto* name : {"thickness_m", "thickness", "elevation_m", "elevation"})
        if (!profile.at(name).is_null()) target.properties.at(name) = profile.at(name);
    if (!profile.at("layers").is_null())
        for (std::size_t i = 0; i < profile.at("layers").size(); ++i)
            target.properties.at("layers").at(i).at("thickness_m") = profile.at("layers").at(i);
}

using Scalars = std::map<std::string, double, std::less<>>;
Scalars geometry_scalars(const Json& value) {
    Scalars result;
    const auto add = [&](const Json& raw, const std::string& prefix) {
        for (std::size_t i = 0; i < raw.size(); ++i) {
            const auto path = prefix + "/" + std::to_string(i);
            for (const auto* name : {"start", "end"})
                for (std::size_t axis = 0; axis < 2; ++axis)
                    result.emplace(path + "/" + name + "/" + std::to_string(axis), scalar(raw.at(i).at(name).at(axis)));
            result.emplace(path + "/sweep_radians", scalar(raw.at(i).at("sweep_radians")));
        }
    };
    add(value.at("boundary"), "/boundary");
    for (std::size_t i = 0; i < value.at("holes").size(); ++i)
        add(value.at("holes").at(i), "/holes/" + std::to_string(i));
    if (const auto profile = field(value, "profile")) {
        for (const auto* name : {"thickness_m", "thickness", "elevation_m", "elevation"})
            if (!profile->at(name).is_null()) result.emplace("/" + std::string(name), scalar(profile->at(name)));
        if (!profile->at("layers").is_null())
            for (std::size_t i = 0; i < profile->at("layers").size(); ++i)
                result.emplace("/layers/" + std::to_string(i) + "/thickness_m", scalar(profile->at("layers").at(i)));
    }
    return result;
}
bool descendant(std::string_view path, std::string_view parent) {
    return path.size() > parent.size() && path.starts_with(parent) && path[parent.size()] == '/';
}
Ids changed_segments(const Scalars& before, const Scalars& after) {
    Ids result;
    for (const auto& [path, value] : before) if (after.at(path) != value) {
        if (!path.starts_with("/boundary/") && !path.starts_with("/holes/")) {
            result.insert(path); continue;
        }
        auto end = path.rfind('/');
        if (!path.ends_with("/sweep_radians")) end = path.rfind('/', end - 1);
        result.insert(path.substr(0, end));
    }
    return result;
}
void admit_receipt(const Json& raw, double actual) {
    (void)proof_budget(raw);
    const auto version = field(raw, "version"), expression = field(raw, "original_expression");
    if (!version || !version_one(*version)) invalid("affected geometry receipt version is unsupported");
    if (!expression || !expression->is_string() || expression->get_ref<const std::string&>().size() > 4096)
        invalid("geometry receipt expression budget exceeded");
    if (decode_constraint_quantity_receipt(raw).metres != actual) invalid("affected geometry receipt is stale");
}
Json retire_receipts(const Entity& source, Entity& result, const Json& before, const Json& after) {
    Json archived = Json::object();
    const auto entries = field(source.properties, "quantity_entries");
    if (!entries) return {{"quantity_entries", std::move(archived)}};
    if (!entries->is_object() || entries->size() > collection_limit) invalid("quantity entry budget exceeded");
    const auto old_values = geometry_scalars(before), new_values = geometry_scalars(after);
    auto affected = changed_segments(old_values, new_values);
    // An absent alias can still carry an opaque future binding. Treat known
    // scalar aliases as one affected dimension without materializing fields
    // or adding mathematical archive scalars that did not exist in source.
    if (affected.contains("/thickness_m") || affected.contains("/thickness")) {
        affected.insert("/thickness_m"); affected.insert("/thickness");
    }
    if (affected.contains("/elevation_m") || affected.contains("/elevation")) {
        affected.insert("/elevation_m"); affected.insert("/elevation");
    }
    for (const auto& [path, value] : old_values)
        if (path.starts_with("/layers/") && path.ends_with("/thickness_m") && new_values.at(path) != value)
            affected.insert(path.substr(0, path.size() - std::string_view("thickness_m").size()) + "thickness");
    for (const auto& [path, raw] : entries->items()) {
        const auto old = old_values.find(path);
        if (old != old_values.end()) {
            if (old->second == new_values.at(path)) continue;
            admit_receipt(raw, old->second);
            archived[path] = raw;
            result.properties.at("quantity_entries").erase(path);
            continue;
        }
        for (const auto& changed : affected)
            if (path == changed || descendant(path, changed) || descendant(changed, path))
                invalid("affected geometry quantity binding is unsupported");
    }
    return {{"quantity_entries", std::move(archived)}};
}
void admit_record(const Json& record) {
    keys(record, {"operation", "source", "result", "receipts"});
    const auto intent = decode_slab_geometry_edit_intent(record.at("operation"));
    const auto& before = record.at("source"), after = record.at("result");
    const auto derived = intent.kind == SlabGeometryEditKind::transform_model ? derive_model(before, intent) : derive(before, intent);
    if (!exact(derived, after) || exact(before, after)) invalid("archive result differs from its mathematical operation");
    const auto& receipts = record.at("receipts");
    keys(receipts, {"quantity_entries"});
    const auto& values = receipts.at("quantity_entries");
    if (!values.is_object() || values.size() > collection_limit) invalid("archive receipt budget exceeded");
    const auto old_values = geometry_scalars(before), new_values = geometry_scalars(after);
    for (const auto& [path, raw] : values.items()) {
        const auto old = old_values.find(path);
        if (old == old_values.end() || old->second == new_values.at(path)) invalid("archive receipt does not bind a changed source scalar");
        admit_receipt(raw, old->second);
    }
}
Slab actual_slab(const Entity& source, bool local = true) {
    if (source.type != "slab" || !source.properties.is_object() || !source.extensions.is_object())
        invalid("actual source must be a slab with object payloads");
    (void)identity(source.id);
    Budget budget; budget.read(source.properties); budget.read(source.extensions);
    const auto layers = field(source.properties, "layers");
    if (layers && (!layers->is_array() || layers->size() > 1024)) invalid("live layer inventory budget exceeded");
    for (const auto* name : {"thickness_m", "thickness", "elevation_m", "elevation"})
        if (const auto value = field(source.properties, name)) (void)scalar(*value);
    if (layers) for (const auto& layer : *layers)
        for (const auto* name : {"thickness_m", "thickness"})
            if (const auto value = field(layer, name)) (void)scalar(*value);
    (void)frame(source);
    validate_slab_geometry_derivation(source);
    if (local) validate_slab_profile_source_entity(source);
    Slab slab; std::string error;
    if (!read_document_slab(source, slab, error)) invalid("actual native slab " + source.id + ": " + error);
    // Local profile admission already constructs this complete actual solid.
    // Resolved transient sources need their own native admission.
    if (!local) (void)make_slab(slab);
    return slab;
}
void admit_map(const Entities& source, const Ids& targets) {
    const auto organization = organize_project(source);
    std::map<std::string, AssemblyModel, std::less<>> catalogs;
    const auto bind = [&](const WallLayerMaterial& reference) {
        (void)identity(reference.catalog_id); (void)identity(reference.material_id);
        const auto found = source.find(reference.catalog_id);
        if (found == source.end() || found->second.type != "assembly_model" ||
            !found->second.properties.is_object() || !found->second.properties.contains("model"))
            invalid("material requires its actual assembly catalog");
        if (!catalogs.contains(reference.catalog_id)) catalogs.emplace(reference.catalog_id,
            AssemblyModel::from_json(found->second.properties.at("model")));
        const auto& materials = catalogs.at(reference.catalog_id).materials();
        if (std::none_of(materials.begin(), materials.end(), [&](const auto& m) { return m.id == reference.material_id; }))
            invalid("material is absent from its actual catalog");
    };
    for (const auto& id : targets) {
        const auto& entity = source.at(id);
        (void)actual_slab(entity);
        const bool scoped = entity.properties.contains("property_id") || entity.properties.contains("building_id") ||
            entity.properties.contains("floor_id") || entity.properties.contains("layer_id") ||
            entity.properties.contains("level_id") || entity.properties.contains("wall_id");
        const auto node = organization.nodes.find(id);
        if (scoped && (node == organization.nodes.end() || !node->second.issues.empty()))
            invalid("target has unresolved actual drawing context");
        // Only the actual resolver changes transient Z. Its retained legacy
        // alias is intentionally not reinterpreted as a local scalar conflict.
        const auto slab = actual_slab(resolve_vertical_placement(source, entity), false);
        if (const auto reference = field(entity.properties, "material_assignment")) {
            const auto version = field(*reference, "version"), catalog = field(*reference, "catalog_id"), item = field(*reference, "material_id");
            if (!version || !version_one(*version) || !catalog || !item) invalid("unsupported slab material assignment");
            bind({identity(*catalog), identity(*item)});
        }
        for (const auto& layer : slab.layers) if (layer.material) bind(*layer.material);
    }
}
} // namespace

nlohmann::json encode_slab_geometry_edit_intent(const SlabGeometryEditIntent& intent) {
    (void)identity(intent.slab_id);
    Json result{{"version", 1}, {"slab_id", intent.slab_id}, {"kind", ""},
        {"vertex", nullptr}, {"transform", nullptr}, {"resize", nullptr}};
    switch (intent.kind) {
    case SlabGeometryEditKind::move_vertex: {
        if (!intent.vertex || intent.transform || intent.resize || intent.uniform_scale != 1 || intent.model_transform)
            invalid("vertex intent must be exclusive");
        const auto& v = *intent.vertex;
        if (v.vertex_index >= collection_limit || (v.hole_index && *v.hole_index >= collection_limit)) invalid("vertex index budget exceeded");
        result.at("kind") = "move_vertex";
        result.at("vertex") = {{"vertex_index", v.vertex_index}, {"proposed_position_m", point(v.proposed_position)},
            {"hole_index", v.hole_index ? Json(*v.hole_index) : Json(nullptr)}};
        break;
    }
    case SlabGeometryEditKind::transform_plan: {
        if (intent.vertex || !intent.transform || intent.resize || intent.model_transform) invalid("transform intent must be exclusive");
        if (scalar(intent.uniform_scale) <= 0) invalid("uniform plan scale must be positive");
        const auto& t = *intent.transform;
        result.at("kind") = "transform_plan";
        result.at("transform") = {{"pivot_m", point(t.pivot)}, {"rotation_radians", scalar(t.rotation_radians)},
            {"flip_horizontal", t.flip_horizontal}, {"flip_vertical", t.flip_vertical}, {"offset_m", point(t.offset)},
            {"uniform_scale", intent.uniform_scale}};
        break;
    }
    case SlabGeometryEditKind::resize_plan: {
        if (intent.vertex || intent.transform || !intent.resize || intent.uniform_scale != 1 || intent.model_transform)
            invalid("resize intent must be exclusive");
        const auto& r = *intent.resize;
        if (scalar(r.scale_x) <= 0 || scalar(r.scale_y) <= 0) invalid("plan scales must be positive");
        result.at("kind") = "resize_plan";
        result.at("resize") = {{"scale_x", r.scale_x}, {"scale_y", r.scale_y}, {"anchor_m", point(r.anchor_m)},
            {"frame_rotation_radians", scalar(r.frame_rotation_radians)}};
        break;
    }
    case SlabGeometryEditKind::transform_model: {
        if (intent.vertex || intent.transform || intent.resize || intent.uniform_scale != 1 || !intent.model_transform)
            invalid("model transform intent must be exclusive");
        const auto& t = *intent.model_transform;
        if (scalar(t.uniform_scale) <= 0) invalid("uniform model scale must be positive");
        result.at("version") = 2; result.at("kind") = "transform_model";
        result.at("transform") = {{"pivot_m", point3(t.pivot_m)}, {"offset_m", point3(t.offset_m)},
            {"rotation_radians", scalar(t.rotation_radians)}, {"uniform_scale", t.uniform_scale},
            {"flip_horizontal", t.flip_horizontal}, {"flip_vertical", t.flip_vertical}};
        break;
    }
    default: invalid("unsupported geometry edit kind");
    }
    (void)proof_budget(result);
    return result;
}
SlabGeometryEditIntent decode_slab_geometry_edit_intent(const nlohmann::json& value) {
    (void)proof_budget(value);
    keys(value, {"version", "slab_id", "kind", "vertex", "transform", "resize"});
    if ((!version_one(value.at("version")) && !version_two(value.at("version"))) ||
        !value.at("kind").is_string()) invalid("unsupported intent version or kind");
    SlabGeometryEditIntent result; result.slab_id = identity(value.at("slab_id"));
    const auto& kind = value.at("kind");
    if (version_two(value.at("version")) != (kind == "transform_model"))
        invalid("intent version does not match its operation kind");
    if (kind == "move_vertex") {
        if (!value.at("transform").is_null() || !value.at("resize").is_null()) invalid("vertex wire must be exclusive");
        const auto& v = value.at("vertex"); keys(v, {"vertex_index", "proposed_position_m", "hole_index"});
        result.kind = SlabGeometryEditKind::move_vertex;
        result.vertex = FootprintVertexEdit{index(v.at("vertex_index")), point(v.at("proposed_position_m")),
            v.at("hole_index").is_null() ? std::nullopt : std::optional<std::size_t>(index(v.at("hole_index")))};
    } else if (kind == "transform_plan") {
        if (!value.at("vertex").is_null() || !value.at("resize").is_null()) invalid("transform wire must be exclusive");
        const auto& t = value.at("transform");
        keys(t, {"pivot_m", "rotation_radians", "flip_horizontal", "flip_vertical", "offset_m", "uniform_scale"});
        if (!t.at("flip_horizontal").is_boolean() || !t.at("flip_vertical").is_boolean()) invalid("reflection flags must be booleans");
        result.kind = SlabGeometryEditKind::transform_plan;
        result.transform = PlanarTransform{point(t.at("pivot_m")), scalar(t.at("rotation_radians")),
            t.at("flip_horizontal").get<bool>(), t.at("flip_vertical").get<bool>(), point(t.at("offset_m"))};
        result.uniform_scale = scalar(t.at("uniform_scale"));
    } else if (kind == "resize_plan") {
        if (!value.at("vertex").is_null() || !value.at("transform").is_null()) invalid("resize wire must be exclusive");
        const auto& r = value.at("resize"); keys(r, {"scale_x", "scale_y", "anchor_m", "frame_rotation_radians"});
        result.kind = SlabGeometryEditKind::resize_plan;
        result.resize = SlabPlanAxisResize{scalar(r.at("scale_x")), scalar(r.at("scale_y")), point(r.at("anchor_m")),
            scalar(r.at("frame_rotation_radians"))};
    } else if (kind == "transform_model") {
        if (!value.at("vertex").is_null() || !value.at("resize").is_null()) invalid("model wire must be exclusive");
        const auto& t = value.at("transform");
        keys(t, {"pivot_m", "offset_m", "rotation_radians", "uniform_scale", "flip_horizontal", "flip_vertical"});
        if (!t.at("flip_horizontal").is_boolean() || !t.at("flip_vertical").is_boolean())
            invalid("reflection flags must be booleans");
        result.kind = SlabGeometryEditKind::transform_model;
        result.model_transform = SlabModelTransform{point3(t.at("pivot_m")), point3(t.at("offset_m")),
            scalar(t.at("rotation_radians")), scalar(t.at("uniform_scale")),
            t.at("flip_horizontal").get<bool>(), t.at("flip_vertical").get<bool>()};
    } else invalid("unsupported intent kind");
    (void)encode_slab_geometry_edit_intent(result);
    return result;
}
void validate_slab_geometry_derivation(const Entity& source) {
    if (!source.extensions.is_object()) invalid("extensions must be an object");
    const auto archive = field(source.extensions, std::string(slab_geometry_derivations_key));
    if (!archive) return;
    (void)proof_budget(*archive);
    keys(*archive, {"version", "operations"});
    const bool model_archive = version_two(archive->at("version"));
    if (!version_one(archive->at("version")) && !model_archive) invalid("unsupported derivation archive namespace");
    const auto& operations = archive->at("operations");
    if (!operations.is_array() || operations.empty() || operations.size() > operation_limit) invalid("archive operation budget exceeded");
    bool contains_model = false;
    for (const auto& record : operations) {
        const auto intent = decode_slab_geometry_edit_intent(record.at("operation"));
        const bool model = intent.kind == SlabGeometryEditKind::transform_model;
        if (model && !model_archive) invalid("version one archive cannot contain a model operation");
        contains_model = contains_model || model;
        admit_record(record);
    }
    if (model_archive && !contains_model) invalid("version two archive requires a model operation");
}
namespace {
Entity replay_with_actual_shift(const Entity& source, const SlabGeometryEditIntent& intent,
    std::optional<double> actual_shift) {
    const auto operation = encode_slab_geometry_edit_intent(intent);
    if (source.id != intent.slab_id) invalid("target differs from actual live source identity");
    (void)actual_slab(source);
    const bool model = intent.kind == SlabGeometryEditKind::transform_model;
    if (model && !actual_shift) invalid("model replay requires actual resolved vertical placement");
    const auto before = model ? model_frame(source, *actual_shift) : frame(source);
    const auto after = model ? derive_model(before, intent) : derive(before, intent);
    if (exact(before, after)) return source;
    auto result = intent.kind == SlabGeometryEditKind::move_vertex
        ? stage_architectural_footprint_vertex_entity(source, *intent.vertex) : source;
    if (model) apply_model_frame(result, after);
    else if (intent.kind != SlabGeometryEditKind::move_vertex) apply_frame(result, after);
    if (!exact(model ? model_frame(result, *actual_shift) : frame(result), after))
        invalid("actual footprint/profile staging differs from mathematical replay");
    const auto receipts = retire_receipts(source, result, before, after);
    const auto key = std::string(slab_geometry_derivations_key);
    if (!result.extensions.contains(key)) result.extensions[key] = {{"version", model ? 2 : 1}, {"operations", Json::array()}};
    else if (model) result.extensions.at(key).at("version") = 2;
    auto& operations = result.extensions.at(key).at("operations");
    if (operations.size() >= operation_limit) invalid("archive operation budget exceeded");
    operations.push_back({{"operation", operation}, {"source", before}, {"result", after}, {"receipts", receipts}});
    (void)actual_slab(result);
    return result;
}
} // namespace
Entity replay_slab_geometry_entity(const Entity& source, const SlabGeometryEditIntent& intent) {
    // Detached replay has no authority to infer a bound floor/level shift.
    // An explicit absolute placement has no level shift. Admit that closed
    // envelope directly; level mode must use the complete actual map below.
    std::optional<double> shift;
    if (intent.kind == SlabGeometryEditKind::transform_model) {
        if (const auto placement = field(source.properties, "vertical_placement")) {
            keys(*placement, {"version", "mode", "offset_m"});
            if (!version_one(placement->at("version")) || !placement->at("mode").is_string() ||
                placement->at("mode") != "absolute")
                invalid("detached model replay cannot resolve level placement");
            if (std::abs(scalar(placement->at("offset_m"))) > 1e9)
                invalid("absolute placement offset range exceeded");
        }
        shift = 0;
    }
    return replay_with_actual_shift(source, intent, shift);
}
std::map<std::string, Entity, std::less<>> replay_slab_geometry_entities(
    const std::map<std::string, Entity, std::less<>>& source, const std::vector<SlabGeometryEditIntent>& intents) {
    if (source.size() > 65536 || intents.size() > collection_limit) invalid("entity/target budget exceeded");
    if (intents.empty()) return source;
    Budget map_budget;
    for (const auto& [id, entity] : source) {
        (void)identity(id);
        if (entity.id != id) invalid("actual map contains inconsistent live identities");
        map_budget.text(id); map_budget.text(entity.type);
        map_budget.read(entity.properties); map_budget.read(entity.extensions);
    }
    Ids targets; std::size_t bytes = 0;
    for (const auto& intent : intents) {
        const auto size = proof_budget(encode_slab_geometry_edit_intent(intent));
        if (size > proof_limit - bytes) invalid("batch intent byte budget exceeded");
        bytes += size;
        if (!targets.insert(intent.slab_id).second) invalid("duplicate geometry target");
        if (!source.contains(intent.slab_id)) invalid("target is absent from actual source map");
    }
    const auto scope = constraint_phase_scope(source);
    for (const auto& id : targets) if (scope.inactive_owner_ids.contains(id)) invalid("geometry target is inactive in the saved design");
    admit_map(source, targets);
    auto result = source;
    std::size_t archive_bytes = 0;
    for (const auto& intent : intents) {
        std::optional<double> shift;
        if (intent.kind == SlabGeometryEditKind::transform_model) {
            const auto& raw = source.at(intent.slab_id);
            const auto original = actual_slab(raw);
            const auto resolved = actual_slab(resolve_vertical_placement(source, raw), false);
            shift = scalar(resolved.elevation - original.elevation);
        }
        auto slab = replay_with_actual_shift(source.at(intent.slab_id), intent, shift);
        if (const auto archive = field(slab.extensions, std::string(slab_geometry_derivations_key))) {
            const auto size = proof_budget(*archive);
            if (size > proof_limit - archive_bytes) invalid("batch archive byte budget exceeded");
            archive_bytes += size;
        }
        result.at(intent.slab_id) = std::move(slab);
    }
    admit_map(result, targets);
    return result;
}
} // namespace sketch
