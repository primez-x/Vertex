#include "sketch/ifc_native_geometry.hpp"
#include "sketch/architecture.hpp"
#include "sketch/hosted_opening_geometry.hpp"
#include "sketch/building_entity.hpp"
#include "sketch/document_solid.hpp"
#include "sketch/building_objects.hpp"
#include "sketch/project_import_worker.hpp"

#include <BRepMesh_IncrementalMesh.hxx>
#include <BRepAlgoAPI_Common.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Iterator.hxx>
#include <TopoDS_Compound.hxx>
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

namespace {
void join_preflight(std::size_t members, std::size_t vertices, std::size_t triangles,
    std::size_t minimum_members = 2) {
    if (members < minimum_members || members > project_import_boundary_segment_limit ||
        members * (members - 1) / 2 > project_import_geometry_pair_limit ||
        members > std::min(vertices, triangles) / 64)
        throw std::invalid_argument("ifc_mesh_budget_exceeded");
}
IfcNativeJoinMesh join_mesh(const RoofJoinPartition& partition,
    std::size_t vertices, std::size_t triangles) {
    IfcNativeJoinMesh result{partition.fused_volume, {}};
    for (const auto& region : partition.regions) {
        IfcNativeJoinRegion output{region.source_roof_id, region.gross_volume, region.net_volume, {}};
        if (!region.shape.IsNull()) {
            output.meshes = tessellate(region.shape, vertices, triangles);
            for (const auto& mesh : output.meshes) {
                if (mesh.vertices.size() > vertices || mesh.triangles.size() > triangles)
                    throw std::invalid_argument("ifc_mesh_budget_exceeded");
                vertices -= mesh.vertices.size(); triangles -= mesh.triangles.size();
            }
        } else if (region.net_volume != 0) throw std::invalid_argument("ifc_native_join_region_invalid");
        result.regions.push_back(std::move(output));
    }
    return result;
}

RoofJoinPartition ordered_wall_regions(const WallJoin& join,
    const std::vector<TopoDS_Shape>& shapes, const TopoDS_Shape& fused) {
    RoofJoinPartition partition{fused, solid_volume(fused), {}};
    TopoDS_Shape earlier;
    double total = 0;
    for (std::size_t i = 0; i < shapes.size(); ++i) {
        auto region = shapes[i];
        const auto gross = solid_volume(region);
        double overlap = 0;
        if (!earlier.IsNull()) {
            BRepAlgoAPI_Common common(region, earlier); common.Build();
            if (!common.IsDone() || common.HasErrors()) throw std::invalid_argument("ifc_native_wall_join_partition_invalid");
            overlap = solid_volume(common.Shape());
            BRepAlgoAPI_Cut cut(region, earlier); cut.Build();
            if (!cut.IsDone() || cut.HasErrors()) throw std::invalid_argument("ifc_native_wall_join_partition_invalid");
            BRep_Builder builder; TopoDS_Compound solids; builder.MakeCompound(solids);
            std::size_t count = 0;
            for (TopExp_Explorer solid(cut.Shape(), TopAbs_SOLID); solid.More(); solid.Next()) {
                builder.Add(solids, solid.Current()); ++count;
            }
            region = count ? TopoDS_Shape{solids} : TopoDS_Shape{};
        }
        const auto net = solid_volume(region), tolerance = 1e-8 * std::max(1.0, partition.fused_volume);
        if (!std::isfinite(net) || net < 0 || std::abs(gross-overlap-net) > tolerance ||
            (!region.IsNull() && !BRepCheck_Analyzer(region).IsValid()))
            throw std::invalid_argument("ifc_native_wall_join_partition_invalid");
        partition.regions.push_back({join.wall_ids[i], region, gross, net}); total += net;
        if (earlier.IsNull()) earlier = shapes[i];
        else {
            BRepAlgoAPI_Fuse fuse(earlier, shapes[i]); fuse.Build();
            if (!fuse.IsDone() || fuse.HasErrors() || fuse.Shape().IsNull() || !BRepCheck_Analyzer(fuse.Shape()).IsValid())
                throw std::invalid_argument("ifc_native_wall_join_partition_invalid");
            earlier = fuse.Shape();
        }
    }
    if (std::abs(total-partition.fused_volume) > 1e-8 * std::max(1.0, partition.fused_volume))
        throw std::invalid_argument("ifc_native_wall_join_partition_invalid");
    return partition;
}
}

