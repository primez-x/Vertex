#include "sketch/annotation_catalog.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <numbers>
#include <set>
#include <stdexcept>
#include <vector>

namespace {
void require(bool value, const char* message) {
    if (!value) { std::cerr << message << '\n'; std::exit(1); }
}
template<class F> void rejected(F f) {
    try { f(); } catch (const std::invalid_argument&) { return; }
    require(false,"Expected invalid_argument");
}
}

int main() {
    using namespace sketch;
    const auto catalog = default_symbol_catalog();
    {
        AnnotationState placed;
        placed.symbols.push_back({"sized", catalog.front().id, {}, {}, true});
        const auto saved = encode_annotation_state(placed, catalog);
        require(saved.at("version") == 3 && saved.at("symbols")[0].contains("width_scale") &&
                    saved.at("symbols")[0].contains("depth_scale") &&
                    saved.at("symbols")[0].contains("flip_horizontal") &&
                    saved.at("symbols")[0].contains("flip_vertical"),
                "Symbol independent dimensions and mirrors must have a versioned persisted representation");
    }
    {
        const auto original = filter_symbol_catalog(catalog, "Toilet Close Coupled", "01_bathroom").front();
        AnnotationState placed;
        placed.symbols.push_back({"pinned", original.id, {{3, 4}, 0.5, 1.4, "ground"}, {}, false});
        const auto saved = encode_annotation_state(placed, {original});
        require(saved.at("symbols")[0].contains("definition"), "Placed symbols must persist a definition snapshot");
        auto hash_changed = original;
        hash_changed.svg_asset->sha256[0] =
            hash_changed.svg_asset->sha256[0] == '0' ? '1' : '0';
        const auto hash_reopened = decode_annotation_state(saved, {hash_changed});
        require(symbol_requires_migration(hash_reopened.symbols.front(), {hash_changed}),
                "Artwork digest change must require explicit migration without a revision bump");
        auto updated = original;
        updated.artwork_revision = 2;
        updated.name = "Revised toilet";
        auto reopened = decode_annotation_state(saved, {updated});
        auto legacy_saved = saved;
        legacy_saved["version"] = 1;
        legacy_saved["symbols"][0].erase("definition");
        legacy_saved["symbols"][0].erase("pinned_svg");
        for (const auto* key : {"width_scale", "depth_scale", "flip_horizontal", "flip_vertical"})
            legacy_saved["symbols"][0].erase(key);
        require(encode_annotation_state(decode_annotation_state(legacy_saved, {original}), {original}) == saved,
                "Legacy state must pin its original definition on upgrade");
        rejected([&] { (void)decode_annotation_state(legacy_saved, {updated}); });
        require(symbol_requires_migration(reopened.symbols.front(), {updated}), "Revision change must be flagged");
        require(!resolved_symbol_definition(reopened.symbols.front(), {updated}).svg_asset,
                "Stale path must never load replacement artwork");
        require(resolved_symbol_definition(reopened.symbols.front(), {updated}).name == original.name,
                "Reopen must retain original definition");
        require(encode_annotation_state(reopened, {updated}) == saved, "Saving must not migrate a pinned instance");
        const auto migrated = migrate_symbol_definition(reopened, "pinned", {updated}, "<svg/>");
        require(!symbol_requires_migration(migrated.symbols.front(), {updated}) &&
                    migrated.symbols.front().pinned_svg == "<svg/>", "Explicit migration must pin new artwork");
        const auto migrated_saved = encode_annotation_state(migrated, {updated});
        auto pinned_v2 = migrated_saved;
        pinned_v2["version"] = 2;
        for (const auto* key : {"width_scale", "depth_scale", "flip_horizontal", "flip_vertical"})
            pinned_v2["symbols"][0].erase(key);
        const auto pinned_upgrade = decode_annotation_state(pinned_v2, {original});
        require(pinned_upgrade.symbols.front().pinned_svg == "<svg/>" &&
                    encode_annotation_state(pinned_upgrade, {original}) == migrated_saved,
                "Version2 upgrade must retain exact SVG bytes and historical definition when catalog artwork differs");
        auto migrated_json = encode_annotation_state(migrated, {updated});
        migrated_json["symbols"][0]["definition"] = saved["symbols"][0]["definition"];
        migrated_json["symbols"][0]["pinned_svg"] = "";
        require(migrated_json == saved, "Migration changed instance properties");
        require(symbol_requires_migration(reopened.symbols.front(), {}), "Removed definitions must be flagged");
        require(encode_annotation_state(decode_annotation_state(saved, {}), {}) == saved,
                "Removed definitions must still reopen and save");
        rejected([&] { (void)migrate_symbol_definition(reopened, "pinned", {}); });
        updated.maximum_scale = 1;
        rejected([&] { (void)migrate_symbol_definition(reopened, "pinned", {updated}); });
        auto malformed_pin = saved;
        malformed_pin["symbols"][0]["definition"]["id"] = "different";
        rejected([&] { (void)decode_annotation_state(malformed_pin, {original}); });
        malformed_pin = saved;
        malformed_pin["symbols"][0]["pinned_svg"] =
            "<svg xmlns=\"http://www.w3.org/2000/svg\"><script>alert(1)</script></svg>";
        rejected([&] { (void)decode_annotation_state(malformed_pin, {original}); });
        malformed_pin["symbols"][0]["pinned_svg"] =
            "<svg xmlns=\"http://www.w3.org/2000/svg\"><path fill=\"url(https://example.test/a)\"/></svg>";
        rejected([&] { (void)decode_annotation_state(malformed_pin, {original}); });
    }
    {
        // An off-origin, asymmetric marker catches use of the wrong anchor,
        // world-axis reflection, and scaling after rotation.
        SymbolDefinition marker{"marker", "markers", "test", 4, 6, {2, -3},
                                0.01, 100, {{{3, -1}, {0, -4}}}};
        SymbolInstance instance{"marker-1", marker.id, {{10, 20}, std::numbers::pi / 2, 2}, {}, true};
        instance.width_scale = 1.5;
        instance.depth_scale = 0.25;
        const auto close = [](Vec2 actual, Vec2 expected) {
            return std::abs(actual.x - expected.x) < 1e-12 &&
                   std::abs(actual.y - expected.y) < 1e-12;
        };
        const auto assert_stroke = [&](Vec2 start, Vec2 end) {
            const auto result = transformed_symbol_preview(marker, instance);
            require(result.size() == 1 && close(result[0].start, start) && close(result[0].end, end),
                    "Symbol transform must resize and mirror local axes about the saved anchor before rotation");
            require(close(transformed_symbol_point(marker, instance, marker.preview[0].start), start),
                    "Footprint point transform must agree with artwork geometry");
            require(close(transformed_symbol_point(marker, instance, marker.anchor), {10, 20}),
                    "Independent dimensions and mirrors must preserve the placement anchor");
        };
        assert_stroke({9, 23}, {10.5, 14});
        instance.flip_horizontal = true;
        assert_stroke({9, 17}, {10.5, 26});
        instance.flip_horizontal = false;
        instance.flip_vertical = true;
        assert_stroke({11, 23}, {9.5, 14});
        instance.flip_horizontal = true;
        assert_stroke({11, 17}, {9.5, 26});
        AnnotationState edited;
        edited.symbols.push_back(instance);
        const auto saved = encode_annotation_state(edited, {marker});
        require(encode_annotation_state(decode_annotation_state(saved, {}), {}) == saved,
                "Independent dimensions and both mirrors must roundtrip with a removed pinned catalog definition");
        for (const auto invalid : {0.0, -1.0, std::numeric_limits<double>::quiet_NaN(),
                                   std::numeric_limits<double>::infinity(), 0.001, 100.0}) {
            auto bad = instance;
            bad.width_scale = invalid;
            rejected([&] { (void)transformed_symbol_preview(marker, bad); });
            bad = instance;
            bad.depth_scale = invalid;
            rejected([&] { (void)transformed_symbol_point(marker, bad, marker.anchor); });
        }
        // Limits apply to each effective axis, not to the independent factor.
        auto within_limits = instance;
        within_limits.placement.scale = 200;
        within_limits.width_scale = within_limits.depth_scale = 0.25;
        require(!transformed_symbol_preview(marker, within_limits).empty(),
                "Combined axis scale must govern limits even when uniform scale alone exceeds them");
        within_limits.placement.scale = 1;
        within_limits.width_scale = marker.minimum_scale;
        within_limits.depth_scale = marker.maximum_scale;
        require(!transformed_symbol_preview(marker, within_limits).empty(),
                "Both inclusive per-axis catalog boundaries must be supported");
        auto overflowing = instance;
        overflowing.width_scale = std::numeric_limits<double>::max();
        rejected([&] { (void)transformed_symbol_preview(marker, overflowing); });
        AnnotationState invalid_state;
        invalid_state.symbols.push_back(overflowing);
        rejected([&] { validate_annotation_state(invalid_state, {marker}); });
        auto oversized_definition = marker;
        oversized_definition.width_metres = std::numeric_limits<double>::max();
        rejected([&] { (void)transformed_symbol_preview(oversized_definition, instance); });
        rejected([&] { (void)transformed_symbol_point(marker, instance,
            {std::numeric_limits<double>::max(), 0}); });
        rejected([&] { (void)transformed_symbol_point(marker, instance,
            {std::numeric_limits<double>::infinity(), 0}); });
        for (const auto* key : {"width_scale", "depth_scale", "flip_horizontal", "flip_vertical"}) {
            auto malformed = saved;
            malformed["symbols"][0].erase(key);
            rejected([&] { (void)decode_annotation_state(malformed, {marker}); });
        }
        auto malformed = saved;
        malformed["symbols"][0]["extra_transform"] = 1;
        rejected([&] { (void)decode_annotation_state(malformed, {marker}); });
        malformed = saved;
        malformed["symbols"][0]["flip_horizontal"] = 1;
        rejected([&] { (void)decode_annotation_state(malformed, {marker}); });
        malformed = saved;
        malformed["symbols"][0]["width_scale"] = "2";
        rejected([&] { (void)decode_annotation_state(malformed, {marker}); });
        malformed = saved;
        malformed["version"] = 2;
        rejected([&] { (void)decode_annotation_state(malformed, {marker}); });
        AnnotationState original;
        original.symbols.push_back({"legacy", marker.id, {{10, 20}, std::numbers::pi / 2, 2}, {}, true});
        const auto original_saved = encode_annotation_state(original, {marker});
        auto legacy = original_saved;
        legacy["version"] = 2;
        for (const auto* key : {"width_scale", "depth_scale", "flip_horizontal", "flip_vertical"})
            legacy["symbols"][0].erase(key);
        const auto upgraded = decode_annotation_state(legacy, {});
        const auto geometry = transformed_symbol_preview(marker, upgraded.symbols[0]);
        require(close(geometry[0].start, {6, 22}) && close(geometry[0].end, {12, 16}) &&
                    encode_annotation_state(upgraded, {}) == original_saved,
                "Version2 upgrade must preserve exact old geometry and pinned definition");
        legacy["version"] = 1;
        legacy["symbols"][0].erase("definition");
        legacy["symbols"][0].erase("pinned_svg");
        require(encode_annotation_state(decode_annotation_state(legacy, {marker}), {marker}) == original_saved,
                "Version1 upgrade must preserve old geometry and pin the original catalog definition");
    }
    require(catalog.size() == 1131, "Preserve 809 legacy symbols and expose 322 SVG symbols");
    const auto svg_toilets = filter_symbol_catalog(catalog, "Toilet Close Coupled", "01_bathroom");
    require(svg_toilets.size() == 1 &&
                svg_toilets.front().id == "svg-v2-01_bathroom-toilet-close-coupled",
            "SVG human names must be searchable with category-qualified stable IDs");
    std::size_t svg_count = 0, nominal_count = 0;
    std::set<std::string> svg_categories, svg_paths;
    for (const auto& definition : catalog) {
        if (!definition.svg_asset) continue;
        ++svg_count;
        const auto& asset = *definition.svg_asset;
        nominal_count += asset.dimensions_are_nominal ? 1 : 0;
        svg_categories.insert(definition.category);
        require(svg_paths.insert(asset.relative_path).second, "Every SVG must retain its own asset");
        require(!definition.name.empty() && asset.view_box[2] > 0 && asset.view_box[3] > 0,
                "SVG renderers require human names and positive intrinsic bounds");
    }
    require(svg_count == 322 && nominal_count == 210 && svg_categories.size() == 25,
            "Import complete SVG category coverage without inventing nominal dimensions");
    AnnotationState every_symbol;
    for (const auto& definition : catalog)
        every_symbol.symbols.push_back({"instance-" + definition.id, definition.id, {}, {}, true});
    const auto every_symbol_encoded = encode_annotation_state(every_symbol, catalog);
    require(encode_annotation_state(decode_annotation_state(every_symbol_encoded, catalog), catalog) ==
                every_symbol_encoded,
            "Every legacy and SVG ID must survive project annotation serialization");
    require(filter_symbol_catalog(catalog, "svg-v2-03_laundry_utility-radiator").size() == 1 &&
                filter_symbol_catalog(catalog, "svg-v2-20_hvac_plumbing-radiator").size() == 1,
            "Repeated source IDs in different categories must not overwrite one another");
    const auto basin = filter_symbol_catalog(catalog, "svg-v2-01_bathroom-basin-oval").front();
    require(basin.width_metres == 0.55 && basin.depth_metres == 0.44 &&
                basin.svg_asset->footprint_view_box == std::array<double, 4>{0, 0, 550, 440},
            "Source nominal millimetres must map to metres without artwork padding");
    auto invalid_svg = basin;
    invalid_svg.svg_asset->relative_path = "../escape.svg";
    rejected([&]{validate_symbol_catalog({invalid_svg});});
    invalid_svg = basin; invalid_svg.svg_asset->view_box[2] = 0;
    rejected([&]{validate_symbol_catalog({invalid_svg});});
    invalid_svg = basin; invalid_svg.svg_asset->footprint_view_box[0] = std::numeric_limits<double>::infinity();
    rejected([&]{validate_symbol_catalog({invalid_svg});});
    require(catalog.size() >= 600,"Expected the expanded production symbol catalog");
    validate_symbol_catalog(catalog);
    const std::set<std::string> required_categories{
        "plumbing", "furniture", "fixtures", "appliances", "accessibility",
        "lighting", "doors_windows", "structural", "site", "commercial"};
    std::set<std::string> categories;
    std::set<std::string> families;
    for (const auto& definition : catalog) {
        categories.insert(definition.category);
        families.insert(definition.family);
        require(definition.width_metres > 0 && definition.depth_metres > 0,
                "Every symbol must expose physical dimensions");
        require(definition.minimum_scale < 1.0 && definition.maximum_scale > 1.0,
                "Every symbol must support practical resizing");
    }
    for (const auto& category : required_categories)
        require(categories.contains(category), "Required symbol category is missing");
    require(families.size() >= 300, "Catalog must expose at least 300 distinct component families");
    const auto plumbing = filter_symbol_catalog(catalog, "", "plumbing");
    require(plumbing.size() >= 36, "Category filtering must return all plumbing variants");
    require(filter_symbol_catalog(catalog, "", "PLUMBING").size() == plumbing.size(),
            "Symbol category filtering must be case-insensitive for field use");
    const auto toilets = filter_symbol_catalog(catalog, "toilet");
    require(toilets.size() >= 18, "Query filtering must match toilet families and variants");
    require(filter_symbol_catalog(catalog, "TOILET").size() == toilets.size(),
            "Symbol search must be case-insensitive for field use");
    const auto has_family = [&](const char* family) {
        return std::any_of(catalog.begin(), catalog.end(), [&](const auto& definition) {
            return definition.family == family && definition.preview.size() >= 5;
        });
    };
    require(has_family("toilet") && has_family("single-bed") && has_family("sofa") &&
                has_family("floor-drain"),
            "Core residential and plumbing families must have usable vector motifs");
    const auto nominal = [&](const char* family) -> const SymbolDefinition& {
        const auto found = std::find_if(catalog.begin(), catalog.end(), [&](const auto& definition) {
            return definition.family == family && definition.id.ends_with("-w2-d2");
        });
        require(found != catalog.end(), "Every representative family needs a nominal variant");
        return *found;
    };
    const auto normalized_preview = [](const SymbolDefinition& definition) {
        std::vector<std::array<long long, 4>> result;
        result.reserve(definition.preview.size());
        const auto normalize = [](double value, double half_extent) {
            return static_cast<long long>(std::llround(value / half_extent * 1000.0));
        };
        for (const auto& stroke : definition.preview) {
            result.push_back({normalize(stroke.start.x - definition.anchor.x,
                                        definition.width_metres / 2.0),
                             normalize(stroke.start.y - definition.anchor.y,
                                       definition.depth_metres / 2.0),
                             normalize(stroke.end.x - definition.anchor.x,
                                       definition.width_metres / 2.0),
                             normalize(stroke.end.y - definition.anchor.y,
                                       definition.depth_metres / 2.0)});
        }
        return result;
    };
    require(normalized_preview(nominal("range")) != normalized_preview(nominal("dishwasher")) &&
                normalized_preview(nominal("washer")) != normalized_preview(nominal("dryer")) &&
                normalized_preview(nominal("wardrobe")) != normalized_preview(nominal("bookcase")) &&
                normalized_preview(nominal("checkout-counter")) !=
                    normalized_preview(nominal("service-counter")),
            "Named symbol families must retain distinguishable vector motifs");
    const auto repeated = default_symbol_catalog();
    require(std::abs(nominal("sofa").width_metres - 2.1) < 1e-12 &&
                std::abs(nominal("sofa").depth_metres - 0.9) < 1e-12 &&
                std::abs(nominal("double-bed").width_metres - 1.4) < 1e-12 &&
                std::abs(nominal("double-bed").depth_metres - 2.0) < 1e-12 &&
                std::abs(nominal("toilet").width_metres - 0.4) < 1e-12 &&
                std::abs(nominal("toilet").depth_metres - 0.7) < 1e-12,
            "Artwork refinements must preserve nominal real-world footprints");
    // Upholstery and sanitary ware need actual curved silhouettes, rather than
    // the old shared box with family decorations drawn over it.
    for (const auto* family : {"sofa", "armchair", "chair", "single-bed", "double-bed",
                               "toilet", "bidet", "bathtub", "sink"}) {
        const auto strokes = normalized_preview(nominal(family));
        require(std::any_of(strokes.begin(), strokes.end(), [](const auto& stroke) {
                    return stroke[0] != stroke[2] && stroke[1] != stroke[3];
                }), "Soft furniture and bowls must retain curved outline segments");
        require(std::none_of(strokes.begin(), strokes.end(), [](const auto& stroke) {
                    return std::abs(stroke[0]) == 1000 && std::abs(stroke[2]) == 1000 &&
                           std::abs(stroke[1]) == 1000 && std::abs(stroke[3]) == 1000;
                }), "Curved families must not regain a generic rectangular footprint overlay");
    }
    std::set<std::vector<std::array<long long, 4>>> residential_signatures;
    for (const auto* family : {"sofa", "armchair", "chair", "bench", "single-bed", "double-bed",
                               "desk", "dining-table", "coffee-table", "side-table", "toilet",
                               "bidet", "urinal", "sink", "double-sink", "bathtub", "shower",
                               "range", "refrigerator", "dishwasher", "washer", "dryer"})
        require(residential_signatures.insert(normalized_preview(nominal(family))).second,
                "Residential family silhouettes must remain structurally distinguishable");
    for (std::size_t i=0;i<catalog.size();++i) {
        require(catalog[i].id == repeated[i].id && catalog[i].width_metres == repeated[i].width_metres,
                "Catalog must be deterministic");
        require(!placed_symbol_preview(catalog[i],{}).empty(),"Every catalog entry must produce preview strokes");
        require(normalized_preview(catalog[i]) == normalized_preview(repeated[i]),
                "All vector coordinates must be deterministic");
        require(catalog[i].svg_asset || normalized_preview(catalog[i]) == normalized_preview(nominal(catalog[i].family.c_str())),
                "Dimension variants must retain their family structure");
    }
    const auto manifest = encode_symbol_catalog_manifest(catalog);
    const auto svg_manifest = encode_symbol_catalog_manifest({basin});
    require(svg_manifest.at("entries").at(0).at("svg_asset").at("relative_path") ==
                "symbols/architectural_v2/symbols/01_bathroom/basin-oval.svg" &&
                svg_manifest.at("entries").at(0).at("name") == "Basin Oval",
            "Offline manifest must preserve renderer metadata and human names");
    require(manifest.at("schema_version") == 1 && manifest.at("catalog_id") == "vertex.symbol-catalog",
            "Symbol catalog manifest must identify its schema");
    require(manifest.at("catalog_revision") == kSymbolCatalogRevision,
            "Symbol catalog manifest must identify its revision");
    require(manifest.at("entry_count") == catalog.size() && manifest.at("family_count") == families.size() &&
                manifest.at("entries").size() == catalog.size(),
            "Symbol catalog manifest must enumerate every entry and family");
    require(manifest.at("category_counts").at("fixtures") >= 63 &&
                manifest.at("category_counts").at("commercial") >= 36,
            "Symbol catalog manifest must retain category coverage");
    const auto& manifest_families = manifest.at("families");
    require(manifest_families.at(0).at("id") == "accessible-bathtub" &&
                manifest_families.at(0).at("variant_count") == 1,
            "Symbol catalog manifest families must be stable and sorted");
    const auto legacy_family = std::find_if(manifest_families.begin(), manifest_families.end(),
        [](const auto& value) { return value.at("id") == "accessible-shower"; });
    require(legacy_family != manifest_families.end() && legacy_family->at("variant_count") == 9,
            "Existing preset families must retain all nine size variants");
    require(manifest == encode_symbol_catalog_manifest(repeated),
            "Symbol catalog manifest must be deterministic");
    require(manifest.at("entries").at(0).at("preview").size() >= 5,
            "Symbol catalog manifest must retain vector previews");
    auto labels = default_label_templates();
    require(filter_label_templates(labels,"room","rooms").size() >= 2,"Label filtering failed");
    require(filter_label_templates(labels,"","missing").empty(),"Category filtering failed");
    AnnotationState state;
    state.labels.push_back(instantiate_label(labels.front(),"label-1"));
    state.labels[0].content = "Guest bedroom\nNorth wing";
    state.labels[0].visible = false;
    state.labels[0].placement = {{4,3},0.25,2};
    state.labels[0].style.bold = true;
    state.labels[0].style.fill_pattern = "hatch";
    state.symbols.push_back({"symbol-1",catalog.front().id,{{10,20},std::numbers::pi/2,2},{},false});
    state.overrides.push_back({"area","area-1",{},false});
    state.overrides.push_back({"output_view","print-1",{},true});
    auto encoded = encode_annotation_state(state,catalog);
    require(encoded.at("catalog_revision") == kSymbolCatalogRevision,
            "Annotation state must pin the symbol catalog revision");
    const auto decoded = decode_annotation_state(nlohmann::json::parse(encoded.dump()),catalog);
    require(encode_annotation_state(decoded,catalog) == encoded,"JSON roundtrip loses edits");
    auto legacy = encoded;
    legacy.erase("catalog_revision");
    require(encode_annotation_state(decode_annotation_state(legacy,catalog),catalog) == encoded,
            "Legacy annotation state without a catalog revision must upgrade deterministically");
    auto unsupported_revision = encoded;
    unsupported_revision["version"] = 1;
    unsupported_revision["catalog_revision"] = kSymbolCatalogRevision + 1;
    rejected([&]{(void)decode_annotation_state(unsupported_revision,catalog);});
    require(labels.front().content == "Bedroom","Instance edits mutated library template");
    auto transformed = placed_symbol_preview(catalog.front(),state.symbols.front().placement);
    const auto local = catalog.front().preview.front().start;
    require(std::abs(transformed.front().start.x-(10-2*local.y)) < 1e-12 &&
            std::abs(transformed.front().start.y-(20+2*local.x)) < 1e-12,"Placement rotation/scale/translation wrong");
    const std::array representatives{"toilet", "double-bed", "sofa", "checkout-counter"};
    const std::array resize_scales{0.5, 1.0, 2.4};
    const std::array rotations{0.0, std::numbers::pi / 4.0, std::numbers::pi / 2.0};
    for (std::size_t family_index = 0; family_index < representatives.size(); ++family_index) {
        const auto& definition = nominal(representatives[family_index]);
        for (const auto scale : resize_scales) {
            for (const auto rotation : rotations) {
                const AnnotationPlacement placement{{10.0 + static_cast<double>(family_index),
                                                     20.0 + scale}, rotation, scale};
                const auto placed = placed_symbol_preview(definition, placement);
                require(!placed.empty(),
                        "Representative symbols must render at every supported fixture scale and rotation");
                const auto first = definition.preview.front().start;
                const auto cosine = std::cos(rotation);
                const auto sine = std::sin(rotation);
                const auto local_x = (first.x - definition.anchor.x) * scale;
                const auto local_y = (first.y - definition.anchor.y) * scale;
                const Vec2 expected{placement.position.x + cosine * local_x - sine * local_y,
                                    placement.position.y + sine * local_x + cosine * local_y};
                require(std::abs(placed.front().start.x - expected.x) < 1e-12 &&
                            std::abs(placed.front().start.y - expected.y) < 1e-12,
                        "Representative symbol resize/rotation must preserve exact placement mathematics");
                for (const auto& stroke : placed) {
                    require(std::isfinite(stroke.start.x) && std::isfinite(stroke.start.y) &&
                                std::isfinite(stroke.end.x) && std::isfinite(stroke.end.y),
                            "Representative symbol transforms must remain finite");
                }
            }
        }
    }
    auto bad = state;
    bad.symbols[0].symbol_id = "missing";
    rejected([&]{validate_annotation_state(bad,catalog);});
    bad = state; bad.symbols[0].id = bad.labels[0].id;
    rejected([&]{validate_annotation_state(bad,catalog);});
    bad = state; bad.overrides.push_back(bad.overrides[0]);
    rejected([&]{validate_annotation_state(bad,catalog);});
    bad = state; bad.labels[0].style.stroke_color = "red";
    rejected([&]{validate_annotation_state(bad,catalog);});
    bad = state; bad.labels[0].placement.position.x = std::numeric_limits<double>::infinity();
    rejected([&]{validate_annotation_state(bad,catalog);});
    rejected([&]{(void)placed_symbol_preview(catalog.front(),{{},0,0.001});});
    auto malformed = encoded; malformed["version"] = 1.0;
    rejected([&]{(void)decode_annotation_state(malformed,catalog);});
    malformed = encoded; malformed["version"] = 4;
    rejected([&]{(void)decode_annotation_state(malformed,catalog);});
    malformed = encoded; malformed["labels"][0]["visible"] = "false";
    rejected([&]{(void)decode_annotation_state(malformed,catalog);});
    malformed = encoded; malformed["symbols"] = nlohmann::json::object();
    rejected([&]{(void)decode_annotation_state(malformed,catalog);});
    auto invalid_catalog = catalog; invalid_catalog.push_back(catalog.front());
    rejected([&]{validate_symbol_catalog(invalid_catalog);});
    invalid_catalog = catalog; invalid_catalog[0].preview.clear();
    rejected([&]{validate_symbol_catalog(invalid_catalog);});
    invalid_catalog = catalog;
    invalid_catalog[0].preview.front().start.x = invalid_catalog[0].width_metres;
    rejected([&]{validate_symbol_catalog(invalid_catalog);});
    std::cout << "annotation_catalog_tests passed\n";
}
