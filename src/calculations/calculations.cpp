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
    std::pair{AppraisalAreaCategory::other_non_living, std::string_view{"other_non_living"}}};

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
