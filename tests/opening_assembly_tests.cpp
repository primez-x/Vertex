#include "sketch/architecture.hpp"
#include "sketch/document.hpp"
#include "sketch/hosted_opening_geometry.hpp"

#include <BRepAdaptor_Curve.hxx>
#include <BRepAlgoAPI_Common.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRep_Tool.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Iterator.hxx>
#include <gp_Ax1.hxx>
#include <gp_Dir.hxx>
#include <gp_Trsf.hxx>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <iomanip>
#include <numbers>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template <typename Function>
void rejects(Function&& function, const char* message) {
    try {
        function();
    } catch (const std::exception&) {
        return;
    }
    throw std::runtime_error(message);
}

sketch::Wall wall() {
    return {"wall-assembly", {{0.0, 0.0}, {5.0, 0.0}, 0.0}, 0.20, 3.0, 0.0, {}};
}

sketch::HostedOpening opening() {
    return {"opening-assembly", 1.0, 1.2, 0.25, 2.1};
}

void near(double actual, double expected, const char* message) {
    require(std::abs(actual - expected) < 1e-7, message);
}

sketch::Wall curved_wall(double sweep, double rotation, sketch::Vec2 origin) {
    const auto point = [&](double angle) {
        return sketch::Vec2{origin.x + 3.0 * std::cos(angle),
                            origin.y + 3.0 * std::sin(angle)};
    };
    return {"curved-host", {point(rotation), point(rotation + sweep), sweep},
            0.20, 3.0, 0.4, {}};
}

void check_curved_window(double sweep, double rotation, sketch::Vec2 origin) {
    using namespace sketch;
    const auto host = curved_wall(sweep, rotation, origin);
    const HostedOpening hosted{"curved-window", 0.65, 1.2, 0.3, 1.6};
    auto assembly = default_opening_assembly(OpeningAssemblyKind::window);
    assembly.inset_m = 0.025;
    const auto shape = make_opening_assembly(host, hosted, assembly);
    require(BRepCheck_Analyzer(shape).IsValid(), "curved window solids are invalid");
    const auto sign = sweep > 0.0 ? 1.0 : -1.0;
    const double fw = assembly.frame_width_m;
    const double clear_width = hosted.width - 2.0 * fw;
    const double clear_height = hosted.height - 2.0 * fw;
    const double sash = std::min(fw * 0.6, clear_width * 0.2);
    const double lengths[]{fw, fw, clear_width, clear_width, sash, sash,
                            clear_width - 2.0 * sash, clear_width - 2.0 * sash,
                            clear_width - 2.0 * sash};
    const double heights[]{hosted.height, hosted.height, fw, fw,
                           clear_height, clear_height, sash, sash,
                           clear_height - 2.0 * sash};
    const double starts[]{0.0, hosted.width - fw, fw, fw, fw,
                          hosted.width - fw - sash, fw + sash, fw + sash, fw + sash};
    std::size_t index = 0;
    for (TopoDS_Iterator parts(shape); parts.More(); parts.Next(), ++index) {
        require(index < 9, "curved window contains unexpected parts");
        const double depth = index < 4 ? assembly.frame_depth_m
                             : index < 8 ? assembly.panel_thickness_m
                                         : assembly.glazing_thickness_m;
        const double radius = 3.0 - sign * assembly.inset_m;
        near(solid_volume(parts.Value()), lengths[index] * depth * heights[index] * radius / 3.0,
             "curved window part has incorrect annular volume");
        bool circular_edge = false;
        for (TopExp_Explorer edges(parts.Value(), TopAbs_EDGE); edges.More(); edges.Next()) {
            const BRepAdaptor_Curve curve(TopoDS::Edge(edges.Current()));
            if (curve.GetType() != GeomAbs_Circle) continue;
            circular_edge = true;
            const auto circle = curve.Circle();
            near(circle.Location().X(), origin.x, "window curve has wrong centre X");
            near(circle.Location().Y(), origin.y, "window curve has wrong centre Y");
            const auto r = circle.Radius();
            require(std::abs(r - (radius - depth * 0.5)) < 1e-7 ||
                    std::abs(r - (radius + depth * 0.5)) < 1e-7,
                    "window edge is not on an exact concentric depth radius");
        }
        require(circular_edge, "curved window retained a straight tangent part");
        double minimum_station = hosted.offset + hosted.width;
        double maximum_station = hosted.offset;
        for (TopExp_Explorer vertices(parts.Value(), TopAbs_VERTEX); vertices.More(); vertices.Next()) {
            const auto point = BRep_Tool::Pnt(TopoDS::Vertex(vertices.Current()));
            const double radial = std::hypot(point.X() - origin.x, point.Y() - origin.y);
            require(radial >= radius - depth * 0.5 - 1e-7 &&
                    radial <= radius + depth * 0.5 + 1e-7,
                    "curved window vertex escapes its depth band");
            const double station = project_host_station(host.baseline,
                {point.X(), point.Y()}, hosted.offset + hosted.width * 0.5);
            minimum_station = std::min(minimum_station, station);
            maximum_station = std::max(maximum_station, station);
        }
        near(minimum_station, hosted.offset + starts[index], "window first jamb station is wrong");
        near(maximum_station, hosted.offset + starts[index] + lengths[index],
             "window second jamb station is wrong");
    }
    require(index == 9, "curved window lost frame, sash, or glazing parts");
}

