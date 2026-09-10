#pragma once
#include "sketch/workspace_history_fence.hpp"

namespace sketch {
enum class WorkspaceDocumentEventKind { edit, undo, redo };

struct WorkspaceDocumentEvent {
    std::string event_id;
    std::uint64_t sequence{};
    WorkspaceDocumentEventKind kind = WorkspaceDocumentEventKind::edit;
    Revision before_revision{};
    Revision after_revision{};
    bool operator==(const WorkspaceDocumentEvent&) const = default;
};

// Internal document-event prerequisite for workspace history. References the
// retained Document revisions without duplicating entity snapshots. This is
// not the persisted workspace_history codec or its lifecycle event model.
struct WorkspaceDocumentHistory {
    WorkspaceHistoryFence baseline;
    std::vector<WorkspaceDocumentEvent> events;
    bool operator==(const WorkspaceDocumentHistory&) const = default;
};

[[nodiscard]] WorkspaceDocumentHistory capture_workspace_document_history(const DocumentSnapshot&);
// Validates the complete document and baseline, then requires exactly one
// typed event per retained revision after the baseline through the head.
// Throws invalid_argument on invalid input; does not grant mutation authority.
void validate_workspace_document_history(const DocumentSnapshot&, const WorkspaceDocumentHistory&);
}
