#include "sketch/hosted_opening_resize.hpp"
#include "sketch/hosted_opening_geometry.hpp"
#include "sketch/architecture.hpp"
#include "sketch/document_solid.hpp"
#include "sketch/project_store.hpp"
#include "sketch/vertical_levels.hpp"

#include <cmath>
#include <filesystem>
#include <iostream>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {
using namespace sketch;
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
bool near(double a, double b) { return std::abs(a-b) < 1e-9; }
bool near(Vec2 a, Vec2 b) { return near(a.x,b.x) && near(a.y,b.y); }
template<class F> void rejects(F&& f, std::string_view message = {}) {
    try { f(); }
    catch (const std::exception& e) {
        if (!message.empty() && std::string(e.what()).find(message) == std::string::npos)
            throw std::runtime_error("Expected rejection containing '"+std::string(message)+
                                     "', received: "+e.what());
        return;
    }
    throw std::runtime_error("invalid hosted opening resize was accepted");
}
template<class F> void rejects_document(F&& f, DocumentErrorCode code) {
    try { f(); }
    catch (const DocumentError& e) {
        require(e.code() == code, "Document rejected command with the wrong error code");
        return;
    }
    throw std::runtime_error("Document accepted a stale hosted opening command");
}

Document fixture(std::string kind = "window", double angle = 0) {
    const VerticalLevelGraph graph({{"ground", 4}}, {});
    auto levels = Entity::create("vertical_levels", {{"model", nlohmann::json::parse(graph.serialize())}});
    levels.id = "levels";
    auto property = Entity::create("property"); property.id = "property";
    auto building = Entity::create("building", {{"property_id", property.id}}); building.id = "building";
    auto floor = Entity::create("floor", {{"building_id", building.id},
        {"vertical_level_binding", {{"version", 1}, {"graph_id", levels.id}, {"level_id", "ground"}}}});
    floor.id = "floor";
    auto layer = Entity::create("layer", {{"floor_id", floor.id}}); layer.id = "layer";
    auto wall = Entity::create("wall", {{"baseline", {{"start", {4,-3}},
        {"end", {4+10*std::cos(angle),-3+10*std::sin(angle)}}, {"sweep_radians", 0}}},
        {"height_m", 3.5}, {"elevation_m", 0}, {"thickness_m", .3}, {"layer_id", layer.id},
        {"vertical_placement", {{"version", 1}, {"mode", "level"}, {"offset_m", .5}}}});
    wall.id = "wall";
    auto opening = Entity::create("opening", {{"wall_id", wall.id}, {"opening_kind", kind},
        {"offset_m", 3.0}, {"offset", 3.0}, {"width_m", 2.0}, {"width", 2.0},
        {"height_m", kind == "door" ? 2.1 : 1.2}, {"sill_m", kind == "door" ? 0 : .9},
        {"layer_id", layer.id}, {"mark", "Retain me"},
        {"quantity_entries", {{"/width_m", {{"expression", "2 m"}}},
            {"/width", {{"expression", "2 m"}}}, {"/offset_m", {{"expression", "3 m"}}},
            {"/offset", {{"expression", "3 m"}}}, {"/height_m", {{"expression", "1.2 m"}}},
            {"legacy", {{"opaque", true}}}}}}, false, {{"vendor", {{"version", 7}}}});
    opening.id = "opening";
    if (kind != "opening")
        opening.properties["opening_assembly"] = opening_assembly_json(default_opening_assembly(
            kind == "door" ? OpeningAssemblyKind::door : OpeningAssemblyKind::window));
    if (kind == "door") opening.properties["door_operation"] = encode_door_operation(DoorOperation{});
    auto sibling = Entity::create("opening", {{"wall_id", wall.id}, {"opening_kind", "opening"},
        {"offset_m", 7.0}, {"width_m", 1.0}, {"height_m", 2.1}, {"sill_m", 0}});
    sibling.id = "sibling";
    return Document::create({levels,property,building,floor,layer,wall,opening,sibling});
}

