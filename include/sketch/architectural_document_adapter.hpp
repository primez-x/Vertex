#pragma once

#include "sketch/architectural_workflow_contract.hpp"
#include "sketch/document.hpp"
#include "sketch/assembly_model.hpp"

#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace sketch {

// Stage one owner and both corner cuts, retaining child identities and opaque
// payloads. The caller completes phase/view memberships, previews the complete
// command through Document and validates architectural geometry before applying.
// These producers do not publish a partial leg or infer registry enrollment.
[[nodiscard]] ApplyEntityChanges corner_window_upsert_command(
    const DocumentSnapshot& source, const Entity& replacement, Revision expected_revision);
[[nodiscard]] ApplyEntityChanges corner_window_remove_command(
    const DocumentSnapshot& source, const std::string& owner_id, Revision expected_revision);

enum class RoomFootprintAnchor { first_corner, center, opposite_corner };

enum class ArchitecturalJoinKind { wall, roof };

// Admit the derived fused geometry before returning one revision-fenced command.
// Source members and their hosted objects remain authoritative and unchanged.
[[nodiscard]] ApplyEntityChanges architectural_join_create_command(
    const DocumentSnapshot& source, const std::string& join_id,
    const std::vector<std::string>& member_ids, ArchitecturalJoinKind kind,
    Revision expected_revision);

// Selection may contain source members, join IDs, or both, of the specified
// kind. Every selected ID must resolve to a join; each join is erased once.
[[nodiscard]] ApplyEntityChanges architectural_join_remove_command(
    const DocumentSnapshot& source, const std::vector<std::string>& selected_ids,
    ArchitecturalJoinKind kind, Revision expected_revision);

// Width follows boundary segment 0; depth follows segment 1. Supply both
// dimensions to resize a rectangular, hole-free footprint, or neither to
// change only height/elevation on any valid room. Supplying measured height
// and elevation can promote a retained plan-only room to a volume; no missing
// measurement is inferred from its footprint. All lengths are metres.
struct RoomDimensionEdit {
    std::optional<double> width_metres;
    std::optional<double> depth_metres;
    double height_metres{};
    double elevation_metres{};
    RoomFootprintAnchor anchor{RoomFootprintAnchor::first_corner};
};

[[nodiscard]] Entity resized_room_volume_entity(const Entity& source,
                                               const RoomDimensionEdit& edit);
[[nodiscard]] ApplyEntityChanges room_dimension_update_command(
    const DocumentSnapshot& source, const std::string& entity_id,
    const RoomDimensionEdit& edit, Revision expected_revision);

enum class BeamEndpoint { start, end };

// Move one authored endpoint in plan metres. Its original Z, the opposite
// endpoint, section, up vector and all other source payload remain unchanged.
struct BeamEndpointEdit {
    BeamEndpoint endpoint{BeamEndpoint::end};
    Vec2 proposed_position{};
};

// Detached actual-map staging of the same persisted plan XY operation. Retains
// raw Z, admits the actual resolved placement and invalidates affected receipts.
// An unchanged endpoint returns the exact source without publication/history.
[[nodiscard]] Entity stage_structural_beam_endpoint_entity(
    const std::map<std::string, Entity, std::less<>>& actual_entities,
    const std::string& entity_id, const BeamEndpointEdit& edit);

// Requires an editable revision-fenced source and a real canonical beam.
// Native geometry and complete Document relationships admit the detached
// candidate before one command is returned. Publication must retain the
// caller's complete captured-source fence, as for other adapter commands.
[[nodiscard]] ApplyEntityChanges beam_endpoint_update_command(
    const DocumentSnapshot& source, const std::string& entity_id,
    const BeamEndpointEdit& edit, Revision expected_revision);

enum class RailingEndpoint { start, end };

// Move one endpoint of a canonical independent straight railing in plan metres.
// Retain Z, height, section, post spacing, placement and source metadata. The
// opposite endpoint and requested endpoint agree with polar reconstruction to
// 1e-7 metres plus 32 machine epsilons times the largest plan coordinate.
// Targets within that tolerance of the current endpoint make no change.
struct RailingEndpointEdit {
    RailingEndpoint endpoint{RailingEndpoint::end};
    Vec2 proposed_position{};
};

