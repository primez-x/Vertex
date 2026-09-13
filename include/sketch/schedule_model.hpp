#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <variant>
#include <vector>

namespace sketch {

// Material source rows remain tied to one authored entity so their editable
// fields can round-trip through the document.  Architectural projections may
// additionally append read-only material_summary rows that aggregate those
// sources by material identity/name.
enum class ScheduleRowKind { door, window, room, material, material_summary, assembly };
enum class ScheduleUnit { metre, square_metre, cubic_metre, kilogram };
struct ScheduleQuantity {
    double value{};
    ScheduleUnit unit{ScheduleUnit::metre};
    bool operator==(const ScheduleQuantity&) const = default;
};
using ScheduleValue = std::variant<std::string, bool, std::int64_t, double, ScheduleQuantity>;

struct ScheduleSourceRef {
    std::string object_id;
    std::string property;
    bool operator==(const ScheduleSourceRef&) const = default;
};
struct ScheduleCalculation {
    ScheduleValue value;
    std::vector<ScheduleSourceRef> sources;
    std::string explanation;
    bool operator==(const ScheduleCalculation&) const = default;
};

// A semantic adapter supplies canonical SI dimensions/areas/volumes and counts.
// IDs and marks are supplied identities, never inferred from row order.
struct ScheduleRecord {
    std::string object_id;
    std::string mark;
    ScheduleRowKind kind{ScheduleRowKind::door};
    std::map<std::string, ScheduleValue> properties;
    std::map<std::string, ScheduleCalculation> calculated;
    bool operator==(const ScheduleRecord&) const = default;
};
struct ScheduleCell {
    ScheduleValue value;
    bool editable{};
    std::vector<ScheduleSourceRef> sources;
    std::string explanation;
    bool operator==(const ScheduleCell&) const = default;
};
struct ScheduleRow {
    std::string object_id;
    std::string mark;
    ScheduleRowKind kind{ScheduleRowKind::door};
    std::map<std::string, ScheduleCell> cells;
    bool operator==(const ScheduleRow&) const = default;
};
struct ScheduleSnapshot {
    std::uint64_t revision{};
    std::vector<ScheduleRow> rows;
    bool operator==(const ScheduleSnapshot&) const = default;
};
struct ScheduleEdit {
    std::uint64_t expected_revision{};
    ScheduleSourceRef target;
    ScheduleValue before;
    ScheduleValue after;
    bool operator==(const ScheduleEdit&) const = default;
};

// Throws invalid_argument for malformed values, duplicate identities/marks,
// ambiguous columns, or missing calculation sources. Sorts rows by object ID.
[[nodiscard]] ScheduleSnapshot build_schedule(const std::vector<ScheduleRecord>& records,
                                               std::uint64_t revision);
// Produces an optimistic command description without mutating the snapshot.
// Calculated-cell rejection includes its explanation and source references.
[[nodiscard]] ScheduleEdit make_schedule_edit(const ScheduleSnapshot& snapshot,
    const std::string& object_id, const std::string& column, ScheduleValue replacement);

} // namespace sketch