void test_jambs_metadata_and_history() {
    for (const auto* kind : {"door","window","opening"}) {
        for (double angle : {0.0,.7,std::numbers::pi*.5}) {
            for (bool keep_start : {false,true}) {
                auto document = fixture(kind,angle);
                const auto before = document.snapshot();
                const auto frame = hosted_opening_resize_frame(before,"opening");
                require(near(frame.start_jamb, {4+3*std::cos(angle),-3+3*std::sin(angle)}) &&
                    near(frame.end_jamb,{4+5*std::cos(angle),-3+5*std::sin(angle)}) &&
                    near(frame.host_thickness_metres,.3) && near(frame.width_metres,2) &&
                    near(frame.angle_radians,angle), "frame must follow translated, rotated host baseline");
                const auto command = hosted_opening_width_resize_command(before,"opening",1.5,keep_start);
                require(command.expected_revision == before.revision() && command.entity_changes.size() == 1,
                        "resize must be one revision-fenced opening edit");
                const auto preview = Document::preview_command(before,command);
                const auto after_frame = hosted_opening_resize_frame(preview,"opening");
                require(near(keep_start ? frame.start_jamb : frame.end_jamb,
                             keep_start ? after_frame.start_jamb : after_frame.end_jamb),
                        "opposite jamb must remain pinned in world coordinates");
                auto expected = before.entities().at("opening");
                expected.properties["width_m"] = 3.0; expected.properties["width"] = 3.0;
                expected.properties["offset_m"] = keep_start ? 3.0 : 2.0;
                expected.properties["offset"] = keep_start ? 3.0 : 2.0;
                expected.properties["quantity_entries"].erase("/width_m");
                expected.properties["quantity_entries"].erase("/width");
                if (!keep_start) {
                    expected.properties["quantity_entries"].erase("/offset_m");
                    expected.properties["quantity_entries"].erase("/offset");
                }
                require(preview.entities().at("opening") == expected,
                        "resize must preserve height, sill, assembly, identity and unrelated receipts/metadata");
                for (const auto& [id,entity] : before.entities())
                    if (id != "opening") require(preview.entities().at(id) == entity, "resize changed another entity");
                require(document.snapshot().entities() == before.entities(), "detached preview mutated source");
                document.apply(command);
                require(document.snapshot().entities() == preview.entities(), "commit differed from admitted preview");
                rejects_document([&] { document.apply(command); },DocumentErrorCode::stale_revision);
                document.undo(document.revision());
                require(document.snapshot().entities() == before.entities(), "undo must restore exact metadata");
                document.redo(document.revision());
                require(document.snapshot().entities() == preview.entities(), "redo must restore exact resized state");
            }
        }
    }
    auto document = fixture();
    document.apply(hosted_opening_width_resize_command(document.snapshot(),"opening",1.5,false));
    const auto path = std::filesystem::temp_directory_path() / ("hosted-resize-"+make_stable_id()+".sketch");
    (void)ProjectStore::save(path,document.snapshot());
    auto reopened = ProjectStore::load(path).document;
    require(reopened.snapshot().entities() == document.snapshot().entities(), "save/reopen lost resized values");
    reopened.undo(reopened.revision());
    require(near(reopened.snapshot().entities().at("opening").properties.at("width_m").get<double>(),2),
            "reopened history must retain undo");
    reopened.redo(reopened.revision());
    require(reopened.snapshot().entities() == document.snapshot().entities(), "reopened redo failed");
    std::filesystem::remove(path);
}