void check_curved_layers(double sweep, double rotation, sketch::Vec2 origin) {
    using namespace sketch;
    auto host = curved_wall(sweep, rotation, origin);
    host.layers = {{"negative-side", 0.03, WallLayerMaterial{"catalog", "finish"}},
                   {"positive-side", 0.17, WallLayerMaterial{"catalog", "core"}}};
    const auto shape = make_wall(host);
    require(BRepCheck_Analyzer(shape).IsValid(), "curved layered wall is invalid");
    const double sign = sweep > 0.0 ? 1.0 : -1.0;
    double offset = -host.thickness * 0.5;
    std::size_t index = 0;
    for (TopoDS_Iterator layers(shape); layers.More(); layers.Next(), ++index) {
        require(index < host.layers.size(), "curved wall added a material layer");
        const double next = offset + host.layers[index].thickness;
        const double first_radius = 3.0 - sign * offset;
        const double last_radius = 3.0 - sign * next;
        const double minimum_radius = std::min(first_radius, last_radius);
        const double maximum_radius = std::max(first_radius, last_radius);
        near(solid_volume(layers.Value()), 0.5 *
             (maximum_radius * maximum_radius - minimum_radius * minimum_radius) *
             std::abs(sweep) * host.height, "material layer volume mirrors its signed offsets");
        for (TopExp_Explorer vertices(layers.Value(), TopAbs_VERTEX); vertices.More(); vertices.Next()) {
            const auto point = BRep_Tool::Pnt(TopoDS::Vertex(vertices.Current()));
            const double radius = std::hypot(point.X() - origin.x, point.Y() - origin.y);
            require(std::abs(radius - minimum_radius) < 1e-7 ||
                    std::abs(radius - maximum_radius) < 1e-7,
                    "material layer occupies the wrong signed-normal side");
        }
        offset = next;
    }
    require(index == host.layers.size(), "curved wall lost material layer order");
}

std::vector<TopoDS_Shape> parts_of(const TopoDS_Shape& shape) {
    std::vector<TopoDS_Shape> parts;
    for (TopoDS_Iterator iterator(shape); iterator.More(); iterator.Next()) {
        parts.push_back(iterator.Value());
    }
    return parts;
}

double intersection_volume(const TopoDS_Shape& first, const TopoDS_Shape& second) {
    BRepAlgoAPI_Common common(first, second);
    common.Build();
    require(common.IsDone() && !common.HasErrors(), "assembly intersection check failed");
    return sketch::solid_volume(common.Shape());
}

