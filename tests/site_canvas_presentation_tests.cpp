#include "../src/desktop/site_canvas_presentation.hpp"

#include <cmath>
#include <iostream>
#include <limits>
#include <numbers>
#include <stdexcept>

namespace {
using namespace sketch;
using namespace sketch::desktop;

void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

void near(double actual, double expected, const char* message) {
    if (!std::isfinite(actual) || std::abs(actual - expected) > 1e-10)
        throw std::runtime_error(message);
}

void point(Vec2 actual, Vec2 expected, const char* message) {
    near(actual.x, expected.x, message);
    near(actual.y, expected.y, message);
}

template<class Callback>
void refuses(Callback&& callback, const char* message) {
    try {
        callback();
    } catch (const std::exception&) {
        return;
    }
    throw std::runtime_error(message);
}

SitePresentationPlacement placement() {
    SitePresentationPlacement result;
    result.forward = {{10.0, 20.0, 3.0}, std::numbers::pi / 2.0};
    result.inverse = inverse_site_transform(result.forward);
    return result;
}

void curve_wall_retains_every_derived_path_and_opening_control() {
    const auto pose = placement();
    CanvasEntity source;
    source.id = QStringLiteral("curved-wall");
    source.type = QStringLiteral("wall");
    source.segments = {{{0, 0}, {4, 0}, 0}, {{4, 0}, {2, 3}, .4}, {{2, 3}, {0, 0}, 0}};
    source.holes = {Boundary{Segment{{1, 1}, {2, 1}, -.2}}};
    source.snap_points = {{3, 4}};
    source.snap_segments = {{{0, 2}, {2, 2}, .1}};
    source.stroke_segments = Boundary{{{0, 0}, {4, 0}, 0}};
    source.hit_segments = {{{0, 1}, {4, 1}, 0}};
    source.drawing_alignment_segments = {{{0, 0}, {2, 0}, 0}};
    source.vertex_handles = {{QStringLiteral("v1"), {1, 2}, 7}};
    source.resize_frame = CanvasSelectionFrame{{1, 2}, .3, 4, 3};
    source.opening_width_controls = CanvasOpeningWidthControls{
        {0, 0}, {1, 0}, .9, 2.1, 7, Segment{{0, 0}, {2, 0}, .25}, .4};
    source.thickness_metres = .2;
    source.hatch_pattern = QStringLiteral("diagonal");

    const auto presented = site_presented_canvas_entity(source, pose);
    point(presented.segments[1].start, {10, 24}, "curve segment start must rotate and translate");
    point(presented.segments[1].end, {7, 22}, "curve segment end must rotate and translate");
    near(presented.segments[1].sweep_radians, .4, "analytic arc sweep sign and magnitude must be stable");
    point(presented.holes[0][0].start, {9, 21}, "semantic holes must share the entity transform");
    near(presented.holes[0][0].sweep_radians, -.2, "hole arc sweep must remain signed");
    point(presented.snap_points[0], {6, 23}, "snap points must be presented");
    point(presented.snap_segments[0].end, {8, 22}, "snap segments must be presented");
    point(presented.stroke_segments->front().end, {10, 24}, "stroke segments must be presented");
    point(presented.hit_segments.front().start, {9, 20}, "interaction paths must be presented");
    point(presented.drawing_alignment_segments.front().end, {10, 22}, "alignment baselines must be presented");
    point(presented.vertex_handles.front().position, {8, 21}, "vertex handles must be presented");
    point(presented.resize_frame->center, {8, 21}, "selection frame center must be presented");
    near(presented.resize_frame->rotation_radians, .3 + std::numbers::pi / 2.0,
         "selection frame orientation must follow the rigid yaw");
    near(presented.resize_frame->width_metres, 4, "selection frame width is a physical size");
    point(presented.opening_width_controls->start_jamb, {10, 20}, "opening start jamb must be presented");
    point(presented.opening_width_controls->end_jamb, {10, 21}, "opening end jamb must be presented");
    point(presented.opening_width_controls->host_baseline->end, {10, 22},
          "analytical opening host baseline must be presented");
    near(presented.opening_width_controls->host_baseline->sweep_radians, .25,
         "analytical host sweep must remain signed and unchanged");
    near(presented.opening_width_controls->width_metres, .9, "opening width must remain physical");
    near(presented.opening_width_controls->offset_metres, .4, "opening station must remain physical");
    near(presented.thickness_metres, .2, "wall thickness must remain physical");
    require(presented.hatch_pattern == source.hatch_pattern, "hatch presentation must be retained");

    point(source.segments[1].start, {4, 0}, "presentation must preserve authored source segments");
    point(source.vertex_handles.front().position, {1, 2}, "presentation must preserve source handles");
    point(source.opening_width_controls->host_baseline->start, {0, 0},
          "presentation must preserve the source analytical baseline");
}

void svg_and_selection_frame_move_without_scaling_artwork() {
    const auto pose = placement();
    CanvasEntity source;
    source.id = QStringLiteral("symbol");
    source.svg_symbol = CanvasSvgSymbol{};
    source.svg_symbol->catalog_id = QStringLiteral("door-window");
    source.svg_symbol->document = QByteArray("<svg/>");
    source.svg_symbol->artwork_sha256 = QByteArray("source-hash");
    source.svg_symbol->position = {2, 3};
    source.svg_symbol->rotation_radians = -.5;
    source.svg_symbol->width_metres = .8;
    source.svg_symbol->depth_metres = .25;
    source.svg_symbol->flip_horizontal = true;
    source.resize_frame = CanvasSelectionFrame{{2, 3}, -.5, .8, .25};

    const auto presented = site_presented_canvas_entity(source, pose);
    point(presented.svg_symbol->position, {7, 22}, "SVG anchor must be presented");
    near(presented.svg_symbol->rotation_radians, -.5 + std::numbers::pi / 2.0,
         "SVG orientation must follow the rigid yaw");
    near(presented.svg_symbol->width_metres, .8, "SVG width must remain a physical size");
    near(presented.svg_symbol->depth_metres, .25, "SVG depth must remain a physical size");
    require(presented.svg_symbol->flip_horizontal && !presented.svg_symbol->flip_vertical,
            "SVG flips must be retained");
    require(presented.svg_symbol->document == source.svg_symbol->document &&
            presented.svg_symbol->artwork_sha256 == source.svg_symbol->artwork_sha256,
            "SVG source bytes and digest must not be rewritten");
    point(presented.resize_frame->center, {7, 22}, "rotated selection frame must share the symbol pose");
    near(presented.resize_frame->rotation_radians, -.5 + std::numbers::pi / 2.0,
         "symbol selection axes must follow the rigid yaw");
    point(source.svg_symbol->position, {2, 3}, "SVG source placement must remain unchanged");
}

void automatic_label_transforms_positions_directions_and_local_offsets() {
    const auto pose = placement();
    CanvasLabel source;
    source.id = QStringLiteral("exterior-dimension");
    source.position = {1, 2};
    source.rotation_radians = .25;
    source.scale = 1.75;
    source.text_height_metres = .16;
    source.automatic_linear_placement = CanvasLinearLabelPlacement{
        Segment{{0, 0}, {2, 0}, 0}, {0, 1}, .35};
    source.leader_start = Vec2{2, 3};
    source.plan_label_offset = Vec2{1, 0};

    const auto presented = site_presented_canvas_label(source, pose);
    point(presented.position, {8, 21}, "label anchor must be presented");
    near(presented.rotation_radians, .25 + std::numbers::pi / 2.0,
         "label orientation must follow the rigid yaw");
    point(presented.automatic_linear_placement->anchor.start, {10, 20},
          "automatic dimension anchor start must be presented");
    point(presented.automatic_linear_placement->anchor.end, {10, 22},
          "automatic dimension anchor end must be presented");
    point(presented.automatic_linear_placement->outward_normal, {-1, 0},
          "automatic exterior normal must rotate as a direction");
    near(presented.automatic_linear_placement->clearance_metres, .35,
         "automatic label clearance must remain a physical distance");
    point(*presented.leader_start, {7, 22}, "label leader anchor must be presented");
    point(*presented.plan_label_offset, {0, 1}, "local plan-label offset must rotate without translation");
    near(presented.scale, 1.75, "label scale must remain unchanged");
    near(presented.text_height_metres, .16, "label text height must remain unchanged");
    point(source.position, {1, 2}, "label source anchor must remain unchanged");
    point(source.automatic_linear_placement->outward_normal, {0, 1},
          "label source normal must remain unchanged");
}

void reference_calibration_flips_and_grid_labels_are_retained() {
    const auto pose = placement();
    CanvasReference reference;
    reference.id = QStringLiteral("underlay");
    reference.image = QImage(2, 3, QImage::Format_ARGB32);
    reference.image.fill(0xff123456);
    reference.position = {1, 2};
    reference.metres_per_source_unit = .0254;
    reference.scale = 1.2;
    reference.rotation_degrees = 15;
    reference.flip_horizontal = true;
    reference.flip_vertical = true;
    const auto presented_reference = site_presented_canvas_reference(reference, pose);
    point(presented_reference.position, {8, 21}, "reference image anchor must be presented");
    near(presented_reference.rotation_degrees, 105, "reference image yaw must be added in degrees");
    near(presented_reference.metres_per_source_unit, .0254,
         "reference calibration must remain unchanged");
    near(presented_reference.scale, 1.2, "reference scale must remain unchanged");
    require(presented_reference.flip_horizontal && presented_reference.flip_vertical,
            "reference flips must be retained");
    require(presented_reference.image.size() == reference.image.size(),
            "reference image pixels must be retained");
    point(reference.position, {1, 2}, "reference source anchor must remain unchanged");
    near(reference.rotation_degrees, 15, "reference source rotation must remain unchanged");

    CanvasReferenceGrid grid;
    grid.id = QStringLiteral("survey-grid");
    grid.lines = {{{0, 0}, {2, 0}, ReferenceGridAxis::x, 4, true},
                  {{0, 0}, {0, 3}, ReferenceGridAxis::y, 2, false}};
    grid.visible = false;
    grid.x_label = QStringLiteral("East");
    grid.y_label = QStringLiteral("North");
    const auto presented_grid = site_presented_canvas_reference_grid(grid, pose);
    point(presented_grid.lines[0].start, {10, 20}, "grid origin must be presented");
    point(presented_grid.lines[0].end, {10, 22}, "grid X lines must rotate into the presented frame");
    point(presented_grid.lines[1].end, {7, 20}, "grid Y lines must rotate into the presented frame");
    require(presented_grid.lines[0].axis == ReferenceGridAxis::x &&
            presented_grid.lines[0].index == 4 && presented_grid.lines[0].major,
            "grid axis identity and line state must be retained");
    require(!presented_grid.visible && presented_grid.x_label == QStringLiteral("East") &&
            presented_grid.y_label == QStringLiteral("North"),
            "grid labels and visibility must be retained");
    point(grid.lines[0].end, {2, 0}, "source grid line must remain unchanged");
}

void inverse_round_trip_and_lowered_limits_refuse_without_source_changes() {
    const auto pose = placement();
    const Vec2 p{3.25, -4.5};
    const Vec2 d{-.75, 2.0};
    point(site_source_plan_point(site_presented_plan_point(p, pose), pose), p,
          "inverse point mapping must restore source coordinates");
    point(site_source_plan_delta(site_presented_plan_delta(d, pose), pose), d,
          "inverse delta mapping must restore source vectors");

    CanvasEntity source;
    source.segments = {{{0, 0}, {1, 0}, 0}, {{1, 0}, {0, 1}, .2}, {{0, 1}, {0, 0}, 0}};
    const auto source_first = source.segments.front().start;
    SiteCanvasPresentationLimits limits;
    limits.maximum_geometry_entries = 2;
    refuses([&] { (void)site_presented_canvas_entity(source, pose, limits); },
            "lowered entity geometry budget must refuse oversized source");
    point(source.segments.front().start, source_first,
          "budget refusal must preserve the source entity");

    CanvasReferenceGrid grid;
    grid.lines.resize(2);
    limits.maximum_grid_lines = 1;
    refuses([&] { (void)site_presented_canvas_reference_grid(grid, pose, limits); },
            "lowered grid-line budget must refuse oversized source");
    require(grid.lines.size() == 2, "grid budget refusal must preserve source line count");

    source.svg_symbol = CanvasSvgSymbol{};
    source.svg_symbol->position.x = std::numeric_limits<double>::infinity();
    refuses([&] { (void)site_presented_canvas_entity(source, pose); },
            "non-finite retained spatial values must refuse presentation");
    require(std::isinf(source.svg_symbol->position.x),
            "finite refusal must not partially rewrite the source");
}

void inverse_entity_transform_cancels_forward_transform() {
    const auto pose = placement();
    SitePresentationPlacement inverse_pose = pose;
    inverse_pose.forward = pose.inverse;
    inverse_pose.inverse = pose.forward;
    CanvasEntity source;
    source.segments = {{{1, 2}, {4, 2}, -.7}};
    source.snap_points = {{-3, 5}};
    source.resize_frame = CanvasSelectionFrame{{1, 2}, .35, 4, 1};
    source.svg_symbol = CanvasSvgSymbol{};
    source.svg_symbol->position = {3, -1};
    source.svg_symbol->rotation_radians = -.2;

    const auto cancelled = site_presented_canvas_entity(
        site_presented_canvas_entity(source, pose), inverse_pose);
    point(cancelled.segments[0].start, source.segments[0].start,
          "inverse placement must restore segment endpoints");
    point(cancelled.segments[0].end, source.segments[0].end,
          "inverse placement must restore segment endpoints");
    near(cancelled.segments[0].sweep_radians, -.7, "inverse placement must preserve signed arc sweep");
    point(cancelled.snap_points[0], source.snap_points[0], "inverse placement must restore snap points");
    point(cancelled.svg_symbol->position, source.svg_symbol->position,
          "inverse placement must restore SVG placement");
    near(cancelled.resize_frame->rotation_radians, .35,
         "inverse placement must restore selection orientation");
}
} // namespace

int main() {
    try {
        curve_wall_retains_every_derived_path_and_opening_control();
        svg_and_selection_frame_move_without_scaling_artwork();
        automatic_label_transforms_positions_directions_and_local_offsets();
        reference_calibration_flips_and_grid_labels_are_retained();
        inverse_round_trip_and_lowered_limits_refuse_without_source_changes();
        inverse_entity_transform_cancels_forward_transform();
        std::cout << "site_canvas_presentation_tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "site_canvas_presentation_tests failed: " << error.what() << '\n';
        return 1;
    }
}