void test_failures_and_noop() {
    auto document = fixture();
    const auto source = document.snapshot();
    for (double scale : {0.0,-1.0,std::numeric_limits<double>::infinity(),
                         std::numeric_limits<double>::quiet_NaN(),std::numeric_limits<double>::max()})
        rejects([&] { (void)hosted_opening_width_resize_command(source,"opening",scale,true); });
    rejects([&] { (void)hosted_opening_width_resize_command(source,"opening",2.5,true); }, "overlap");
    rejects([&] { (void)hosted_opening_width_resize_command(source,"opening",4,false); });
    rejects([&] { (void)hosted_opening_width_resize_command(source,"opening",4,true); });
    rejects([&] { (void)hosted_opening_width_resize_command(source,"opening",.01,true); }, "clear opening width");
    rejects([&] { (void)hosted_opening_width_resize_command(source,"missing",1.5,true); }, "missing");
    rejects([&] { (void)hosted_opening_width_resize_command(source,"wall",1.5,true); }, "opening");
    const auto noop = hosted_opening_width_resize_command(source,"opening",1,true);
    require(noop.entity_changes.empty(), "unit scale must retain all receipts and produce no changes");
    require(document.snapshot().entities() == source.entities() && document.revision() == source.revision(),
            "failure mutated the source");

    auto bad_sibling = source.entities().at("sibling");
    bad_sibling.properties["width_m"] = -1;
    auto invalid = Document::fork(source);
    invalid.apply(ApplyEntityChanges{invalid.revision(),{EntityChange::upsert(bad_sibling)}, {},"Bad sibling"});
    rejects([&] { (void)hosted_opening_width_resize_command(invalid.snapshot(),"opening",1.2,true); });

    auto bad_assembly = source.entities().at("sibling");
    bad_assembly.properties["opening_kind"] = "window";
    auto assembly = default_opening_assembly(OpeningAssemblyKind::window);
    assembly.frame_width_m = .6;
    bad_assembly.properties["opening_assembly"] = opening_assembly_json(assembly);
    auto invalid_frame = Document::fork(source);
    invalid_frame.apply(ApplyEntityChanges{invalid_frame.revision(),{EntityChange::upsert(bad_assembly)}, {},"Bad frame"});
    rejects([&] { (void)hosted_opening_width_resize_command(invalid_frame.snapshot(),"opening",1.2,true); },
            "clear opening width");
}

void test_curved_jambs_and_history() {
    for (const auto* kind : {"door","window","opening"}) {
        for (double sweep : {std::numbers::pi/2,-std::numbers::pi/2,
                             3*std::numbers::pi/2,-3*std::numbers::pi/2}) {
            for (bool keep_start : {false,true}) {
                auto document = fixture(kind);
                const auto expected_point = [](double angle) {
                    return Vec2{4+10*std::cos(angle),-3+10*std::sin(angle)};
                };
                const auto start=expected_point(.7),end=expected_point(.7+sweep);
                auto wall = document.snapshot().entities().at("wall");
                wall.properties["baseline"] = {{"start",{start.x,start.y}},
                    {"end",{end.x,end.y}},{"sweep_radians",sweep}};
                auto opening=document.snapshot().entities().at("opening");
                if(kind==std::string_view("door")) {
                    auto profile=parse_opening_assembly(opening.properties.at("opening_assembly"));
                    // A wide planar leaf on a curved host needs head depth for
                    // its chord sagitta; retain that explicit manufactured size.
                    profile.frame_depth_m=.3;
                    opening.properties["opening_assembly"]=opening_assembly_json(profile);
                }
                document.apply(ApplyEntityChanges{document.revision(),
                    {EntityChange::upsert(wall),EntityChange::upsert(opening)}, {},"Curve host"});
                const auto before = document.snapshot();
                const double sign = sweep > 0 ? 1 : -1;
                const auto frame = hosted_opening_resize_frame(before,"opening");
                require(near(frame.start_jamb,expected_point(.7+sign*.3)) &&
                    near(frame.end_jamb,expected_point(.7+sign*.5)) && near(frame.width_metres,2) &&
                    near(frame.offset_metres,3) && near(frame.host_baseline.sweep_radians,sweep),
                    "curved frame must locate jambs by measured arc station");
                const double angle = .7+sign*.4+sign*std::numbers::pi/2;
                require(near(std::cos(frame.angle_radians),std::cos(angle)) &&
                    near(std::sin(frame.angle_radians),std::sin(angle)),
                    "curved frame angle must follow the midpoint tangent");
                const auto command = hosted_opening_width_resize_command(before,"opening",1.5,keep_start);
                const auto preview = Document::preview_command(before,command);
                const auto resized = hosted_opening_resize_frame(preview,"opening");
                require(near(resized.start_jamb,expected_point(.7+sign*(keep_start?.3:.2))) &&
                    near(resized.end_jamb,expected_point(.7+sign*(keep_start?.6:.5))) &&
                    near(resized.width_metres,3), "curved resize must move one jamb by arc length");
                require(near(keep_start ? frame.start_jamb : frame.end_jamb,
                    keep_start ? resized.start_jamb : resized.end_jamb), "curved resize must pin opposite jamb");
                require(near(segment_length(hosted_opening_span(resized.host_baseline,resized.offset_metres,3)),3) &&
                    std::hypot(resized.end_jamb.x-resized.start_jamb.x,
                               resized.end_jamb.y-resized.start_jamb.y) < 3,
                    "curved width must remain arc length rather than the jamb chord");
                for (const auto& [id,entity] : before.entities())
                    if (id != "opening") require(preview.entities().at(id) == entity,"curved resize changed another entity");
                const auto& old_opening = before.entities().at("opening");
                const auto& new_opening = preview.entities().at("opening");
                for (const auto* key : {"height_m","sill_m","opening_kind","mark"})
                    require(new_opening.properties.at(key) == old_opening.properties.at(key),
                        "curved resize changed authoritative opening metadata");
                for (const auto* key : {"opening_assembly","door_operation"})
                    if (old_opening.properties.contains(key))
                        require(new_opening.properties.at(key) == old_opening.properties.at(key),
                            "curved resize changed manufactured dimensions or door operation");
                require(new_opening.extensions == old_opening.extensions &&
                    !new_opening.properties.at("quantity_entries").contains("/width_m") &&
                    new_opening.properties.at("quantity_entries").contains("/height_m") &&
                    new_opening.properties.at("quantity_entries").contains("/offset_m") == keep_start,
                    "curved resize must retain unrelated metadata and invalidate only changed receipts");
                rejects([&] { (void)hosted_opening_width_resize_command(before,"opening",2.5,true); },"overlap");
                rejects([&] { (void)hosted_opening_width_resize_command(before,"opening",4,false); });
                require(document.snapshot().entities() == before.entities(),"curved preview or rejection mutated source");
                document.apply(command);
                rejects_document([&] { document.apply(command); },DocumentErrorCode::stale_revision);
                document.undo(document.revision());
                require(document.snapshot().entities() == before.entities(),"curved undo lost exact source");
                document.redo(document.revision());
                require(document.snapshot().entities() == preview.entities(),"curved redo lost exact resized state");
                if (kind == std::string_view("window") && keep_start && sweep == 3*std::numbers::pi/2) {
                    const auto path = std::filesystem::temp_directory_path() / ("curved-resize-"+make_stable_id()+".sketch");
                    (void)ProjectStore::save(path,document.snapshot());
                    auto reopened = ProjectStore::load(path).document;
                    require(reopened.snapshot().entities() == preview.entities(),"save/reopen lost curved resize");
                    reopened.undo(reopened.revision());
                    require(reopened.snapshot().entities() == before.entities(),"reopened curved undo failed");
                    reopened.redo(reopened.revision());
                    require(reopened.snapshot().entities() == preview.entities(),"reopened curved redo failed");
                    std::filesystem::remove(path);
                }
            }
        }
    }
}

