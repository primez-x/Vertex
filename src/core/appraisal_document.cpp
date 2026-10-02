#include "sketch/appraisal_document.hpp"
#include "sketch/boundary_entity.hpp"
#include "sketch/wall_measurement.hpp"
#include "sketch/document_digest.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <limits>
#include <map>
#include <numeric>
#include <set>
#include <stdexcept>
#include <string_view>

namespace sketch {
namespace {

using Json = nlohmann::json;

bool boundary_type(std::string_view type) {
    // Architectural room boundaries remain an independent model and never
    // become appraisal measurement geometry merely because they are closed.
    return type == "boundary" || type == "measurement_boundary";
}

std::optional<std::string> text(const Json& object, std::string_view key) {
    const auto name = std::string(key);
    const auto found = object.find(name);
    if (found == object.end()) return std::nullopt;
    if (!found->is_string())
        throw std::invalid_argument(name + " must be a string");
    return found->get<std::string>();
}

Vec2 point(const Json& value) {
    if (!value.is_array() || value.size() != 2 || !value[0].is_number() ||
        !value[1].is_number())
        throw std::invalid_argument("boundary point must contain two numbers");
    Vec2 result{value[0].get<double>(), value[1].get<double>()};
    if (!std::isfinite(result.x) || !std::isfinite(result.y))
        throw std::invalid_argument("boundary point must be finite");
    return result;
}

Segment segment(const Json& value) {
    if (!value.is_object() || !value.contains("start") || !value.contains("end"))
        throw std::invalid_argument("boundary segment is incomplete");
    double sweep = 0.0;
    if (const auto found = value.find("sweep_radians"); found != value.end()) {
        if (!found->is_number())
            throw std::invalid_argument("boundary sweep must be numeric");
        sweep = found->get<double>();
    }
    if (!std::isfinite(sweep))
        throw std::invalid_argument("boundary sweep must be finite");
    return {point(value.at("start")), point(value.at("end")), sweep};
}

Boundary geometry(const Entity& entity) {
    const auto format = inspect_boundary_entity_version(entity);
    if (format.format == BoundaryEntityFormat::identified_v1)
        return boundary_geometry(decode_identified_boundary_entity(entity));
    if (format.format == BoundaryEntityFormat::unsupported_version)
        throw std::invalid_argument(format.diagnostic);
    const Json* values = nullptr;
    if (entity.properties.contains("boundary")) values = &entity.properties.at("boundary");
    else if (entity.properties.contains("segments")) values = &entity.properties.at("segments");
    if (values == nullptr || !values->is_array())
        throw std::invalid_argument("boundary geometry is missing");
    Boundary result;
    result.reserve(values->size());
    for (const auto& value : *values) result.push_back(segment(value));
    if (result.empty()) throw std::invalid_argument("boundary geometry is empty");
    return result;
}

std::vector<std::string> deduction_ids(const Entity& entity) {
    const auto found = entity.properties.find("deduction_ids");
    if (found == entity.properties.end()) return {};
    if (!found->is_array())
        throw std::invalid_argument("deduction_ids must be an array");
    std::vector<std::string> result;
    std::set<std::string, std::less<>> unique;
    for (const auto& value : *found) {
        if (!value.is_string() || value.get_ref<const std::string&>().empty() ||
            !unique.insert(value.get_ref<const std::string&>()).second)
            throw std::invalid_argument("deduction_ids must contain unique nonempty IDs");
        result.push_back(value.get<std::string>());
    }
    return result;
}

ExactRational factor(const Json& properties) {
    if (properties.contains("factor_numerator") || properties.contains("factor_denominator")) {
        if (!properties.contains("factor_numerator") ||
            !properties.contains("factor_denominator") ||
            !properties.at("factor_numerator").is_number_integer() ||
            !properties.at("factor_denominator").is_number_integer())
            throw std::invalid_argument("factor numerator and denominator must be integers");
        auto numerator = properties.at("factor_numerator").get<std::int64_t>();
        auto denominator = properties.at("factor_denominator").get<std::int64_t>();
        if (numerator < 0 || denominator <= 0)
            throw std::invalid_argument("factor must be nonnegative with a positive denominator");
        const auto divisor = std::gcd(numerator, denominator);
        return {numerator / divisor, denominator / divisor};
    }
    const auto found = properties.find("factor");
    if (found == properties.end()) return {1, 1};
    if (!found->is_number()) throw std::invalid_argument("factor must be numeric");
    const auto value = found->get<double>();
    if (!std::isfinite(value) || value < 0.0 || value > 1000000.0)
        throw std::invalid_argument("factor is outside the supported range");
    constexpr std::int64_t scale = 1000000000;
    const auto numerator = static_cast<std::int64_t>(std::llround(value * scale));
    const auto divisor = std::gcd(numerator, scale);
    return {numerator / divisor, scale / divisor};
}

std::string scope(const Entity& entity) {
    if (const auto declared = text(entity.properties, "calculation_scope")) {
        if (*declared != "building" && *declared != "site")
            throw std::invalid_argument("calculation_scope must be building or site");
        return *declared;
    }
    const auto classification = text(entity.properties, "measurement_classification")
        .value_or(text(entity.properties, "classification").value_or(""));
    return classification == "survey" ? "site" : "building";
}

void exact_keys(const Json& value, std::initializer_list<std::string_view> allowed) {
    if (!value.is_object())
        throw std::invalid_argument("appraisal declaration must be an object");
    for (const auto& [key, ignored] : value.items()) {
        (void)ignored;
        if (std::find(allowed.begin(), allowed.end(), key) == allowed.end())
            throw std::invalid_argument("unknown appraisal declaration: " + key);
    }
}

struct Declarations {
    std::optional<AppraisalPolicy> policy;
    AppraisalFacts facts;
    std::vector<std::string> missing;
};

Declarations declarations(const Entity& property, const Entity& floor,
                          const Entity& boundary) {
    Declarations result;
    const auto declaration_object = [](const Json& owner, const char* key) {
        const auto found = owner.find(key);
        if (found == owner.end()) return Json::object();
        if (!found->is_object())
            throw std::invalid_argument(std::string(key) + " must be an object");
        return *found;
    };
    const auto token = [&](const Json& object, const char* key, auto parser, auto& target) {
        const auto found = object.find(key);
        if (found == object.end()) {
            result.missing.push_back(std::string("Declare ") + key + '.');
            return;
        }
        if (!found->is_string())
            throw std::invalid_argument(std::string(key) + " must be a string");
        const auto parsed = parser(found->get<std::string>());
        if (!parsed) throw std::invalid_argument(std::string("unknown appraisal ") + key);
        target = *parsed;
    };
    const auto policy = declaration_object(property.properties, "appraisal_policy");
    exact_keys(policy, {"policy_kind", "version", "property_kind", "measurement_basis"});
    if (!policy.contains("version")) result.missing.push_back("Declare policy version.");
    else if (!policy.at("version").is_number_integer() || policy.at("version") != 1)
        throw std::invalid_argument("unsupported appraisal policy version");
    AppraisalPolicy declared_policy;
    token(policy, "policy_kind", parse_appraisal_policy_kind, declared_policy.kind);
    if (policy.contains("policy_kind") && policy.contains("version"))
        result.policy = declared_policy;
    token(policy, "property_kind", parse_property_kind, result.facts.property_kind);
    token(policy, "measurement_basis", parse_measurement_basis, result.facts.measurement_basis);
    const auto level = declaration_object(floor.properties, "appraisal_facts");
    exact_keys(level, {"grade"});
    token(level, "grade", parse_grade_status, result.facts.grade);
    const auto area = declaration_object(boundary.properties, "appraisal_facts");
    exact_keys(area, {"finish", "access", "ceiling_eligibility", "area_use", "boundary_role"});
    token(area, "finish", parse_finish_status, result.facts.finish);
    token(area, "access", parse_access_status, result.facts.access);
    token(area, "ceiling_eligibility", parse_ceiling_eligibility, result.facts.ceiling);
    token(area, "area_use", parse_area_use, result.facts.use);
    token(area, "boundary_role", parse_boundary_role, result.facts.role);
    if (area.contains("boundary_role") && result.facts.role != BoundaryRole::measured_area) {
        // Voids have no dwelling finish/access/use to declare. Supplied values
        // were still parsed above, so malformed declarations remain errors.
        for (const auto* key : {"grade", "finish", "access", "ceiling_eligibility", "area_use"})
            std::erase(result.missing, std::string("Declare ") + key + '.');
    }
    return result;
}

bool visible(const std::set<std::string, std::less<>>* ids, const std::string& id) {
    return ids == nullptr || ids->contains(id);
}

bool wall_measurement_sources_visible(
    const Entity& boundary, const std::set<std::string, std::less<>>* visible_entity_ids) {
    if (visible_entity_ids == nullptr) return true;
    const auto source = boundary.properties.find("wall_measurement_source");
    if (source == boundary.properties.end()) return true;
    if (!source->is_object() || !source->contains("walls") || !source->at("walls").is_array() ||
        source->at("walls").empty())
        return false;
    for (const auto& record : source->at("walls")) {
        if (!record.is_object() || !record.contains("id") || !record.at("id").is_string() ||
            !visible_entity_ids->contains(record.at("id").get<std::string>()))
            return false;
    }
    return true;
}

struct Context {
    const Entity* floor{};
    const Entity* building{};
};

Context context(const DocumentSnapshot& document, const Entity& boundary,
                const std::string& property_id) {
    const auto boundary_property = text(boundary.properties, "property_id");
    if (boundary_property && !boundary_property->empty() && *boundary_property != property_id)
        throw std::invalid_argument("boundary property_id disagrees with its appraisal property");
    const auto floor_id = text(boundary.properties, "floor_id");
    if (!floor_id || floor_id->empty())
        throw std::invalid_argument("boundary is missing floor_id");
    const auto floor = document.entities().find(*floor_id);
    if (floor == document.entities().end() || floor->second.type != "floor")
        throw std::invalid_argument("boundary references an unknown floor");
    const auto floor_property = text(floor->second.properties, "property_id");
    if (floor_property && !floor_property->empty() && *floor_property != property_id)
        throw std::invalid_argument("boundary floor belongs to a different property");
    const auto floor_building_id = text(floor->second.properties, "building_id");
    if (!floor_building_id || floor_building_id->empty())
        throw std::invalid_argument("boundary floor is missing building_id");
    const auto boundary_building_id = text(boundary.properties, "building_id");
    if (boundary_building_id && !boundary_building_id->empty() &&
        *boundary_building_id != *floor_building_id)
        throw std::invalid_argument("boundary building_id disagrees with its floor");
    const auto building = document.entities().find(*floor_building_id);
    if (building == document.entities().end() || building->second.type != "building")
        throw std::invalid_argument("boundary references an unknown building");
    const auto owner = text(building->second.properties, "property_id");
    if (!owner || *owner != property_id)
        throw std::invalid_argument("boundary building belongs to a different property");
    return {&floor->second, &building->second};
}

bool belongs_to_other_property(const DocumentSnapshot& document, const Entity& boundary,
                               const std::string& property_id) {
    try {
        const auto floor_id = text(boundary.properties, "floor_id");
        if (!floor_id) return false;
        const auto floor = document.entities().find(*floor_id);
        if (floor == document.entities().end() || floor->second.type != "floor") return false;
        const auto building_id = text(floor->second.properties, "building_id");
        if (!building_id) return false;
        const auto building = document.entities().find(*building_id);
        if (building == document.entities().end() || building->second.type != "building") return false;
        const auto owner_id = text(building->second.properties, "property_id");
        if (!owner_id || *owner_id == property_id) return false;
        const auto owner = document.entities().find(*owner_id);
        if (owner == document.entities().end() || owner->second.type != "property") return false;
        (void)context(document, boundary, *owner_id);
        return true;
    } catch (const std::exception&) {
        // Conflicting/malformed ownership cannot safely prove that this source
        // belongs elsewhere. Keep its invalid row, with no numeric contribution.
        return false;
    }
}

} // namespace

CalculationProfile appraisal_display_profile(
    const nlohmann::json& property_properties, AreaUnit display_unit) {
    auto profile = builtin_appraisal_profile();
    profile.display_unit = display_unit;
    const auto configuration = property_properties.find("calculation_profile");
    if (configuration == property_properties.end()) return profile;
    if (!configuration->is_object())
        throw std::invalid_argument("calculation_profile must be an object");
    const auto precision = configuration->find("decimal_places");
    if (precision == configuration->end()) return profile;
    if (precision->is_number_unsigned()) {
        const auto value = precision->get<std::uint64_t>();
        if (value <= 6) {
            profile.decimal_places = static_cast<unsigned>(value);
            return profile;
        }
    } else if (precision->is_number_integer()) {
        const auto value = precision->get<std::int64_t>();
        if (value >= 0 && value <= 6) {
            profile.decimal_places = static_cast<unsigned>(value);
            return profile;
        }
    }
    throw std::invalid_argument("calculation_profile.decimal_places must be an integer from 0 to 6");
}

AppraisalDocumentReport build_appraisal_document_report(
    const DocumentSnapshot& document, const std::string& property_id,
    AreaUnit display_unit,
    const std::set<std::string, std::less<>>* visible_entity_ids) {
    AppraisalDocumentReport result;
    result.revision = document.revision();
    result.property_id = property_id;
    result.source_document_id = document.document_id();
    result.source_entities_sha256 = entity_map_digest(document.entities());
    const auto property = document.entities().find(property_id);
    if (property == document.entities().end() || property->second.type != "property")
        throw std::invalid_argument("appraisal property does not exist");
    const auto workflow = text(property->second.properties, "calculation_workflow").value_or("measurement");
    if (workflow != "measurement" && workflow != "appraisal")
        throw std::invalid_argument("calculation_workflow must be measurement or appraisal");
    result.configured = workflow == "appraisal";
    if (!result.configured) return result;
    CalculationProfile profile;
    std::optional<std::string> display_error;
    try {
        profile = appraisal_display_profile(property->second.properties, display_unit);
        result.display_decimal_places = profile.decimal_places;
    } catch (const std::invalid_argument& error) {
        display_error = std::string("Area display: ") + error.what();
        result.issues.push_back(*display_error);
    }
    if (!property->second.properties.contains("appraisal_policy")) {
        result.issues.push_back("Declare an appraisal policy before producing automatic totals.");
    }

    const auto invalid_boundary = [&](const std::string& id, const std::string& message) {
        auto found = std::find_if(result.boundaries.begin(), result.boundaries.end(),
            [&](const auto& value) { return value.boundary_id == id; });
        if (found == result.boundaries.end()) {
            result.boundaries.push_back({id, false, {}});
            found = std::prev(result.boundaries.end());
        }
        found->qualification = {};
        found->qualification.issues.push_back({"invalid_input", message});
        found->measurement.reset();
        result.issues.push_back(id + ": " + message);
    };

    std::vector<const Entity*> candidates;
    for (const auto& [id, entity] : document.entities()) {
        if (!boundary_type(entity.type) || !visible(visible_entity_ids, id)) continue;
        if (belongs_to_other_property(document, entity, property_id)) continue;
        try {
            if (scope(entity) == "site") continue;
            const auto owner = context(document, entity, property_id);
            (void)owner;
            candidates.push_back(&entity);
        } catch (const std::exception& error) {
            invalid_boundary(id, error.what());
        }
    }

    std::set<std::string, std::less<>> building_deductions;
    for (const auto* entity : candidates) {
        try {
            if (scope(*entity) == "site") continue;
            for (const auto& id : deduction_ids(*entity)) {
                if (id == entity->id)
                    throw std::invalid_argument("a boundary cannot deduct itself");
                if (!visible(visible_entity_ids, id))
                    throw std::invalid_argument("deduction " + id + " is hidden by the active design phase");
                building_deductions.insert(id);
            }
        } catch (const std::exception& error) {
            result.issues.push_back(entity->id + ": " + error.what());
        }
    }

    profile.classifications["unqualified"] = {false, false, AppraisalAreaCategory::none};
    std::vector<MeasurementArea> areas;
    std::map<std::string, MeasurementArea, std::less<>> measurements;
    for (const auto* entity : candidates) {
        try {
            if (scope(*entity) == "site") continue;
            const auto owner = context(document, *entity, property_id);
            const auto floor_id = text(entity->properties, "floor_id").value();
            const auto building_id = owner.building->id;
            if (display_error) throw std::invalid_argument(*display_error);
            const auto declared = declarations(property->second, *owner.floor, *entity);
            result.policy = declared.policy;
            if (!wall_measurement_source_current(document, *entity) ||
                !wall_measurement_sources_visible(*entity, visible_entity_ids)) {
                auto qualification = declared.policy
                    ? derive_appraisal_category(declared.facts, *declared.policy) : AppraisalQualification{};
                if (!declared.policy)
                    qualification.issues.push_back({"undeclared_policy", "Declare an appraisal policy before producing automatic totals."});
                for (const auto& missing : declared.missing)
                    qualification.issues.push_back({"undeclared", missing});
                qualification.qualified = false;
                qualification.derived_category.reset();
                qualification.issues.push_back({
                    "stale_measurement_source",
                    "Exterior measurement is stale; refresh exterior measurement from source walls before producing appraisal totals."});
                const bool exclusion = declared.facts.role != BoundaryRole::measured_area;
                result.boundaries.push_back({entity->id, exclusion, std::move(qualification)});
                result.issues.push_back(entity->id +
                    ": Exterior measurement is stale; refresh exterior measurement from source walls before producing appraisal totals.");
                continue;
            }
            std::vector<AreaDeduction> deductions;
            for (const auto& deduction_id : deduction_ids(*entity)) {
                const auto deduction = document.entities().find(deduction_id);
                if (deduction == document.entities().end() || !boundary_type(deduction->second.type))
                    throw std::invalid_argument("deduction " + deduction_id + " is unavailable");
                if (!visible(visible_entity_ids, deduction_id))
                    throw std::invalid_argument("deduction " + deduction_id + " is hidden by the active design phase");
                const auto deduction_owner = context(document, deduction->second, property_id);
                const auto deduction_floor = text(deduction->second.properties, "floor_id");
                if (!deduction_floor || *deduction_floor != floor_id ||
                    deduction_owner.building->id != building_id)
                    throw std::invalid_argument("deduction " + deduction_id + " must share its parent floor and building");
                if (scope(deduction->second) == "site")
                    throw std::invalid_argument("site boundary " + deduction_id + " cannot be a building deduction");
                if (!deduction_ids(deduction->second).empty())
                    throw std::invalid_argument("deduction " + deduction_id + " cannot contain another deduction");
                if (!wall_measurement_source_current(document, deduction->second) ||
                    !wall_measurement_sources_visible(deduction->second, visible_entity_ids))
                    throw std::invalid_argument("deduction " + deduction_id +
                        " has a stale exterior measurement; refresh exterior measurement from source walls");
                deductions.push_back({deduction_id, geometry(deduction->second)});
            }
            MeasurementArea area{entity->id, building_id, floor_id, "unqualified",
                                 geometry(*entity), std::move(deductions),
                                 factor(entity->properties), AreaScope::building};
            AppraisalQualification qualification;
            std::optional<AreaCalculation> diagnostic;
            if (declared.policy) qualification = qualify_appraisal_area(area, declared.facts, *declared.policy);
            else {
                // Measure valid geometry without deriving policy-specific facts
                // or provenance from AppraisalPolicy's default constructor.
                diagnostic = calculate_area(area, profile);
                qualification.physical_square_metres = diagnostic->net_square_metres;
                qualification.adjusted_square_metres = diagnostic->factored_square_metres;
                qualification.issues.push_back({"undeclared_policy", "Declare an appraisal policy before producing automatic totals."});
                if (area.factor.numerator != area.factor.denominator)
                    qualification.issues.push_back({"factor_not_unity", "Qualified physical area requires a factor exactly equal to one."});
            }
            for (const auto& missing : declared.missing)
                qualification.issues.push_back({"undeclared", missing});
            qualification.qualified = qualification.qualified && declared.missing.empty();
            if (!qualification.qualified) qualification.derived_category.reset();
            const bool exclusion = declared.facts.role != BoundaryRole::measured_area;
            result.boundaries.push_back({entity->id, exclusion, qualification, std::move(diagnostic)});
            if (qualification.qualified && !exclusion) {
                if (!qualification.derived_category)
                    throw std::invalid_argument("qualified measured area has no derived category");
                area.classification = std::string(appraisal_category_name(*qualification.derived_category));
            }
            // Retain only inputs whose geometry and dependencies were validated
            // by qualification. They can still be individually inspected when
            // declarations or property-wide aggregation prevent totals.
            measurements.emplace(entity->id, area);
            if (!qualification.qualified) {
                for (const auto& issue : qualification.issues)
                    result.issues.push_back(entity->id + ": " + issue.message);
                continue;
            }
            if (exclusion) {
                if (!building_deductions.contains(entity->id))
                    result.issues.push_back(entity->id + ": exclusion must be linked as a deduction");
                continue;
            }
            areas.push_back(std::move(area));
        } catch (const std::exception& error) {
            invalid_boundary(entity->id, error.what());
        }
    }
    if (areas.empty()) result.issues.push_back("No qualified building appraisal areas are available.");
    std::sort(result.boundaries.begin(), result.boundaries.end(),
              [](const auto& left, const auto& right) {
                  return left.boundary_id < right.boundary_id;
              });
    result.qualified = result.issues.empty();
    if (result.qualified) {
        try {
            result.calculation = calculate_appraisal_areas(areas, profile);
        } catch (const std::exception& error) {
            result.qualified = false;
            result.issues.push_back(error.what());
        }
    }
    for (auto& boundary : result.boundaries) {
        if (boundary.measurement) continue;
        if (result.calculation) {
            const auto& calculated = result.calculation->calculation.areas;
            const auto found = std::find_if(calculated.begin(), calculated.end(), [&](const auto& value) {
                return value.area_id == boundary.boundary_id;
            });
            if (found != calculated.end()) {
                boundary.measurement = *found;
                continue;
            }
        }
        const auto input = measurements.find(boundary.boundary_id);
        if (input == measurements.end()) continue;
        try {
            // Exclusions never enter areas. An unqualified trace uses the
            // explicit noncontributing classification rather than inventing a
            // category; valid individual categories survive global failures.
            boundary.measurement = calculate_area(input->second, profile);
        } catch (const std::exception& error) {
            result.qualified = false;
            result.calculation.reset();
            invalid_boundary(boundary.boundary_id, error.what());
        }
    }
    std::sort(result.issues.begin(), result.issues.end());
    result.issues.erase(std::unique(result.issues.begin(), result.issues.end()), result.issues.end());
    return result;
}

} // namespace sketch
