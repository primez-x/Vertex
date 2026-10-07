#pragma once

#include "sketch/door_operation.hpp"
#include "sketch/opening_assembly.hpp"
#include "sketch/wall_semantics.hpp"
#include "sketch/roof_join_semantics.hpp"

#include <array>
#include <cstddef>
#include <optional>
#include <vector>

namespace sketch {

struct Entity;

// OCCT types stay behind this optional bridge. All coordinates are world metres.
// Each mesh describes one native solid; triangle indices are zero based.
struct IfcNativeMesh {
    std::vector<std::array<double, 3>> vertices;
    std::vector<std::array<std::size_t, 3>> triangles;
};
// Authored-order, disjoint actual solid regions. Fully occluded members retain
// identity and quantities with no mesh. Coordinates remain in the join's local
// physical frame; the caller applies the shared site occurrence placement once.
struct IfcNativeJoinRegion {
    std::string source_id;
    double gross_volume_m3{};
    double net_volume_m3{};
    std::vector<IfcNativeMesh> meshes;
    std::optional<std::string> layer_id;
};
struct IfcNativeJoinMesh {
    double net_volume_m3{};
    std::vector<IfcNativeJoinRegion> regions;
};
[[nodiscard]] IfcNativeJoinMesh ifc_native_wall_join_mesh(
    const WallJoin& join, const std::vector<Wall>& walls,
    std::size_t vertex_budget, std::size_t triangle_budget);
[[nodiscard]] IfcNativeJoinMesh ifc_native_roof_join_mesh(
    const RoofJoin& join, const std::vector<Entity>& resolved_roofs,
    std::size_t vertex_budget, std::size_t triangle_budget);
inline constexpr double ifc_native_mesh_deviation_m = 0.001;

[[nodiscard]] std::vector<IfcNativeMesh> ifc_native_wall_mesh(
    const Wall& wall, std::size_t vertex_budget, std::size_t triangle_budget);
[[nodiscard]] std::vector<IfcNativeMesh> ifc_native_void_mesh(
    const Wall& wall, const HostedOpening& opening,
    std::size_t vertex_budget, std::size_t triangle_budget);
[[nodiscard]] std::vector<IfcNativeMesh> ifc_native_fill_mesh(
    const Wall& wall, const HostedOpening& opening, const OpeningAssembly& assembly,
    const std::optional<DoorOperation>& operation,
    std::size_t vertex_budget, std::size_t triangle_budget);

// Canonical authored roofs (panel/gable/hip, including through-openings) and
// architectural room volumes use the same validated solids as native views.
// Entity placement must already have been resolved exactly once by the caller.
[[nodiscard]] std::vector<IfcNativeMesh> ifc_native_roof_mesh(
    const Entity& roof, std::size_t vertex_budget, std::size_t triangle_budget);
[[nodiscard]] std::vector<IfcNativeMesh> ifc_native_room_mesh(
    const Entity& room, std::size_t vertex_budget, std::size_t triangle_budget);

// Canonical stair/railing solids, without independent placement resolution.
// Hosted rails require the current resolved stair, never a metadata substitute.
[[nodiscard]] std::vector<IfcNativeMesh> ifc_native_stair_mesh(
    const Entity& stair, std::size_t vertex_budget, std::size_t triangle_budget);
[[nodiscard]] std::vector<IfcNativeMesh> ifc_native_railing_mesh(
    const Entity& railing, const Entity* resolved_stair,
    std::size_t vertex_budget, std::size_t triangle_budget);

} // namespace sketch
