#pragma once

#include "sketch/document.hpp"

#include <cstddef>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace sketch {

using RoofCloneEntities = std::map<std::string, Entity, std::less<>>;
using RoofCloneIdentityMap = std::map<std::string, std::string, std::less<>>;
// Embedded source instance identities are local to their actual catalog.
using RoofCloneHostedInstanceKey = std::pair<std::string, std::string>;
using RoofCloneHostedInstanceIdentityMap = std::map<RoofCloneHostedInstanceKey, std::string>;

struct RoofCloneDiagnostic {
    std::string entity_id;
    std::string reason;
    bool blocking{true};
    bool operator==(const RoofCloneDiagnostic&) const = default;
};

struct RoofClonePlan {
    std::vector<std::string> selected_roof_ids;
    // Explicit roofs, wholly selected joins, qualified bound dimensions and,
    // when opted in, affected hosted assembly catalogs.
    std::vector<std::string> required_entity_ids;
    // Each actual owned opening and copied bound view overlay, exactly once.
    std::vector<std::string> required_child_ids;
    std::size_t copied_view_object_reference_count{};
    std::size_t copied_view_appearance_count{};
    std::size_t copied_annotation_override_count{};
    std::vector<RoofCloneDiagnostic> diagnostics;
    // Legacy discovery/replay remains the default for retained copy proofs.
    bool include_hosted_instances{false};
    std::vector<RoofCloneHostedInstanceKey> required_hosted_instance_ids;
    [[nodiscard]] bool ready() const noexcept;
    bool operator==(const RoofClonePlan&) const = default;
};

struct RoofCloneResult {
    RoofCloneEntities entities;
    RoofCloneIdentityMap original_to_clone;
    std::vector<std::string> fresh_identity_ids;
    RoofCloneHostedInstanceIdentityMap original_to_hosted_instance_clone;
};

// Reads actual saved active roof owners, including baseline owners. A join is
// copied only when every member is explicitly selected. Partial selections
// become independent roofs retaining the effective join material assignment.
[[nodiscard]] RoofClonePlan inspect_roof_clone_plan(
    const RoofCloneEntities& source, const std::vector<std::string>& selected_roof_ids,
    bool include_hosted_instances = false);

// Reinspects source and requires an exact fresh mapping for every entity/child
// slot. Source owners and phase registries remain exact. Qualified presentation
// rows are appended to their existing owners. This performs no transformation
// or phase enrollment; enclosing Document creation reserves identities across
// history and enrolls new owners. Retained derivation archives are provenance
// and stay byte-equivalent, including their historical owner and cut IDs.
// Hosted opt-in copies each affected raw catalog with only selected roof-hosted
// instances. Catalog-local definitions/materials and numeric forms remain exact;
// actual catalog, hosted instance and host slots gain fresh identities. Copied
// material assignments referring to a copied catalog follow that catalog. The
// selected hosted instances' actual generated aliases gain additive qualified
// view, appearance, overlay and annotation rows using the candidate's aliases.
// exact qualified instance map is required and all fresh names are returned for
// history reservation. Placement movement remains the caller's responsibility.
[[nodiscard]] RoofCloneResult replay_roof_clone(
    const RoofCloneEntities& source, const RoofClonePlan& plan,
    const RoofCloneIdentityMap& identities,
    const RoofCloneHostedInstanceIdentityMap& hosted_instance_identities = {});

} // namespace sketch
