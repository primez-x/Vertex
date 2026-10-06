#pragma once

#include "sketch/door_operation.hpp"
#include "sketch/opening_assembly.hpp"
#include "sketch/wall_semantics.hpp"

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

} // namespace sketch
