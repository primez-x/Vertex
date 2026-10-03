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
    for (const auto& [value, name] : detail::appraisal_category_tokens)
        if (category == value)
            return name;
    throw std::invalid_argument("Unknown appraisal area category");
}

std::string_view appraisal_policy_kind_name(AppraisalPolicyKind value) {
    switch (value) {
    case AppraisalPolicyKind::residential_declared: return "residential_declared";
    case AppraisalPolicyKind::light_commercial_declared: return "light_commercial_declared";
    case AppraisalPolicyKind::ansi_z765_2021: return "ansi_z765_2021";
    }
    throw std::invalid_argument("Unknown appraisal_policy_kind");
}
std::optional<AppraisalPolicyKind> parse_appraisal_policy_kind(std::string_view token) {
    if (token == "residential_declared") return AppraisalPolicyKind::residential_declared;
    if (token == "light_commercial_declared") return AppraisalPolicyKind::light_commercial_declared;
    if (token == "ansi_z765_2021") return AppraisalPolicyKind::ansi_z765_2021;
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
    case AccessStatus::through_unfinished: return "through_unfinished";
    }
    throw std::invalid_argument("Unknown access_status");
}
std::optional<AccessStatus> parse_access_status(std::string_view token) {
    if (token == "direct_interior") return AccessStatus::direct_interior;
    if (token == "noncontinuous") return AccessStatus::noncontinuous;
    if (token == "unknown") return AccessStatus::unknown;
    if (token == "through_unfinished") return AccessStatus::through_unfinished;
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
    if (policy.kind == AppraisalPolicyKind::ansi_z765_2021) return "vertex-ansi-z765-2021-v1";
    return policy.kind == AppraisalPolicyKind::residential_declared ?
        "vertex-residential-declared-v1" : "vertex-light-commercial-declared-v1";
}

std::string_view acquisition_increment_name(AcquisitionIncrement value) {
    switch (value) { case AcquisitionIncrement::inch: return "inch";
    case AcquisitionIncrement::tenth_foot: return "tenth_foot"; }
    throw std::invalid_argument("Unknown acquisition increment");
}
std::optional<AcquisitionIncrement> parse_acquisition_increment(std::string_view token) {
    if (token == "inch") return AcquisitionIncrement::inch;
    if (token == "tenth_foot") return AcquisitionIncrement::tenth_foot;
    return std::nullopt;
}
double rounded_ansi_ceiling_height_metres(double observed_metres, AcquisitionIncrement increment) {
    double step_metres;
    switch (increment) {
    case AcquisitionIncrement::inch: step_metres = 0.0254; break;
    case AcquisitionIncrement::tenth_foot: step_metres = 0.03048; break;
    default: throw std::invalid_argument("Unknown acquisition increment");
    }
    if (!std::isfinite(observed_metres) || observed_metres < 0)
        throw std::invalid_argument("Ceiling height must be finite and nonnegative");
    const double units = observed_metres / step_metres;
    // Beyond this bound double cannot distinguish every consecutive increment.
    constexpr double maximum_increment_count = 9007199254740991.0; // 2^53 - 1
    if (!std::isfinite(units) || units > maximum_increment_count)
        throw std::invalid_argument("Ceiling height cannot be represented at acquisition precision");
    const double rounded_units = std::round(units);
    const double rounded_metres = rounded_units * step_metres;
    if (rounded_units > maximum_increment_count || !std::isfinite(rounded_metres))
        throw std::invalid_argument("Rounded ceiling height cannot be represented at acquisition precision");
    return rounded_units == 0 ? 0.0 : rounded_metres;
}
std::string_view dwelling_identity_name(DwellingIdentity value) {
    switch (value) { case DwellingIdentity::primary: return "primary";
    case DwellingIdentity::attached_adu: return "attached_adu";
    case DwellingIdentity::detached_adu: return "detached_adu";
    case DwellingIdentity::detached_other: return "detached_other"; }
    throw std::invalid_argument("Unknown dwelling identity");
}
std::optional<DwellingIdentity> parse_dwelling_identity(std::string_view token) {
    if (token == "primary") return DwellingIdentity::primary;
    if (token == "attached_adu") return DwellingIdentity::attached_adu;
    if (token == "detached_adu") return DwellingIdentity::detached_adu;
    if (token == "detached_other") return DwellingIdentity::detached_other;
    return std::nullopt;
}
std::string_view ceiling_kind_name(CeilingKind value) {
    switch (value) { case CeilingKind::flat: return "flat";
    case CeilingKind::sloped: return "sloped"; case CeilingKind::stairs: return "stairs"; }
    throw std::invalid_argument("Unknown ceiling kind");
}
std::optional<CeilingKind> parse_ceiling_kind(std::string_view token) {
    if (token == "flat") return CeilingKind::flat;
    if (token == "sloped") return CeilingKind::sloped;
    if (token == "stairs") return CeilingKind::stairs;
    return std::nullopt;
}

