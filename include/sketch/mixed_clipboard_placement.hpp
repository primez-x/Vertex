#pragma once

#include "sketch/mixed_clipboard_transfer.hpp"
#include "sketch/roof_opening_group_edit.hpp"

#include <map>
#include <utility>

namespace sketch {

using MixedClipboardIdentityMapping = std::map<std::string, std::string, std::less<>>;
using MixedClipboardRoofOpeningMapping = std::map<std::pair<std::string, std::string>, std::string>;

struct MixedClipboardPlacementRequest {
    MixedClipboardTransfer transfer;
    // Prepared by the ordinary graph producer on this same actual snapshot.
    // Contains fresh additions only; reused material carriers have no change.
    ApplyEntityChanges ordinary;
    MixedClipboardIdentityMapping identity_mapping;
    MixedClipboardIdentityMapping material_catalog_mapping;
    MixedClipboardRoofOpeningMapping roof_opening_identity_mapping;
    std::vector<CornerWindowCloneRequest> corners;
    // A destination ID names an actual roof or the exact mapped ordinary roof
    // addition. Selecting a source roof dominates its own transported children;
    // independent children may target a different mapped fresh roof.
    std::vector<RoofOpeningGroupClonePlacement> skylights;
    std::vector<Entity> imported_material_catalogs;
};

struct MixedClipboardPlacement {
    // Includes each copied roof once, with its complete independent skylight
    // cohort already replayed into the original prepared ordinary addition.
    std::vector<EntityChange> fresh_entity_changes;
    // Existing actual hosts only; copied hosts never acquire original-map edit
    // authority or appear in roof_candidates.
    std::vector<RoofEditIntent> roof_intents;
    // Actual original-map replay consequences only, keyed by changed owner.
    // Root supplies baseline replacement/phase augmentation where required.
    std::map<std::string, Entity, std::less<>> roof_candidates;
    MixedClipboardIdentityMapping identity_mapping;
    MixedClipboardIdentityMapping material_catalog_mapping;
    MixedClipboardRoofOpeningMapping roof_opening_identity_mapping;
};

// Passive admission, exact family coverage, selected-host dominance, shared
// material reconciliation and one fresh identity reservation against complete
// passive content plus all retained/undone history. No publication or command
// authority is returned. Caller completes typed whole-command/phase admission,
// native geometry preview, actual source fencing and one apply.
[[nodiscard]] MixedClipboardPlacement prepare_mixed_clipboard_placement(
    const DocumentSnapshot& destination, const MixedClipboardPlacementRequest& request,
    Revision expected_revision);

} // namespace sketch
