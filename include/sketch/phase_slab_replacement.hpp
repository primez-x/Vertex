#pragma once

#include "sketch/phase_slab_profile_edit.hpp"
#include "sketch/slab_geometry_edit.hpp"
#include "sketch/slab_layer_stack_edit.hpp"

#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace sketch {

using PhaseSlabReplacementEntities = std::map<std::string, Entity, std::less<>>;
using PhaseSlabReplacementIdentityMap = std::map<std::string, std::string, std::less<>>;
using PhaseSlabReplacementHostedInstanceKey = std::pair<std::string, std::string>;
using PhaseSlabReplacementHostedInstanceIdentityMap = std::map<PhaseSlabReplacementHostedInstanceKey, std::string>;

struct PhaseSlabReplacementDiagnostic {
    std::string entity_id;
    std::string reason;
    bool blocking{true};
    bool operator==(const PhaseSlabReplacementDiagnostic&) const = default;
};

// Inventory derived from the actual source, never supplied clone authority.
struct PhaseSlabReplacementPlan {
    std::string registry_id;
    std::string alternative_id;
    std::vector<std::string> seed_slab_ids;
    // Actual seed owners plus each affected hosted catalog, exactly once.
    std::vector<std::string> required_entity_ids;
    // Actual slab layers and bound view overlays share the document namespace.
    std::vector<std::string> required_child_ids;
    std::vector<PhaseSlabReplacementDiagnostic> diagnostics;
    // Embedded identities are qualified by their actual source catalog.
    std::vector<PhaseSlabReplacementHostedInstanceKey> required_hosted_instance_ids;
    [[nodiscard]] bool ready() const noexcept;
    bool operator==(const PhaseSlabReplacementPlan&) const = default;
};

struct PhaseSlabReplacementResult {
    PhaseSlabReplacementEntities entities;
    // Complete source roster, including removed layers whose mapped IDs are
    // reserved without becoming live proposed children.
    PhaseSlabReplacementIdentityMap original_to_proposed;
    // Entity/child/qualified instance mapping outputs and new stack row IDs.
    std::vector<std::string> fresh_identity_ids;
    PhaseSlabReplacementHostedInstanceIdentityMap original_to_hosted_instance_proposed;
};

[[nodiscard]] PhaseSlabReplacementPlan inspect_phase_slab_replacement_plan(
    const PhaseSlabReplacementEntities& source, const std::vector<std::string>& seed_slab_ids,
    const std::string& registry_id, const std::string& alternative_id);

// Reinspect the source, independently replay typed inputs and derive fresh
// owners. Shared-baseline originals remain exact; qualified presentation rows
// are additive. Ordinary geometry retains its actual owner and child IDs.
// Exactly one edit family is nonempty. Geometry may additionally include
// actual ordinary/proposed owners, independently partitioned from the source.
// New stack rows retain their declared fresh IDs; existing rows remap through
// the complete source-derived mapping.
// Hosted catalogs gain exact selected-instance copies. Their originals stay
// live and exact; only seed slabs acquire demolition membership. Model XYZ
// similarities and rigid plan transforms also transform copied placement.
// Hosted nonunit plan scaling refuses until an XY-only physical codec exists.
// The enclosing Document reserves fresh identities across retained history.
[[nodiscard]] PhaseSlabReplacementResult replay_phase_slab_replacement(
    const PhaseSlabReplacementEntities& source, const PhaseSlabReplacementPlan& plan,
    const PhaseSlabReplacementIdentityMap& identities,
    const std::vector<SlabProfileEditIntent>& slab_profiles,
    const std::vector<SlabLayerStackEditIntent>& slab_stacks = {},
    const std::vector<SlabGeometryEditIntent>& slab_geometry = {},
    const std::vector<SlabGeometryEditIntent>& ordinary_geometry = {},
    const PhaseSlabReplacementHostedInstanceIdentityMap& hosted_instance_identities = {});

struct PhaseSlabReplacementAuthoring {
    std::string registry_id;
    std::string alternative_id;
    std::vector<std::string> seed_slab_ids;
    PhaseSlabReplacementIdentityMap identities;
    std::vector<SlabProfileEditIntent> slab_profiles;
    std::vector<SlabLayerStackEditIntent> slab_stacks;
    std::vector<SlabGeometryEditIntent> slab_geometry;
    std::vector<SlabGeometryEditIntent> ordinary_geometry;
    PhaseSlabReplacementHostedInstanceIdentityMap hosted_instance_identities;
};

// Exact v1 fields: version, registry_id, alternative_id, seed_slab_ids,
// identities and slab_profiles. Exclusive v2 replaces slab_profiles with
// slab_stacks; exclusive v3 uses slab_geometry. Geometry carries mathematical
// intent only. Exclusive v4 adds ordinary_geometry to v3's six fields and
// requires both geometry lists nonempty. Seeds exactly match the changed
// baseline targets; ordinary targets are disjoint actual changed owners.
// Hosted replacement v5 has exactly ten fields: all three primary edit arrays,
// ordinary_geometry, and hosted_instance_identities accompany the five common
// fields. Exactly one primary array is nonempty. Qualified rows are sorted
// {catalog_id, instance_id, proposed_instance_id}; replay derives their exact
// roster from actual source slots. V1-V4 retain their original encoding.
[[nodiscard]] nlohmann::json encode_phase_slab_replacement_authoring(
    const PhaseSlabReplacementAuthoring& authoring);
[[nodiscard]] PhaseSlabReplacementAuthoring decode_phase_slab_replacement_authoring(
    const nlohmann::json& value);
[[nodiscard]] PhaseSlabReplacementEntities replay_phase_slab_replacement_authoring(
    const PhaseSlabReplacementEntities& source, const PhaseSlabReplacementAuthoring& authoring);

struct PhaseSlabProfileReplacementRequest {
    std::string registry_id;
    std::string alternative_id;
    std::vector<std::string> seed_slab_ids;
    bool operator==(const PhaseSlabProfileReplacementRequest&) const = default;
};

struct PhaseSlabGeometryEditPartition {
    std::vector<SlabGeometryEditIntent> baseline_geometry;
    std::vector<SlabGeometryEditIntent> ordinary_geometry;
    std::optional<PhaseSlabProfileReplacementRequest> replacement;
};

// Independently replay the full actual map, discard exact no-ops and classify
// changed targets from saved membership. Shared-baseline targets require one
// actual active alternative; no supplied role list establishes authority.
[[nodiscard]] PhaseSlabGeometryEditPartition partition_phase_slab_geometry_edits(
    const PhaseSlabReplacementEntities& source, const std::vector<SlabGeometryEditIntent>& slab_geometry);

// Source-equivalent batches and ordinary/proposed edits require no allocation.
// Shared-baseline edits require one actual active alternative; mixed scopes,
// inactive/demolished owners and overlapping membership are refused.
[[nodiscard]] std::optional<PhaseSlabProfileReplacementRequest> phase_slab_profile_replacement_request(
    const PhaseSlabReplacementEntities& source, const std::vector<SlabProfileEditIntent>& slab_profiles);
[[nodiscard]] std::optional<PhaseSlabProfileReplacementRequest> phase_slab_layer_stack_replacement_request(
    const PhaseSlabReplacementEntities& source, const std::vector<SlabLayerStackEditIntent>& slab_stacks);
[[nodiscard]] std::optional<PhaseSlabProfileReplacementRequest> phase_slab_geometry_replacement_request(
    const PhaseSlabReplacementEntities& source, const std::vector<SlabGeometryEditIntent>& slab_geometry);

} // namespace sketch
