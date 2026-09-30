#include "sketch/ifc_native_geometry.hpp"
#include "sketch/architecture.hpp"
#include "sketch/hosted_opening_geometry.hpp"

#include <BRepMesh_IncrementalMesh.hxx>
#include <BRep_Tool.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <Poly_Triangulation.hxx>
#include <algorithm>
#include <cmath>
#include <map>
#include <stdexcept>

namespace sketch {
namespace {
void preflight(const Wall& wall, std::size_t vertices, std::size_t triangles) {
    validate_wall_semantics(wall);
    if (!vertices || !triangles) throw std::invalid_argument("ifc_mesh_budget_exceeded");
    const auto angle = std::abs(wall.baseline.sweep_radians);
    if (angle > 1e-7) {
        const auto radius = std::hypot(wall.baseline.end.x - wall.baseline.start.x,
                                      wall.baseline.end.y - wall.baseline.start.y) /
                            (2.0 * std::abs(std::sin(angle * 0.5))) + wall.thickness;
        // A conservative bound before OCCT allocates triangulation storage.
        const auto step = 2.0 * std::acos(std::clamp(1.0 -
            ifc_native_mesh_deviation_m * 0.5 / radius, -1.0, 1.0));
        if (!(step > 0) || std::ceil(angle / step) >
            static_cast<double>(std::min(vertices, triangles)) / 64.0)
            throw std::invalid_argument("ifc_mesh_budget_exceeded");
    }
}

std::vector<IfcNativeMesh> tessellate(const TopoDS_Shape& shape,
    std::size_t vertices, std::size_t triangles) {
    BRepMesh_IncrementalMesh mesher(shape, ifc_native_mesh_deviation_m * 0.5,
                                   false, 0.25, false);
    if (!mesher.IsDone()) throw std::invalid_argument("ifc_native_mesh_failed");
    std::vector<IfcNativeMesh> result;
    for (TopExp_Explorer solids(shape, TopAbs_SOLID); solids.More(); solids.Next()) {
        IfcNativeMesh output;
        std::map<std::array<double, 3>, std::size_t> welded;
        for (TopExp_Explorer faces(solids.Current(), TopAbs_FACE); faces.More(); faces.Next()) {
            const auto face = TopoDS::Face(faces.Current());
            TopLoc_Location location;
            const auto mesh = BRep_Tool::Triangulation(face, location);
            if (mesh.IsNull() || mesh->NbNodes() < 3 || mesh->NbTriangles() < 1)
                throw std::invalid_argument("ifc_native_mesh_failed");
            const auto node_count = static_cast<std::size_t>(mesh->NbNodes());
            const auto triangle_count = static_cast<std::size_t>(mesh->NbTriangles());
            if (node_count > vertices || triangle_count > triangles)
                throw std::invalid_argument("ifc_mesh_budget_exceeded");
            vertices -= node_count; triangles -= triangle_count;
            std::vector<std::size_t> nodes;
            for (int i = 1; i <= mesh->NbNodes(); ++i) {
                const auto point = mesh->Node(i).Transformed(location.Transformation());
                const std::array<double, 3> coordinate{point.X(), point.Y(), point.Z()};
                auto key = coordinate;
                for (auto& value : key) value = std::round(value * 1e9) / 1e9;
                const auto [entry, inserted] = welded.emplace(key, output.vertices.size());
                if (inserted) output.vertices.push_back(coordinate);
                nodes.push_back(entry->second);
            }
            for (int i = 1; i <= mesh->NbTriangles(); ++i) {
                int a, b, c; mesh->Triangle(i).Get(a, b, c);
                if (face.Orientation() == TopAbs_REVERSED) std::swap(b, c);
                const std::array<std::size_t, 3> triangle{nodes[a - 1], nodes[b - 1], nodes[c - 1]};
                if (triangle[0] == triangle[1] || triangle[0] == triangle[2] || triangle[1] == triangle[2])
                    throw std::invalid_argument("ifc_native_mesh_degenerate");
                output.triangles.push_back(triangle);
            }
        }
        if (output.vertices.empty()) throw std::invalid_argument("ifc_native_mesh_failed");
        std::map<std::pair<std::size_t, std::size_t>, std::pair<int, int>> edges;
        for (const auto& triangle : output.triangles) {
            for (std::size_t i = 0; i < 3; ++i) {
                const auto a = triangle[i], b = triangle[(i + 1) % 3];
                auto& edge = edges[{std::min(a, b), std::max(a, b)}];
                ++edge.first; edge.second += a < b ? 1 : -1;
            }
        }
        for (const auto& [key, edge] : edges)
            if (edge.first != 2 || edge.second != 0)
                throw std::invalid_argument("ifc_native_mesh_not_closed");
        result.push_back(std::move(output));
    }
    if (result.empty()) throw std::invalid_argument("ifc_native_mesh_failed");
    return result;
}
} // namespace

std::vector<IfcNativeMesh> ifc_native_wall_mesh(const Wall& wall,
    std::size_t vertices, std::size_t triangles) {
    preflight(wall, vertices, triangles);
    return tessellate(make_wall(wall), vertices, triangles);
}
std::vector<IfcNativeMesh> ifc_native_void_mesh(const Wall& wall, const HostedOpening& opening,
    std::size_t vertices, std::size_t triangles) {
    Wall checked = wall; checked.openings = {opening};
    preflight(checked, vertices, triangles);
    Wall cut = wall;
    cut.baseline = hosted_opening_span(wall.baseline, opening.offset, opening.width);
    cut.elevation = wall.elevation + opening.sill;
    cut.height = opening.height; cut.openings.clear(); cut.layers.clear(); cut.slope_rise.reset();
    return tessellate(make_wall(cut), vertices, triangles);
}
std::vector<IfcNativeMesh> ifc_native_fill_mesh(const Wall& wall, const HostedOpening& opening,
    const OpeningAssembly& assembly, const std::optional<DoorOperation>& operation,
    std::size_t vertices, std::size_t triangles) {
    preflight(wall, vertices, triangles);
    return tessellate(make_opening_assembly(wall, opening, assembly, operation), vertices, triangles);
}
} // namespace sketch
