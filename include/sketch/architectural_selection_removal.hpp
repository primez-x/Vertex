#pragma once

#include "sketch/roof_removal.hpp"

#include <optional>

namespace sketch {

struct ArchitecturalSelectionRemovalIntent {
    std::vector<std::string> object_ids;
    // Actual catalog/instance keys; presentation aliases carry no authority.
    std::vector<std::pair<std::string, std::string>> components;
    RoofRemovalAdditionalIdentities roof_additional_identities;
    // Separate saved authority for exact qualified embedded extent dimensions.
    // Historical/default intents retain their original deletion consequences.
    bool complete_component_dimensions = false;
    // Separate physical-owner authority for dimensions on actual retired hosted
    // rows. Requires admitted hosts and is exclusive with component-only intent.
    bool complete_hosted_component_dimensions = false;
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
// Component membership retirement is an independent opt-in. It requires the
// admitted component-host lane. Physical roof owners require the separate roof
// membership opt-in and complete roof-hosted catalog consequences.
// Wide catalog-local identities are a separate grammar opt-in for explicit
// coordinated membership replay; historical complete producers stay strict.
// A required component choice needs membership completion and is forwarded to
// every leaf against this same complete actual source before native admission.
// Hosted dimension completion requires actual selected owners and admitted
// component hosts; it supplies no additional family or phase-demolition authority.
[[nodiscard]] RoofRemovalEntities replay_architectural_selection_removal(
    const RoofRemovalEntities& actual, const ArchitecturalSelectionRemovalIntent& intent,
    bool allow_manufactured_opening_hosts = false,
    bool complete_roof_hosted_catalog_consequences = false,
    bool complete_wall_hosted_catalog_consequences = false,
    bool complete_placed_catalog_consequences = false,
    bool complete_corner_window_removal = false,
    bool complete_component_phase_memberships = false,
    bool complete_roof_component_phase_memberships = false,
    bool allow_wide_component_local_ids = false,
    const std::optional<std::pair<std::string, std::string>>& required_component_phase_choice = std::nullopt);

} // namespace sketch
