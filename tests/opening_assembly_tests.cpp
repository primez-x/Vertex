#include "sketch/architecture.hpp"
#include "sketch/document.hpp"
#include "sketch/hosted_opening_geometry.hpp"
#include "sketch/hosted_opening_plan.hpp"

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepAlgoAPI_Common.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <BRep_Tool.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Iterator.hxx>
#include <gp_Ax1.hxx>
#include <gp_Dir.hxx>
#include <gp_Trsf.hxx>
#include <gp_Vec.hxx>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <iomanip>
#include <limits>
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

void near(double actual, double expected, const char* message, double epsilon = 1e-7) {
    if (!(std::abs(actual - expected) < epsilon)) {
        std::ostringstream detail;
        detail << message << ": actual " << std::setprecision(17) << actual << ", expected " << expected;
        throw std::runtime_error(detail.str());
    }
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

double shared_face_area(const TopoDS_Shape& first, const TopoDS_Shape& second) {
    // Solid Common may discard lower-dimensional contacts. Intersect actual
    // coplanar faces so an edge or point connection cannot masquerade as a
    // finite physical mating face. Derive the contact solely from the solids.
    double area = 0.0;
    for (TopExp_Explorer a(first, TopAbs_FACE); a.More(); a.Next()) {
        const auto face_a = TopoDS::Face(a.Current());
        const BRepAdaptor_Surface surface_a(face_a);
        if (surface_a.GetType() != GeomAbs_Plane) continue;
        const auto plane_a = surface_a.Plane();
        for (TopExp_Explorer b(second, TopAbs_FACE); b.More(); b.Next()) {
            const auto face_b = TopoDS::Face(b.Current());
            const BRepAdaptor_Surface surface_b(face_b);
            if (surface_b.GetType() != GeomAbs_Plane) continue;
            const auto plane_b = surface_b.Plane();
            if (!plane_a.Axis().Direction().IsParallel(plane_b.Axis().Direction(), 1e-7) ||
                plane_a.Distance(plane_b.Location()) > 1e-7) continue;
            BRepAlgoAPI_Common common(face_a, face_b); common.Build();
            require(common.IsDone() && !common.HasErrors(), "assembly mating-face check failed");
            area += sketch::surface_area(common.Shape());
        }
    }
    return area;
}

gp_Pnt mass_centre(const TopoDS_Shape& shape) {
    GProp_GProps properties;
    BRepGProp::VolumeProperties(shape, properties);
    return properties.CentreOfMass();
}

gp_Pnt attached_hinge_vertex(const std::vector<TopoDS_Shape>& parts, bool end, bool left) {
    // Obtain the attachment from the actual closed sash, independently of the
    // factory's axis calculation. Its outward corner must lie on the jamb's
    // clear face and remain a vertex of the posed sash.
    std::vector<gp_Pnt> sash_vertices;
    for (TopExp_Explorer vertices(parts[end ? 5 : 4], TopAbs_VERTEX); vertices.More(); vertices.Next())
        sash_vertices.push_back(BRep_Tool::Pnt(TopoDS::Vertex(vertices.Current())));
    require(!sash_vertices.empty(), "closed casement lost hinge-side sash vertices");
    const auto preferred = [&](const gp_Pnt& first, const gp_Pnt& second) {
        if (std::abs(first.X() - second.X()) > 1e-7) return end ? first.X() > second.X() : first.X() < second.X();
        if (std::abs(first.Y() - second.Y()) > 1e-7) return left ? first.Y() > second.Y() : first.Y() < second.Y();
        return first.Z() < second.Z();
    };
    const auto pivot = *std::min_element(sash_vertices.begin(), sash_vertices.end(), preferred);
    double minimum_x = std::numeric_limits<double>::infinity(), maximum_x = -minimum_x;
    double minimum_y = minimum_x, maximum_y = maximum_x;
    double minimum_z = minimum_x, maximum_z = maximum_x;
    for (TopExp_Explorer vertices(parts[end ? 1 : 0], TopAbs_VERTEX); vertices.More(); vertices.Next()) {
        const auto point = BRep_Tool::Pnt(TopoDS::Vertex(vertices.Current()));
        minimum_x = std::min(minimum_x, point.X()); maximum_x = std::max(maximum_x, point.X());
        minimum_y = std::min(minimum_y, point.Y()); maximum_y = std::max(maximum_y, point.Y());
        minimum_z = std::min(minimum_z, point.Z()); maximum_z = std::max(maximum_z, point.Z());
    }
    near(pivot.X(), end ? minimum_x : maximum_x, "closed sash hinge edge is detached from jamb face");
    require(pivot.Y() >= minimum_y - 1e-7 && pivot.Y() <= maximum_y + 1e-7 &&
            pivot.Z() >= minimum_z - 1e-7 && pivot.Z() <= maximum_z + 1e-7,
            "closed sash hinge vertex is outside the real jamb face");
    return pivot;
}

bool has_vertex_at(const TopoDS_Shape& shape, const gp_Pnt& point) {
    for (TopExp_Explorer vertices(shape, TopAbs_VERTEX); vertices.More(); vertices.Next())
        if (BRep_Tool::Pnt(TopoDS::Vertex(vertices.Current())).Distance(point) < 1e-7) return true;
    return false;
}

void check_window_descriptors() {
    using namespace sketch;
    const nlohmann::json bay{{"version", 3}, {"kind", "window"},
        {"frame_width_m", 0.08}, {"frame_depth_m", 0.12},
        {"panel_thickness_m", 0.04}, {"glazing_thickness_m", 0.02}, {"inset_m", 0.0},
        {"window_layout", "bay"}, {"window_hinge_at_end", false}, {"window_open_left", true},
        {"window_angle_degrees", 90.0}, {"window_slide_fraction", 0.0},
        {"window_bay_projection_m", 0.65}, {"window_bay_front_fraction", 0.5}};
    require(opening_assembly_json(parse_opening_assembly(bay)) == bay,
            "projecting bay descriptor did not round trip through its exact v3 schema");
    const auto canonical = default_opening_assembly(OpeningAssemblyKind::window);
    const auto legacy = opening_assembly_json(canonical);
    require(legacy.size() == 7 && legacy.at("version") == 1,
            "canonical window descriptor changed legacy persistence");
    auto canonical_v2_door = legacy;
    canonical_v2_door["version"] = 2;
    canonical_v2_door["kind"] = "door";
    canonical_v2_door["window_layout"] = "fixed";
    canonical_v2_door["window_hinge_at_end"] = false;
    canonical_v2_door["window_open_left"] = true;
    canonical_v2_door["window_angle_degrees"] = 90.0;
    canonical_v2_door["window_slide_fraction"] = 0.0;
    rejects([&] { (void)parse_opening_assembly(canonical_v2_door); },
            "a canonical v2 door accepted a window-only descriptor");
    for (const auto layout : {WindowLayoutKind::double_fixed, WindowLayoutKind::triple_fixed,
                              WindowLayoutKind::casement, WindowLayoutKind::sliding}) {
        auto value = canonical;
        value.window_layout = layout;
        if (layout == WindowLayoutKind::casement) {
            value.window_hinge_at_end = true;
            value.window_open_left = false;
            value.window_angle_degrees = 0;
        }
        if (layout == WindowLayoutKind::sliding) value.window_slide_fraction = 0.75;
        const auto json = opening_assembly_json(value);
        require(json.size() == 12 && json.at("version") == 2,
                "window descriptor did not emit complete v2 schema");
        require(parse_opening_assembly(json) == value, "v2 window descriptor round trip failed");
        auto invalid = json; invalid["unknown"] = 1;
        rejects([&]{ (void)parse_opening_assembly(invalid); }, "unknown descriptor key accepted");
        invalid = json; invalid["window_hinge_at_end"] = 1;
        rejects([&]{ (void)parse_opening_assembly(invalid); }, "nonboolean window handing accepted");
        invalid = json; invalid["window_angle_degrees"] = "90";
        rejects([&]{ (void)parse_opening_assembly(invalid); }, "nonnumeric angle accepted");
        invalid = json; invalid.erase("window_open_left");
        rejects([&]{ (void)parse_opening_assembly(invalid); }, "incomplete v2 schema accepted");
        invalid = json; invalid["version"] = 3;
        rejects([&]{ (void)parse_opening_assembly(invalid); }, "unknown descriptor version accepted");
        invalid = json; invalid["version"] = 2.0;
        rejects([&]{ (void)parse_opening_assembly(invalid); }, "floating descriptor version accepted");
        invalid = json; invalid["window_layout"] = "awning";
        rejects([&]{ (void)parse_opening_assembly(invalid); }, "unknown window layout accepted");
        invalid = json; invalid["window_slide_fraction"] = nullptr;
        rejects([&]{ (void)parse_opening_assembly(invalid); }, "null window travel accepted");
        invalid = json; invalid["kind"] = "door";
        rejects([&]{ (void)parse_opening_assembly(invalid); }, "door admitted window mechanism");
    }
    auto value = canonical;
    value.window_hinge_at_end = true;
    rejects([&]{ validate_opening_assembly(value); }, "fixed layout admitted dormant handing");
    value = canonical; value.window_layout = WindowLayoutKind::casement;
    value.window_slide_fraction = 0.1;
    rejects([&]{ validate_opening_assembly(value); }, "casement admitted dormant sliding travel");
    value.window_slide_fraction = 0; value.window_angle_degrees = 181;
    rejects([&]{ validate_opening_assembly(value); }, "casement angle exceeded 180 degrees");
    value.window_angle_degrees = -1;
    rejects([&]{ validate_opening_assembly(value); }, "negative casement angle accepted");
    value.window_angle_degrees = std::numeric_limits<double>::infinity();
    rejects([&]{ validate_opening_assembly(value); }, "nonfinite window angle accepted");
    value = canonical; value.window_layout = WindowLayoutKind::sliding;
    value.window_slide_fraction = 1.01;
    rejects([&]{ validate_opening_assembly(value); }, "slider travel exceeded panel width");
    value.window_slide_fraction = 0; value.window_angle_degrees = 0;
    rejects([&]{ validate_opening_assembly(value); }, "slider admitted dormant swing angle");
    value.window_angle_degrees = 90; value.window_slide_fraction = -0.1;
    rejects([&]{ validate_opening_assembly(value); }, "negative slider travel accepted");
    value.window_slide_fraction = std::numeric_limits<double>::quiet_NaN();
    rejects([&]{ validate_opening_assembly(value); }, "nonfinite slider travel accepted");
}

void check_bay_windows() {
    using namespace sketch;
    auto profile = default_opening_assembly(OpeningAssemblyKind::window);
    profile.window_layout = WindowLayoutKind::bay;
    profile.window_bay_projection_m = 0.65;
    const HostedOpening hosted{"bay", 1.0, 2.1, 0.3, 1.6};
    auto host = wall(); host.openings.push_back(hosted);
    const auto cut_wall = make_wall(host);
    near(solid_volume(cut_wall), 2.328, "bay changed the straight wall mouth cut");
    for (bool left : {true, false}) for (double inset : {-0.025, 0.025}) {
        profile.window_open_left = left; profile.inset_m = inset;
        const auto geometry = make_opening_assembly_geometry(host, hosted, profile);
        const auto parts = parts_of(geometry.shape);
        require(parts.size() == 14 && BRepCheck_Analyzer(geometry.shape).IsValid(),
                "bay lost mounting parts, sealed plates or three framed panes");
        for (std::size_t facet : {6u, 10u}) {
            double rear_attachment = 0.0;
            for (std::size_t mounting = 0; mounting < parts.size(); ++mounting)
                if (mounting < 4 || mounting >= 12)
                    rear_attachment += shared_face_area(parts[facet], parts[mounting]);
            require(rear_attachment > 0.05,
                    "bay rear facet has no finite mating face with its mounting assembly");
        }
        require(shared_face_area(parts[6], parts[8]) > 0.05 &&
                shared_face_area(parts[8], parts[10]) > 0.05,
                "consecutive bay facets have no finite miter mating faces");
        require(shared_face_area(parts[0], parts[12]) > 0.05 &&
                shared_face_area(parts[1], parts[13]) > 0.05,
                "bay butt adapters have no finite mating faces with their mounting jambs");
        require(shared_face_area(parts[2], parts[12]) > 0.01 &&
                shared_face_area(parts[3], parts[13]) > 0.01,
                "bay butt adapters have no finite mating faces with recessed mounting returns");
        require(shared_face_area(parts[6], parts[12]) > 0.05 &&
                shared_face_area(parts[10], parts[13]) > 0.05,
                "bay butt adapters have no finite mating faces with their rear facets");
        require(!geometry.door_swing && geometry.door_swings.empty(), "bay gained a door swing");
        const double side = left ? 1.0 : -1.0;
        double outermost = 0.0, low_z = 10.0, high_z = -10.0;
        for (const auto& part : parts) {
            require(solid_volume(part) > 0, "bay contains a surface in place of a material solid");
            near(intersection_volume(part, cut_wall), 0, "bay penetrated real host wall material");
            for (TopExp_Explorer v(part, TopAbs_VERTEX); v.More(); v.Next()) {
                const auto p = BRep_Tool::Pnt(TopoDS::Vertex(v.Current()));
                require(p.X() >= 1.0 - 1e-7 && p.X() <= 3.1 + 1e-7,
                        "bay escaped its hosted mouth width");
                outermost = std::max(outermost, side * p.Y());
                low_z = std::min(low_z, p.Z()); high_z = std::max(high_z, p.Z());
            }
        }
        // Boolean-cut vertices carry OCCT's 1e-7 modelling tolerance. Allow
        // that skin plus floating-point roundoff without relaxing other checks.
        near(outermost, 0.75, "bay depth was not measured beyond the selected wall face", 2e-7);
        near(low_z, 0.3, "bay lost sill elevation"); near(high_z, 1.9, "bay lost opening height");
        near(mass_centre(parts[0]).Y(), inset, "bay mounting jamb lost its inset anchor");
        // Front pane and two angled panes are distinct positive-volume parts.
        near(side * mass_centre(parts[9]).Y(), 0.69, "front glazing lost its facet depth");
        for (std::size_t pane : {7u, 9u, 11u}) {
            require(solid_volume(parts[pane]) > 0.005, "bay pane has no useful glazed area");
            for (std::size_t other = 0; other < parts.size(); ++other)
                if (other != pane) near(intersection_volume(parts[pane], parts[other]), 0,
                                       "bay glazing overlaps another material");
        }
        for (std::size_t first = 0; first < parts.size(); ++first)
            for (std::size_t second = first + 1; second < parts.size(); ++second)
                near(intersection_volume(parts[first], parts[second]), 0, "bay facet joints share material");
        const auto plan = project_hosted_opening_plan(host, hosted, profile);
        require(std::any_of(plan.begin(), plan.end(), [&](const auto& s) {
            return std::max(side * s.start.y, side * s.end.y) > 0.74;
        }), "bay plan omitted projecting front facet");
        require(std::any_of(plan.begin(), plan.end(), [](const auto& s) {
            return std::abs(s.start.x - s.end.x) > 0.2 && std::abs(s.start.y - s.end.y) > 0.2;
        }), "bay plan omitted its angled side glazing");
    }
    profile.inset_m = 0; profile.window_open_left = true;
    const auto original = make_opening_assembly(host, hosted, profile);
    auto rotated = host;
    const double angle = 0.63;
    rotated.baseline = {{7, -4}, {7 + 5 * std::cos(angle), -4 + 5 * std::sin(angle)}, 0};
    rotated.elevation = 0.4;
    gp_Trsf transform; transform.SetRotation(gp_Ax1(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)), angle);
    transform.SetTranslationPart(gp_Vec(7, -4, 0.4));
    const auto expected = BRepBuilderAPI_Transform(original, transform, true).Shape();
    near(intersection_volume(expected, make_opening_assembly(rotated, hosted, profile)), solid_volume(original),
         "bay did not follow its rotated and elevated host");
    const auto json = opening_assembly_json(profile);
    for (const char* key : {"window_bay_projection_m", "window_bay_front_fraction"}) {
        auto invalid = json; invalid.erase(key);
        rejects([&] { (void)parse_opening_assembly(invalid); }, "incomplete bay schema accepted");
        invalid = json; invalid[key] = "0.5";
        rejects([&] { (void)parse_opening_assembly(invalid); }, "nonnumeric bay dimension accepted");
    }
    auto invalid = json; invalid["unknown"] = 0;
    rejects([&] { (void)parse_opening_assembly(invalid); }, "extra bay schema field accepted");
    invalid = json; invalid["version"] = 2;
    invalid.erase("window_bay_projection_m"); invalid.erase("window_bay_front_fraction");
    rejects([&] { (void)parse_opening_assembly(invalid); }, "v2 bay descriptor accepted");
    invalid = json; invalid["window_layout"] = "fixed";
    rejects([&] { (void)parse_opening_assembly(invalid); }, "v3 non-bay descriptor accepted");
    invalid = json; invalid["kind"] = "door";
    rejects([&] { (void)parse_opening_assembly(invalid); }, "v3 bay door accepted");
    for (double projection : {0.0, -0.1, 10.1, std::numeric_limits<double>::infinity()}) {
        auto bad = profile; bad.window_bay_projection_m = projection;
        rejects([&] { validate_opening_assembly(bad); }, "invalid bay projection accepted");
    }
    for (double fraction : {0.0, 1.0, -0.1, std::numeric_limits<double>::quiet_NaN()}) {
        auto bad = profile; bad.window_bay_front_fraction = fraction;
        rejects([&] { validate_opening_assembly(bad); }, "invalid front fraction accepted");
    }
    for (double fraction : {0.01, 0.99}) {
        auto bad = profile; bad.window_bay_front_fraction = fraction;
        rejects([&] { (void)make_opening_assembly(host, hosted, bad); }, "bay facets admitted no glazing/frame fit");
    }
    for (int field = 0; field < 3; ++field) {
        auto bad = profile;
        if (field == 0) bad.window_hinge_at_end = true;
        if (field == 1) bad.window_angle_degrees = 0;
        if (field == 2) bad.window_slide_fraction = 0.1;
        rejects([&] { validate_opening_assembly(bad); }, "bay admitted dormant movement");
    }
    auto bad = profile; bad.window_layout = WindowLayoutKind::fixed;
    rejects([&] { validate_opening_assembly(bad); }, "fixed window admitted dormant projection");
    rejects([&] { (void)make_opening_assembly(curved_wall(1.5, 0.63, {7, -4}), hosted, profile); },
            "bay accepted an unfitted curved host");
}

