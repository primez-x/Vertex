#include "sketch/annotation_catalog.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <set>
#include <stdexcept>

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
        result.push_back(std::move(s));
    }
    return result;
}

std::vector<SymbolDefinition> filter_symbol_catalog(
    const std::vector<SymbolDefinition>& catalog, std::string_view query,
    std::string_view category) {
    std::vector<SymbolDefinition> result;
    for (const auto& symbol : catalog) {
        if (!category.empty() && symbol.category != category) continue;
        if (!query.empty() && symbol.id.find(query) == std::string::npos &&
            symbol.family.find(query) == std::string::npos &&
            symbol.category.find(query) == std::string::npos) continue;
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