namespace {
AppraisalQualification derive_ansi(const AppraisalFacts& f, ExactRational factor) {
    AppraisalQualification r{"vertex-ansi-z765-2021-v1", 1};
    auto issue = [&](std::string code, std::string message) { r.issues.push_back({std::move(code), std::move(message)}); };
    if (factor.numerator != factor.denominator) issue("factor_not_unity", "ANSI physical area requires a unity factor.");
    if (f.property_kind != PropertyKind::detached_single_family &&
        f.property_kind != PropertyKind::attached_single_family && f.property_kind != PropertyKind::manufactured_home)
        issue("property_kind_incompatible", "ANSI applies to attached, detached and manufactured single-family dwellings.");
    if (f.measurement_basis != MeasurementBasis::exterior && f.measurement_basis != MeasurementBasis::plans)
        issue("measurement_basis_incompatible", "ANSI requires exterior measurements or a declared plans basis.");
    if (!f.ansi) { issue("ansi_facts_missing", "Declare ANSI measurement, floor and area evidence."); return r; }
    const auto& a = *f.ansi;
    if (a.dwelling_identity) (void)dwelling_identity_name(*a.dwelling_identity);
    if (a.ceiling.kind) (void)ceiling_kind_name(*a.ceiling.kind);
    if (!a.measurement.interior_inspected) issue("interior_inspected_missing", "Declare whether the interior was inspected.");
    if (!a.measurement.direct_measurement) issue("direct_measurement_missing", "Declare whether the dwelling was directly measured.");
    if (!a.measurement.acquisition_increment) issue("acquisition_increment_missing", "Declare inch or tenth-foot acquisition precision.");
    else (void)acquisition_increment_name(*a.measurement.acquisition_increment);
    if ((f.measurement_basis == MeasurementBasis::plans || a.measurement.interior_inspected == false ||
         a.measurement.direct_measurement == false) &&
        a.measurement.limitations_statement.find_first_not_of(" \t\r\n") == std::string::npos)
        issue("limitations_statement_missing", "Explain measurement limitations for plans, an uninspected interior or indirect measurements.");
    if (!a.any_part_below_grade) issue("floor_grade_missing", "Declare whether any part of this floor is below grade.");
    const bool below = a.any_part_below_grade.value_or(false);
    if (f.grade != GradeStatus::unknown && f.grade != (below ? GradeStatus::below : GradeStatus::above))
        issue("grade_contradiction", "Floor grade contradicts any_part_below_grade; resolve the declarations.");
    if (f.use == AreaUse::commercial_occupiable || f.use == AreaUse::commercial_common || f.use == AreaUse::commercial_service)
        issue("area_use_incompatible", "Commercial uses are outside this ANSI single-family policy.");
    const bool measured = f.role == BoundaryRole::measured_area || f.role == BoundaryRole::stair_footprint;
    if (f.role == BoundaryRole::stair_footprint && a.ceiling.kind != CeilingKind::stairs)
        issue("stair_evidence_required", "An ANSI stair footprint must declare stairs and its descending source floor.");
    if (a.ceiling.kind == CeilingKind::stairs && f.role != BoundaryRole::stair_footprint)
        issue("stair_role_required", "Only a descending stair footprint may use the stairs ceiling case.");
    if (a.ceiling.kind == CeilingKind::stairs && a.ceiling.stair_from_floor_id.empty())
        issue("stair_source_missing", "Stairs require the floor from which they descend.");
    std::optional<AppraisalAreaCategory> category;
    if (measured && f.use == AreaUse::dwelling) {
        if (!a.dwelling_identity) issue("dwelling_identity_missing", "Declare primary dwelling, attached ADU, detached ADU or other detached structure.");
        else (void)dwelling_identity_name(*a.dwelling_identity);
        if (f.finish == FinishStatus::unknown) issue("finish_unknown", "Declare finished or unfinished area.");
        bool finished = f.finish == FinishStatus::finished;
        bool nonstandard = false;
        if (finished) {
            if (!a.year_round_suitable || !a.finish_matches_dwelling) issue("finish_evidence_missing", "Declare year-round suitability and finish comparable to the dwelling.");
            if (a.year_round_suitable == false || a.finish_matches_dwelling == false) {
                finished = false;
                r.rule_notes.push_back("Reported unfinished because year-round suitability or comparable finish is not satisfied.");
            }
            if (f.access == AccessStatus::unknown) issue("access_unknown", "Declare finished area access.");
            if (!a.ceiling.kind) issue("ceiling_kind_missing", "Declare flat, sloped or stairs ceiling evidence.");
            else {
                (void)ceiling_kind_name(*a.ceiling.kind);
                if (*a.ceiling.kind == CeilingKind::flat) {
                    if (!a.ceiling.minimum_height_m || !std::isfinite(*a.ceiling.minimum_height_m) || *a.ceiling.minimum_height_m < 0)
                        issue("ceiling_height_missing", "Flat ceilings require a finite nonnegative minimum height in metres.");
                    else if (a.measurement.acquisition_increment) {
                        try {
                            const double rounded_height = rounded_ansi_ceiling_height_metres(
                                *a.ceiling.minimum_height_m, *a.measurement.acquisition_increment);
                            nonstandard = rounded_height < 2.1336;
                            r.rule_notes.push_back(*a.measurement.acquisition_increment == AcquisitionIncrement::inch ?
                                "Flat ceiling height compared with seven feet after nearest-inch acquisition rounding; original observed height retained." :
                                "Flat ceiling height compared with seven feet after nearest-tenth-foot acquisition rounding; original observed height retained.");
                            if (nonstandard) r.rule_notes.push_back("Nonstandard finished: acquisition-rounded minimum flat ceiling height is below seven feet.");
                        } catch (const std::invalid_argument&) {
                            issue("ceiling_height_invalid", "Flat ceiling height cannot be represented at the declared acquisition precision.");
                        }
                    }
                } else if (*a.ceiling.kind == CeilingKind::sloped) {
                    const auto high = a.ceiling.at_least_7ft_area_m2, room = a.ceiling.room_floor_area_m2;
                    if (!high || !room || !std::isfinite(*high) || !std::isfinite(*room) || *high < 0 || *room <= 0 || *high > *room)
                        issue("sloped_ceiling_evidence_invalid", "Sloped ceilings require valid room floor and at-least-seven-foot areas.");
                    else {
                        nonstandard = *high < *room * 0.5;
                        if (nonstandard) r.rule_notes.push_back("Nonstandard finished: less than half the provisional whole-room denominator reaches seven feet.");
                    }
                    r.rule_notes.push_back("Sloped ceiling denominator uses complete room geometry before below-five-foot exclusions. This interpretation is provisional because final publisher ANSI text has not been verified.");
                }
            }
            nonstandard = nonstandard || f.access == AccessStatus::through_unfinished;
            if (f.access == AccessStatus::through_unfinished) r.rule_notes.push_back("Nonstandard finished: access passes through unfinished space.");
        }
        const auto identity = a.dwelling_identity.value_or(DwellingIdentity::primary);
        const bool separate_adu = identity == DwellingIdentity::detached_adu || identity == DwellingIdentity::attached_adu;
        const bool detached = identity == DwellingIdentity::detached_other;
        if (separate_adu || detached) {
            if (detached) category = !finished ? (below ? AppraisalAreaCategory::detached_other_below_grade_unfinished : AppraisalAreaCategory::detached_other_above_grade_unfinished) :
                nonstandard ? (below ? AppraisalAreaCategory::detached_other_below_grade_nonstandard_finished : AppraisalAreaCategory::detached_other_above_grade_nonstandard_finished) :
                (below ? AppraisalAreaCategory::detached_other_below_grade_finished : AppraisalAreaCategory::detached_other_above_grade_finished);
            else category = !finished ? (below ? AppraisalAreaCategory::adu_below_grade_unfinished : AppraisalAreaCategory::adu_above_grade_unfinished) :
                nonstandard ? (below ? AppraisalAreaCategory::adu_below_grade_nonstandard_finished : AppraisalAreaCategory::adu_above_grade_nonstandard_finished) :
                (below ? AppraisalAreaCategory::adu_below_grade_finished : AppraisalAreaCategory::adu_above_grade_finished);
        } else if (!finished) category = below ? AppraisalAreaCategory::below_grade_unfinished : AppraisalAreaCategory::above_grade_unfinished;
        else if (f.access == AccessStatus::noncontinuous) {
            if (below) issue("noncontinuous_below_grade", "Primary noncontinuous finished area must be above grade.");
            category = AppraisalAreaCategory::noncontinuous_finished;
        } else if (nonstandard) category = below ? AppraisalAreaCategory::below_grade_nonstandard_finished : AppraisalAreaCategory::above_grade_nonstandard_finished;
        else category = below ? AppraisalAreaCategory::below_grade_finished : AppraisalAreaCategory::above_grade_finished;
    } else if (measured) {
        switch (f.use) {
        case AreaUse::garage: category = AppraisalAreaCategory::garage; break;
        case AreaUse::carport: category = AppraisalAreaCategory::carport; break;
        case AreaUse::porch: category = AppraisalAreaCategory::porch; break;
        case AreaUse::patio: category = AppraisalAreaCategory::patio; break;
        case AreaUse::deck: category = AppraisalAreaCategory::deck; break;
        case AreaUse::other_non_living: category = AppraisalAreaCategory::other_non_living; break;
        default: break;
        }
    }
    r.qualified = r.issues.empty();
    if (r.qualified) r.derived_category = category;
    return r;
}
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
    if (policy.kind == AppraisalPolicyKind::ansi_z765_2021) return derive_ansi(f, factor);
    auto issue = [&](std::string code, std::string message) {
        result.issues.push_back({std::move(code), std::move(message)});
    };
    if (factor.numerator != factor.denominator)
        issue("factor_not_unity", "Qualified physical area requires a factor exactly equal to one.");
    if (f.access == AccessStatus::through_unfinished)
        issue("access_incompatible", "Through-unfinished access requires the ANSI policy.");
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
    if (policy.kind == AppraisalPolicyKind::ansi_z765_2021 && facts.ansi) {
        const auto& ceiling = facts.ansi->ceiling;
        auto fail = [&](std::string code, std::string message) {
            result.issues.push_back({std::move(code), std::move(message)});
        };
        if (ceiling.kind == CeilingKind::stairs && ceiling.stair_from_floor_id != area.floor_id)
            fail("stair_source_floor_mismatch", "Stairs must be included on the floor from which they descend.");
        if (ceiling.kind == CeilingKind::sloped) {
            if (ceiling.room_boundary_id != area.id)
                fail("room_anchor_mismatch", "Sloped ceiling evidence must identify this complete room boundary.");
            if (ceiling.source_geometry_sha256.empty())
                fail("ceiling_geometry_binding_missing", "Bind sloped ceiling observations to current room and deduction geometry.");
            if (ceiling.room_floor_area_m2 && std::abs(*ceiling.room_floor_area_m2 - calculation.base_square_metres) > tolerance(calculation.base_square_metres))
                fail("room_geometry_mismatch", "Sloped ceiling room area must match this complete room boundary's current physical geometry.");
            std::set<std::string> unique;
            auto low_geometry = measured;
            low_geometry.deductions.clear();
            for (const auto& id : ceiling.below_5ft_deduction_ids) {
                if (id.empty() || !unique.insert(id).second) { fail("low_ceiling_deduction_invalid", "Below-five-foot IDs must be unique and nonempty."); continue; }
                const auto found = std::find_if(calculation.deductions.begin(), calculation.deductions.end(), [&](const auto& d) { return d.id == id; });
                if (found == calculation.deductions.end()) fail("low_ceiling_deduction_missing", "Below-five-foot geometry must be linked as a real contained deduction: " + id);
                else {
                    const auto source = std::find_if(area.deductions.begin(), area.deductions.end(), [&](const auto& d) { return d.id == id; });
                    low_geometry.deductions.push_back(*source);
                }
            }
            const double low = calculate_area(low_geometry, physical).deducted_square_metres;
            if (ceiling.at_least_7ft_area_m2 && *ceiling.at_least_7ft_area_m2 > calculation.base_square_metres - low + tolerance(calculation.base_square_metres))
                fail("high_ceiling_area_exceeds_support", "At-least-seven-foot area exceeds room geometry after below-five-foot exclusions.");
            if (ceiling.at_least_7ft_area_m2 && *ceiling.at_least_7ft_area_m2 > calculation.net_square_metres + tolerance(calculation.base_square_metres))
                fail("high_ceiling_area_exceeds_candidate", "At-least-seven-foot area exceeds this room's current net candidate geometry.");
        }
        result.qualified = result.issues.empty();
        if (!result.qualified) result.derived_category.reset();
    }
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
    for (const auto& [category, name] : detail::appraisal_category_tokens) {
        if (category != AppraisalAreaCategory::none && category <= AppraisalAreaCategory::commercial_service)
            profile.classifications.emplace(std::string(name), ClassificationRule{
                true, category == AppraisalAreaCategory::above_grade_finished, category});
    }
    return profile;
}

CalculationProfile ansi_appraisal_profile() {
    CalculationProfile profile{"vertex-ansi-z765-2021-v1", 1, AreaUnit::square_foot, 0, {}};
    for (const auto& [category, name] : detail::appraisal_category_tokens)
        if (category != AppraisalAreaCategory::none && category != AppraisalAreaCategory::commercial_occupiable &&
            category != AppraisalAreaCategory::commercial_common && category != AppraisalAreaCategory::commercial_service)
            profile.classifications.emplace(std::string(name), ClassificationRule{true, category == AppraisalAreaCategory::above_grade_finished, category});
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
            for (const auto& [category, name] : detail::appraisal_category_tokens) {
                if (category == AppraisalAreaCategory::none)
                    continue;
                if (profile.id != "vertex-ansi-z765-2021-v1" && category > AppraisalAreaCategory::commercial_service) continue;
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
