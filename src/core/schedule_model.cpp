#include "sketch/schedule_model.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>

namespace sketch {
namespace {
void validate_value(const ScheduleValue& value) {
    if (const auto* number = std::get_if<double>(&value); number && !std::isfinite(*number))
        throw std::invalid_argument("Schedule numeric value must be finite");
    if (const auto* quantity = std::get_if<ScheduleQuantity>(&value)) {
        if (!std::isfinite(quantity->value))
            throw std::invalid_argument("Schedule quantity must be finite");
        switch (quantity->unit) {
        case ScheduleUnit::metre: case ScheduleUnit::square_metre:
        case ScheduleUnit::cubic_metre: case ScheduleUnit::kilogram: break;
        default: throw std::invalid_argument("Unknown schedule quantity unit");
        }
    }
}
bool same_type(const ScheduleValue& a, const ScheduleValue& b) {
    if (a.index() != b.index()) return false;
    const auto* quantity = std::get_if<ScheduleQuantity>(&a);
    return !quantity || quantity->unit == std::get<ScheduleQuantity>(b).unit;
}
}

ScheduleSnapshot build_schedule(const std::vector<ScheduleRecord>& records,
                                std::uint64_t revision) {
    std::map<std::string, const ScheduleRecord*> sources;
    std::set<std::pair<ScheduleRowKind, std::string>> marks;
    for (const auto& record : records) {
        switch (record.kind) {
        case ScheduleRowKind::door: case ScheduleRowKind::window:
        case ScheduleRowKind::room: case ScheduleRowKind::material:
        case ScheduleRowKind::material_summary: case ScheduleRowKind::assembly:
        case ScheduleRowKind::building: break;
        default: throw std::invalid_argument("Unknown schedule row kind");
        }
        if (record.object_id.empty() || !sources.emplace(record.object_id, &record).second)
            throw std::invalid_argument("Missing or duplicate schedule object ID");
        if (record.mark.empty() || !marks.emplace(record.kind, record.mark).second)
            throw std::invalid_argument("Missing or duplicate schedule mark within row kind");
        for (const auto& [name, value] : record.properties) {
            if (name.empty() || name == "mark" || record.calculated.contains(name))
                throw std::invalid_argument("Empty, reserved or ambiguous schedule property");
            validate_value(value);
        }
    }
    ScheduleSnapshot result{revision, {}};
    for (const auto& [id, record] : sources) {
        ScheduleRow row{id, record->mark, record->kind, {}};
        row.cells.emplace("mark", ScheduleCell{record->mark, true, {{id, "mark"}}, {}});
        for (const auto& [name, value] : record->properties)
            row.cells.emplace(name, ScheduleCell{value, true, {{id, name}}, {}});
        for (const auto& [name, calculation] : record->calculated) {
            if (name.empty() || name == "mark" || calculation.sources.empty() ||
                calculation.explanation.empty())
                throw std::invalid_argument("Calculated cell needs a name, explanation and sources");
            validate_value(calculation.value);
            for (const auto& ref : calculation.sources) {
                const auto source = sources.find(ref.object_id);
                // A semantic room boundary is a first-class geometric source
                // even when it is not duplicated into the tabular property
                // map.  It remains provenance-only and therefore cannot be
                // edited as a schedule cell.
                const bool geometry_source = ref.property == "boundary" ||
                                             ref.property == "holes" ||
                                             ref.property == "geometry";
                if (source == sources.end() || (ref.property != "mark" &&
                    !geometry_source && !source->second->properties.contains(ref.property)))
                    throw std::invalid_argument("Missing schedule source: " + ref.object_id + "." + ref.property);
            }
            auto refs = calculation.sources;
            std::sort(refs.begin(), refs.end(), [](const auto& a, const auto& b) {
                return std::pair{a.object_id, a.property} < std::pair{b.object_id, b.property};
            });
            refs.erase(std::unique(refs.begin(), refs.end()), refs.end());
            row.cells.emplace(name, ScheduleCell{calculation.value, false,
                std::move(refs), calculation.explanation});
        }
        result.rows.push_back(std::move(row));
    }
    return result;
}

ScheduleEdit make_schedule_edit(const ScheduleSnapshot& snapshot,
    const std::string& object_id, const std::string& column, ScheduleValue replacement) {
    const auto row = std::find_if(snapshot.rows.begin(), snapshot.rows.end(),
        [&](const auto& candidate) { return candidate.object_id == object_id; });
    if (row == snapshot.rows.end()) throw std::invalid_argument("Schedule row not found");
    const auto found = row->cells.find(column);
    if (found == row->cells.end()) throw std::invalid_argument("Schedule column not found");
    const auto& cell = found->second;
    if (!cell.editable) {
        auto reason = "Calculated schedule cell is read-only: " + cell.explanation;
        for (const auto& ref : cell.sources) reason += " [" + ref.object_id + "." + ref.property + "]";
        throw std::invalid_argument(reason);
    }
    if (cell.sources.size() != 1 || cell.sources.front() != ScheduleSourceRef{object_id, column})
        throw std::invalid_argument("Editable schedule cell has an ambiguous source");
    validate_value(replacement);
    if (!same_type(cell.value, replacement))
        throw std::invalid_argument("Schedule edit must preserve the property type and unit");
    if (column == "mark") {
        const auto* mark = std::get_if<std::string>(&replacement);
        if (!mark || mark->empty()) throw std::invalid_argument("Schedule mark cannot be empty");
        for (const auto& other : snapshot.rows)
            if (other.object_id != object_id && other.kind == row->kind && other.mark == *mark)
                throw std::invalid_argument("Duplicate schedule mark within row kind");
    }
    return {snapshot.revision, cell.sources.front(), cell.value, std::move(replacement)};
}
} // namespace sketch
