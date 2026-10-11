#include "sketch/assembly_dimension.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <initializer_list>
#include <map>
#include <numbers>
#include <stdexcept>
#include <utility>

namespace sketch {
namespace {
using Json = nlohmann::json;
using Component = AssemblyInstancePropertyEditTarget;
using Path = std::vector<std::string>;

void require(bool condition, const char* message) {
    if (!condition) throw std::invalid_argument(std::string("Assembly dimension: ") + message);
}
void utf8(std::string_view value) {
    for (std::size_t i = 0; i < value.size(); ++i) {
        const auto c = static_cast<unsigned char>(value[i]);
        if (c < 0x80) continue;
        const std::size_t tail = c >= 0xc2 && c <= 0xdf ? 1 :
            c >= 0xe0 && c <= 0xef ? 2 : c >= 0xf0 && c <= 0xf4 ? 3 : 0;
        require(tail && tail < value.size() - i, "invalid UTF-8");
        const auto first = static_cast<unsigned char>(value[i + 1]);
        require(!((c == 0xe0 && first < 0xa0) || (c == 0xed && first >= 0xa0) ||
            (c == 0xf0 && first < 0x90) || (c == 0xf4 && first >= 0x90)), "invalid UTF-8");
        for (std::size_t j = 1; j <= tail; ++j) {
            const auto byte = static_cast<unsigned char>(value[i + j]);
            require(byte >= 0x80 && byte <= 0xbf, "invalid UTF-8");
        }
        i += tail;
    }
}
void document_id(std::string_view id) {
    require(!id.empty() && id.size() <= 128 && std::all_of(id.begin(), id.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == ':';
    }), "invalid document identity");
}
void local_id(std::string_view id) {
    require(!id.empty() && id.size() <= 4096 && id.find('\0') == std::string_view::npos &&
        !std::all_of(id.begin(), id.end(), [](unsigned char c) { return std::isspace(c); }),
        "invalid local identity");
    utf8(id);
}
void topology_id(std::string_view id) {
    local_id(id);
    require(id.size() <= 128, "topology identity exceeds bound");
}
void validate_target(const AssemblyDimensionTarget& target) {
    document_id(target.component.catalog_id);
    local_id(target.component.instance_id);
    if (target.component.document_entity_id) document_id(*target.component.document_entity_id);
    require(target.component.part_path.size() < 32, "part path exceeds depth bound");
    for (const auto& part : target.component.part_path) local_id(part);
    (void)assembly_dimension_axis_name(target.axis);
    if (!target.profile) return;
    require(target.axis == AssemblyDimensionAxis::width, "profile target has a competing extent axis");
    const auto& profile = *target.profile;
    local_id(profile.profile_id); topology_id(profile.ring_id);
    switch (profile.measurement) {
    case AssemblyProfileDimensionMeasurement::edge_length:
        require(profile.segment_id && !profile.start_vertex_id && !profile.end_vertex_id,
            "edge selector requires only a segment identity");
        topology_id(*profile.segment_id);
        break;
    case AssemblyProfileDimensionMeasurement::vertex_distance:
        require(!profile.segment_id && profile.start_vertex_id && profile.end_vertex_id,
            "vertex selector requires only two vertex identities");
        topology_id(*profile.start_vertex_id); topology_id(*profile.end_vertex_id);
        require(*profile.start_vertex_id != *profile.end_vertex_id, "vertex identities must be distinct");
        break;
    default: require(false, "unknown profile measurement");
    }
}
void fields(const Json& value, std::initializer_list<const char*> names) {
    require(value.is_object() && value.size() == names.size(), "target requires closed fields");
    for (const auto* name : names) require(value.contains(name), "target field is missing");
}
const std::string& string(const Json& value) {
    require(value.is_string(), "target requires text identity");
    return value.get_ref<const std::string&>();
}

// Same conservative complete-map ceilings as catalog phase edit. Walk borrowed
// raw data before copying or decoding; catalogs additionally reserve their
// nonlinear geometry/graph validation through the existing transfer ledger.
struct RawBudget {
    std::size_t bytes{}, nodes{};
    void reserve(std::size_t count) {
        require(count <= 4 * 1024 * 1024 - nodes && count <= (64 * 1024 * 1024 - bytes) / 32,
            "raw complexity budget exceeded");
        nodes += count; bytes += 32 * count;
    }
    void text(std::string_view value) {
        require(value.size() <= (64 * 1024 * 1024 - bytes) / 6, "raw byte budget exceeded");
        bytes += 6 * value.size();
        utf8(value);
    }
    void read(const Json& value, std::size_t depth = 0) {
        require(depth <= 64, "raw depth budget exceeded"); reserve(1);
        require(!value.is_binary() && !value.is_discarded() &&
            (!value.is_number_float() || std::isfinite(value.get<double>())), "nonportable raw JSON");
        if (value.is_string()) text(value.get_ref<const std::string&>());
        else if (value.is_array()) for (const auto& child : value) read(child, depth + 1);
        else if (value.is_object()) for (const auto& [key, child] : value.items()) {
            text(key); read(child, depth + 1);
        }
    }
    void map(const AssemblyDocumentEntities& actual) {
        require(actual.size() <= 100000, "actual entity count exceeds bound");
        for (const auto& [id, entity] : actual) {
            document_id(id);
            require(entity.id == id && entity.properties.is_object() && entity.extensions.is_object(),
                "actual map identity or envelope is inconsistent");
            text(id); text(entity.type); read(entity.properties); read(entity.extensions);
        }
    }
};
struct MeasurementBudget {
    std::size_t work{};
    RawBudget output;
    void charge(std::size_t count) {
        require(count <= 64 * 1024 * 1024 - work, "batched analytical work budget exceeded");
        work += count;
    }
    void path(const Path& value) {
        output.reserve(1 + value.size());
        for (const auto& id : value) output.text(id);
    }
    void target(const AssemblyDimensionTarget& value) {
        output.reserve(16); path(value.component.part_path);
        output.text(value.component.catalog_id); output.text(value.component.instance_id);
        if (value.component.document_entity_id) output.text(*value.component.document_entity_id);
        if (value.profile) {
            output.text(value.profile->profile_id); output.text(value.profile->ring_id);
            if (value.profile->segment_id) output.text(*value.profile->segment_id);
            if (value.profile->start_vertex_id) output.text(*value.profile->start_vertex_id);
            if (value.profile->end_vertex_id) output.text(*value.profile->end_vertex_id);
        }
    }
    void profile_geometry(const AssemblyProfile& value) {
        output.reserve(16); output.text(value.id);
        if (value.material_slot) output.text(*value.material_slot);
        const auto boundary = [&](const Boundary& ring) { output.reserve(1 + 8 * ring.size()); };
        boundary(value.outer);
        output.reserve(value.holes.size());
        for (const auto& hole : value.holes) boundary(hole);
        if (!value.topology) return;
        const auto topology = [&](const AssemblyProfileRingIdentity& ring) {
            output.reserve(4 + ring.segment_ids.size() + ring.vertex_ids.size());
            output.text(ring.ring_id);
            for (const auto& id : ring.segment_ids) output.text(id);
            for (const auto& id : ring.vertex_ids) output.text(id);
        };
        output.reserve(2 + value.topology->holes.size());
        topology(value.topology->outer);
        for (const auto& hole : value.topology->holes) topology(hole);
    }
    template<class T> void overrides(const T& value) {
        output.reserve(3);
        for (const auto* strings : {&value.property_overrides, &value.material_overrides})
            for (const auto& [key, text] : *strings) { output.reserve(2); output.text(key); output.text(text); }
        for (const auto& [key, quantity] : value.quantity_overrides) {
            (void)quantity; output.reserve(4); output.text(key);
        }
    }
    void node(const AssemblyExpandedNode& value) {
        output.reserve(32); path(value.part_path); output.text(value.type_id);
        for (const auto* strings : {&value.properties, &value.materials})
            for (const auto& [key, text] : *strings) { output.reserve(2); output.text(key); output.text(text); }
        for (const auto& [key, quantity] : value.quantities) {
            (void)quantity; output.reserve(4); output.text(key);
        }
        if (value.source_part) {
            output.reserve(16); output.text(value.source_part->id); output.text(value.source_part->type_id);
            overrides(*value.source_part);
        }
        if (value.path_override) {
            output.reserve(16); path(value.path_override->part_path); overrides(*value.path_override);
            // Selected-node provenance retains actual path profile overrides.
            // Admit their geometry/topology copies before assigning the node.
            if (value.path_override->profile_overrides) {
                output.reserve(1 + value.path_override->profile_overrides->size());
                for (const auto& profile : *value.path_override->profile_overrides) profile_geometry(profile);
            }
        }
    }
    void profile(const Component& target, const AssemblyExpandedProfile& value) {
        output.reserve(8); output.text(target.catalog_id); output.text(target.instance_id);
        if (target.document_entity_id) output.text(*target.document_entity_id);
        path(value.part_path); output.text(value.type_id); output.text(value.profile.id);
    }
};
Component root(Component target) { target.part_path.clear(); return target; }
bool descendant(const Path& path, const Path& selected) {
    return path.size() >= selected.size() && std::equal(selected.begin(), selected.end(), path.begin());
}
double finite(double value) {
    require(std::isfinite(value), "measurement arithmetic overflow");
    return value;
}
AssemblyPoint3 finite(AssemblyPoint3 value) {
    (void)finite(value.x); (void)finite(value.y); (void)finite(value.z); return value;
}
double z_scale(const AssemblyTransform& pose) {
    const auto result = finite(pose.scale * pose.vertical_scale);
    require(result > 0, "vertical scale underflow"); return result;
}
void validate_placement_host(const AssemblyDocumentEntities& actual, const AssemblyInstance& instance) {
    if (!instance.placement) return;
    const auto& id = instance.placement->host_entity_id;
    const auto host = actual.find(id);
    require(host != actual.end() && host->second.id == id &&
        is_complete_assembly_catalog_host_type(host->second.type), "actual placement host is missing or invalid");
}
using Expansions = std::map<Component, AssemblyExpansion>;
Expansions admitted_expansions(const AssemblyDocumentEntities& actual,
    std::span<const AssemblyDimensionTarget> targets) {
    RawBudget raw; raw.map(actual);
    AssemblyCatalogTransferBudget work;
    for (const auto& [id, entity] : actual) {
        (void)id;
        if (entity.type == "assembly_model") admit_existing_assembly_catalog_work(entity, work);
    }
    // The preceding loop admits every actual catalog before the first decode,
    // including unreachable definitions and unrelated catalog owners.
    std::map<std::string, AssemblyModel, std::less<>> models;
    for (const auto& [id, entity] : actual) if (entity.type == "assembly_model")
        models.emplace(id, AssemblyModel::from_json(entity.properties.at("model")));
    Expansions result;
    for (const auto& target : targets) result.try_emplace(root(target.component));
    AssemblyExpansionBudget budget;
    std::map<Component, bool> found;
    const auto expand = [&](Component identity, const AssemblyModel& model, const AssemblyInstance& instance) {
        validate_placement_host(actual, instance);
        auto expansion = model.expand(instance, budget);
        const auto wanted = result.find(identity);
        if (wanted != result.end()) {
            require(found.emplace(identity, true).second, "actual root identity is duplicated");
            wanted->second = std::move(expansion);
        }
    };
    for (const auto& [id, model] : models) for (const auto& instance : model.instances())
        expand({id, instance.id, std::nullopt, {}}, model, instance);
    for (const auto& [id, entity] : actual) if (entity.type == "assembly_instance") {
        const auto value = decode_document_assembly_instance(entity);
        const auto model = models.find(value.assembly_catalog_id);
        require(model != models.end(), "independent root references a missing catalog");
        expand({value.assembly_catalog_id, value.instance.id, id, {}}, model->second, value.instance);
    }
    for (const auto& [identity, expansion] : result) {
        (void)expansion;
        require(found.contains(identity), "qualified actual root does not exist");
    }
    return result;
}

struct ProfileFrame {
    AssemblyTransform pose;
    Vec2 origin;
    double xx{}, xy{}, yx{}, yy{}, zz{};
};
ProfileFrame profile_frame(const AssemblyExpandedProfile& profile, const AssemblyTransform& relative) {
    require(!profile.profile.outer.empty(), "actual profile has no outer geometry");
    const auto [c, s] = assembly_rotation_components(relative.rotation_radians);
    const auto sign = relative.mirrored_y ? -1.0 : 1.0;
    return {relative, profile.profile.outer.front().start, c * relative.scale, -s * sign * relative.scale,
        s * relative.scale, c * sign * relative.scale, z_scale(relative)};
}
// Work around each real profile vertex before rotation/reflection/scaling.
// Offsets between profile origins use translation differences and differences
// of coefficients, avoiding subtraction of large absolute world coordinates.
Bounds2 profile_bounds(const AssemblyProfile& profile, const ProfileFrame& frame) {
    Boundary centered; centered.reserve(profile.outer.size());
    const auto scale_point = [&](Vec2 p) {
        return Vec2{finite((p.x - frame.origin.x) * frame.pose.scale),
                    finite((p.y - frame.origin.y) * frame.pose.scale)};
    };
    // PlanarTransform reflects AFTER rotating; the assembly contract reflects
    // BEFORE rotating. R(theta)*F == F*R(-theta) for Y reflection.
    PlanarTransform transform;
    transform.rotation_radians = frame.pose.mirrored_y ? -frame.pose.rotation_radians : frame.pose.rotation_radians;
    transform.flip_vertical = frame.pose.mirrored_y;
    for (const auto& edge : profile.outer)
        centered.push_back(transform_segment({scale_point(edge.start), scale_point(edge.end), edge.sweep_radians}, transform));
    return boundary_bounds(centered);
}
AssemblyPoint3 profile_origin_difference(const AssemblyProfile& profile, const ProfileFrame& frame,
    const AssemblyProfile& reference, const ProfileFrame& anchor) {
    const auto dx = frame.origin.x - anchor.origin.x, dy = frame.origin.y - anchor.origin.y;
    return finite(AssemblyPoint3{
        (frame.pose.translation_m.x - anchor.pose.translation_m.x) +
            (frame.xx - anchor.xx) * anchor.origin.x + (frame.xy - anchor.xy) * anchor.origin.y + frame.xx * dx + frame.xy * dy,
        (frame.pose.translation_m.y - anchor.pose.translation_m.y) +
            (frame.yx - anchor.yx) * anchor.origin.x + (frame.yy - anchor.yy) * anchor.origin.y + frame.yx * dx + frame.yy * dy,
        (frame.pose.translation_m.z - anchor.pose.translation_m.z) +
            (frame.zz - anchor.zz) * reference.elevation_m + frame.zz * (profile.elevation_m - reference.elevation_m)});
}
AssemblyPoint3 add_world_offset(AssemblyPoint3 origin, AssemblyPoint3 offset,
    AssemblyPoint3 width, AssemblyPoint3 depth) {
    return finite(AssemblyPoint3{origin.x + width.x * offset.x + depth.x * offset.y,
        origin.y + width.y * offset.x + depth.y * offset.y, origin.z + offset.z});
}
void measure_own_profile(AssemblyDimensionResolution& result, const AssemblyExpansion& expansion,
    MeasurementBudget& budget, AssemblyPoint3 local_width, AssemblyPoint3 local_depth) {
    const auto& target = result.target;
    const auto& selector = *target.profile;
    const auto expanded = std::find_if(expansion.profiles.begin(), expansion.profiles.end(), [&](const auto& profile) {
        return profile.part_path == target.component.part_path && profile.profile.id == selector.profile_id;
    });
    require(expanded != expansion.profiles.end(), "exact selected node's own profile does not exist");
    require(expanded->transform == result.selected_node.transform, "own profile frame is inconsistent");
    const auto& profile = expanded->profile;
    require(profile.topology.has_value(), "selected profile has no retained topology identities");
    const auto& topology = *profile.topology;
    require(topology.holes.size() == profile.holes.size(), "actual topology hole count is inconsistent");
    budget.charge(topology.holes.size());
    const AssemblyProfileRingIdentity* identity = nullptr;
    const Boundary* boundary = nullptr;
    if (topology.outer.ring_id == selector.ring_id) {
        identity = &topology.outer; boundary = &profile.outer;
    } else {
        const auto hole = std::find_if(topology.holes.begin(), topology.holes.end(), [&](const auto& ring) {
            return ring.ring_id == selector.ring_id;
        });
        require(hole != topology.holes.end(), "selected profile ring identity does not exist");
        identity = &*hole;
        boundary = &profile.holes[static_cast<std::size_t>(hole - topology.holes.begin())];
    }
    require(!boundary->empty() && identity->segment_ids.size() == boundary->size() &&
        identity->vertex_ids.size() == boundary->size(), "actual topology ring counts are inconsistent");
    budget.charge(2 * boundary->size());
    AssemblyProfileDimensionGeometry geometry;
    geometry.elevation_profile_m = profile.elevation_m;
    if (selector.measurement == AssemblyProfileDimensionMeasurement::edge_length) {
        const auto segment = std::find(identity->segment_ids.begin(), identity->segment_ids.end(), *selector.segment_id);
        require(segment != identity->segment_ids.end(), "selected segment identity does not exist");
        const auto& source = (*boundary)[static_cast<std::size_t>(segment - identity->segment_ids.begin())];
        geometry.segment_profile_m = source;
        geometry.start_profile_m = source.start; geometry.end_profile_m = source.end;
        // Measure the actual analytical source edge, then scale once. Its chord
        // is retained separately for presentation, never substituted for length.
        result.span_metres = finite(segment_length(source) * result.selected_node.transform.scale);
    } else {
        const auto vertex = [&](const std::string& id) {
            const auto found = std::find(identity->vertex_ids.begin(), identity->vertex_ids.end(), id);
            require(found != identity->vertex_ids.end(), "selected vertex identity does not exist");
            return (*boundary)[static_cast<std::size_t>(found - identity->vertex_ids.begin())].start;
        };
        geometry.start_profile_m = vertex(*selector.start_vertex_id);
        geometry.end_profile_m = vertex(*selector.end_vertex_id);
    }
    const auto scale = result.selected_node.transform.scale;
    const auto start = geometry.start_profile_m, end = geometry.end_profile_m;
    const auto offset = finite(AssemblyPoint3{
        (end.x - start.x) * scale, (end.y - start.y) * scale, 0});
    if (geometry.segment_profile_m) {
        // The admitted ring's signed area integrates every analytical edge.
        // Charge both linear ring passes before doing that work.
        budget.charge(boundary->size()); budget.charge(boundary->size());
        const auto area = finite(signed_area(*boundary));
        require(area != 0.0, "selected profile ring has no representable orientation");
        const auto dx = finite(end.x - start.x), dy = finite(end.y - start.y);
        const auto chord = finite(std::hypot(dx, dy));
        require(chord > 0.0, "selected profile edge has no representable chord");
        const Vec2 left{finite(-dy / chord), finite(dx / chord)};
        const auto sweep = finite(geometry.segment_profile_m->sweep_radians);
        require(std::abs(sweep) < 2.0 * std::numbers::pi,
            "selected profile edge sweep exceeds numeric bounds");
        // At the analytical arc midpoint, the directed tangent is parallel to
        // its chord. Its sagitta avoids subtracting absolute source positions.
        const auto sagitta = finite((chord / 2.0) * finite(std::tan(sweep / 4.0)));
        AssemblyProfileDimensionEdgePlacement placement;
        placement.midpoint_from_anchor_m = {
            finite(finite(dx / 2.0 - left.x * sagitta) * scale),
            finite(finite(dy / 2.0 - left.y * sagitta) * scale)};
        const bool hole = boundary != &profile.outer;
        const auto outward_sign = (area > 0.0) != hole ? -1.0 : 1.0;
        placement.outward_unit_profile = {
            finite(left.x * outward_sign), finite(left.y * outward_sign)};
        budget.output.reserve(8);
        geometry.edge_placement = placement;
    }
    if (selector.measurement == AssemblyProfileDimensionMeasurement::vertex_distance)
        result.span_metres = finite(std::hypot(offset.x, offset.y));
    require(result.span_metres > 0, "selected profile dimension is unrepresentable");
    const auto anchor = finite(AssemblyPoint3{start.x * scale, start.y * scale,
        profile.elevation_m * z_scale(result.selected_node.transform)});
    result.anchor_world_m = add_world_offset(result.selected_world_frame.translation_m, anchor,
        result.width_direction_world, result.depth_direction_world);
    result.anchor_authored_m = add_world_offset(result.selected_node.transform.translation_m, anchor,
        local_width, local_depth);
    result.start_from_anchor_m = {}; result.end_from_anchor_m = offset;
    geometry.segment_from_anchor_m = {{}, {offset.x, offset.y},
        geometry.segment_profile_m ? geometry.segment_profile_m->sweep_radians : 0.0};
    const auto bounds = segment_bounds(geometry.segment_from_anchor_m);
    result.minimum_from_anchor_m = {bounds.minimum.x, bounds.minimum.y, 0};
    result.maximum_from_anchor_m = {bounds.maximum.x, bounds.maximum.y, 0};
    result.start_world_m = result.anchor_world_m;
    result.end_world_m = add_world_offset(result.anchor_world_m, offset,
        result.width_direction_world, result.depth_direction_world);
    result.start_authored_m = result.anchor_authored_m;
    result.end_authored_m = add_world_offset(result.anchor_authored_m, offset, local_width, local_depth);
    budget.profile(target.component, *expanded); budget.output.reserve(32);
    result.profiles.push_back({target.component.catalog_id, target.component.instance_id,
        target.component.document_entity_id, expanded->part_path, expanded->type_id, profile.id});
    result.profile_geometry = geometry;
}
AssemblyDimensionResolution measure(const AssemblyDimensionTarget& target,
    const AssemblyExpansion& expansion, MeasurementBudget& budget,
    const std::map<std::string, SitePresentationPlacement, std::less<>>& presentations) {
    budget.charge(expansion.nodes.size() + expansion.profiles.size());
    const auto selected = std::find_if(expansion.nodes.begin(), expansion.nodes.end(), [&](const auto& node) {
        return node.part_path == target.component.part_path;
    });
    require(selected != expansion.nodes.end(), "exact selected part path does not exist");
    budget.node(*selected);
    budget.output.reserve(32);
    AssemblyDimensionResolution result;
    result.target = target; result.selected_node = *selected; result.selected_world_frame = selected->transform;
    if (expansion.source_instance.placement) {
        budget.output.text(expansion.source_instance.placement->host_entity_id);
        result.placement_host_id=expansion.source_instance.placement->host_entity_id;
    }
    if (target.component.document_entity_id) {
        const auto& placement = presentations.at(*target.component.document_entity_id);
        budget.output.reserve(placement.dependency_ids.size() + 32);
        for (const auto& id : placement.dependency_ids) budget.output.text(id);
        result.presentation = placement;
        const auto& pose = result.presentation->forward;
        result.selected_world_frame = compose_assembly_transform(
            {{pose.translation_m.x, pose.translation_m.y, pose.translation_m.z}, pose.rotation_radians, 1.0}, selected->transform);
    }
    const auto [c, s] = assembly_rotation_components(result.selected_world_frame.rotation_radians);
    const auto sign = result.selected_world_frame.mirrored_y ? -1.0 : 1.0;
    result.width_direction_world = {c, s, 0}; result.depth_direction_world = {-s * sign, c * sign, 0};
    result.height_direction_world = {0, 0, 1};
    const auto [local_c, local_s] = assembly_rotation_components(selected->transform.rotation_radians);
    const auto local_sign = selected->transform.mirrored_y ? -1.0 : 1.0;
    const AssemblyPoint3 local_width{local_c, local_s, 0};
    const AssemblyPoint3 local_depth{-local_s * local_sign, local_c * local_sign, 0};

    if (target.profile) {
        measure_own_profile(result, expansion, budget, local_width, local_depth);
        return result;
    }

    // Reconstruct descendant poses from admitted source/path transforms rather
    // than subtracting rounded world translations from the selected node.
    std::map<Path, AssemblyTransform> relative;
    relative.emplace(selected->part_path, AssemblyTransform{{}, 0, selected->transform.scale, false,
        selected->transform.vertical_scale});
    for (const auto& node : expansion.nodes) {
        if (!descendant(node.part_path, selected->part_path) || node.part_path == selected->part_path) continue;
        require(node.source_part.has_value(), "descendant node lacks actual part provenance");
        auto parent = node.part_path; parent.pop_back();
        const auto pose = node.path_override && node.path_override->transform ?
            *node.path_override->transform : node.source_part->transform;
        const auto found = relative.find(parent);
        require(found != relative.end(), "actual expanded parent order is inconsistent");
        relative.emplace(node.part_path, compose_assembly_transform(found->second, pose));
    }
    const AssemblyProfile* reference = nullptr;
    ProfileFrame anchor;
    for (const auto& expanded : expansion.profiles) {
        if (!descendant(expanded.part_path, selected->part_path)) continue;
        budget.charge(expanded.profile.outer.size());
        budget.profile(target.component, expanded);
        const auto frame = profile_frame(expanded, relative.at(expanded.part_path));
        const auto bounds = profile_bounds(expanded.profile, frame);
        if (!reference) {
            reference = &expanded.profile; anchor = frame;
            const auto origin = finite(AssemblyPoint3{
                frame.pose.translation_m.x + frame.xx * frame.origin.x + frame.xy * frame.origin.y,
                frame.pose.translation_m.y + frame.yx * frame.origin.x + frame.yy * frame.origin.y,
                frame.pose.translation_m.z + frame.zz * reference->elevation_m});
            result.anchor_world_m = add_world_offset(result.selected_world_frame.translation_m, origin,
                result.width_direction_world, result.depth_direction_world);
            result.anchor_authored_m = add_world_offset(selected->transform.translation_m, origin,
                local_width, local_depth);
        }
        const auto origin = profile_origin_difference(expanded.profile, frame, *reference, anchor);
        const auto height = finite(expanded.profile.height_m * frame.zz);
        require(height > 0, "actual extrusion height underflow");
        const auto low = finite(AssemblyPoint3{origin.x + bounds.minimum.x, origin.y + bounds.minimum.y, origin.z});
        const auto high = finite(AssemblyPoint3{origin.x + bounds.maximum.x, origin.y + bounds.maximum.y, origin.z + height});
        if (result.profiles.empty()) { result.minimum_from_anchor_m = low; result.maximum_from_anchor_m = high; }
        else {
            auto& min = result.minimum_from_anchor_m; auto& max = result.maximum_from_anchor_m;
            min = {std::min(min.x, low.x), std::min(min.y, low.y), std::min(min.z, low.z)};
            max = {std::max(max.x, high.x), std::max(max.y, high.y), std::max(max.z, high.z)};
        }
        result.profiles.push_back({target.component.catalog_id, target.component.instance_id,
            target.component.document_entity_id, expanded.part_path, expanded.type_id, expanded.profile.id});
    }
    require(reference, "selected component has no analytical profile geometry; native host-copy dimensions are unsupported");
    const auto& low = result.minimum_from_anchor_m; const auto& high = result.maximum_from_anchor_m;
    result.start_from_anchor_m = low; result.end_from_anchor_m = low;
    switch (target.axis) {
    case AssemblyDimensionAxis::width: result.end_from_anchor_m.x = high.x; result.span_metres = finite(high.x - low.x); break;
    case AssemblyDimensionAxis::depth: result.end_from_anchor_m.y = high.y; result.span_metres = finite(high.y - low.y); break;
    case AssemblyDimensionAxis::height: result.end_from_anchor_m.z = high.z; result.span_metres = finite(high.z - low.z); break;
    }
    require(result.span_metres > 0, "selected component dimension is unrepresentable");
    result.start_world_m = add_world_offset(result.anchor_world_m, result.start_from_anchor_m,
        result.width_direction_world, result.depth_direction_world);
    result.end_world_m = add_world_offset(result.anchor_world_m, result.end_from_anchor_m,
        result.width_direction_world, result.depth_direction_world);
    result.start_authored_m = add_world_offset(result.anchor_authored_m, result.start_from_anchor_m,
        local_width, local_depth);
    result.end_authored_m = add_world_offset(result.anchor_authored_m, result.end_from_anchor_m,
        local_width, local_depth);
    return result;
}
} // namespace

