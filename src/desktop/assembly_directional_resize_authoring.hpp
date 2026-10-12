#pragma once

#include "sketch/assembly_directional_resize.hpp"
#include "sketch/assembly_instance_property_edit.hpp"

#include <span>

namespace sketch::desktop {

// Source-only producer for one actual independent document entity. Captured
// source must be editable; the entity/catalog/local instance/root pose are
// resolved from that exact immutable snapshot, with complete model expansion.
// Anchor/factors must be finite and factors positive. Catalogue decoding has
// its own bounded raw-work admission; independent decode, actual expansion and
// exact candidate drafting share available_work, committed only on success.
// An omitted world-axis angle retains the single root's historical axes;
// an explicit angle must be finite, including for unit factors.
//
// Returns sorted, unique Set intents for EVERY changed own-profile roster,
// retaining unchanged topology and replacing changed topology against actual
// own/hidden same-ID type IDs. Identified own sources use reviewed v5; entirely
// anonymous genuine map deformation uses atomic initial v6. Geometry-free
// ancestors do not suppress descendants. Unit factors yield no intents; more
// than 1000 changed nodes refuses the whole proposal without truncation.
//
// Fresh IDs use one captured UUID namespace and deterministic sequence within
// this invocation. Retain the returned intents for preview/review/final replay;
// invoking again creates a distinct proposal. This creates no candidate
// snapshot, phase allocation, dimension decisions, preparation or publish token.
// Caller must retain source and require shares_full_snapshot_with/current
// source proof, plus selection/viewport/recovery fences, before publication.
[[nodiscard]] std::vector<AssemblyInstancePropertyEditIntent> draft_assembly_directional_resize_intents(
    const DocumentSnapshot& source, const std::string& independent_entity_id,
    const AssemblyDirectionalResize& operation, AssemblyExpansionBudget& available_work);

// Source-only component cohort producer, not the full mixed-selection workflow.
// Requires 1..1000 unique, nonempty actual independent document entity IDs and
// a finite explicit world_axis_rotation_radians, even for unit factors. Every
// root receives the SAME world anchor/axes/factors from the immutable editable
// source. IDs and paths are processed in structural sorted order; complete raw
// catalog admission shares one bounded ledger and decodes each actual catalog
// once. All roots share available_work, one UUID namespace/identity sequence
// and the aggregate 1000 changed-node cap. Later-root failure commits no work
// and returns no partial proposal. All single-root topology/source guarantees
// above apply. The caller owns one atomic mixed command, source fences, phase,
// room and dimension review, candidate preparation and publication.
[[nodiscard]] std::vector<AssemblyInstancePropertyEditIntent> draft_assembly_directional_resize_intents(
    const DocumentSnapshot& source, std::span<const std::string> independent_entity_ids,
    const AssemblyDirectionalResize& operation, AssemblyExpansionBudget& available_work);

} // namespace sketch::desktop
