#pragma once

#include "sketch/document.hpp"

#include <cstddef>
#include <map>
#include <string>
#include <vector>

namespace sketch {

using RoofRemovalEntities = std::map<std::string, Entity, std::less<>>;
using RoofRemovalAdditionalIdentities = std::map<std::string, std::vector<std::string>, std::less<>>;

struct RoofRemovalDiagnostic {
    std::string entity_id;
    std::string reason;
    bool blocking{true};
    bool operator==(const RoofRemovalDiagnostic&) const = default;
};

struct RoofRemovalPlan {
    std::vector<std::string> selected_roof_ids;
    // Selected roofs and old joins with no surviving component of two or more.
    std::vector<std::string> removed_owner_ids;
    // Includes supported associative dimensions actually targeting removed owners.
    std::vector<std::string> removed_entity_ids;
    // Actual native-contact components, including singleton survivors. Each
    // component and the component sequence retain the old join's authored order.
    std::map<std::string, std::vector<std::vector<std::string>>, std::less<>> surviving_join_components;
    // Keys are actual old join / bound-overlay IDs; only nonzero counts occur.
    // First surviving >=2 component keeps the old join ID; slots follow it.
    std::map<std::string, std::size_t, std::less<>> additional_identity_counts;
    std::vector<RoofRemovalDiagnostic> diagnostics;
    [[nodiscard]] bool ready() const noexcept;
    bool operator==(const RoofRemovalPlan&) const = default;
};

struct RoofRemovalResult {
    RoofRemovalEntities entities;
    std::vector<std::string> fresh_identity_ids;
};

// Explicit actual active ordinary/proposed roof owners alone confer authority.
// Shared-baseline demolition in a saved active alternative uses the existing
// typed demolition path. Unsupported affected references block this operation.
[[nodiscard]] RoofRemovalPlan inspect_roof_removal_plan(
    const RoofRemovalEntities& source, const std::vector<std::string>& selected_roof_ids);

// Rederives the entire operation from source and explicit selection. Supplied
// identities must exactly fill actual additional join/overlay slots and must
// not occur anywhere in retained source JSON. Inputs remain untouched.
[[nodiscard]] RoofRemovalResult replay_roof_removal(
    const RoofRemovalEntities& source, const std::vector<std::string>& selected_roof_ids,
    const RoofRemovalAdditionalIdentities& additional_identities);

} // namespace sketch
