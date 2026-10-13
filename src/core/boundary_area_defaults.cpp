#include "sketch/boundary_area_defaults.hpp"
#include "sketch/appraisal_document.hpp"
#include "sketch/boundary_entity.hpp"
#include "sketch/area_type_presets.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>
#include <string_view>

namespace sketch {
namespace {
using json = nlohmann::json;
bool is_closed_boundary_entity(std::string_view type) {
    return type == "boundary" || type == "measurement_boundary" || type == "room_boundary";
}
std::string area_unit_name(AreaUnit unit) {
    switch (unit) {
    case AreaUnit::square_metre: return "square_metre";
    case AreaUnit::square_foot: return "square_foot";
    case AreaUnit::acre: return "acre";
    }
    throw std::invalid_argument("Unknown area display unit");
}
std::optional<std::string> read_string(const json& object, std::string_view key) {
    const auto key_string = std::string(key);
    if (!object.contains(key_string) || !object.at(key_string).is_string()) {
        return std::nullopt;
    }
    return object.at(key_string).get<std::string>();
}

std::string calculation_workflow_name(const json& properties) {
    if (!properties.contains("calculation_workflow")) return "measurement";
    const auto& value = properties.at("calculation_workflow");
    if (!value.is_string()) {
        throw std::invalid_argument("Calculation workflow must be a string");
    }
    const auto workflow = value.get<std::string>();
    if (workflow != "measurement" && workflow != "appraisal") {
        throw std::invalid_argument("Calculation workflow must be measurement or appraisal");
    }
    return workflow;
}

std::optional<std::string> area_classification_for_workflow(
    const json& properties, std::string_view workflow) {
    if (workflow == "appraisal") {
        if (const auto category = read_string(properties, "appraisal_category");
            category.has_value() && !category->empty()) {
            return category;
        }
        // Compatibility for projects written by the first appraisal preview,
        // which stored the appraisal category in the shared classification
        // field. New edits keep both workflows' meanings independently.
        if (const auto legacy = read_string(properties, "classification");
            legacy.has_value() && parse_appraisal_category(*legacy).has_value()) {
            return legacy;
        }
        return std::nullopt;
    }
    if (const auto measurement = read_string(properties, "measurement_classification");
        measurement.has_value() && !measurement->empty()) {
        return measurement;
    }
    return read_string(properties, "classification");
}

AreaUnit parse_area_unit(const json& value) {
    if (!value.is_string()) {
        throw std::invalid_argument("Calculation profile display_unit must be a string");
    }
    const auto name = value.get<std::string>();
    if (name == "square_metre") {
        return AreaUnit::square_metre;
    }
    if (name == "square_foot") {
        return AreaUnit::square_foot;
    }
    if (name == "acre") {
        return AreaUnit::acre;
    }
    throw std::invalid_argument("Calculation profile display_unit is unknown");
}

CalculationProfile read_calculation_profile(const json& properties) {
    if (!properties.contains("calculation_profile")) {
        return default_calculation_profile();
    }
    const auto& value = properties.at("calculation_profile");
    if (!value.is_object()) {
        throw std::invalid_argument("Calculation profile must be an object");
    }
    CalculationProfile profile = default_calculation_profile();
    if (const auto id = read_string(value, "id")) {
        profile.id = *id;
    }
    if (value.contains("version")) {
        if (!value.at("version").is_number_unsigned() ||
            value.at("version").get<std::uint64_t>() > std::numeric_limits<unsigned>::max()) {
            throw std::invalid_argument("Calculation profile version must be positive");
        }
        profile.version = value.at("version").get<unsigned>();
    }
    if (profile.id.empty() || profile.version == 0) {
        throw std::invalid_argument("Calculation profile needs an ID and positive version");
    }
    if (value.contains("display_unit")) {
        profile.display_unit = parse_area_unit(value.at("display_unit"));
    }
    profile.decimal_places = appraisal_display_profile(properties).decimal_places;
    if (value.contains("classifications")) {
        if (!value.at("classifications").is_object()) {
            throw std::invalid_argument("Calculation profile classifications must be an object");
        }
        profile.classifications.clear();
        for (const auto& [name, rule] : value.at("classifications").items()) {
            if (!rule.is_object() || !rule.contains("building_total") ||
                !rule.contains("living_total") || !rule.at("building_total").is_boolean() ||
                !rule.at("living_total").is_boolean()) {
                throw std::invalid_argument("Calculation classification rules must contain boolean totals");
            }
            auto appraisal_category = AppraisalAreaCategory::none;
            if (rule.contains("appraisal_category")) {
                if (!rule.at("appraisal_category").is_string()) {
                    throw std::invalid_argument(
                        "Calculation appraisal categories must be strings");
                }
                const auto parsed = parse_appraisal_category(
                    rule.at("appraisal_category").get<std::string>());
                if (!parsed.has_value()) {
                    throw std::invalid_argument(
                        "Calculation appraisal category is unknown");
                }
                appraisal_category = *parsed;
            }
            profile.classifications.emplace(
                name, ClassificationRule{rule.at("building_total").get<bool>(),
                                         rule.at("living_total").get<bool>(),
                                         appraisal_category});
        }
    }
    return profile;
}

json calculation_profile_json(const CalculationProfile& profile) {
    json classifications = json::object();
    for (const auto& [name, rule] : profile.classifications) {
        classifications[name] = json{{"building_total", rule.building_total},
                                     {"living_total", rule.living_total},
                                     {"appraisal_category",
                                      appraisal_category_name(rule.appraisal_category)}};
    }
    return json{{"id", profile.id},
                {"version", profile.version},
                {"display_unit", area_unit_name(profile.display_unit)},
                {"decimal_places", profile.decimal_places},
                {"classifications", std::move(classifications)}};
}

void appraisal_keys(const json& value, std::initializer_list<std::string_view> allowed) {
    if (!value.is_object()) throw std::invalid_argument("Appraisal declaration must be an object");
    for (const auto& [key, ignored] : value.items()) {
        (void)ignored;
        if (std::find(allowed.begin(), allowed.end(), key) == allowed.end())
            throw std::invalid_argument("Unknown appraisal declaration field: " + key);
    }
}
void validate_declarations(const json& property, const json& floor,
                                             const json& boundary) {
    struct { AppraisalPolicy policy; AppraisalFacts facts; } result;
    const auto token = [&](const json& object, const char* key, auto parser, auto& target) {
        if (!object.contains(key)) {
            return;
        }
        if (!object.at(key).is_string()) throw std::invalid_argument(std::string(key) + " must be a string");
        const auto parsed = parser(object.at(key).get<std::string>());
        if (!parsed) throw std::invalid_argument(std::string("Unknown appraisal ") + key);
        target = *parsed;
    };
    const auto policy = property.value("appraisal_policy", json::object());
    appraisal_keys(policy, {"policy_kind", "version", "property_kind", "measurement_basis", "ansi"});
    if (policy.contains("version") && (!policy.at("version").is_number_integer() ||
        (policy.at("version") != 1 && !(policy.at("version") == 2 && policy.value("policy_kind", json()) == "ansi_z765_2021"))))
        throw std::invalid_argument("Unsupported appraisal policy version");
    if (policy.contains("version")) result.policy.version = policy.at("version").get<unsigned>();
    token(policy, "policy_kind", parse_appraisal_policy_kind, result.policy.kind);
    token(policy, "property_kind", parse_property_kind, result.facts.property_kind);
    token(policy, "measurement_basis", parse_measurement_basis, result.facts.measurement_basis);
    const auto level = floor.value("appraisal_facts", json::object());
    appraisal_keys(level, {"grade", "ansi"});
    token(level, "grade", parse_grade_status, result.facts.grade);
    const auto area = boundary.value("appraisal_facts", json::object());
    appraisal_keys(area, {"finish", "access", "ceiling_eligibility", "area_use", "boundary_role", "ansi"});
    token(area, "finish", parse_finish_status, result.facts.finish);
    token(area, "access", parse_access_status, result.facts.access);
    token(area, "ceiling_eligibility", parse_ceiling_eligibility, result.facts.ceiling);
    token(area, "area_use", parse_area_use, result.facts.use);
    token(area, "boundary_role", parse_boundary_role, result.facts.role);
    if (policy.contains("ansi") || level.contains("ansi") || area.contains("ansi") ||
        result.policy.kind == AppraisalPolicyKind::ansi_z765_2021) {
        AnsiAppraisalFacts evidence;
        const auto object = [](const json& owner, const char* key) {
            const auto value = owner.value(key, json::object());
            if (!value.is_object()) throw std::invalid_argument(std::string(key) + " must be an object");
            return value;
        };
        const auto boolean = [](const json& value, const char* key, std::optional<bool>& out) {
            if (!value.contains(key)) return;
            if (!value.at(key).is_boolean()) throw std::invalid_argument(std::string(key) + " must be boolean");
            out = value.at(key).get<bool>();
        };
        const auto optional_token = [](const json& value, const char* key, auto parser, auto& out) {
            if (!value.contains(key)) return;
            if (!value.at(key).is_string()) throw std::invalid_argument(std::string(key) + " must be a string");
            out = parser(value.at(key).get<std::string>());
            if (!out) throw std::invalid_argument(std::string("Unknown appraisal ") + key);
        };
        const auto text = [](const json& value, const char* key) {
            if (!value.contains(key)) return std::string{};
            if (!value.at(key).is_string()) throw std::invalid_argument(std::string(key) + " must be a string");
            return value.at(key).get<std::string>();
        };
        const auto measurement = object(policy, "ansi");
        appraisal_keys(measurement, {"interior_inspected", "direct_measurement", "acquisition_increment", "limitations_statement", "limitation_declarations"});
        boolean(measurement, "interior_inspected", evidence.measurement.interior_inspected);
        boolean(measurement, "direct_measurement", evidence.measurement.direct_measurement);
        optional_token(measurement, "acquisition_increment", parse_acquisition_increment, evidence.measurement.acquisition_increment);
        evidence.measurement.limitations_statement = text(measurement, "limitations_statement");
        if (measurement.contains("limitation_declarations")) {
            const auto& declarations = measurement.at("limitation_declarations");
            if (!declarations.is_array() || declarations.size() > 3)
                throw std::invalid_argument("limitation_declarations must contain at most three declarations");
            std::set<AnsiDeclarationKind> kinds;
            for (const auto& declaration : declarations) {
                appraisal_keys(declaration, {"kind", "statement"});
                if (!declaration.contains("kind") || !declaration.contains("statement"))
                    throw std::invalid_argument("limitation declaration requires kind and statement");
                const auto kind = parse_ansi_declaration_kind(text(declaration, "kind"));
                if (!kind || !kinds.insert(*kind).second)
                    throw std::invalid_argument("limitation declaration kind is unknown or duplicated");
                const auto statement = text(declaration, "statement");
                if (statement.size() > 8192)
                    throw std::invalid_argument("limitation declaration statement is too long");
                evidence.measurement.limitation_declarations.push_back({*kind, statement});
            }
        }
        const auto floor_evidence = object(level, "ansi");
        appraisal_keys(floor_evidence, {"any_part_below_grade"});
        boolean(floor_evidence, "any_part_below_grade", evidence.any_part_below_grade);
        const auto area_evidence = object(area, "ansi");
        appraisal_keys(area_evidence, {"year_round_suitable", "finish_matches_dwelling", "dwelling_identity", "ceiling"});
        boolean(area_evidence, "year_round_suitable", evidence.year_round_suitable);
        boolean(area_evidence, "finish_matches_dwelling", evidence.finish_matches_dwelling);
        optional_token(area_evidence, "dwelling_identity", parse_dwelling_identity, evidence.dwelling_identity);
        const auto ceiling = object(area_evidence, "ceiling");
        appraisal_keys(ceiling, {"kind", "minimum_height_m", "at_least_7ft_area_m2", "room_floor_area_m2", "complete_room_observed", "below_5ft_deduction_ids", "stair_from_floor_id", "room_boundary_id", "source_geometry_sha256"});
        optional_token(ceiling, "kind", parse_ceiling_kind, evidence.ceiling.kind);
        const auto number = [&](const char* key, std::optional<double>& out) {
            if (!ceiling.contains(key)) return;
            if (!ceiling.at(key).is_number()) throw std::invalid_argument(std::string(key) + " must be numeric");
            const auto value = ceiling.at(key).get<double>();
            if (!std::isfinite(value) || value < 0) throw std::invalid_argument(std::string(key) + " must be finite and nonnegative");
            out = value;
        };
        number("minimum_height_m", evidence.ceiling.minimum_height_m);
        number("at_least_7ft_area_m2", evidence.ceiling.at_least_7ft_area_m2);
        number("room_floor_area_m2", evidence.ceiling.room_floor_area_m2);
        boolean(ceiling, "complete_room_observed", evidence.ceiling.complete_room_observed);
        evidence.ceiling.stair_from_floor_id = text(ceiling, "stair_from_floor_id");
        evidence.ceiling.room_boundary_id = text(ceiling, "room_boundary_id");
        evidence.ceiling.source_geometry_sha256 = text(ceiling, "source_geometry_sha256");
        if (ceiling.contains("below_5ft_deduction_ids")) {
            if (!ceiling.at("below_5ft_deduction_ids").is_array()) throw std::invalid_argument("below_5ft_deduction_ids must be an array");
            std::set<std::string> ids;
            for (const auto& id : ceiling.at("below_5ft_deduction_ids")) {
                if (!id.is_string() || id.get<std::string>().empty() || !ids.insert(id.get<std::string>()).second)
                    throw std::invalid_argument("Choose each low-height boundary once");
                evidence.ceiling.below_5ft_deduction_ids.push_back(id.get<std::string>());
            }
        }
        if (evidence.ceiling.kind) {
            const auto kind = *evidence.ceiling.kind;
            if ((kind != CeilingKind::flat && ceiling.contains("minimum_height_m")) ||
                (kind != CeilingKind::stairs && ceiling.contains("stair_from_floor_id")) ||
                (kind != CeilingKind::sloped && (ceiling.contains("at_least_7ft_area_m2") || ceiling.contains("room_floor_area_m2") ||
                 ceiling.contains("complete_room_observed") || ceiling.contains("below_5ft_deduction_ids") || ceiling.contains("room_boundary_id") || ceiling.contains("source_geometry_sha256"))))
                throw std::invalid_argument("Ceiling evidence does not match its declared kind");
        }
        result.facts.ansi = std::move(evidence);
    }
}

std::optional<std::string> appraisal_boundary_property_id(
    const DocumentSnapshot& snapshot, const Entity& boundary) {
    const auto floor_id = read_string(boundary.properties, "floor_id");
    const auto floor = floor_id ? snapshot.entities().find(*floor_id) : snapshot.entities().end();
    auto building_id = floor != snapshot.entities().end() && floor->second.type == "floor"
        ? read_string(floor->second.properties, "building_id") : std::nullopt;
    if (!building_id) building_id = read_string(boundary.properties, "building_id");
    if (!building_id) return read_string(boundary.properties, "property_id");
    const auto building = snapshot.entities().find(*building_id);
    if (building == snapshot.entities().end() || building->second.type != "building")
        return read_string(boundary.properties, "property_id");
    const auto owner = read_string(building->second.properties, "property_id");
    return owner ? owner : read_string(boundary.properties, "property_id");
}

bool appraisal_property_has_declarations(const DocumentSnapshot& snapshot,
                                        const Entity& property, const std::vector<std::string>& created_ids) {
    if (property.properties.contains("appraisal_policy")) return true;
    for (const auto& [id, boundary] : snapshot.entities()) {
        if (std::find(created_ids.begin(), created_ids.end(), id) != created_ids.end()) continue;
        if (!is_closed_boundary_entity(boundary.type) ||
            appraisal_boundary_property_id(snapshot, boundary) != property.id)
            continue;
        if (read_string(boundary.properties, "calculation_scope").value_or("building") == "site") continue;
        if (boundary.properties.contains("appraisal_facts")) return true;
        const auto floor_id = read_string(boundary.properties, "floor_id");
        const auto floor = floor_id ? snapshot.entities().find(*floor_id) : snapshot.entities().end();
        if (floor != snapshot.entities().end() && floor->second.properties.contains("appraisal_facts")) return true;
    }
    return false;
}

// Type-implied facts remain editable declarations, with their provenance in
// appraisal_fact_defaults. Unknown facts continue withholding totals, and ANSI
// observations are never defaulted.
struct AppraisalClassificationDefaults {
    std::optional<std::string> grade; // Floor-level fact; implied by the type.
    bool grade_from_floor{};          // The type does not imply a grade.
    std::vector<std::pair<std::string, std::string>> area;
};

std::optional<AppraisalClassificationDefaults> appraisal_classification_defaults(
    std::string_view classification, bool ansi) {
    const auto category = parse_appraisal_category(classification);
    if (!category || *category == AppraisalAreaCategory::none) return std::nullopt;
    using C = AppraisalAreaCategory;
    AppraisalClassificationDefaults result;
    const auto dwelling = [&](const char* grade, const char* finish, const char* access, const char* ceiling) {
        result.grade = grade;
        result.area = {{"finish", finish}, {"access", access}};
        if (ceiling != nullptr) result.area.emplace_back("ceiling_eligibility", ceiling);
        result.area.emplace_back("area_use", "dwelling");
    };
    const auto other_use = [&](std::string use) {
        result.grade_from_floor = true;
        result.area = {{"finish", "unknown"}, {"access", "unknown"}, {"ceiling_eligibility", "unknown"},
                       {"area_use", std::move(use)}};
    };
    switch (*category) {
    case C::above_grade_finished: dwelling("above", "finished", "direct_interior", "standard"); break;
    case C::above_grade_unfinished: dwelling("above", "unfinished", "unknown", "unknown"); break;
    case C::below_grade_finished: dwelling("below", "finished", "direct_interior", "standard"); break;
    case C::below_grade_unfinished: dwelling("below", "unfinished", "unknown", "unknown"); break;
    case C::above_grade_nonstandard_finished: dwelling("above", "finished", "direct_interior", "nonstandard"); break;
    case C::below_grade_nonstandard_finished: dwelling("below", "finished", "direct_interior", "nonstandard"); break;
    // Noncontinuous access does not imply ceiling eligibility; it stays Undeclared.
    case C::noncontinuous_finished: dwelling("above", "finished", "noncontinuous", nullptr); break;
    // ADU and detached identities are ANSI evidence and stay explicit.
    case C::adu_above_grade_finished: case C::detached_other_above_grade_finished:
    case C::adu_above_grade_nonstandard_finished: case C::detached_other_above_grade_nonstandard_finished:
        dwelling("above", "finished", "direct_interior", nullptr); break;
    case C::adu_below_grade_finished: case C::detached_other_below_grade_finished:
    case C::adu_below_grade_nonstandard_finished: case C::detached_other_below_grade_nonstandard_finished:
        dwelling("below", "finished", "direct_interior", nullptr); break;
    case C::adu_above_grade_unfinished: case C::detached_other_above_grade_unfinished:
        dwelling("above", "unfinished", "unknown", nullptr); break;
    case C::adu_below_grade_unfinished: case C::detached_other_below_grade_unfinished:
        dwelling("below", "unfinished", "unknown", nullptr); break;
    default: other_use(std::string(appraisal_category_name(*category))); break;
    }
    result.area.emplace_back("boundary_role", "measured_area");
    if (ansi) {
        result.grade.reset();
        result.grade_from_floor = false;
        std::erase_if(result.area, [](const auto& fact) { return fact.first == "ceiling_eligibility"; });
    }
    return result;
}

// The floor's declared reporting level, when one exists, is the user's own
// grade declaration for that floor. Names and elevations are not evidence.
std::optional<std::string> appraisal_declared_floor_level_grade(const Entity& property, const std::string& floor_id) {
    const auto stored = property.properties.find("appraisal_reporting");
    if (stored == property.properties.end()) return std::nullopt;
    try {
        std::optional<std::string> grade;
        for (const auto& unit : parse_appraisal_reporting_settings(*stored).living_units)
            for (const auto& level : unit.levels) {
                if (level.floor_id != floor_id || !level.grade_level_type) continue;
                const std::string value = *level.grade_level_type == "above_grade" ? "above" : "below";
                if (grade && *grade != value) return std::nullopt;
                grade = value;
            }
        return grade;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::vector<EntityChange> residential_setup_changes(const DocumentSnapshot& snapshot,
    const Entity& property, const json& policy) {
    const auto current = calculation_workflow_name(property.properties);
    auto updated = property;
    if (current != "appraisal") {
        (void)read_calculation_profile(property.properties);
        // Preserve the user's exact measurement configuration, including extensions.
        updated.properties["measurement_calculation_profile"] = property.properties.contains("calculation_profile")
            ? property.properties.at("calculation_profile") : calculation_profile_json(default_calculation_profile());
        updated.properties["calculation_profile"] = calculation_profile_json(builtin_appraisal_profile());
    }
    updated.properties["calculation_workflow"] = "appraisal";
    updated.properties["appraisal_policy"] = policy;
    std::vector<EntityChange> result{EntityChange::upsert(std::move(updated))};
    if (current == "appraisal") return result;
    for (const auto& [id, source] : snapshot.entities()) {
        (void)id;
        if (!is_closed_boundary_entity(source.type) || appraisal_boundary_property_id(snapshot, source) != property.id) continue;
        auto boundary = source;
        const auto shared = read_string(boundary.properties, "classification");
        const auto measurement = read_string(boundary.properties, "measurement_classification");
        if (!measurement || measurement->empty())
            boundary.properties["measurement_classification"] = shared && !shared->empty() && !parse_appraisal_category(*shared)
                ? *shared : std::string{"measurement"};
        if (!read_string(boundary.properties, "appraisal_category") && shared && parse_appraisal_category(*shared))
            boundary.properties["appraisal_category"] = *shared;
        if (boundary != source) result.push_back(EntityChange::upsert(std::move(boundary)));
    }
    return result;
}
} // namespace
CalculationProfile default_calculation_profile() {
    auto profile = CalculationProfile{
        "vertex-default",
        2,
        AreaUnit::square_foot,
        1, // design system: one decimal (1,842.5 sf)
        {{"measurement", ClassificationRule{true, false}},
         {"survey", ClassificationRule{false, false}},
         {"room", ClassificationRule{true, true}},
         {"living", ClassificationRule{true, true}},
         {"interior", ClassificationRule{false, false}},
         {"exterior", ClassificationRule{true, false}},
         {"party", ClassificationRule{true, false}}}};
    for (const auto& preset : area_type_presets)
        if (!preset.classification.empty())
            profile.classifications.emplace(std::string(preset.classification),
                ClassificationRule{preset.building_total, preset.living_total});
    return profile;
}
void validate_boundary_area_defaults_intent(const BoundaryAreaDefaultsIntent& intent) {
    if (intent.version != 1) throw std::invalid_argument("Unsupported area defaults algorithm version");
    if (intent.staged_category) {
        const auto category = parse_appraisal_category(*intent.staged_category);
        if (!category || *category == AppraisalAreaCategory::none)
            throw std::invalid_argument("Area defaults require a supported staged appraisal category");
    }
}
AreaTypeDefaultChanges boundary_area_default_changes(const DocumentSnapshot& snapshot,
    const std::string& property_id, const std::vector<std::string>& created_ids,
    const BoundaryAreaDefaultsIntent& intent) {
    validate_boundary_area_defaults_intent(intent);
    const auto& staged = intent.staged_category;
    AreaTypeDefaultChanges result;
    const auto stored_property = snapshot.entities().find(property_id);
    if (stored_property == snapshot.entities().end() || stored_property->second.type != "property") throw std::invalid_argument("Area defaults require a property");
    std::map<std::string, Entity, std::less<>> changed;
    const auto current = [&](const std::string& id) -> Entity& {
        auto found = changed.find(id);
        if (found == changed.end()) found = changed.emplace(id, snapshot.entities().at(id)).first;
        return found->second;
    };
    const auto stage_category = [&] {
        for (const auto& id : created_ids) {
            if (!snapshot.entities().contains(id) || appraisal_boundary_property_id(snapshot, snapshot.entities().at(id)) != property_id)
                throw std::invalid_argument("Area defaults require a created boundary in the source property");
            auto& boundary = current(id);
            if (read_string(boundary.properties, "appraisal_category") ||
                read_string(boundary.properties, "classification").value_or("") != "measurement") continue;
            boundary.properties["measurement_classification"] = "measurement";
            boundary.properties["appraisal_category"] = *staged;
        }
    };
    if (staged && !appraisal_property_has_declarations(snapshot, stored_property->second, created_ids)) {
        const json residential{{"policy_kind", "residential_declared"}, {"version", 1},
                               {"property_kind", "detached_single_family"}, {"measurement_basis", "exterior"}};
        for (auto& change : residential_setup_changes(snapshot, stored_property->second, residential))
            if (change.kind == EntityChangeKind::upsert) changed.insert_or_assign(change.entity.id, change.entity);
        stage_category();
        result.set_up = true;
    }
    if (staged && !result.set_up && calculation_workflow_name(stored_property->second.properties) == "appraisal")
        stage_category();
    const auto& property = changed.contains(property_id) ? changed.at(property_id) : stored_property->second;
    if (calculation_workflow_name(property.properties) == "appraisal" && property.properties.contains("appraisal_policy")) {
        const auto& policy = property.properties.at("appraisal_policy");
        const bool ansi = policy.is_object() && policy.value("policy_kind", json()) == "ansi_z765_2021";
        for (const auto& id : created_ids) {
            if (!snapshot.entities().contains(id) || appraisal_boundary_property_id(snapshot, snapshot.entities().at(id)) != property_id)
                throw std::invalid_argument("Area defaults require a created boundary in the source property");
            auto& boundary = current(id);
            // Interactive commits may keep the chosen token in the
            // shared classification field; read it as Appraisal does.
            const auto category = area_classification_for_workflow(boundary.properties, "appraisal");
            const auto defaults = category ? appraisal_classification_defaults(*category, ansi) : std::nullopt;
            if (!defaults) continue;
            auto facts = boundary.properties.value("appraisal_facts", json::object());
            if (!facts.is_object()) continue;
            json recorded = json::object();
            for (const auto& [key, value] : defaults->area)
                if (!facts.contains(key)) { facts[key] = value; recorded[key] = value; }
            if (!recorded.empty()) {
                boundary.properties["appraisal_facts"] = facts;
                boundary.properties["appraisal_fact_defaults"] = json{{"version", 1}, {"source", "classification"},
                    {"classification", *category}, {"facts", recorded}};
                result.defaulted_type = *category;
            }
            if (!defaults->grade && !defaults->grade_from_floor) continue;
            const auto floor_id = read_string(boundary.properties, "floor_id");
            if (!floor_id || !snapshot.entities().contains(*floor_id) || snapshot.entities().at(*floor_id).type != "floor") continue;
            auto& floor = current(*floor_id);
            auto level = floor.properties.value("appraisal_facts", json::object());
            if (!level.is_object()) continue;
            const auto floor_name = read_string(floor.properties, "name").value_or("this floor");
            if (level.contains("grade")) {
                if (defaults->grade && level.at("grade").is_string() && level.at("grade").get<std::string>() != *defaults->grade)
                    result.kept_notes.push_back(floor_name + " stays " + level.at("grade").get<std::string>() + " grade");
                continue;
            }
            const auto declared_level = appraisal_declared_floor_level_grade(property, *floor_id);
            if (defaults->grade && declared_level && *declared_level != *defaults->grade) {
                result.kept_notes.push_back(floor_name + " grade left undeclared: its reporting level is " + *declared_level + " grade");
                continue;
            }
            const auto grade = defaults->grade ? *defaults->grade : declared_level.value_or("above");
            level["grade"] = grade;
            floor.properties["appraisal_facts"] = level;
            floor.properties["appraisal_fact_defaults"] = json{{"version", 1}, {"source", "classification"},
                {"facts", {{"grade", grade}}}};
        }
    }
    for (auto& [id, entity] : changed)
        if (entity != snapshot.entities().at(id)) result.changes.push_back(EntityChange::upsert(entity));
    // Every recorded declaration must still read as a valid declaration.
    for (const auto& id : created_ids) {
        const auto& boundary = changed.contains(id) ? changed.at(id) : snapshot.entities().at(id);
        const auto floor_id = read_string(boundary.properties, "floor_id").value_or("");
        const auto& floor = changed.contains(floor_id) ? changed.at(floor_id) : snapshot.entities().at(floor_id);
        validate_declarations(property.properties, floor.properties, boundary.properties);
    }
    return result;
}

} // namespace sketch