IfcNativeJoinMesh ifc_native_wall_join_mesh(const WallJoin& join,
    const std::vector<Wall>& walls, std::size_t vertices, std::size_t triangles) {
    join_preflight(walls.size(), vertices, triangles);
    std::size_t openings = 0, parts = 0, stations = 0;
    for (const auto& wall : walls) {
        preflight(wall, vertices, triangles);
        openings += wall.openings.size();
        parts += std::max(std::size_t{1}, wall.layers.size());
        const auto angle = std::abs(wall.baseline.sweep_radians);
        if (angle > 1e-7) {
            const auto radius = std::hypot(wall.baseline.end.x-wall.baseline.start.x,
                wall.baseline.end.y-wall.baseline.start.y) / (2*std::abs(std::sin(angle*.5))) + wall.thickness;
            const auto step = 2*std::acos(std::clamp(1-ifc_native_mesh_deviation_m*.5/radius,-1.0,1.0));
            const auto needed = std::ceil(angle/step) * std::max(std::size_t{1},wall.layers.size());
            if (!std::isfinite(needed) || needed < 1 || needed > static_cast<double>(std::min(vertices,triangles)/64-stations))
                throw std::invalid_argument("ifc_mesh_budget_exceeded");
            stations += static_cast<std::size_t>(needed);
        }
        if (parts > 256 || parts > std::min(vertices, triangles) / 64 ||
            openings > 256 || parts + openings + stations > std::min(vertices, triangles) / 64)
            throw std::invalid_argument("ifc_mesh_budget_exceeded");
    }
    // The wall factory supplies the authoritative connectivity and hosted-cut
    // contract. Ordered subtraction assigns those same solids' overlap without
    // introducing roof semantics or the roof factory's sixteen-member limit.
    const auto fused = make_wall_join(join, walls);
    std::vector<TopoDS_Shape> shapes;
    for (const auto& id : join.wall_ids) {
        const auto source = std::find_if(walls.begin(), walls.end(), [&](const Wall& wall) { return wall.id == id; });
        if (source == walls.end()) throw std::invalid_argument("ifc_native_join_source_missing");
        shapes.push_back(make_wall(*source));
    }
    const auto partition = ordered_wall_regions(join, shapes, fused);
    // make_wall emits the actual cut layers as ordered direct compound
    // children. Intersect those with the disjoint member region so layer
    // bindings follow real geometry, including arcs, slopes and junction cuts.
    RoofJoinPartition materials{partition.shape, partition.fused_volume, {}};
    std::vector<std::optional<std::string>> layer_ids;
    for (std::size_t i = 0; i < join.wall_ids.size(); ++i) {
        const auto& source = *std::find_if(walls.begin(), walls.end(), [&](const Wall& wall) { return wall.id == join.wall_ids[i]; });
        const auto& region = partition.regions[i];
        if (source.layers.empty()) {
            materials.regions.push_back(region); layer_ids.push_back(std::nullopt);
            continue;
        }
        TopoDS_Iterator child(shapes[i]);
        double sum = 0;
        for (const auto& layer : source.layers) {
            if (!child.More()) throw std::invalid_argument("ifc_native_wall_layer_partition_invalid");
            const auto layer_shape = child.Value(); child.Next();
            TopoDS_Shape net;
            if (!region.shape.IsNull()) {
                BRepAlgoAPI_Common common(layer_shape, region.shape);
                common.Build();
                if (!common.IsDone() || common.HasErrors()) throw std::invalid_argument("ifc_native_wall_layer_partition_invalid");
                BRep_Builder builder; TopoDS_Compound solids; builder.MakeCompound(solids);
                std::size_t count = 0;
                for (TopExp_Explorer solid(common.Shape(), TopAbs_SOLID); solid.More(); solid.Next()) {
                    builder.Add(solids, solid.Current()); ++count;
                }
                if (count) net = solids;
                if (!net.IsNull() && !BRepCheck_Analyzer(net).IsValid())
                    throw std::invalid_argument("ifc_native_wall_layer_partition_invalid");
            }
            const auto gross = solid_volume(layer_shape), volume = solid_volume(net);
            if (!std::isfinite(volume) || volume < 0 || volume > gross + 1e-8 * std::max(1.0, gross))
                throw std::invalid_argument("ifc_native_wall_layer_partition_invalid");
            materials.regions.push_back({source.id, net, gross, volume});
            layer_ids.push_back(layer.id); sum += volume;
        }
        if (child.More() || std::abs(sum - region.net_volume) > 1e-8 * std::max(1.0, region.net_volume))
            throw std::invalid_argument("ifc_native_wall_layer_partition_invalid");
    }
    auto result = join_mesh(materials, vertices, triangles);
    for (std::size_t i = 0; i < result.regions.size(); ++i) result.regions[i].layer_id = layer_ids[i];
    return result;
}

