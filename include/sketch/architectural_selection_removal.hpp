#pragma once

#include "sketch/roof_removal.hpp"

namespace sketch {

struct ArchitecturalSelectionRemovalIntent {
    std::vector<std::string> object_ids;
    // Actual catalog/instance keys; presentation aliases carry no authority.
    std::vector<std::pair<std::string, std::string>> components;
    RoofRemovalAdditionalIdentities roof_additional_identities;
};

// Closed actual-selection producer. Roof and primitive/component lanes each
// independently admit the complete same source. Only the ordinary demolition
// composer combines their source-derived consequences. Wall/room roots require
// their dedicated authoring path. Inputs are never modified.
// Opening-hosted component admission is explicit; historical/default callers
// retain their existing body and replay authority.
// Complete roof-hosted catalog consequences are a separate explicit opt-in.
// Complete wall/opening catalog consequences are independent: new wall
// authoring can enable them without extending historical roof-only authority.
// Complete placed consequences additionally admit qualified rows on a sole
// active proposed opening without removing its active existing/proposed wall.
// The separate corner opt-in supplies complete owner-hosted component admission
// only for the explicit new corner selection producer; defaults stay unchanged.
[[nodiscard]] RoofRemovalEntities replay_architectural_selection_removal(
    const RoofRemovalEntities& actual, const ArchitecturalSelectionRemovalIntent& intent,
    bool allow_manufactured_opening_hosts = false,
    bool complete_roof_hosted_catalog_consequences = false,
    bool complete_wall_hosted_catalog_consequences = false,
    bool complete_placed_catalog_consequences = false,
    bool complete_corner_window_removal = false);

} // namespace sketch
