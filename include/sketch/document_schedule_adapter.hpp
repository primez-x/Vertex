#pragma once

#include "sketch/document.hpp"
#include "sketch/schedule_model.hpp"

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

}  // namespace sketch

