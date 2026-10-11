#pragma once

#include "sketch/selection_geometry_transform.hpp"

namespace sketch {

// Pure complete original-source derivation: prepares the typed selection, then
// replays exactly its required context/plane room reviews in supplied order.
// Every canonical review binds snapshot/authoring/save state to the original
// source and its entity digest to the preceding derived geometry/review stage.
// Selected room dimension placement is the source-derived requirement, never
// caller geometric authority. Explicitly removed references have no surviving
// placement, and retiring rooms require acknowledged removal of their callouts.
// Final selected room area callouts follow the
// captured operators around the explicitly reviewed final room geometry.
// The result includes the complete entity map and existing typed room topology
// transitions; neither a caller candidate map nor a supplemental lane is used.
// An unchanged geometry stage starts directly at the actual original source.
//
// This is not a publication command. Fresh identity reservation against retained
// history, identity lifetimes, full final Document admission and atomic live-head
// publication remain responsibilities of the enclosing authoring command.
[[nodiscard]] ReplayedPhysicalWallRoomReview replay_selection_geometry_review(
    const DocumentSnapshot& source, const SelectionGeometryTransformRequest& request,
    const std::vector<nlohmann::json>& reviewed_room_intents);

} // namespace sketch