void test_join_admission() {
    auto document = fixture();
    auto other_wall = document.snapshot().entities().at("wall");
    other_wall.id = "joined-wall";
    other_wall.properties["baseline"] = {{"start", {14,-3}}, {"end", {14,2}}, {"sweep_radians", 0}};
    auto join = Entity::create("wall_join",wall_join_json(WallJoin{"joined",{"wall",other_wall.id}}));
    join.id = "joined";
    document.apply(ApplyEntityChanges{document.revision(),
        {EntityChange::upsert(other_wall),EntityChange::upsert(join)}, {},"Join host"});
    const auto command = hosted_opening_width_resize_command(document.snapshot(),"opening",1.2,true);
    require(command.entity_changes.size() == 1, "valid joined host should admit opening resize");

    // The Document admits relationship structure; detached geometry admission
    // must resolve every member rather than validating only the edited cut.
    other_wall.properties["baseline"] = {{"start", {25,-3}}, {"end", {25,2}}, {"sweep_radians", 0}};
    document.apply(ApplyEntityChanges{document.revision(),{EntityChange::upsert(other_wall)}, {},"Separate member"});
    const auto before = document.snapshot();
    rejects([&] { (void)hosted_opening_width_resize_command(before,"opening",1.2,true); });
    require(document.snapshot().entities() == before.entities(), "join rejection mutated a source member");
}
} // namespace

int main() {
    try {
        test_jambs_metadata_and_history();
        test_failures_and_noop();
        test_curved_jambs_and_history();
        test_join_admission();
        std::cout << "hosted opening resize tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
