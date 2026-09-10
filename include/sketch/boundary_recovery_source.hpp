#pragma once

#include "sketch/document.hpp"
#include "sketch/project_organization.hpp"

#include <string>

namespace sketch {

// Provenance for recovered authoring state, not authority to commit it.
// Inspection never repairs the context or implicitly rebinds a stale source.
struct BoundaryRecoverySource {
    std::string document_id;
    Revision revision{};
    std::string authoring_digest;
    DrawingContext context;
    bool operator==(const BoundaryRecoverySource&) const = default;
};

enum class BoundaryRecoverySourceStatus {
    current,
    read_only,
    foreign_document,
    stale_revision,
    stale_digest,
    missing_context,
    mismatched_context,
};

// Throws std::invalid_argument unless the snapshot is editable and the complete
// context exactly matches the resolved context of its recorded layer.
[[nodiscard]] BoundaryRecoverySource capture_boundary_recovery_source(
    const DocumentSnapshot& snapshot, const DrawingContext& context);

// Reports the first failure in enum order, without changing either value.
// A current source still requires the normal commit validation and authority.
[[nodiscard]] BoundaryRecoverySourceStatus inspect_boundary_recovery_source(
    const DocumentSnapshot& snapshot, const BoundaryRecoverySource& source);

}  // namespace sketch
