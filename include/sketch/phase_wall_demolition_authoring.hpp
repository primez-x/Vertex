#pragma once

#include "sketch/architectural_selection_removal.hpp"
#include "sketch/phase_constraint_authoring.hpp"
#include "sketch/phase_wall_demolition.hpp"
#include "sketch/physical_wall_phase_review.hpp"
#include "sketch/wall_join_removal.hpp"

namespace sketch {

struct PhaseWallDemolitionAuthoring {
    PhaseWallDemolitionIntent wall_demolition;
    nlohmann::json other_authoring=nullptr;
    ArchitecturalSelectionRemovalIntent ordinary;
    std::vector<std::string> opening_ids;
    nlohmann::json room_review_intent=nullptr;
    std::vector<std::string> ordinary_wall_ids;
    PhysicalWallJoinRemovalAdditionalIdentities wall_additional_identities;
};

// Closed inner one retains its exact six-field authority. Closed inner two
// adds actual ordinary wall roots and their source-derived join destinations;
// historical children retain their exact source-bound wire authority.
[[nodiscard]] nlohmann::json encode_phase_wall_demolition_authoring(
    const PhaseWallDemolitionAuthoring& intent);
[[nodiscard]] PhaseWallDemolitionAuthoring decode_phase_wall_demolition_authoring(
    const nlohmann::json& value);

struct PhaseWallDemolitionAuthoringPreview {
    std::map<std::string,Entity,std::less<>> edited_entities;
    PhysicalWallRoomPhaseReviewInventory room_inventory;
    bool needs_room_review{};
};

// The complete analytical stage is bound to the original actual capture.
// No Document preview, fabricated snapshot or automatic room decision occurs.
[[nodiscard]] PhaseWallDemolitionAuthoringPreview inspect_phase_wall_demolition_authoring(
    const DocumentSnapshot& actual,const PhaseConstraintAuthoringIntent& intent);
[[nodiscard]] std::map<std::string,Entity,std::less<>> replay_phase_wall_demolition_authoring(
    const std::map<std::string,Entity,std::less<>>& actual,
    const PhaseConstraintAuthoringIntent& intent);
// Semantic outer34 only. Document retains revision/history/save/asset lifetime
// admission and final publication authority, which entity maps cannot prove.
[[nodiscard]] ApplyBoundaryConstraintChanges phase_wall_demolition_authoring_command(
    const PhaseConstraintAuthoringIntent& intent);

} // namespace sketch
