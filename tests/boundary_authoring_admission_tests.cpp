#include "sketch/boundary_authoring_recovery.hpp"
#include "support/noninteractive_errors.hpp"

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {
using namespace sketch;

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

template <typename Function>
void rejected(Function&& function, std::string_view message) {
    try {
        function();
    } catch (const std::invalid_argument&) {
        return;
    }
    throw std::runtime_error(std::string(message));
}

// This checks the actual public save/parse/load path with the session's policy.
// It deliberately does not use the resource estimator as its expected result.
void require_persistable(const BoundaryAuthoringSession& session) {
    const auto checkpoint = session.recovery_checkpoint();
    const auto policy = session.resource_policy();
    const auto bytes = encode_boundary_authoring_recovery(checkpoint, policy).dump();
    const auto decoded = decode_boundary_authoring_recovery(nlohmann::json::parse(bytes), policy);
    require(decoded.supported(), "an accepted session must decode under its own policy");
    const auto restored = BoundaryAuthoringSession::from_recovery_checkpoint(
        *decoded.checkpoint, policy);
    require(restored.recovery_checkpoint() == checkpoint,
            "an accepted session must restore its complete history under the same policy");
    require(encode_boundary_authoring_recovery(restored.recovery_checkpoint(), policy).dump() == bytes,
            "resource admission must not change canonical checkpoint bytes");
}

template <typename Function>
void require_rejection_preserves(BoundaryAuthoringSession& session, Function&& function) {
    const auto before = session.recovery_checkpoint();
    const auto roots = session.structural_stats();
    const auto phase = session.phase();
    const auto pen = session.pen_state();
    rejected(std::forward<Function>(function), "an over-budget operation must be rejected");
    require(session.recovery_checkpoint() == before && session.phase() == phase &&
                session.pen_state() == pen,
            "resource rejection must preserve geometry, pointer, counters and full history");
    const auto after = session.structural_stats();
    require(after.history_root_identity == roots.history_root_identity &&
                after.redo_root_identity == roots.redo_root_identity &&
                after.retained_sequence_chunks == roots.retained_sequence_chunks,
            "resource rejection must not publish replacement history or geometry roots");
    require_persistable(session);
}

void action_ceiling_preserves_undo_redo_and_branching() {
    auto policy = boundary_authoring_recovery_default_limits;
    policy.max_actions = 3;
    BoundaryAuthoringOptions options;
    options.automatic_dimension_placement = true;
    BoundaryAuthoringSession session(BoundaryAuthoringMode::draw_first, options, policy);
    (void)session.anchor({-0.0, 0.0});
    (void)session.add_line_to({1.0, 0.0});
    const auto discarded_id = session.add_line_to({2.0, 0.0});
    require_persistable(session);
    require_rejection_preserves(session, [&] { (void)session.add_line_to({3.0, 0.0}); });

    session.set_pointer({-0.0, std::nextafter(10.0, 11.0)});
    const auto full = session.recovery_checkpoint();
    for (int index = 0; index < 3; ++index) {
        require(session.undo(), "admission must reserve the complete undo zipper");
        require_persistable(session);
    }
    require(!session.undo(), "undo must stop at the initial state");
    for (int index = 0; index < 3; ++index) {
        require(session.redo(), "near-limit history must remain fully redoable");
        require_persistable(session);
    }
    require(session.recovery_checkpoint() == full,
            "full near-limit navigation must preserve counters and pointer bits");
    require(session.undo(), "the final action must be available for branching");
    const auto branch_id = session.add_line_to({2.0, 1.0});
    require(branch_id != discarded_id && !session.can_redo() &&
                session.recovery_checkpoint().actions.size() == 3,
            "a branch must drop discarded resource charges and keep fresh identities");
    require_persistable(session);

    auto copy = session;
    require(copy.resource_policy().max_actions == 3, "a session copy must preserve its policy");
    copy.reset();
    require(copy.resource_policy().max_actions == 3, "reset must not relax the session policy");
    require_persistable(copy);
    require_persistable(session);
}

void typed_restore_uses_destination_policy_and_preserves_state_on_failure() {
    BoundaryAuthoringSession source(BoundaryAuthoringMode::draw_first);
    (void)source.anchor({0.0, 0.0});
    for (int index = 1; index <= 3; ++index) {
        (void)source.add_line_to({static_cast<double>(index), 0.0});
    }
    const auto large = source.recovery_checkpoint();
    auto policy = boundary_authoring_recovery_default_limits;
    policy.max_actions = 2;
    BoundaryAuthoringSession destination(BoundaryAuthoringMode::draw_first, {}, policy);
    destination.set_classification("existing");
    destination.set_pointer({1.25, -0.0});
    require_rejection_preserves(destination, [&] { destination.restore_recovery_checkpoint(large); });
    require(destination.resource_policy().max_actions == 2,
            "rejected restore must not replace the destination policy");
    rejected([&] { (void)BoundaryAuthoringSession::from_recovery_checkpoint(large, policy); },
             "typed static restore must enforce the supplied policy");
    rejected([&] { (void)encode_boundary_authoring_recovery(large, policy); },
             "encoding must enforce the same action policy");
    const auto encoded = encode_boundary_authoring_recovery(large);
    rejected([&] { (void)decode_boundary_authoring_recovery(encoded, policy); },
             "decoding must enforce the same action policy before replay");
}

void raw_wire_limits_apply_before_live_state_becomes_unsavable() {
    // Independent raw limits must constrain live admission, even when compact
    // geometry remains small. Do not assume the conservative charge equals a
    // serialized byte count: every accepted prefix is checked through the codec.
    for (int kind = 0; kind < 4; ++kind) {
        auto policy = boundary_authoring_recovery_default_limits;
        if (kind == 0) policy.max_string_bytes = 4096;
        if (kind == 1) policy.max_encoded_bytes = 4096;
        if (kind == 2) policy.max_json_values = 512;
        if (kind == 3) policy.max_total_generated_ids = 40;
        BoundaryAuthoringOptions options;
        options.automatic_dimension_placement = true;
        BoundaryAuthoringSession session(BoundaryAuthoringMode::draw_first, options, policy);
        (void)session.anchor({0.0, 0.0});
        require_persistable(session);
        bool hit_limit = false;
        for (int index = 1; index <= 128; ++index) {
            const auto before = session.recovery_checkpoint();
            const auto roots = session.structural_stats();
            try {
                (void)session.add_line_to({static_cast<double>(index), 0.0});
            } catch (const std::invalid_argument&) {
                require(session.recovery_checkpoint() == before &&
                            session.structural_stats().history_root_identity == roots.history_root_identity,
                        "wire-budget rejection must preserve the last accepted state");
                hit_limit = true;
                break;
            }
            require_persistable(session);
        }
        require(hit_limit, "the selected raw policy must stop a bounded live input sequence");
        session.set_pointer({-0.0, std::nextafter(1.0, 2.0)});
        require_persistable(session);
        while (session.undo()) require_persistable(session);
        while (session.redo()) require_persistable(session);
    }
}

void invalid_policy_is_rejected_before_session_use() {
    auto policy = boundary_authoring_recovery_default_limits;
    policy.max_actions = 0;
    rejected([&] { BoundaryAuthoringSession invalid(BoundaryAuthoringMode::draw_first, {}, policy); },
             "a zero action policy must be rejected during construction");
}
}  // namespace

int main() {
    sketch::testing::noninteractive_errors();
    try {
        action_ceiling_preserves_undo_redo_and_branching();
        typed_restore_uses_destination_policy_and_preserves_state_on_failure();
        raw_wire_limits_apply_before_live_state_becomes_unsavable();
        invalid_policy_is_rejected_before_session_use();
        std::cout << "Shared live, typed restore and wire admission tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "boundary_authoring_admission_tests: " << error.what() << '\n';
        return 1;
    }
}