IfcNativeJoinMesh ifc_native_roof_join_mesh(const RoofJoin& join,
    const std::vector<Entity>& roofs, std::size_t vertices, std::size_t triangles) {
    validate_roof_join_semantics(join);
    if (roofs.size() != join.roof_ids.size()) throw std::invalid_argument("ifc_native_join_source_count_invalid");
    join_preflight(roofs.size(), vertices, triangles, join.singleton_material_scope ? 1 : 2);
    std::vector<TopoDS_Shape> shapes;
    std::size_t cuts = 0;
    for (const auto& id : join.roof_ids) {
        const auto source = std::find_if(roofs.begin(), roofs.end(), [&](const Entity& roof) { return roof.id == id; });
        if (source == roofs.end() || source->type != "roof") throw std::invalid_argument("ifc_native_join_source_missing");
        const auto openings = source->properties.find("roof_openings");
        if (openings != source->properties.end()) {
            if (!openings->is_array()) throw std::invalid_argument("ifc_native_roof_openings_invalid");
            cuts += openings->size();
        }
        if (cuts > 256 || cuts > std::min(vertices, triangles) / 64)
            throw std::invalid_argument("ifc_mesh_budget_exceeded");
    }
    for (const auto& id : join.roof_ids) {
        const auto source = std::find_if(roofs.begin(), roofs.end(), [&](const Entity& roof) { return roof.id == id; });
        shapes.push_back(make_building_shape(decode_building_entity(*source)));
    }
    return join_mesh(make_roof_join_partition(join, shapes), vertices, triangles);
}

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
    cut.top_gradient_m_per_m.reset();
    return tessellate(make_wall(cut), vertices, triangles);
}
std::vector<IfcNativeMesh> ifc_native_fill_mesh(const Wall& wall, const HostedOpening& opening,
    const OpeningAssembly& assembly, const std::optional<DoorOperation>& operation,
    std::size_t vertices, std::size_t triangles) {
    preflight(wall, vertices, triangles);
    return tessellate(make_opening_assembly(wall, opening, assembly, operation), vertices, triangles);
}

std::vector<IfcNativeMesh> ifc_native_roof_mesh(const Entity& roof,
    std::size_t vertices, std::size_t triangles) {
    if (roof.type != "roof") throw std::invalid_argument("ifc_native_roof_type_invalid");
    const auto openings = roof.properties.find("roof_openings");
    const auto cuts = openings != roof.properties.end() && openings->is_array() ? openings->size() : 0;
    if (cuts > 256) throw std::invalid_argument("ifc_mesh_budget_exceeded");
    if (vertices < 24 + cuts * 16 || triangles < 12 + cuts * 16)
        throw std::invalid_argument("ifc_mesh_budget_exceeded");
    return tessellate(make_building_shape(decode_building_entity(roof)), vertices, triangles);
}

