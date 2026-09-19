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
            constexpr int segments = 32;
            for (int index = 0; index < segments; ++index) {
                const double first = 2.0 * std::numbers::pi * index / segments;
                const double second = 2.0 * std::numbers::pi * (index + 1) / segments;
                line(centre_x + radius * std::cos(first), centre_y + radius * std::sin(first),
                     centre_x + radius * std::cos(second), centre_y + radius * std::sin(second));
            }
        };
        // Original code-authored plan artwork. Curves are tessellated here so
        // every preview/export consumer uses the same offline vector geometry.
        const auto arc = [&](double cx, double cy, double rx, double ry,
                             double start, double sweep, int segments) {
            for (int i = 0; i < segments; ++i) {
                const double a = start + sweep * i / segments;
                const double b = start + sweep * (i + 1) / segments;
                line(cx + rx * std::cos(a), cy + ry * std::sin(a),
                     cx + rx * std::cos(b), cy + ry * std::sin(b));
            }
        };
        const auto ellipse = [&](double cx, double cy, double rx, double ry) {
            arc(cx, cy, rx, ry, 0, 2 * std::numbers::pi, 32);
        };
        const auto rounded = [&](double left, double bottom, double right, double top,
                                 double rx, double ry) {
            line(left + rx, bottom, right - rx, bottom);
            line(right, bottom + ry, right, top - ry);
            line(right - rx, top, left + rx, top);
            line(left, top - ry, left, bottom + ry);
            const double quarter = std::numbers::pi / 2;
            arc(right - rx, bottom + ry, rx, ry, -quarter, quarter, 4);
            arc(right - rx, top - ry, rx, ry, 0, quarter, 4);
            arc(left + rx, top - ry, rx, ry, quarter, quarter, 4);
            arc(left + rx, bottom + ry, rx, ry, 2 * quarter, quarter, 4);
        };
        const std::string_view family_id = f.id;
        // Hard casework has a rectangular footprint. Soft furniture and bowls
        // supply their own silhouette, with no superimposed bounding rectangle.
        if (f.shape == 4 || (f.shape == 6 && family_id != "water-heater") || f.shape >= 8)
            rect(-1, -1, 1, 1);
        if (f.shape == 8) { line(-0.8,0,0.8,0); line(-0.8,-0.35,-0.8,0.35); line(0.8,-0.35,0.8,0.35); }
        if (f.shape == 9) { line(-0.7,0,0.7,0); line(0,-0.7,0,0.7); line(-0.5,-0.5,0.5,0.5); line(-0.5,0.5,0.5,-0.5); }
        if (f.shape == 10) { line(0,-1,0,1); line(-1,0,1,0); line(-0.7,-0.7,0.7,0.7); line(-0.7,0.7,0.7,-0.7); }
        if (f.shape == 11) { line(-1,-1,1,-1); line(1,-1,1,1); line(-1,1,1,1); line(-0.9,-0.9,0.8,0.8); }
        if (f.shape == 12) { line(-0.75,-0.75,0.75,-0.75); line(0.75,-0.75,0.75,0.75); line(0.75,0.75,-0.75,0.75); line(-0.75,0.75,-0.75,-0.75); line(-0.75,0,0.75,0); }
        if (f.shape == 14) { line(0,-0.9,0,0.9); line(0,0.9,-0.25,0.55); line(0,0.9,0.25,0.55); }
        if (f.shape == 15) { line(-0.7,0,0.7,0); line(0,-0.7,0,0.7); line(-0.5,-0.5,0.5,0.5); line(-0.5,0.5,0.5,-0.5); }
        if (f.shape == 16) { line(-0.7,-0.7,0.7,0.7); line(-0.7,0.7,0.7,-0.7); }
        if (f.shape == 17) { line(-0.8,-0.7,0.8,-0.7); line(-0.8,-0.35,0.8,-0.35); line(-0.8,0,0.8,0); line(-0.8,0.35,0.8,0.35); line(-0.8,0.7,0.8,0.7); }
        if (family_id == "range") {
            // Four burner rings and a front control rail make the appliance
            // recognizable at plan scale while staying inside the footprint.
            circle(-0.42, 0.34, 0.16);
            circle(0.42, 0.34, 0.16);
            circle(-0.42, -0.34, 0.16);
            circle(0.42, -0.34, 0.16);
            line(-0.82, -0.78, 0.82, -0.78);
            line(-0.55, -0.82, -0.55, -0.68);
            line(0.0, -0.82, 0.0, -0.68);
            line(0.55, -0.82, 0.55, -0.68);
        } else if (family_id == "refrigerator") {
            line(-0.92, 0.08, 0.92, 0.08);
            line(-0.78, 0.58, 0.78, 0.58);
            line(0.62, 0.22, 0.62, 0.48);
            line(0.62, -0.42, 0.62, -0.16);
            line(-0.78, -0.58, 0.78, -0.58);
        } else if (family_id == "dishwasher") {
            line(-0.82, 0.62, 0.82, 0.62);
            line(-0.68, 0.62, -0.68, 0.42);
            line(-0.34, 0.62, -0.34, 0.42);
            line(0.0, 0.62, 0.0, 0.42);
            line(0.34, 0.62, 0.34, 0.42);
            line(0.68, 0.62, 0.68, 0.42);
            line(-0.72, -0.58, 0.72, -0.58);
            line(-0.72, -0.38, 0.72, -0.38);
        } else if (family_id == "washer") {
            circle(0.0, -0.05, 0.54);
            circle(0.0, -0.05, 0.12);
            line(-0.82, 0.62, 0.82, 0.62);
            circle(-0.58, 0.78, 0.07);
            circle(0.0, 0.78, 0.07);
            circle(0.58, 0.78, 0.07);
        } else if (family_id == "dryer") {
            circle(0.0, -0.05, 0.54);
            line(-0.38, -0.05, 0.38, -0.05);
            line(0.0, -0.43, 0.0, 0.33);
            line(-0.82, 0.62, 0.82, 0.62);
            line(-0.62, 0.78, -0.48, 0.78);
            line(-0.18, 0.78, 0.18, 0.78);
            line(0.48, 0.78, 0.62, 0.78);
        } else if (family_id == "water-heater") {
            ellipse(0, 0, 1, 1);
            ellipse(0, 0, 0.84, 0.84);
            circle(-0.3, 0.3, 0.09);
            circle(0.3, 0.3, 0.09);
            rect(-0.22, -0.65, 0.22, -0.38);
        } else if (family_id == "wardrobe") {
            line(0.0, -0.9, 0.0, 0.9);
            line(-0.5, -0.72, -0.5, 0.72);
            line(0.5, -0.72, 0.5, 0.72);
            circle(-0.12, 0.0, 0.06);
            circle(0.12, 0.0, 0.06);
        } else if (family_id == "bookcase") {
            line(-0.9, -0.55, 0.9, -0.55);
            line(-0.9, -0.05, 0.9, -0.05);
            line(-0.9, 0.45, 0.9, 0.45);
            line(-0.72, 0.72, -0.72, -0.72);
            line(0.72, 0.72, 0.72, -0.72);
        } else if (family_id == "cabinet") {
            line(0.0, -0.78, 0.0, 0.78);
            line(-0.52, 0.0, -0.36, 0.0);
            line(0.36, 0.0, 0.52, 0.0);
            line(-0.72, 0.68, 0.72, 0.68);
            line(-0.72, -0.68, 0.72, -0.68);
        } else if (family_id == "shelving") {
            for (const double y_value : {-0.68, -0.34, 0.0, 0.34, 0.68})
                line(-0.88, y_value, 0.88, y_value);
            line(-0.78, -0.82, -0.78, 0.82);
            line(0.78, -0.82, 0.78, 0.82);
        } else if (family_id == "toilet" || family_id == "accessible-toilet") {
            const double bowl_width = family_id == "toilet" ? 0.88 : 0.44;
            rounded(-bowl_width, 0.48, bowl_width, 1, 0.09, 0.09); // cistern
            rounded(-bowl_width * 0.32, 0.68, bowl_width * 0.32, 0.8, 0.04, 0.04);
            // The tapered pan joins the tank; two ellipses describe the seat.
            polyline({{-bowl_width * 0.68, 0.48}, {-bowl_width, -0.18}});
            polyline({{bowl_width * 0.68, 0.48}, {bowl_width, -0.18}});
            arc(0, -0.18, bowl_width, 0.82, std::numbers::pi, std::numbers::pi, 16);
            ellipse(0, -0.2, bowl_width * 0.71, 0.56);
            if (family_id == "accessible-toilet") {
                rounded(-0.9, -0.72, -0.8, 0.86, 0.03, 0.03);
                rounded(-0.68, 0.9, 0.8, 1, 0.03, 0.03);
            }
        } else if (family_id == "single-bed" || family_id == "double-bed") {
            rounded(-1, -1, 1, 1, 0.08, 0.04);
            rounded(-0.94, -0.94, 0.94, 0.9, 0.1, 0.06);
            line(-0.94, 0.96, 0.94, 0.96); // headboard
            const int pillows = family_id == "single-bed" ? 1 : 2;
            for (int i = 0; i < pillows; ++i) {
                const double left = -0.8 + i * 1.6 / pillows;
                rounded(left, 0.53, left + 1.6 / pillows - 0.06, 0.82, 0.1, 0.07);
            }
            line(-0.94, 0.37, 0.94, 0.37); // turned-down duvet
            line(-0.94, 0.24, 0.94, 0.24);
            polyline({{-0.84, 0.24}, {-0.84, -0.78}, {-0.7, -0.9}});
        } else if (family_id == "sofa" || family_id == "armchair") {
            rounded(-1, -1, 1, 1, 0.12, 0.2); // upholstered carcass
            const double inner = family_id == "sofa" ? 0.79 : 0.65;
            rounded(-0.96, -0.88, -inner, 0.69, 0.05, 0.12); // arms
            rounded(inner, -0.88, 0.96, 0.69, 0.05, 0.12);
            const int cushions = family_id == "sofa" ? 3 : 1;
            const double span = 2 * (inner - 0.035) / cushions;
            for (int i = 0; i < cushions; ++i) {
                const double left = -inner + 0.035 + i * span;
                rounded(left, 0.48, left + span - 0.035, 0.89, 0.05, 0.09);
                rounded(left, -0.84, left + span - 0.035, 0.4, 0.055, 0.12);
            }
            line(-inner + 0.04, -0.94, inner - 0.04, -0.94); // front apron seam
        } else if (family_id == "chair") {
            rounded(-0.88, 0.52, 0.88, 1, 0.14, 0.1); // curved back rest
            rounded(-0.9, -0.9, 0.9, 0.4, 0.2, 0.22); // seat
            line(-0.7, 0.4, -0.7, 0.52);
            line(0.7, 0.4, 0.7, 0.52);
            line(-0.7, -0.9, -0.7, -1);
            line(0.7, -0.9, 0.7, -1);
        } else if (family_id == "bench") {
            rounded(-1, -1, 1, 1, 0.06, 0.12);
            line(-0.82, -0.34, 0.82, -0.34);
            line(-0.82, 0.0, 0.82, 0.0);
            line(-0.82, 0.34, 0.82, 0.34);
            line(-0.72, -0.78, -0.72, 0.78);
            line(0.72, -0.78, 0.72, 0.78);
        } else if (family_id == "desk") {
            rounded(-1, -1, 1, 1, 0.05, 0.1);
            rect(-0.9, -0.86, -0.48, 0.86); // drawer pedestal
            line(-0.8, -0.7, -0.58, -0.7);
            rounded(-0.2, 0.16, 0.6, 0.75, 0.04, 0.05); // desk pad
            line(0.76, 0.2, 0.76, 0.66);
        } else if (family_id == "dining-table") {
            rounded(-1, -1, 1, 1, 0.18, 0.3);
            rounded(-0.95, -0.9, 0.95, 0.9, 0.16, 0.26);
            line(0, -0.9, 0, 0.9); // extension leaf joint
        } else if (family_id == "coffee-table") {
            ellipse(0, 0, 1, 1);
            ellipse(0, 0, 0.92, 0.87);
        } else if (family_id == "side-table") {
            rounded(-1, -1, 1, 1, 0.12, 0.12);
            for (const double lx : {-0.76, 0.76})
                for (const double ly : {-0.76, 0.76})
                    rect(lx - 0.08, ly - 0.08, lx + 0.08, ly + 0.08);
        } else if (family_id == "sink" || family_id == "double-sink") {
            rounded(-1, -1, 1, 1, 0.1, 0.12);
            const int bowls = family_id == "double-sink" ? 2 : 1;
            for (int i = 0; i < bowls; ++i) {
                const double left = -0.85 + i * 1.7 / bowls;
                rounded(left, -0.82, left + 1.7 / bowls - 0.07, 0.58, 0.16, 0.22);
                circle(left + 0.85 / bowls - 0.035, -0.08, 0.08);
            }
            polyline({{-0.08, 0.78}, {-0.08, 0.38}, {0.08, 0.38}, {0.08, 0.78}});
            circle(-0.3, 0.78, 0.06);
            circle(0.3, 0.78, 0.06);
        } else if (family_id == "bathtub") {
            rounded(-1, -1, 1, 1, 0.12, 0.06);
            rounded(-0.8, -0.85, 0.8, 0.8, 0.4, 0.22);
            circle(0, 0.57, 0.07);
            line(-0.15, 0.9, 0.15, 0.9);
            line(0, 0.9, 0, 0.72);
        } else if (family_id == "shower" || family_id == "accessible-shower") {
            rect(-1, -1, 1, 1);
            rect(-0.9, -0.9, 0.9, 0.9);
            circle(0, 0, 0.1);
            for (const double corner_x : {-0.9, 0.9})
                for (const double corner_y : {-0.9, 0.9})
                    line(corner_x, corner_y, corner_x * 0.1, corner_y * 0.1);
            line(0, 0.9, 0, 0.68);
            arc(0, 0.68, 0.15, 0.1, std::numbers::pi, std::numbers::pi, 8);
            if (family_id == "accessible-shower") {
                rect(-0.84, 0.15, -0.42, 0.78); // fold-down seat
                line(0.8, -0.6, 0.8, 0.6); // grab rail
            }
        } else if (family_id == "floor-drain") {
            circle(0.0, 0.0, 0.55);
            line(-0.55, 0.0, 0.55, 0.0);
            line(0.0, -0.55, 0.0, 0.55);
            line(-0.38, -0.38, 0.38, 0.38);
            line(-0.38, 0.38, 0.38, -0.38);
        } else if (family_id == "cleanout") {
            circle(0.0, 0.0, 0.58);
            circle(0.0, 0.0, 0.25);
            line(-0.25, 0.0, 0.25, 0.0);
        } else if (family_id == "hose-bib") {
            circle(0.0, 0.0, 0.5);
            line(-0.72, 0.0, -0.28, 0.0);
            line(0.28, 0.0, 0.72, 0.0);
            line(0.0, -0.5, 0.0, 0.5);
            line(-0.25, 0.28, 0.25, 0.28);
        } else if (family_id == "water-meter") {
            rect(-0.68, -0.45, 0.68, 0.45);
            circle(0.0, 0.0, 0.28);
            line(-0.1, -0.1, 0.18, 0.16);
        } else if (family_id == "urinal" || family_id == "bidet") {
            rounded(-0.9, -1, 0.9, 1, 0.4, 0.42);
            ellipse(0, -0.18, 0.64, family_id == "urinal" ? 0.56 : 0.68);
            circle(0, -0.46, 0.07);
            if (family_id == "bidet") {
                line(0, 0.84, 0, 0.58);
                circle(-0.25, 0.74, 0.07);
                circle(0.25, 0.74, 0.07);
            } else {
                rect(-0.28, 0.68, 0.28, 0.84);
            }
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
        } else if (family_id == "checkout-counter") {
            line(-0.72, -0.2, 0.72, -0.2);
            line(-0.72, 0.2, 0.72, 0.2);
            line(-0.5, 0.2, -0.5, 0.72);
            line(0.5, 0.2, 0.5, 0.72);
            rect(-0.18, 0.28, 0.18, 0.58);
            line(-0.82, -0.72, 0.82, -0.72);
        } else if (family_id == "service-counter") {
            line(-0.82, -0.45, 0.82, -0.45);
            line(-0.82, 0.45, 0.82, 0.45);
            line(-0.62, 0.45, -0.62, 0.82);
            line(0.0, 0.45, 0.0, 0.82);
            line(0.62, 0.45, 0.62, 0.82);
        } else if (family_id == "display-case") {
            line(-0.82, -0.55, 0.82, -0.55);
            line(-0.82, 0.55, 0.82, 0.55);
            line(-0.55, -0.55, -0.55, 0.55);
            line(0.55, -0.55, 0.55, 0.55);
            line(-0.72, 0.0, 0.72, 0.0);
        } else if (family_id == "pallet-rack") {
            for (const double y_value : {-0.62, 0.0, 0.62})
                line(-0.82, y_value, 0.82, y_value);
            line(-0.72, -0.82, -0.72, 0.82);
            line(0.72, -0.82, 0.72, 0.82);
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
            {"catalog_revision", kSymbolCatalogRevision},
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
    const auto folded_category = folded(category);
    for (const auto& symbol : catalog) {
        if (!folded_category.empty() && folded(symbol.category) != folded_category) continue;
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
    json j{{"version",1},{"catalog_revision",kSymbolCatalogRevision},
           {"labels",json::array()},{"symbols",json::array()},{"overrides",json::array()}};
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
        const auto catalog_revision = j.contains("catalog_revision")
            ? j.at("catalog_revision")
            : json(kLegacySymbolCatalogRevision);
        check(catalog_revision.is_number_integer() &&
                  catalog_revision == kSymbolCatalogRevision,
              "Unsupported symbol catalog revision");
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
