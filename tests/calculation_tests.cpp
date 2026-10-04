#include "sketch/calculations.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <numbers>
#include <stdexcept>

namespace {
void check(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
void near(double actual, double expected, double tolerance, const char* message) {
    check(std::isfinite(actual) && std::abs(actual - expected) <= tolerance, message);
}
template <class Function> void rejected(Function&& function) {
    try {
        function();
    } catch (const std::invalid_argument&) {
        return;
    }
    throw std::runtime_error("Invalid calculation was accepted");
}
sketch::Boundary rectangle(double x, double y, double w, double h) {
    return {{{x, y}, {x + w, y}, 0},
            {{x + w, y}, {x + w, y + h}, 0},
            {{x + w, y + h}, {x, y + h}, 0},
            {{x, y + h}, {x, y}, 0}};
}
sketch::MeasurementArea room(std::string id, sketch::Boundary boundary) {
    return {std::move(id), "building-1", "floor-1", "living", std::move(boundary), {}, {1, 1}};
}
void appraisal_tests() {
    using namespace sketch;
    using Category = AppraisalAreaCategory;
    auto profile = builtin_appraisal_profile();
    check(profile.version == 1 && profile.display_unit == AreaUnit::square_foot,
          "Built-in appraisal policy has a version and square-foot display");
    const std::vector<std::pair<Category, std::string>> categories{
        {Category::above_grade_finished, "above_grade_finished"},
        {Category::above_grade_unfinished, "above_grade_unfinished"},
        {Category::below_grade_finished, "below_grade_finished"},
        {Category::below_grade_unfinished, "below_grade_unfinished"},
        {Category::garage, "garage"}, {Category::carport, "carport"},
        {Category::porch, "porch"}, {Category::patio, "patio"},
        {Category::deck, "deck"}, {Category::other_non_living, "other_non_living"}};
    std::vector<MeasurementArea> areas;
    for (const auto& [category, name] : categories) {
        check(appraisal_category_name(category) == name && parse_appraisal_category(name) == category,
              "Category persistence names round trip");
        auto area = room(name, rectangle(0, 0, 10, 10));
        area.classification = name;
        area.floor_id = name;
        areas.push_back(area);
    }
    check(parse_appraisal_category("none") == Category::none &&
              appraisal_category_name(Category::none) == "none" &&
              !parse_appraisal_category("Finished basement") && !parse_appraisal_category("GARAGE"),
          "Category parsing is explicit and rejects unknown values");
    rejected([] { (void)appraisal_category_name(static_cast<Category>(999)); });
    auto second = areas.front();
    second.id = "second";
    second.floor_id = "second-floor";
    second.deductions = {{"hole", rectangle(1, 1, 2, 5)}};
    second.factor = {1, 2};
    areas.push_back(second);
    auto other_building = second;
    other_building.id = "other-building";
    other_building.building_id = "building-2";
    areas.push_back(other_building);
    auto site = areas.front();
    site.id = "site";
    site.scope = AreaScope::site;
    areas.push_back(site);
    const auto report = calculate_appraisal_areas(areas, profile);
    near(report.property.gla().total.square_metres, 190, 1e-7,
         "GLA sums finished above-grade floors after deductions and factors only");
    near(report.by_building.at("building-1").gla().total.square_metres, 145, 1e-7,
         "Building appraisal totals stay separate");
    near(report.by_floor.at({"building-1", "second-floor"}).gla().total.square_metres, 45, 1e-7,
         "Per-floor subtotal applies deduction before factor");
    near(report.by_floor.at({"building-2", "second-floor"}).gla().total.square_metres, 45, 1e-7,
         "Floor identity includes building");
    for (const auto& [category, name] : categories) {
        const auto& bucket = report.property.by_category.at(category);
        near(bucket.total.square_metres, category == Category::above_grade_finished ? 190 : 100,
             1e-7, "All appraisal categories remain separate");
        if (category != Category::above_grade_finished)
            check(bucket.area_ids == std::vector<std::string>{name}, "Bucket provenance is exact");
    }
    check(report.property.gla().area_ids ==
              std::vector<std::string>{"above_grade_finished", "other-building", "second"},
          "GLA provenance is sorted and excludes site and nonliving areas");
    check(report.calculation.areas.size() == areas.size(), "Base report retains site calculations");
    auto enclosed_finished = room("enclosed-finished", rectangle(0, 0, 10, 10));
    enclosed_finished.classification = "above_grade_finished";
    enclosed_finished.deductions = {{"internal-garage", rectangle(1, 1, 2, 5)}};
    auto internal_garage = room("internal-garage", rectangle(1, 1, 2, 5));
    internal_garage.classification = "garage";
    const auto enclosed_report =
        calculate_appraisal_areas({enclosed_finished, internal_garage}, profile);
    near(enclosed_report.property.gla().total.square_metres, 90, 1e-7,
         "A categorized internal garage subtracts from enclosing GLA");
    near(enclosed_report.property.by_category.at(Category::garage).total.square_metres,
         10, 1e-7, "A categorized internal garage contributes once to its own bucket");
    check(enclosed_report.property.by_category.at(Category::garage).area_ids ==
              std::vector<std::string>{"internal-garage"},
          "Internal ancillary category retains exact contribution provenance");
    areas.front().classification = "below_grade_finished";
    near(calculate_appraisal_areas(areas, profile).property.gla().total.square_metres, 90, 1e-7,
         "Classification changes immediately recalculate GLA");
    profile.classifications["living"] = {true, true};
    auto legacy = room("legacy", rectangle(0, 0, 2, 2));
    const auto legacy_report = calculate_appraisal_areas({legacy}, profile);
    near(legacy_report.property.gla().total.square_metres, 0, 0,
         "Legacy living flag does not imply an appraisal category");
    check(profile.classifications.at("living").appraisal_category == Category::none,
          "Two-boolean legacy rule defaults to none");
    near(legacy_report.calculation.living.square_metres, 4, 1e-9,
         "Appraisal extension preserves legacy totals");
    profile.classifications["living"].appraisal_category = Category::above_grade_finished;
    near(calculate_appraisal_areas({legacy}, profile).property.gla().total.square_metres, 4, 1e-9,
         "Explicit rule category works independently of classification spelling");
    legacy.boundary = rectangle(0, 0, 0.0004, 1);
    auto tiny = legacy;
    tiny.id = "tiny";
    tiny.floor_id = "floor-2";
    check(calculate_appraisal_areas({legacy, tiny}, profile).property.gla().total.display.text == "0.01",
          "Appraisal aggregate rounds once after summing unrounded areas");
    profile.classifications["living"].appraisal_category = static_cast<Category>(999);
    rejected([&] { (void)calculate_appraisal_areas({legacy}, profile); });
    const auto empty = calculate_appraisal_areas({}, builtin_appraisal_profile());
    check(empty.property.by_category.size() == 16 && empty.property.gla().total.display.text == "0.00",
          "Empty reports expose all zero-valued categories");
}

void ansi_policy_tests() {
    using namespace sketch;
    const AppraisalPolicy policy{AppraisalPolicyKind::ansi_z765_2021, 1};
    AppraisalFacts facts{PropertyKind::detached_single_family, MeasurementBasis::exterior,
        GradeStatus::above, FinishStatus::finished, AccessStatus::direct_interior,
        CeilingEligibility::unknown, AreaUse::dwelling, BoundaryRole::measured_area};
    AnsiAppraisalFacts ansi;
    ansi.measurement = {true, true, AcquisitionIncrement::inch, ""};
    ansi.any_part_below_grade = false;
    ansi.year_round_suitable = true;
    ansi.finish_matches_dwelling = true;
    ansi.dwelling_identity = DwellingIdentity::primary;
    ansi.ceiling.kind = CeilingKind::flat;
    ansi.ceiling.minimum_height_m = 2.1336;
    facts.ansi = ansi;
    auto area = room("room", rectangle(0, 0, 10, 10));
    const auto qualified = [&](const AppraisalFacts& f) { return qualify_appraisal_area(area, f, policy); };
    check(qualified(facts).qualified && qualified(facts).derived_category == AppraisalAreaCategory::above_grade_finished,
        "Exactly seven feet qualifies without legacy ceiling approval");
    facts.ansi->ceiling.minimum_height_m = std::nextafter(2.1336, 0.0);
    auto result = qualified(facts);
    // Acquisition precision precedes the threshold: a one-ULP shortfall still
    // records as seven feet to the declared nearest inch.
    check(result.qualified && result.derived_category == AppraisalAreaCategory::above_grade_finished && !result.rule_notes.empty(),
        "A height immediately below seven feet qualifies after recorded inch rounding");
    facts.ansi->ceiling.minimum_height_m = 2.1336;
    facts.access = AccessStatus::through_unfinished;
    check(qualified(facts).derived_category == AppraisalAreaCategory::above_grade_nonstandard_finished,
        "Through unfinished access is nonstandard");
    check(!derive_appraisal_category(facts).qualified, "ANSI access token cannot qualify under legacy rules");
    facts.access = AccessStatus::direct_interior;
    facts.ansi->any_part_below_grade = true;
    check(!qualified(facts).qualified, "Contradictory grade requires correction");
    facts.grade = GradeStatus::unknown;
    check(qualified(facts).derived_category == AppraisalAreaCategory::below_grade_finished,
        "Any below-grade portion derives the whole floor below");
    facts.ansi->any_part_below_grade = false;
    for (const auto identity : {DwellingIdentity::attached_adu, DwellingIdentity::detached_adu}) {
        facts.ansi->dwelling_identity = identity;
        check(qualified(facts).derived_category == AppraisalAreaCategory::adu_above_grade_finished,
            "Every ADU is separately measured regardless of interior access");
        facts.finish = FinishStatus::unfinished;
        check(qualified(facts).derived_category == AppraisalAreaCategory::adu_above_grade_unfinished,
            "Unfinished ADU remains separate");
        facts.finish = FinishStatus::finished;
    }
    facts.ansi->dwelling_identity = DwellingIdentity::detached_other;
    check(qualified(facts).derived_category == AppraisalAreaCategory::detached_other_above_grade_finished,
        "Detached other structure is excluded from primary GLA");
    facts.ansi->dwelling_identity = DwellingIdentity::primary;
    facts.ansi->year_round_suitable = false;
    check(qualified(facts).derived_category == AppraisalAreaCategory::above_grade_unfinished,
        "Year-round unsuitability cannot inflate finished area");
    facts.ansi->year_round_suitable = true;
    facts.measurement_basis = MeasurementBasis::plans;
    check(!qualified(facts).qualified, "Plans require a limitations statement");
    facts.ansi->measurement.limitations_statement = "Measurements supplied from building plans; interior not inspected.";
    check(qualified(facts).qualified, "Plans can carry limitations without guessed measurements");
    facts.measurement_basis = MeasurementBasis::exterior;
    for (const auto kind : {PropertyKind::apartment_unit, PropertyKind::multifamily, PropertyKind::light_commercial}) {
        facts.property_kind = kind;
        check(!qualified(facts).qualified, "ANSI unsupported property designs are rejected");
    }
    facts.property_kind = PropertyKind::manufactured_home;
    check(qualified(facts).qualified, "Manufactured single-family designs are supported");
    facts.ansi->ceiling = {};
    facts.ansi->ceiling.kind = CeilingKind::stairs;
    facts.ansi->ceiling.stair_from_floor_id = area.floor_id;
    check(!qualified(facts).qualified, "A room cannot select a stair height waiver");
    facts.role = BoundaryRole::stair_footprint;
    check(qualified(facts).derived_category == AppraisalAreaCategory::above_grade_finished,
        "Stair footprint contributes on descending source floor");
    facts.ansi->ceiling.stair_from_floor_id = "other-floor";
    check(!qualified(facts).qualified, "Stair source floor must match contribution floor");
    facts.role = BoundaryRole::measured_area;
    facts.ansi->ceiling = {};
    facts.ansi->ceiling.kind = CeilingKind::sloped;
    facts.ansi->ceiling.room_boundary_id = area.id;
    facts.ansi->ceiling.source_geometry_sha256 = "document-layer-verifies-binding";
    facts.ansi->ceiling.room_floor_area_m2 = 100;
    facts.ansi->ceiling.at_least_7ft_area_m2 = 49.9;
    check(qualified(facts).derived_category == AppraisalAreaCategory::above_grade_nonstandard_finished,
        "49.9 percent sloped room is nonstandard under provisional whole-room denominator");
    facts.ansi->ceiling.at_least_7ft_area_m2 = 50;
    check(qualified(facts).derived_category == AppraisalAreaCategory::above_grade_finished && !qualified(facts).rule_notes.empty(),
        "50 percent sloped room reaches rule threshold and retains interpretation caveat");
    facts.ansi->ceiling.below_5ft_deduction_ids = {"low"};
    check(!qualified(facts).qualified, "Scalar ceiling evidence cannot substitute for under-five-foot geometry");
    area.deductions = {{"low", rectangle(0, 0, 4, 10)}};
    near(*qualified(facts).physical_square_metres, 60, 1e-7, "Under-five-foot geometry actually subtracts from physical area");
    facts.ansi->ceiling.at_least_7ft_area_m2 = 61;
    check(!qualified(facts).qualified, "Seven-foot evidence cannot exceed actual eligible surface");
    area.deductions.push_back({"aaa-other", rectangle(0, 0, 4, 10)});
    check(!qualified(facts).qualified, "Low-mask overlap with an earlier deduction cannot enlarge support");
    area.deductions.back().id = "zzz-other";
    check(!qualified(facts).qualified, "Renaming an overlapping deduction cannot change ceiling evidence validity");
    facts.ansi->ceiling.at_least_7ft_area_m2 = 50;
    facts.ansi->ceiling.room_floor_area_m2 = 99;
    check(!qualified(facts).qualified, "Room evidence must match current complete-room geometry");
    const auto profile = ansi_appraisal_profile();
    check(profile.decimal_places == 0 && profile.display_unit == AreaUnit::square_foot &&
        builtin_appraisal_profile().decimal_places == 2 &&
        !builtin_appraisal_profile().classifications.contains("adu_above_grade_finished"),
        "Opt-in canonical reporting leaves the builtin legacy profile unchanged");
    auto first = room("first", rectangle(0, 0, 1, 0.151 * 0.09290304));
    first.classification = "above_grade_finished";
    auto second = first;
    second.id = "second";
    second.boundary = rectangle(2, 0, 1, 0.451 * 0.09290304);
    const auto totals = calculate_appraisal_areas({first, second}, profile);
    check(totals.property.gla().total.display.text == "1", "ANSI totals round once after summing unrounded physical areas");
}

void ansi_v2_sloped_tests() {
    using namespace sketch;
    const AppraisalPolicy v2{AppraisalPolicyKind::ansi_z765_2021, 2};
    AppraisalFacts facts{PropertyKind::detached_single_family, MeasurementBasis::exterior,
        GradeStatus::above, FinishStatus::finished, AccessStatus::direct_interior,
        CeilingEligibility::unknown, AreaUse::dwelling, BoundaryRole::measured_area};
    AnsiAppraisalFacts ansi;
    ansi.measurement = {true, true, AcquisitionIncrement::inch, ""};
    ansi.any_part_below_grade = false;
    ansi.year_round_suitable = true;
    ansi.finish_matches_dwelling = true;
    ansi.dwelling_identity = DwellingIdentity::primary;
    ansi.ceiling.kind = CeilingKind::sloped;
    ansi.ceiling.room_floor_area_m2 = 100;
    ansi.ceiling.at_least_7ft_area_m2 = 35;
    ansi.ceiling.room_boundary_id = "room";
    ansi.ceiling.source_geometry_sha256 = "document-layer-verifies-binding";
    ansi.ceiling.complete_room_observed = true;
    ansi.ceiling.below_5ft_deduction_ids = {"low"};
    facts.ansi = ansi;
    auto area = room("room", rectangle(0, 0, 10, 10));
    area.deductions = {{"low", rectangle(0, 0, 4, 10)}};
    const auto evaluate = [&] { return qualify_appraisal_area(area, facts, v2); };
    auto result = evaluate();
    check(result.qualified && result.derived_category == AppraisalAreaCategory::above_grade_finished &&
          result.policy_version == 2 && result.policy_id == "vertex-ansi-z765-2021-v2",
          "V2 high35 over net60 qualifies while retaining explicit v2 identity");
    near(*result.physical_square_metres, 60, 1e-7, "V2 excludes low40 from physical contribution");
    check(qualify_appraisal_area(area, facts, {AppraisalPolicyKind::ansi_z765_2021, 1}).derived_category ==
          AppraisalAreaCategory::above_grade_nonstandard_finished, "V1 keeps gross100 denominator for high35");
    result = derive_appraisal_category(facts, v2);
    check(!result.qualified && !result.derived_category, "V2 scalar-only sloped evidence cannot derive a category");
    facts.ansi->ceiling.at_least_7ft_area_m2 = 29.9;
    check(evaluate().derived_category == AppraisalAreaCategory::above_grade_nonstandard_finished,
          "V2 below half of net60 is nonstandard");
    facts.ansi->ceiling.at_least_7ft_area_m2 = 30;
    check(evaluate().derived_category == AppraisalAreaCategory::above_grade_finished, "V2 exact half net60 qualifies");
    facts.ansi->ceiling.at_least_7ft_area_m2 = 61;
    check(!evaluate().qualified, "V2 high observation cannot exceed countable net geometry");
    facts.ansi->ceiling.at_least_7ft_area_m2 = 30;
    for (const auto observation : {std::optional<bool>{}, std::optional<bool>{false}}) {
        facts.ansi->ceiling.complete_room_observed = observation;
        result = evaluate();
        check(!result.qualified && !result.derived_category, "V2 requires affirmative complete-room observation");
    }
    facts.ansi->ceiling.complete_room_observed = true;
    area.deductions.push_back({"opening", rectangle(4, 0, 2, 10)});
    facts.ansi->ceiling.at_least_7ft_area_m2 = 25;
    result = evaluate();
    check(result.qualified && result.derived_category == AppraisalAreaCategory::above_grade_finished,
          "V2 high25 over complete room minus low40 and opening20 qualifies");
    near(*result.physical_square_metres, 40, 1e-7, "V2 actual other-void opening reduces countable denominator");
    area.deductions[1].boundary = rectangle(0, 0, 2, 10);
    result = evaluate();
    check(result.qualified && result.derived_category == AppraisalAreaCategory::above_grade_nonstandard_finished,
          "V2 overlapping exclusions use union geometry instead of double subtraction");
    near(*result.physical_square_metres, 60, 1e-7, "V2 overlap leaves actual net60 candidate");
    std::reverse(area.deductions.begin(), area.deductions.end());
    area.deductions[0].id = "aaa-opening";
    area.deductions[1].id = "zzz-low";
    facts.ansi->ceiling.below_5ft_deduction_ids = {"zzz-low"};
    const auto reordered = evaluate();
    check(reordered.qualified && reordered.derived_category == result.derived_category,
          "V2 renaming and reordering overlapping exclusions cannot alter half-test category");
    near(*reordered.physical_square_metres, *result.physical_square_metres, 1e-7,
         "V2 renaming and reordering exclusions preserves physical candidate");
    area.deductions.clear();
    facts.ansi->ceiling.below_5ft_deduction_ids.clear();
    facts.ansi->ceiling.at_least_7ft_area_m2 = 49.9;
    check(evaluate().derived_category == AppraisalAreaCategory::above_grade_nonstandard_finished,
          "V2 without low geometry uses whole countable100 denominator");
    facts.ansi->ceiling.at_least_7ft_area_m2 = 50;
    check(evaluate().derived_category == AppraisalAreaCategory::above_grade_finished, "V2 no-low exact half qualifies");
    area.deductions = {{"low", rectangle(0, 0, 10, 10)}};
    facts.ansi->ceiling.below_5ft_deduction_ids = {"low"};
    facts.ansi->ceiling.at_least_7ft_area_m2 = 0;
    check(!evaluate().qualified && !evaluate().derived_category, "V2 zero countable candidate cannot qualify");
    for (unsigned version : {0U, 3U}) {
        rejected([&] { (void)appraisal_policy_id({AppraisalPolicyKind::ansi_z765_2021, version}); });
        rejected([&] { (void)ansi_appraisal_profile(version); });
    }
    for (auto kind : {AppraisalPolicyKind::residential_declared, AppraisalPolicyKind::light_commercial_declared})
        rejected([&] { (void)appraisal_policy_id({kind, 2}); });
    for (unsigned version : {1U, 2U}) {
        const auto profile = ansi_appraisal_profile(version);
        check(profile.version == version && profile.id == (version == 1 ? "vertex-ansi-z765-2021-v1" : "vertex-ansi-z765-2021-v2") &&
              profile.classifications.contains("adu_above_grade_finished"), "Both ANSI profile versions retain ADU buckets");
        auto adu = room("adu", rectangle(0, 0, 10, 10));
        adu.classification = "adu_above_grade_finished";
        const auto totals = calculate_appraisal_areas({adu}, profile);
        near(totals.property.by_category.at(AppraisalAreaCategory::adu_above_grade_finished).total.square_metres,
             100, 1e-7, "Both ANSI profile aggregates retain ADU contributions");
        near(totals.property.gla().total.square_metres, 0, 1e-7, "ADU remains separate from primary GLA");
    }
}

void ansi_ceiling_rounding_helper_tests() {
    using namespace sketch;
    struct RoundedCase { double observed_metres; AcquisitionIncrement increment; double rounded_metres; };
    // Expected acquisition heights are literal conversions of independently
    // counted inches or tenths of a foot, not another rounding implementation.
    const RoundedCase cases[]{
        {6.96 * 0.3048, AcquisitionIncrement::tenth_foot, 2.1336},
        {6.85 * 0.3048, AcquisitionIncrement::tenth_foot, 2.10312},
        {6.96 * 0.3048, AcquisitionIncrement::inch, 2.1336},
        {6.85 * 0.3048, AcquisitionIncrement::inch, 2.0828},
        {6.951 * 0.3048, AcquisitionIncrement::tenth_foot, 2.1336},
        {6.951 * 0.3048, AcquisitionIncrement::inch, 2.1082},
        {0.01524, AcquisitionIncrement::tenth_foot, 0.03048},
        {0.0127, AcquisitionIncrement::inch, 0.0254},
        {0.0, AcquisitionIncrement::tenth_foot, 0.0},
        {-0.0, AcquisitionIncrement::inch, 0.0},
        {std::numeric_limits<double>::denorm_min(), AcquisitionIncrement::inch, 0.0}};
    for (const auto& item : cases)
        near(rounded_ansi_ceiling_height_metres(item.observed_metres, item.increment),
            item.rounded_metres, 1e-12, "Shared ceiling rounding must report the declared acquisition increment");
    struct Cutoff { double metres; AcquisitionIncrement increment; double lower_rounded_metres; };
    for(const auto& cutoff : {Cutoff{2.11836,AcquisitionIncrement::tenth_foot,2.10312},
                             Cutoff{2.1209,AcquisitionIncrement::inch,2.1082}}) {
        near(rounded_ansi_ceiling_height_metres(std::nextafter(cutoff.metres,0.0),cutoff.increment),
            cutoff.lower_rounded_metres,1e-12,"Represented value below the seven-foot acquisition cutoff stays below it");
        near(rounded_ansi_ceiling_height_metres(cutoff.metres,cutoff.increment),2.1336,1e-12,
            "Seven-foot acquisition cutoff rounds upward");
        near(rounded_ansi_ceiling_height_metres(std::nextafter(cutoff.metres,std::numeric_limits<double>::infinity()),cutoff.increment),
            2.1336,1e-12,"Represented value above the seven-foot acquisition cutoff remains qualifying");
    }
    for (const auto increment : {AcquisitionIncrement::inch, AcquisitionIncrement::tenth_foot}) {
        for (const double invalid : {-1.0, std::numeric_limits<double>::quiet_NaN(),
                 std::numeric_limits<double>::infinity(), std::numeric_limits<double>::max(), 1e15})
            rejected([&] { (void)rounded_ansi_ceiling_height_metres(invalid, increment); });
    }
    rejected([] { (void)rounded_ansi_ceiling_height_metres(2.1336, static_cast<AcquisitionIncrement>(999)); });
}

void ansi_flat_ceiling_acquisition_tests() {
    using namespace sketch;
    const AppraisalPolicy policy{AppraisalPolicyKind::ansi_z765_2021, 1};
    AppraisalFacts facts{PropertyKind::detached_single_family, MeasurementBasis::exterior,
        GradeStatus::above, FinishStatus::finished, AccessStatus::direct_interior,
        CeilingEligibility::unknown, AreaUse::dwelling, BoundaryRole::measured_area};
    AnsiAppraisalFacts ansi;
    ansi.measurement = {true, true, AcquisitionIncrement::inch, ""};
    ansi.any_part_below_grade = false;
    ansi.year_round_suitable = true;
    ansi.finish_matches_dwelling = true;
    ansi.dwelling_identity = DwellingIdentity::primary;
    ansi.ceiling.kind = CeilingKind::flat;
    facts.ansi = ansi;
    const auto area = room("flat-room", rectangle(0, 0, 10, 10));
    struct HeightCase { double feet; AcquisitionIncrement increment; bool standard; };
    // Fannie Mae's September 2023 ANSI Answers example records 6.96 feet as
    // seven feet and 6.85 feet as 6.9 feet with tenth-foot acquisition precision.
    const HeightCase cases[]{
        {6.96, AcquisitionIncrement::tenth_foot, true},
        {6.85, AcquisitionIncrement::tenth_foot, false},
        {6.96, AcquisitionIncrement::inch, true},
        {6.85, AcquisitionIncrement::inch, false},
        {6.951, AcquisitionIncrement::tenth_foot, true},
        {6.951, AcquisitionIncrement::inch, false},
        {6.94, AcquisitionIncrement::tenth_foot, false},
        {6.94, AcquisitionIncrement::inch, false},
        {7.0, AcquisitionIncrement::tenth_foot, true},
        {7.0, AcquisitionIncrement::inch, true},
        {7.01, AcquisitionIncrement::tenth_foot, true},
        {7.01, AcquisitionIncrement::inch, true},
        {0.0, AcquisitionIncrement::tenth_foot, false},
        {0.0, AcquisitionIncrement::inch, false}};
    for (const auto& item : cases) {
        const double observed = item.feet * 0.3048;
        facts.ansi->measurement.acquisition_increment = item.increment;
        facts.ansi->ceiling.minimum_height_m = observed;
        const auto result = qualify_appraisal_area(area, facts, policy);
        check(result.qualified && result.derived_category == (item.standard ?
            AppraisalAreaCategory::above_grade_finished : AppraisalAreaCategory::above_grade_nonstandard_finished),
            "Flat ceiling eligibility must use declared acquisition rounding before the seven-foot threshold");
        near(*result.physical_square_metres, 100, 1e-7, "Ceiling rounding must preserve physical area");
        check(facts.ansi->ceiling.minimum_height_m == observed,
            "Qualification must retain the original observed ceiling height");
    }
    for (const auto increment : {AcquisitionIncrement::inch, AcquisitionIncrement::tenth_foot}) {
        facts.ansi->measurement.acquisition_increment = increment;
        for (const double observed : {std::nextafter(2.1336, 0.0), 2.1336,
                 std::nextafter(2.1336, std::numeric_limits<double>::infinity())}) {
            facts.ansi->ceiling.minimum_height_m = observed;
            check(qualify_appraisal_area(area, facts, policy).derived_category == AppraisalAreaCategory::above_grade_finished,
                "Immediately below, exactly at and immediately above seven feet share a recorded seven-foot height");
        }
        for (const double invalid : {-1.0, std::numeric_limits<double>::quiet_NaN(),
                 std::numeric_limits<double>::infinity(), std::numeric_limits<double>::max()}) {
            facts.ansi->ceiling.minimum_height_m = invalid;
            const auto result = qualify_appraisal_area(area, facts, policy);
            check(!result.qualified && !result.derived_category && !result.issues.empty(),
                "Malformed or unrepresentable ceiling observations must withhold eligibility");
        }
    }
    facts.ansi->ceiling.minimum_height_m = 6.96 * 0.3048;
    facts.ansi->measurement.acquisition_increment.reset();
    const auto missing = qualify_appraisal_area(area, facts, policy);
    check(!missing.qualified && !missing.derived_category &&
        std::any_of(missing.issues.begin(), missing.issues.end(), [](const auto& issue) {
            return issue.code == "acquisition_increment_missing";
        }), "Missing acquisition precision must not silently default to a qualifying increment");
    facts.ansi->measurement.acquisition_increment = static_cast<AcquisitionIncrement>(999);
    rejected([&] { (void)qualify_appraisal_area(area, facts, policy); });
}
void declared_policy_tests() {
    using namespace sketch;
    using C = AppraisalAreaCategory;
    const AppraisalFacts standard{PropertyKind::detached_single_family, MeasurementBasis::exterior,
        GradeStatus::above, FinishStatus::finished, AccessStatus::direct_interior,
        CeilingEligibility::standard, AreaUse::dwelling, BoundaryRole::measured_area};
    for (auto value : {AppraisalPolicyKind::residential_declared, AppraisalPolicyKind::light_commercial_declared}) {
        check(parse_appraisal_policy_kind(appraisal_policy_kind_name(value)) == value, "Fact persistence tokens round trip");
    }
    check(!parse_appraisal_policy_kind("invalid"), "Unknown fact token rejected");
    rejected([] { (void)appraisal_policy_kind_name(static_cast<AppraisalPolicyKind>(999)); });
    for (auto value : {PropertyKind::detached_single_family, PropertyKind::attached_single_family, PropertyKind::manufactured_home, PropertyKind::apartment_unit, PropertyKind::multifamily, PropertyKind::light_commercial}) {
        check(parse_property_kind(property_kind_name(value)) == value, "Fact persistence tokens round trip");
    }
    check(!parse_property_kind("invalid"), "Unknown fact token rejected");
    rejected([] { (void)property_kind_name(static_cast<PropertyKind>(999)); });
    for (auto value : {MeasurementBasis::exterior, MeasurementBasis::interior_perimeter, MeasurementBasis::plans, MeasurementBasis::unknown}) {
        check(parse_measurement_basis(measurement_basis_name(value)) == value, "Fact persistence tokens round trip");
    }
    check(!parse_measurement_basis("invalid"), "Unknown fact token rejected");
    rejected([] { (void)measurement_basis_name(static_cast<MeasurementBasis>(999)); });
    for (auto value : {GradeStatus::above, GradeStatus::below, GradeStatus::unknown}) {
        check(parse_grade_status(grade_status_name(value)) == value, "Fact persistence tokens round trip");
    }
    check(!parse_grade_status("invalid"), "Unknown fact token rejected");
    rejected([] { (void)grade_status_name(static_cast<GradeStatus>(999)); });
    for (auto value : {FinishStatus::finished, FinishStatus::unfinished, FinishStatus::unknown}) {
        check(parse_finish_status(finish_status_name(value)) == value, "Fact persistence tokens round trip");
    }
    check(!parse_finish_status("invalid"), "Unknown fact token rejected");
    rejected([] { (void)finish_status_name(static_cast<FinishStatus>(999)); });
    for (auto value : {AccessStatus::direct_interior, AccessStatus::noncontinuous, AccessStatus::unknown}) {
        check(parse_access_status(access_status_name(value)) == value, "Fact persistence tokens round trip");
    }
    check(!parse_access_status("invalid"), "Unknown fact token rejected");
    rejected([] { (void)access_status_name(static_cast<AccessStatus>(999)); });
    for (auto value : {CeilingEligibility::standard, CeilingEligibility::nonstandard, CeilingEligibility::unknown}) {
        check(parse_ceiling_eligibility(ceiling_eligibility_name(value)) == value, "Fact persistence tokens round trip");
    }
    check(!parse_ceiling_eligibility("invalid"), "Unknown fact token rejected");
    rejected([] { (void)ceiling_eligibility_name(static_cast<CeilingEligibility>(999)); });
    for (auto value : {AreaUse::dwelling, AreaUse::garage, AreaUse::carport, AreaUse::porch, AreaUse::patio, AreaUse::deck, AreaUse::commercial_occupiable, AreaUse::commercial_common, AreaUse::commercial_service, AreaUse::other_non_living}) {
        check(parse_area_use(area_use_name(value)) == value, "Fact persistence tokens round trip");
    }
    check(!parse_area_use("invalid"), "Unknown fact token rejected");
    rejected([] { (void)area_use_name(static_cast<AreaUse>(999)); });
    for (auto value : {BoundaryRole::measured_area, BoundaryRole::open_to_below, BoundaryRole::stair_footprint, BoundaryRole::other_void}) {
        check(parse_boundary_role(boundary_role_name(value)) == value, "Fact persistence tokens round trip");
    }
    check(!parse_boundary_role("invalid"), "Unknown fact token rejected");
    rejected([] { (void)boundary_role_name(static_cast<BoundaryRole>(999)); });
    { auto invalid = standard; invalid.property_kind = static_cast<PropertyKind>(999);
      rejected([&] { (void)derive_appraisal_category(invalid); }); }
    { auto invalid = standard; invalid.measurement_basis = static_cast<MeasurementBasis>(999);
      rejected([&] { (void)derive_appraisal_category(invalid); }); }
    { auto invalid = standard; invalid.grade = static_cast<GradeStatus>(999);
      rejected([&] { (void)derive_appraisal_category(invalid); }); }
    { auto invalid = standard; invalid.finish = static_cast<FinishStatus>(999);
      rejected([&] { (void)derive_appraisal_category(invalid); }); }
    { auto invalid = standard; invalid.access = static_cast<AccessStatus>(999);
      rejected([&] { (void)derive_appraisal_category(invalid); }); }
    { auto invalid = standard; invalid.ceiling = static_cast<CeilingEligibility>(999);
      rejected([&] { (void)derive_appraisal_category(invalid); }); }
    { auto invalid = standard; invalid.use = static_cast<AreaUse>(999);
      rejected([&] { (void)derive_appraisal_category(invalid); }); }
    { auto invalid = standard; invalid.role = static_cast<BoundaryRole>(999);
      rejected([&] { (void)derive_appraisal_category(invalid); }); }
    rejected([&] { (void)derive_appraisal_category(standard, {static_cast<AppraisalPolicyKind>(999), 1}); });
    auto expect = [](const AppraisalFacts& facts, C category, AppraisalPolicy policy = {}) {
        const auto result = derive_appraisal_category(facts, policy);
        check(result.qualified && result.issues.empty() && result.derived_category == category,
              "Declared facts derive the expected category");
    };
    expect(standard, C::above_grade_finished);
    for (auto kind : {PropertyKind::attached_single_family, PropertyKind::manufactured_home}) {
        auto facts = standard; facts.property_kind = kind;
        expect(facts, C::above_grade_finished);
    }
    for (auto grade : {GradeStatus::above, GradeStatus::below}) {
        auto facts = standard; facts.grade = grade;
        expect(facts, grade == GradeStatus::above ? C::above_grade_finished : C::below_grade_finished);
        facts.ceiling = CeilingEligibility::nonstandard;
        expect(facts, grade == GradeStatus::above ? C::above_grade_nonstandard_finished : C::below_grade_nonstandard_finished);
        facts.finish = FinishStatus::unfinished;
        facts.access = AccessStatus::unknown; facts.ceiling = CeilingEligibility::unknown;
        expect(facts, grade == GradeStatus::above ? C::above_grade_unfinished : C::below_grade_unfinished);
    }
    auto facts = standard; facts.access = AccessStatus::noncontinuous;
    expect(facts, C::noncontinuous_finished);
    facts.ceiling = CeilingEligibility::nonstandard;
    expect(facts, C::noncontinuous_finished); // Noncontinuous takes precedence above grade.
    facts.grade = GradeStatus::below;
    check(!derive_appraisal_category(facts).qualified,
          "Below-grade noncontinuous finished space has no supported category");
    for (const auto& [use, category] : std::vector<std::pair<AreaUse, C>>{
             {AreaUse::garage, C::garage}, {AreaUse::carport, C::carport},
             {AreaUse::porch, C::porch}, {AreaUse::patio, C::patio}, {AreaUse::deck, C::deck},
             {AreaUse::other_non_living, C::other_non_living}}) {
        facts = standard; facts.use = use; facts.grade = GradeStatus::unknown;
        facts.finish = FinishStatus::unknown; facts.ceiling = CeilingEligibility::unknown;
        facts.access = AccessStatus::unknown;
        expect(facts, category);
    }
    facts = standard; facts.property_kind = PropertyKind::apartment_unit;
    check(!derive_appraisal_category(facts).qualified, "Apartment exterior basis is incompatible");
    facts.measurement_basis = MeasurementBasis::interior_perimeter;
    expect(facts, C::above_grade_finished);
    for (auto kind : {PropertyKind::multifamily, PropertyKind::light_commercial}) {
        facts = standard; facts.property_kind = kind;
        check(!derive_appraisal_category(facts).qualified, "Residential policy rejects unsupported property kinds");
    }
    facts = standard; facts.measurement_basis = MeasurementBasis::interior_perimeter;
    check(!derive_appraisal_category(facts).qualified, "Whole house needs exterior or plans basis");
    facts.measurement_basis = MeasurementBasis::plans;
    expect(facts, C::above_grade_finished);
    facts = standard; facts.measurement_basis = MeasurementBasis::unknown;
    facts.grade = GradeStatus::unknown; facts.finish = FinishStatus::unknown;
    const auto unknown = derive_appraisal_category(facts);
    check(!unknown.qualified && !unknown.derived_category && unknown.issues.size() == 3 &&
          unknown.issues[0].code == "measurement_basis_unknown" &&
          unknown.issues[1].code == "grade_unknown" && unknown.issues[2].code == "finish_unknown",
          "Unknown required facts produce deterministic structured issues");
    for (bool access : {false, true}) {
        facts = standard;
        if (access) facts.access = AccessStatus::unknown;
        else facts.ceiling = CeilingEligibility::unknown;
        check(!derive_appraisal_category(facts).qualified, "Finished dwelling requires access and ceiling facts");
    }
    for (auto factor : {ExactRational{1, 2}, ExactRational{0, 1}, ExactRational{2, 1}}) {
        const auto result = derive_appraisal_category(standard, {}, factor);
        check(!result.qualified && !result.derived_category && result.issues[0].code == "factor_not_unity",
              "Factored arithmetic is not a qualified physical area");
    }
    check(derive_appraisal_category(standard, {}, {2, 2}).qualified, "Exact rational unity is accepted");
    rejected([&] { (void)derive_appraisal_category(standard, {}, {1, 0}); });
    rejected([&] { (void)derive_appraisal_category(standard, {}, {-1, 1}); });
    for (auto role : {BoundaryRole::open_to_below, BoundaryRole::stair_footprint, BoundaryRole::other_void}) {
        facts = standard; facts.role = role; facts.grade = GradeStatus::unknown;
        facts.finish = FinishStatus::unknown; facts.access = AccessStatus::unknown;
        facts.ceiling = CeilingEligibility::unknown;
        const auto result = derive_appraisal_category(facts);
        check(result.qualified && !result.derived_category,
              "Exclusion roles cannot create standalone category contributions");
    }
    AppraisalPolicy commercial{AppraisalPolicyKind::light_commercial_declared, 1};
    facts = {}; facts.property_kind = PropertyKind::light_commercial;
    facts.measurement_basis = MeasurementBasis::exterior;
    facts.role = BoundaryRole::open_to_below;
    const auto commercial_exclusion = derive_appraisal_category(facts, commercial);
    check(commercial_exclusion.qualified && !commercial_exclusion.derived_category,
          "Commercial exclusion roles do not require a fictitious contributing area use");
    facts.role = BoundaryRole::measured_area;
    for (auto use : {AreaUse::commercial_occupiable, AreaUse::commercial_common, AreaUse::commercial_service}) {
        facts.use = use;
        expect(facts, use == AreaUse::commercial_occupiable ? C::commercial_occupiable :
                      use == AreaUse::commercial_common ? C::commercial_common : C::commercial_service, commercial);
        check(!derive_appraisal_category(facts).qualified, "Commercial use cannot enter residential policy");
    }
    facts.use = AreaUse::dwelling;
    check(!derive_appraisal_category(facts, commercial).qualified, "Commercial policy rejects dwelling use");
    rejected([&] { (void)derive_appraisal_category(standard, {AppraisalPolicyKind::residential_declared, 2}); });
    auto area = room("qualified", rectangle(0, 0, 10, 10));
    area.deductions = {{"void", rectangle(1, 1, 2, 5)}};
    area.factor = {1, 2};
    auto measured = qualify_appraisal_area(area, standard);
    check(!measured.qualified && measured.policy_id == "vertex-residential-declared-v1" && measured.policy_version == 1,
          "Measurement retains declared policy provenance");
    near(measured.physical_square_metres.value(), 90, 1e-7, "Physical measurement stays separate from adjustment");
    near(measured.adjusted_square_metres.value(), 45, 1e-7, "Custom adjustment remains inspectable");
    area.factor = {1, 1}; area.scope = AreaScope::site;
    check(!qualify_appraisal_area(area, standard).qualified, "Site scope cannot qualify as building area");
    std::vector<MeasurementArea> commercial_areas;
    for (const auto& [category, name] : std::vector<std::pair<C, std::string>>{
             {C::above_grade_nonstandard_finished, "above_grade_nonstandard_finished"},
             {C::below_grade_nonstandard_finished, "below_grade_nonstandard_finished"},
             {C::noncontinuous_finished, "noncontinuous_finished"},
             {C::commercial_occupiable, "commercial_occupiable"},
             {C::commercial_common, "commercial_common"},
             {C::commercial_service, "commercial_service"}}) {
        check(parse_appraisal_category(name) == category && appraisal_category_name(category) == name,
              "New categories have stable persistence names");
        auto item = room(name, rectangle(0, 0, 10, 10));
        item.classification = name; item.floor_id = name;
        commercial_areas.push_back(item);
    }
    const auto totals = calculate_appraisal_areas(commercial_areas, builtin_appraisal_profile()).property;
    near(totals.commercial_gross_square_metres(), 300, 1e-7,
         "Commercial gross sums occupiable common service once, excluding residential categories");
    near(totals.by_category.at(C::commercial_occupiable).total.square_metres, 100, 1e-7,
         "Commercial occupiable excludes common and service areas");
    near(totals.nonstandard_finished_square_metres(), 200, 1e-7,
         "Nonstandard subtotal excludes noncontinuous and regular finished area");
    near(totals.gla().total.square_metres, 0, 1e-7, "New categories never enter legacy GLA");
}
} // namespace

int main() {
    try {
        using namespace sketch;
        appraisal_tests();
        ansi_policy_tests();
        ansi_v2_sloped_tests();
        ansi_ceiling_rounding_helper_tests();
        ansi_flat_ceiling_acquisition_tests();
        declared_policy_tests();
        CalculationProfile profile{"custom-metric",
                                   1,
                                   AreaUnit::square_metre,
                                   2,
                                   {{"living", {true, true}}, {"garage", {true, false}}}};
        auto area = room("a", rectangle(0, 0, 12, 8));
        auto parcel = room("parcel", rectangle(-1, -1, 100, 100));
        parcel.scope = AreaScope::site;
        const auto mixed = calculate_areas({area, parcel}, profile);
        near(mixed.building.square_metres, 96, 1e-9, "Site enclosure must not add building area");
        near(mixed.living.square_metres, 96, 1e-9, "Site scope must override a living classification rule");
        auto overlapping_site = parcel;
        overlapping_site.id = "second-parcel";
        rejected([&] { (void)calculate_areas({parcel, overlapping_site}, profile); });
        overlapping_site.scope = static_cast<AreaScope>(99);
        rejected([&] { (void)calculate_area(overlapping_site, profile); });
        auto result = calculate_area(area, profile);
        near(result.base_square_metres, 96, 1e-9, "Base area");
        near(result.perimeter_metres, 40, 1e-9, "Boundary perimeter");
        check(result.area_id == "a" && result.profile_id == profile.id && result.profile_version == 1,
              "Calculation provenance");
        area.deductions = {{"b", rectangle(3, 1, 3, 2)}, {"a", rectangle(1, 1, 4, 2)}};
        area.factor = {1, 2};
        result = calculate_area(area, profile);
        near(result.deducted_square_metres, 10, 1e-7, "Overlapping deductions subtract union once");
        near(result.net_square_metres, 86, 1e-7, "Net boundary area");
        near(result.factored_square_metres, 43, 1e-7, "Factor applies after deductions");
        check(result.deductions[0].id == "a", "Deduction provenance order is deterministic");
        near(result.deductions[0].applied_square_metres, 8, 1e-7, "First deduction trace");
        near(result.deductions[1].applied_square_metres, 2, 1e-7, "Overlapping deduction trace");
        std::reverse(area.deductions.begin(), area.deductions.end());
        near(calculate_area(area, profile).factored_square_metres, 43, 1e-7,
             "Input order cannot alter total");
        area.deductions = {{"all", area.boundary}};
        near(calculate_area(area, profile).net_square_metres, 0, 1e-7, "Full-area deduction yields zero");
        area.deductions = {{"outside", rectangle(11, 1, 3, 2)}};
        rejected([&] { (void)calculate_area(area, profile); });
        area.deductions = {{"duplicate", rectangle(1, 1, 1, 1)}, {"duplicate", rectangle(3, 1, 1, 1)}};
        rejected([&] { (void)calculate_area(area, profile); });

        area = room("circle", {{{1, 0}, {-1, 0}, std::numbers::pi}, {{-1, 0}, {1, 0}, std::numbers::pi}});
        result = calculate_area(area, profile);
        near(result.base_square_metres, std::numbers::pi, 1e-8, "Circular area remains analytical");
        near(result.perimeter_metres, 2 * std::numbers::pi, 1e-8, "Circular perimeter remains analytical");
        for (auto& edge : area.boundary) {
            std::swap(edge.start, edge.end);
            edge.sweep_radians *= -1;
        }
        std::reverse(area.boundary.begin(), area.boundary.end());
        near(calculate_area(area, profile).base_square_metres, std::numbers::pi, 1e-8,
             "Winding invariant area");
        area = room("translated", rectangle(1e9, 1e9, 4, 3));
        area.deductions = {{"hole", rectangle(1e9 + 1, 1e9 + 1, 1, 1)}};
        near(calculate_area(area, profile).net_square_metres, 11, 1e-7, "Translation preserves deductions");

        auto a = room("a", rectangle(0, 0, 1.004, 1));
        auto b = room("b", rectangle(2, 0, 1.004, 1));
        b.classification = "garage";
        auto report = calculate_areas({a, b}, profile);
        near(report.building.square_metres, 2.008, 1e-10, "Aggregate unrounded values");
        check(report.building.display.text == "2.01", "Round aggregate only after summing");
        check(report.living.display.text == "1.00", "Explicit living classification rule");
        check(report.by_classification.size() == 2, "Classification subtotals");
        b.boundary = rectangle(0.5, 0, 1, 1);
        rejected([&] { (void)calculate_areas({a, b}, profile); });
        b.floor_id = "floor-2";
        check(calculate_areas({a, b}, profile).areas.size() == 2, "Separate floors may share footprints");
        b.id = a.id;
        rejected([&] { (void)calculate_areas({a, b}, profile); });
        a.classification = "unknown";
        rejected([&] { (void)calculate_area(a, profile); });
        a.classification = "living";
        a.factor = {-1, 1};
        rejected([&] { (void)calculate_area(a, profile); });
        a.factor = {1, 0};
        rejected([&] { (void)calculate_area(a, profile); });
        a.factor = {1, 1};
        a.boundary.pop_back();
        rejected([&] { (void)calculate_area(a, profile); });

        profile.display_unit = AreaUnit::square_foot;
        near(display_area(0.09290304, profile).unrounded, 1, 1e-12, "Exact SI-square-foot conversion");
        profile.display_unit = AreaUnit::acre;
        near(display_area(4046.8564224, profile).unrounded, 1, 1e-12, "Acre conversion");
        profile.display_unit = AreaUnit::square_metre;
        profile.decimal_places = 1;
        check(display_area(1.25, profile).text == "1.3", "Half-away rounding policy");
        near(display_area(1.25, profile).rounding_delta, 0.05, 1e-12, "Visible rounding delta");
        rejected([&] { (void)display_area(std::numeric_limits<double>::infinity(), profile); });
        profile.decimal_places = 20;
        rejected([&] { (void)display_area(1, profile); });
        std::cout << "Calculation tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
