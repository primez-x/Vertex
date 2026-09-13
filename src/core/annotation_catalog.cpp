#include "sketch/annotation_catalog.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <initializer_list>
#include <map>
#include <numbers>
#include <set>
#include <stdexcept>
#include <utility>

namespace sketch {
namespace {
using json = nlohmann::json;
void check(bool ok, const char* message) { if (!ok) throw std::invalid_argument(message); }
void point(Vec2 p) { check(std::isfinite(p.x) && std::isfinite(p.y), "Non-finite annotation point"); }
void placement(const AnnotationPlacement& p) {
    point(p.position);
    check(std::isfinite(p.rotation_radians) && std::isfinite(p.scale) && p.scale > 0,
          "Invalid annotation placement");
}
void color(const std::string& c) {
    check(c.size() == 7 && c[0] == '#' && c.find_first_not_of("0123456789abcdefABCDEF", 1) == std::string::npos,
          "Color must be #RRGGBB");
}
void style(const AnnotationStyle& s) {
    check(!s.font_family.empty() && s.font_family.size() <= 256, "Invalid font family");
    check(std::isfinite(s.text_height_metres) && s.text_height_metres > 0 &&
          std::isfinite(s.stroke_width_metres) && s.stroke_width_metres >= 0, "Invalid style dimensions");
    color(s.stroke_color); color(s.fill_color);
    check(s.fill_pattern == "none" || s.fill_pattern == "solid" || s.fill_pattern == "hatch", "Invalid fill pattern");
}
json encode_style(const AnnotationStyle& s) {
    return {{"font_family",s.font_family},{"text_height_metres",s.text_height_metres},
        {"stroke_width_metres",s.stroke_width_metres},{"stroke_color",s.stroke_color},
        {"fill_color",s.fill_color},{"fill_pattern",s.fill_pattern},{"bold",s.bold},{"italic",s.italic}};
}
AnnotationStyle decode_style(const json& j) {
    return {j.at("font_family").get<std::string>(), j.at("text_height_metres").get<double>(),
        j.at("stroke_width_metres").get<double>(), j.at("stroke_color").get<std::string>(),
        j.at("fill_color").get<std::string>(), j.at("fill_pattern").get<std::string>(),
        j.at("bold").get<bool>(),j.at("italic").get<bool>()};
}
json encode_placement(const AnnotationPlacement& p) {
    return {{"x",p.position.x},{"y",p.position.y},{"rotation_radians",p.rotation_radians},{"scale",p.scale}};
}
AnnotationPlacement decode_placement(const json& j) {
    return {{j.at("x").get<double>(),j.at("y").get<double>()},j.at("rotation_radians").get<double>(),j.at("scale").get<double>()};
}
void unique_id(std::set<std::string>& ids, const std::string& id) {
    check(!id.empty() && id.size() <= 256 && ids.insert(id).second, "Empty, oversized, or duplicate annotation ID");
}
std::string folded(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (const auto character : value) {
        result.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
    }
    return result;
}
}

std::vector<LabelTemplate> default_label_templates() {
    return {{"bedroom","rooms","Bedroom"},{"bathroom","rooms","Bathroom"},
        {"kitchen","rooms","Kitchen"},{"living-room","rooms","Living Room"},
        {"dining-room","rooms","Dining Room"},{"garage","rooms","Garage"},
        {"office","rooms","Office"},{"hall","rooms","Hall"},
        {"closet","rooms","Closet"},{"laundry","rooms","Laundry"},
        {"note","notes","Note"},{"north","orientation","North"}};
}

LabelInstance instantiate_label(const LabelTemplate& t, std::string id) {
    check(!id.empty() && !t.id.empty(), "Label IDs required");
    return {std::move(id),t.id,t.content,{},{},true};
}

std::vector<LabelTemplate> filter_label_templates(const std::vector<LabelTemplate>& labels,
    std::string_view query, std::string_view category) {
    std::vector<LabelTemplate> result;
    for (const auto& label : labels)
        if ((category.empty() || label.category == category) &&
            (query.empty() || label.content.find(query) != std::string::npos || label.id.find(query) != std::string::npos))
            result.push_back(label);
    return result;
}

std::vector<SymbolDefinition> default_symbol_catalog() {
    struct Family { const char* id; const char* category; double width; double depth; int shape; };
    // Dimension variants are stable, explicitly named parametric footprints.
    static constexpr std::array families{
        Family{"chair","furniture",0.5,0.5,1}, Family{"armchair","furniture",0.9,0.9,1},
        Family{"sofa","furniture",2.1,0.9,1}, Family{"bench","furniture",1.5,0.5,1},
        Family{"desk","furniture",1.2,0.6,2}, Family{"dining-table","furniture",1.8,0.9,2},
        Family{"coffee-table","furniture",1.0,0.6,2}, Family{"side-table","furniture",0.5,0.5,2},
        Family{"single-bed","furniture",0.9,2.0,3}, Family{"double-bed","furniture",1.4,2.0,3},
        Family{"wardrobe","storage",1.5,0.6,4}, Family{"bookcase","storage",0.9,0.3,4},
        Family{"cabinet","storage",0.6,0.6,4}, Family{"shelving","storage",1.2,0.4,4},
        Family{"sink","fixtures",0.6,0.5,5}, Family{"bathtub","fixtures",0.8,1.7,5},
        Family{"shower","fixtures",0.9,0.9,5}, Family{"toilet","fixtures",0.4,0.7,5},
        Family{"range","appliances",0.6,0.6,6}, Family{"refrigerator","appliances",0.9,0.7,6},
        Family{"dishwasher","appliances",0.6,0.6,6}, Family{"washer","appliances",0.6,0.65,6},
        Family{"dryer","appliances",0.6,0.65,6}, Family{"water-heater","appliances",0.6,0.6,6},
        Family{"floor-drain","plumbing",0.15,0.15,5}, Family{"cleanout","plumbing",0.15,0.15,5},
        Family{"hose-bib","plumbing",0.12,0.12,5}, Family{"water-meter","plumbing",0.3,0.2,5},
        Family{"urinal","fixtures",0.45,0.45,5}, Family{"double-sink","fixtures",1.2,0.5,5},
        Family{"bidet","fixtures",0.4,0.6,5}, Family{"accessible-toilet","accessibility",0.8,0.8,7},
        Family{"grab-bar","accessibility",0.9,0.1,8}, Family{"accessible-shower","accessibility",1.5,1.5,5},
        Family{"ceiling-light","lighting",0.3,0.3,9}, Family{"wall-sconce","lighting",0.2,0.15,9},
        Family{"recessed-light","lighting",0.15,0.15,9}, Family{"ceiling-fan","lighting",1.2,1.2,10},
        Family{"single-door","doors_windows",0.9,0.1,11}, Family{"double-door","doors_windows",1.8,0.1,11},
        Family{"sliding-door","doors_windows",1.8,0.12,11}, Family{"window","doors_windows",1.2,0.12,12},
        Family{"bay-window","doors_windows",2.4,0.3,12}, Family{"parking-space","site",2.7,5.5,13},
        Family{"north-arrow","site",0.3,0.3,14}, Family{"tree","site",3.0,3.0,15},
        Family{"column-symbol","structural",0.3,0.3,16}, Family{"stair-symbol","structural",1.0,2.0,17},
        Family{"checkout-counter","commercial",1.8,0.7,18}, Family{"service-counter","commercial",2.4,0.8,18},
        Family{"display-case","commercial",1.5,0.6,4}, Family{"pallet-rack","commercial",2.7,1.1,4}};
    std::vector<SymbolDefinition> result;
    for (const auto& f : families) for (int w = 0; w < 3; ++w) for (int d = 0; d < 3; ++d) {
        SymbolDefinition s;
        s.id = std::string(f.id) + "-w" + std::to_string(w+1) + "-d" + std::to_string(d+1);
        s.family = f.id; s.category = f.category;
        s.width_metres = f.width * (0.8 + 0.2*w); s.depth_metres = f.depth * (0.8 + 0.2*d);
        const double x = s.width_metres/2, y = s.depth_metres/2;
        auto line = [&](double ax, double ay, double bx, double by) { s.preview.push_back({{ax*x,ay*y},{bx*x,by*y}}); };
        const auto rect = [&](double left, double bottom, double right, double top) {
            line(left, bottom, right, bottom);
            line(right, bottom, right, top);
            line(right, top, left, top);
            line(left, top, left, bottom);
        };
        const auto polyline = [&](std::initializer_list<std::pair<double, double>> points) {
            if (points.size() < 2) return;
            auto previous = points.begin();
            for (auto current = std::next(previous); current != points.end(); ++current) {
                line(previous->first, previous->second, current->first, current->second);
                previous = current;
            }
        };
        const auto circle = [&](double centre_x, double centre_y, double radius) {
            constexpr int segments = 12;
            for (int index = 0; index < segments; ++index) {
                const double first = 2.0 * std::numbers::pi * index / segments;
                const double second = 2.0 * std::numbers::pi * (index + 1) / segments;
                line(centre_x + radius * std::cos(first), centre_y + radius * std::sin(first),
                     centre_x + radius * std::cos(second), centre_y + radius * std::sin(second));
            }
        };
        line(-1,-1,1,-1); line(1,-1,1,1); line(1,1,-1,1); line(-1,1,-1,-1);
        if (f.shape == 1) { line(-0.7,-1,-0.7,0.7); line(0.7,-1,0.7,0.7); line(-1,0.7,1,0.7); }
        if (f.shape == 2) { line(-0.8,-0.8,0.8,-0.8); line(0.8,-0.8,0.8,0.8); line(0.8,0.8,-0.8,0.8); line(-0.8,0.8,-0.8,-0.8); }
        if (f.shape == 3) { line(-1,0.6,1,0.6); line(-0.8,0.8,0.8,0.8); }
        if (f.shape == 4) { line(0,-1,0,1); line(-1,0,1,0); }
        if (f.shape == 5) { line(-0.7,-0.7,0.7,-0.7); line(0.7,-0.7,0.7,0.7); line(0.7,0.7,-0.7,0.7); line(-0.7,0.7,-0.7,-0.7); line(-0.15,0.5,0.15,0.5); }
        if (f.shape == 6) { line(-1,-1,1,1); line(-1,1,1,-1); }
        if (f.shape == 7) { line(-0.8,-0.8,0.8,-0.8); line(0.8,-0.8,0.8,0.8); line(0.8,0.8,-0.8,0.8); line(-0.8,0.8,-0.8,-0.8); line(-0.8,0,0.8,0); }
        if (f.shape == 8) { line(-0.8,0,0.8,0); line(-0.8,-0.35,-0.8,0.35); line(0.8,-0.35,0.8,0.35); }
        if (f.shape == 9) { line(-0.7,0,0.7,0); line(0,-0.7,0,0.7); line(-0.5,-0.5,0.5,0.5); line(-0.5,0.5,0.5,-0.5); }
        if (f.shape == 10) { line(0,-1,0,1); line(-1,0,1,0); line(-0.7,-0.7,0.7,0.7); line(-0.7,0.7,0.7,-0.7); }
        if (f.shape == 11) { line(-1,-1,1,-1); line(1,-1,1,1); line(-1,1,1,1); line(-0.9,-0.9,0.8,0.8); }
        if (f.shape == 12) { line(-0.75,-0.75,0.75,-0.75); line(0.75,-0.75,0.75,0.75); line(0.75,0.75,-0.75,0.75); line(-0.75,0.75,-0.75,-0.75); line(-0.75,0,0.75,0); }
        if (f.shape == 14) { line(0,-0.9,0,0.9); line(0,0.9,-0.25,0.55); line(0,0.9,0.25,0.55); }
        if (f.shape == 15) { line(-0.7,0,0.7,0); line(0,-0.7,0,0.7); line(-0.5,-0.5,0.5,0.5); line(-0.5,0.5,0.5,-0.5); }
        if (f.shape == 16) { line(-0.7,-0.7,0.7,0.7); line(-0.7,0.7,0.7,-0.7); }
        if (f.shape == 17) { line(-0.8,-0.7,0.8,-0.7); line(-0.8,-0.35,0.8,-0.35); line(-0.8,0,0.8,0); line(-0.8,0.35,0.8,0.35); line(-0.8,0.7,0.8,0.7); }
        if (f.shape == 18) { line(-0.8,-0.55,-0.8,0.55); line(-0.8,0.55,0.8,0.55); line(0.8,0.55,0.8,-0.55); line(-0.8,0,0.8,0); }
        const std::string_view family_id = f.id;
        if (family_id == "toilet") {
            rect(-0.38, -0.92, 0.38, -0.5); // tank
            polyline({{-0.38, -0.5}, {-0.28, -0.2}, {-0.2, 0.28},
                      {0.2, 0.28}, {0.28, -0.2}, {0.38, -0.5}});
            line(-0.2, 0.28, 0.2, 0.28);
        } else if (family_id == "accessible-toilet") {
            rect(-0.42, -0.9, 0.42, -0.52);
            polyline({{-0.42, -0.52}, {-0.28, -0.15}, {-0.2, 0.35},
                      {0.2, 0.35}, {0.28, -0.15}, {0.42, -0.52}});
            circle(0.0, 0.48, 0.2);
            line(-0.65, -0.82, -0.65, 0.65);
            line(-0.65, 0.65, 0.05, 0.65);
        } else if (family_id == "single-bed" || family_id == "double-bed") {
            line(-0.88, 0.54, 0.88, 0.54);
            line(-0.88, 0.2, 0.88, 0.2);
            line(-0.88, 0.54, -0.88, 0.86);
            line(0.88, 0.54, 0.88, 0.86);
            line(-0.88, -0.1, 0.88, -0.1);
        } else if (family_id == "sofa") {
            line(-0.72, 0.18, 0.72, 0.18);
            line(-0.72, -0.18, 0.72, -0.18);
            line(-0.72, 0.18, -0.72, -0.18);
            line(0.72, 0.18, 0.72, -0.18);
            line(-0.9, -0.75, -0.9, 0.55);
            line(0.9, -0.75, 0.9, 0.55);
        } else if (family_id == "chair" || family_id == "armchair") {
            line(-0.62, 0.15, 0.62, 0.15);
            line(-0.62, 0.15, -0.62, -0.65);
            line(0.62, 0.15, 0.62, -0.65);
            line(-0.82, -0.65, 0.82, -0.65);
            if (family_id == "armchair") {
                line(-0.9, -0.2, -0.9, 0.55);
                line(0.9, -0.2, 0.9, 0.55);
            }
        } else if (family_id == "desk" || family_id == "dining-table" ||
                   family_id == "coffee-table" || family_id == "side-table") {
            line(-0.78, 0.42, 0.78, 0.42);
            line(-0.78, -0.42, 0.78, -0.42);
            line(-0.6, -0.42, -0.6, -0.9);
            line(0.6, -0.42, 0.6, -0.9);
        } else if (family_id == "sink" || family_id == "double-sink") {
            const double centre = family_id == "double-sink" ? 0.45 : 0.0;
            circle(-centre, 0.0, 0.28);
            if (family_id == "double-sink") circle(centre, 0.0, 0.28);
            line(-0.2, 0.72, 0.2, 0.72);
            line(0.0, 0.72, 0.0, 0.45);
        } else if (family_id == "bathtub") {
            rect(-0.78, -0.62, 0.78, 0.62);
            polyline({{-0.58, -0.42}, {0.58, -0.42}, {0.58, 0.42}, {-0.58, 0.42}});
            circle(0.58, 0.42, 0.08);
        } else if (family_id == "shower" || family_id == "accessible-shower") {
            circle(0.0, 0.0, 0.32);
            line(-0.78, 0.78, 0.2, 0.78);
            line(0.2, 0.78, 0.2, 0.2);
            line(-0.2, -0.2, 0.2, 0.2);
            line(-0.2, 0.2, 0.2, -0.2);
        } else if (family_id == "floor-drain" || family_id == "cleanout" ||
                   family_id == "hose-bib" || family_id == "water-meter") {
            circle(0.0, 0.0, 0.55);
            line(-0.55, 0.0, 0.55, 0.0);
            line(0.0, -0.55, 0.0, 0.55);
        } else if (family_id == "grab-bar") {
            line(-0.75, -0.45, -0.75, 0.45);
            line(-0.75, 0.45, 0.75, 0.45);
            line(0.75, 0.45, 0.75, -0.45);
        } else if (family_id == "ceiling-light" || family_id == "wall-sconce" ||
                   family_id == "recessed-light") {
            circle(0.0, 0.0, 0.5);
            line(-0.35, -0.35, 0.35, 0.35);
            line(-0.35, 0.35, 0.35, -0.35);
        } else if (family_id == "ceiling-fan") {
            circle(0.0, 0.0, 0.2);
            line(0.0, 0.2, 0.0, 0.9);
            line(0.0, -0.2, 0.0, -0.9);
            line(0.2, 0.0, 0.9, 0.0);
            line(-0.2, 0.0, -0.9, 0.0);
        } else if (family_id == "single-door" || family_id == "double-door" ||
                   family_id == "sliding-door") {
            line(-0.85, -0.82, 0.85, -0.82);
            line(-0.85, 0.82, 0.85, 0.82);
            line(-0.85, -0.82, -0.85, 0.82);
            line(0.0, -0.82, 0.0, 0.82);
            line(-0.85, -0.82, 0.65, 0.55);
            if (family_id == "double-door") line(0.85, -0.82, -0.65, 0.55);
        } else if (family_id == "window" || family_id == "bay-window") {
            line(-0.72, -0.72, 0.72, 0.72);
            line(-0.72, 0.72, 0.72, -0.72);
            line(0.0, -0.85, 0.0, 0.85);
            line(-0.85, 0.0, 0.85, 0.0);
        } else if (family_id == "parking-space") {
            line(-0.75, -0.85, -0.75, 0.85);
            line(0.75, -0.85, 0.75, 0.85);
            line(-0.75, -0.7, 0.75, -0.7);
        } else if (family_id == "tree") {
            circle(0.0, 0.0, 0.72);
            line(-0.12, -0.72, -0.12, -0.95);
            line(0.12, -0.72, 0.12, -0.95);
        } else if (family_id == "column-symbol") {
            line(-0.55, -0.55, 0.55, 0.55);
            line(-0.55, 0.55, 0.55, -0.55);
        } else if (family_id == "stair-symbol") {
            for (int step = -3; step <= 3; ++step) {
                const double y_value = step * 0.22;
                line(-0.75, y_value, 0.75, y_value);
            }
            line(-0.75, -0.75, 0.75, 0.75);
        }
        result.push_back(std::move(s));
    }
    return result;
}

nlohmann::json encode_symbol_catalog_manifest(const std::vector<SymbolDefinition>& catalog) {
    validate_symbol_catalog(catalog);

    std::vector<const SymbolDefinition*> entries;
    entries.reserve(catalog.size());
    for (const auto& definition : catalog) entries.push_back(&definition);
    std::sort(entries.begin(), entries.end(), [](const auto* left, const auto* right) {
        return left->id < right->id;
    });

    std::map<std::string, std::size_t> category_counts;
    std::map<std::string, std::pair<std::string, std::size_t>> family_summary;
    nlohmann::json encoded_entries = nlohmann::json::array();
    for (const auto* definition : entries) {
        ++category_counts[definition->category];
        auto [family, inserted] = family_summary.emplace(
            definition->family, std::make_pair(definition->category, std::size_t{0}));
        if (!inserted && family->second.first != definition->category) {
            throw std::invalid_argument("Symbol family cannot span categories");
        }
        ++family->second.second;

        nlohmann::json preview = nlohmann::json::array();
        for (const auto& stroke : definition->preview) {
            preview.push_back({{"start", {{"x", stroke.start.x}, {"y", stroke.start.y}}},
                               {"end", {{"x", stroke.end.x}, {"y", stroke.end.y}}}});
        }
        encoded_entries.push_back({
            {"id", definition->id},
            {"family", definition->family},
            {"category", definition->category},
            {"width_metres", definition->width_metres},
            {"depth_metres", definition->depth_metres},
            {"anchor", {{"x", definition->anchor.x}, {"y", definition->anchor.y}}},
            {"minimum_scale", definition->minimum_scale},
            {"maximum_scale", definition->maximum_scale},
            {"preview", std::move(preview)},
        });
    }

    nlohmann::json encoded_families = nlohmann::json::array();
    for (const auto& [family, summary] : family_summary) {
        encoded_families.push_back({{"id", family}, {"category", summary.first},
                                    {"variant_count", summary.second}});
    }
    return {{"schema_version", 1},
            {"catalog_id", "vertex.symbol-catalog"},
            {"entry_count", catalog.size()},
            {"family_count", family_summary.size()},
            {"category_counts", category_counts},
            {"families", std::move(encoded_families)},
            {"entries", std::move(encoded_entries)}};
}

std::vector<SymbolDefinition> filter_symbol_catalog(
    const std::vector<SymbolDefinition>& catalog, std::string_view query,
    std::string_view category) {
    std::vector<SymbolDefinition> result;
    const auto folded_query = folded(query);
    for (const auto& symbol : catalog) {
        if (!category.empty() && symbol.category != category) continue;
        if (!folded_query.empty() && folded(symbol.id).find(folded_query) == std::string::npos &&
            folded(symbol.family).find(folded_query) == std::string::npos &&
            folded(symbol.category).find(folded_query) == std::string::npos) continue;
        result.push_back(symbol);
    }
    return result;
}

void validate_symbol_catalog(const std::vector<SymbolDefinition>& catalog) {
    std::set<std::string> ids;
    for (const auto& s : catalog) {
        unique_id(ids,s.id); point(s.anchor);
        check(!s.family.empty() && !s.category.empty(), "Missing symbol metadata");
        check(std::isfinite(s.width_metres) && s.width_metres > 0 && std::isfinite(s.depth_metres) && s.depth_metres > 0,
              "Invalid symbol dimensions");
        check(std::isfinite(s.minimum_scale) && s.minimum_scale > 0 && std::isfinite(s.maximum_scale) &&
              s.maximum_scale >= s.minimum_scale, "Invalid symbol scale limits");
        check(!s.preview.empty(), "Empty symbol preview");
        for (const auto& stroke : s.preview) {
            point(stroke.start); point(stroke.end);
            check(stroke.start.x != stroke.end.x || stroke.start.y != stroke.end.y, "Degenerate symbol stroke");
            const auto within_footprint = [&](Vec2 value) {
                return std::abs(value.x - s.anchor.x) <= s.width_metres / 2.0 + 1e-9 &&
                       std::abs(value.y - s.anchor.y) <= s.depth_metres / 2.0 + 1e-9;
            };
            check(within_footprint(stroke.start) && within_footprint(stroke.end),
                  "Symbol preview extends beyond its physical footprint");
        }
    }
}

std::vector<SymbolStroke> placed_symbol_preview(const SymbolDefinition& s, const AnnotationPlacement& p) {
    validate_symbol_catalog({s}); placement(p);
    check(p.scale >= s.minimum_scale && p.scale <= s.maximum_scale, "Symbol scale outside catalog limits");
    const double c = std::cos(p.rotation_radians), sn = std::sin(p.rotation_radians);
    auto transform = [&](Vec2 v) {
        const double x=(v.x-s.anchor.x)*p.scale, y=(v.y-s.anchor.y)*p.scale;
        Vec2 result{p.position.x+c*x-sn*y,p.position.y+sn*x+c*y}; point(result); return result;
    };
    std::vector<SymbolStroke> result;
    for (const auto& stroke : s.preview) result.push_back({transform(stroke.start),transform(stroke.end)});
    return result;
}

void validate_annotation_state(const AnnotationState& state, const std::vector<SymbolDefinition>& catalog) {
    validate_symbol_catalog(catalog);
    check(state.labels.size() <= 100000 && state.symbols.size() <= 100000 && state.overrides.size() <= 100000,
          "Annotation collection too large");
    std::set<std::string> ids;
    for (const auto& label : state.labels) {
        unique_id(ids,label.id); placement(label.placement); style(label.style);
        check(label.content.size() <= 65536 && label.template_id.size() <= 256, "Label text too large");
    }
    for (const auto& symbol : state.symbols) {
        unique_id(ids,symbol.id); style(symbol.style);
        auto it = std::find_if(catalog.begin(),catalog.end(),[&](const auto& s) { return s.id == symbol.symbol_id; });
        check(it != catalog.end(), "Unknown symbol definition");
        (void)placed_symbol_preview(*it,symbol.placement);
    }
    std::set<std::pair<std::string,std::string>> targets;
    for (const auto& o : state.overrides) {
        check(o.target_kind == "area" || o.target_kind == "object" || o.target_kind == "output_view", "Invalid presentation target kind");
        check(!o.target_id.empty() && o.target_id.size() <= 256 && targets.emplace(o.target_kind,o.target_id).second,
              "Invalid or duplicate presentation target");
        style(o.style);
    }
}

json encode_annotation_state(const AnnotationState& state, const std::vector<SymbolDefinition>& catalog) {
    validate_annotation_state(state,catalog);
    json j{{"version",1},{"labels",json::array()},{"symbols",json::array()},{"overrides",json::array()}};
    for (const auto& l : state.labels) j["labels"].push_back({{"id",l.id},{"template_id",l.template_id},{"content",l.content},
        {"style",encode_style(l.style)},{"placement",encode_placement(l.placement)},{"visible",l.visible}});
    for (const auto& s : state.symbols) j["symbols"].push_back({{"id",s.id},{"symbol_id",s.symbol_id},
        {"style",encode_style(s.style)},{"placement",encode_placement(s.placement)},{"visible",s.visible}});
    for (const auto& o : state.overrides) j["overrides"].push_back({{"target_kind",o.target_kind},{"target_id",o.target_id},
        {"style",encode_style(o.style)},{"visible",o.visible}});
    return j;
}

AnnotationState decode_annotation_state(const json& j, const std::vector<SymbolDefinition>& catalog) {
    try {
        check(j.at("version").is_number_integer() && j.at("version") == 1, "Unsupported annotation version");
        for (const char* key : {"labels","symbols","overrides"})
            check(j.at(key).is_array() && j.at(key).size() <= 100000, "Invalid annotation collection");
        AnnotationState state;
        for (const auto& l : j.at("labels")) state.labels.push_back({l.at("id").get<std::string>(),
            l.at("template_id").get<std::string>(),l.at("content").get<std::string>(),decode_style(l.at("style")),
            decode_placement(l.at("placement")),l.at("visible").get<bool>()});
        for (const auto& s : j.at("symbols")) state.symbols.push_back({s.at("id").get<std::string>(),
            s.at("symbol_id").get<std::string>(),decode_placement(s.at("placement")),decode_style(s.at("style")),s.at("visible").get<bool>()});
        for (const auto& o : j.at("overrides")) state.overrides.push_back({o.at("target_kind").get<std::string>(),
            o.at("target_id").get<std::string>(),decode_style(o.at("style")),o.at("visible").get<bool>()});
        validate_annotation_state(state,catalog); return state;
    } catch (const json::exception&) { throw std::invalid_argument("Malformed annotation JSON"); }
}
} // namespace sketch
