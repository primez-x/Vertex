#pragma once

#include "sketch/structural_hosted_components.hpp"

#include <tuple>

namespace sketch {

using StructuralCloneEntities = StructuralHostedEntities;
using StructuralCloneIdentityMap = StructuralHostedIdentityMap;
using StructuralCloneHostedInstanceKey = StructuralHostedInstanceKey;
using StructuralCloneHostedInstanceIdentityMap = StructuralHostedInstanceIdentityMap;
// Source child namespaces belong to a saved view inside its actual entity.
using StructuralCloneOverlayKey = std::tuple<std::string, std::string, std::string>;
using StructuralCloneOverlayIdentityMap = std::map<StructuralCloneOverlayKey, std::string>;

struct StructuralCloneDiagnostic {
    std::string entity_id;
    std::string reason;
    bool blocking{true};
    bool operator==(const StructuralCloneDiagnostic&) const = default;
};

struct StructuralClonePlan {
    std::vector<std::string> selected_object_ids;
    // Actual active physical owners and affected hosted catalogs, exactly once.
    std::vector<std::string> required_entity_ids;
    // (actual view entity ID, saved view ID, overlay ID), exactly once.
    // Repeated local names in distinct saved views retain separate authority.
    std::vector<StructuralCloneOverlayKey> required_overlay_ids;
    std::vector<StructuralCloneHostedInstanceKey> required_hosted_instance_ids;
    std::vector<StructuralCloneDiagnostic> diagnostics;
    [[nodiscard]] bool ready() const noexcept;
    bool operator==(const StructuralClonePlan&) const = default;
};

// Discovery reads actual saved active column/beam owners, including baseline
// owners, and their qualified hosted rows. It grants no mutation authority.
[[nodiscard]] StructuralClonePlan inspect_structural_clone_plan(
    const StructuralCloneEntities& actual, const std::vector<std::string>& selected_object_ids);

struct StructuralCloneAuthoring {
    // Exactly one typed edit per copied actual owner, including identity edits.
    std::vector<StructuralObjectEditIntent> edits;
    StructuralCloneIdentityMap identities;
    StructuralCloneOverlayIdentityMap overlay_identities;
    StructuralCloneHostedInstanceIdentityMap hosted_instance_identities;
};

// Closed v1: version, edits, identities, overlay_identities,
// hosted_instance_identities. Overlay rows contain view_entity_id,
// saved_view_id, overlay_id and proposed_overlay_id. Hosted
// rows contain catalog_id, instance_id and proposed_instance_id. Computed
// presentation aliases are never authored identity keys.
[[nodiscard]] nlohmann::json encode_structural_clone_authoring(const StructuralCloneAuthoring& authoring);
[[nodiscard]] StructuralCloneAuthoring decode_structural_clone_authoring(const nlohmann::json& value);

// Complete additive candidate, derived from actual-map typed world/profile
// replay. Originals and all registries stay exact. Copies own private catalogs
// with only selected hosted rows and retain raw definitions/order/overrides.
// Known saved-view and object-annotation rows append copies for both physical
// owners and computed hosted aliases. Unknown affected references refuse.
// The enclosing Document reserves retained-history/assets and enrolls copies.
[[nodiscard]] StructuralCloneEntities replay_structural_clone_authoring(
    const StructuralCloneEntities& actual, const StructuralCloneAuthoring& authoring);

} // namespace sketch
