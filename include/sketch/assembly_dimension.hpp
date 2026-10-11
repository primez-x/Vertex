#pragma once

#include "sketch/assembly_instance_property_edit.hpp"
#include "sketch/site_frame.hpp"

#include <span>
#include <string_view>

namespace sketch {

enum class AssemblyDimensionAxis { width, depth, height };

enum class AssemblyProfileDimensionMeasurement { edge_length, vertex_distance };

struct AssemblyProfileDimensionTarget {
    std::string profile_id;
    std::string ring_id;
    AssemblyProfileDimensionMeasurement measurement{AssemblyProfileDimensionMeasurement::edge_length};
    // Closed selector: edge_length requires only segment_id; vertex_distance
    // requires two distinct vertex IDs and no segment ID. These are actual
    // retained identities, never boundary indices or presentation aliases.
    std::optional<std::string> segment_id;
    std::optional<std::string> start_vertex_id, end_vertex_id;
    bool operator==(const AssemblyProfileDimensionTarget&) const = default;
};

struct AssemblyDimensionTarget {
    // Actual catalog/root/path identities, never presentation aliases.
    AssemblyInstancePropertyEditTarget component;
    AssemblyDimensionAxis axis{AssemblyDimensionAxis::width};
    // Appended for aggregate compatibility. Profile measurement is independent
    // of extent axes; the compatibility axis must remain its default width.
    std::optional<AssemblyProfileDimensionTarget> profile;
    bool operator==(const AssemblyDimensionTarget&) const = default;
};

[[nodiscard]] std::string_view assembly_dimension_axis_name(AssemblyDimensionAxis);
[[nodiscard]] std::string_view assembly_profile_dimension_measurement_name(AssemblyProfileDimensionMeasurement);
// Closed v1: version, form, catalog_id, instance_id, document_entity_id
// (explicit null for embedded), part_path (explicit array), axis. Document IDs
// <=128 bytes, local IDs <=4096 bytes, path length <32. Unknown fields/schema
// refuse. This carries measurement identity only, never mutation authority.
// Distinct closed v2 form assembly_profile_dimension_target retains common
// root/path fields, profile_id, ring_id, measurement and either segment_id or
// start_vertex_id/end_vertex_id. It has no axis. Topology IDs <=128 bytes;
// profile IDs follow the local-ID bound. V1 fields/meaning remain unchanged.
[[nodiscard]] nlohmann::json encode_assembly_dimension_target(const AssemblyDimensionTarget&);
[[nodiscard]] AssemblyDimensionTarget decode_assembly_dimension_target(const nlohmann::json&);

struct AssemblyProfileDimensionEdgePlacement {
    // Analytical edge midpoint in physical metres along the selected node's
    // unit width/depth axes, relative to the dimension's start anchor.
    Vec2 midpoint_from_anchor_m;
    // Unit normal pointing out of profile material, in source local XY.
    // Apply the actual width/depth basis, including its reflection, once.
    Vec2 outward_unit_profile;
};

struct AssemblyProfileDimensionGeometry {
    // Exact source points and elevation in the selected profile's local frame.
    Vec2 start_profile_m, end_profile_m;
    double elevation_profile_m{};
    // Present only for edge_length, including the original analytical sweep.
    std::optional<Segment> segment_profile_m;
    // Physical metres along the selected node's unit width/depth axes, relative
    // to resolution.anchor_world_m/anchor_authored_m. Source sweep is preserved;
    // reflecting these axes reverses its visible sweep. Vertex distance is a
    // straight chord. This avoids subtracting distant absolute endpoints.
    Segment segment_from_anchor_m;
    // Edge-only default placement; vertex distances retain their chord policy.
    std::optional<AssemblyProfileDimensionEdgePlacement> edge_placement;
};

struct AssemblyDimensionResolution {
    AssemblyDimensionTarget target;
    double span_metres{};
    // Extent targets project extrema onto a common dimension line at the other
    // two minima; these endpoints need not lie on the profile surface. Profile
    // targets retain the actual selected source endpoints at profile elevation.
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
    std::optional<AssemblyProfileDimensionGeometry> profile_geometry;
};

// Pure complete-actual-map resolution. Admits raw envelopes and all catalog
// work before model decoding, then expands every embedded/independent root
// under one shared document budget. Extent targets select the exact existing
// node and its actual descendant profiles. Bounds include circular-arc extrema
// in the selected node's rotated/reflected axes, never a world-aligned box.
// Height uses exact profile elevation/extrusion endpoints and positive Z scale.
// Metadata-only/legacy native host copies, missing paths, malformed owners and
// unrepresentable measurements throw std::invalid_argument. Inactive retained
// roots may resolve; this supplies no active-edit or publication authorization.
// Profile targets select only the exact node's own identified profile/ring.
// Edge span is analytical physical length (including arcs); vertex span is
// the source-vertex chord. Anonymous topology and missing/retired IDs refuse.
[[nodiscard]] AssemblyDimensionResolution resolve_assembly_dimension(
    const AssemblyDocumentEntities& actual, const AssemblyDimensionTarget&);
// 1..1000 targets, in input order, including duplicates. Complete map admission
// and expansion are shared once. Aggregate analytical work and retained output
// are bounded; no snapshot/candidate fabrication occurs.
[[nodiscard]] std::vector<AssemblyDimensionResolution> resolve_assembly_dimensions(
    const AssemblyDocumentEntities& actual, std::span<const AssemblyDimensionTarget>);

} // namespace sketch