void check_curved_door(double sweep, double rotation, sketch::Vec2 origin) {
    using namespace sketch;
    const auto host = curved_wall(sweep, rotation, origin);
    const HostedOpening hosted{"curved-door", 0.65, 0.8, 0.0, 2.1};
    auto assembly = default_opening_assembly(OpeningAssemblyKind::door);
    assembly.inset_m = 0.025;
    assembly.glazing_thickness_m = 0.02;
    const auto closed = make_opening_assembly(host, hosted, assembly);
    require(BRepCheck_Analyzer(closed).IsValid(), "curved door solid is invalid");
    const auto parts = parts_of(closed);
    require(parts.size() == 5, "curved glazed door lost a part");
    const double sign = sweep > 0.0 ? 1.0 : -1.0;
    const double radius = 3.0 - sign * assembly.inset_m;
    const double beta = (hosted.width - 2.0 * assembly.frame_width_m) / 6.0;
    const double apothem = radius * std::cos(beta);
    const double half_leaf = (apothem - assembly.panel_thickness_m * 0.5) *
                            std::tan(beta) - 2.0 * default_geometry_tolerance_metres;
    const double height = hosted.height - assembly.frame_width_m;
    near(solid_volume(parts[3]), 2.0 * half_leaf * assembly.panel_thickness_m * height *
         (1.0 - 0.65 * 0.65), "curved door leaf has wrong chord length or glazing aperture");
    near(solid_volume(parts[4]), 2.0 * half_leaf * 0.65 * assembly.glazing_thickness_m *
         height * 0.65, "curved door pane has wrong chord dimensions");
    near(intersection_volume(parts[3], parts[4]), 0.0, "door pane overlaps leaf material");
    for (std::size_t index = 0; index < 3; ++index) {
        near(intersection_volume(parts[index], parts[3]), 0.0,
             "closed chord leaf intersects its annular frame");
    }
    const double mid_angle = rotation + sign * (hosted.offset + hosted.width * 0.5) / 3.0;
    const Vec2 radial{std::cos(mid_angle), std::sin(mid_angle)};
    const Vec2 along{-sign * radial.y, sign * radial.x};
    const Vec2 midpoint{origin.x + radial.x * apothem, origin.y + radial.y * apothem};
    // Independent vertex coordinates prove that the closed leaf follows the
    // trimmed chord, rather than the tangent at either station jamb.
    for (TopExp_Explorer vertices(parts[3], TopAbs_VERTEX); vertices.More(); vertices.Next()) {
        const auto point = BRep_Tool::Pnt(TopoDS::Vertex(vertices.Current()));
        const double x = (point.X() - midpoint.x) * along.x +
                         (point.Y() - midpoint.y) * along.y;
        const double r = (point.X() - origin.x) * radial.x +
                         (point.Y() - origin.y) * radial.y;
        if (!(std::abs(x) <= half_leaf + 1e-7 &&
              r >= apothem - assembly.panel_thickness_m * 0.5 - 1e-7 &&
              r <= apothem + assembly.panel_thickness_m * 0.5 + 1e-7)) {
            std::ostringstream detail;
            detail << std::setprecision(17) << "door leaf escaped its chord profile: sweep="
                   << sweep << " x=" << x << " half_leaf=" << half_leaf
                   << " r=" << r << " apothem=" << apothem
                   << " point=(" << point.X() << ',' << point.Y() << ')';
            throw std::runtime_error(detail.str());
        }
    }
    for (const bool hinge_at_end : {false, true}) {
        for (const bool swing_left : {false, true}) {
            const DoorOperation operation{hinge_at_end, swing_left, 70.0};
            const auto swung = parts_of(make_opening_assembly(host, hosted, assembly, operation));
            require(swung.size() == 5, "swung door lost a part");
            const double hinge_along = hinge_at_end ? half_leaf : -half_leaf;
            const gp_Pnt hinge(midpoint.x + along.x * hinge_along,
                               midpoint.y + along.y * hinge_along, host.elevation + hosted.sill);
            gp_Trsf transform;
            transform.SetRotation(gp_Ax1(hinge, gp_Dir(0.0, 0.0, 1.0)),
                70.0 * std::numbers::pi / 180.0 * (swing_left ? 1.0 : -1.0) *
                (hinge_at_end ? -1.0 : 1.0));
            for (std::size_t index = 3; index < 5; ++index) {
                const auto expected = BRepBuilderAPI_Transform(parts[index], transform, true).Shape();
                near(intersection_volume(expected, swung[index]), solid_volume(expected),
                     "door handing did not rotate about its actual trimmed endpoint");
            }
            near(intersection_volume(swung[3], swung[4]), 0.0,
                 "swung glazing does not follow its leaf aperture");
        }
    }
    auto wide = hosted;
    wide.width = 2.4;
    rejects([&] { (void)make_opening_assembly(host, wide, assembly); },
            "curved door with no chord head coverage was accepted");
}
}

