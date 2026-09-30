#include "sketch/constraint_wall_edit.hpp"
#include "sketch/constraint_tolerances.hpp"
#include "sketch/wall_semantics.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>

namespace sketch {
namespace {
using json = nlohmann::json;
using ordered_json = nlohmann::ordered_json;
[[noreturn]] void invalid(std::string message) { throw std::invalid_argument(std::move(message)); }
double finite_number(const json& value, std::string_view description) {
    if (!value.is_number()) {
        invalid(std::string(description) + " must be a number");
    }
    const auto result = value.get<double>();
    if (!std::isfinite(result)) {
        invalid(std::string(description) + " must be finite");
    }
    return result;
}

Vec2 point(const json& value, std::string_view description) {
    if (!value.is_array() || value.size() != 2) {
        invalid(std::string(description) + " must contain exactly two coordinates");
    }
    return {finite_number(value.at(0), description), finite_number(value.at(1), description)};
}

Segment read_baseline(const Entity& entity) {
    if (entity.type != "wall") {
        invalid("Constraint owner is not a wall: " + entity.id);
    }
    if (!entity.properties.is_object()) {
        invalid("Wall properties must be an object: " + entity.id);
    }
    try {
        const auto& baseline = entity.properties.at("baseline");
        if (!baseline.is_object()) {
            invalid("Wall baseline must be an object: " + entity.id);
        }
        return {point(baseline.at("start"), "Wall start"),
                point(baseline.at("end"), "Wall end"),
                finite_number(baseline.at("sweep_radians"), "Wall sweep")};
    } catch (const std::out_of_range&) {
        invalid("Wall baseline is missing required geometry: " + entity.id);
    }
}

std::string unit_name(Unit unit) {
    switch (unit) {
        case Unit::metre:
            return "m";
        case Unit::millimetre:
            return "mm";
        case Unit::centimetre:
            return "cm";
        case Unit::foot:
            return "ft";
        case Unit::inch:
            return "in";
    }
    invalid("Unknown quantity unit");
}

Quantity normalize_positive_quantity(const Quantity& value) {
    Quantity parsed;
    try {
        parsed = parse_quantity(value.original_expression, value.entered_unit);
    } catch (const std::exception&) {
        invalid("Wall length quantity is not exactly parseable");
    }
    if (parsed.metres != value.metres || parsed.exact_metres != value.exact_metres ||
        parsed.entered_unit != value.entered_unit ||
        parsed.original_expression != value.original_expression || !(parsed.metres > 0.0) ||
        !std::isfinite(parsed.metres)) {
        invalid("Wall length quantity is internally inconsistent or non-positive");
    }
    return parsed;
}

void update_baseline_json(json& target, const Segment& baseline) {
    target["start"] = {baseline.start.x, baseline.start.y};
    target["end"] = {baseline.end.x, baseline.end.y};
    target["sweep_radians"] = baseline.sweep_radians;
}

void set_baseline(Entity& wall, const Segment& baseline) {
    update_baseline_json(wall.properties.at("baseline"), baseline);
}

// Unknown members are opaque metadata. Updating recognized fields preserves
// them; invalidating a receipt containing them must fail closed rather than
// destroy data whose meaning this version cannot establish.
bool validate_length_receipt(const json& receipt, const Entity& wall) {
    if (!receipt.is_object() || !receipt.contains("version") ||
        !receipt.at("version").is_number_integer() || receipt.at("version") != 1) {
        invalid("Wall has unsupported last_length_entry extension metadata: " + wall.id);
    }
    try {
        const auto& expression = receipt.at("original_expression");
        const auto& entered_unit = receipt.at("entered_unit");
        if (!expression.is_string() || !entered_unit.is_string()) {
            invalid("Wall length receipt expression and unit must be strings");
        }
        std::optional<Unit> unit;
        for (const auto candidate : {Unit::metre, Unit::millimetre, Unit::centimetre,
                                     Unit::foot, Unit::inch}) {
            if (entered_unit == unit_name(candidate)) {
                unit = candidate;
                break;
            }
        }
        if (!unit) {
            invalid("Wall length receipt has an unsupported entered unit");
        }
        const auto quantity = parse_quantity(expression.get_ref<const std::string&>(), *unit);
        const auto& exact = receipt.at("exact_metres");
        if (quantity.entered_unit != *unit || !std::isfinite(quantity.metres) ||
            quantity.metres <= 0.0 || !exact.is_object() ||
            !exact.at("numerator").is_number_integer() ||
            !exact.at("denominator").is_number_integer() ||
            exact.at("numerator") != quantity.exact_metres.numerator ||
            exact.at("denominator") != quantity.exact_metres.denominator) {
            invalid("Wall length receipt has inconsistent exact quantity metadata");
        }
        auto receipt_wall = wall;
        receipt_wall.properties["baseline"] = receipt.at("baseline");
        const auto recorded = read_baseline(receipt_wall);
        const auto current = read_baseline(wall);
        // Receipts store the coordinates produced by the command, not a
        // measurement approximation; any subsequent coordinate change stales it.
        if (recorded.sweep_radians != 0.0 || current.sweep_radians != 0.0 ||
            recorded.start.x != current.start.x || recorded.start.y != current.start.y ||
            recorded.end.x != current.end.x || recorded.end.y != current.end.y ||
            std::abs(std::hypot(recorded.end.x - recorded.start.x,
                                recorded.end.y - recorded.start.y) - quantity.metres) >
                constraint_linear_tolerance_metres) {
            invalid("Wall length receipt does not match its stored baseline");
        }
        return receipt.size() != 5 || exact.size() != 2 || receipt.at("baseline").size() != 3;
    } catch (const json::exception&) {
        invalid("Wall length receipt is missing or has malformed required metadata: " + wall.id);
    }
}

void validate_or_clear_length_receipt(Entity& wall, bool write_receipt,
                                      const Quantity* quantity, const Segment& baseline) {
    auto section = wall.extensions.find("constraint_authoring");
    if (section != wall.extensions.end()) {
        if (!section->is_object() || !section->contains("version") ||
            !section->at("version").is_number_integer() || section->at("version") != 1) {
            invalid("Wall has unsupported constraint_authoring extension metadata: " + wall.id);
        }
        const auto receipt = section->find("last_length_entry");
        if (receipt != section->end()) {
            const bool opaque_metadata = validate_length_receipt(*receipt, wall);
            if (!write_receipt && opaque_metadata) {
                invalid("Wall edit would discard unsupported last_length_entry metadata: " + wall.id);
            }
        }
    }
    if (write_receipt) {
        if (quantity == nullptr) {
            invalid("Wall length receipt is missing its exact quantity");
        }
        if (section == wall.extensions.end()) {
            wall.extensions["constraint_authoring"] = ordered_json{{"version", 1}};
            section = wall.extensions.find("constraint_authoring");
        }
        auto& receipt = (*section)["last_length_entry"];
        receipt["version"] = 1;
        receipt["original_expression"] = quantity->original_expression;
        receipt["entered_unit"] = unit_name(quantity->entered_unit);
        receipt["exact_metres"]["numerator"] = quantity->exact_metres.numerator;
        receipt["exact_metres"]["denominator"] = quantity->exact_metres.denominator;
        update_baseline_json(receipt["baseline"], baseline);
    } else if (section != wall.extensions.end()) {
        section->erase("last_length_entry");
    }
}

}

void rebase_wall_length_receipt(Entity& wall, const Segment& transformed_baseline) {
    auto section = wall.extensions.find("constraint_authoring");
    if (section == wall.extensions.end()) {
        return;
    }
    if (!section->is_object() || !section->contains("version") ||
        !section->at("version").is_number_integer() || section->at("version") != 1) {
        invalid("Wall has unsupported constraint_authoring extension metadata: " + wall.id);
    }
    auto receipt = section->find("last_length_entry");
    if (receipt == section->end()) {
        return;
    }
    (void)validate_length_receipt(*receipt, wall);
    const auto original = read_baseline(wall);
    const auto& transformed = transformed_baseline;
    const auto original_length = std::hypot(original.end.x - original.start.x,
                                             original.end.y - original.start.y);
    const auto transformed_length = std::hypot(transformed.end.x - transformed.start.x,
                                                transformed.end.y - transformed.start.y);
    if (transformed.sweep_radians != 0.0 ||
        !std::isfinite(transformed.start.x) || !std::isfinite(transformed.start.y) ||
        !std::isfinite(transformed.end.x) || !std::isfinite(transformed.end.y) ||
        !std::isfinite(original_length) || !std::isfinite(transformed_length) ||
        transformed_length <= 0.0 ||
        std::abs(transformed_length - original_length) > constraint_linear_tolerance_metres) {
        invalid("Wall length receipt requires a finite length-preserving straight transform: " + wall.id);
    }
    auto updated = *receipt;
    update_baseline_json(updated.at("baseline"), transformed);
    auto transformed_wall = wall;
    set_baseline(transformed_wall, transformed);
    (void)validate_length_receipt(updated, transformed_wall);
    receipt->swap(updated);
}

namespace {
bool valid_wall_identifier(const std::string& id) {
    return !id.empty() && id.size() <= 128 &&
        std::all_of(id.begin(),id.end(),[](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == ':';
        });
}
void validate_edit(const ConstraintWallGeometryEdit& edit) {
    const auto& b = edit.baseline;
    const auto baseline_length = std::hypot(b.end.x-b.start.x,b.end.y-b.start.y);
    if (!valid_wall_identifier(edit.wall_id) || b.sweep_radians != 0.0 ||
        !std::isfinite(b.start.x) || !std::isfinite(b.start.y) ||
        !std::isfinite(b.end.x) || !std::isfinite(b.end.y) ||
        !std::isfinite(baseline_length) || baseline_length <= constraint_linear_tolerance_metres)
        invalid("Wall constraint edit requires an identified finite straight nondegenerate baseline");
    if (edit.length_entry) {
        const auto length = normalize_positive_quantity(*edit.length_entry);
        if (std::abs(std::hypot(b.end.x-b.start.x,b.end.y-b.start.y)-length.metres) >
            constraint_linear_tolerance_metres)
            invalid("Wall constraint edit exact quantity does not match its baseline");
    }
}
void exact_fields(const json& value, std::initializer_list<const char*> fields) {
    if (!value.is_object() || value.size() != fields.size())
        invalid("Wall constraint edit contains unexpected fields");
    for (const auto* field : fields)
        if (!value.contains(field)) invalid("Wall constraint edit is missing a required field");
}
}

Entity replay_constraint_wall_edit(const Entity& source, const ConstraintWallGeometryEdit& edit) {
    validate_edit(edit);
    if (source.id != edit.wall_id || source.type != "wall")
        invalid("Wall constraint edit owner is not its original wall");
    const auto old = read_baseline(source);
    if (old.sweep_radians != 0.0)
        invalid("Wall constraint solving requires an entirely straight wall");
    const auto near = [](Vec2 a, Vec2 b) {
        return std::hypot(a.x-b.x,a.y-b.y) <= constraint_linear_tolerance_metres;
    };
    if (near(old.start, edit.baseline.end) && near(old.end, edit.baseline.start))
        invalid("Wall constraint edit would reverse wall endpoint identity");
    auto result = source;
    validate_or_clear_length_receipt(result, edit.length_entry.has_value(),
        edit.length_entry ? &*edit.length_entry : nullptr, edit.baseline);
    set_baseline(result, edit.baseline);
    return result;
}

nlohmann::json encode_constraint_wall_edit(const ConstraintWallGeometryEdit& edit) {
    validate_edit(edit);
    json b = json::object();
    update_baseline_json(b, edit.baseline);
    json receipt = nullptr;
    if (edit.length_entry) {
        const auto& q = *edit.length_entry;
        receipt = {{"original_expression",q.original_expression}, {"entered_unit",unit_name(q.entered_unit)},
            {"exact_metres",{{"numerator",q.exact_metres.numerator},{"denominator",q.exact_metres.denominator}}}};
    }
    return {{"wall_id",edit.wall_id},{"baseline",b},{"length_entry",receipt}};
}

ConstraintWallGeometryEdit decode_constraint_wall_edit(const nlohmann::json& value) {
    exact_fields(value,{"wall_id","baseline","length_entry"});
    exact_fields(value.at("baseline"),{"start","end","sweep_radians"});
    if (!value.at("wall_id").is_string()) invalid("Wall constraint owner ID must be a string");
    Entity temporary{value.at("wall_id").get<std::string>(),"wall",{{"baseline",value.at("baseline")}}};
    ConstraintWallGeometryEdit result{temporary.id,read_baseline(temporary),std::nullopt};
    const auto& entry = value.at("length_entry");
    if (!entry.is_null()) {
        exact_fields(entry,{"original_expression","entered_unit","exact_metres"});
        exact_fields(entry.at("exact_metres"),{"numerator","denominator"});
        if (!entry.at("original_expression").is_string() || !entry.at("entered_unit").is_string())
            invalid("Wall constraint length expression and unit must be strings");
        std::optional<Unit> unit;
        for (const auto candidate : {Unit::metre,Unit::millimetre,Unit::centimetre,Unit::foot,Unit::inch})
            if (entry.at("entered_unit") == unit_name(candidate)) unit = candidate;
        if (!unit) invalid("Wall constraint length has an unsupported unit");
        const auto q = parse_quantity(entry.at("original_expression").get<std::string>(),*unit);
        if (q.entered_unit != *unit || !entry.at("exact_metres").at("numerator").is_number_integer() ||
            !entry.at("exact_metres").at("denominator").is_number_integer() ||
            entry.at("exact_metres").at("numerator") != q.exact_metres.numerator ||
            entry.at("exact_metres").at("denominator") != q.exact_metres.denominator)
            invalid("Wall constraint length has inconsistent exact quantity metadata");
        result.length_entry = q;
    }
    validate_edit(result);
    return result;
}
void validate_constraint_wall_host(const std::string& wall_id, const std::map<std::string, Entity, std::less<>>& entities) {
    const auto& entity = entities.at(wall_id);
    const auto baseline = read_baseline(entity);
    try {
        Wall wall{
            wall_id,
            baseline,
            finite_number(entity.properties.at("thickness_m"), "Wall thickness"),
            finite_number(entity.properties.at("height_m"), "Wall height"),
            finite_number(entity.properties.at("elevation_m"), "Wall elevation"),
            {},
        };
        if (const auto layers = entity.properties.find("layers");
            layers != entity.properties.end()) {
            wall.layers = parse_wall_layers(layers.value(), wall.thickness);
        }
        if (const auto slope = entity.properties.find("slope_rise_m");
            slope != entity.properties.end()) {
            if (!slope->is_number()) {
                invalid("Wall slope_rise_m must be a finite number");
            }
            wall.slope_rise = slope->get<double>();
        }
        for (const auto& [id, candidate] : entities) {
            if (candidate.type != "opening" || !candidate.properties.is_object()) {
                continue;
            }
            const auto host = candidate.properties.find("wall_id");
            if (host == candidate.properties.end() || !host->is_string() ||
                host->get_ref<const std::string&>() != wall_id) {
                continue;
            }
            wall.openings.push_back(
                {id, finite_number(candidate.properties.at("offset_m"), "Opening offset"),
                 finite_number(candidate.properties.at("width_m"), "Opening width"),
                 finite_number(candidate.properties.at("sill_m"), "Opening sill"),
                 finite_number(candidate.properties.at("height_m"), "Opening height")});
        }
        validate_wall_semantics(wall);
    } catch (const std::out_of_range&) {
        invalid("Wall or hosted opening is missing required geometry: " + wall_id);
    }
}

} // namespace sketch
