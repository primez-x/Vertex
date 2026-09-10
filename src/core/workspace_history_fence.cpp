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

WorkspaceBaselineNavigation derive_workspace_baseline_navigation(
    const DocumentSnapshot& snapshot, const WorkspaceHistoryFence& fence) {
    validate_workspace_history_fence(snapshot, fence);
    std::vector<Revision> undo;
    std::vector<Revision> redo;
    for (std::size_t index = 1; index <= static_cast<std::size_t>(fence.baseline_revision); ++index) {
        const auto& record = snapshot.history()[index];
        if (!record.source_revision) {
            // Action messages are user text; an ordinary edit may be named
            // "undo" or "redo". Navigation is identified by provenance.
            undo.push_back(record.revision);
            redo.clear();
        } else if (record.action == "undo" && !undo.empty()) {
            redo.push_back(undo.back());
            undo.pop_back();
        } else if (record.action == "redo" && !redo.empty()) {
            undo.push_back(redo.back());
            redo.pop_back();
        } else {
            throw std::invalid_argument("Baseline command navigation is inconsistent");
        }
    }
    if (undo.size() != fence.undo_stack.size() || redo.size() != fence.redo_stack.size()) {
        throw std::invalid_argument("Baseline command and snapshot stacks disagree");
    }
    WorkspaceBaselineNavigation result;
    result.undo_stack.reserve(undo.size());
    result.redo_stack.reserve(redo.size());
    for (std::size_t index = 0; index < undo.size(); ++index)
        result.undo_stack.push_back({undo[index], fence.undo_stack[index]});
    for (std::size_t index = 0; index < redo.size(); ++index)
        result.redo_stack.push_back({redo[index], fence.redo_stack[index]});
    return result;
}
}
