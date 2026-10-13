#pragma once

// Market presentation of the shipped component library.
//
// The architectural SVG catalog is sourced in millimetres ("Base Cabinet 900",
// "Door Hinged 910 Left"). Imperial projects must never show those metric
// names or land at soft-converted sizes such as 2' 11 7/16" × 1' 11 5/8".
// Where a standard US nominal product exists, an imperial project shows the
// US name and places the component at the US nominal size (a 900 mm base
// cabinet is a 36" base cabinet, 36" × 24"). Components without a standard US
// equivalent keep their catalog size, and the library shows it converted and
// rounded to the whole inch.
//
// The catalog itself (IDs, artwork, hashes) is unchanged; this is a desktop
// presentation and placement-size layer only. Metric projects always use the
// catalog names and sizes.

#include "sketch/annotation_catalog.hpp"

#include "measurement_format.hpp"

#include <QString>

#include <cmath>
#include <optional>
#include <string_view>

namespace sketch::desktop {

struct ImperialCatalogVariant {
    std::string_view id;
    // Raw library name; library_display_name() sentence-cases it.
    std::string_view name;
    // Nominal width; for hosted openings the clear opening (wall cut) width.
    double width_inches{};
    // Nominal depth of a free-standing component; 0 keeps the catalog depth.
    double depth_inches{};
};

inline constexpr ImperialCatalogVariant imperial_catalog_variants[] = {
    // Bathroom vanities: US widths 24"/36", 21" standard depth.
    {"svg-v2-01_bathroom-vanity-600", "24\" Vanity", 24, 21},
    {"svg-v2-01_bathroom-vanity-900", "36\" Vanity", 36, 21},
    // Kitchen cabinets: 24" deep base, 12" deep wall cabinets.
    {"svg-v2-02_kitchen-base-cabinet-600", "24\" Base Cabinet", 24, 24},
    {"svg-v2-02_kitchen-base-cabinet-900", "36\" Base Cabinet", 36, 24},
    {"svg-v2-02_kitchen-wall-cabinet-600", "24\" Wall / Upper Cabinet", 24, 12},
    {"svg-v2-02_kitchen-wall-cabinet-900", "36\" Wall / Upper Cabinet", 36, 12},
    // Hinged doors: 760/810/910 mm leaves are the 2' 6", 2' 8" and 3' 0" doors.
    {"svg-v2-09_doors-door-hinged-760-left", "30\" Hinged Door Left", 30, 0},
    {"svg-v2-09_doors-door-hinged-760-right", "30\" Hinged Door Right", 30, 0},
    {"svg-v2-09_doors-door-hinged-810-left", "32\" Hinged Door Left", 32, 0},
    {"svg-v2-09_doors-door-hinged-810-right", "32\" Hinged Door Right", 32, 0},
    {"svg-v2-09_doors-door-hinged-910-left", "36\" Hinged Door Left", 36, 0},
    {"svg-v2-09_doors-door-hinged-910-right", "36\" Hinged Door Right", 36, 0},
    {"svg-v2-09_doors-door-hinged-interior", {}, 32, 0},
    {"svg-v2-09_doors-door-hinged-exterior", {}, 36, 0},
    {"svg-v2-09_doors-door-french-double", {}, 72, 0},
    {"svg-v2-09_doors-door-sliding", {}, 72, 0},
    {"svg-v2-09_doors-door-bifold", {}, 72, 0},
    {"svg-v2-09_doors-door-garage-single", {}, 108, 0},
    {"svg-v2-09_doors-door-garage-double", {}, 192, 0},
    {"svg-v2-16_walls_openings-cased-opening", {}, 36, 0},
    // Fixed windows: 1200/600 mm units are the 4' 0" and 2' 0" windows.
    {"svg-v2-10_windows-window-fixed-1200", "48\" Fixed Window", 48, 0},
    {"svg-v2-10_windows-window-fixed-600", "24\" Fixed Window", 24, 0},
};

[[nodiscard]] inline const ImperialCatalogVariant* imperial_catalog_variant(std::string_view id) noexcept {
    for (const auto& variant : imperial_catalog_variants)
        if (variant.id == id) return &variant;
    return nullptr;
}

// Raw (not yet sentence-cased) library name in the project's unit system.
[[nodiscard]] inline QString catalog_symbol_name(const SymbolDefinition& definition, bool metric) {
    if (!metric)
        if (const auto* variant = imperial_catalog_variant(definition.id); variant && !variant->name.empty())
            return QString::fromUtf8(variant->name.data(), static_cast<qsizetype>(variant->name.size()));
    return QString::fromStdString(definition.name.empty() ? definition.family : definition.name);
}

// Per-axis multipliers that place a free-standing component at its market
// nominal size (1, 1 when the catalog size already applies).
struct CatalogAxisScale {
    double width{1.0};
    double depth{1.0};
};

[[nodiscard]] inline CatalogAxisScale catalog_market_axis_scale(const SymbolDefinition& definition, bool metric) {
    CatalogAxisScale result;
    if (metric || definition.width_metres <= 0.0 || definition.depth_metres <= 0.0) return result;
    const auto* variant = imperial_catalog_variant(definition.id);
    if (!variant) return result;
    if (variant->width_inches > 0.0)
        result.width = variant->width_inches * units::metres_per_inch / definition.width_metres;
    if (variant->depth_inches > 0.0)
        result.depth = variant->depth_inches * units::metres_per_inch / definition.depth_metres;
    return result;
}

// Hosted-opening wall-cut width in the project's market. Imperial uses the US
// nominal width when one exists, otherwise the catalog width rounded to the
// nearest whole inch; metric keeps the catalog width.
[[nodiscard]] inline double market_opening_width(const SymbolDefinition& definition,
                                                 double catalog_width_metres, bool metric) {
    if (metric) return catalog_width_metres;
    if (const auto* variant = imperial_catalog_variant(definition.id); variant && variant->width_inches > 0.0)
        return variant->width_inches * units::metres_per_inch;
    const auto inches = std::round(catalog_width_metres / units::metres_per_inch);
    return inches >= 1.0 ? inches * units::metres_per_inch : catalog_width_metres;
}

// Library size text in the project's unit system. Imperial catalog sizes are
// shown at whole-inch precision (they are nominal product sizes, and a
// soft-converted 1/16" reads as false precision); metric uses the formatter's
// active precision.
[[nodiscard]] inline QString catalog_size_text(double width_metres, double depth_metres, bool metric) {
    auto precision = units::display_precision();
    if (!metric) {
        precision.decimal_feet = false;
        precision.imperial_denominator = 1;
    }
    return QStringLiteral("%1 × %2").arg(units::length_input_text(width_metres, metric, precision),
                                         units::length_input_text(depth_metres, metric, precision));
}

} // namespace sketch::desktop
