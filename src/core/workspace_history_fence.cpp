#include "sketch/workspace_history_fence.hpp"
#include "sketch/document_digest.hpp"
#include <stdexcept>

namespace sketch {
namespace {
void validate_document(const DocumentSnapshot& snapshot) {
    try { (void)Document::fork(snapshot); }
    catch (const DocumentError& error) {
        throw std::invalid_argument(std::string("Invalid history-fence document: ") + error.what());
    }
}
}
WorkspaceHistoryFence capture_workspace_history_fence(const DocumentSnapshot& snapshot) {
    validate_document(snapshot);
    const auto& head = snapshot.history().back();
    return {snapshot.document_id(), snapshot.revision(),
            document_authoring_source_digest_v1(snapshot), head.undo_stack, head.redo_stack};
}

void validate_workspace_history_fence(const DocumentSnapshot& snapshot,
                                     const WorkspaceHistoryFence& fence) {
    validate_document(snapshot);
    if (fence.document_id != snapshot.document_id() ||
        fence.baseline_revision >= snapshot.history().size()) {
        throw std::invalid_argument("History fence references a foreign or missing document revision");
    }
    const auto& baseline = snapshot.history()[static_cast<std::size_t>(fence.baseline_revision)];
    if (baseline.revision != fence.baseline_revision || baseline.undo_stack != fence.undo_stack ||
        baseline.redo_stack != fence.redo_stack) {
        throw std::invalid_argument("History fence does not match retained navigation stacks");
    }
    if (document_authoring_source_digest_v1_at_revision(snapshot, fence.baseline_revision) !=
        fence.source_digest) {
        throw std::invalid_argument("History fence source digest does not match retained baseline");
    }
}
}
