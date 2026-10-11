#pragma once

#include "sketch/assembly_phase_membership.hpp"
#include "sketch/document.hpp"

namespace sketch {

using AssemblyPhaseTransportEntities = std::map<std::string, Entity, std::less<>>;
using AssemblyPhaseCopiedRoots = std::map<AssemblyPhaseRoot, AssemblyPhaseRoot>;
using AssemblyPhaseCopiedHosts = std::map<std::string, std::string, std::less<>>;

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