int main() {
    try {
        using namespace sketch;
        const auto door = default_opening_assembly(OpeningAssemblyKind::door);
        const auto window = default_opening_assembly(OpeningAssemblyKind::window);
        require(parse_opening_assembly_kind("door") == OpeningAssemblyKind::door,
                "door assembly kind codec failed");
        require(parse_opening_assembly_kind("window") == OpeningAssemblyKind::window,
                "window assembly kind codec failed");
        require(!parse_opening_assembly_kind("curtain-wall").has_value(),
                "unknown assembly kind was accepted");
        require(parse_opening_assembly(opening_assembly_json(door)) == door,
                "door assembly JSON round trip failed");
        require(parse_opening_assembly(opening_assembly_json(window)) == window,
                "window assembly JSON round trip failed");

        const auto host = wall();
        const auto hosted = opening();
        const auto door_shape = make_opening_assembly(host, hosted, door,
                                                      DoorOperation{false, true, 90.0});
        const auto window_shape = make_opening_assembly(host, hosted, window);
        require(!door_shape.IsNull() && !window_shape.IsNull(),
                "opening assemblies must produce non-null OCCT compounds");
        require(std::isfinite(solid_volume(door_shape)) && solid_volume(door_shape) > 0.0,
                "door assembly volume must be positive");
        require(std::isfinite(solid_volume(window_shape)) && solid_volume(window_shape) > 0.0,
                "window assembly volume must be positive");
        near(solid_volume(door_shape), 0.134336, "straight door solid volume changed");
        near(solid_volume(window_shape), 0.10617728, "straight window solid volume changed");

        for (const double sweep : {std::numbers::pi / 2.0, -std::numbers::pi / 2.0,
                                  1.5 * std::numbers::pi, -1.5 * std::numbers::pi}) {
            check_curved_window(sweep, 0.0, {0.0, 0.0});
            check_curved_window(sweep, 0.63, {7.0, -4.0});
            check_curved_layers(sweep, 0.0, {0.0, 0.0});
            check_curved_layers(sweep, 0.63, {7.0, -4.0});
            check_curved_door(sweep, 0.63, {7.0, -4.0});
        }
        const auto curved_host = curved_wall(std::numbers::pi / 2.0, 0.63, {7.0, -4.0});
        const HostedOpening curved_window{"window-clearance", 0.65, 1.2, 0.3, 1.6};
        const auto window_parts = parts_of(make_opening_assembly(curved_host, curved_window, window));
        require(window_parts.size() == 9, "window clearance fixture lost a part");
        for (std::size_t index = 0; index < 8; ++index) {
            near(intersection_volume(window_parts[index], window_parts[8]), 0.0,
                 "curved window glass overlaps its frame or sash");
        }

        auto glazed_door = door;
        glazed_door.glazing_thickness_m = 0.02;
        const auto straight_parts = parts_of(make_opening_assembly(host, hosted, glazed_door));
        require(straight_parts.size() == 5, "straight glazed door lost a part");
        near(solid_volume(straight_parts[3]), 0.084032 * (1.0 - 0.65 * 0.65),
             "straight glazed leaf did not remove the pane aperture");
        near(intersection_volume(straight_parts[3], straight_parts[4]), 0.0,
             "straight door pane overlaps leaf material");

        auto too_deep = door;
        too_deep.frame_depth_m = 0.30;
        rejects([&] { (void)make_opening_assembly(host, hosted, too_deep); },
                "assembly deeper than host wall was accepted");
        auto too_narrow = door;
        too_narrow.frame_width_m = 0.61;
        rejects([&] { (void)make_opening_assembly(host, hosted, too_narrow); },
                "assembly with no clear width was accepted");

        auto wall_entity = Entity::create("wall", {
            {"baseline", {{"start", {0.0, 0.0}}, {"end", {5.0, 0.0}},
                           {"sweep_radians", 0.0}}},
            {"thickness_m", 0.20}, {"height_m", 3.0}, {"elevation_m", 0.0}});
        wall_entity.id = host.id;
        auto opening_entity = Entity::create("opening", {
            {"wall_id", host.id}, {"opening_kind", "door"},
            {"offset_m", hosted.offset}, {"width_m", hosted.width},
            {"sill_m", hosted.sill}, {"height_m", hosted.height},
            {"opening_assembly", opening_assembly_json(door)}});
        opening_entity.id = hosted.id;
        const auto document = Document::create({wall_entity, opening_entity});
        require(document.snapshot().entities().at(hosted.id).properties
                    .at("opening_assembly").at("kind") == "door",
                "document did not retain opening assembly profile");

        auto mismatched = opening_entity;
        mismatched.properties["opening_kind"] = "window";
        rejects([&] { (void)Document::create({wall_entity, mismatched}); },
                "opening kind/profile mismatch was accepted");
        auto malformed = opening_entity;
        malformed.properties["opening_assembly"]["version"] = 2;
        rejects([&] { (void)Document::create({wall_entity, malformed}); },
                "unsupported opening assembly version was accepted");

        std::cout << "Opening assembly tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
