#pragma once

#include "sketch/document.hpp"

#include <cstddef>
#include <map>
#include <string>
#include <vector>

namespace sketch {

using RoofCloneEntities = std::map<std::string, Entity, std::less<>>;
using RoofCloneIdentityMap = std::map<std::string, std::string, std::less<>>;

struct RoofCloneDiagnostic {
    std::string entity_id;
    std::string reason;
    bool blocking{true};
    bool operator==(const RoofCloneDiagnostic&) const = default;
};

struct RoofClonePlan {
    std::vector<std::string> selected_roof_ids;
    // Explicit roofs, wholly selected joins and qualified bound dimensions.
    std::vector<std::string> required_entity_ids;
    // Each actual owned opening and copied bound view overlay, exactly once.
    std::vector<std::string> required_child_ids;
    std::size_t copied_view_object_reference_count{};
    std::size_t copied_view_appearance_count{};
    std::size_t copied_annotation_override_count{};
    std::vector<RoofCloneDiagnostic> diagnostics;
    [[nodiscard]] bool ready() const noexcept;
    bool operator==(const RoofClonePlan&) const = default;
};

struct RoofCloneResult {
    RoofCloneEntities entities;
    RoofCloneIdentityMap original_to_clone;
    std::vector<std::string> fresh_identity_ids;
};

// Reads actual saved active roof owners, including baseline owners. A join is
// copied only when every member is explicitly selected. Partial selections
// become independent roofs retaining the effective join material assignment.
[[nodiscard]] RoofClonePlan inspect_roof_clone_plan(
    const RoofCloneEntities& source, const std::vector<std::string>& selected_roof_ids);

// Reinspects source and requires an exact fresh mapping for every entity/child
// slot. Source owners and phase registries remain exact. Qualified presentation
// rows are appended to their existing owners. This performs no transformation
// or phase enrollment; enclosing Document creation reserves identities across
// history and enrolls new owners. Retained derivation archives are provenance
// and stay byte-equivalent, including their historical owner and cut IDs.
[[nodiscard]] RoofCloneResult replay_roof_clone(
    const RoofCloneEntities& source, const RoofClonePlan& plan,
    const RoofCloneIdentityMap& identities);

} // namespace sketch
