#include "sketch/annotation_catalog.hpp"
#include "sketch/pinc_symbol_counterparts.hpp"

#include <algorithm>
#include <array>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace {
using namespace sketch;

struct ExpectedBinding {
    std::string_view source_kind;
    std::string_view catalog_id;
};

constexpr std::array<ExpectedBinding, 80> kExpectedBindings{{
    {"Arrow - North", "svg-v2-25_drafting_symbols-north-arrow"},
    {"Arrow - Plain", "svg-v2-25_drafting_symbols-arrow-plain"},
    {"Bath - Bathtub 5'", "svg-v2-01_bathroom-tub-alcove-5ft"},
    {"Bath - Bidet", "svg-v2-01_bathroom-bidet"},
    {"Bath - Corner Tub", "svg-v2-01_bathroom-tub-corner"},
    {"Bath - Freestanding Tub", "svg-v2-01_bathroom-tub-freestanding-oval"},
    {"Bath - Linen Cabinet", "svg-v2-08_storage-linen-cabinet"},
    {"Bath - Pedestal Sink", "svg-v2-01_bathroom-basin-pedestal"},
    {"Bath - Shower Corner", "svg-v2-01_bathroom-shower-quadrant"},
    {"Bath - Shower Glass", "svg-v2-01_bathroom-shower-glass"},
    {"Bath - Shower Rect", "svg-v2-01_bathroom-shower-rectangular"},
    {"Bath - Toilet", "svg-v2-01_bathroom-toilet-close-coupled"},
    {"Bath - Tub / Shower", "svg-v2-01_bathroom-tub-shower-combination"},
    {"Bath - Vanity Double", "svg-v2-01_bathroom-vanity-double"},
    {"Bath - Vanity Single", "svg-v2-01_bathroom-vanity-600"},
    {"Cased Opening", "svg-v2-16_walls_openings-opening"},
    {"Closet - Rod Shelf", "svg-v2-08_storage-closet-rod-shelf"},
    {"Closet - Walk In", "svg-v2-08_storage-closet-walk-in"},
    {"Column", "svg-v2-15_structure_access-column-round"},
    {"Door - Barn", "svg-v2-09_doors-door-barn"},
    {"Door - Bifold", "svg-v2-09_doors-door-bifold"},
    {"Door - Exterior", "svg-v2-09_doors-door-hinged-810-left"},
    {"Door - French Double", "svg-v2-09_doors-door-double"},
    {"Door - Garage Double", "svg-v2-09_doors-door-garage"},
    {"Door - Garage Single", "svg-v2-09_doors-door-garage"},
    {"Door - Interior", "svg-v2-09_doors-door-hinged-810-left"},
    {"Door - Pocket", "svg-v2-09_doors-door-pocket"},
    {"Door - Sliding", "svg-v2-09_doors-door-sliding-glass"},
    {"Fireplace - Corner", "svg-v2-18_fireplace-fireplace-corner"},
    {"Fireplace - Double Sided", "svg-v2-18_fireplace-fireplace-double-sided"},
    {"Fireplace - Standard", "svg-v2-18_fireplace-fireplace-standard"},
    {"Hot Tub", "svg-v2-13_outdoor-hot-tub"},
    {"Kitchen - Cabinet Run", "svg-v2-02_kitchen-cabinet-run"},
    {"Kitchen - Cooktop", "svg-v2-02_kitchen-cooktop-gas-four"},
    {"Kitchen - Corner Cabinet", "svg-v2-02_kitchen-corner-cabinet"},
    {"Kitchen - Dishwasher", "svg-v2-02_kitchen-dishwasher"},
    {"Kitchen - Island", "svg-v2-02_kitchen-kitchen-island"},
    {"Kitchen - Island Seating", "svg-v2-02_kitchen-kitchen-island-seating"},
    {"Kitchen - Microwave", "svg-v2-02_kitchen-microwave"},
    {"Kitchen - Pantry", "svg-v2-02_kitchen-pantry-cabinet"},
    {"Kitchen - Peninsula", "svg-v2-02_kitchen-kitchen-peninsula"},
    {"Kitchen - Range", "svg-v2-02_kitchen-range-standard"},
    {"Kitchen - Refrigerator", "svg-v2-02_kitchen-fridge-french-door"},
    {"Kitchen - Sink Base", "svg-v2-02_kitchen-sink-base"},
    {"Kitchen - Sink Double", "svg-v2-02_kitchen-sink-stainless-double"},
    {"Kitchen - Sink Single", "svg-v2-02_kitchen-sink-stainless-single"},
    {"Kitchen - Wall Oven", "svg-v2-02_kitchen-oven-built-in"},
    {"Laundry - Dryer", "svg-v2-03_laundry_utility-dryer-front-load"},
    {"Laundry - Utility Sink", "svg-v2-03_laundry_utility-laundry-sink"},
    {"Laundry - Washer", "svg-v2-03_laundry_utility-washer-front-load"},
    {"Laundry - Washer Dryer Stack", "svg-v2-03_laundry_utility-washer-dryer-stack"},
    {"Mechanical - Electrical Panel", "svg-v2-03_laundry_utility-electrical-panel"},
    {"Mechanical - FWA", "svg-v2-03_laundry_utility-air-handler"},
    {"Mechanical - Floor Drain", "svg-v2-20_hvac_plumbing-floor-drain"},
    {"Mechanical - Furnace", "svg-v2-20_hvac_plumbing-furnace"},
    {"Mechanical - HVAC", "svg-v2-20_hvac_plumbing-hvac-system"},
    {"Mechanical - HWB", "svg-v2-20_hvac_plumbing-baseboard-heater"},
    {"Mechanical - Sump", "svg-v2-20_hvac_plumbing-sump-pump"},
    {"Mechanical - Water Heater", "svg-v2-03_laundry_utility-water-heater"},
    {"Misc - Structural Issue", "svg-v2-25_drafting_symbols-structural-issue"},
    {"Open Below", "svg-v2-16_walls_openings-open-below"},
    {"Outdoor - Fire Pit", "svg-v2-13_outdoor-fire-pit"},
    {"Outdoor - Grill", "svg-v2-13_outdoor-barbecue-grill"},
    {"Pool - Freeform", "svg-v2-21_swimming_pools-pool-freeform"},
    {"Pool - Kidney", "svg-v2-21_swimming_pools-pool-kidney"},
    {"Pool - Oval", "svg-v2-21_swimming_pools-pool-oval"},
    {"Pool - Rectangle", "svg-v2-21_swimming_pools-pool-rectangular"},
    {"Railing", "svg-v2-17_railings-railing"},
    {"Stairs Down", "svg-v2-11_circulation-stairs-straight-down"},
    {"Stairs L", "svg-v2-11_circulation-stairs-l"},
    {"Stairs Spiral", "svg-v2-11_circulation-stairs-spiral"},
    {"Stairs U", "svg-v2-11_circulation-stairs-u"},
    {"Stairs Up", "svg-v2-11_circulation-stairs-straight-up"},
    {"Window - Bay", "svg-v2-10_windows-window-bay"},
    {"Window - Bow", "svg-v2-10_windows-window-bow"},
    {"Window - Corner", "svg-v2-10_windows-window-corner"},
    {"Window - Double", "svg-v2-10_windows-window-double"},
    {"Window - Standard", "svg-v2-10_windows-window-standard"},
    {"Window - Triple", "svg-v2-10_windows-window-triple"},
    {"Wood Stove", "svg-v2-18_fireplace-wood-stove"},
}};

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

