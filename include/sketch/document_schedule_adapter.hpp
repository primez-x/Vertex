#pragma once

#include "sketch/document.hpp"
#include "sketch/schedule_model.hpp"

#include <set>
#include <string>
#include <vector>

namespace sketch {

// A deterministic, read-only projection of schedule-bearing Document
// entities. The ScheduleSnapshot is tied to the source Document revision;
// diagnostics make malformed or incomplete rows visible without allowing a
// partial row to masquerade as certified output.
struct DocumentScheduleProjection {
    ScheduleSnapshot snapshot;
    std::vector<std::string> diagnostics;
    bool operator==(const DocumentScheduleProjection&) const = default;
};

[[nodiscard]] DocumentScheduleProjection build_document_schedules(
    const DocumentSnapshot& document);

// Projects only visible source entities, including their derived material rows.
// Hidden entities contribute neither rows nor diagnostics; the source revision
// remains unchanged. The caller supplies organization and design-phase visibility.
[[nodiscard]] DocumentScheduleProjection build_document_schedules(
    const DocumentSnapshot& document,
    const std::set<std::string, std::less<>>& visible_entity_ids);

// Converts a validated schedule edit into the ordinary revision-checked
// Document command.  The command updates the source entity rather than a
// derived schedule copy; calculated cells and stale snapshots are rejected.
[[nodiscard]] ApplyEntityChanges make_document_schedule_edit(
    const DocumentSnapshot& document, const ScheduleEdit& edit);

}  // namespace sketch
