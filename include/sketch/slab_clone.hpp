#pragma once

#include "sketch/document.hpp"

#include <cstddef>
#include <map>
#include <string>
#include <vector>

namespace sketch {

using SlabCloneEntities = std::map<std::string, Entity, std::less<>>;
using SlabCloneIdentityMap = std::map<std::string, std::string, std::less<>>;

struct SlabCloneDiagnostic {
    std::string entity_id;
    std::string reason;
    bool blocking{true};
    bool operator==(const SlabCloneDiagnostic&) const = default;
};

struct SlabClonePlan {
    std::vector<std::string> selected_slab_ids;
    // Actual active slab owners, including floor, ceiling and foundation kinds.
    std::vector<std::string> required_entity_ids;
    // Actual source layers and copied bound view overlays, exactly once.
    std::vector<std::string> required_child_ids;
    std::size_t copied_view_object_reference_count{};
    std::size_t copied_view_appearance_count{};
    std::size_t copied_view_overlay_count{};
    std::size_t copied_annotation_override_count{};
    std::vector<SlabCloneDiagnostic> diagnostics;
    [[nodiscard]] bool ready() const noexcept;
    bool operator==(const SlabClonePlan&) const = default;
};

struct SlabCloneResult {
    SlabCloneEntities entities;
    SlabCloneIdentityMap original_to_clone;
    std::vector<std::string> fresh_identity_ids;
};

// Discover a bounded, unique explicit selection from the actual saved active
// source, including baseline owners. Unsupported affected live dependencies
// block copying; native slabs do not acquire measurement-boundary authority.
[[nodiscard]] SlabClonePlan inspect_slab_clone_plan(
    const SlabCloneEntities& source, const std::vector<std::string>& selected_slab_ids);

// Reinspect the source and require an exact fresh owner/layer/overlay mapping.
// Originals and phase registries remain exact; qualified presentation rows gain
// additive copies retaining their unknown fields. Scalar/profile/geometry and
// opaque receipts remain exact. Validated geometry operation slab IDs and
// retired receipt layer IDs are historical provenance and never remapped.
// This performs no transform or phase enrollment. Enclosing Document creation
// reserves identities throughout history and enrolls the new owners.
[[nodiscard]] SlabCloneResult replay_slab_clone(
    const SlabCloneEntities& source, const SlabClonePlan& plan,
    const SlabCloneIdentityMap& identities);

} // namespace sketch
