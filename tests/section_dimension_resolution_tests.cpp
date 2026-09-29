#include "sketch/section_dimension_resolution.hpp"
#include "sketch/building_entity.hpp"
#include "sketch/vertical_levels.hpp"
#include "support/noninteractive_errors.hpp"

#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
using namespace sketch;
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
void nearly_equal(double actual, double expected, const char* message) {
    if (!std::isfinite(actual) || std::abs(actual - expected) >= 1e-6)
        throw std::runtime_error(std::string(message) + ": expected " + std::to_string(expected) +
            ", got " + std::to_string(actual));
}
nlohmann::json segment(double x0, double y0, double x1, double y1) {
    return {{"start", {x0, y0}}, {"end", {x1, y1}}, {"sweep_radians", 0.0}};
}
nlohmann::json rectangle(double width, double depth) {
    return nlohmann::json::array({segment(0, 0, width, 0), segment(width, 0, width, depth),
        segment(width, depth, 0, depth), segment(0, depth, 0, 0)});
}
Entity wall() {
    auto value = Entity::create("wall", {{"baseline", segment(0, 0, 4, 0)},
        {"height_m", 3.0}, {"thickness_m", 0.2}, {"elevation_m", 2.0}});
    value.id = "wall"; return value;
}
Entity opening() {
    auto value = Entity::create("opening", {{"wall_id", "wall"}, {"opening_kind", "door"},
        {"offset_m", 1.0}, {"width_m", 0.9}, {"sill_m", 0.2}, {"height_m", 2.0}});
    value.id = "door"; return value;
}
CoordinatedView section(std::string id) {
    CoordinatedView view{"section", "Section", CoordinatedViewKind::section};
    view.direction = {0, 1, 0}; view.up = {0, 0, 1}; view.object_ids = {std::move(id)};
    return view;
}
SectionOverlay dimension(std::string id, SectionDimensionAxis axis = SectionDimensionAxis::horizontal) {
    SectionOverlay overlay; overlay.id = "dimension"; overlay.kind = SectionOverlayKind::dimension;
    overlay.start_m = {-999, 888}; overlay.end_m = {999, 888};
    overlay.dimension_binding = SectionDimensionBinding{std::move(id), axis, 0.75};
    return overlay;
}
ResolvedSectionDimension resolved(const DocumentSnapshot& source, const CoordinatedView& view,
                                 const SectionOverlay& overlay) {
    const auto result = resolve_section_dimension(source, view, overlay);
    if (!result.dimension) throw std::runtime_error(result.diagnostic + " frame direction=" +
        std::to_string(view.direction[0]) + "," + std::to_string(view.direction[1]) + "," + std::to_string(view.direction[2]));
    require(result.diagnostic.empty(), "resolved result has a failure diagnostic");
    return *result.dimension;
}
void edits_and_frame() {
    auto source = Document::create({wall(), opening()});
    auto view = section("wall"); auto overlay = dimension("wall");
    const auto before = source.snapshot().entities();
    auto result = resolved(source.snapshot(), view, overlay);
    nearly_equal(result.measured_metres, 4, "full wall width");
    nearly_equal(result.line_start_m[1], 5.75, "line offset independent of measurement");
    require(result.associative && result.start_m[0] != overlay.start_m[0], "binding used stale endpoints");
    view.origin_m = {1, 20, 1};
    view.presentation.crop = ViewCrop{-0.01, 0.01, -0.01, 0.01};
    view.presentation.cut_depth_m = 0.05; view.presentation.far_depth_m = 0.1;
    result = resolved(source.snapshot(), view, overlay);
    nearly_equal(result.measured_metres, 4, "crop/far plane/section plane changed source truth");
    nearly_equal(result.line_start_m[0], -1, "origin applied in actual frame");
    nearly_equal(result.line_start_m[1], 4.75, "up origin applied in actual frame");
    const double c = std::sqrt(0.5); view.direction = {c, c, 0};
    result = resolved(source.snapshot(), view, overlay);
    nearly_equal(result.measured_metres, 4.2 * c, "rotated frame must use source geometry");
    auto changed = wall(); changed.properties["baseline"] = segment(0, 0, 7, 0);
    auto edited = Document::create({changed, opening()}); view = section("wall");
    nearly_equal(resolved(edited.snapshot(), view, overlay).measured_metres, 7, "source edit must update dimension");
    require(source.snapshot().entities() == before, "resolution modified source or declared quantities");
    auto door_view = section("door"); auto door_dimension = dimension("door");
    nearly_equal(resolved(source.snapshot(), door_view, door_dimension).measured_metres, 0.9, "hosted opening width");
    door_dimension.dimension_binding->axis = SectionDimensionAxis::vertical;
    nearly_equal(resolved(source.snapshot(), door_view, door_dimension).measured_metres, 2, "hosted opening height");
    const auto door = resolved(source.snapshot(), door_view, door_dimension);
    nearly_equal(door.start_m[1], 2.2, "opening inherits host elevation and sill");
    auto wider = opening(); wider.properties["width_m"] = 1.3;
    auto changed_opening = Document::create({wall(), wider});
    door_dimension.dimension_binding->axis = SectionDimensionAxis::horizontal;
    nearly_equal(resolved(changed_opening.snapshot(), door_view, door_dimension).measured_metres, 1.3,
        "opening source edit must update semantic handles");
}
void families_and_curves() {
    auto room = Entity::create("room", {{"boundary", rectangle(5, 3)}, {"height_m", 2.8}, {"elevation_m", 1.2}});
    room.id = "room";
    auto slab = Entity::create("slab", {{"boundary", rectangle(6, 4)}, {"holes", nlohmann::json::array()},
        {"thickness_m", 0.3}, {"elevation_m", 1.0}}); slab.id = "slab";
    auto roof = encode_building_entity(SlopedRoofPanel{.id = "roof", .run = 4, .span = 3,
        .rise = 1, .pitch_radians = std::atan(0.25), .thickness = 0.2});
    auto column = encode_building_entity(CircularColumn{.id = "column", .base_center = {1, 0, 0},
        .radius = 0.7, .height = 3});
    auto source = Document::create({room, slab, roof, column});
    nearly_equal(resolved(source.snapshot(), section("room"), dimension("room")).measured_metres, 5, "room width");
    nearly_equal(resolved(source.snapshot(), section("slab"), dimension("slab", SectionDimensionAxis::vertical)).measured_metres,
        0.3, "slab thickness extent");
    require(resolved(source.snapshot(), section("roof"), dimension("roof")).measured_metres > 3.9, "roof geometry unresolved");
    nearly_equal(resolved(source.snapshot(), section("column"), dimension("column")).measured_metres, 1.4, "curved silhouette diameter");
    auto curved = section("column"); curved.direction = {0, 0, -1}; curved.up = {0, 1, 0};
    const auto circle = resolved(source.snapshot(), curved, dimension("column"));
    nearly_equal(circle.measured_metres, 1.4, "analytic arc extrema");
    nearly_equal(circle.start_m[0], 0.3, "minimum silhouette anchor"); nearly_equal(circle.start_m[1], 0, "arc anchor paired coordinate");
    const std::vector<BuildingObject> other_forms{
        RectangularColumn{.id = "rect", .width = 0.4, .depth = 0.6, .height = 3},
        Beam{.id = "beam", .start = {0, 0, 0}, .end = {3, 0, 1}, .width = 0.2, .depth = 0.3},
        StairFlight{.id = "stair", .riser_count = 4, .total_rise = 2, .going = 0.25, .width = 1.5},
        Railing{.id = "railing", .length = 3, .height = 1.1, .thickness = 0.08, .post_spacing = 0.9},
        GableRoof{.id = "gable", .length = 5, .span = 4, .rise = 1, .pitch_radians = std::atan(0.5), .thickness = 0.1},
        HipRoof{.id = "hip", .length = 6, .span = 4, .rise = 1, .pitch_radians = std::atan(0.5), .thickness = 0.1}};
    for (const auto& object : other_forms) {
        const auto encoded = encode_building_entity(object);
        const auto single = Document::create({encoded});
        const auto extent = resolved(single.snapshot(), section(encoded.id), dimension(encoded.id));
        require(extent.associative && extent.measured_metres > 0.1, "supported building form unresolved");
    }
    // Exact source extents remain measurable when an oblique circular surface
    // would produce an ellipse outside the display line/arc codec.
    const double c = std::sqrt(0.5);
    curved.direction = {c, 0, -c}; curved.up = {0, 1, 0};
    const auto oblique = resolved(source.snapshot(), curved, dimension("column"));
    nearly_equal(oblique.measured_metres, (3.0 + 1.4) * c, "oblique curved source extent");
}
void levels() {
    const VerticalLevelGraph graph({{"ground", 0}, {"upper", 10}}, {{"storey", "ground", "upper"}});
    auto levels = Entity::create("vertical_levels", {{"model", nlohmann::json::parse(graph.serialize())}}); levels.id = "levels";
    auto property = Entity::create("property"); property.id = "property";
    auto building = Entity::create("building", {{"property_id", property.id}}); building.id = "building";
    auto floor = Entity::create("floor", {{"building_id", building.id}, {"vertical_level_binding",
        {{"version", 1}, {"graph_id", levels.id}, {"level_id", "upper"}}}}); floor.id = "floor";
    auto layer = Entity::create("layer", {{"floor_id", floor.id}}); layer.id = "layer";
    auto placed = wall(); placed.properties["layer_id"] = layer.id;
    placed.properties["vertical_placement"] = {{"version", 1}, {"mode", "level"}, {"offset_m", 0.5}};
    auto source = Document::create({levels, property, building, floor, layer, placed, opening()});
    const auto result = resolved(source.snapshot(), section("door"), dimension("door", SectionDimensionAxis::vertical));
    nearly_equal(result.start_m[1], 12.7, "hosted opening must resolve source elevation plus level offset and sill");
    nearly_equal(result.measured_metres, 2, "level resolution changed declared opening height");
}
void failures_and_detached() {
    auto label = Entity::create("label"); label.id = "label";
    auto source = Document::create({wall(), opening(), label});
    for (const auto& id : {"missing", "label"}) {
        const auto result = resolve_section_dimension(source.snapshot(), section(id), dimension(id));
        require(!result.dimension && !result.diagnostic.empty(), "unresolved binding silently fell back");
    }
    auto view = section("wall"); auto overlay = dimension("wall");
    view.object_ids.clear();
    nearly_equal(resolved(source.snapshot(), view, overlay).measured_metres, 4, "empty reference list means all objects");
    view.object_ids = {"door"};
    require(!resolve_section_dimension(source.snapshot(), view, overlay).dimension, "reference outside view accepted");
    view = section("wall"); view.direction = {0, 0, 0};
    require(!resolve_section_dimension(source.snapshot(), view, overlay).dimension, "invalid frame accepted");
    auto bad_opening = opening(); bad_opening.properties["height_m"] = -1;
    auto invalid_geometry = Document::create({wall(), bad_opening});
    require(!resolve_section_dimension(invalid_geometry.snapshot(), section("wall"), overlay).dimension,
        "invalid hosted geometry ignored");
    overlay.dimension_binding.reset(); overlay.start_m = {1, 2}; overlay.end_m = {4, 6};
    const auto detached = resolved(source.snapshot(), section("wall"), overlay);
    nearly_equal(detached.measured_metres, 5, "legacy detached measurement");
    require(!detached.associative && detached.line_start_m == overlay.start_m, "detached coordinates altered");
    overlay.end_m = overlay.start_m;
    require(!resolve_section_dimension(source.snapshot(), section("wall"), overlay).dimension, "degenerate dimension accepted");
    overlay = dimension("wall"); overlay.dimension_binding->line_offset_m = std::numeric_limits<double>::infinity();
    require(!resolve_section_dimension(source.snapshot(), section("wall"), overlay).dimension, "nonfinite placement accepted");
}
}
int main() {
    sketch::testing::noninteractive_errors();
    try {
        edits_and_frame(); families_and_curves(); levels(); failures_and_detached();
        std::cout << "section dimension resolution tests passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