std::string_view assembly_dimension_axis_name(AssemblyDimensionAxis axis) {
    switch (axis) {
    case AssemblyDimensionAxis::width: return "width";
    case AssemblyDimensionAxis::depth: return "depth";
    case AssemblyDimensionAxis::height: return "height";
    }
    throw std::invalid_argument("Assembly dimension: unknown axis");
}
std::string_view assembly_profile_dimension_measurement_name(AssemblyProfileDimensionMeasurement measurement) {
    switch (measurement) {
    case AssemblyProfileDimensionMeasurement::edge_length: return "edge_length";
    case AssemblyProfileDimensionMeasurement::vertex_distance: return "vertex_distance";
    }
    throw std::invalid_argument("Assembly dimension: unknown profile measurement");
}
Json encode_assembly_dimension_target(const AssemblyDimensionTarget& target) {
    validate_target(target);
    const auto& component = target.component;
    if (target.profile) {
        const auto& profile = *target.profile;
        Json value{{"version", 2}, {"form", "assembly_profile_dimension_target"},
            {"catalog_id", component.catalog_id}, {"instance_id", component.instance_id},
            {"document_entity_id", component.document_entity_id ? Json(*component.document_entity_id) : Json(nullptr)},
            {"part_path", component.part_path}, {"profile_id", profile.profile_id}, {"ring_id", profile.ring_id},
            {"measurement", assembly_profile_dimension_measurement_name(profile.measurement)}};
        if (profile.measurement == AssemblyProfileDimensionMeasurement::edge_length)
            value["segment_id"] = *profile.segment_id;
        else {
            value["start_vertex_id"] = *profile.start_vertex_id;
            value["end_vertex_id"] = *profile.end_vertex_id;
        }
        return value;
    }
    return {{"version", 1}, {"form", "assembly_dimension_target"}, {"catalog_id", component.catalog_id},
        {"instance_id", component.instance_id}, {"document_entity_id", component.document_entity_id ?
            Json(*component.document_entity_id) : Json(nullptr)}, {"part_path", component.part_path},
        {"axis", assembly_dimension_axis_name(target.axis)}};
}
AssemblyDimensionTarget decode_assembly_dimension_target(const Json& value) {
    require(value.is_object() && value.contains("version") && value.contains("form"), "target schema is missing");
    const bool profile_wire = value.at("form") == "assembly_profile_dimension_target";
    bool edge_wire = false;
    if (profile_wire) {
        require(value.contains("measurement"), "profile measurement is missing");
        edge_wire = value.at("measurement") == "edge_length";
        require(edge_wire || value.at("measurement") == "vertex_distance", "unknown profile measurement");
        if (edge_wire)
            fields(value, {"version", "form", "catalog_id", "instance_id", "document_entity_id", "part_path",
                "profile_id", "ring_id", "measurement", "segment_id"});
        else
            fields(value, {"version", "form", "catalog_id", "instance_id", "document_entity_id", "part_path",
                "profile_id", "ring_id", "measurement", "start_vertex_id", "end_vertex_id"});
        require(value.at("version").is_number_integer() && value.at("version") == 2, "unknown profile target schema");
        local_id(string(value.at("profile_id"))); topology_id(string(value.at("ring_id")));
        if (edge_wire) topology_id(string(value.at("segment_id")));
        else {
            topology_id(string(value.at("start_vertex_id"))); topology_id(string(value.at("end_vertex_id")));
            require(value.at("start_vertex_id") != value.at("end_vertex_id"), "vertex identities must be distinct");
        }
    } else {
        fields(value, {"version", "form", "catalog_id", "instance_id", "document_entity_id", "part_path", "axis"});
        require(value.at("version").is_number_integer() && value.at("version") == 1 &&
            value.at("form") == "assembly_dimension_target", "unknown target schema");
    }
    // Bound borrowed typed fields before allocating their copies.
    document_id(string(value.at("catalog_id"))); local_id(string(value.at("instance_id")));
    if (!value.at("document_entity_id").is_null()) document_id(string(value.at("document_entity_id")));
    const auto& path = value.at("part_path");
    require(path.is_array() && path.size() < 32, "invalid target part path");
    for (const auto& part : path) local_id(string(part));
    AssemblyDimensionTarget target;
    if (profile_wire) {
        AssemblyProfileDimensionTarget profile;
        profile.profile_id = string(value.at("profile_id")); profile.ring_id = string(value.at("ring_id"));
        if (edge_wire) profile.segment_id = string(value.at("segment_id"));
        else {
            profile.measurement = AssemblyProfileDimensionMeasurement::vertex_distance;
            profile.start_vertex_id = string(value.at("start_vertex_id"));
            profile.end_vertex_id = string(value.at("end_vertex_id"));
        }
        target.profile = std::move(profile);
    } else {
        const auto& axis = value.at("axis");
        if (axis == "width") target.axis = AssemblyDimensionAxis::width;
        else if (axis == "depth") target.axis = AssemblyDimensionAxis::depth;
        else if (axis == "height") target.axis = AssemblyDimensionAxis::height;
        else require(false, "unknown target axis");
    }
    target.component = {string(value.at("catalog_id")), string(value.at("instance_id")), std::nullopt,
        path.get<Path>()};
    if (!value.at("document_entity_id").is_null()) target.component.document_entity_id = string(value.at("document_entity_id"));
    return target;
}
std::vector<AssemblyDimensionResolution> resolve_assembly_dimensions(const AssemblyDocumentEntities& actual,
    std::span<const AssemblyDimensionTarget> targets) {
    require(!targets.empty() && targets.size() <= 1000, "target count exceeds bound");
    for (const auto& target : targets) validate_target(target);
    // Admit the complete borrowed selector batch before root-map/path or
    // resolution target copies, sharing the retained output ledger below.
    MeasurementBudget budget;
    for (const auto& target : targets) budget.target(target);
    const auto expansions = admitted_expansions(actual, targets);
    std::vector<std::string> owners;
    for (const auto& [identity, expansion] : expansions) {
        (void)expansion;
        if (identity.document_entity_id) owners.push_back(*identity.document_entity_id);
    }
    std::sort(owners.begin(), owners.end()); owners.erase(std::unique(owners.begin(), owners.end()), owners.end());
    if (!owners.empty()) (void)resolve_vertical_placements(actual, owners);
    const auto presentations = owners.empty() ? std::map<std::string, SitePresentationPlacement, std::less<>>{} :
        resolve_site_presentations(actual, owners);
    std::vector<AssemblyDimensionResolution> result; result.reserve(targets.size());
    for (const auto& target : targets)
        result.push_back(measure(target, expansions.at(root(target.component)), budget, presentations));
    return result;
}
AssemblyDimensionResolution resolve_assembly_dimension(const AssemblyDocumentEntities& actual,
    const AssemblyDimensionTarget& target) {
    return resolve_assembly_dimensions(actual, std::span<const AssemblyDimensionTarget>(&target, 1)).front();
}

} // namespace sketch