std::vector<IfcNativeMesh> ifc_native_room_mesh(const Entity& room,
    std::size_t vertices, std::size_t triangles) {
    if (room.type != "room") throw std::invalid_argument("ifc_native_room_type_invalid");
    if (!vertices || !triangles) throw std::invalid_argument("ifc_mesh_budget_exceeded");
    // Check the input's minimum triangulation cost before the solid decoder
    // enters OCCT. Actual storage is charged again by the shared tessellator.
    std::size_t edges = 0;
    std::size_t stations = 0;
    const auto storage = std::min(vertices, triangles);
    const auto charge = [&](const nlohmann::json& boundary) {
        if (!boundary.is_array() || boundary.size() > storage / 4 - edges || boundary.size() > 2048 - edges)
            throw std::invalid_argument("ifc_mesh_budget_exceeded");
        edges += boundary.size();
        for (const auto& edge : boundary) {
            // Structure/numbers are strictly decoded below; malformed data
            // does not get a speculative geometry substitute in this preflight.
            if (!edge.is_object() || !edge.contains("sweep_radians") || !edge.at("sweep_radians").is_number() ||
                !edge.contains("start") || !edge.contains("end") || !edge.at("start").is_array() || !edge.at("end").is_array() ||
                edge.at("start").size() != 2 || edge.at("end").size() != 2) continue;
            const auto angle = std::abs(edge.at("sweep_radians").get<double>());
            if (!std::isfinite(angle) || angle <= 1e-7) continue;
            const auto& a = edge.at("start"); const auto& b = edge.at("end");
            if (!a[0].is_number() || !a[1].is_number() || !b[0].is_number() || !b[1].is_number()) continue;
            const auto radius = std::hypot(b[0].get<double>()-a[0].get<double>(),b[1].get<double>()-a[1].get<double>()) /
                (2.0*std::abs(std::sin(angle*.5)));
            const auto step = 2.0*std::acos(std::clamp(1.0-ifc_native_mesh_deviation_m*.5/radius,-1.0,1.0));
            const auto needed = std::ceil(angle/step);
            if (!std::isfinite(needed) || needed < 1 || needed > static_cast<double>(storage/64-stations))
                throw std::invalid_argument("ifc_mesh_budget_exceeded");
            stations += static_cast<std::size_t>(needed);
        }
    };
    if (room.properties.contains("boundary")) charge(room.properties.at("boundary"));
    else if (room.properties.contains("segments")) charge(room.properties.at("segments"));
    if (room.properties.contains("holes") && room.properties.at("holes").is_array()) {
        if (room.properties.at("holes").size() > 256) throw std::invalid_argument("ifc_mesh_budget_exceeded");
        for (const auto& hole : room.properties.at("holes")) charge(hole);
    }
    RoomVolume volume;
    std::string error;
    if (!read_document_room(room, volume, error)) throw std::invalid_argument(error);
    return tessellate(make_room_volume(volume), vertices, triangles);
}

std::vector<IfcNativeMesh> ifc_native_stair_mesh(const Entity& stair,
    std::size_t vertices, std::size_t triangles) {
    if (stair.type != "stair") throw std::invalid_argument("ifc_native_stair_type_invalid");
    const auto& p = stair.properties;
    // Reserve before semantic layout allocates treads or tests landing overlap.
    if (!p.contains("riser_count") || !p.at("riser_count").is_number_integer() ||
        p.at("riser_count") <= 0 || p.at("riser_count") > 10000)
        throw std::invalid_argument("ifc_native_stair_count_invalid");
    const auto risers = p.at("riser_count").get<std::size_t>();
    const auto landings = p.contains("landings") && p.at("landings").is_array() ? p.at("landings").size() : 0;
    const auto work = risers + landings + 2;
    if (work > std::min(vertices, triangles) / 64 || landings > 256 ||
        4 * work * (work - 1) / 2 > project_import_geometry_pair_limit)
        throw std::invalid_argument("ifc_mesh_budget_exceeded");
    return tessellate(make_stair_flight(decode_stair_properties(stair.id, p)), vertices, triangles);
}

std::vector<IfcNativeMesh> ifc_native_railing_mesh(const Entity& entity,
    const Entity* resolved_stair, std::size_t vertices, std::size_t triangles) {
    if (entity.type != "railing") throw std::invalid_argument("ifc_native_railing_type_invalid");
    const auto railing = decode_railing_properties(entity.id, entity.properties);
    if (railing.host || railing.landing_host) {
        if (!resolved_stair || resolved_stair->type != "stair")
            throw std::invalid_argument("ifc_native_railing_host_missing");
        // The host preflight runs before its decoder's topology work.
        std::size_t work{}, railing_work{};
        try {
            work = project_import_detail::native_stair_railing_work(*resolved_stair);
            railing_work = project_import_detail::native_stair_railing_work(entity, resolved_stair);
        } catch (const std::exception&) {
            throw std::invalid_argument("ifc_mesh_budget_exceeded");
        }
        if (railing_work > std::min(vertices, triangles) / 64 ||
            4 * work * (work - 1) / 2 > project_import_geometry_pair_limit)
            throw std::invalid_argument("ifc_mesh_budget_exceeded");
        const auto stair = decode_stair_properties(resolved_stair->id, resolved_stair->properties);
        const auto layout = derive_hosted_railing_layout(railing, stair);
        if (layout.posts.size() + 1 > std::min(vertices, triangles) / 64)
            throw std::invalid_argument("ifc_mesh_budget_exceeded");
        return tessellate(make_hosted_railing(railing, stair), vertices, triangles);
    }
    const auto posts = std::floor((railing.length - 1e-7) / railing.post_spacing) + 3;
    if (!std::isfinite(posts) || posts > static_cast<double>(std::min(vertices, triangles) / 64))
        throw std::invalid_argument("ifc_mesh_budget_exceeded");
    return tessellate(make_railing(railing), vertices, triangles);
}
} // namespace sketch
