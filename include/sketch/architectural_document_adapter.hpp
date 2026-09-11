#pragma once

#include "sketch/architectural_workflow_contract.hpp"
#include "sketch/document.hpp"

namespace sketch {

// Converts a validated architectural transaction into the existing typed
// Document command boundary. It preserves unrelated measurement entities and
// emits at most one change per entity, so the operation is atomic and undoable.
[[nodiscard]] DocumentSnapshot preview_architectural_transaction(
    const DocumentSnapshot& source, const ArchitecturalTransaction& transaction);

// Applies the transaction with the caller's current revision fence. A stale
// fence or document validation failure leaves the document unchanged.
Revision apply_architectural_transaction(Document& document,
                                         const ArchitecturalTransaction& transaction,
                                         Revision expected_revision);

}  // namespace sketch
