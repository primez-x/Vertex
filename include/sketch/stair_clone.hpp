#pragma once

#include "sketch/stair_transform.hpp"

#include <map>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace sketch {

using StairCloneEntities = std::map<std::string, Entity, std::less<>>;
using StairCloneIdentityMap = std::map<std::string, std::string, std::less<>>;
using StairCloneChildKey = std::pair<std::string, std::string>;
using StairCloneChildIdentityMap = std::map<StairCloneChildKey, std::string>;
using StairCloneHostedInstanceKey = std::pair<std::string, std::string>;
using StairCloneHostedInstanceIdentityMap = std::map<StairCloneHostedInstanceKey, std::string>;
using StairCloneOverlayKey = std::tuple<std::string, std::string, std::string>;
using StairCloneOverlayIdentityMap = std::map<StairCloneOverlayKey, std::string>;

struct StairCloneDiagnostic {
    std::string entity_id;
    std::string reason;
    bool blocking{true};
    bool operator==(const StairCloneDiagnostic&) const = default;
};

struct StairClonePlan {
    std::vector<std::string> selected_object_ids;
    // Selected actual stairs/independent rails, active attached rails and the
    // private catalogs needed by their actual hosted rows.
    std::vector<std::string> required_entity_ids;
    std::vector<StairCloneChildKey> required_child_ids;
    std::vector<StairCloneHostedInstanceKey> required_hosted_instance_ids;
    std::vector<StairCloneOverlayKey> required_overlay_ids;
    std::vector<StairCloneDiagnostic> diagnostics;
    [[nodiscard]] bool ready() const noexcept;
    bool operator==(const StairClonePlan&) const = default;
};

// Exactly one captured actual-source operator per selected owner. Identity
// operators still copy the complete closure. Hosted rail roots require their
// selected actual host and an affine-equivalent operator, as in ordinary replay.
[[nodiscard]] StairClonePlan inspect_stair_clone_plan(
    const StairCloneEntities& actual, const std::vector<StairTransformIntent>& transforms);

struct StairCloneAuthoring {
    std::vector<StairTransformIntent> transforms;
    StairCloneIdentityMap identities;
    StairCloneChildIdentityMap child_identities;
    StairCloneHostedInstanceIdentityMap hosted_instance_identities;
    StairCloneOverlayIdentityMap overlay_identities;
};

// Closed v1: version, transforms, identities, child_identities,
// hosted_instance_identities, overlay_identities. Qualified row shapes match
// phase stair replacement, but no registry or destination scope is authored.
[[nodiscard]] nlohmann::json encode_stair_clone_authoring(const StairCloneAuthoring& authoring);
[[nodiscard]] StairCloneAuthoring decode_stair_clone_authoring(const nlohmann::json& value);

// Complete additive actual-source candidate. Originals, phase registries,
// organization, levels and assets remain exact. Only known saved presentation
// and object annotation rows append corresponding copied rows. Unknown affected
// references refuse; computed aliases are derived and never authored mappings.
// The enclosing Document reserves history/assets/render identities and performs
// any later explicit scope enrollment. It supplies no fabricated source map.
[[nodiscard]] StairCloneEntities replay_stair_clone_authoring(
    const StairCloneEntities& actual, const StairCloneAuthoring& authoring);

} // namespace sketch
