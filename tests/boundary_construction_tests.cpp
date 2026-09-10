#include "sketch/boundary_construction.hpp"

#include "support/noninteractive_errors.hpp"

#include <cmath>
#include <functional>
#include <iostream>
#include <stdexcept>

namespace {

using sketch::AcceptedBoundaryChain;
using sketch::AngleInput;
using sketch::BoundaryAuthoringMode;
using sketch::BoundaryAuthoringOptions;
using sketch::BoundaryAuthoringSession;
using sketch::BoundaryConstructionKind;
using sketch::Quantity;
using sketch::Vec2;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

Quantity q(const char* expression) {
    return sketch::parse_quantity(expression);
}

AngleInput angle(const char* expression) {
    return sketch::parse_angle(expression);
}

void expect_invalid(const std::function<void()>& operation, const char* message) {
    bool rejected = false;
    try {
        operation();
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    require(rejected, message);
}

AcceptedBoundaryChain close_drawn_chain(
    const std::function<void(BoundaryAuthoringSession&)>& construction) {
    BoundaryAuthoringSession session(BoundaryAuthoringMode::draw_first);
    (void)session.anchor({0.0, 0.0});
    construction(session);
    // Keep the fixture a simple cycle even when the constructor under test
    // itself is a straight edge; a two-edge out-and-back is overlapping. An
    // analytical arc can close directly along its chord without overlap.
    const auto final_sweep = session.view().active_chain->segments.back().segment.sweep_radians;
    if (final_sweep == 0.0) (void)session.add_line(q("1 m"), angle("90 deg"));
    (void)session.add_closing_segment();
    return session.close_chain();
}

void require_replay_equal(const AcceptedBoundaryChain& accepted,
                          const BoundaryAuthoringOptions& options = {}) {
    const auto replay = sketch::replay_accepted_chain(accepted, options);
    require(replay.anchor.x == accepted.anchor.x && replay.anchor.y == accepted.anchor.y,
            "replay must retain the captured anchor");
    require(replay.boundary == accepted.boundary,
            "replay must reconstruct the displayed boundary exactly");
    require(replay.classification == accepted.classification &&
                replay.dimensions == accepted.dimensions &&
                replay.receipts == accepted.receipts && replay.classified == accepted.classified,
            "replay must retain verified semantic data");
    sketch::verify_accepted_chain(accepted, options);
}

void test_every_construction_constructor_replays() {
    const auto heading = close_drawn_chain([](BoundaryAuthoringSession& session) {
        (void)session.add_line(q("1 m"), angle("0 deg"));
    });
    require(heading.receipts.front().kind == BoundaryConstructionKind::line_heading,
            "heading receipt kind");
    require(heading.receipts.front().start.x == 0.0 && heading.receipts.front().start.y == 0.0,
            "heading receipt start context");
    require_replay_equal(heading);
    const auto heading_envelope = sketch::boundary_construction_envelope(heading);
    require(heading_envelope.at("version") == 2,
            "current accepted-chain envelopes must use schema version two");
    const auto decoded_heading = sketch::decode_boundary_receipt_envelope(heading_envelope);
    require(decoded_heading.supported() && decoded_heading.record.has_value() &&
                decoded_heading.record->boundary_id == heading.boundary.id,
            "accepted chain adapter must produce a strict lower-layer envelope");

    const auto rise_run = close_drawn_chain([](BoundaryAuthoringSession& session) {
        (void)session.add_line_rise_run(q("0 m"), q("1 m"));
    });
    require(rise_run.receipts.front().kind == BoundaryConstructionKind::line_rise_run,
            "rise/run receipt kind");
    require_replay_equal(rise_run);

    const auto relative = close_drawn_chain([](BoundaryAuthoringSession& session) {
        (void)session.add_line(q("1 m"), angle("0 deg"));
        (void)session.add_line_relative_turn(q("1 m"), angle("90 deg"));
    });
    require(relative.receipts[1].kind == BoundaryConstructionKind::line_relative_turn,
            "relative-turn receipt kind");
    require_replay_equal(relative);

    const auto chord_angle = close_drawn_chain([](BoundaryAuthoringSession& session) {
        (void)session.add_arc_chord_angle({1.0, 0.0}, angle("90 deg"));
    });
    require(chord_angle.receipts.front().kind == BoundaryConstructionKind::arc_chord_angle &&
                chord_angle.receipts.front().chord_end.has_value() &&
                chord_angle.receipts.front().angle.has_value() &&
                !chord_angle.receipts.front().sweep.has_value(),
            "chord-angle receipt must retain one explicit chord endpoint and angle input");
    require_replay_equal(chord_angle);

    const auto chord_height = close_drawn_chain([](BoundaryAuthoringSession& session) {
        (void)session.add_arc_chord_height({1.0, 0.0}, q("0.25 m"));
    });
    require(chord_height.receipts.front().kind == BoundaryConstructionKind::arc_chord_height &&
                chord_height.receipts.front().chord_end.has_value(),
            "chord-height receipt endpoint");
    require_replay_equal(chord_height);

    const auto chord_length = close_drawn_chain([](BoundaryAuthoringSession& session) {
        (void)session.add_arc_chord_arc_length({1.0, 0.0}, q("1.5 m"), true);
    });
    require(chord_length.receipts.front().kind == BoundaryConstructionKind::arc_chord_length &&
                chord_length.receipts.front().clockwise,
            "chord-length receipt direction");
    require_replay_equal(chord_length);

    const auto start_tangent = close_drawn_chain([](BoundaryAuthoringSession& session) {
        (void)session.add_arc_start_tangent(angle("0 deg"), q("1 m"), angle("90 deg"));
    });
    require(start_tangent.receipts.front().kind == BoundaryConstructionKind::arc_start_tangent &&
                start_tangent.receipts.front().tangent.has_value() &&
                start_tangent.receipts.front().sweep.has_value(),
            "start-tangent receipt inputs");
    require_replay_equal(start_tangent);

    const auto closure = heading;
    require(closure.receipts.back().kind == BoundaryConstructionKind::line_closure &&
                closure.receipts.back().closure_delta.has_value(),
            "closing edge must have a closure receipt");
}

void test_dimensions_are_verified_against_replayed_geometry() {
    BoundaryAuthoringOptions automatic_options;
    automatic_options.automatic_dimension_placement = true;
    const auto automatic = [&] {
        BoundaryAuthoringSession session(BoundaryAuthoringMode::draw_first, automatic_options);
        (void)session.anchor({0.0, 0.0});
        (void)session.add_line(q("1 m"), angle("0 deg"));
        (void)session.add_line(q("1 m"), angle("90 deg"));
        (void)session.add_closing_segment();
        const auto accepted = session.close_chain();
        require(accepted.dimensions.size() == accepted.boundary.segments.size(),
                "automatic dimensions must cover each edge");
        require_replay_equal(accepted, automatic_options);
        return accepted;
    }();

    auto moved_automatic = automatic;
    moved_automatic.dimensions.front().text_position.x += 0.01;
    expect_invalid([&] { (void)sketch::replay_accepted_chain(moved_automatic, automatic_options); },
                   "tampered automatic dimension position must be rejected");

    BoundaryAuthoringSession defined(BoundaryAuthoringMode::define_first);
    defined.set_classification("living_area");
    (void)defined.anchor({0.0, 0.0});
    (void)defined.add_line(q("1 m"), angle("0 deg"));
    (void)defined.place_manual_dimension({0.5, 0.2});
    (void)defined.add_line(q("1 m"), angle("90 deg"));
    (void)defined.place_manual_dimension({1.2, 0.5});
    (void)defined.add_closing_segment();
    (void)defined.place_manual_dimension({0.0, 0.5});
    const auto manual = defined.close_chain();
    require_replay_equal(manual);

    auto bad_manual = manual;
    bad_manual.dimensions.front().automatic_placement_version = 1;
    expect_invalid([&] { (void)sketch::replay_accepted_chain(bad_manual); },
                   "manual dimension with automatic metadata must be rejected");
}

void test_receipt_tampering_and_invalid_context_are_atomic_to_replay() {
    const auto source = close_drawn_chain([](BoundaryAuthoringSession& session) {
        (void)session.add_arc_chord_angle({1.0, 0.0}, angle("90 deg"));
    });
    require_replay_equal(source);

    auto bad_geometry = source;
    bad_geometry.boundary.segments.front().segment.end.y += 0.01;
    expect_invalid([&] { sketch::verify_accepted_chain(bad_geometry); },
                   "displayed geometry tampering must be rejected");

    auto bad_angle = source;
    bad_angle.receipts.front().angle->radians += 0.01;
    expect_invalid([&] { (void)sketch::replay_accepted_chain(bad_angle); },
                   "receipt angle tampering must be rejected");

    const auto line_source = close_drawn_chain([](BoundaryAuthoringSession& session) {
        (void)session.add_line(q("1 m"), angle("0 deg"));
    });
    auto bad_quantity = line_source;
    bad_quantity.receipts.front().distance->metres += 0.01;
    expect_invalid([&] { (void)sketch::replay_accepted_chain(bad_quantity); },
                   "receipt quantity tampering must be rejected");

    auto extraneous = source;
    extraneous.receipts.front().sweep = angle("90 deg");
    expect_invalid([&] { (void)sketch::replay_accepted_chain(extraneous); },
                   "extraneous receipt fields must be rejected");

    auto missing_endpoint = source;
    missing_endpoint.receipts.front().chord_end.reset();
    expect_invalid([&] { (void)sketch::replay_accepted_chain(missing_endpoint); },
                   "missing chord context must be rejected");

    auto bad_closure = source;
    bad_closure.receipts.back().closure_delta->x += 0.01;
    expect_invalid([&] { (void)sketch::replay_accepted_chain(bad_closure); },
                   "tampered closure receipt must be rejected");

    auto wrong_anchor = source;
    wrong_anchor.anchor.x = 0.5;
    expect_invalid([&] { (void)sketch::replay_accepted_chain(wrong_anchor); },
                   "wrong captured anchor must be rejected");

    auto wrong_receipt_id = source;
    wrong_receipt_id.receipts.front().segment_id = "other-segment";
    expect_invalid([&] { (void)sketch::replay_accepted_chain(wrong_receipt_id); },
                   "receipt identity mismatch must be rejected");

    auto missing_receipt = source;
    missing_receipt.receipts.pop_back();
    expect_invalid([&] { (void)sketch::replay_accepted_chain(missing_receipt); },
                   "receipt count mismatch must be rejected");

    auto unsupported_type = source;
    unsupported_type.boundary.type = "unsupported_boundary";
    expect_invalid([&] { (void)sketch::replay_accepted_chain(unsupported_type); },
                   "unsupported boundary type must be rejected");

    auto unsupported_option = BoundaryAuthoringOptions{};
    unsupported_option.default_boundary_type = "unsupported_boundary";
    expect_invalid([&] { (void)sketch::replay_accepted_chain(source, unsupported_option); },
                   "unsupported replay boundary context must be rejected");

    auto mismatched_option = BoundaryAuthoringOptions{};
    mismatched_option.default_boundary_type = "room_boundary";
    expect_invalid([&] { (void)sketch::replay_accepted_chain(source, mismatched_option); },
                   "mismatched replay boundary context must be rejected");
}

void test_copy_and_local_history_preserve_replay_identity() {
    BoundaryAuthoringSession original(BoundaryAuthoringMode::draw_first);
    (void)original.anchor({0.0, 0.0});
    const auto first_id = original.add_line(q("1 m"), angle("0 deg"));
    auto copied = original;
    require(copied.view().active_chain->segments.front().segment_id == first_id,
            "session copies must retain segment IDs");
    require(copied.undo() && copied.redo(), "copy local undo/redo must work");
    require(copied.view().active_chain->segments.front().segment_id == first_id,
            "undo/redo must restore the same segment ID");
    (void)copied.add_line(q("1 m"), angle("90 deg"));
    (void)copied.add_closing_segment();
    const auto accepted = copied.close_chain();
    require_replay_equal(accepted);
}

void test_exact_cardinal_heading_rectangle_and_near_cardinal_input() {
    BoundaryAuthoringSession cardinal(BoundaryAuthoringMode::draw_first);
    (void)cardinal.anchor({0.0, 0.0});
    (void)cardinal.add_line(q("1 m"), angle("0 deg"));
    (void)cardinal.add_line(q("1 m"), angle("90 deg"));
    (void)cardinal.add_line(q("1 m"), angle("180 deg"));
    (void)cardinal.add_line(q("1 m"), angle("270 deg"));
    const auto before_close = cardinal.view();
    require(before_close.active_chain->segments.size() == 4,
            "cardinal rectangle must not need an artificial closure edge");
    require(before_close.active_chain->segments[0].segment.end.x == 1.0 &&
                before_close.active_chain->segments[0].segment.end.y == 0.0 &&
                before_close.active_chain->segments[1].segment.end.x == 1.0 &&
                before_close.active_chain->segments[1].segment.end.y == 1.0 &&
                before_close.active_chain->segments[2].segment.end.x == 0.0 &&
                before_close.active_chain->segments[2].segment.end.y == 1.0 &&
                before_close.active_chain->segments[3].segment.end.x == 0.0 &&
                before_close.active_chain->segments[3].segment.end.y == 0.0,
            "exact cardinal headings must produce exact axis endpoints");
    const auto accepted = cardinal.close_chain();
    require_replay_equal(accepted);

    BoundaryAuthoringSession near_cardinal(BoundaryAuthoringMode::draw_first);
    (void)near_cardinal.anchor({0.0, 0.0});
    (void)near_cardinal.add_line(q("1 m"), angle("90.001 deg"));
    const auto endpoint = near_cardinal.view().active_chain->segments.front().segment.end;
    require(std::abs(endpoint.x) > 1e-8 && endpoint.y < 1.0,
            "near-cardinal heading must retain trigonometric geometry");

    BoundaryAuthoringSession exact_receipt(BoundaryAuthoringMode::draw_first);
    (void)exact_receipt.anchor({0.0, 0.0});
    const auto before_bad_angle = exact_receipt.view();
    const AngleInput sub_tolerance_mismatch{0.0, "0.0000000000001 rad", "0"};
    expect_invalid(
        [&] { (void)exact_receipt.add_line(q("1 m"), sub_tolerance_mismatch); },
        "an exact receipt must reject a sub-tolerance original-angle mismatch");
    require(exact_receipt.view() == before_bad_angle,
            "rejected exact-angle mismatch must leave session state unchanged");
}

void test_point_native_line_workflow_in_both_modes_and_local_history() {
    const Vec2 points[]{{123456789.125, -987654321.75},
                        {123456792.625, -987654321.75},
                        {123456792.625, -987654318.25},
                        {123456789.125, -987654318.25}};

    BoundaryAuthoringSession draw(BoundaryAuthoringMode::draw_first);
    (void)draw.anchor(points[0]);
    const auto first_id = draw.add_line_to(points[1]);
    const auto first_view = draw.view();
    require(first_view.active_chain->receipts.front().kind ==
                BoundaryConstructionKind::line_to_point &&
                first_view.active_chain->receipts.front().chord_end.has_value() &&
                !first_view.active_chain->receipts.front().distance.has_value() &&
                !first_view.active_chain->receipts.front().heading.has_value() &&
                first_view.active_chain->segments.front().segment.end.x == points[1].x &&
                first_view.active_chain->segments.front().segment.end.y == points[1].y,
            "Draw First point-native line must retain only its exact endpoint");
    require(draw.undo() && draw.redo() &&
                draw.view().active_chain->segments.front().segment_id == first_id,
            "point-native line must retain its identity through local undo/redo");
    (void)draw.add_line_to(points[2]);
    (void)draw.add_line_to(points[3]);
    (void)draw.add_line_to(points[0]);
    const auto draw_accepted = draw.close_chain();
    require(draw_accepted.boundary.segments.back().segment.end.x == points[0].x &&
                draw_accepted.boundary.segments.back().segment.end.y == points[0].y,
            "Draw First point-native line must close at the captured anchor exactly");
    require_replay_equal(draw_accepted);
    const auto draw_envelope = sketch::boundary_construction_envelope(draw_accepted);
    require(draw_envelope.at("version") == 2 &&
                draw_envelope.at("segments").at(0).at("receipt").at("kind") ==
                    "line_to_point",
            "point-native accepted chains must emit schema two receipts");

    BoundaryAuthoringSession define(BoundaryAuthoringMode::define_first);
    define.set_classification("living_area");
    (void)define.anchor(points[0]);
    const auto define_id = define.add_line_to(points[1]);
    require(define.pending_dimension().has_value() &&
                define.pending_dimension()->segment_id == define_id,
            "Define First point-native line must enter its dimension phase");
    (void)define.place_manual_dimension({123456791.0, -987654322.0});
    (void)define.add_line_to(points[2]);
    (void)define.place_manual_dimension({123456794.0, -987654320.0});
    (void)define.add_line_to(points[3]);
    (void)define.place_manual_dimension({123456791.0, -987654317.0});
    (void)define.add_line_to(points[0]);
    (void)define.place_manual_dimension({123456787.0, -987654320.0});
    const auto define_accepted = define.close_chain();
    require(define_accepted.dimensions.size() == 4 &&
                define_accepted.receipts.front().kind == BoundaryConstructionKind::line_to_point,
            "Define First point-native lines must resolve one manual dimension per edge");
    require_replay_equal(define_accepted);
}

}  // namespace

int main() {
    sketch::testing::noninteractive_errors();
    try {
        test_every_construction_constructor_replays();
        test_dimensions_are_verified_against_replayed_geometry();
        test_receipt_tampering_and_invalid_context_are_atomic_to_replay();
        test_copy_and_local_history_preserve_replay_identity();
        test_exact_cardinal_heading_rectangle_and_near_cardinal_input();
        test_point_native_line_workflow_in_both_modes_and_local_history();
        std::cout << "Boundary construction replay tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
