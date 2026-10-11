#pragma once

#include "sketch/assembly_phase_membership.hpp"
#include "sketch/document.hpp"

namespace sketch {

using AssemblyPhaseTransportEntities = std::map<std::string, Entity, std::less<>>;
using AssemblyPhaseCopiedRoots = std::map<AssemblyPhaseRoot, AssemblyPhaseRoot>;
using AssemblyPhaseCopiedHosts = std::map<std::string, std::string, std::less<>>;

// Read-only admission of a complete source-derived physical retirement set.
// Actual declared rows must be sole proposed members of the requested saved
// active choice. Every reached catalog, placement host and inherited opening/
// corner host must be active and cannot belong to a foreign physical registry,
// including unlisted rows. Retained active baseline carriers remain permitted.
// Unlisted rows remain unlisted; no membership is inferred.
// The full raw source and catalog work are bounded before model resolution.
void validate_assembly_phase_retirement_roots(
    const AssemblyPhaseTransportEntities& actual,
    const std::vector<AssemblyPhaseRoot>& retired_roots,
    std::string_view registry_id, std::string_view alternative_id);

// The typed physical producer supplies an independently admitted complete
// candidate and its exact source-derived copied row/host bindings. This helper
// grants no geometry, command, fresh enrollment or presentation-alias authority.
// With copied rows, source companions remain exact in the input candidate. Declared active
// rows reuse their actual companion: baseline sources gain demolition in the
// actual saved alternative, proposed sources retain their partition, and fresh
// destinations become proposed. Unlisted rows retain legacy behavior.
// Both actual registries must save the requested choice. Copied placement hosts
// must be actual baseline->demolished/fresh-proposed replacements in that same
// registry; cross-registry coupling refuses. Source rows remain source-exact.
// Complete-map membership validation precedes return; empty mappings validate
// and return the candidate exactly. Inputs are never mutated.
[[nodiscard]] AssemblyPhaseTransportEntities
transport_assembly_phase_memberships_for_host_copies(
    const AssemblyPhaseTransportEntities& source,
    const AssemblyPhaseTransportEntities& candidate,
    const AssemblyPhaseCopiedRoots& copied_roots,
    const AssemblyPhaseCopiedHosts& copied_hosts,
    std::string_view registry_id, std::string_view alternative_id);

// Ordinary additive copies supply an independently admitted complete candidate
// after actual physical phase enrollment. Source hosts, reached catalogs and
// companions remain raw-exact; original physical registry roles and saved
// choices remain unchanged. Only declared active source rows gain fresh roots
// in their actual companion. The fresh actual placement host supplies its
// registry and baseline-existing or saved-alternative-proposed destination role.
// Declared cross-registry coupling refuses; unlisted source rows remain unlisted.
// Their hosts may follow independently admitted ordinary phase enrollment. No
// allocation, caller phase choice, geometry or Document publication authority
// is supplied. Empty mappings still admit both complete maps and return exactly
// the candidate. Inputs are never mutated.
[[nodiscard]] AssemblyPhaseTransportEntities
transport_assembly_phase_memberships_for_independent_host_copies(
    const AssemblyPhaseTransportEntities& source,
    const AssemblyPhaseTransportEntities& candidate,
    const AssemblyPhaseCopiedRoots& copied_roots,
    const AssemblyPhaseCopiedHosts& copied_hosts);

// A producer independently admits additive embedded copy geometry and fresh
// qualified row identities. Mappings are same-catalog only and contain at most
// 1000 roots. Source rows, definitions, materials and catalog envelopes remain
// raw-exact; only the mapped fresh rows may be added. Actual retained hosts
// and opening/corner ancestry remain active and unchanged. Declared source row
// activity comes from its actual companion, never its shared catalog carrier.
// Reuse that exact companion: the saved baseline choice appends baseline roots,
// and a saved alternative appends proposed roots without source demolition.
// Original registry roles, choices and all companion partitions remain exact.
// Unlisted sources remain unlisted; no companion or geometry is synthesized.
// Both complete maps are admitted before return, including empty mappings.
// Supplies no allocation, physical admission or Document publication authority.
[[nodiscard]] AssemblyPhaseTransportEntities
transport_assembly_phase_memberships_for_embedded_copies(
    const AssemblyPhaseTransportEntities& source,
    const AssemblyPhaseTransportEntities& candidate,
    const AssemblyPhaseCopiedRoots& copied_roots);

// Independently derives actual unlisted carrier/complete host cohorts. Exact
// registry-to-fresh-required-companion allocations enroll destinations only.
// Missing/extraneous allocations and disagreeing physical registries refuse.
// Declared roots retain the existing three-argument transport semantics.
[[nodiscard]] AssemblyPhaseTransportEntities
transport_assembly_phase_memberships_for_embedded_copies(
    const AssemblyPhaseTransportEntities& source,
    const AssemblyPhaseTransportEntities& candidate,
    const AssemblyPhaseCopiedRoots& copied_roots,
    const std::map<std::string, std::string, std::less<>>& membership_entity_ids);

// A removal producer may leave unchanged source companions referencing rows
// now absent from its independently admitted candidate. Prune only those actual
// absences from every partition; present inactive rows remain declared. Removing
// a baseline row while its source registry has an active alternative refuses.
// Removing a registry permits removing its companion only when every bound
// catalog/row is absent. Existing source companions cannot otherwise disappear
// or be rewritten by the input candidate. No fresh enrollment is inferred.
[[nodiscard]] AssemblyPhaseTransportEntities prune_removed_assembly_phase_memberships(
    const AssemblyPhaseTransportEntities& source,
    const AssemblyPhaseTransportEntities& candidate);

} // namespace sketch
