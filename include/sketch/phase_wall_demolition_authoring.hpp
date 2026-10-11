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
    bool complete_hosted_catalog_consequences{false};
    bool complete_corner_window_consequences{false};
    bool complete_actual_corner_window_cohorts{false};
    std::vector<std::string> explicit_corner_window_ids;
    bool complete_component_phase_memberships{false};
    nlohmann::json component_authoring=nullptr;
    bool complete_component_dimensions{false};
};

// Closed inner one retains its exact six-field authority. Closed inner two
// adds actual ordinary wall roots and their source-derived join destinations;
// closed inner three explicitly admits complete ordinary hosted catalog
// consequences, with optional ordinary walls. Historical children retain their
// exact source-bound wire authority. Closed inner four additionally derives
// complete actual corner-window consequences from the selected wall hosts and
// requires complete hosted catalog consequences. It grants no independent
// corner root or managed-cut removal authority.
// Closed inner five separately admits actual owner-classified host consequences
// and explicit corner roots. Baseline owners park their owner/two-cut aggregate;
// proposed/ordinary owners retire it, including corners on baseline wall hosts.
// Closed inner six separately completes qualified component memberships.
// Its nullable full phase-thirty child adds baseline demolition membership;
// physical leaves prune only their independently proved retired proposed rows.
// Closed inner seven separately completes qualified dimensions on those actual
// physically retired rows. Baseline semantic demolition retains rows/dimensions;
// independent owners sharing the local tuple remain exact. Historical inner
// one through six retain their authority and cannot nest coordinated inner nine.
[[nodiscard]] nlohmann::json encode_phase_wall_demolition_authoring(
    const PhaseWallDemolitionAuthoring& intent);
[[nodiscard]] PhaseWallDemolitionAuthoring decode_phase_wall_demolition_authoring(
    const nlohmann::json& value);
[[nodiscard]] bool phase_wall_demolition_complete_component_phase_memberships(
    const nlohmann::json& value);
[[nodiscard]] bool phase_wall_demolition_complete_component_dimensions(
    const nlohmann::json& value);
// Reserve complete source catalogs cumulatively before phase/scope resolution.
void preflight_phase_wall_demolition_component_catalogs(
    const std::map<std::string,Entity,std::less<>>& source,
    const nlohmann::json& value,const PhaseConstraintAuthoringIntent& enclosing);

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
