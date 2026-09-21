#include "sketch/geometry_operations.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <numbers>
#include <stdexcept>

namespace {
void require(bool ok) { if (!ok) throw std::runtime_error("geometry operation assertion failed"); }
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
template<class F> void rejects(F f) {
    try { f(); } catch (const std::invalid_argument&) { return; }
    throw std::runtime_error("invalid operation accepted");
}
bool near(double a, double b) { return std::abs(a-b) < 1e-8; }
sketch::IdentifiedBoundary square() {
    return {"b", "boundary", {{"e0","v0","v1",{{0,0},{4,0},0}},
        {"e1","v1","v2",{{4,0},{4,3},0}}, {"e2","v2","v3",{{4,3},{0,3},0}},
        {"e3","v3","v0",{{0,3},{0,0},0}}}};
}

void test_rotated_boundary_transform_sequence() {
    using namespace sketch;
    auto source = square();
    for (auto& edge : source.segments) {
        if (edge.segment.start.y == 3.0) edge.segment.start.y = 2.0;
        if (edge.segment.end.y == 3.0) edge.segment.end.y = 2.0;
    }
    const auto rotated = rotate_boundary(source, {2, 1}, std::numbers::pi / 2);
    const auto flipped = flip_boundary(rotated, {2, 1}, BoundaryFlipAxis::vertical);
    const LegacyBoundaryIdentityOptions ids{{"a", "b", "c", "d"}, {"w", "x", "y", "z"}};
    const auto cloned = clone_boundary(flipped, "copy", ids, {1.2192, 0});
    auto minimum = cloned.segments.front().segment.start;
    auto maximum = minimum;
    for (const auto& edge : cloned.segments) {
        for (const auto point : {edge.segment.start, edge.segment.end}) {
            minimum.x = std::min(minimum.x, point.x);
            minimum.y = std::min(minimum.y, point.y);
            maximum.x = std::max(maximum.x, point.x);
            maximum.y = std::max(maximum.y, point.y);
        }
    }
    const Vec2 pivot{(minimum.x + maximum.x) / 2, (minimum.y + maximum.y) / 2};
    auto candidate = rotate_boundary(cloned, pivot, 35 * std::numbers::pi / 180);
    for (auto& edge : candidate.segments) {
        edge.segment.start.x += 2;
        edge.segment.end.x += 2;
    }
    require(validate_boundary(boundary_geometry(candidate)).empty(),
            "rotated and translated rectangle with overlapping opposite-edge bounds must validate");
    require(near(signed_area(boundary_geometry(candidate)), -8) &&
                near(perimeter(boundary_geometry(candidate)), 12),
            "composed rigid transforms must retain rectangle area and perimeter");
    require(decode_identified_boundary_entity(encode_identified_boundary_entity(candidate)) == candidate,
            "transformed rectangle must remain admissible as an identified boundary");
}

void test_direct_boundary_edits() {
    using namespace sketch;
    const auto source = square();
    auto expected = source;
    expected.segments[0].segment.end = {5, 1};
    expected.segments[1].segment.start = {5, 1};
    require(move_boundary_vertex(source, "v1", {5, 1}) == expected,
            "moving one vertex must update both incident edges and preserve all other data");
    expected = source;
    expected.segments[0].segment.start = {-1, 0};
    expected.segments[3].segment.end = {-1, 0};
    require(move_boundary_vertex(source, "v0", {-1, 0}) == expected,
            "moving the first vertex must update the closing edge");
    for (const auto fixed : {BoundaryFixedEndpoint::start, BoundaryFixedEndpoint::end}) {
        for (const auto connected : {false, true}) {
            expected = source;
            const auto fixed_id = fixed == BoundaryFixedEndpoint::start ? "v0" : "v1";
            const auto moving_id = fixed == BoundaryFixedEndpoint::start ? "v1" : "v0";
            const double delta = fixed == BoundaryFixedEndpoint::start ? 2 : -2;
            for (auto& edge : expected.segments) {
                if (edge.start_vertex_id == moving_id || (connected && edge.start_vertex_id != fixed_id))
                    edge.segment.start.x += delta;
                if (edge.end_vertex_id == moving_id || (connected && edge.end_vertex_id != fixed_id))
                    edge.segment.end.x += delta;
            }
            require(set_boundary_segment_length(source, "e0", 6, fixed, connected) == expected,
                    "length edits must honor fixed endpoint and connected translation mode");
        }
    }
    auto curved = source;
    curved.segments[0].segment.sweep_radians = std::numbers::pi / 2;
    expected = curved;
    expected.segments[0].segment.end = {5, 0};
    expected.segments[1].segment.start = {5, 0};
    require(move_boundary_vertex(curved, "v1", {5, 0}) == expected,
            "vertex edit must retain the analytical arc sweep");
    const double arc_target = 5 * std::numbers::pi / (2 * std::sqrt(2.0));
    const auto resized = set_boundary_segment_length(curved, "e0", arc_target,
                                                     BoundaryFixedEndpoint::start, false);
    require(near(resized.segments[0].segment.end.x, 5) &&
                near(segment_length(resized.segments[0].segment), arc_target) &&
                resized.segments[0].segment.sweep_radians == std::numbers::pi / 2,
            "curved target length must be arc length with unchanged sweep");
    const auto connected_curve = set_boundary_segment_length(curved, "e0", arc_target,
                                                              BoundaryFixedEndpoint::end, true);
    require(near(connected_curve.segments[0].segment.start.x, -1) &&
                connected_curve.segments[0].segment.end.x == 4 &&
                near(segment_length(connected_curve.segments[0].segment), arc_target) &&
                connected_curve.segments[0].segment.sweep_radians == std::numbers::pi / 2 &&
                near(connected_curve.segments[2].segment.start.x, 3) &&
                near(connected_curve.segments[2].segment.end.x, -1),
            "connected curved edits must anchor the end and translate the opposite chain");
    expected = source;
    expected.segments[0].segment.end = {4, 1};
    expected.segments[1].segment.start = {4, 1};
    require(set_boundary_segment_length(source, "e1", 2, BoundaryFixedEndpoint::end, false) == expected,
            "vertical segment shortening must move toward the fixed endpoint");
    const auto diagonal = move_boundary_vertex(source, "v1", {3, -1});
    const auto shortened = set_boundary_segment_length(diagonal, "e0", std::sqrt(10.0) / 2,
                                                       BoundaryFixedEndpoint::start, false);
    require(near(shortened.segments[0].segment.end.x, 1.5) &&
                near(shortened.segments[0].segment.end.y, -0.5),
            "diagonal resizing must retain chord direction");
    require(set_boundary_segment_length(curved, "e0", segment_length(curved.segments[0].segment),
                                        BoundaryFixedEndpoint::end, true) == curved,
            "unchanged target length must preserve exact geometry");
    require(source == square(), "direct edits must never mutate their source");
}

void test_direct_boundary_edit_rejections() {
    using namespace sketch;
    const auto source = square();
    rejects([&] { (void)move_boundary_vertex(source, "missing", {1, 1}); });
    for (const auto value : {std::numeric_limits<double>::quiet_NaN(),
                             std::numeric_limits<double>::infinity()}) {
        rejects([&] { (void)move_boundary_vertex(source, "v0", {value, 0}); });
        rejects([&] { (void)move_boundary_vertex(source, "v0", {0, value}); });
    }
    rejects([&] { (void)move_boundary_vertex(source, "v1", {0, 0}); });
    rejects([&] { (void)move_boundary_vertex(source, "v1", {-1, 2}); });
    rejects([&] { (void)set_boundary_segment_length(source, "missing", 5, BoundaryFixedEndpoint::start, false); });
    for (const auto value : {0.0, -1.0, std::numeric_limits<double>::quiet_NaN(),
                             std::numeric_limits<double>::infinity()}) {
        rejects([&] { (void)set_boundary_segment_length(source, "e0", value, BoundaryFixedEndpoint::start, false); });
    }
    rejects([&] { (void)set_boundary_segment_length(source, "e0", 5, static_cast<BoundaryFixedEndpoint>(99), false); });
    auto degenerate = source;
    degenerate.segments[0].segment.end = {0, 0};
    degenerate.segments[1].segment.start = {0, 0};
    const auto unchanged = degenerate;
    rejects([&] { (void)set_boundary_segment_length(degenerate, "e0", 5, BoundaryFixedEndpoint::start, false); });
    require(degenerate == unchanged && source == square(), "rejections must leave input unchanged");
    // A concave boundary whose extended bottom edge crosses its right-hand notch.
    const IdentifiedBoundary concave{"c", "boundary", {
        {"a", "a0", "a1", {{0, 0}, {2, 0}, 0}},
        {"b", "a1", "a2", {{2, 0}, {2, 2}, 0}},
        {"c", "a2", "a3", {{2, 2}, {4, -1}, 0}},
        {"d", "a3", "a4", {{4, -1}, {4, 4}, 0}},
        {"e", "a4", "a5", {{4, 4}, {0, 4}, 0}},
        {"f", "a5", "a0", {{0, 4}, {0, 0}, 0}}}};
    require(validate_boundary(boundary_geometry(concave)).empty());
    rejects([&] { (void)set_boundary_segment_length(concave, "a", 3.5, BoundaryFixedEndpoint::start, false); });
    rejects([&] { (void)set_boundary_segment_length(concave, "a", 4, BoundaryFixedEndpoint::end, true); });
}
}
int main() {
    try {
        test_direct_boundary_edits();
        test_direct_boundary_edit_rejections();
        test_rotated_boundary_transform_sequence();
        using namespace sketch;
        const auto source = square();
        const auto rotated = rotate_boundary(source, {1,1}, std::numbers::pi/2);
        require(near(rotated.segments[0].segment.start.x,2));
        require(near(signed_area(boundary_geometry(rotated)),12));
        require(source == square());
        const auto flipped = flip_boundary(source,{1,1},BoundaryFlipAxis::horizontal);
        require(near(signed_area(boundary_geometry(flipped)),-12));
        require(flipped.segments[0].segment_id == "e0");
        const auto inserted = insert_boundary_vertex(source,"e0",0.25,"vx","ex");
        require(inserted.segments.size()==5 && inserted.segments[0].segment.end.x==1);
        require(inserted.segments[1].start_vertex_id=="vx");
        require(near(signed_area(boundary_geometry(inserted)),12));
        rejects([&]{ (void)insert_boundary_vertex(source,"e0",0,"vx","ex"); });
        rejects([&]{ (void)insert_boundary_vertex(source,"e0",0.5,"v0","ex"); });
        rejects([&]{ (void)insert_boundary_vertex(source,"e0",0.5,"vx","e1"); });
        rejects([&]{ (void)insert_boundary_vertex(source,"missing",0.5,"vx","ex"); });
        rejects([&]{ (void)rotate_boundary(source,{0,0},std::numeric_limits<double>::infinity()); });
        auto curved=source;
        curved.segments[0].segment.sweep_radians=std::numbers::pi/2;
        const auto split=insert_boundary_vertex(curved,"e0",0.3,"vx","ex");
        require(near(perimeter(boundary_geometry(curved)),perimeter(boundary_geometry(split))));
        require(near(signed_area(boundary_geometry(curved)),signed_area(boundary_geometry(split))));
        require(flip_boundary(curved,{0,0},BoundaryFlipAxis::vertical).segments[0].segment.sweep_radians<0);
        const LegacyBoundaryIdentityOptions ids{{"a","b","c","d"},{"w","x","y","z"}};
        const auto cloned=clone_boundary(source,"copy",ids,{10,20});
        require(cloned.id=="copy" && cloned.segments[0].segment.start.x==10);
        require(near(signed_area(boundary_geometry(cloned)),12));
        rejects([&]{ (void)clone_boundary(source,"b",ids,{0,0}); });
        rejects([&]{ (void)clone_boundary(source,"copy",{}, {0,0}); });
        auto bad_ids=ids; bad_ids.vertex_ids[0]="v0";
        rejects([&]{ (void)clone_boundary(source,"copy",bad_ids,{0,0}); });
        require(jump_to_boundary_vertex(source,"v2").y==3);
        rejects([&]{ (void)jump_to_boundary_vertex(source,"missing"); });
        auto open=boundary_geometry(source); open.pop_back();
        require(automatically_close_boundary(open).size()==4);
        require(automatically_close_boundary(boundary_geometry(source)).size()==4);
        open[1].start.x+=0.00000001;
        rejects([&]{ (void)automatically_close_boundary(open); });
        rejects([&]{ (void)automatically_close_boundary({{{0,0},{1,0},0}}); });
        require(complete_bay_window({0,0},{1,-1},{3,-1},{4,0}).size()==3);
        rejects([&]{ (void)complete_bay_window({0,0},{3,-1},{1,-1},{4,0}); });
        const std::vector<Segment> unordered{
            {{4, 3}, {0, 3}, 0},
            {{0, 0}, {4, 0}, 0},
            {{0, 3}, {0, 0}, 0},
            {{4, 0}, {4, 3}, 0},
        };
        const auto assembled = assemble_boundary_from_segments(unordered, 1);
        require(assembled.size() == 4 && assembled.front().start.x == 0.0 &&
                    assembled.front().start.y == 0.0 && assembled.front().end.x == 4.0 &&
                    assembled.front().end.y == 0.0 && near(signed_area(assembled), 12.0),
                "unordered existing segments must assemble into one analytical cycle");
        const auto curved_segments = std::vector<Segment>{
            {{0, 0}, {2, 0}, 0},
            {{2, 0}, {2, 2}, 0},
            {{2, 2}, {0, 2}, 0},
            {{0, 2}, {0, 0}, std::numbers::pi / 2},
        };
        const auto curved_assembled = assemble_boundary_from_segments(curved_segments, 3);
        const auto retained_arc = std::any_of(curved_assembled.begin(), curved_assembled.end(),
                                               [](const Segment& segment) {
                                                   return segment.sweep_radians > 0.0;
                                               });
        require(curved_assembled.size() == 4 &&
                    near(perimeter(curved_assembled), perimeter(curved_segments)) &&
                    retained_arc,
                "existing curved segments must retain analytical sweep and length");
        auto open_segments = unordered;
        open_segments.pop_back();
        rejects([&]{ (void)assemble_boundary_from_segments(open_segments); });
        auto branched_segments = unordered;
        branched_segments.push_back({{4, 0}, {8, 0}, 0});
        rejects([&]{ (void)assemble_boundary_from_segments(branched_segments); });
        auto disconnected_segments = unordered;
        disconnected_segments.push_back({{10, 10}, {11, 10}, 0});
        rejects([&]{ (void)assemble_boundary_from_segments(disconnected_segments); });
        auto gapped_segments = unordered;
        gapped_segments[3].start.x += 0.001;
        rejects([&]{ (void)assemble_boundary_from_segments(gapped_segments); });
        rejects([&]{ (void)assemble_boundary_from_segments(unordered, unordered.size()); });
        const std::vector<Segment> adjacent_faces{
            {{0, 0}, {2, 0}, 0}, {{2, 0}, {4, 0}, 0}, {{4, 0}, {4, 2}, 0},
            {{4, 2}, {2, 2}, 0}, {{2, 2}, {0, 2}, 0}, {{0, 2}, {0, 0}, 0},
            {{2, 0}, {2, 2}, 0}, {{12, 12}, {13, 12}, 0}};
        const auto detected = detect_closed_boundaries(adjacent_faces);
        require(detected.size() == 2 && near(std::abs(signed_area(detected[0])), 4.0) &&
                    near(std::abs(signed_area(detected[1])), 4.0) &&
                    signed_area(detected[0]) > 0.0 && signed_area(detected[1]) > 0.0,
                "automatic face detection must find adjacent bounded areas deterministically");
        const auto reversed_faces = std::vector<Segment>{
            {{2, 2}, {2, 0}, 0}, {{2, 0}, {0, 0}, 0}, {{0, 0}, {0, 2}, 0},
            {{0, 2}, {2, 2}, 0}, {{2, 2}, {4, 2}, 0}, {{4, 2}, {4, 0}, 0},
            {{4, 0}, {2, 0}, 0}};
        const auto detected_reversed = detect_closed_boundaries(reversed_faces);
        require(detected_reversed.size() == detected.size() &&
                    near(std::abs(signed_area(detected_reversed[0])),
                         std::abs(signed_area(detected[0]))) &&
                    near(std::abs(signed_area(detected_reversed[1])),
                         std::abs(signed_area(detected[1]))) &&
                    signed_area(detected_reversed[0]) > 0.0 &&
                    signed_area(detected_reversed[1]) > 0.0,
                "automatic face detection must be independent of input segment order and winding");
        require(detect_closed_boundaries({{{0, 0}, {1, 0}, 0}}).empty(),
                "open geometry without a bounded face must produce no areas");
        require(apex_command_id("Auto Close")=="boundary.auto_close");
        require(apex_command_id("unknown").empty());
        require(shortcut_conflicts(apex_operation_preset()).empty());
        require(shortcut_conflicts({{"R","rotate"},{"R","flip"}})==std::vector<std::string>{"R"});
        std::cout << "geometry operations tests passed\n";
    } catch (const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
