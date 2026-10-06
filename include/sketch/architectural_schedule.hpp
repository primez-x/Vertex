#pragma once

#include "sketch/document_schedule_adapter.hpp"
#include <string>
#include <string_view>

namespace sketch {

// Length-prefixed source identities keep keys unambiguous even when supported
// IDs contain colons. Readable source/join identities remain separate cells.
[[nodiscard]] std::string roof_join_material_schedule_row_id(
    std::string_view join_id, std::string_view source_roof_id);

// Augments assigned-material rows with measured solid volume, never an authored
// substitute. Roof joins expose fused volume and ordered disjoint material net
// quantities; source gross rows remain identifiable and excluded from those
// net summaries. Missing or invalid geometry/bindings produce a diagnostic.
[[nodiscard]] DocumentScheduleProjection build_architectural_schedules(const DocumentSnapshot& document);
[[nodiscard]] DocumentScheduleProjection build_architectural_schedules(const DocumentSnapshot& document,
    const std::set<std::string, std::less<>>& visible_entity_ids);

} // namespace sketch