// Hosted flight/landing railings follow their stair and refuse this free edit.
// Complete Document relationships and resolved native geometry admit the
// detached candidate. Publication retains the caller's captured-source fence.
[[nodiscard]] ApplyEntityChanges railing_endpoint_update_command(
    const DocumentSnapshot& source, const std::string& entity_id,
    const RailingEndpointEdit& edit, Revision expected_revision);

// Typed semantic edits retain the container's identity, extensions and unrelated
// properties. Apply through Document for atomic admission and revision fencing.
[[nodiscard]] ApplyEntityChanges assembly_type_update_command(
    const DocumentSnapshot& source, const std::string& entity_id,
    AssemblyType replacement, Revision expected_revision);
[[nodiscard]] ApplyEntityChanges model_phase_selection_command(
    const DocumentSnapshot& source, const std::string& entity_id,
    std::optional<std::string> alternative, Revision expected_revision);
// Edits only the existing alternative's name and demolished baseline members;
// its identity, proposed members and active selection are retained.
[[nodiscard]] ApplyEntityChanges model_phase_alternative_update_command(
    const DocumentSnapshot& source, const std::string& entity_id,
    const std::string& alternative_id, std::string name,
    std::vector<std::string> demolished_ids, Revision expected_revision);

// Converts a validated architectural transaction into the existing typed
// Document command boundary. It preserves unrelated measurement entities and
// emits at most one change per entity, so the operation is atomic and undoable.
[[nodiscard]] ApplyEntityChanges architectural_transaction_command(
    const DocumentSnapshot& source, const ArchitecturalTransaction& transaction,
    Revision expected_revision);

// Translate a prepared fresh physical clipboard graph without making its copied
// phase metadata an authoring source. The original editable snapshot supplies
// read-only context/levels/catalog definitions; every returned owner is fresh.
// Per-owner offsets are captured local XY translations, keyed by fresh identity;
// missing entries use offset. Nonphysical inventory is returned unchanged for
// the caller's drawing/annotation lanes; hosted catalog rows for fresh drawing
// boundaries still follow their captured offsets. Hosted openings/rails follow fresh
// hosts once, and world-authored catalog profiles use the host's actual Site
// frame. Conflicting dependent offsets or incomplete physical closures refuse.
// Changed quantity_entries retire verbatim into the bounded passive extension
// clipboard_translation_quantity_archive:{version:1,rows:[{pointer,receipt,
// original_value,translated_value,offset_m:[x,y]}]}; untouched receipts and prior
// compatible rows remain exact. This is geometry staging, not publication,
// destination enrollment, a retained command proof or full Document admission.
[[nodiscard]] std::map<std::string, Entity, std::less<>>
stage_fresh_architectural_clipboard_translation(
    const DocumentSnapshot& actual_source,
    const std::map<std::string, Entity, std::less<>>& fresh_entities,
    Vec2 offset, Revision expected_revision,
    const std::map<std::string, Vec2, std::less<>>& owner_offsets = {});

// A captured model-space pivot for rotation, uniform scaling and XYZ movement.
// The shared-transform overload applies this same operator to every object;
// object positions must never replace the caller's pivot. Hosted railings
// follow their selected stair once.
struct ArchitecturalGroupTransform {
    Vec3 pivot{};
    Vec3 offset{};
    double rotation_z_radians{};
    double scale{1.0};
    bool flip_horizontal{false};
    bool flip_vertical{false};
};
// One captured local-frame operation per persisted object. Callers convert a
// common Site/world operation into each target's frame before entering here.
struct ArchitecturalGroupTransformTarget {
    std::string entity_id;
    ArchitecturalGroupTransform transform;
};
inline constexpr std::size_t maximum_architectural_group_targets = 1000;

// Complete actual-map stair/railing transform, including attached rails and
// affected hosted catalog rows. No phase identities or memberships are changed.
[[nodiscard]] std::map<std::string, Entity, std::less<>> stage_stair_group_transform_entities(
    const std::map<std::string, Entity, std::less<>>& actual_entities,
    std::span<const ArchitecturalGroupTransformTarget> targets);

