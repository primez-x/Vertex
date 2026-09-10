#include "sketch/boundary_authoring_session.hpp"

#include "support/noninteractive_errors.hpp"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <numbers>
#include <stdexcept>
#include <string_view>

namespace {

using namespace sketch;

[[noreturn]] void fail(std::string_view message) {
    std::cerr << "boundary_authoring_session_tests: " << message << '\n';
    std::exit(1);
}

void require(bool condition, std::string_view message) {
    if (!condition) fail(message);
}

void require_near(double actual, double expected, double tolerance, std::string_view message) {
    if (!std::isfinite(actual) || std::abs(actual - expected) > tolerance) fail(message);
}

template <typename Function>
void rejected(Function&& function, std::string_view message) {
    try {
        function();
    } catch (const std::invalid_argument&) {
        return;
    }
    fail(message);
}

Quantity q(std::string_view expression) {
    return parse_quantity(expression, Unit::metre);
}

AngleInput angle(std::string_view expression) {
    return parse_angle(expression);
}

void add_rectangle(BoundaryAuthoringSession& session, bool place_dimensions) {
    (void)session.add_line_rise_run(q("0 m"), q("4 m"));
    if (place_dimensions) (void)session.place_manual_dimension({2.0, 0.25});
    (void)session.add_line_rise_run(q("3 m"), q("0 m"));
    if (place_dimensions) (void)session.place_manual_dimension({3.75, 1.5});
    (void)session.add_line_rise_run(q("0 m"), q("-4 m"));
    if (place_dimensions) (void)session.place_manual_dimension({2.0, 2.75});
    (void)session.add_line_rise_run(q("-3 m"), q("0 m"));
    if (place_dimensions) (void)session.place_manual_dimension({0.25, 1.5});
}

void test_define_first_classification_and_dimension_phase() {
    BoundaryAuthoringSession session(BoundaryAuthoringMode::define_first);
    require(session.phase() == BoundaryAuthoringPhase::awaiting_classification,
            "Define First must begin by awaiting classification");
    const auto before = session.view();
    rejected([&] { (void)session.anchor({0.0, 0.0}); },
             "Define First must reject an anchor before classification");
    require(session.view() == before, "rejected Define First anchor mutated state");

    session.set_classification("living_area");
    (void)session.anchor({0.0, 0.0});
    add_rectangle(session, true);
    require(session.phase() == BoundaryAuthoringPhase::drawing,
            "manual dimension placement must release Define First drawing phase");
    require(!session.view().pending_dimension.has_value(),
            "all manually placed dimensions should clear the pending edge");
    const auto accepted = session.close_chain();
    require(accepted.boundary.type == "measurement_boundary" &&
                accepted.classification == "living_area" && accepted.boundary.segments.size() == 4,
            "Define First must seal the classified analytical cycle");
    require(accepted.dimensions.size() == 4, "Define First must retain each edge dimension");
    for (std::size_t index = 0; index < accepted.dimensions.size(); ++index) {
        require(accepted.dimensions[index].boundary_id == accepted.boundary.id,
                "dimension must bind the sealed boundary identity");
        require(accepted.dimensions[index].segment_id == accepted.boundary.segments[index].segment_id,
                "dimension must bind its stable edge identity");
    }
    require(validate_boundary(boundary_geometry(accepted.boundary)).empty(),
            "sealed Define First geometry must be a valid analytical cycle");
}

void test_automatic_dimension_placement_is_deterministic() {
    BoundaryAuthoringOptions options;
    options.automatic_dimension_placement = true;
    BoundaryAuthoringSession first(BoundaryAuthoringMode::define_first, options);
    BoundaryAuthoringSession second(BoundaryAuthoringMode::define_first, options);
    first.set_classification("living_area");
    second.set_classification("living_area");
    (void)first.anchor({0.0, 0.0});
    (void)second.anchor({0.0, 0.0});
    (void)first.add_line(q("4 m"), angle("0 rad"));
    (void)second.add_line(q("4 m"), angle("0 rad"));
    const auto first_view = first.view();
    const auto second_view = second.view();
    require(!first_view.pending_dimension.has_value() &&
                first_view.active_chain->dimensions.size() == 1,
            "automatic placement must complete the edge dimension phase");
    require(first_view.active_chain->dimensions.front().placement ==
                BoundaryDimensionPlacement::automatic &&
                first_view.active_chain->dimensions.front().automatic_placement_version ==
                    std::uint32_t{1},
            "automatic placement must use version one semantics");
    require(first_view.active_chain->dimensions.front().text_position.x ==
                second_view.active_chain->dimensions.front().text_position.x &&
                first_view.active_chain->dimensions.front().text_position.y ==
                second_view.active_chain->dimensions.front().text_position.y,
            "automatic dimension position must be deterministic");
}

void test_draw_first_classifies_after_measured_linework_and_supports_pen_up() {
    BoundaryAuthoringOptions options;
    options.automatic_dimension_placement = true;
    BoundaryAuthoringSession session(BoundaryAuthoringMode::draw_first, options);
    require(session.phase() == BoundaryAuthoringPhase::awaiting_anchor,
            "Draw First must begin by awaiting an anchor");
    (void)session.anchor({0.0, 0.0});
    add_rectangle(session, false);
    const auto provisional = session.close_chain();
    require(provisional.boundary.type == "measurement_boundary",
            "unclassified Draw First closure must retain a recognized provisional type");
    session.classify_last_chain("living_area");
    require(session.accepted_chains().back().classification == "living_area" &&
                session.accepted_chains().back().boundary.type == "measurement_boundary",
            "Draw First classification must be able to follow measured closure");

    session.pen_up();
    session.set_pointer({100.0, 100.0});
    (void)session.anchor({10.0, 10.0});
    (void)session.add_line_rise_run(q("0 m"), q("1 m"));
    (void)session.add_line_rise_run(q("1 m"), q("0 m"));
    (void)session.add_line_rise_run(q("0 m"), q("-1 m"));
    (void)session.add_line_rise_run(q("-1 m"), q("0 m"));
    session.classify_current_chain("porch");
    (void)session.close_chain();

    const auto accepted = session.accepted_chains();
    require(accepted.size() == 2, "pen-up relocation must retain multiple independent chains");
    require(accepted[0].boundary.segments.front().segment.start.x == 0.0 &&
                accepted[1].boundary.segments.front().segment.start.x == 10.0,
            "pen-up relocation must not join chains through pointer coordinates");
}

void test_line_inputs_and_exact_receipts() {
    BoundaryAuthoringSession session(BoundaryAuthoringMode::draw_first);
    (void)session.anchor({0.0, 0.0});
    (void)session.add_line(q("1/3 ft"), angle("90 deg"));
    (void)session.add_line_rise_run(q("1/2 m"), q("1/4 m"));
    (void)session.add_line_relative_turn(q("1 m"), angle("90 deg"));
    const auto view = session.view();
    require(view.active_chain->receipts.size() == 3, "each line must retain one construction receipt");
    require(view.active_chain->pending_dimensions.empty(),
            "Draw First measured linework should not require Define First dimensions");
    require(view.active_chain->receipts[0].kind == BoundaryConstructionKind::line_heading &&
                view.active_chain->receipts[0].distance->original_expression == "1/3 ft" &&
                view.active_chain->receipts[0].distance->exact_metres == ExactRational{127, 1250} &&
                view.active_chain->receipts[0].heading->original_expression == "90 deg",
            "heading line must preserve normalized exact quantity and angle input");
    require(view.active_chain->receipts[1].kind == BoundaryConstructionKind::line_rise_run &&
                view.active_chain->receipts[1].rise->exact_metres == ExactRational{1, 2} &&
                view.active_chain->receipts[1].run->exact_metres == ExactRational{1, 4},
            "rise/run line must preserve exact quantities");
    require(view.active_chain->receipts[2].kind == BoundaryConstructionKind::line_relative_turn &&
                view.active_chain->receipts[2].turn->original_expression == "90 deg",
            "relative turn line must preserve its angle receipt");
    require_near(view.active_chain->segments[0].segment.end.x, 0.0, 1e-12,
         "heading line x coordinate");
    require_near(view.active_chain->segments[0].segment.end.y, 0.1016, 1e-12,
         "heading line y coordinate");
}

void test_all_analytic_arc_forms_retain_independent_geometry() {
    {
        BoundaryAuthoringSession session(BoundaryAuthoringMode::draw_first);
        (void)session.anchor({-1.0, 0.0});
        (void)session.add_arc_chord_angle({1.0, 0.0}, angle("180 deg"));
        const auto state = session.view();
        const auto& arc = state.active_chain->segments.front().segment;
        require_near(arc.end.x, 1.0, 1e-12, "chord angle arc endpoint x");
        require_near(arc.end.y, 0.0, 1e-12, "chord angle arc endpoint y");
        require_near(segment_length(arc), std::numbers::pi, 1e-12, "chord angle arc length");
    }
    {
        BoundaryAuthoringSession session(BoundaryAuthoringMode::draw_first);
        (void)session.anchor({-1.0, 0.0});
        (void)session.add_arc_chord_height({1.0, 0.0}, q("1 m"));
        const auto state = session.view();
        const auto& arc = state.active_chain->segments.front().segment;
        require_near(arc.sweep_radians, std::numbers::pi, 1e-12, "chord height arc sweep");
        require_near(segment_length(arc), std::numbers::pi, 1e-12, "chord height arc length");
    }
    {
        BoundaryAuthoringSession session(BoundaryAuthoringMode::draw_first);
        (void)session.anchor({1.0, 0.0});
        (void)session.add_arc_chord_arc_length({0.0, 1.0}, q("1.5707963267948966 m"));
        const auto state = session.view();
        const auto& arc = state.active_chain->segments.front().segment;
        require_near(arc.end.x, 0.0, 1e-12, "chord length arc endpoint x");
        require_near(arc.end.y, 1.0, 1e-12, "chord length arc endpoint y");
        require_near(segment_length(arc), std::numbers::pi / 2.0, 1e-12,
             "chord length arc measured length");
    }
    {
        BoundaryAuthoringSession session(BoundaryAuthoringMode::draw_first);
        (void)session.anchor({1.0, 0.0});
        (void)session.add_arc_start_tangent(angle("90 deg"), q("1.5707963267948966 m"),
                                             angle("90 deg"));
        const auto state = session.view();
        const auto& arc = state.active_chain->segments.front().segment;
        require_near(arc.end.x, 0.0, 1e-12, "tangent arc endpoint x");
        require_near(arc.end.y, 1.0, 1e-12, "tangent arc endpoint y");
        require_near(segment_length(arc), std::numbers::pi / 2.0, 1e-12,
             "tangent arc measured length");
    }
}

void test_invalid_operations_are_atomic_and_semantic_history_excludes_pointer() {
    BoundaryAuthoringSession session(BoundaryAuthoringMode::draw_first);
    session.set_pointer({4.0, 5.0});
    require(!session.can_undo(), "pointer movement must not enter semantic undo history");
    (void)session.anchor({0.0, 0.0});
    (void)session.add_line(q("1 m"), angle("0 rad"));
    const auto before = session.view();
    rejected([&] { (void)session.add_arc_chord_angle({1.0, 0.0}, angle("0 rad")); },
             "unsupported zero-sweep arc must reject");
    require(session.view() == before, "rejected arc mutated the authoring state");
    session.set_pointer({9.0, 9.0});
    require(session.can_undo(), "semantic actions must remain undoable after pointer motion");
    const auto segment_id = before.active_chain->segments.front().segment_id;
    require(session.undo(), "semantic undo should remove the measured line");
    require(session.view().active_chain->segments.empty(), "undo did not restore prior geometry");
    require(session.view().pointer.has_value() && session.view().pointer->x == 9.0 &&
                session.view().pointer->y == 9.0,
            "undo must not rewind the ephemeral pointer");
    require(session.redo(), "semantic redo should restore the measured line");
    require(session.view().active_chain->segments.front().segment_id == segment_id,
            "redo must retain the original stable segment identity");

    require(session.undo(), "undo should open a branch point for the replacement line");
    const auto replacement_id = session.add_line(q("2 m"), angle("0 rad"));
    require(replacement_id != segment_id && !session.can_redo(),
            "a new semantic action must discard redo while retaining monotonic identities");

    const auto before_open_close = session.view();
    rejected([&] { (void)session.close_chain(); }, "open chain closure must reject");
    require(session.view() == before_open_close, "failed closure mutated state");
    session.cancel();
    require(session.phase() == BoundaryAuthoringPhase::cancelled &&
                session.accepted_chains().empty(),
            "cancellation must discard unfinished authoring atomically");
    session.reset();
    require(session.phase() == BoundaryAuthoringPhase::awaiting_anchor,
            "reset must return Draw First to the initial anchor phase");
}

void test_explicit_closure_dimension_and_session_identity_lifetime() {
    BoundaryAuthoringSession session(BoundaryAuthoringMode::define_first);
    const auto namespace_before = session.view().identity_namespace;
    session.set_classification("living_area");
    (void)session.anchor({0.0, 0.0});
    const auto first_segment_id = session.add_line_rise_run(q("0 m"), q("2 m"));
    (void)session.place_manual_dimension({1.0, -0.25});
    (void)session.add_line_rise_run(q("2 m"), q("0 m"));
    (void)session.place_manual_dimension({2.25, 1.0});

    const auto open_before = session.view();
    rejected([&] { (void)session.close_chain(); },
             "an open chain must reject sealing before explicit closure");
    require(session.view() == open_before, "open-chain rejection mutated the session");

    const auto closing_segment_id = session.add_closing_segment();
    const auto pending = session.pending_dimension();
    require(session.phase() == BoundaryAuthoringPhase::awaiting_dimension && pending.has_value() &&
                pending->segment_id == closing_segment_id,
            "explicit closure must create a real edge with its own dimension phase");
    const auto after_closing = session.view();
    require(after_closing.active_chain->receipts.back().kind ==
                BoundaryConstructionKind::line_closure,
            "explicit closure must retain a line-closure receipt");
    require(after_closing.active_chain->segments.back().end_vertex_id ==
                after_closing.active_chain->segments.front().start_vertex_id,
            "explicit closure must reuse the first vertex identity at append time");
    (void)session.place_manual_dimension({1.0, 1.0});
    auto replay_copy = session;
    const auto recomputed = replay_copy.close_chain();
    const auto accepted = session.close_chain();
    require(accepted == recomputed,
            "copying a normalized session must reproduce the same sealed chain and IDs");
    require(accepted.boundary.segments.size() == 3 && accepted.dimensions.size() == 3 &&
                accepted.boundary.segments.front().segment_id == first_segment_id,
            "explicit closure must seal the complete analytical cycle and dimensions");
    const auto boundary_namespace_start = accepted.boundary.id.find('-') + 1;
    const auto boundary_namespace_end = accepted.boundary.id.rfind('-');
    require(boundary_namespace_start < boundary_namespace_end &&
                namespace_before == accepted.boundary.id.substr(
                    boundary_namespace_start, boundary_namespace_end - boundary_namespace_start),
            "boundary identity must retain the session namespace");

    BoundaryAuthoringSession independent(BoundaryAuthoringMode::draw_first);
    require(independent.view().identity_namespace != namespace_before,
            "independent sessions must not share an identity namespace");
    (void)independent.anchor({10.0, 10.0});
    const auto independent_segment_id =
        independent.add_line_rise_run(q("0 m"), q("2 m"));
    require(independent_segment_id != first_segment_id,
            "independent sessions must not emit colliding segment identities");

    const auto retained_id = accepted.boundary.segments.back().segment_id;
    session.reset();
    session.set_classification("garage");
    (void)session.anchor({20.0, 20.0});
    const auto reset_segment_id = session.add_line_rise_run(q("0 m"), q("1 m"));
    require(reset_segment_id != retained_id && session.view().identity_namespace == namespace_before,
            "reset must retain the namespace while advancing retired identity counters");
}

void test_malformed_exact_quantity_rejects_without_state_change() {
    BoundaryAuthoringSession session(BoundaryAuthoringMode::draw_first);
    (void)session.anchor({0.0, 0.0});
    const auto before = session.view();
    auto malformed = q("1 m");
    malformed.metres = 2.0;
    rejected([&] { (void)session.add_line(malformed, angle("0 rad")); },
             "an internally inconsistent exact quantity must reject");
    require(session.view() == before, "malformed exact quantity mutated session state");
    auto malformed_angle = angle("0 rad");
    malformed_angle.normalized_expression = "not-an-angle";
    rejected([&] { (void)session.add_line(q("1 m"), malformed_angle); },
             "a malformed normalized angle expression must reject");
    require(session.view() == before, "malformed exact angle mutated session state");
}

void test_strict_close_rejects_self_intersection_and_near_join() {
    BoundaryAuthoringSession session(BoundaryAuthoringMode::draw_first);
    (void)session.anchor({0.0, 0.0});
    (void)session.add_line_rise_run(q("0 m"), q("4 m"));
    (void)session.add_line_rise_run(q("4 m"), q("-4 m"));
    (void)session.add_line_rise_run(q("0 m"), q("4 m"));
    (void)session.add_line_rise_run(q("-4 m"), q("-3.99999999 m"));
    const auto before = session.view();
    rejected([&] { (void)session.close_chain(); },
             "closure must reject a non-exact final join and self-intersection");
    require(session.view() == before, "invalid strict closure mutated state");
}

}  // namespace

int main() {
    sketch::testing::noninteractive_errors();
    try {
        test_define_first_classification_and_dimension_phase();
        test_automatic_dimension_placement_is_deterministic();
        test_draw_first_classifies_after_measured_linework_and_supports_pen_up();
        test_line_inputs_and_exact_receipts();
        test_all_analytic_arc_forms_retain_independent_geometry();
        test_invalid_operations_are_atomic_and_semantic_history_excludes_pointer();
        test_explicit_closure_dimension_and_session_identity_lifetime();
        test_malformed_exact_quantity_rejects_without_state_change();
        test_strict_close_rejects_self_intersection_and_near_join();
        std::cout << "Boundary authoring session tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