void check_window_layout_geometry() {
    using namespace sketch;
    const auto host = wall();
    const auto hosted = opening();
    const auto canonical = default_opening_assembly(OpeningAssemblyKind::window);
    const double clear_width = hosted.width - 2 * canonical.frame_width_m;
    const double clear_height = hosted.height - 2 * canonical.frame_width_m;
    for (const int count : {2, 3}) {
        auto profile = canonical;
        profile.window_layout = count == 2 ? WindowLayoutKind::double_fixed : WindowLayoutKind::triple_fixed;
        const auto parts = parts_of(make_opening_assembly(host, hosted, profile));
        require(parts.size() == static_cast<std::size_t>(4 + count - 1 + 5 * count),
                "split window lost distinct mullions, sashes, or panes");
        const double width = (clear_width - (count - 1) * profile.frame_width_m) / count;
        const double bar = std::min(profile.frame_width_m * 0.6, width * 0.2);
        for (int pane = 0; pane < count; ++pane) {
            const std::size_t first = 4 + count - 1 + 5 * pane;
            near(solid_volume(parts[first + 4]), (width - 2 * bar) * (clear_height - 2 * bar) * profile.glazing_thickness_m,
                 "split window pane volume is wrong");
            for (std::size_t part = 0; part < parts.size(); ++part)
                if (part != first + 4) near(intersection_volume(parts[first + 4], parts[part]), 0,
                                           "split glazing overlaps sash, frame, or sibling");
        }
        const auto curve = curved_wall(-std::numbers::pi / 2, 0.63, {7, -4});
        const auto curved = make_opening_assembly(curve, hosted, profile);
        require(BRepCheck_Analyzer(curved).IsValid() && parts_of(curved).size() == parts.size(),
                "split window lost fitted curved geometry");
        auto tiny = hosted; tiny.width = 2 * profile.frame_width_m + (count - 1) * profile.frame_width_m;
        rejects([&]{ (void)make_opening_assembly(host, tiny, profile); }, "split panes admitted zero clear width");
    }
    auto cut_host = host; cut_host.openings.push_back(hosted);
    const auto wall_solid = make_wall(cut_host);
    for (bool end : {false, true}) for (bool left : {false, true}) {
        auto profile = canonical;
        profile.window_layout = WindowLayoutKind::casement;
        profile.window_hinge_at_end = end; profile.window_open_left = left;
        profile.window_angle_degrees = 0;
        const auto closed = parts_of(make_opening_assembly(host, hosted, profile));
        const auto fixed = parts_of(make_opening_assembly(host, hosted, canonical));
        near(intersection_volume(closed[8], fixed[8]), solid_volume(fixed[8]), "zero-angle casement did not remain closed");
        const auto attachment = attached_hinge_vertex(closed, end, left);
        for (double angle : {70.0, 90.0}) {
            profile.window_angle_degrees = angle;
            const auto opened = parts_of(make_opening_assembly(host, hosted, profile));
            require(opened.size() == 9, "casement lost real sash parts or glazing");
            const double side = left ? 1.0 : -1.0;
            gp_Trsf transform;
            transform.SetRotation(gp_Ax1(attachment, gp_Dir(0, 0, 1)),
                angle * std::numbers::pi / 180 * side * (end ? -1 : 1));
            require(has_vertex_at(opened[end ? 5 : 4], attachment),
                    "opened casement hinge edge detached from its actual jamb attachment");
            for (std::size_t part = 4; part < 9; ++part) {
                const auto expected = BRepBuilderAPI_Transform(closed[part], transform, true).Shape();
                near(intersection_volume(expected, opened[part]), solid_volume(expected), "casement sash or pane rotated about wrong jamb");
                near(intersection_volume(opened[part], wall_solid), 0, "casement intersects actual host");
                for (std::size_t frame_part = 0; frame_part < 4; ++frame_part)
                    near(intersection_volume(opened[part], opened[frame_part]), 0, "casement intersects frame or sill");
            }
            for (std::size_t part = 4; part < 8; ++part)
                near(intersection_volume(opened[part], opened[8]), 0, "posed casement glazing overlaps sash material");
            near(solid_volume(make_opening_assembly(host, hosted, profile)), 0.10617728,
                 "casement rotation changed material volume");
            const auto plan = project_hosted_opening_plan(host, hosted, profile);
            require(std::any_of(plan.begin(), plan.end(), [&](const auto& segment) {
                return std::max(std::abs(segment.start.y), std::abs(segment.end.y)) > host.thickness * 0.5 + 1e-7;
            }), "casement plan retained only the closed frame footprint");
            const auto geometry = make_opening_assembly_geometry(host, hosted, profile);
            require(!geometry.door_swing && geometry.door_swings.empty(), "casement gained misleading door operations");
        }
        profile.window_angle_degrees = 180;
        rejects([&]{ (void)make_opening_assembly(host, hosted, profile); },
                "recessed casement admitted a 180-degree pose through its jamb");
        profile.window_angle_degrees = 1;
        rejects([&]{ (void)make_opening_assembly(host, hosted, profile); },
                "small-angle casement admitted finite-depth collision with opposite jamb");
        profile.window_layout = WindowLayoutKind::sliding; profile.window_angle_degrees = 90;
        for (double travel : {0.0, 0.5, 1.0}) {
            profile.window_slide_fraction = travel;
            const auto parts = parts_of(make_opening_assembly(host, hosted, profile));
            require(parts.size() == 14, "slider requires two separately framed glazed sashes");
            const double half = clear_width / 2;
            near(mass_centre(parts[8]).X(), hosted.offset + profile.frame_width_m + (end ? 1.5 : 0.5) * half + (end ? -1 : 1) * half * travel,
                 "slider movable pane has wrong bounded travel");
            near(mass_centre(parts[13]).X(), hosted.offset + profile.frame_width_m + (end ? 0.5 : 1.5) * half,
                 "slider stationary pane moved");
            near(mass_centre(parts[8]).Y(), (left ? 1 : -1) * (profile.panel_thickness_m * 0.5 + default_geometry_tolerance_metres),
                 "slider track side is wrong");
            for (std::size_t pane : {std::size_t{8}, std::size_t{13}}) {
                near(intersection_volume(parts[pane], wall_solid), 0, "slider pane entered host wall");
                for (std::size_t frame_part = 0; frame_part < 4; ++frame_part)
                    near(intersection_volume(parts[pane], parts[frame_part]), 0, "slider pane entered frame");
            }
            for (std::size_t first = 4; first < 9; ++first) for (std::size_t second = 9; second < 14; ++second)
                near(intersection_volume(parts[first], parts[second]), 0, "sliding sashes share physical material");
            const auto plan = project_hosted_opening_plan(host, hosted, profile);
            require(!plan.empty() && std::none_of(plan.begin(), plan.end(), [](const auto& segment) {
                return segment.sweep_radians != 0;
            }), "straight sliding plan gained a schematic swing arc");
        }
    }
    for (auto layout : {WindowLayoutKind::casement, WindowLayoutKind::sliding}) {
        auto profile = canonical; profile.window_layout = layout;
        const auto curve = curved_wall(std::numbers::pi / 2, 0.63, {7, -4});
        rejects([&]{ (void)make_opening_assembly(curve, hosted, profile); }, "curved moving window silently became fixed");
    }
    auto shallow = canonical; shallow.window_layout = WindowLayoutKind::sliding; shallow.frame_depth_m = 0.06;
    rejects([&]{ (void)make_opening_assembly(host, hosted, shallow); }, "window slider tracks escaped frame depth");
    const double rotation = 0.63;
    const Vec2 origin{7, -4};
    auto rotated_host = host;
    rotated_host.baseline.start = origin;
    rotated_host.baseline.end = {origin.x + 5 * std::cos(rotation), origin.y + 5 * std::sin(rotation)};
    gp_Trsf rotate, translate;
    rotate.SetRotation(gp_Ax1(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)), rotation);
    translate.SetTranslation(gp_Vec(origin.x, origin.y, 0));
    translate.Multiply(rotate);
    for (auto layout : {WindowLayoutKind::double_fixed, WindowLayoutKind::triple_fixed,
                        WindowLayoutKind::casement, WindowLayoutKind::sliding}) {
        for (bool end : {false, true}) for (bool left : {false, true}) {
            auto profile = canonical; profile.window_layout = layout; profile.inset_m = 0.025;
            if (layout == WindowLayoutKind::casement || layout == WindowLayoutKind::sliding) {
                profile.window_hinge_at_end = end; profile.window_open_left = left;
            }
            if (layout == WindowLayoutKind::sliding) profile.window_slide_fraction = 0.5;
            const auto expected = BRepBuilderAPI_Transform(make_opening_assembly(host, hosted, profile), translate, true).Shape();
            const auto actual = make_opening_assembly(rotated_host, hosted, profile);
            near(intersection_volume(expected, actual), solid_volume(expected),
                 "window geometry lost its signed inset or operation on a rotated host");
        }
    }
    auto thick_host = host; thick_host.thickness = 0.4;
    auto recessed = canonical; recessed.window_layout = WindowLayoutKind::casement;
    recessed.inset_m = 0.035; recessed.frame_depth_m = 0.08;
    for (bool end : {false, true}) for (bool left : {false, true}) {
        recessed.window_hinge_at_end = end; recessed.window_open_left = left;
        thick_host.openings = {hosted};
        const auto parts = parts_of(make_opening_assembly(thick_host, hosted, recessed));
        const auto solid = make_wall(thick_host);
        for (std::size_t part = 4; part < 9; ++part)
            near(intersection_volume(parts[part], solid), 0, "recessed casement intersects thick actual host");
    }
}

