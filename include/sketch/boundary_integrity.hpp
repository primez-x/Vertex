#pragma once
#include "sketch/document.hpp"
#include <set>

namespace sketch {
struct PhysicalWallPhaseSelection;
// Detached import/copy of one boundary with a fresh entity owner. Local child
// identities, geometry, input expressions and opaque fields remain exact.
// Supported receipt/derivation owners are qualified and remapped atomically;
// unsupported active evidence throws without changing source. External source
// references remain unchanged; the caller must admit the complete destination
// through Document before publishing it.
[[nodiscard]] Entity remap_boundary_owner_identity(
    const Entity& source, std::string destination_id);
[[nodiscard]] std::map<std::string, Entity, std::less<>> transformed_boundary_entities(
    const std::map<std::string, Entity, std::less<>>& source,
    const BoundaryTransformation& transformation);
// Reconstruct all owners/dimensions from the same source revision. Plain
// identified owners retain their topology origin as explicit derivation.
// Requires a nonempty group with unique owners and one shared operator.
[[nodiscard]] std::map<std::string, Entity, std::less<>> transformed_boundary_entities_batch(
    const std::map<std::string, Entity, std::less<>>& source,
    const std::vector<BoundaryTransformation>& transformations);
// Pure reconstruction with a distinct operator for each unique owner. All
// owners/dimensions use the original source; this grants no authority to publish
// geometry or history. Dependencies and source reconciliation remain with
// Document. Targets must be nonempty.
[[nodiscard]] std::map<std::string, Entity, std::less<>> transformed_boundary_entities_per_owner_batch(
    const std::map<std::string, Entity, std::less<>>& source,
    const std::vector<BoundaryTransformation>& transformations);
// Reconstruct an entire entity state from a qualified receipt-backed offset.
// Preserves identities and all unrelated entities; invalid derivations throw.
[[nodiscard]] std::map<std::string, Entity, std::less<>> translated_boundary_entities(
    const std::map<std::string, Entity, std::less<>>& source,
    const BoundaryTranslation& translation);
// Deterministically applies a stable-ID coordinate edit. Receipt-backed input
// is retained as immutable derivation evidence and the edit list is replayed
// during integrity validation and project-history restoration.
[[nodiscard]] std::map<std::string, Entity, std::less<>> edited_boundary_entities(
    const std::map<std::string, Entity, std::less<>>& source,
    const BoundaryGeometryEdit& edit);
// Pure reconstruction within an already complete room-review assignment.
// Only the dedicated intent replayer may publish this candidate. Other reviewed
// retained owners may temporarily occupy destinations while all assignments
// are rebuilt; ordinary single-owner commands keep the exclusive-owner guard.
[[nodiscard]] std::map<std::string, Entity, std::less<>> edited_boundary_entities_for_room_review(
    const std::map<std::string, Entity, std::less<>>& source,
    const BoundaryGeometryEdit& edit, const std::set<std::string>& reviewed_owners);
// Pure reconstruction of a staged room-review assignment in an explicitly
// evaluated phase. Membership and reference decisions must already be staged;
// the enclosing typed intent and final Document admission retain authority.
[[nodiscard]] std::map<std::string, Entity, std::less<>> edited_boundary_entities_for_phase_room_review(
    const std::map<std::string, Entity, std::less<>>& source,
    const BoundaryGeometryEdit& edit, const std::set<std::string>& reviewed_owners,
    const PhysicalWallPhaseSelection& selection);
// Applies solved vertex positions together when sequential intermediate geometry
// would be invalid. Existing sequential derivations remain byte-compatible.
[[nodiscard]] std::map<std::string, Entity, std::less<>> edited_boundary_entities_batch(
    const std::map<std::string, Entity, std::less<>>& source,
    const std::vector<BoundaryGeometryEdit>& edits);
// Retained history only: after stable derivation fails to reproduce a source
// replacement, accept a complete exact original-v1 kernel result. Live edits
// continue to use edited_boundary_entities[_batch]. No history is rewritten.
[[nodiscard]] std::map<std::string, Entity, std::less<>> replayed_boundary_entities(
    const std::map<std::string, Entity, std::less<>>& source,
    const BoundaryGeometryEdit& edit);
[[nodiscard]] std::map<std::string, Entity, std::less<>> replayed_boundary_entities_batch(
    const std::map<std::string, Entity, std::less<>>& source,
    const std::vector<BoundaryGeometryEdit>& edits);
// Validate every supported boundary, qualified construction receipt and
// dimension, including stable child references, even when another entity has an unknown version. Unknown
// versions return a document-wide read-only reason.
[[nodiscard]] std::optional<std::string> validate_boundary_integrity(
    const std::map<std::string, Entity, std::less<>>& entities);
// Ordinary commands cannot strip identities. Exact undo/redo restoration is
// separately checked against its retained source revision by Document.
void validate_boundary_transition(
    const std::map<std::string, Entity, std::less<>>& before,
    const std::map<std::string, Entity, std::less<>>& after,
    bool allow_explicit_relationship_transform = false);
void record_boundary_identities(BoundaryIdentityHistory& history,
    const std::map<std::string, Entity, std::less<>>& entities);
void record_boundary_identity_transition(BoundaryIdentityHistory& history,
    const std::map<std::string, Entity, std::less<>>& before,
    const std::map<std::string, Entity, std::less<>>& after);
void validate_boundary_identity_transition(const BoundaryIdentityHistory& history,
    const std::map<std::string, Entity, std::less<>>& before,
    const std::map<std::string, Entity, std::less<>>& after,
    const BoundaryGeometryEdit* typed_edit = nullptr);
} // namespace sketch