// The captured group operator in AssemblyTransform convention, before raw
// level compensation or Site presentation. Uses the ordinary group math.
[[nodiscard]] AssemblyTransform architectural_group_assembly_transform(
    const ArchitecturalGroupTransform& transform);

// Detached actual-map producer for canonical v1 columns and straight beams.
// Retains the caller's original group operation, resolves its real level datum,
// and uses the ordinary geometry and changed-receipt invalidation semantics.
// Identity operations return the exact source without allocating an identity.
// This stages geometry only; the caller owns phase/relationship publication.
[[nodiscard]] Entity stage_structural_group_transform_entity(
    const std::map<std::string, Entity, std::less<>>& actual_entities,
    const std::string& object_id, const ArchitecturalGroupTransform& transform);

// Complete persisted physical-object group through the existing transaction
// and native admission boundary. Planar walls use the qualified connected-wall
// lane instead; callers may merge non-wall changes into that complete command.
// Render aliases/embedded profiles are not persisted targets. Caller retains
// the complete source, view/selection and compatible Site-frame authority.
// Positive uniform scale and Z yaw precede global horizontal/vertical flips
// about the shared pivot, then XYZ offset. Mirrors leave world Z unchanged.
// Incomplete/future physical descriptors require a qualified family lane.
// Level-relative placements retain their bindings and share the world pivot.
// A selected hosted railing requires its selected stair; selected/hidden
// dependents follow the host once. Connected stair dimensions cannot scale.
// Horizontal authoring replays slabs from the actual source and captured pivot,
// retaining geometry derivations/retired receipts and moving actual hosted
// catalog placements in the same final command. Affected catalogs are admitted
// consequences, not selected transform roots. Ordinary admission retains the
// saved-design guards; shared-baseline replacement requires its qualified lane.
// Equivalent identity intent validates targets/transaction identity and returns
// no history edit. Walls, joins and embedded assembly members are not targets.
[[nodiscard]] ApplyEntityChanges architectural_group_transform_command(
    const DocumentSnapshot& source, std::span<const std::string> entity_ids,
    const ArchitecturalGroupTransform& transform, const std::string& transaction_id,
    Revision expected_revision);

// The same atomic admission with individually captured local pivots/operations.
// Selected hosted railings require affine-equivalent intent to their selected
// stair (including scale/reflection); the host updates all dependents once.
// Equivalence allows 64 machine epsilons times each coefficient's magnitude
// (with a unit floor) for frame-conversion roundoff, with equal reflection parity.
// Identity targets retain their exact payload even in a mixed operation.
// Each slab retains its original captured operation for actual-source replay;
// another family's affine/level compensation never becomes slab edit intent.
[[nodiscard]] ApplyEntityChanges architectural_group_transform_command(
    const DocumentSnapshot& source, std::span<const ArchitecturalGroupTransformTarget> targets,
    const std::string& transaction_id, Revision expected_revision);

// Admit changed physical wall/opening, slab, room, canonical beam and independent
// straight-railing descriptors against the complete detached candidate.
// Hosted siblings and affected fused joins
// participate even when hidden. Changed plan-only room footprints receive
// analytical admission without manufacturing missing volume fields.
// Metadata-only edits and incomplete legacy transport descriptors do not force
// solid generation. Explicit required IDs must have valid physical geometry,
// including a full room volume when a room ID is explicitly required and when
// its entered dimension is unchanged. Beam admission uses the canonical codec
// and native builder with the completed candidate's resolved vertical placement;
// railing admission uses the same resolved canonical building-object boundary.
void validate_architectural_geometry_changes(
    const DocumentSnapshot& source, const DocumentSnapshot& candidate,
    const std::vector<std::string>& required_ids = {});

// Builds a detached preview using the same command that will be committed.
[[nodiscard]] DocumentSnapshot preview_architectural_transaction(
    const DocumentSnapshot& source, const ArchitecturalTransaction& transaction);

// Applies the transaction with the caller's current revision fence. A stale
// fence or document validation failure leaves the document unchanged.
Revision apply_architectural_transaction(Document& document,
                                         const ArchitecturalTransaction& transaction,
                                         Revision expected_revision);

}  // namespace sketch
