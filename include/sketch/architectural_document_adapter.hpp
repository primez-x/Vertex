#pragma once

#include "sketch/architectural_workflow_contract.hpp"
#include "sketch/document.hpp"
#include "sketch/assembly_model.hpp"

namespace sketch {

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
// change only height/elevation on any valid room. All lengths are metres.
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

// Typed semantic edits retain the container's identity, extensions and unrelated
// properties. Apply through Document for atomic admission and revision fencing.
[[nodiscard]] ApplyEntityChanges assembly_type_update_command(
    const DocumentSnapshot& source, const std::string& entity_id,
    AssemblyType replacement, Revision expected_revision);
[[nodiscard]] ApplyEntityChanges model_phase_selection_command(
    const DocumentSnapshot& source, const std::string& entity_id,
    std::optional<std::string> alternative, Revision expected_revision);

// Converts a validated architectural transaction into the existing typed
// Document command boundary. It preserves unrelated measurement entities and
// emits at most one change per entity, so the operation is atomic and undoable.
[[nodiscard]] ApplyEntityChanges architectural_transaction_command(
    const DocumentSnapshot& source, const ArchitecturalTransaction& transaction,
    Revision expected_revision);

// Builds a detached preview using the same command that will be committed.
[[nodiscard]] DocumentSnapshot preview_architectural_transaction(
    const DocumentSnapshot& source, const ArchitecturalTransaction& transaction);

// Applies the transaction with the caller's current revision fence. A stale
// fence or document validation failure leaves the document unchanged.
Revision apply_architectural_transaction(Document& document,
                                         const ArchitecturalTransaction& transaction,
                                         Revision expected_revision);

}  // namespace sketch
