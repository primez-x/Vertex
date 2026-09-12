#pragma once

#include "sketch/document_schedule_adapter.hpp"

namespace sketch {

// Augments assigned-material rows with measured solid volume, never an authored
// substitute. Missing or invalid solid geometry produces an explicit diagnostic.
[[nodiscard]] DocumentScheduleProjection build_architectural_schedules(const DocumentSnapshot& document);
[[nodiscard]] DocumentScheduleProjection build_architectural_schedules(const DocumentSnapshot& document,
    const std::set<std::string, std::less<>>& visible_entity_ids);

} // namespace sketch
