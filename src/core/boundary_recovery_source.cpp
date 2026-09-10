#include "sketch/boundary_recovery_source.hpp"

#include "sketch/document_digest.hpp"

#include <stdexcept>

namespace sketch {

BoundaryRecoverySource capture_boundary_recovery_source(
    const DocumentSnapshot& snapshot, const DrawingContext& context) {
    if (!snapshot.is_editable()) {
        throw std::invalid_argument("boundary recovery source requires an editable document");
    }
    if (!context.complete()) {
        throw std::invalid_argument("boundary recovery source requires a complete drawing context");
    }
    const auto actual = organize_project(snapshot).drawing_context(context.layer_id);
    if (!actual || !actual->complete()) {
        throw std::invalid_argument("boundary recovery source drawing context cannot be resolved");
    }
    if (*actual != context) {
        throw std::invalid_argument("boundary recovery source drawing context does not match its layer");
    }
    return {snapshot.document_id(), snapshot.revision(),
            document_authoring_source_digest_v1(snapshot), context};
}

BoundaryRecoverySourceStatus inspect_boundary_recovery_source(
    const DocumentSnapshot& snapshot, const BoundaryRecoverySource& source) {
    if (!snapshot.is_editable()) return BoundaryRecoverySourceStatus::read_only;
    if (snapshot.document_id() != source.document_id) {
        return BoundaryRecoverySourceStatus::foreign_document;
    }
    if (snapshot.revision() != source.revision) {
        return BoundaryRecoverySourceStatus::stale_revision;
    }
    if (document_authoring_source_digest_v1(snapshot) != source.authoring_digest) {
        return BoundaryRecoverySourceStatus::stale_digest;
    }
    if (!source.context.complete()) return BoundaryRecoverySourceStatus::missing_context;
    const auto actual = organize_project(snapshot).drawing_context(source.context.layer_id);
    if (!actual || !actual->complete()) return BoundaryRecoverySourceStatus::missing_context;
    if (*actual != source.context) return BoundaryRecoverySourceStatus::mismatched_context;
    return BoundaryRecoverySourceStatus::current;
}

}  // namespace sketch
