#pragma once

#include "sketch/architectural_workflow_contract.hpp"
#include "sketch/document.hpp"
#include "sketch/assembly_model.hpp"

namespace sketch {

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
