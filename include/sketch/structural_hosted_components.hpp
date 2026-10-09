#pragma once

#include "sketch/assembly_document_adapter.hpp"
#include "sketch/structural_object_edit.hpp"

namespace sketch {

using StructuralHostedEntities = std::map<std::string, Entity, std::less<>>;
using StructuralHostedIdentityMap = std::map<std::string, std::string, std::less<>>;
using StructuralHostedInstanceKey = std::pair<std::string, std::string>;
using StructuralHostedInstanceIdentityMap = std::map<StructuralHostedInstanceKey, std::string>;

struct StructuralHostedComponentDiagnostic {
    std::string entity_id;
    std::string reason;
    bool blocking{true};
    bool operator==(const StructuralHostedComponentDiagnostic&) const = default;
};

// Actual-source inventory only. Local instance identities are catalog-qualified;
// computed render identities are never supplied as copy authority.
struct StructuralHostedComponentPlan {
    std::vector<std::string> host_ids;
    std::vector<std::string> catalog_ids;
    std::vector<StructuralHostedInstanceKey> instance_ids;
    EmbeddedAssemblyPresentationIds original_presentation_ids;
    std::vector<StructuralHostedComponentDiagnostic> diagnostics;
    [[nodiscard]] bool ready() const noexcept;
    bool operator==(const StructuralHostedComponentPlan&) const = default;
};

[[nodiscard]] StructuralHostedComponentPlan inspect_structural_hosted_components(
    const StructuralHostedEntities& actual, const std::vector<std::string>& host_ids);

// Physical authoring is in the actual host's source frame. Detached view
// points and world-authored profiles follow F*G*F^-1, resolving F from source.
[[nodiscard]] AssemblyTransform structural_world_transform(const StructuralHostedEntities& actual,
    const std::string& host_id, const ArchitecturalGroupTransform& transform);
// Legacy host copies retain their source-geometry frame; type-owned embedded
// profiles retain world roots. Only actual typed expansion chooses the frame.
[[nodiscard]] AssemblyTransform structural_hosted_transform(const StructuralHostedEntities& actual,
    const std::string& catalog_id, const std::string& instance_id,
    const ArchitecturalGroupTransform& transform);

// Ordinary geometry authority only: independently replay typed physical edits
// and patch actual hosted catalogs. No roles, copies, enrollment or quantities.
// Profile/endpoint edits retain placement scalars. Type-owned world profiles
// compose Gworld*A; host-derived source geometry uses Gsource*A*Gsource^-1.
[[nodiscard]] StructuralHostedEntities replay_structural_hosted_component_geometry(
    const StructuralHostedEntities& actual, const std::vector<StructuralObjectEditIntent>& edits);

struct StructuralHostedComponentCopyResult {
    // New private catalogs only; actual source catalogs remain untouched.
    std::vector<Entity> catalogs;
    StructuralHostedIdentityMap original_to_proposed_presentation;
    std::vector<std::string> fresh_aliases;
};

// Host-map keys must equal the actual changed typed edit targets. An explicit
// independent copy may opt into unchanged targets; replacement callers retain
// changed-target-only authority. Catalog and
// qualified instance maps must equal the independently inspected hosted roster.
// Scratch proposed hosts derive from typed replay; only selected hosted rows are
// copied, preserving raw definitions, overrides, metadata and source row order.
// The caller owns phase/presentation integration and retained-history identity
// reservation, and must recompute aliases in its complete final candidate.
[[nodiscard]] StructuralHostedComponentCopyResult copy_structural_hosted_components(
    const StructuralHostedEntities& actual, const std::vector<StructuralObjectEditIntent>& edits,
    const StructuralHostedIdentityMap& object_ids, const StructuralHostedIdentityMap& catalog_ids,
    const StructuralHostedInstanceIdentityMap& hosted_instance_ids,
    bool include_unchanged_hosts = false);

// Admission scratch copy only. Validate the actual supported catalog, then omit
// only known instance-ID and placement-host slots. Unknown siblings stay exact.
[[nodiscard]] Entity structural_hosted_catalog_opaque_remainder(const Entity& actual_catalog);

} // namespace sketch
