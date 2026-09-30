#include "sketch/plan_axis_resize.hpp"
#include "sketch/building_entity.hpp"
#include "sketch/document_solid.hpp"
#include "sketch/project_store.hpp"
#include "sketch/vertical_levels.hpp"

#include <cmath>
#include <filesystem>
#include <iostream>
#include <limits>
#include <numbers>
#include <stdexcept>

namespace {
using namespace sketch;
using Json = nlohmann::json;
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void near(double actual, double expected, const char* message) {
    require(std::isfinite(actual) && std::abs(actual - expected) < 1e-7, message);
}
template <typename F> void rejects(F&& f) {
    try { f(); } catch (const std::exception&) { return; }
    throw std::runtime_error("unrepresentable resize accepted");
}
Json edge(double ax, double ay, double bx, double by, double sweep = 0) {
    return {{"start", {ax, ay}}, {"end", {bx, by}}, {"sweep_radians", sweep}};
}
Json rectangle(double x, double y, double w, double d) {
    return Json::array({edge(x,y,x+w,y), edge(x+w,y,x+w,y+d),
        edge(x+w,y+d,x,y+d), edge(x,y+d,x,y)});
}

void wall_openings_and_history() {
    auto wall = Entity::create("wall", {{"baseline", edge(1,2,5,2)},
        {"thickness_m", .2}, {"height_m", 3}, {"elevation_m", 4},
        {"layers", Json::array({{{"id", "outer"}, {"thickness_m", .08}},
                               {{"id", "inner"}, {"thickness_m", .12}}})}});
    wall.id = "wall";
    wall.extensions["opaque"] = {1, "retained"};
    auto opening = Entity::create("opening", {{"wall_id", wall.id}, {"opening_kind", "window"},
        {"offset_m", 1}, {"width_m", 1}, {"sill_m", 1}, {"height_m", 1}});
    opening.id = "window";
    auto unrelated = Entity::create("label", {{"text", "untouched"}});
    auto doc = Document::create({wall, opening, unrelated});
    const auto before = doc.snapshot();
    const auto command = plan_axis_resize_command(before, wall.id, 2, 3, {1,2});
    require(command.expected_revision == before.revision(), "resize revision fence");
    require(command.entity_changes.size() == 2, "wall and opening atomic change");
    require(doc.snapshot().entities() == before.entities(), "builder mutated input");
    const auto preview = Document::preview_command(before, command);
    Wall resized;
    std::string error;
    const auto& child = preview.entities().at(opening.id);
    require(read_document_wall(preview.entities().at(wall.id), {&child}, resized, error), "wall decode");
    near(resized.baseline.start.x, 1, "wall anchor");
    near(resized.baseline.end.x, 9, "wall length");
    near(resized.thickness, .6, "wall thickness");
    near(resized.height, 3, "wall height unchanged");
    near(resized.elevation, 4, "wall elevation unchanged");
    near(resized.openings[0].offset, 2, "opening station");
    near(resized.openings[0].width, 2, "opening width");
    near(resized.openings[0].sill, 1, "opening sill unchanged");
    near(resized.layers[0].thickness, .24, "wall layer scale");
    near(solid_volume(make_wall(resized)), (8*3 - 2)*.6, "wall net solid quantity");
    require(preview.entities().at(wall.id).extensions == wall.extensions, "extensions preserved");
    require(preview.entities().at(unrelated.id) == unrelated, "unrelated entity preserved");
    doc.apply(command);
    rejects([&] { doc.apply(command); });
    require(doc.snapshot().entities() == preview.entities(), "committed preview equality");
    doc.undo(doc.revision());
    require(doc.snapshot().entities() == before.entities(), "resize undo exact");
    doc.redo(doc.revision());
    require(doc.snapshot().entities() == preview.entities(), "resize redo exact");
    const auto path = std::filesystem::temp_directory_path() / (make_stable_id() + ".vsketch");
    try {
        (void)ProjectStore::save(path, doc.snapshot());
        const auto loaded = ProjectStore::load(path);
        require(loaded.document.snapshot().entities() == doc.snapshot().entities(), "resize save roundtrip");
        auto restored = Document::fork(loaded.document.snapshot());
        restored.undo(restored.revision());
        require(restored.snapshot().entities() == before.entities(), "saved resize history");
    } catch (...) { std::filesystem::remove(path); throw; }
    std::filesystem::remove(path);
}

void footprints_with_holes_and_arc_rejection() {
    auto room = Entity::create("room", {{"boundary", rectangle(1,2,4,3)},
        {"holes", Json::array({rectangle(2,3,1,1)})}, {"height_m", 3}, {"elevation_m", 5}});
    room.id = "room";
    auto slab = room;
    slab.id = "slab";
    slab.type = "slab";
    slab.properties["thickness_m"] = .2;
    auto doc = Document::create({room, slab});
    doc.apply(plan_axis_resize_command(doc.snapshot(), room.id, 2, 3, {1,2}));
    RoomVolume r;
    std::string error;
    require(read_document_room(doc.snapshot().entities().at(room.id), r, error), "room decode");
    near(solid_volume(make_room_volume(r)), (12-1)*6*3, "room quantity includes holes");
    near(r.boundary[0].start.x, 1, "room x anchor");
    near(r.boundary[0].start.y, 2, "room y anchor");
    doc.apply(plan_axis_resize_command(doc.snapshot(), slab.id, 2, 3, {1,2}));
    Slab s;
    require(read_document_slab(doc.snapshot().entities().at(slab.id), s, error), "slab decode");
    near(solid_volume(make_slab(s)), (12-1)*6*.2, "slab quantity includes holes");
    room.properties["boundary"][0]["sweep_radians"] = .2;
    auto curved = Document::create({room});
    rejects([&] { (void)plan_axis_resize_command(curved.snapshot(), room.id, 2, 1, {0,0}); });
    curved.apply(plan_axis_resize_command(curved.snapshot(), room.id, 2, 2, {0,0}));
    near(curved.snapshot().entities().at(room.id).properties["boundary"][0]["sweep_radians"].get<double>(), .2,
         "uniform arc preserves sweep");
}

void oriented_building_parameters() {
    const double angle = std::numbers::pi / 4;
    auto column = encode_building_entity(RectangularColumn{"column", {1,2,5}, .4,.6,3,angle});
    column.properties["quantity_entries"] = {{"/width_m", {{"old", "width"}}},
        {"/height_m", {{"old", "height"}}}, {"/vendor", {{"opaque", true}}}};
    auto doc = Document::create({column});
    near(plan_axis_resize_frame(column), angle, "column natural frame");
    rejects([&] { (void)plan_axis_resize_command(doc.snapshot(), column.id, 2,3,{1,2}); });
    doc.apply(plan_axis_resize_command(doc.snapshot(), column.id, 2,3,{1,2},angle));
    const auto c = std::get<RectangularColumn>(decode_building_entity(doc.snapshot().entities().at(column.id)));
    near(c.width,.8,"column width"); near(c.depth,1.8,"column depth");
    near(c.height,3,"column vertical size"); near(c.base_center.z,5,"column vertical position");
    near(solid_volume(make_rectangular_column(c)),.8*1.8*3,"column quantity");
    const auto resized_snapshot = doc.snapshot();
    const auto& receipts = resized_snapshot.entities().at(column.id).properties.at("quantity_entries");
    require(!receipts.contains("/width_m") && receipts.contains("/height_m") && receipts.contains("/vendor"),
            "only changed quantity receipts removed");
    auto circular = encode_building_entity(CircularColumn{"circular", {},.3,3});
    auto circle_doc = Document::create({circular});
    rejects([&] { (void)plan_axis_resize_command(circle_doc.snapshot(), circular.id,2,1,{}); });
    circle_doc.apply(plan_axis_resize_command(circle_doc.snapshot(), circular.id,2,2,{}));
    near(std::get<CircularColumn>(decode_building_entity(circle_doc.snapshot().entities().at(circular.id))).radius,.6,
         "circular radius uniform resize");
    auto stair = encode_building_entity(StairFlight{.id="stair", .base_position={1,2,0},
        .orientation_radians=angle, .riser_count=8, .total_rise=2, .going=.3, .width=1,
        .top_landing=StairLanding{.5,.15}});
    auto stair_doc = Document::create({stair});
    stair_doc.apply(plan_axis_resize_command(stair_doc.snapshot(),stair.id,2,3,{1,2},angle));
    const auto flight = std::get<StairFlight>(decode_building_entity(stair_doc.snapshot().entities().at(stair.id)));
    near(flight.going,.6,"stair going"); near(flight.width,3,"stair width");
    near(flight.total_rise,2,"stair rise preserved"); near(flight.top_landing->depth,1,"landing depth");
    require(solid_volume(make_stair_flight(flight)) > 0,"stair solid generated");
}

void roofs_beams_and_railings() {
    auto shed = encode_building_entity(SlopedRoofPanel{.id="shed", .base_position={1,2,3},
        .orientation_radians=0, .run=4, .span=3, .rise=1, .pitch_radians=std::atan(.25),
        .overhang=.2, .thickness=.1, .openings={{"light",1,1,1,.5}}});
    auto gable = encode_building_entity(GableRoof{.id="gable", .length=6, .span=4,
        .rise=1, .pitch_radians=std::atan(.5), .overhang=.3, .thickness=.1,
        .openings={{"light",-1,-1,.5,.5}}});
    auto hip = encode_building_entity(HipRoof{.id="hip", .length=6, .span=4,
        .rise=1, .pitch_radians=std::atan(.5), .overhang=.3, .thickness=.1});
    auto beam = encode_building_entity(Beam{.id="beam", .start={1,2,3}, .end={5,2,3},
        .width=.2, .depth=.3});
    auto rail = encode_building_entity(Railing{.id="rail", .base_position={1,2,3},
        .length=4, .height=1, .thickness=.05, .post_spacing=1});
    auto doc = Document::create({shed,gable,hip,beam,rail});
    for (const auto& id : {shed.id,gable.id})
        doc.apply(plan_axis_resize_command(doc.snapshot(),id,2,3,{1,2}));
    doc.apply(plan_axis_resize_command(doc.snapshot(),hip.id,3,2,{1,2}));
    const auto snapshot = doc.snapshot();
    const auto roof = std::get<SlopedRoofPanel>(decode_building_entity(snapshot.entities().at(shed.id)));
    require(roof.run>8,"shed run compensates retained overhang and pitch");
    near(roof.span,9.8,"shed span compensates retained overhang");
    near(roof.rise,1,"shed rise"); near(roof.overhang,.2,"roof overhang retained");
    near(roof.pitch_radians,std::atan(1/roof.run),"roof physical pitch recalculated");
    near(roof.openings[0].x,roof.run/4,"roof opening local x");
    near(roof.openings[0].depth,roof.span/6,"roof opening depth");
    require(solid_volume(make_sloped_roof_panel(roof))>0,"roof with resized opening solid");
    const auto g = std::get<GableRoof>(decode_building_entity(snapshot.entities().at(gable.id)));
    near(g.length,12.6,"gable length"); near(g.span,13.2,"gable span");
    near(g.pitch_radians,std::atan(1./6.6),"gable pitch");
    require(solid_volume(make_gable_roof(g))>0,"gable solid");
    const auto h = std::get<HipRoof>(decode_building_entity(snapshot.entities().at(hip.id)));
    require(solid_volume(make_hip_roof(h))>0,"hip solid");
    rejects([&] { (void)plan_axis_resize_command(snapshot,hip.id,.1,1,{}); });
    const auto shed_bounds = plan_axis_resize_bounds(shed);
    require(shed_bounds.minimum.x < 1 && shed_bounds.maximum.x > 5,
            "roof bounds include actual overhang");

    doc.apply(plan_axis_resize_command(doc.snapshot(),beam.id,2,3,{1,2}));
    const auto b = std::get<Beam>(decode_building_entity(doc.snapshot().entities().at(beam.id)));
    near(b.end.x,9,"beam axis length"); near(b.width,.6,"beam section width");
    near(b.depth,.3,"beam vertical section depth");
    near(solid_volume(make_beam(b)),8*.6*.3,"beam volume");
    auto oblique = beam;
    oblique.properties["end_m"][2] = 4;
    auto oblique_doc = Document::create({oblique});
    rejects([&] { (void)plan_axis_resize_command(oblique_doc.snapshot(),beam.id,2,1,{}); });

    doc.apply(plan_axis_resize_command(doc.snapshot(),rail.id,2,3,{1,2}));
    const auto r = std::get<Railing>(decode_building_entity(doc.snapshot().entities().at(rail.id)));
    near(r.length,7.95,"railing path compensates cap extent"); near(r.thickness,.15,"railing square section width");
    near(r.post_spacing,7.95/4,"railing post spacing"); near(r.height,1,"railing overall height");
    require(solid_volume(make_railing(r))>0,"resized railing actual solid");
    const auto rail_bounds = plan_axis_resize_bounds(rail);
    near(rail_bounds.minimum.x,1-.025,"railing bounds include start post");
    near(rail_bounds.maximum.x,5+.025,"railing bounds include end post");
}

void circular_column_orientation_survives_resize_history() {
    auto circular = encode_building_entity(CircularColumn{"oriented-circle", {1,2,3}, .3, 3});
    circular.properties["rotation_rad"] = .6;
    circular.properties["mark"] = "C-1";
    circular.extensions["opaque"] = {1, "retained"};
    near(plan_axis_resize_frame(circular), .6, "circular selection frame must retain orientation");
    auto legacy = circular;
    legacy.properties.erase("rotation_rad");
    near(plan_axis_resize_frame(legacy), 0, "legacy circular selection frame defaults to zero");
    auto malformed = circular;
    malformed.properties["rotation_rad"] = std::numeric_limits<double>::infinity();
    rejects([&] { (void)plan_axis_resize_frame(malformed); });

    auto doc = Document::create({circular});
    doc.apply(plan_axis_resize_command(doc.snapshot(), circular.id, 2, 2, {1,2}, .6));
    const auto resized = doc.snapshot().entities().at(circular.id);
    near(resized.properties.at("radius_m").get<double>(), .6, "oriented circular radius resized");
    near(plan_axis_resize_frame(resized), .6, "resize must retain circular orientation");
    require(resized.properties.at("mark") == "C-1" && resized.extensions == circular.extensions,
            "circular resize must retain unrelated properties and metadata");
    doc.undo(doc.revision());
    require(doc.snapshot().entities().at(circular.id) == circular,
            "undo must restore exact circular orientation and dimensions");
    doc.redo(doc.revision());
    require(doc.snapshot().entities().at(circular.id) == resized,
            "redo must restore exact circular orientation and dimensions");
}

void rotated_physical_footprints_match_gesture_and_anchor() {
    const double angle=.6, c=std::cos(angle), s=std::sin(angle);
    auto roof=encode_building_entity(SlopedRoofPanel{.id="anchored-roof", .base_position={5,4,3},
        .orientation_radians=angle, .run=4, .span=3, .rise=1,
        .pitch_radians=std::atan(.25), .overhang=.3, .thickness=.2});
    auto rail=encode_building_entity(Railing{.id="anchored-rail", .base_position={5,4,3},
        .orientation_radians=angle, .length=4, .height=1, .thickness=.1, .post_spacing=1});
    for (const auto& original : {roof,rail}) {
        for (const auto factor : {Vec2{2,1},Vec2{1,3},Vec2{2,3}}) {
            for (const bool upper_edge : {false,true}) {
                auto doc=Document::create({original});
                const auto before=plan_axis_resize_bounds(original);
                const auto anchor_x = factor.x==1 ? (before.minimum.x+before.maximum.x)*.5
                    : (upper_edge ? before.maximum.x : before.minimum.x);
                const auto anchor_y = factor.y==1 ? (before.minimum.y+before.maximum.y)*.5
                    : (upper_edge ? before.maximum.y : before.minimum.y);
                const Vec2 anchor{c*anchor_x-s*anchor_y,s*anchor_x+c*anchor_y};
                doc.apply(plan_axis_resize_command(doc.snapshot(),original.id,factor.x,factor.y,anchor,angle));
                const auto after=plan_axis_resize_bounds(doc.snapshot().entities().at(original.id));
                near(after.maximum.x-after.minimum.x,(before.maximum.x-before.minimum.x)*factor.x,
                    "full rotated physical width must match gesture");
                near(after.maximum.y-after.minimum.y,(before.maximum.y-before.minimum.y)*factor.y,
                    "full rotated physical depth must match gesture");
                if (factor.x!=1) near(upper_edge ? after.maximum.x : after.minimum.x,anchor_x,
                    "opposite rotated x edge must remain pinned");
                if (factor.y!=1) near(upper_edge ? after.maximum.y : after.minimum.y,anchor_y,
                    "opposite rotated y edge must remain pinned");
                if (factor.x==1) near(after.minimum.x,before.minimum.x,"unedited x extent moved");
                if (factor.y==1) near(after.minimum.y,before.minimum.y,"unedited y extent moved");
            }
        }
    }
}

void connected_stair_retains_levels() {
    const VerticalLevelGraph graph({{"lower",0},{"upper",2}},{{"link","lower","upper"}});
    auto levels = Entity::create("vertical_levels", {{"model",Json::parse(graph.serialize())}});
    levels.id = "levels";
    auto stair = encode_building_entity(StairFlight{.id="connected-stair", .riser_count=8,
        .total_rise=2, .going=.3, .width=1,
        .level_connection=StairLevelConnection{levels.id,"link","lower","upper"}});
    auto doc = Document::create({levels,stair});
    doc.apply(plan_axis_resize_command(doc.snapshot(),stair.id,2,3,{}));
    const auto snapshot = doc.snapshot();
    const auto resized = std::get<StairFlight>(decode_building_entity(snapshot.entities().at(stair.id)));
    require(resized.level_connection == std::get<StairFlight>(decode_building_entity(stair)).level_connection,
            "stair resize retained exact level connection");
    require(snapshot.entities().at(levels.id) == levels,"plan resize altered level graph");
    near(resized.total_rise,2,"connected stair rise retained");
}

void constraints_and_manufactured_opening_failures() {
    auto wall=Entity::create("wall", {{"baseline",edge(0,0,4,0)},
        {"thickness_m",.2},{"height_m",3},{"elevation_m",0}});
    wall.id="locked-wall";
    auto locked=Entity::create("constraint", {{"version",1},{"relation","fixed_length"},
        {"wall_ids",{wall.id}},{"length_m",4},
        {"quantity_entries",{{"/length_m",{{"version",1},{"original_expression","4 m"},
            {"entered_unit","m"},{"exact_metres",{{"numerator",4},{"denominator",1}}}}}}},
        {"bindings",Json::array({{{"owner_id",wall.id},{"feature","baseline"},{"role","start"}},
                                {{"owner_id",wall.id},{"feature","baseline"},{"role","end"}}})}});
    auto doc=Document::create({wall,locked});
    doc.mark_saved(doc.revision());
    const auto before=doc.snapshot();
    bool rejected_by_constraint=false;
    try { (void)plan_axis_resize_command(before,wall.id,2,1,{}); }
    catch (const DocumentError& e) { rejected_by_constraint=e.code()==DocumentErrorCode::constraint_violation; }
    require(rejected_by_constraint,"resize bypassed fixed-length constraint");
    require(doc.snapshot().entities()==before.entities() && doc.revision()==before.revision() &&
            doc.snapshot().history().size()==before.history().size() &&
            doc.snapshot().saved_revision_optional()==before.saved_revision_optional(),
            "constraint rejection changed document/history/save marker");

    for (const auto kind : {OpeningAssemblyKind::door,OpeningAssemblyKind::window}) {
        const OpeningAssembly assembly{.kind=kind,.frame_width_m=.08,.frame_depth_m=.12,
            .panel_thickness_m=.04,.glazing_thickness_m=.005,.inset_m=.02};
        const bool door=kind==OpeningAssemblyKind::door;
        auto opening=Entity::create("opening", {{"wall_id",wall.id},
            {"opening_kind",door ? "door" : "window"},{"offset_m",1},{"width_m",1},
            {"sill_m",door ? 0 : 1},{"height_m",door ? 2 : 1},
            {"opening_assembly",opening_assembly_json(assembly)}});
        if (door) opening.properties["door_operation"]=encode_door_operation({false,true,45});
        auto assembly_doc=Document::create({wall,opening});
        const auto assembly_before=assembly_doc.snapshot();
        rejects([&] { (void)plan_axis_resize_command(assembly_before,wall.id,.1,1,{}); });
        require(assembly_doc.snapshot().entities()==assembly_before.entities() &&
                assembly_doc.revision()==assembly_before.revision(),"narrow assembly failure mutated source");
        assembly_doc.apply(plan_axis_resize_command(assembly_before,wall.id,2,3,{}));
        const auto snapshot=assembly_doc.snapshot();
        const auto& child=snapshot.entities().at(opening.id);
        const auto profile=parse_opening_assembly(child.properties.at("opening_assembly"));
        near(profile.frame_width_m,.08,"manufactured frame face width retained");
        near(profile.frame_depth_m,.36,"manufactured frame depth follows host normal");
        near(profile.panel_thickness_m,.12,"panel depth follows host normal");
        near(profile.glazing_thickness_m,.015,"glazing depth follows host normal");
        near(profile.inset_m,.06,"assembly inset follows host normal");
        Wall host;
        std::string error;
        require(read_document_wall(snapshot.entities().at(wall.id),{&child},host,error),"assembly host decode");
        std::optional<DoorOperation> operation;
        if (door) {
            require(child.properties.at("door_operation")==opening.properties.at("door_operation"),
                    "door swing operation changed during resize");
            operation=decode_door_operation(child.properties.at("door_operation"));
        }
        require(solid_volume(make_opening_assembly(host,host.openings[0],profile,operation))>0,
                "resized manufactured opening solid invalid");
    }

    auto connected=wall;
    connected.id="joined-wall";
    connected.properties["baseline"]=edge(4,0,4,3);
    auto join=Entity::create("wall_join",wall_join_json(WallJoin{"join",{wall.id,connected.id}}));
    join.id="join";
    auto joined=Document::create({wall,connected,join});
    const auto joined_before=joined.snapshot();
    rejects([&] { (void)plan_axis_resize_command(joined_before,wall.id,.5,1,{}); });
    require(joined.snapshot().entities()==joined_before.entities(),"disconnecting a fused join mutated source");
}

void failures_are_atomic() {
    auto wall = Entity::create("wall", {{"baseline", edge(0,0,4,0)},
        {"thickness_m", .2}, {"height_m", 3}, {"elevation_m", 0}});
    auto doc = Document::create({wall});
    const auto before = doc.snapshot();
    for (const double value : {0.,-1.,std::numeric_limits<double>::infinity(),
                              std::numeric_limits<double>::quiet_NaN()})
        rejects([&] { (void)plan_axis_resize_command(before,wall.id,value,1,{}); });
    rejects([&] { (void)plan_axis_resize_command(before,"missing",2,1,{}); });
    rejects([&] { (void)plan_axis_resize_command(before,wall.id,2,1,{0,0},.4); });
    require(doc.snapshot().entities() == before.entities() && doc.revision() == before.revision(),
            "failed resize mutated document");
    doc.mark_read_only("test");
    rejects([&] { (void)plan_axis_resize_command(doc.snapshot(),wall.id,2,1,{}); });
}
}

int main() {
    try {
        wall_openings_and_history();
        footprints_with_holes_and_arc_rejection();
        oriented_building_parameters();
        circular_column_orientation_survives_resize_history();
        roofs_beams_and_railings();
        rotated_physical_footprints_match_gesture_and_anchor();
        connected_stair_retains_levels();
        constraints_and_manufactured_opening_failures();
        failures_are_atomic();
        std::cout << "plan axis resize tests passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n'; return 1;
    }
}