void check_multi_leaf_doors() {
    using namespace sketch;
    const auto host=wall();
    const auto hosted=opening();
    const auto assembly=default_opening_assembly(OpeningAssemblyKind::door);
    for(bool end:{false,true}) for(bool left:{false,true}) {
        const DoorOperation operation{end,left,70,DoorOperationKind::double_hinged};
        const auto geometry=make_opening_assembly_geometry(host,hosted,assembly,operation);
        const auto parts=parts_of(geometry.shape);
        require(parts.size()==5 && geometry.door_swings.size()==2 && !geometry.door_swing,
                "double door requires two physical leaves and two clear-leaf arcs");
        near(solid_volume(geometry.shape),0.134336,"double leaf factory changed assembly material volume");
        for(std::size_t i=0;i<2;++i) {
            const bool jamb=i==0?end:!end;
            const double hinge=jamb?2.12:1.08;
            const double direction=jamb?-1.0:1.0;
            const double pivot_offset=0.10+2.0*default_geometry_tolerance_metres;
            const auto centre=mass_centre(parts[3+i]);
            near(solid_volume(parts[3+i]),0.042016,"double leaf is not half clear width");
            near(centre.X(),hinge+direction*(0.26*std::cos(70*std::numbers::pi/180)+pivot_offset*std::sin(70*std::numbers::pi/180)),"double leaf hinge or rotation is wrong");
            near(centre.Y(),(left?1:-1)*(pivot_offset*(1-std::cos(70*std::numbers::pi/180))+0.26*std::sin(70*std::numbers::pi/180)),"double leaves swing to inconsistent sides");
            near(segment_length(geometry.door_swings[i]),std::hypot(0.52,pivot_offset)*70*std::numbers::pi/180,"double clear-leaf arc has wrong offset-pivot radius");
            near(geometry.door_swings[i].start.x,1.6,"double arc does not start at the meeting edge");
        }
        near(intersection_volume(parts[3],parts[4]),0,"double leaves overlap physically");
        for(std::size_t leaf=3;leaf<5;++leaf) for(std::size_t frame_part=0;frame_part<3;++frame_part)
            near(intersection_volume(parts[leaf],parts[frame_part]),0,"double leaf intersects manufactured frame");
        auto wide_angle=operation;wide_angle.angle_degrees=180;
        const auto outward=parts_of(make_opening_assembly(host,hosted,assembly,wide_angle));
        near(intersection_volume(outward[3],outward[4]),0,"180-degree double leaves overlap physically");
        auto right_angle=operation;right_angle.angle_degrees=90;
        const auto right=parts_of(make_opening_assembly(host,hosted,assembly,right_angle));
        for(const auto* pose:{&outward,&right}) for(std::size_t leaf=3;leaf<5;++leaf)
            for(std::size_t frame_part=0;frame_part<3;++frame_part)
                near(intersection_volume((*pose)[leaf],(*pose)[frame_part]),0,"double leaf intersects frame at 90 or 180 degrees");
        auto cut_host=host;cut_host.openings.push_back(hosted);
        const auto host_solid=make_wall(cut_host);
        for(const auto* pose:{&parts,&outward,&right}) for(std::size_t leaf=3;leaf<5;++leaf)
            near(intersection_volume((*pose)[leaf],host_solid),0,"double leaf intersects physical host wall");
        const auto plan=project_hosted_opening_plan(host,hosted,assembly,operation);
        require(std::count_if(plan.begin(),plan.end(),[](const auto& segment){return segment.sweep_radians!=0;})==2,
                "manufactured double door projection must retain both analytical arcs");

        const DoorOperation slide{end,left,70,DoorOperationKind::sliding,0.5};
        const auto slid=make_opening_assembly_geometry(host,hosted,assembly,slide);
        const auto panels=parts_of(slid.shape);
        require(panels.size()==5 && slid.door_swings.empty() && !slid.door_swing,
                "sliding assembly must contain two actual panels without swing arcs");
        near(solid_volume(slid.shape),0.134336,"sliding travel changes assembly material volume");
        near(solid_volume(panels[3]),0.042016,"sliding movable panel width is wrong");
        near(mass_centre(panels[3]).X(),1.6,"sliding movable panel travel is wrong");
        near(mass_centre(panels[4]).X(),end?1.34:1.86,"sliding fixed panel moved");
        near(mass_centre(panels[3]).Y(),(left?1:-1)*(0.02+default_geometry_tolerance_metres),"sliding track side is wrong");
        near(mass_centre(panels[4]).Y(),(left?-1:1)*(0.02+default_geometry_tolerance_metres),"fixed and moving tracks are not separated");
        near(intersection_volume(panels[3],panels[4]),0,"sliding panels physically overlap");
        auto other_angle=slide;other_angle.angle_degrees=170;
        const auto unchanged=parts_of(make_opening_assembly(host,hosted,assembly,other_angle));
        near(intersection_volume(panels[3],unchanged[3]),0.042016,"sliding panel accidentally swung with angle");
        auto fully_open=slide;fully_open.slide_fraction=1;
        const auto stacked=parts_of(make_opening_assembly(host,hosted,assembly,fully_open));
        near(mass_centre(stacked[3]).X(),end?1.34:1.86,"fully open slider did not stack over fixed panel");
        near(intersection_volume(stacked[3],stacked[4]),0,"stacked sliding panels share solid material");
        const auto sliding_plan=project_hosted_opening_plan(host,hosted,assembly,slide);
        require(std::none_of(sliding_plan.begin(),sliding_plan.end(),[](const auto& segment){return segment.sweep_radians!=0;}),
                "sliding plan projection gained a swing arc");
    }
    rejects([&]{(void)make_opening_assembly(host,hosted,assembly,DoorOperation{false,true,1,DoorOperationKind::double_hinged});},
            "double door accepted colliding finite-thickness leaves");
    auto tiny=hosted;tiny.width=0.17;
    rejects([&]{(void)make_opening_assembly(host,tiny,assembly,DoorOperation{false,true,160,DoorOperationKind::double_hinged});},
            "thick narrow double leaves crossed the centre plane beyond 90 degrees");
    auto shallow=assembly;shallow.frame_depth_m=0.06;
    rejects([&]{(void)make_opening_assembly(host,hosted,shallow,DoorOperation{false,true,90,DoorOperationKind::sliding});},
            "sliding tracks escaped shallow frame depth");
    auto thick_host=host;thick_host.thickness=0.40;thick_host.openings.push_back(hosted);
    auto recessed=assembly;recessed.frame_depth_m=0.08;recessed.inset_m=0.035;
    const auto thick_solid=make_wall(thick_host);
    for(bool left:{false,true}) for(double angle:{70.0,90.0,180.0}) {
        const auto clear=parts_of(make_opening_assembly(thick_host,hosted,recessed,
            DoorOperation{false,left,angle,DoorOperationKind::double_hinged}));
        for(std::size_t leaf=3;leaf<5;++leaf) {
            near(intersection_volume(clear[leaf],thick_solid),0,"recessed double leaf intersects thick host wall");
            for(std::size_t frame_part=0;frame_part<3;++frame_part)
                near(intersection_volume(clear[leaf],clear[frame_part]),0,"recessed double leaf intersects frame");
        }
    }
    const auto curve=curved_wall(std::numbers::pi/2,0.63,{7,-4});
    const HostedOpening narrow{"curved-double",0.65,0.8,0,2.1};
    const auto curved=make_opening_assembly_geometry(curve,narrow,assembly,
        DoorOperation{false,true,70,DoorOperationKind::double_hinged});
    const auto halves=parts_of(curved.shape);
    require(BRepCheck_Analyzer(curved.shape).IsValid() && halves.size()==5 && curved.door_swings.size()==2,
            "fitted curved double leaf solids or arcs are invalid");
    near(intersection_volume(halves[3],halves[4]),0,"curved double leaves overlap");
    const double beta=0.64/6.0;
    const double fitted_half_width=(3.0*std::cos(beta)-0.02)*std::tan(beta)-2.0*default_geometry_tolerance_metres;
    const double chord_apothem=3.0*std::cos(beta);
    const double inner_pivot=chord_apothem-2.90*std::cos(0.8/6.0)+2.0*default_geometry_tolerance_metres;
    for(std::size_t i=0;i<2;++i) {
        near(solid_volume(halves[3+i]),fitted_half_width*0.04*2.02,"curved double leaf has wrong fitted chord volume");
        near(segment_length(curved.door_swings[i]),std::hypot(fitted_half_width,inner_pivot)*70*std::numbers::pi/180,
            "curved double arc has wrong fitted half-leaf radius");
    }
    for(double sweep:{std::numbers::pi/2,-std::numbers::pi/2}) for(bool left:{false,true})
        for(double angle:{70.0,90.0,180.0}) {
            const auto curved_host=curved_wall(sweep,0.63,{7,-4});
            auto inset_assembly=assembly;inset_assembly.inset_m=0.025;
            const auto clear=parts_of(make_opening_assembly(curved_host,narrow,inset_assembly,
                DoorOperation{false,left,angle,DoorOperationKind::double_hinged}));
            for(std::size_t leaf=3;leaf<5;++leaf) for(std::size_t frame_part=0;frame_part<3;++frame_part)
                near(intersection_volume(clear[leaf],clear[frame_part]),0,"curved double leaf intersects annular frame");
            near(intersection_volume(clear[3],clear[4]),0,"offset-pivot curved double leaves overlap");
            auto cut_host=curved_host;cut_host.openings.push_back(narrow);
            const auto host_solid=make_wall(cut_host);
            for(std::size_t leaf=3;leaf<5;++leaf)
                near(intersection_volume(clear[leaf],host_solid),0,"curved double leaf intersects actual host wall");
        }
    rejects([&]{(void)make_opening_assembly(curve,narrow,assembly,
        DoorOperation{false,true,90,DoorOperationKind::sliding});},"curved sliding tracks were silently accepted");
    auto glazed=assembly;glazed.glazing_thickness_m=0.02;
    for(const auto kind:{DoorOperationKind::double_hinged,DoorOperationKind::sliding}) {
        const auto parts=parts_of(make_opening_assembly(host,hosted,glazed,DoorOperation{false,true,90,kind}));
        require(parts.size()==7,"two glazed leaves must have separate material and pane solids");
        near(solid_volume(parts[3]),0.042016*(1-0.65*0.65),"multi-leaf glazing aperture removed wrong volume");
        near(intersection_volume(parts[3],parts[4]),0,"multi-leaf pane overlaps its leaf material");
        near(intersection_volume(parts[5],parts[6]),0,"second pane overlaps its leaf material");
    }
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
        check_window_descriptors();
        check_bay_windows();
        check_window_layout_geometry();
        check_multi_leaf_doors();
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

        for (const auto layout : {WindowLayoutKind::double_fixed, WindowLayoutKind::triple_fixed,
                                  WindowLayoutKind::casement, WindowLayoutKind::sliding}) {
            auto profile = window; profile.window_layout = layout;
            if (layout == WindowLayoutKind::casement) profile.window_angle_degrees = 0;
            if (layout == WindowLayoutKind::sliding) profile.window_slide_fraction = 0.5;
            auto persisted = opening_entity;
            persisted.properties["opening_kind"] = "window";
            persisted.properties["opening_assembly"] = opening_assembly_json(profile);
            const auto saved = Document::create({wall_entity, persisted});
            require(parse_opening_assembly(saved.snapshot().entities().at(hosted.id).properties.at("opening_assembly")) == profile,
                    "document lost v2 window descriptor");
        }

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