const SymbolDefinition& require_symbol(const std::vector<SymbolDefinition>& catalog,
                                      std::string_view id) {
    for (const auto& symbol : catalog)
        if (symbol.id == id) return symbol;
    throw std::runtime_error("catalog is missing symbol " + std::string(id));
}

void test_pinc_counterparts_cover_the_static_source_inventory() {
    const auto counterparts = default_pinc_symbol_counterparts();
    require(counterparts.size() == kExpectedBindings.size(),
            "expected exactly 80 Pinc symbol counterparts");

    std::unordered_set<std::string> source_kinds;
    for (const auto& counterpart : counterparts) {
        require(!counterpart.source_kind.empty(), "counterpart has an empty Pinc source kind");
        require(!counterpart.catalog_id.empty(), "counterpart has an empty catalog ID");
        require(!counterpart.fidelity_note.empty(), "counterpart has no fidelity note");
        require(source_kinds.insert(counterpart.source_kind).second,
                "duplicate Pinc source kind: " + counterpart.source_kind);
    }

    for (const auto& expected : kExpectedBindings) {
        const auto found = std::find_if(counterparts.begin(), counterparts.end(),
            [&](const auto& counterpart) { return counterpart.source_kind == expected.source_kind; });
        require(found != counterparts.end(),
                "missing Pinc source kind: " + std::string(expected.source_kind));
        require(found->catalog_id == expected.catalog_id,
                "wrong catalog counterpart for " + std::string(expected.source_kind));
    }
}

void test_counterparts_resolve_to_distinct_bundled_window_artwork() {
    const auto catalog = default_symbol_catalog();
    std::set<std::string> artwork_hashes;
    for (const auto id : {"svg-v2-10_windows-window-bow",
                          "svg-v2-10_windows-window-corner",
                          "svg-v2-10_windows-window-standard"}) {
        const auto& symbol = require_symbol(catalog, id);
        require(symbol.svg_asset.has_value(), "window counterpart is not a bundled SVG");
        require(!symbol.svg_asset->sha256.empty(), "window SVG has no content hash");
        require(artwork_hashes.insert(symbol.svg_asset->sha256).second,
                "bow, corner, and standard windows must use distinct artwork");
        require(symbol.width_metres > 0 && symbol.depth_metres > 0,
                "window counterpart has no positive nominal footprint");
        require(symbol.svg_asset->dimensions_are_nominal,
                "window counterpart must identify its editable default dimensions as nominal");
    }
}
} // namespace

int main() {
    try {
        test_pinc_counterparts_cover_the_static_source_inventory();
        test_counterparts_resolve_to_distinct_bundled_window_artwork();
    } catch (const std::exception& error) {
        std::cerr << "pinc symbol counterpart test failed: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
