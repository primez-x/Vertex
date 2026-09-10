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

void validate_historical_boundary_recovery_source(
    const DocumentSnapshot& snapshot, const BoundaryRecoverySource& source) {
    try { (void)Document::fork(snapshot); }
    catch (const DocumentError& error) {
        throw std::invalid_argument(std::string("Invalid historical recovery document: ") + error.what());
    }
    if (source.document_id != snapshot.document_id() || source.revision >= snapshot.history().size())
        throw std::invalid_argument("Historical boundary source references a foreign or missing revision");
    if (document_authoring_source_digest_v1_at_revision(snapshot, source.revision) != source.authoring_digest)
        throw std::invalid_argument("Historical boundary source digest does not match retained history");
    if (!source.context.complete()) throw std::invalid_argument("Historical drawing context is incomplete");
    const auto& record = snapshot.history()[static_cast<std::size_t>(source.revision)];
    const auto context = organize_project(record.entities).drawing_context(source.context.layer_id);
    if (!context || *context != source.context)
        throw std::invalid_argument("Historical drawing context does not match its original layer");
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
