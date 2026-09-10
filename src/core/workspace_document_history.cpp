#include "sketch/workspace_document_history.hpp"
#include <stdexcept>
#include <unordered_set>

namespace sketch {
WorkspaceDocumentHistory capture_workspace_document_history(const DocumentSnapshot& snapshot) {
    return {capture_workspace_history_fence(snapshot), {}};
}

void validate_workspace_document_history(const DocumentSnapshot& snapshot,
                                         const WorkspaceDocumentHistory& history) {
    validate_workspace_history_fence(snapshot, history.baseline);
    const auto baseline = static_cast<std::size_t>(history.baseline.baseline_revision);
    if (history.events.size() != snapshot.history().size() - baseline - 1) {
        throw std::invalid_argument("Document events do not cover every revision after the baseline");
    }
    std::unordered_set<std::string> ids;
    for (std::size_t index = 0; index < history.events.size(); ++index) {
        const auto& event = history.events[index];
        if (event.event_id.empty() || event.event_id.size() > 128 ||
            event.event_id.find('\0') != std::string::npos || !ids.insert(event.event_id).second) {
            throw std::invalid_argument("Document event ID is invalid or duplicated");
        }
        const auto& record = snapshot.history()[baseline + index + 1];
        if (event.sequence != index + 1 || event.after_revision != record.revision ||
            event.before_revision != record.revision - 1) {
            throw std::invalid_argument("Document event sequence or revision chain is invalid");
        }
        bool matches = false;
        switch (event.kind) {
        case WorkspaceDocumentEventKind::edit:
            matches = !record.source_revision.has_value();
            break;
        case WorkspaceDocumentEventKind::undo:
            matches = record.source_revision.has_value() && record.action == "undo";
            break;
        case WorkspaceDocumentEventKind::redo:
            matches = record.source_revision.has_value() && record.action == "redo";
            break;
        default:
            break;
        }
        if (!matches) {
            throw std::invalid_argument("Document event kind does not match its retained revision");
        }
    }
}
}
