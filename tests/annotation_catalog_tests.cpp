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
    require(catalog.size() == 468,"Expected 52 families with nine dimension variants");
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
    require(families.size() == 52, "Every symbol family must have a stable identity");
    const auto plumbing = filter_symbol_catalog(catalog, "", "plumbing");
    require(plumbing.size() == 36, "Category filtering must return all plumbing variants");
    require(filter_symbol_catalog(catalog, "", "PLUMBING").size() == plumbing.size(),
            "Symbol category filtering must be case-insensitive for field use");
    const auto toilets = filter_symbol_catalog(catalog, "toilet");
    require(toilets.size() == 18, "Query filtering must match toilet families and variants");
    require(filter_symbol_catalog(catalog, "TOILET").size() == 18,
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
    for (std::size_t i=0;i<catalog.size();++i) {
        require(catalog[i].id == repeated[i].id && catalog[i].width_metres == repeated[i].width_metres,
                "Catalog must be deterministic");
        require(!placed_symbol_preview(catalog[i],{}).empty(),"Every catalog entry must produce preview strokes");
    }
    const auto manifest = encode_symbol_catalog_manifest(catalog);
    require(manifest.at("schema_version") == 1 && manifest.at("catalog_id") == "vertex.symbol-catalog",
            "Symbol catalog manifest must identify its schema");
    require(manifest.at("catalog_revision") == kSymbolCatalogRevision,
            "Symbol catalog manifest must identify its revision");
    require(manifest.at("entry_count") == catalog.size() && manifest.at("family_count") == families.size() &&
                manifest.at("entries").size() == catalog.size(),
            "Symbol catalog manifest must enumerate every entry and family");
    require(manifest.at("category_counts").at("fixtures") == 63 &&
                manifest.at("category_counts").at("commercial") == 36,
            "Symbol catalog manifest must retain category coverage");
    require(manifest.at("families").at(0).at("id") == "accessible-shower" &&
                manifest.at("families").at(0).at("variant_count") == 9,
            "Symbol catalog manifest families must be stable and sorted");
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
    malformed = encoded; malformed["version"] = 2;
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
