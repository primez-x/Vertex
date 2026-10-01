#include "sketch/architecture.hpp"
#include "sketch/document.hpp"
#include "sketch/hosted_opening_geometry.hpp"
#include "sketch/hosted_opening_plan.hpp"

#include <BRepAdaptor_Curve.hxx>
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

gp_Pnt mass_centre(const TopoDS_Shape& shape) {
    GProp_GProps properties;
    BRepGProp::VolumeProperties(shape, properties);
    return properties.CentreOfMass();
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
