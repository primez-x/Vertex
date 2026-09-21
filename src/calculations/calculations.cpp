#include "sketch/calculations.hpp"
#include "sketch/architecture.hpp"
#include <BRepAlgoAPI_Common.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <Standard_Failure.hxx>
#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <locale>
#include <set>
#include <sstream>
#include <stdexcept>

namespace sketch {
namespace {
constexpr std::array category_names{
    std::pair{AppraisalAreaCategory::none, std::string_view{"none"}},
    std::pair{AppraisalAreaCategory::above_grade_finished, std::string_view{"above_grade_finished"}},
    std::pair{AppraisalAreaCategory::above_grade_unfinished, std::string_view{"above_grade_unfinished"}},
    std::pair{AppraisalAreaCategory::below_grade_finished, std::string_view{"below_grade_finished"}},
    std::pair{AppraisalAreaCategory::below_grade_unfinished, std::string_view{"below_grade_unfinished"}},
    std::pair{AppraisalAreaCategory::garage, std::string_view{"garage"}},
    std::pair{AppraisalAreaCategory::carport, std::string_view{"carport"}},
    std::pair{AppraisalAreaCategory::porch, std::string_view{"porch"}},
    std::pair{AppraisalAreaCategory::patio, std::string_view{"patio"}},
    std::pair{AppraisalAreaCategory::deck, std::string_view{"deck"}},
    std::pair{AppraisalAreaCategory::other_non_living, std::string_view{"other_non_living"}},
    std::pair{AppraisalAreaCategory::above_grade_nonstandard_finished, std::string_view{"above_grade_nonstandard_finished"}},
    std::pair{AppraisalAreaCategory::below_grade_nonstandard_finished, std::string_view{"below_grade_nonstandard_finished"}},
    std::pair{AppraisalAreaCategory::noncontinuous_finished, std::string_view{"noncontinuous_finished"}},
    std::pair{AppraisalAreaCategory::commercial_occupiable, std::string_view{"commercial_occupiable"}},
    std::pair{AppraisalAreaCategory::commercial_common, std::string_view{"commercial_common"}},
    std::pair{AppraisalAreaCategory::commercial_service, std::string_view{"commercial_service"}}};

double tolerance(double area) { return std::max(1e-6, std::abs(area) * 1e-8); }
void profile_valid(const CalculationProfile& p) {
    if (p.id.empty() || p.version == 0 || p.decimal_places > 6)
        throw std::invalid_argument("Calculation profile needs ID, version and 0-6 decimal places");
    switch (p.display_unit) {
    case AreaUnit::square_metre:
    case AreaUnit::square_foot:
    case AreaUnit::acre:
        break;
    default:
        throw std::invalid_argument("Unknown area display unit");
    }
    for (const auto& [name, rule] : p.classifications)
        (void)appraisal_category_name(rule.appraisal_category);
}
double analytical(const Boundary& b) {
    auto issues = validate_boundary(b);
    if (!issues.empty())
        throw std::invalid_argument("Cannot calculate area: " + issues.front().message);
    double value = std::abs(signed_area(b));
    if (!std::isfinite(value) || value <= 0)
        throw std::invalid_argument("Area must be positive and finite");
    return value;
}
TopoDS_Shape face(const Boundary& b, Vec2 origin, double expected) {
    auto local = b;
    for (auto& edge : local) {
        edge.start.x -= origin.x;
        edge.start.y -= origin.y;
        edge.end.x -= origin.x;
        edge.end.y -= origin.y;
    }
    auto shape = make_planar_face(local);
    if (std::abs(surface_area(shape) - expected) > tolerance(expected))
        throw std::invalid_argument("Planar geometry disagrees with analytical area");
    return shape;
}
template <class Operation> TopoDS_Shape boolean(const TopoDS_Shape& a, const TopoDS_Shape& b) {
    try {
        Operation operation(a, b);
        operation.Build();
        if (!operation.IsDone() || operation.HasErrors())
            throw std::invalid_argument("Area intersection could not be resolved reliably");
        auto shape = operation.Shape();
        if (!shape.IsNull() && !BRepCheck_Analyzer(shape).IsValid())
            throw std::invalid_argument("Area intersection produced invalid geometry");
        return shape;
    } catch (const Standard_Failure& e) {
        throw std::invalid_argument(std::string("Area intersection failed: ") + e.what());
    }
}
struct Calculated {
    AreaCalculation result;
    TopoDS_Shape net;
};
Calculated calculate(const MeasurementArea& a, const CalculationProfile& p, Vec2 origin) {
    profile_valid(p);
    if (a.scope != AreaScope::building && a.scope != AreaScope::site)
        throw std::invalid_argument("Unknown area calculation scope");
    if (a.id.empty() || a.building_id.empty() || a.floor_id.empty() ||
        !p.classifications.contains(a.classification))
        throw std::invalid_argument("Area needs building/floor IDs and an explicit classification rule");
    if (a.factor.denominator <= 0 || a.factor.numerator < 0)
        throw std::invalid_argument("Area factor must be nonnegative with a positive denominator");
    double base = analytical(a.boundary), previous = base;
    auto original = face(a.boundary, origin, base), remaining = original;
    AreaCalculation r{a.id,       a.building_id,
                      a.floor_id, a.classification,
                      p.id,       p.version,
                      base,       perimeter(a.boundary),
                      {},         0,
                      base,       a.factor,
                      0,          {}, a.scope};
    std::vector<const AreaDeduction*> ordered;
    for (const auto& d : a.deductions)
        ordered.push_back(&d);
    std::sort(ordered.begin(), ordered.end(), [](auto x, auto y) { return x->id < y->id; });
    std::set<std::string> ids;
    for (const auto* d : ordered) {
        if (d->id.empty() || !ids.insert(d->id).second)
            throw std::invalid_argument("Deduction IDs must be nonempty and unique");
        double requested = analytical(d->boundary);
        auto tool = face(d->boundary, origin, requested);
        double contained = surface_area(boolean<BRepAlgoAPI_Common>(original, tool));
        if (std::abs(contained - requested) > tolerance(requested))
            throw std::invalid_argument("Deduction " + d->id + " extends outside area " + a.id);
        auto next = boolean<BRepAlgoAPI_Cut>(remaining, tool);
        double next_area = surface_area(next);
        if (next_area > previous + tolerance(base))
            throw std::invalid_argument("Deduction unexpectedly increased area");
        next_area = std::clamp(next_area, 0.0, previous);
        r.deductions.push_back({d->id, requested, previous - next_area});
        remaining = std::move(next);
        previous = next_area;
    }
    r.net_square_metres = previous;
    r.deducted_square_metres = base - previous;
    r.factored_square_metres =
        previous * (static_cast<double>(a.factor.numerator) / static_cast<double>(a.factor.denominator));
    if (!std::isfinite(r.factored_square_metres))
        throw std::invalid_argument("Factored area overflow");
    r.display = display_area(r.factored_square_metres, p);
    return {std::move(r), std::move(remaining)};
}
AreaTotal total(long double value, const CalculationProfile& p) {
    const auto metres = static_cast<double>(value);
    return {metres, display_area(metres, p)};
}
} // namespace

std::string_view appraisal_category_name(AppraisalAreaCategory category) {
    for (const auto& [value, name] : category_names)
        if (category == value)
            return name;
    throw std::invalid_argument("Unknown appraisal area category");
}

std::optional<AppraisalAreaCategory> parse_appraisal_category(std::string_view name) {
    for (const auto& [value, token] : category_names)
        if (name == token)
            return value;
    return std::nullopt;
}

std::string_view appraisal_policy_kind_name(AppraisalPolicyKind value) {
    switch (value) {
    case AppraisalPolicyKind::residential_declared: return "residential_declared";
    case AppraisalPolicyKind::light_commercial_declared: return "light_commercial_declared";
    }
    throw std::invalid_argument("Unknown appraisal_policy_kind");
}
std::optional<AppraisalPolicyKind> parse_appraisal_policy_kind(std::string_view token) {
    if (token == "residential_declared") return AppraisalPolicyKind::residential_declared;
    if (token == "light_commercial_declared") return AppraisalPolicyKind::light_commercial_declared;
    return std::nullopt;
}

std::string_view property_kind_name(PropertyKind value) {
    switch (value) {
    case PropertyKind::detached_single_family: return "detached_single_family";
    case PropertyKind::attached_single_family: return "attached_single_family";
    case PropertyKind::manufactured_home: return "manufactured_home";
    case PropertyKind::apartment_unit: return "apartment_unit";
    case PropertyKind::multifamily: return "multifamily";
    case PropertyKind::light_commercial: return "light_commercial";
    }
    throw std::invalid_argument("Unknown property_kind");
}
std::optional<PropertyKind> parse_property_kind(std::string_view token) {
    if (token == "detached_single_family") return PropertyKind::detached_single_family;
    if (token == "attached_single_family") return PropertyKind::attached_single_family;
    if (token == "manufactured_home") return PropertyKind::manufactured_home;
    if (token == "apartment_unit") return PropertyKind::apartment_unit;
    if (token == "multifamily") return PropertyKind::multifamily;
    if (token == "light_commercial") return PropertyKind::light_commercial;
    return std::nullopt;
}

std::string_view measurement_basis_name(MeasurementBasis value) {
    switch (value) {
    case MeasurementBasis::exterior: return "exterior";
    case MeasurementBasis::interior_perimeter: return "interior_perimeter";
    case MeasurementBasis::plans: return "plans";
    case MeasurementBasis::unknown: return "unknown";
    }
    throw std::invalid_argument("Unknown measurement_basis");
}
std::optional<MeasurementBasis> parse_measurement_basis(std::string_view token) {
    if (token == "exterior") return MeasurementBasis::exterior;
    if (token == "interior_perimeter") return MeasurementBasis::interior_perimeter;
    if (token == "plans") return MeasurementBasis::plans;
    if (token == "unknown") return MeasurementBasis::unknown;
    return std::nullopt;
}

std::string_view grade_status_name(GradeStatus value) {
    switch (value) {
    case GradeStatus::above: return "above";
    case GradeStatus::below: return "below";
    case GradeStatus::unknown: return "unknown";
    }
    throw std::invalid_argument("Unknown grade_status");
}
std::optional<GradeStatus> parse_grade_status(std::string_view token) {
    if (token == "above") return GradeStatus::above;
    if (token == "below") return GradeStatus::below;
    if (token == "unknown") return GradeStatus::unknown;
    return std::nullopt;
}

std::string_view finish_status_name(FinishStatus value) {
    switch (value) {
    case FinishStatus::finished: return "finished";
    case FinishStatus::unfinished: return "unfinished";
    case FinishStatus::unknown: return "unknown";
    }
    throw std::invalid_argument("Unknown finish_status");
}
std::optional<FinishStatus> parse_finish_status(std::string_view token) {
    if (token == "finished") return FinishStatus::finished;
    if (token == "unfinished") return FinishStatus::unfinished;
    if (token == "unknown") return FinishStatus::unknown;
    return std::nullopt;
}

std::string_view access_status_name(AccessStatus value) {
    switch (value) {
    case AccessStatus::direct_interior: return "direct_interior";
    case AccessStatus::noncontinuous: return "noncontinuous";
    case AccessStatus::unknown: return "unknown";
    }
    throw std::invalid_argument("Unknown access_status");
}
std::optional<AccessStatus> parse_access_status(std::string_view token) {
    if (token == "direct_interior") return AccessStatus::direct_interior;
    if (token == "noncontinuous") return AccessStatus::noncontinuous;
    if (token == "unknown") return AccessStatus::unknown;
    return std::nullopt;
}

std::string_view ceiling_eligibility_name(CeilingEligibility value) {
    switch (value) {
    case CeilingEligibility::standard: return "standard";
    case CeilingEligibility::nonstandard: return "nonstandard";
    case CeilingEligibility::unknown: return "unknown";
    }
    throw std::invalid_argument("Unknown ceiling_eligibility");
}
std::optional<CeilingEligibility> parse_ceiling_eligibility(std::string_view token) {
    if (token == "standard") return CeilingEligibility::standard;
    if (token == "nonstandard") return CeilingEligibility::nonstandard;
    if (token == "unknown") return CeilingEligibility::unknown;
    return std::nullopt;
}

std::string_view area_use_name(AreaUse value) {
    switch (value) {
    case AreaUse::dwelling: return "dwelling";
    case AreaUse::garage: return "garage";
    case AreaUse::carport: return "carport";
    case AreaUse::porch: return "porch";
    case AreaUse::patio: return "patio";
    case AreaUse::deck: return "deck";
    case AreaUse::commercial_occupiable: return "commercial_occupiable";
    case AreaUse::commercial_common: return "commercial_common";
    case AreaUse::commercial_service: return "commercial_service";
    case AreaUse::other_non_living: return "other_non_living";
    }
    throw std::invalid_argument("Unknown area_use");
}
std::optional<AreaUse> parse_area_use(std::string_view token) {
    if (token == "dwelling") return AreaUse::dwelling;
    if (token == "garage") return AreaUse::garage;
    if (token == "carport") return AreaUse::carport;
    if (token == "porch") return AreaUse::porch;
    if (token == "patio") return AreaUse::patio;
    if (token == "deck") return AreaUse::deck;
    if (token == "commercial_occupiable") return AreaUse::commercial_occupiable;
    if (token == "commercial_common") return AreaUse::commercial_common;
    if (token == "commercial_service") return AreaUse::commercial_service;
    if (token == "other_non_living") return AreaUse::other_non_living;
    return std::nullopt;
}

std::string_view boundary_role_name(BoundaryRole value) {
    switch (value) {
    case BoundaryRole::measured_area: return "measured_area";
    case BoundaryRole::open_to_below: return "open_to_below";
    case BoundaryRole::stair_footprint: return "stair_footprint";
    case BoundaryRole::other_void: return "other_void";
    }
    throw std::invalid_argument("Unknown boundary_role");
}
std::optional<BoundaryRole> parse_boundary_role(std::string_view token) {
    if (token == "measured_area") return BoundaryRole::measured_area;
    if (token == "open_to_below") return BoundaryRole::open_to_below;
    if (token == "stair_footprint") return BoundaryRole::stair_footprint;
    if (token == "other_void") return BoundaryRole::other_void;
    return std::nullopt;
}

std::string_view appraisal_policy_id(AppraisalPolicy policy) {
    (void)appraisal_policy_kind_name(policy.kind);
    if (policy.version != 1)
        throw std::invalid_argument("Unsupported appraisal policy version");
    return policy.kind == AppraisalPolicyKind::residential_declared ?
        "vertex-residential-declared-v1" : "vertex-light-commercial-declared-v1";
}

AppraisalQualification derive_appraisal_category(const AppraisalFacts& f, AppraisalPolicy policy,
                                                 ExactRational factor) {
    AppraisalQualification result;
    result.policy_id = appraisal_policy_id(policy);
    result.policy_version = policy.version;
    // Validate every enum, even when a fact is irrelevant to the selected use.
    (void)property_kind_name(f.property_kind);
    (void)measurement_basis_name(f.measurement_basis);
    (void)grade_status_name(f.grade);
    (void)finish_status_name(f.finish);
    (void)access_status_name(f.access);
    (void)ceiling_eligibility_name(f.ceiling);
    (void)area_use_name(f.use);
    (void)boundary_role_name(f.role);
    if (factor.denominator <= 0 || factor.numerator < 0)
        throw std::invalid_argument("Area factor must be nonnegative with a positive denominator");
    auto issue = [&](std::string code, std::string message) {
        result.issues.push_back({std::move(code), std::move(message)});
    };
    if (factor.numerator != factor.denominator)
        issue("factor_not_unity", "Qualified physical area requires a factor exactly equal to one.");
    const bool residential = policy.kind == AppraisalPolicyKind::residential_declared;
    if ((residential && (f.property_kind == PropertyKind::multifamily ||
                         f.property_kind == PropertyKind::light_commercial)) ||
        (!residential && f.property_kind != PropertyKind::light_commercial))
        issue("property_kind_incompatible", "Property kind is unsupported by the selected policy.");
    if (f.measurement_basis == MeasurementBasis::unknown)
        issue("measurement_basis_unknown", "Declare the measurement basis.");
    else if (residential && f.property_kind == PropertyKind::apartment_unit &&
             f.measurement_basis != MeasurementBasis::interior_perimeter)
        issue("measurement_basis_incompatible", "Apartment units require interior perimeter measurements.");
    else if (residential && f.property_kind != PropertyKind::apartment_unit &&
             f.measurement_basis == MeasurementBasis::interior_perimeter)
        issue("measurement_basis_incompatible", "Whole-house residential measurements require exterior or plans basis.");
    const bool commercial_use = f.use == AreaUse::commercial_occupiable ||
        f.use == AreaUse::commercial_common || f.use == AreaUse::commercial_service;
    if (f.role == BoundaryRole::measured_area && residential == commercial_use)
        issue("area_use_incompatible", "Area use is incompatible with the selected policy.");

    std::optional<AppraisalAreaCategory> category;
    if (f.role == BoundaryRole::measured_area && residential && f.use == AreaUse::dwelling) {
        if (f.grade == GradeStatus::unknown)
            issue("grade_unknown", "Declare above grade or below grade; any partly below level is below grade.");
        if (f.finish == FinishStatus::unknown)
            issue("finish_unknown", "Declare finished or unfinished area.");
        if (f.finish == FinishStatus::finished) {
            if (f.access == AccessStatus::unknown)
                issue("access_unknown", "Finished dwelling area requires an access declaration.");
            if (f.ceiling == CeilingEligibility::unknown)
                issue("ceiling_unknown", "Finished dwelling area requires a ceiling eligibility declaration.");
            if (f.access == AccessStatus::noncontinuous && f.grade == GradeStatus::below)
                issue("noncontinuous_below_grade", "Noncontinuous finished area is supported only above grade.");
            if (f.access == AccessStatus::noncontinuous)
                category = AppraisalAreaCategory::noncontinuous_finished;
            else if (f.ceiling == CeilingEligibility::nonstandard)
                category = f.grade == GradeStatus::above ? AppraisalAreaCategory::above_grade_nonstandard_finished :
                                                          AppraisalAreaCategory::below_grade_nonstandard_finished;
            else
                category = f.grade == GradeStatus::above ? AppraisalAreaCategory::above_grade_finished :
                                                          AppraisalAreaCategory::below_grade_finished;
        } else if (f.finish == FinishStatus::unfinished) {
            category = f.grade == GradeStatus::above ? AppraisalAreaCategory::above_grade_unfinished :
                                                      AppraisalAreaCategory::below_grade_unfinished;
        }
    } else if (f.role == BoundaryRole::measured_area) {
        switch (f.use) {
        case AreaUse::garage: category = AppraisalAreaCategory::garage; break;
        case AreaUse::carport: category = AppraisalAreaCategory::carport; break;
        case AreaUse::porch: category = AppraisalAreaCategory::porch; break;
        case AreaUse::patio: category = AppraisalAreaCategory::patio; break;
        case AreaUse::deck: category = AppraisalAreaCategory::deck; break;
        case AreaUse::other_non_living: category = AppraisalAreaCategory::other_non_living; break;
        case AreaUse::commercial_occupiable: category = AppraisalAreaCategory::commercial_occupiable; break;
        case AreaUse::commercial_common: category = AppraisalAreaCategory::commercial_common; break;
        case AreaUse::commercial_service: category = AppraisalAreaCategory::commercial_service; break;
        case AreaUse::dwelling: break;
        }
    }
    result.qualified = result.issues.empty();
    if (result.qualified)
        result.derived_category = category;
    return result;
}

AppraisalQualification qualify_appraisal_area(const MeasurementArea& area, const AppraisalFacts& facts,
                                              AppraisalPolicy policy) {
    auto result = derive_appraisal_category(facts, policy, area.factor);
    auto measured = area;
    measured.classification = "physical";
    const CalculationProfile physical{"vertex-physical", 1, AreaUnit::square_metre, 2,
                                       {{"physical", {false, false}}}};
    const auto calculation = calculate_area(measured, physical);
    result.physical_square_metres = calculation.net_square_metres;
    result.adjusted_square_metres = calculation.factored_square_metres;
    if (area.scope != AreaScope::building) {
        result.issues.push_back({"scope_incompatible", "Declared appraisal policies require building scope."});
        result.qualified = false;
        result.derived_category.reset();
    }
    return result;
}

double AppraisalTotals::commercial_gross_square_metres() const {
    return by_category.at(AppraisalAreaCategory::commercial_occupiable).total.square_metres +
           by_category.at(AppraisalAreaCategory::commercial_common).total.square_metres +
           by_category.at(AppraisalAreaCategory::commercial_service).total.square_metres;
}

double AppraisalTotals::nonstandard_finished_square_metres() const {
    return by_category.at(AppraisalAreaCategory::above_grade_nonstandard_finished).total.square_metres +
           by_category.at(AppraisalAreaCategory::below_grade_nonstandard_finished).total.square_metres;
}

CalculationProfile builtin_appraisal_profile() {
    CalculationProfile profile{"vertex-appraisal", 1, AreaUnit::square_foot, 2, {}};
    for (const auto& [category, name] : category_names) {
        if (category != AppraisalAreaCategory::none)
            profile.classifications.emplace(std::string(name), ClassificationRule{
                true, category == AppraisalAreaCategory::above_grade_finished, category});
    }
    return profile;
}

AppraisalCalculationReport calculate_appraisal_areas(const std::vector<MeasurementArea>& areas,
                                                     const CalculationProfile& profile) {
    AppraisalCalculationReport report;
    report.calculation = calculate_areas(areas, profile);
    struct Accumulator {
        std::map<AppraisalAreaCategory, long double> values;
        std::map<AppraisalAreaCategory, std::vector<std::string>> ids;
        void add(AppraisalAreaCategory category, const AreaCalculation& area) {
            if (category == AppraisalAreaCategory::none)
                return;
            values[category] += area.factored_square_metres;
            ids[category].push_back(area.area_id);
        }
        AppraisalTotals finish(const CalculationProfile& profile) const {
            AppraisalTotals result;
            for (const auto& [category, name] : category_names) {
                if (category == AppraisalAreaCategory::none)
                    continue;
                const auto value = values.find(category);
                const auto provenance = ids.find(category);
                result.by_category.emplace(category, AppraisalAreaBucket{
                    total(value == values.end() ? 0 : value->second, profile),
                    provenance == ids.end() ? std::vector<std::string>{} : provenance->second});
            }
            return result;
        }
    } property;
    std::map<std::string, Accumulator> buildings;
    std::map<std::pair<std::string, std::string>, Accumulator> floors;
    for (const auto& area : report.calculation.areas) {
        if (area.scope != AreaScope::building)
            continue;
        const auto category = profile.classifications.at(area.classification).appraisal_category;
        property.add(category, area);
        buildings[area.building_id].add(category, area);
        floors[{area.building_id, area.floor_id}].add(category, area);
    }
    report.property = property.finish(profile);
    for (const auto& [id, accumulator] : buildings)
        report.by_building.emplace(id, accumulator.finish(profile));
    for (const auto& [id, accumulator] : floors)
        report.by_floor.emplace(id, accumulator.finish(profile));
    return report;
}

AreaCalculation calculate_area(const MeasurementArea& a, const CalculationProfile& p) {
    (void)analytical(a.boundary);
    return calculate(a, p, a.boundary.front().start).result;
}
CalculationReport calculate_areas(const std::vector<MeasurementArea>& areas, const CalculationProfile& p) {
    profile_valid(p);
    CalculationReport report;
    report.profile_id = p.id;
    report.profile_version = p.version;
    std::set<std::string> ids;
    std::map<std::pair<std::string, std::string>, Vec2> origins;
    std::vector<Calculated> calculated;
    std::vector<const MeasurementArea*> ordered;
    for (const auto& a : areas)
        ordered.push_back(&a);
    std::sort(ordered.begin(), ordered.end(), [](auto x, auto y) { return x->id < y->id; });
    long double building = 0, living = 0;
    std::map<std::string, long double> classifications;
    for (const auto* a : ordered) {
        if (!ids.insert(a->id).second)
            throw std::invalid_argument("Duplicate area ID in aggregation");
        (void)analytical(a->boundary);
        // Shared local coordinates keep large survey offsets out of CAD booleans.
        auto origin = origins.try_emplace({a->building_id, a->floor_id}, a->boundary.front().start).first;
        auto item = calculate(*a, p, origin->second);
        for (const auto& prior : calculated) {
            if (prior.result.scope != a->scope || prior.result.building_id != a->building_id || prior.result.floor_id != a->floor_id)
                continue;
            double overlap = surface_area(boolean<BRepAlgoAPI_Common>(prior.net, item.net));
            if (overlap > tolerance(std::min(prior.result.net_square_metres, item.result.net_square_metres)))
                throw std::invalid_argument("Areas " + prior.result.area_id + " and " + a->id +
                                            " overlap on the same floor");
        }
        long double value = item.result.factored_square_metres;
        auto rule = p.classifications.at(a->classification);
        if (a->scope == AreaScope::building && rule.building_total)
            building += value;
        if (a->scope == AreaScope::building && rule.living_total)
            living += value;
        classifications[a->classification] += value;
        report.areas.push_back(item.result);
        calculated.push_back(std::move(item));
    }
    for (const auto& [name, value] : classifications)
        report.by_classification.emplace(name, total(value, p));
    report.building = total(building, p);
    report.living = total(living, p);
    return report;
}
DisplayArea display_area(double metres, const CalculationProfile& p) {
    profile_valid(p);
    if (!std::isfinite(metres) || metres < 0)
        throw std::invalid_argument("Area must be finite and nonnegative");
    double divisor = p.display_unit == AreaUnit::square_foot ? 0.09290304
                     : p.display_unit == AreaUnit::acre      ? 4046.8564224
                                                             : 1.0;
    double value = metres / divisor, scale = std::pow(10.0, p.decimal_places);
    if (!std::isfinite(value * scale))
        throw std::invalid_argument("Area display exceeds numeric range");
    double rounded = std::round(value * scale) / scale;
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::fixed << std::setprecision(static_cast<int>(p.decimal_places)) << rounded;
    return {p.display_unit, value, rounded, rounded - value, out.str()};
}
} // namespace sketch
