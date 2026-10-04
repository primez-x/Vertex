#include "sketch/boundary_authoring_recovery.hpp"

#include "support/noninteractive_errors.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <initializer_list>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace sketch;

[[noreturn]] void fail(std::string_view message) {
    std::cerr << "boundary_authoring_recovery_tests: " << message << '\n';
    std::exit(1);
}

void require(bool condition, std::string_view message) {
    if (!condition) fail(message);
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

template <typename Function>
void throws_any(Function&& function, std::string_view message) {
    try {
        function();
    } catch (const std::exception&) {
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

bool has_kind(const BoundaryAuthoringCheckpoint& checkpoint,
              BoundaryAuthoringActionKind kind) {
    return std::any_of(checkpoint.actions.begin(), checkpoint.actions.end(),
                       [kind](const BoundaryAuthoringAction& action) {
                           return action.kind == kind;
                       });
}

void require_kinds(const BoundaryAuthoringCheckpoint& checkpoint,
                   std::initializer_list<BoundaryAuthoringActionKind> kinds,
                   std::string_view message) {
    for (const auto kind : kinds) {
        if (!has_kind(checkpoint, kind)) fail(message);
    }
}

void round_trip_view(const BoundaryAuthoringSession& source, std::string_view message) {
    const auto checkpoint = source.recovery_checkpoint();
    const auto encoded = encode_boundary_authoring_recovery(checkpoint);
    const auto decoded = decode_boundary_authoring_recovery(encoded);
    require(decoded.supported() && decoded.checkpoint.has_value(), message);
    const auto restored = BoundaryAuthoringSession::from_recovery_checkpoint(*decoded.checkpoint);
    require(restored.view() == source.view(), message);
    require(restored.recovery_checkpoint() == checkpoint, message);
    const auto revised = BoundaryAuthoringSession::revise_recovery_checkpoint(checkpoint);
    const auto fresh = revised.recovery_checkpoint();
    require(fresh.identity_namespace != checkpoint.identity_namespace &&
            fresh.history_position == checkpoint.history_position && fresh.counters == checkpoint.counters &&
            fresh.actions.size() == checkpoint.actions.size() && revised.phase() == source.phase() &&
            fresh.extensions.dump() == checkpoint.extensions.dump(),
            "fresh replay preserves history, phase, counters and extensions");
    require(BoundaryAuthoringSession::from_recovery_checkpoint(fresh).view() == revised.view(),
            "fresh replay emits a canonically restorable checkpoint");
    for (std::size_t i = 0; i < fresh.actions.size(); ++i) {
        require(fresh.actions[i].kind == checkpoint.actions[i].kind &&
                fresh.actions[i].generated_ids.size() == checkpoint.actions[i].generated_ids.size(),
                "fresh replay preserves action semantics and allocation counts");
        for (const auto& id : fresh.actions[i].generated_ids)
            require(id.find(fresh.identity_namespace) != std::string::npos &&
                    id.find(checkpoint.identity_namespace) == std::string::npos,
                    "fresh replay never retains generated source IDs");
    }
    require(encode_boundary_authoring_recovery(*decoded.checkpoint).dump() == encoded.dump(),
            "recovery round-trip must be deterministic");
}

void add_rectangle_without_dimensions(BoundaryAuthoringSession& session) {
    (void)session.add_line_rise_run(q("0 m"), q("2 m"));
    (void)session.add_line_rise_run(q("2 m"), q("0 m"));
    (void)session.add_line_rise_run(q("0 m"), q("-2 m"));
    (void)session.add_line_rise_run(q("-2 m"), q("0 m"));
}

Vec2 current_end(const BoundaryAuthoringSession& session) {
    const auto state = session.view();
    require(state.active_chain.has_value() && !state.active_chain->segments.empty(),
            "an active edge is required to calculate the next test endpoint");
    return state.active_chain->segments.back().segment.end;
}

void test_empty_checkpoint_round_trip() {
    BoundaryAuthoringSession source(BoundaryAuthoringMode::draw_first);
    source.set_pointer({-0.0, 1.25});
    const auto checkpoint = source.recovery_checkpoint();
    require(checkpoint.actions.empty() && checkpoint.history_position == 0,
            "a pristine session should have an empty action timeline");
    require(checkpoint.counters == BoundaryAuthoringCounters{},
            "a pristine session should retain initial next-ID counters");

    const auto encoded = encode_boundary_authoring_recovery(checkpoint);
    const auto decoded = decode_boundary_authoring_recovery(encoded);
    require(decoded.supported() && decoded.checkpoint.has_value(),
            "known recovery checkpoint should decode as supported");
    require(*decoded.checkpoint == checkpoint,
            "empty recovery checkpoint should preserve exact typed values");
    require(encode_boundary_authoring_recovery(*decoded.checkpoint).dump() == encoded.dump(),
            "recovery encode/decode should be deterministic");

    auto restored = BoundaryAuthoringSession::from_recovery_checkpoint(*decoded.checkpoint);
    require(restored.view() == source.view(),
            "restored pristine session should preserve mode, phase and pointer");
}

void test_actions_round_trip_preserves_receipts_and_pointer_outside_history() {
    BoundaryAuthoringOptions options;
    options.automatic_dimension_placement = true;
    BoundaryAuthoringSession source(BoundaryAuthoringMode::draw_first, options);
    source.set_pointer({5.5, 6.5});
    (void)source.anchor({0.0, 0.0});
    (void)source.add_line(q("1/3 ft"), angle("90 deg"));
    (void)source.add_line_to({2.0, 1.0});
    source.pen_up();

    const auto checkpoint = source.recovery_checkpoint();
    require(checkpoint.actions.size() == 4,
            "anchor, two edges and pen up should produce one action each");
    require(checkpoint.actions[1].kind == BoundaryAuthoringActionKind::line_heading &&
                checkpoint.actions[1].receipt.has_value(),
            "line action should retain its canonical construction receipt");
    require(checkpoint.actions[1].receipt->distance->exact_metres == ExactRational{127, 1250},
            "line action should retain exact quantity provenance");
    require(checkpoint.actions[2].kind == BoundaryAuthoringActionKind::line_to_point,
            "point-native line should retain its distinct semantic kind");
    require(checkpoint.actions[1].counters_after.next_segment_id ==
                checkpoint.actions[1].counters_before.next_segment_id + 1,
            "edge action should record its authoritative segment allocation");

    const auto encoded = encode_boundary_authoring_recovery(checkpoint);
    const auto decoded = decode_boundary_authoring_recovery(encoded);
    require(decoded.supported() && decoded.checkpoint.has_value(),
            "action checkpoint should decode as supported");
    const auto restored = BoundaryAuthoringSession::from_recovery_checkpoint(*decoded.checkpoint);
    require(restored.view() == source.view(),
            "recovery should preserve exact receipts, geometry, phase and pointer");
    require(restored.recovery_checkpoint() == checkpoint,
            "restored action timeline should re-export identically");
}

void test_all_action_variants_and_valid_phase_round_trips() {
    {
        BoundaryAuthoringSession session(BoundaryAuthoringMode::draw_first);
        require(session.phase() == BoundaryAuthoringPhase::awaiting_anchor,
                "Draw First should begin in its anchor phase");
        session.set_classification("initial_area");
        require(session.phase() == BoundaryAuthoringPhase::awaiting_anchor,
                "Draw First classification should still await an anchor");
        (void)session.anchor({0.0, 0.0});
        require(session.phase() == BoundaryAuthoringPhase::drawing,
                "anchoring should enter the drawing phase");
        session.pen_up();
        require(session.pen_state() == BoundaryPenState::up,
                "pen_up should expose the lifted pen state");
        session.pen_down();
        require(session.pen_state() == BoundaryPenState::down,
                "pen_down should expose the lowered pen state");
        session.classify_current_chain("classified_area");
        require(session.view().active_chain->classified &&
                    session.view().active_chain->classification == "classified_area",
                "classify_current_chain should classify the active draft");
        const auto checkpoint = session.recovery_checkpoint();
        require_kinds(checkpoint,
                      {BoundaryAuthoringActionKind::set_classification,
                       BoundaryAuthoringActionKind::classify_current_chain,
                       BoundaryAuthoringActionKind::anchor,
                       BoundaryAuthoringActionKind::pen_up,
                       BoundaryAuthoringActionKind::pen_down},
                      "classification, anchor and pen actions should be normalized");
        round_trip_view(session, "Draw First phase actions should round-trip");
    }

    {
        BoundaryAuthoringSession session(BoundaryAuthoringMode::draw_first);
        (void)session.anchor({0.0, 0.0});
        (void)session.add_line(q("1 m"), angle("0 rad"));
        (void)session.add_line_rise_run(q("1 m"), q("1 m"));
        (void)session.add_line_relative_turn(q("1 m"), angle("90 deg"));

        auto end = current_end(session);
        (void)session.add_arc_chord_angle({end.x + 1.0, end.y}, angle("90 deg"));
        end = current_end(session);
        (void)session.add_arc_chord_height({end.x, end.y + 1.0}, q("0.5 m"));
        end = current_end(session);
        (void)session.add_arc_chord_arc_length({end.x - 1.0, end.y}, q("2 m"));
        end = current_end(session);
        (void)session.add_arc_start_tangent(0.0, q("2 m"), 1.5707963267948966);
        end = current_end(session);
        (void)session.add_line_to({end.x + 1.0, end.y + 1.0});

        const auto checkpoint = session.recovery_checkpoint();
        require(checkpoint.actions.size() == 9,
                "anchor and the eight open edge variants should produce nine actions");
        require_kinds(checkpoint,
                      {BoundaryAuthoringActionKind::line_heading,
                       BoundaryAuthoringActionKind::line_rise_run,
                       BoundaryAuthoringActionKind::line_relative_turn,
                       BoundaryAuthoringActionKind::arc_chord_angle,
                       BoundaryAuthoringActionKind::arc_chord_height,
                       BoundaryAuthoringActionKind::arc_chord_length,
                       BoundaryAuthoringActionKind::arc_start_tangent,
                       BoundaryAuthoringActionKind::line_to_point},
                      "every open construction variant should be normalized");
        for (const auto& action : checkpoint.actions) {
            if (action.kind == BoundaryAuthoringActionKind::anchor) continue;
            require(action.receipt.has_value(),
                    "every edge action should retain its canonical construction receipt");
        }
        round_trip_view(session, "all open construction variants should round-trip");
    }

    {
        BoundaryAuthoringSession session(BoundaryAuthoringMode::define_first);
        require(session.phase() == BoundaryAuthoringPhase::awaiting_classification,
                "Define First should begin in its classification phase");
        session.set_classification("living_area");
        require(session.phase() == BoundaryAuthoringPhase::awaiting_anchor,
                "Define First classification should release the anchor phase");
        (void)session.anchor({0.0, 0.0});
        (void)session.add_line_rise_run(q("0 m"), q("2 m"));
        require(session.phase() == BoundaryAuthoringPhase::awaiting_dimension &&
                    session.pending_dimension().has_value(),
                "Define First edges should enter the pending manual dimension phase");
        round_trip_view(session, "a pending manual dimension should round-trip");
        (void)session.place_manual_dimension({1.0, -0.25});
        require(session.phase() == BoundaryAuthoringPhase::drawing,
                "placing the only pending dimension should restore drawing");
        (void)session.add_line_rise_run(q("2 m"), q("0 m"));
        (void)session.place_manual_dimension({2.25, 1.0});
        (void)session.add_closing_segment();
        require(session.phase() == BoundaryAuthoringPhase::awaiting_dimension &&
                    session.pending_dimension().has_value(),
                "line closure should create a real pending dimension phase");
        (void)session.place_manual_dimension({1.0, 1.0});
        require(session.phase() == BoundaryAuthoringPhase::drawing,
                "placing the closure dimension should restore drawing");
        const auto accepted = session.close_chain();
        require(session.phase() == BoundaryAuthoringPhase::completed &&
                    accepted.dimensions.size() == 3,
                "completed Define First closure should retain all dimensions");
        const auto checkpoint = session.recovery_checkpoint();
        require_kinds(checkpoint,
                      {BoundaryAuthoringActionKind::set_classification,
                       BoundaryAuthoringActionKind::anchor,
                       BoundaryAuthoringActionKind::line_rise_run,
                       BoundaryAuthoringActionKind::line_closure,
                       BoundaryAuthoringActionKind::manual_dimension,
                       BoundaryAuthoringActionKind::close_chain},
                      "manual Define First closure should normalize every phase action");
        round_trip_view(session, "completed manual Define First closure should round-trip");
    }

    {
        BoundaryAuthoringSession session(BoundaryAuthoringMode::define_first);
        session.set_classification("garage");
        (void)session.anchor({0.0, 0.0});
        (void)session.add_line(q("2 m"), angle("0 rad"));
        require(session.phase() == BoundaryAuthoringPhase::awaiting_dimension &&
                    session.pending_dimension().has_value(),
                "explicit automatic placement should start from a pending edge");
        (void)session.place_automatic_dimension();
        require(session.phase() == BoundaryAuthoringPhase::drawing &&
                    session.view().active_chain->dimensions.back().placement ==
                        BoundaryDimensionPlacement::automatic,
                "automatic placement should complete the pending edge");
        const auto checkpoint = session.recovery_checkpoint();
        require_kinds(checkpoint, {BoundaryAuthoringActionKind::automatic_dimension},
                      "automatic dimension placement should have its own action kind");
        round_trip_view(session, "automatic dimension placement should round-trip");
    }
}

void test_multiple_accepted_chains_and_classify_last() {
    BoundaryAuthoringSession session(BoundaryAuthoringMode::draw_first);
    (void)session.anchor({0.0, 0.0});
    add_rectangle_without_dimensions(session);
    const auto first = session.close_chain();
    require(!first.classified && first.classification.empty() &&
                session.phase() == BoundaryAuthoringPhase::completed,
            "Draw First should permit an unclassified completed chain");
    session.classify_last_chain("living_area");
    require(session.accepted_chains().back().classified &&
                session.accepted_chains().back().classification == "living_area",
            "classify_last_chain should classify the most recent accepted chain");

    (void)session.anchor({10.0, 10.0});
    add_rectangle_without_dimensions(session);
    session.classify_current_chain("porch");
    const auto second = session.close_chain();
    require(session.accepted_chains().size() == 2 && second.classification == "porch" &&
                session.accepted_chains().front().classification == "living_area",
            "multiple accepted chains should retain independent classifications");

    const auto checkpoint = session.recovery_checkpoint();
    require_kinds(checkpoint,
                  {BoundaryAuthoringActionKind::classify_last_chain,
                   BoundaryAuthoringActionKind::classify_current_chain,
                   BoundaryAuthoringActionKind::close_chain},
                  "accepted-chain classification actions should be normalized");
    round_trip_view(session, "multiple accepted chains should round-trip");
}

void test_undo_redo_branch_and_reset_retain_highwater() {
    BoundaryAuthoringSession source(BoundaryAuthoringMode::draw_first);
    (void)source.anchor({0.0, 0.0});
    const auto first = source.add_line(q("1 m"), angle("0 rad"));
    const auto second = source.add_line(q("1 m"), angle("0 rad"));
    const auto after_second = source.view();
    const auto highwater = source.counters();
    require(source.undo(), "first edge should be undoable");
    const auto with_redo = source.recovery_checkpoint();
    require(with_redo.history_position + 1 == with_redo.actions.size() &&
                with_redo.counters == highwater && source.can_redo(),
            "undo should preserve the redo tail and allocated high-water values");

    const auto redo_encoded = encode_boundary_authoring_recovery(with_redo);
    const auto redo_decoded = decode_boundary_authoring_recovery(redo_encoded);
    require(redo_decoded.supported() && redo_decoded.checkpoint.has_value(),
            "a checkpoint with a redo tail should decode as supported");
    auto restored_redo =
        BoundaryAuthoringSession::from_recovery_checkpoint(*redo_decoded.checkpoint);
    require(restored_redo.can_redo(),
            "recovery should retain an actual redo tail rather than only its position");
    require(restored_redo.redo() && restored_redo.view() == after_second,
            "redo after JSON recovery should restore the original semantic state");

    const auto replacement = source.add_line(q("2 m"), angle("0 rad"));
    require(replacement != first && !source.can_redo(),
            "a branch action should discard redo while retaining monotonic IDs");
    const auto branched = source.recovery_checkpoint();
    const auto abandoned_id_is_absent = std::none_of(
        branched.actions.begin(), branched.actions.end(), [&](const BoundaryAuthoringAction& action) {
            return std::find(action.generated_ids.begin(), action.generated_ids.end(), second) !=
                   action.generated_ids.end();
        });
    require(branched.actions.size() == branched.history_position &&
                branched.counters.next_segment_id > with_redo.counters.next_segment_id &&
                abandoned_id_is_absent,
            "branch truncation should retain abandoned allocation gaps");
    round_trip_view(source, "a branch-gapped timeline should round-trip");

    source.reset();
    const auto reset = source.recovery_checkpoint();
    require(reset.actions.empty() && reset.history_position == 0 &&
                reset.counters == branched.counters,
            "reset should clear semantic history without rewinding high-water counters");
    require(source.phase() == BoundaryAuthoringPhase::awaiting_anchor,
            "reset should restore Draw First's initial phase");
    round_trip_view(source, "reset should preserve high-water counters through recovery");
}

void test_unknown_version_is_opaque_and_known_shape_is_strict() {
    BoundaryAuthoringSession source(BoundaryAuthoringMode::draw_first);
    auto encoded = encode_boundary_authoring_recovery(source.recovery_checkpoint());
    const auto inspected = inspect_boundary_authoring_recovery(encoded);
    require(inspected.format == BoundaryAuthoringRecoveryFormat::supported_v1 &&
                inspected.version == std::uint64_t{1},
            "inspect should identify the supported recovery schema without replaying it");
    encoded["version"] = 99;
    encoded["future_member"] = nlohmann::json{{"preserve", true}};
    const auto unknown_inspection = inspect_boundary_authoring_recovery(encoded);
    require(unknown_inspection.format == BoundaryAuthoringRecoveryFormat::unsupported_version &&
                unknown_inspection.version == std::uint64_t{99},
            "inspect should identify an unknown positive recovery schema");
    const auto opaque = decode_boundary_authoring_recovery(encoded);
    require(!opaque.supported() && opaque.original_envelope.has_value() &&
                *opaque.original_envelope == encoded,
            "unknown positive recovery versions should remain opaque");
    auto future_replay = encode_boundary_authoring_recovery(source.recovery_checkpoint());
    future_replay["replay_version"] = 999;
    future_replay["future_member"] = nlohmann::json{{"preserve", true}};
    const auto opaque_replay = decode_boundary_authoring_recovery(future_replay);
    require(!opaque_replay.supported() && opaque_replay.opaque() &&
                *opaque_replay.original_envelope == future_replay &&
                opaque_replay.replay_version == std::uint64_t{999},
            "unknown positive replay versions should preserve the complete envelope");
    rejected([&] {
        auto malformed = encode_boundary_authoring_recovery(source.recovery_checkpoint());
        malformed["actions"] = nlohmann::json::array({nlohmann::json{{"kind", "anchor"}}});
        (void)decode_boundary_authoring_recovery(malformed);
    }, "known recovery actions must reject missing typed fields");
    rejected([&] {
        auto malformed = encode_boundary_authoring_recovery(source.recovery_checkpoint());
        malformed["replay_version"] = 0;
        (void)decode_boundary_authoring_recovery(malformed);
    }, "known recovery envelopes must reject nonpositive replay versions");
    rejected([&] {
        auto malformed = encode_boundary_authoring_recovery(source.recovery_checkpoint());
        malformed["replay_version"] = -1;
        (void)decode_boundary_authoring_recovery(malformed);
    }, "known recovery envelopes must reject negative replay versions");
    rejected([&] {
        auto malformed = encode_boundary_authoring_recovery(source.recovery_checkpoint());
        malformed["replay_version"] = "1";
        (void)decode_boundary_authoring_recovery(malformed);
    }, "known recovery envelopes must reject noninteger replay versions");
    rejected([&] {
        auto malformed = encode_boundary_authoring_recovery(source.recovery_checkpoint());
        malformed["unexpected"] = true;
        (void)decode_boundary_authoring_recovery(malformed);
    }, "known recovery envelopes must reject unknown top-level members");
    rejected([&] {
        auto malformed = encode_boundary_authoring_recovery(source.recovery_checkpoint());
        malformed["extensions"] = nlohmann::json::array();
        (void)decode_boundary_authoring_recovery(malformed);
    }, "known recovery extensions must remain an object");
}

void test_reset_and_cancel_terminal_semantics() {
    BoundaryAuthoringSession define_first(BoundaryAuthoringMode::define_first);
    const auto namespace_before = define_first.view().identity_namespace;
    define_first.set_classification("living_area");
    (void)define_first.anchor({0.0, 0.0});
    (void)define_first.add_line(q("1 m"), angle("0 rad"));
    const auto retained = define_first.counters();
    define_first.reset();
    require(define_first.phase() == BoundaryAuthoringPhase::awaiting_classification &&
                !define_first.active_chain().has_value() &&
                !define_first.pending_dimension().has_value() &&
                define_first.counters() == retained &&
                define_first.view().identity_namespace == namespace_before,
            "Define First reset should clear semantic state while retaining identity highwaters");
    round_trip_view(define_first, "Define First reset should be recoverable");

    BoundaryAuthoringSession cancelled(BoundaryAuthoringMode::draw_first);
    cancelled.set_pointer({7.0, 8.0});
    (void)cancelled.anchor({0.0, 0.0});
    (void)cancelled.add_line(q("1 m"), angle("0 rad"));
    const auto cancelled_counters = cancelled.counters();
    cancelled.cancel();
    require(cancelled.phase() == BoundaryAuthoringPhase::cancelled &&
                !cancelled.active_chain().has_value() && cancelled.accepted_chains().empty() &&
                !cancelled.can_undo() && !cancelled.can_redo() &&
                cancelled.counters() == cancelled_counters,
            "cancel should be terminal, clear active semantic state and retain highwaters");
    rejected([&] { (void)cancelled.recovery_checkpoint(); },
             "cancelled sessions must not expose an active recovery checkpoint");
}

void test_counter_tampering_and_resource_budgets() {
    BoundaryAuthoringSession source(BoundaryAuthoringMode::draw_first);
    (void)source.anchor({0.0, 0.0});
    (void)source.add_line(q("1 m"), angle("0 rad"));
    const auto checkpoint = source.recovery_checkpoint();
    const auto encoded = encode_boundary_authoring_recovery(checkpoint);

    const auto expect_rejected = [&](auto&& mutate, std::string_view message) {
        auto tampered = encoded;
        mutate(tampered);
        rejected([&] { (void)decode_boundary_authoring_recovery(tampered); }, message);
    };
    expect_rejected(
        [](nlohmann::json& value) {
            value["actions"][1]["counters_after"]["next_segment_id"] =
                value["actions"][1]["counters_before"]["next_segment_id"];
        },
        "an action must reject an inconsistent segment allocation fence");
    expect_rejected(
        [](nlohmann::json& value) {
            value["actions"][1]["counters_before"]["next_segment_id"] =
                value["actions"][1]["counters_after"]["next_segment_id"].get<std::uint64_t>() +
                10;
            value["actions"][1]["counters_after"]["next_segment_id"] =
                value["actions"][1]["counters_before"]["next_segment_id"];
        },
        "an action that moves its allocation fence forward must reject canonical replay");
    expect_rejected(
        [](nlohmann::json& value) {
            value["actions"][1]["counters_before"]["next_segment_id"] =
                std::numeric_limits<std::uint64_t>::max();
            value["actions"][1]["counters_after"]["next_segment_id"] =
                std::numeric_limits<std::uint64_t>::max();
        },
        "an exhausted action allocation counter must reject before allocation");
    expect_rejected(
        [](nlohmann::json& value) {
            value["actions"][1]["generated_ids"][0] = "forged-segment-id";
        },
        "tampered generated identities must reject canonical replay");
    expect_rejected(
        [](nlohmann::json& value) {
            value["counters"]["next_segment_id"] = 1;
        },
        "final counters below the action highwater must reject recovery");
    expect_rejected(
        [](nlohmann::json& value) {
            value["actions"][1]["unexpected"] = true;
        },
        "known actions must reject unknown fields");

    {
        auto limits = boundary_authoring_recovery_default_limits;
        limits.max_encoded_bytes = encoded.dump().size() - 1;
        rejected([&] { (void)decode_boundary_authoring_recovery(encoded, limits); },
                 "encoded recovery input must fail its byte budget without truncation");
    }
    {
        auto limits = boundary_authoring_recovery_default_limits;
        limits.max_json_depth = 1;
        rejected([&] { (void)decode_boundary_authoring_recovery(encoded, limits); },
                 "recovery input must fail its JSON depth budget");
    }
    {
        auto limits = boundary_authoring_recovery_default_limits;
        limits.max_json_values = 1;
        rejected([&] { (void)decode_boundary_authoring_recovery(encoded, limits); },
                 "recovery input must fail its JSON value budget");
    }
    {
        auto limits = boundary_authoring_recovery_default_limits;
        limits.max_string_bytes = 1;
        rejected([&] { (void)decode_boundary_authoring_recovery(encoded, limits); },
                 "recovery input must fail its string budget");
    }
    {
        auto limits = boundary_authoring_recovery_default_limits;
        limits.max_actions = 1;
        rejected([&] { (void)decode_boundary_authoring_recovery(encoded, limits); },
                 "recovery input must fail its action budget");
    }
    {
        auto limits = boundary_authoring_recovery_default_limits;
        limits.max_generated_ids_per_action = 1;
        rejected([&] { (void)decode_boundary_authoring_recovery(encoded, limits); },
                 "recovery input must fail its per-action identity budget");
    }
    {
        auto limits = boundary_authoring_recovery_default_limits;
        limits.max_total_generated_ids = 1;
        rejected([&] { (void)decode_boundary_authoring_recovery(encoded, limits); },
                 "recovery input must fail its total identity budget");
    }
    {
        auto limits = boundary_authoring_recovery_default_limits;
        limits.max_replay_work = 1;
        rejected([&] { (void)decode_boundary_authoring_recovery(encoded, limits); },
                 "recovery input must fail its replay-work budget");
    }

    auto too_many_ids = encoded;
    too_many_ids["actions"][1]["generated_ids"] = nlohmann::json::array();
    for (std::size_t index = 0; index < 65; ++index) {
        too_many_ids["actions"][1]["generated_ids"].push_back("generated-" +
                                                                  std::to_string(index));
    }
    rejected([&] { (void)decode_boundary_authoring_recovery(too_many_ids); },
             "an action with too many generated identities must reject without truncation");

    BoundaryAuthoringSession closed(BoundaryAuthoringMode::define_first);
    closed.set_classification("living_area");
    (void)closed.anchor({0.0, 0.0});
    (void)closed.add_line_rise_run(q("0 m"), q("2 m"));
    (void)closed.place_manual_dimension({1.0, -0.25});
    (void)closed.add_line_rise_run(q("2 m"), q("0 m"));
    (void)closed.place_manual_dimension({2.25, 1.0});
    (void)closed.add_closing_segment();
    (void)closed.place_manual_dimension({1.0, 1.0});
    (void)closed.close_chain();
    const auto closed_encoded = encode_boundary_authoring_recovery(closed.recovery_checkpoint());
    {
        auto limits = boundary_authoring_recovery_default_limits;
        limits.max_chain_edges = 2;
        rejected([&] { (void)decode_boundary_authoring_recovery(closed_encoded, limits); },
                 "close records must fail their retained edge budget");
    }
    {
        auto limits = boundary_authoring_recovery_default_limits;
        limits.max_dimensions_per_chain = 2;
        rejected([&] { (void)decode_boundary_authoring_recovery(closed_encoded, limits); },
                 "close records must fail their retained dimension budget");
    }
    {
        auto tampered = closed_encoded;
        tampered["actions"][3]["dimension"]["text_position"][0] = 99.0;
        rejected([&] { (void)decode_boundary_authoring_recovery(tampered); },
                 "tampered dimension placement must reject canonical replay");
    }
}

void test_styled_checkpoint_preserves_dimension_presentation() {
    BoundaryAuthoringSession source(BoundaryAuthoringMode::define_first);
    source.set_classification("living_area");
    (void)source.anchor({0.0, 0.0});
    (void)source.add_line(q("2 m"), angle("0 rad"));
    (void)source.place_manual_dimension({1.0, -0.25});
    auto checkpoint = source.recovery_checkpoint();
    checkpoint.version = 3;
    checkpoint.actions.back().dimension->presentation =
        BoundaryDimensionPresentation{3.5, "#123abc", true, true, false, 1.5707963267948966};
    const auto encoded = encode_boundary_authoring_recovery(checkpoint);
    require(encoded["version"] == 3 &&
                encoded["actions"].back()["dimension"]["presentation"]["visible"] == false,
            "styled recovery must retain the full dimension presentation");
    const auto decoded = decode_boundary_authoring_recovery(encoded);
    require(decoded.supported() && *decoded.checkpoint == checkpoint,
            "styled checkpoint must round trip without changing receipts or presentation");
    auto restored = BoundaryAuthoringSession::from_recovery_checkpoint(*decoded.checkpoint);
    require(restored.active_chain()->dimensions.front().presentation ==
                checkpoint.actions.back().dimension->presentation,
            "recovery replay must apply presentation to the pending dimension");
}

void test_styled_dimensions_recover_accepted_chains_and_revise_namespace() {
    BoundaryAuthoringSession source(BoundaryAuthoringMode::define_first);
    source.set_classification("living_area");
    (void)source.anchor({0.0, 0.0});
    const BoundaryDimensionPresentation horizontal{4.0, "#123abc", true, true, true, 0.0};
    const BoundaryDimensionPresentation vertical{3.5, "#456def", false, true, true,
                                                  std::numbers::pi / 2.0};
    const BoundaryDimensionPresentation hidden{2.5, "#263241", true, false, false, 0.0};
    (void)source.add_line(q("2000 mm"), angle("0 deg"));
    (void)source.place_manual_dimension({1.0, -0.25}, horizontal);
    (void)source.add_line(q("2 m"), angle("90 deg"));
    (void)source.place_manual_dimension({2.25, 1.0}, vertical);
    (void)source.add_closing_segment();
    (void)source.place_manual_dimension({1.0, 1.0}, hidden);
    const auto accepted = source.close_chain();
    round_trip_view(source, "accepted styled dimensions must survive recovery and fresh-namespace replay");
    const auto checkpoint = source.recovery_checkpoint();
    require(checkpoint.version == 3 && boundary_authoring_checkpoint_has_dimension_presentation(checkpoint),
            "styled accepted history must select the presentation recovery dialect");
    const auto encoded = encode_boundary_authoring_recovery(checkpoint);
    const auto close_dimensions = encoded["actions"].back()["chain"]["dimensions"];
    require(close_dimensions[0]["presentation"]["rotation_radians"] == 0.0 &&
                close_dimensions[1]["presentation"]["rotation_radians"] == std::numbers::pi / 2.0 &&
                close_dimensions[2]["presentation"]["visible"] == false,
            "close records must retain horizontal, vertical and hidden dimension presentation");
    auto revised = BoundaryAuthoringSession::revise_recovery_checkpoint(checkpoint);
    const auto revised_chain = revised.accepted_chains().front();
    for (std::size_t index = 0; index < accepted.dimensions.size(); ++index) {
        require(revised_chain.dimensions[index].presentation == accepted.dimensions[index].presentation &&
                    revised_chain.dimensions[index].text_position.x == accepted.dimensions[index].text_position.x &&
                    revised_chain.dimensions[index].id != accepted.dimensions[index].id &&
                    revised_chain.dimensions[index].boundary_id == revised_chain.boundary.id &&
                    revised_chain.dimensions[index].segment_id == revised_chain.boundary.segments[index].segment_id,
                "revision must retain styled measured intent while regenerating all topology identities");
    }
    auto chain_only = checkpoint;
    for (auto& action : chain_only.actions) if (action.dimension) action.dimension->presentation.reset();
    require(boundary_authoring_checkpoint_has_dimension_presentation(chain_only),
            "presentation detection must inspect close records as well as placement actions");
    for (const auto legacy_version : {1, 2}) {
        auto downgraded = encoded;
        downgraded["version"] = legacy_version;
        rejected([&] { (void)decode_boundary_authoring_recovery(downgraded); },
                 "known older recovery dialects must reject presentation rather than discard it");
        auto downgraded_checkpoint = checkpoint;
        downgraded_checkpoint.version = legacy_version;
        rejected([&] { (void)encode_boundary_authoring_recovery(downgraded_checkpoint); },
                 "a styled checkpoint cannot be written using an older known dialect");
    }
    for (const int malformed_field : {0, 1, 2, 3, 4, 5}) {
        auto malformed = encoded;
        auto& style = malformed["actions"][3]["dimension"]["presentation"];
        if (malformed_field == 0) style.erase("bold");
        if (malformed_field == 1) style["unexpected"] = true;
        if (malformed_field == 2) style["visible"] = 0;
        if (malformed_field == 3) style["text_height_mm"] = 0.49;
        if (malformed_field == 4) style["color"] = "red";
        if (malformed_field == 5) style["rotation_radians"] = "vertical";
        rejected([&] { (void)decode_boundary_authoring_recovery(malformed); },
                 "known presentation recovery must reject malformed style fields");
    }
    auto malformed_chain = encoded;
    malformed_chain["actions"].back()["chain"]["dimensions"][0]["presentation"]["italic"] = "yes";
    rejected([&] { (void)decode_boundary_authoring_recovery(malformed_chain); },
             "presentation validation must also cover accepted close records");
    for (const std::string_view field : {"text_height_mm", "color", "bold", "italic", "visible",
                                        "rotation_radians"}) {
        auto tampered = encoded;
        auto& style = tampered["actions"].back()["chain"]["dimensions"][0]["presentation"];
        if (field == "text_height_mm") style[field] = 3.0;
        else if (field == "color") style[field] = "#ffffff";
        else if (field == "rotation_radians") style[field] = 0.25;
        else style[field] = !style[field].get<bool>();
        rejected([&] { (void)decode_boundary_authoring_recovery(tampered); },
                 "all six presentation fields must participate in canonical close replay equality");
    }
    auto future = encoded;
    future["version"] = 4;
    future["actions"][3]["dimension"]["presentation"] = "future presentation";
    require(decode_boundary_authoring_recovery(future).original_envelope == future,
            "future recovery dialects must remain completely opaque, including unknown presentation");
    future["version"] = 3;
    future["replay_version"] = 99;
    require(decode_boundary_authoring_recovery(future).original_envelope == future,
            "future replay dialects inside v3 must preserve the complete envelope");
}

void test_styled_redo_only_history_and_typed_chord_compatibility() {
    BoundaryAuthoringSession source(BoundaryAuthoringMode::define_first);
    source.set_classification("living_area");
    (void)source.anchor({0.0, 0.0});
    (void)source.add_line(q("2 m"), angle("0 deg"));
    const BoundaryDimensionPresentation hidden{2.5, "#263241", false, false, false, 0.0};
    const auto styled = source.place_manual_dimension({1.0, -0.25}, hidden);
    require(source.undo(), "styled placement must be undoable");
    const auto checkpoint = source.recovery_checkpoint();
    require(checkpoint.version == 3 && checkpoint.history_position < checkpoint.actions.size() &&
                source.active_chain()->dimensions.empty() &&
                boundary_authoring_checkpoint_has_dimension_presentation(checkpoint),
            "styled data only in redo history must select v3 even with an unstyled active state");
    round_trip_view(source, "redo-only presentation must survive recovery and revision");
    auto restored = BoundaryAuthoringSession::from_recovery_checkpoint(
        *decode_boundary_authoring_recovery(encode_boundary_authoring_recovery(checkpoint)).checkpoint);
    require(restored.redo() && restored.active_chain()->dimensions.front() == styled,
            "recovering an undone styled placement must restore exact presentation on redo");
    require(restored.undo(), "redo placement should be undoable again");
    (void)restored.place_manual_dimension({1.0, -0.5});
    const auto unstyled = restored.recovery_checkpoint();
    require(unstyled.version == 1 && !boundary_authoring_checkpoint_has_dimension_presentation(unstyled) &&
                !encode_boundary_authoring_recovery(unstyled)["actions"].back()["dimension"].contains("presentation"),
            "replacing the styled redo branch with an unstyled placement must retain the legacy encoding");

    BoundaryAuthoringSession typed(BoundaryAuthoringMode::define_first);
    typed.set_classification("living_area");
    (void)typed.anchor({0.0, 0.0});
    (void)typed.add_arc_chord_angle(q("2 m"), angle("0 deg"), angle("90 deg"));
    require(typed.recovery_checkpoint().version == 2,
            "unstyled typed chord input must retain recovery v2");
    (void)typed.place_manual_dimension({1.0, -0.25}, hidden);
    require(typed.recovery_checkpoint().version == 3 &&
                boundary_authoring_checkpoint_has_typed_chord(typed.recovery_checkpoint()),
            "presentation recovery v3 must also support typed chord receipts");
    round_trip_view(typed, "typed chord and presentation must replay together in recovery v3");
}

void test_fault_injection_is_atomic() {
    BoundaryAuthoringSession source(BoundaryAuthoringMode::draw_first);
    (void)source.anchor({0.0, 0.0});
    const auto before = source.recovery_checkpoint();
    for (const auto point : {BoundaryAuthoringFaultPoint::before_semantic_mutation,
                             BoundaryAuthoringFaultPoint::before_action_append,
                             BoundaryAuthoringFaultPoint::before_publication}) {
        bool armed = true;
        source.set_fault_hook([&](BoundaryAuthoringFaultPoint observed) {
            if (armed && observed == point) {
                armed = false;
                throw std::runtime_error("injected recovery transaction failure");
            }
        });
        throws_any([&] { (void)source.add_line(q("1 m"), angle("0 rad")); },
                   "fault injection should propagate as an exception");
        require(source.recovery_checkpoint() == before,
                "faulted semantic mutation must preserve exported state and counters");
        source.clear_fault_hook();
    }

    BoundaryAuthoringSession navigation(BoundaryAuthoringMode::draw_first);
    (void)navigation.anchor({0.0, 0.0});
    (void)navigation.add_line(q("1 m"), angle("0 rad"));
    const auto before_undo = navigation.recovery_checkpoint();
    for (const auto point : {BoundaryAuthoringFaultPoint::before_semantic_mutation,
                             BoundaryAuthoringFaultPoint::before_publication}) {
        bool armed = true;
        navigation.set_fault_hook([&](BoundaryAuthoringFaultPoint observed) {
            if (armed && observed == point) {
                armed = false;
                throw std::runtime_error("injected undo transaction failure");
            }
        });
        throws_any([&] { require(navigation.undo(), "undo should be available"); },
                   "fault injection should propagate during undo");
        navigation.clear_fault_hook();
        require(navigation.recovery_checkpoint() == before_undo,
                "faulted undo must preserve semantic state, history and counters");
    }
    require(navigation.undo(), "undo should succeed after its fault hooks are cleared");
    const auto before_redo = navigation.recovery_checkpoint();
    for (const auto point : {BoundaryAuthoringFaultPoint::before_semantic_mutation,
                             BoundaryAuthoringFaultPoint::before_publication}) {
        bool armed = true;
        navigation.set_fault_hook([&](BoundaryAuthoringFaultPoint observed) {
            if (armed && observed == point) {
                armed = false;
                throw std::runtime_error("injected redo transaction failure");
            }
        });
        throws_any([&] { require(navigation.redo(), "redo should be available"); },
                   "fault injection should propagate during redo");
        navigation.clear_fault_hook();
        require(navigation.recovery_checkpoint() == before_redo,
                "faulted redo must preserve semantic state, history and counters");
    }

    BoundaryAuthoringSession pending(BoundaryAuthoringMode::define_first);
    pending.set_classification("living_area");
    (void)pending.anchor({0.0, 0.0});
    (void)pending.add_line(q("1 m"), angle("0 rad"));
    const auto before_dimension = pending.recovery_checkpoint();
    for (const auto point : {BoundaryAuthoringFaultPoint::before_semantic_mutation,
                             BoundaryAuthoringFaultPoint::before_action_append,
                             BoundaryAuthoringFaultPoint::before_publication}) {
        bool armed = true;
        pending.set_fault_hook([&](BoundaryAuthoringFaultPoint observed) {
            if (armed && observed == point) {
                armed = false;
                throw std::runtime_error("injected dimension transaction failure");
            }
        });
        throws_any([&] { (void)pending.place_manual_dimension({0.5, -0.25}); },
                   "fault injection should propagate during pending-dimension placement");
        pending.clear_fault_hook();
        require(pending.recovery_checkpoint() == before_dimension,
                "faulted pending-dimension placement must preserve its candidate state");
    }

    BoundaryAuthoringSession target(BoundaryAuthoringMode::define_first);
    target.set_classification("target");
    (void)target.anchor({10.0, 10.0});
    (void)target.add_line(q("1 m"), angle("0 rad"));
    const auto before_restore = target.recovery_checkpoint();
    target.set_fault_hook([](BoundaryAuthoringFaultPoint observed) {
        if (observed == BoundaryAuthoringFaultPoint::before_publication) {
            throw std::runtime_error("injected restore transaction failure");
        }
    });
    throws_any([&] { target.restore_recovery_checkpoint(source.recovery_checkpoint()); },
               "fault injection should propagate during checkpoint restore");
    target.clear_fault_hook();
    require(target.recovery_checkpoint() == before_restore,
            "faulted checkpoint restore must preserve the original pending state");
    target.restore_recovery_checkpoint(source.recovery_checkpoint());
    require(target.recovery_checkpoint() == source.recovery_checkpoint(),
            "checkpoint restore should succeed after its publication fault is cleared");
}

}  // namespace

int main() {
    sketch::testing::noninteractive_errors();
    try {
        test_empty_checkpoint_round_trip();
        test_actions_round_trip_preserves_receipts_and_pointer_outside_history();
        test_all_action_variants_and_valid_phase_round_trips();
        test_multiple_accepted_chains_and_classify_last();
        test_undo_redo_branch_and_reset_retain_highwater();
        test_unknown_version_is_opaque_and_known_shape_is_strict();
        test_reset_and_cancel_terminal_semantics();
        test_counter_tampering_and_resource_budgets();
        test_styled_checkpoint_preserves_dimension_presentation();
        test_styled_dimensions_recover_accepted_chains_and_revise_namespace();
        test_styled_redo_only_history_and_typed_chord_compatibility();
        test_fault_injection_is_atomic();
        std::cout << "Boundary authoring recovery tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
