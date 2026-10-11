#pragma once

#include "sketch/assembly_instance_property_edit.hpp"
#include "sketch/site_frame.hpp"

#include <span>
#include <string_view>

namespace sketch {

enum class AssemblyDimensionAxis { width, depth, height };

struct AssemblyDimensionTarget {
    // Actual catalog/root/path identities, never presentation aliases.
    AssemblyInstancePropertyEditTarget component;
    AssemblyDimensionAxis axis{AssemblyDimensionAxis::width};
    bool operator==(const AssemblyDimensionTarget&) const = default;
};

[[nodiscard]] std::string_view assembly_dimension_axis_name(AssemblyDimensionAxis);
// Closed v1: version, form, catalog_id, instance_id, document_entity_id
// (explicit null for embedded), part_path (explicit array), axis. Document IDs
// <=128 bytes, local IDs <=4096 bytes, path length <32. Unknown fields/schema
// refuse. This carries measurement identity only, never mutation authority.
[[nodiscard]] nlohmann::json encode_assembly_dimension_target(const AssemblyDimensionTarget&);
[[nodiscard]] AssemblyDimensionTarget decode_assembly_dimension_target(const nlohmann::json&);

struct AssemblyDimensionResolution {
    AssemblyDimensionTarget target;
    double span_metres{};
    // Extrema projected onto a common dimension line at the other two minima;
    // these presentation endpoints need not lie on the profile surface.
    AssemblyPoint3 start_world_m, end_world_m;
    // Authored plan coordinates before independent Site presentation. Canvas
    // and annotation pipelines apply that presentation once, alongside the
    // saved text position. Height projects to a point in plan, never an edge.
    AssemblyPoint3 anchor_authored_m, start_authored_m, end_authored_m;
    // Actual expanded selected node in authored coordinates, including retained
    // part/path overrides. World frame incorporates independent presentation
    // placement exactly once; its scale and vertical_scale remain authoritative.
    AssemblyExpandedNode selected_node;
    AssemblyTransform selected_world_frame;
    AssemblyPoint3 width_direction_world, depth_direction_world, height_direction_world;
    std::optional<SitePresentationPlacement> presentation;
    // Actual retained root placement dependency, shared by every nested target.
    std::optional<std::string> placement_host_id;
    std::vector<AssemblyProfilePresentationIdentity> profiles;
    // Geometry-centered physical-metre coordinates along the selected unit
    // axes. These offsets and span remain usable when distant world origins
    // cannot represent two separate endpoints in double precision. Renderers
    // should transform offsets after subtracting their view origin.
    AssemblyPoint3 anchor_world_m;
    AssemblyPoint3 minimum_from_anchor_m, maximum_from_anchor_m;
    AssemblyPoint3 start_from_anchor_m, end_from_anchor_m;
};

// Pure complete-actual-map resolution. Admits raw envelopes and all catalog
// work before model decoding, then expands every embedded/independent root
// under one shared document budget. Selects the exact existing node and its
// actual descendant profiles. Bounds include analytical circular-arc extrema
// in the selected node's rotated/reflected axes, never a world-aligned box.
// Height uses exact profile elevation/extrusion endpoints and positive Z scale.
// Metadata-only/legacy native host copies, missing paths, malformed owners and
// unrepresentable measurements throw std::invalid_argument. Inactive retained
// roots may resolve; this supplies no active-edit or publication authorization.
[[nodiscard]] AssemblyDimensionResolution resolve_assembly_dimension(
    const AssemblyDocumentEntities& actual, const AssemblyDimensionTarget&);
// 1..1000 targets, in input order, including duplicates. Complete map admission
// and expansion are shared once. Aggregate analytical work and retained output
// are bounded; no snapshot/candidate fabrication occurs.
[[nodiscard]] std::vector<AssemblyDimensionResolution> resolve_assembly_dimensions(
    const AssemblyDocumentEntities& actual, std::span<const AssemblyDimensionTarget>);

} // namespace sketch
