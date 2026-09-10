#pragma once

#include "sketch/boundary_authoring_recovery.hpp"

#include <bit>
#include <cstdint>
#include <stdexcept>
#include <string_view>

namespace sketch::testing::recovery_oracle {
using Json = nlohmann::json;

inline Json point(Vec2 value) {
    return Json::array({std::bit_cast<std::uint64_t>(value.x),
                        std::bit_cast<std::uint64_t>(value.y)});
}
inline Json optional_point(const std::optional<Vec2>& value) {
    return value ? point(*value) : Json(nullptr);
}
inline Json optional_string(const std::optional<std::string>& value) {
    return value ? Json(*value) : Json(nullptr);
}
inline Json pending(const PendingBoundaryDimension& value) {
    return {value.boundary_id, value.segment_id};
}
template <typename Range, typename Transform>
Json values(const Range& range, Transform transform) {
    auto result = Json::array();
    for (const auto& value : range) result.push_back(transform(value));
    return result;
}
inline Json segment(const IdentifiedSegment& value) {
    return {{"id", value.segment_id}, {"start_vertex", value.start_vertex_id},
            {"end_vertex", value.end_vertex_id}, {"start_bits", point(value.segment.start)},
            {"end_bits", point(value.segment.end)},
            {"sweep_bits", std::bit_cast<std::uint64_t>(value.segment.sweep_radians)}};
}
inline Json dimension(const BoundaryDimension& value) {
    return {{"id", value.id}, {"boundary_id", value.boundary_id},
            {"segment_id", value.segment_id}, {"text_bits", point(value.text_position)},
            {"placement", static_cast<int>(value.placement)},
            {"automatic_version", value.automatic_placement_version
                                      ? Json(*value.automatic_placement_version) : Json(nullptr)}};
}
inline Json receipt(const ConstructionReceipt& value) {
    return encode_construction_receipt(value);
}
inline Json active(const BoundaryDraftChain& value) {
    return {{"id", value.boundary_id}, {"type", value.type},
            {"classification", value.classification}, {"classified", value.classified},
            {"anchor_bits", point(value.anchor)}, {"segments", values(value.segments, segment)},
            {"receipts", values(value.receipts, receipt)},
            {"dimensions", values(value.dimensions, dimension)},
            {"pending", values(value.pending_dimensions, pending)}};
}
inline Json accepted(const AcceptedBoundaryChain& value) {
    return {{"id", value.boundary.id}, {"type", value.boundary.type},
            {"classification", value.classification}, {"classified", value.classified},
            {"anchor_bits", point(value.anchor)},
            {"segments", values(value.boundary.segments, segment)},
            {"receipts", values(value.receipts, receipt)},
            {"dimensions", values(value.dimensions, dimension)}};
}
inline Json view(const BoundaryAuthoringState& value) {
    return {{"mode", static_cast<int>(value.mode)}, {"phase", static_cast<int>(value.phase)},
            {"pen", static_cast<int>(value.pen_state)}, {"namespace", value.identity_namespace},
            {"pointer_bits", optional_point(value.pointer)},
            {"anchor_bits", optional_point(value.anchor)},
            {"classification", optional_string(value.classification)},
            {"active", value.active_chain ? active(*value.active_chain) : Json(nullptr)},
            {"pending_first", value.pending_dimension ? pending(*value.pending_dimension) : Json(nullptr)},
            {"pending", values(value.pending_dimensions, pending)},
            {"accepted", values(value.accepted_chains, accepted)},
            {"undo_depth", value.semantic_undo_depth}, {"redo_depth", value.semantic_redo_depth}};
}
inline Json session(const BoundaryAuthoringSession& value) {
    return {{"view", view(value.view())}, {"can_undo", value.can_undo()},
            {"can_redo", value.can_redo()},
            {"checkpoint_bytes", encode_boundary_authoring_recovery(value.recovery_checkpoint()).dump()}};
}
inline void branch(BoundaryAuthoringSession& value) {
    const auto state = value.view();
    if (state.active_chain) {
        if (!state.active_chain->pending_dimensions.empty()) {
            (void)value.place_automatic_dimension();
        } else if (state.pen_state == BoundaryPenState::down) {
            value.pen_up();
        } else {
            value.pen_down();
        }
    } else {
        value.set_classification("oracle_branch");
    }
}
inline Json history(const BoundaryAuthoringSession& source) {
    auto cursor = source;
    while (cursor.undo()) {}
    auto result = Json::array();
    do {
        auto record = session(cursor);
        auto branched = cursor;
        branch(branched);
        record["branch"] = session(branched);
        if (!branched.undo()) throw std::runtime_error("oracle branch must be undoable");
        record["branch_undo"] = session(branched);
        if (!branched.redo()) throw std::runtime_error("oracle branch must be redoable");
        record["branch_redo"] = session(branched);
        auto discarded = cursor;
        discarded.discard_redo_branch();
        record["discarded_redo"] = session(discarded);
        result.push_back(std::move(record));
    } while (cursor.redo());
    return result;
}
inline Json fixture(const BoundaryAuthoringSession& source, std::string_view label) {
    auto reset = source;
    reset.reset();
    auto cancelled = source;
    cancelled.cancel();
    return {{"label", label},
            {"checkpoint", encode_boundary_authoring_recovery(source.recovery_checkpoint())},
            {"initial", session(source)}, {"positions", history(source)},
            {"reset", session(reset)}, {"cancelled_view", view(cancelled.view())}};
}
}  // namespace sketch::testing::recovery_oracle
