#pragma once

#include "sketch/architectural_selection_removal.hpp"
#include "sketch/drawing_selection_removal.hpp"

namespace sketch {

// Explicit ordinary selection authority. Architectural roots and qualified
// components are independent of drawing owners/annotation rows. A hosted
// skylight's roof must never enter object_ids merely because it is the host.
// Wall/opening/room-review and saved-alternative demolition use their own
// complete proofs; this intent supplies the ordinary architectural/drawing lane.
// Closed v4 records complete_component_dimensions with explicit membership flags
// for a component-only architectural lane; older versions retain their meaning.
// Closed v5 instead records complete_hosted_component_dimensions with explicit
// membership flags for actual selected architectural owners and their hosts.
struct ArchitecturalDrawingRemovalIntent {
    ArchitecturalSelectionRemovalIntent architectural;
    DrawingSelectionRemovalIntent drawing;
    bool allow_manufactured_opening_hosts{};
    // Closed v2 authority for actual component-row retirement and its typed
    // saved-phase memberships. False preserves the exact historical v1 wire.
    bool complete_component_phase_memberships = false;
    // Closed v3 extends that authority to actual selected roof owners and
    // complete original-source companion composition across removal leaves.
    bool complete_roof_component_phase_memberships = false;
};

[[nodiscard]] nlohmann::json encode_architectural_drawing_removal_intent(
    const ArchitecturalDrawingRemovalIntent& intent);
[[nodiscard]] ArchitecturalDrawingRemovalIntent decode_architectural_drawing_removal_intent(
    const nlohmann::json& value);

// Both leaves see the complete original snapshot. Drawing retirement is
// composed after the admitted architectural consequences, with actual source
// row authority and complete retained-history identity reservation.
[[nodiscard]] DrawingSelectionRemovalEntities replay_architectural_drawing_removal(
    const DocumentSnapshot& source, const ArchitecturalDrawingRemovalIntent& intent);

// Asset-free source-derived command; the controller owns final publication.
[[nodiscard]] ApplyEntityChanges prepare_architectural_drawing_removal(
    const DocumentSnapshot& source, const ArchitecturalDrawingRemovalIntent& intent,
    const std::string& message);

} // namespace sketch
