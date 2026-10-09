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
[[nodiscard]] RoofRemovalEntities replay_architectural_selection_removal(
    const RoofRemovalEntities& actual, const ArchitecturalSelectionRemovalIntent& intent);

} // namespace sketch
