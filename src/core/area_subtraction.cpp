#include "sketch/area_subtraction.hpp"
#include "sketch/boundary_entity.hpp"
#include "sketch/calculations.hpp"
#include "sketch/project_organization.hpp"

#include <algorithm>
#include <set>
#include <stdexcept>
#include <vector>

namespace sketch {
namespace {
using Json = nlohmann::json;
[[noreturn]] void invalid(const std::string& message) {
    throw std::invalid_argument("Auto-Subtract: " + message);
}
bool measurement(const Entity& entity) {
    return entity.type == "boundary" || entity.type == "measurement_boundary";
}
std::string text(const Json& value, const char* key) {
    const auto found = value.find(key);
    if (found == value.end()) return {};
    if (!found->is_string()) invalid(std::string(key) + " must be a string");
    return found->get<std::string>();
}
DrawingContext context(const DocumentSnapshot& snapshot, const Entity& entity) {
    if (!measurement(entity) || entity.id.empty()) invalid("choose a measurement area with a stable identity");
    const auto floor = text(entity.properties, "floor_id");
    const auto organization = organize_project(snapshot);
    const auto floor_node = organization.nodes.find(floor);
    if (floor_node == organization.nodes.end() || floor_node->second.type != "floor" || !floor_node->second.issues.empty())
        invalid("area must reference a resolved floor");
    const auto& resolved = floor_node->second.context;
    if (resolved.property_id.empty() || resolved.building_id.empty() || resolved.floor_id != floor)
        invalid("area must have a resolved property, building and floor");
    for (const auto& [key, expected] : std::vector<std::pair<const char*, std::string>>{
        {"property_id", resolved.property_id}, {"building_id", resolved.building_id}}) {
        const auto explicit_id = text(entity.properties, key);
        if (!explicit_id.empty() && explicit_id != expected) invalid(std::string(key) + " disagrees with the area floor");
    }
    const auto layer = text(entity.properties, "layer_id");
    if (!layer.empty()) {
        const auto layer_context = organization.drawing_context(layer);
        if (!layer_context || layer_context->property_id != resolved.property_id ||
            layer_context->building_id != resolved.building_id || layer_context->floor_id != floor)
            invalid("area layer disagrees with its floor");
    }
    return resolved;
}
bool same_owner(const DrawingContext& a, const DrawingContext& b) {
    return a.property_id == b.property_id && a.building_id == b.building_id && a.floor_id == b.floor_id;
}
std::vector<std::string> deductions(const Entity& entity) {
    const auto found = entity.properties.find("deduction_ids");
    if (found == entity.properties.end()) return {};
    if (!found->is_array()) invalid("deduction_ids must be an array");
    std::vector<std::string> result;
    std::set<std::string> ids;
    for (const auto& value : *found) {
        if (!value.is_string() || value.get_ref<const std::string&>().empty() ||
            !ids.insert(value.get<std::string>()).second) invalid("deduction IDs must be unique nonempty strings");
        result.push_back(value.get<std::string>());
    }
    return result;
}
Boundary geometry(const Entity& entity) {
    if (!measurement(entity)) invalid("architectural rooms are independent of measurement adjustments");
    const auto inspected = inspect_boundary_entity_version(entity);
    if (inspected.format == BoundaryEntityFormat::unsupported_version) invalid(inspected.diagnostic);
    const auto identified = inspected.format == BoundaryEntityFormat::identified_v1
        ? entity : upgrade_legacy_boundary_entity(entity);
    const auto boundary = boundary_geometry(decode_identified_boundary_entity(identified));
    const auto diagnostics = validate_boundary(boundary);
    if (!diagnostics.empty()) invalid(diagnostics.front().message);
    return boundary;
}
template<class Parser> auto fact(const Json& object, const char* key, Parser parser) {
    const auto token = text(object, key);
    const auto parsed = parser(token);
    if (!parsed) invalid(std::string("declare a known ") + key + " before choosing an adjustment TYPE");
    return *parsed;
}
}

std::string area_subtraction_type(const DocumentSnapshot& snapshot, const Entity& entity) {
    const auto owner = context(snapshot, entity);
    const auto& property = snapshot.entities().at(owner.property_id);
    const auto workflow = property.properties.value("calculation_workflow", std::string{"measurement"});
    if (workflow == "measurement") {
        auto type = text(entity.properties, "measurement_classification");
        if (type.empty()) type = text(entity.properties, "classification");
        if (type.empty()) invalid("assign a measurement TYPE before choosing an adjustment");
        return type;
    }
    if (workflow != "appraisal") invalid("area calculation workflow is unsupported");
    if (!entity.properties.contains("appraisal_facts")) {
        // A pending authoring TYPE and truly undeclared legacy areas retain
        // explicit classification behavior. Once facts exist, saved categories
        // and role strings are historical metadata rather than TYPE authority.
        const auto authoring_type = text(entity.properties, "classification");
        if (authoring_type.starts_with("role:")) {
            const auto role = parse_boundary_role(authoring_type.substr(5));
            if (!role || *role == BoundaryRole::measured_area) invalid("choose a known exclusion TYPE");
            return "role:" + std::string(boundary_role_name(*role));
        }
        auto category = text(entity.properties, "appraisal_category");
        if (!category.empty() && (!parse_appraisal_category(category) || category == "none"))
            invalid("choose a known appraisal TYPE");
        if (category.empty()) category = text(entity.properties, "classification");
        if (const auto parsed = parse_appraisal_category(category); parsed && *parsed != AppraisalAreaCategory::none)
            return std::string(appraisal_category_name(*parsed));
        invalid("declare the appraisal area TYPE");
    }
    if (!property.properties.contains("appraisal_policy") || !property.properties.at("appraisal_policy").is_object() ||
        !entity.properties.at("appraisal_facts").is_object()) invalid("declare the appraisal area TYPE");
    const auto& facts = entity.properties.at("appraisal_facts");
    for (const auto& [key, value] : facts.items()) {
        (void)value;
        if (key != "boundary_role" && key != "area_use" && key != "finish" && key != "access" && key != "ceiling_eligibility")
            invalid("unknown appraisal declaration: " + key);
    }
    const auto validate_supplied = [&](const char* key, auto parser) {
        if (facts.contains(key)) (void)fact(facts, key, parser);
    };
    // Eligibility facts can be irrelevant to a non-dwelling/void TYPE, but
    // supplied malformed tokens remain invalid under the declaration contract.
    validate_supplied("finish", parse_finish_status);
    validate_supplied("access", parse_access_status);
    validate_supplied("ceiling_eligibility", parse_ceiling_eligibility);
    validate_supplied("area_use", parse_area_use);
    const auto role = fact(facts, "boundary_role", parse_boundary_role);
    if (role != BoundaryRole::measured_area) return "role:" + std::string(boundary_role_name(role));
    const auto use = fact(facts, "area_use", parse_area_use);
    if (use != AreaUse::dwelling) return std::string(area_use_name(use));
    const auto& floor = snapshot.entities().at(owner.floor_id);
    if (!floor.properties.contains("appraisal_facts")) invalid("declare the floor grade for dwelling TYPE");
    const auto grade = fact(floor.properties.at("appraisal_facts"), "grade", parse_grade_status);
    const auto finish = fact(facts, "finish", parse_finish_status);
    if (grade == GradeStatus::unknown || finish == FinishStatus::unknown) invalid("declare grade and finish for dwelling TYPE");
    if (finish == FinishStatus::unfinished)
        return grade == GradeStatus::above ? "above_grade_unfinished" : "below_grade_unfinished";
    const auto access = fact(facts, "access", parse_access_status);
    const auto ceiling = fact(facts, "ceiling_eligibility", parse_ceiling_eligibility);
    if (access == AccessStatus::unknown || ceiling == CeilingEligibility::unknown) invalid("declare access and ceiling for dwelling TYPE");
    if (access == AccessStatus::noncontinuous) return "noncontinuous_finished";
    if (ceiling == CeilingEligibility::nonstandard)
        return grade == GradeStatus::above ? "above_grade_nonstandard_finished" : "below_grade_nonstandard_finished";
    return grade == GradeStatus::above ? "above_grade_finished" : "below_grade_finished";
}

Entity prepare_area_subtraction_target(const DocumentSnapshot& snapshot, const Entity& subtractor,
                                       std::string_view target_id, bool remove) {
    if (!measurement(subtractor) || subtractor.id.empty() || target_id.empty() || subtractor.id == target_id)
        invalid("choose distinct measurement source and target areas");
    const auto found = snapshot.entities().find(target_id);
    if (found == snapshot.entities().end() || !measurement(found->second)) invalid("target measurement area is unavailable");
    auto result = found->second;
    auto ids = deductions(result);
    if (remove) {
        const auto source = snapshot.entities().find(subtractor.id);
        if (source == snapshot.entities().end() || !measurement(source->second)) invalid("subtractor identity is unavailable");
        std::erase(ids, subtractor.id);
        if (ids.empty()) result.properties.erase("deduction_ids");
        else result.properties["deduction_ids"] = ids;
        return result;
    }
    const auto source_owner = context(snapshot, subtractor), target_owner = context(snapshot, result);
    if (!same_owner(source_owner, target_owner)) invalid("source and target must share property, building and floor");
    if (text(subtractor.properties, "calculation_scope") != text(result.properties, "calculation_scope") &&
        (text(subtractor.properties, "calculation_scope") == "site" || text(result.properties, "calculation_scope") == "site"))
        invalid("site and building areas cannot adjust each other");
    if (area_subtraction_type(snapshot, subtractor) == area_subtraction_type(snapshot, result))
        invalid("source and target must have different area TYPEs");
    if (!deductions(subtractor).empty()) invalid("a subtracting area cannot contain another deduction");
    for (const auto& [id, other] : snapshot.entities()) {
        if (!measurement(other) || id == result.id) continue;
        const auto references = deductions(other);
        if (std::find(references.begin(), references.end(), result.id) != references.end())
            invalid("an adjustment target cannot itself be a deduction");
    }
    if (std::find(ids.begin(), ids.end(), subtractor.id) == ids.end()) ids.push_back(subtractor.id);
    std::vector<AreaDeduction> tools;
    for (const auto& id : ids) {
        if (id == result.id) invalid("target cannot deduct itself");
        const auto existing = snapshot.entities().find(id);
        const auto* tool = id == subtractor.id ? &subtractor : existing != snapshot.entities().end() ? &existing->second : nullptr;
        if (!tool || !same_owner(context(snapshot, *tool), target_owner) || !deductions(*tool).empty())
            invalid("existing deduction is missing, nested or belongs to another floor");
        tools.push_back({id, geometry(*tool)});
    }
    const CalculationProfile physical{"vertex-explicit-area-adjustment", 1, AreaUnit::square_metre, 2,
        {{"physical", {false, false}}}};
    (void)calculate_area({result.id, target_owner.building_id, target_owner.floor_id,
        "physical", geometry(result), tools, {1, 1}}, physical);
    result.properties["deduction_ids"] = ids;
    return result;
}
}
