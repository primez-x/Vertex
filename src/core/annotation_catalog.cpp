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
    check(p.layer_id.size() <= 256, "Invalid annotation layer ID");
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
    auto result = json{{"x",p.position.x},{"y",p.position.y},
        {"rotation_radians",p.rotation_radians},{"scale",p.scale}};
    if (!p.layer_id.empty()) result["layer_id"] = p.layer_id;
    return result;
}
AnnotationPlacement decode_placement(const json& j) {
    return {{j.at("x").get<double>(),j.at("y").get<double>()},
        j.at("rotation_radians").get<double>(),j.at("scale").get<double>(),
        j.contains("layer_id") ? j.at("layer_id").get<std::string>() : std::string{}};
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
        Family{"display-case","commercial",1.5,0.6,4}, Family{"pallet-rack","commercial",2.7,1.1,4},

        // Expanded plan library. These are distinct real-world component
        // families; only the original compact catalog above carries nine
        // preset footprints. Expanded families ship at a standard footprint
        // and remain continuously size-adjustable after placement.
        Family{"loveseat","furniture",1.6,0.9,19}, Family{"sectional-sofa","furniture",2.8,2.0,19},
        Family{"chaise-lounge","furniture",0.9,1.7,19}, Family{"recliner","furniture",0.9,1.0,19},
        Family{"rocking-chair","furniture",0.75,0.9,20}, Family{"dining-chair","furniture",0.5,0.55,20},
        Family{"office-chair","furniture",0.65,0.65,20}, Family{"task-chair","furniture",0.6,0.6,20},
        Family{"bar-stool","furniture",0.45,0.45,20}, Family{"counter-stool","furniture",0.45,0.45,20},
        Family{"ottoman","furniture",0.75,0.6,19}, Family{"pouf","furniture",0.55,0.55,19},
        Family{"sofa-table","furniture",1.4,0.4,21}, Family{"console-table","furniture",1.2,0.4,21},
        Family{"end-table","furniture",0.55,0.55,21}, Family{"round-dining-table","furniture",1.2,1.2,21},
        Family{"square-dining-table","furniture",1.1,1.1,21}, Family{"oval-dining-table","furniture",1.8,1.0,21},
        Family{"conference-table","furniture",3.0,1.2,37}, Family{"folding-table","furniture",1.8,0.75,21},
        Family{"writing-desk","furniture",1.2,0.6,37}, Family{"executive-desk","furniture",1.8,0.9,37},
        Family{"corner-desk","furniture",1.8,1.8,37}, Family{"computer-desk","furniture",1.4,0.7,37},
        Family{"credenza","furniture",1.8,0.5,23}, Family{"dresser","furniture",1.5,0.5,23},
        Family{"nightstand","furniture",0.55,0.45,23}, Family{"chest-of-drawers","furniture",0.9,0.5,23},
        Family{"vanity-table","furniture",1.2,0.5,23}, Family{"media-console","furniture",1.8,0.45,23},
        Family{"tv-stand","furniture",1.4,0.45,23}, Family{"piano","furniture",1.5,0.65,24},
        Family{"grand-piano","furniture",1.5,1.8,24}, Family{"crib","furniture",0.75,1.35,22},
        Family{"bunk-bed","furniture",1.0,2.0,22}, Family{"queen-bed","furniture",1.52,2.03,22},
        Family{"king-bed","furniture",1.93,2.03,22}, Family{"murphy-bed","furniture",1.55,0.55,22},
        Family{"daybed","furniture",1.0,2.0,22}, Family{"futon","furniture",1.9,0.9,19},

        Family{"linen-cabinet","storage",0.6,0.45,23}, Family{"base-cabinet","storage",0.9,0.6,23},
        Family{"wall-cabinet","storage",0.9,0.35,23}, Family{"tall-cabinet","storage",0.6,0.6,23},
        Family{"corner-cabinet","storage",0.9,0.9,23}, Family{"pantry-cabinet","storage",0.75,0.6,23},
        Family{"filing-cabinet","storage",0.5,0.65,23}, Family{"lateral-file","storage",0.9,0.5,23},
        Family{"locker-single","storage",0.3,0.45,23}, Family{"locker-bank","storage",1.8,0.45,23},
        Family{"coat-rack","storage",0.5,0.5,23}, Family{"shoe-rack","storage",1.0,0.35,23},
        Family{"utility-shelving","storage",1.2,0.5,23}, Family{"closet-organizer","storage",1.8,0.55,23},
        Family{"gun-safe","storage",0.8,0.65,23},

        Family{"pedestal-sink","fixtures",0.55,0.5,26}, Family{"wall-hung-sink","fixtures",0.55,0.5,26},
        Family{"vanity-sink","fixtures",0.9,0.55,26}, Family{"corner-sink","fixtures",0.65,0.65,26},
        Family{"utility-sink","fixtures",0.65,0.6,26}, Family{"mop-sink","fixtures",0.75,0.75,26},
        Family{"kitchen-island-sink","fixtures",0.85,0.55,26}, Family{"soaking-tub","fixtures",0.85,1.8,26},
        Family{"corner-tub","fixtures",1.5,1.5,26}, Family{"whirlpool-tub","fixtures",1.0,1.9,26},
        Family{"shower-tub-combo","fixtures",0.85,1.75,26}, Family{"neo-angle-shower","fixtures",1.0,1.0,26},
        Family{"walk-in-shower","fixtures",1.5,1.0,26}, Family{"drinking-fountain","fixtures",0.45,0.45,26},
        Family{"eyewash-station","fixtures",0.6,0.55,26}, Family{"laundry-tub","fixtures",0.65,0.6,26},
        Family{"service-sink","fixtures",0.8,0.65,26}, Family{"wall-hung-toilet","fixtures",0.4,0.65,26},
        Family{"tankless-toilet","fixtures",0.4,0.65,26}, Family{"baby-changing-station","fixtures",0.9,0.55,26},

        Family{"cooktop","appliances",0.75,0.6,25}, Family{"wall-oven","appliances",0.75,0.65,25},
        Family{"double-wall-oven","appliances",0.75,0.65,25}, Family{"microwave","appliances",0.75,0.45,25},
        Family{"range-hood","appliances",0.9,0.5,25}, Family{"freezer","appliances",0.8,0.75,25},
        Family{"undercounter-refrigerator","appliances",0.6,0.6,25}, Family{"wine-cooler","appliances",0.6,0.6,25},
        Family{"ice-maker","appliances",0.45,0.6,25}, Family{"trash-compactor","appliances",0.4,0.6,25},
        Family{"garbage-disposal","appliances",0.3,0.3,25}, Family{"beverage-center","appliances",0.9,0.6,25},
        Family{"coffee-maker","appliances",0.35,0.45,25}, Family{"toaster-oven","appliances",0.45,0.4,25},
        Family{"chest-freezer","appliances",1.0,0.75,25}, Family{"upright-freezer","appliances",0.8,0.75,25},
        Family{"stack-washer-dryer","appliances",0.7,0.75,39}, Family{"laundry-center","appliances",0.75,0.8,39},
        Family{"furnace","mechanical",0.9,0.8,27}, Family{"boiler","mechanical",0.9,0.9,27},
        Family{"heat-pump","mechanical",1.0,0.45,27}, Family{"air-handler","mechanical",1.2,0.75,27},
        Family{"condenser-unit","mechanical",1.0,1.0,27}, Family{"electrical-panel","electrical",0.45,0.15,28},

        Family{"pendant-light","lighting",0.3,0.3,28}, Family{"chandelier","lighting",0.8,0.8,28},
        Family{"track-light","lighting",1.2,0.15,28}, Family{"floor-lamp","lighting",0.45,0.45,28},
        Family{"table-lamp","lighting",0.3,0.3,28}, Family{"strip-light","lighting",1.2,0.1,28},
        Family{"emergency-light","lighting",0.45,0.15,28}, Family{"exit-sign","electrical",0.35,0.1,28},
        Family{"smoke-detector","electrical",0.15,0.15,28}, Family{"carbon-monoxide-detector","electrical",0.15,0.15,28},
        Family{"occupancy-sensor","electrical",0.12,0.12,28}, Family{"thermostat","electrical",0.12,0.04,28},
        Family{"duplex-outlet","electrical",0.1,0.04,28}, Family{"quad-outlet","electrical",0.15,0.04,28},
        Family{"floor-outlet","electrical",0.12,0.12,28}, Family{"data-outlet","electrical",0.1,0.04,28},
        Family{"wall-switch","electrical",0.1,0.04,28}, Family{"dimmer-switch","electrical",0.1,0.04,28},
        Family{"junction-box","electrical",0.15,0.15,28}, Family{"speaker-ceiling","electrical",0.2,0.2,28},
        Family{"security-camera","electrical",0.25,0.15,28}, Family{"fire-alarm-pull","safety",0.12,0.05,35},
        Family{"fire-alarm-horn","safety",0.2,0.08,35}, Family{"sprinkler-head","safety",0.12,0.12,35},
        Family{"exhaust-fan","mechanical",0.35,0.35,27},

        Family{"pocket-door","doors_windows",0.9,0.12,29}, Family{"bifold-door","doors_windows",0.9,0.12,29},
        Family{"barn-door","doors_windows",1.0,0.12,29}, Family{"revolving-door","doors_windows",1.8,1.8,29},
        Family{"overhead-door","doors_windows",2.7,0.2,29}, Family{"rolling-door","doors_windows",2.4,0.2,29},
        Family{"storefront-door","doors_windows",0.9,0.12,29}, Family{"glass-door","doors_windows",0.9,0.12,29},
        Family{"french-door","doors_windows",1.5,0.12,29}, Family{"dutch-door","doors_windows",0.9,0.12,29},
        Family{"casement-window","doors_windows",1.2,0.15,29}, Family{"awning-window","doors_windows",1.0,0.15,29},
        Family{"double-hung-window","doors_windows",1.0,0.15,29}, Family{"sliding-window","doors_windows",1.5,0.15,29},
        Family{"fixed-window","doors_windows",1.2,0.15,29}, Family{"clerestory-window","doors_windows",1.5,0.15,29},
        Family{"corner-window","doors_windows",1.8,0.15,29}, Family{"skylight","doors_windows",1.0,1.2,29},
        Family{"storefront-window","doors_windows",2.4,0.15,29}, Family{"louver","doors_windows",0.9,0.15,29},

        Family{"round-column","structural",0.4,0.4,30}, Family{"square-column","structural",0.4,0.4,30},
        Family{"steel-column","structural",0.35,0.35,30}, Family{"pilaster","structural",0.5,0.25,30},
        Family{"footing","structural",0.9,0.9,30}, Family{"pier","structural",0.5,0.5,30},
        Family{"beam-symbol","structural",2.4,0.3,30}, Family{"joist-direction","structural",1.5,0.3,30},
        Family{"truss-symbol","structural",3.0,0.4,30}, Family{"elevator","circulation",1.8,1.8,30},
        Family{"escalator","circulation",1.2,4.5,30}, Family{"straight-stair","circulation",1.0,3.0,30},
        Family{"l-stair","circulation",2.0,2.5,30}, Family{"u-stair","circulation",2.2,3.0,30},
        Family{"spiral-stair","circulation",1.8,1.8,30}, Family{"ramp","circulation",1.2,4.0,30},
        Family{"ladder","circulation",0.5,1.8,30}, Family{"guardrail","circulation",2.0,0.1,30},
        Family{"handrail","circulation",2.0,0.08,30}, Family{"expansion-joint","structural",1.5,0.15,30},

        Family{"deciduous-tree","site",4.0,4.0,31}, Family{"evergreen-tree","site",3.0,3.0,31},
        Family{"shrub","site",1.0,1.0,31}, Family{"hedge","site",2.0,0.6,31},
        Family{"planter","site",1.2,0.6,31}, Family{"bollard","site",0.2,0.2,31},
        Family{"light-pole","site",0.3,0.3,31}, Family{"fire-hydrant","site",0.4,0.4,31},
        Family{"mailbox","site",0.4,0.3,31}, Family{"dumpster","site",2.0,1.2,31},
        Family{"trash-enclosure","site",3.0,2.0,31}, Family{"bike-rack","site",1.8,0.6,31},
        Family{"picnic-table","site",1.8,1.5,31}, Family{"site-bench","site",1.8,0.6,31},
        Family{"fountain","site",2.0,2.0,31}, Family{"swimming-pool","site",8.0,4.0,31},
        Family{"hot-tub","site",2.2,2.2,31}, Family{"playground-set","site",5.0,4.0,31},
        Family{"fence-gate","site",1.2,0.15,31}, Family{"property-monument","site",0.2,0.2,31},
        Family{"compact-car","site",1.75,4.1,32}, Family{"sedan","site",1.8,4.7,32},
        Family{"suv","site",1.95,4.9,32}, Family{"pickup-truck","site",2.0,5.4,32},
        Family{"van","site",2.0,5.2,32}, Family{"delivery-truck","site",2.5,7.0,32},
        Family{"motorcycle","site",0.8,2.1,32}, Family{"bicycle","site",0.65,1.8,32},
        Family{"accessible-parking","accessibility",3.6,5.5,36}, Family{"loading-zone","site",3.0,8.0,32},

        Family{"retail-shelf","commercial",1.2,0.45,33}, Family{"gondola-single","commercial",2.4,0.6,33},
        Family{"gondola-double","commercial",2.4,1.2,33}, Family{"wall-display","commercial",1.8,0.45,33},
        Family{"clothing-rack","commercial",1.2,0.6,33}, Family{"round-clothing-rack","commercial",1.2,1.2,33},
        Family{"cash-wrap","commercial",2.0,0.75,33}, Family{"kiosk","commercial",2.0,2.0,33},
        Family{"shopping-cart","commercial",0.65,1.0,33}, Family{"display-table","commercial",1.8,0.9,33},
        Family{"refrigerated-case","commercial",2.4,0.9,33}, Family{"freezer-case","commercial",2.0,0.9,33},
        Family{"produce-bin","commercial",1.2,0.9,33}, Family{"bakery-case","commercial",1.8,0.8,33},
        Family{"deli-case","commercial",2.4,0.9,33}, Family{"pharmacy-counter","commercial",2.4,0.8,33},
        Family{"reception-desk","commercial",2.4,1.2,37}, Family{"waiting-chair","commercial",0.65,0.65,20},
        Family{"salon-chair","commercial",0.75,0.75,20}, Family{"barber-chair","commercial",0.75,0.75,20},
        Family{"exam-table","commercial",0.75,1.9,34}, Family{"treatment-chair","commercial",0.8,1.8,34},
        Family{"dental-chair","commercial",0.8,1.9,34}, Family{"hospital-bed","commercial",1.0,2.2,34},
        Family{"restaurant-booth","commercial",1.8,1.5,34}, Family{"bar-counter","commercial",2.4,0.75,34},
        Family{"hostess-stand","commercial",0.6,0.6,34}, Family{"commercial-range","commercial",1.2,0.9,34},
        Family{"prep-table","commercial",1.8,0.75,34}, Family{"three-compartment-sink","commercial",2.1,0.75,34},

        Family{"wheelchair-turning-circle","accessibility",1.5,1.5,36}, Family{"transfer-space","accessibility",0.9,1.5,36},
        Family{"accessible-lavatory","accessibility",0.8,0.6,36}, Family{"accessible-bathtub","accessibility",0.9,1.8,36},
        Family{"accessible-parking-sign","accessibility",0.3,0.15,36}, Family{"tactile-warning","accessibility",0.9,0.6,36},
        Family{"platform-lift","accessibility",1.2,1.5,36}, Family{"stair-lift","accessibility",0.7,1.5,36},
        Family{"fire-extinguisher","safety",0.25,0.15,35}, Family{"fire-hose-cabinet","safety",0.75,0.2,35},
        Family{"fire-department-connection","safety",0.4,0.2,35}, Family{"first-aid-cabinet","safety",0.45,0.15,35},
        Family{"defibrillator","safety",0.4,0.15,35}, Family{"emergency-shower","safety",0.8,0.8,35},
        Family{"refuge-area","accessibility",1.5,1.5,36},

        Family{"single-workstation","furniture",1.5,0.75,37}, Family{"double-workstation","furniture",1.8,1.5,37},
        Family{"quad-workstation","furniture",3.0,3.0,37}, Family{"office-cubicle","furniture",2.0,2.0,37},
        Family{"drafting-table","furniture",1.5,0.9,37}, Family{"printer-stand","furniture",0.75,0.65,23},
        Family{"office-copier","office_equipment",0.7,0.75,24}, Family{"office-printer","office_equipment",0.5,0.45,24},
        Family{"paper-shredder","office_equipment",0.4,0.4,24}, Family{"server-rack","office_equipment",0.6,1.1,40},
        Family{"mail-sorter","office_equipment",1.2,0.45,23}, Family{"training-table","furniture",1.8,0.6,37},
        Family{"lectern","furniture",0.6,0.5,24}, Family{"whiteboard","office_equipment",1.8,0.08,24},
        Family{"floor-screen","furniture",1.8,0.15,23},

        Family{"outdoor-grill","site",1.2,0.65,25}, Family{"patio-chair","site",0.65,0.65,20},
        Family{"patio-table","site",1.2,1.2,21}, Family{"umbrella-table","site",1.5,1.5,21},
        Family{"outdoor-lounge-chair","site",0.75,1.8,19}, Family{"fire-pit","site",1.2,1.2,31},
        Family{"gazebo","site",3.5,3.5,31}, Family{"pergola","site",4.0,3.0,31},
        Family{"basketball-hoop","site",1.2,1.5,38}, Family{"goal-post","site",2.0,0.4,38},

        Family{"janitor-cart","commercial",0.65,1.2,33}, Family{"mop-bucket","commercial",0.5,0.65,39},
        Family{"vending-machine","commercial",1.0,0.85,33}, Family{"atm","commercial",0.75,0.75,33},
        Family{"turnstile","commercial",1.0,1.0,33}, Family{"queue-stanchion","commercial",0.35,0.35,33},
        Family{"parcel-locker","commercial",1.8,0.6,33}, Family{"commercial-dishwasher","commercial",0.9,0.9,25},
        Family{"walk-in-cooler","commercial",3.0,2.5,40}, Family{"walk-in-freezer","commercial",3.0,2.5,40},
        Family{"pizza-oven","commercial",1.2,1.2,34}, Family{"deep-fryer","commercial",0.5,0.8,34},
        Family{"commercial-griddle","commercial",0.9,0.8,34}, Family{"steam-table","commercial",1.8,0.75,34},
        Family{"conveyor-counter","commercial",2.4,0.7,34},

        Family{"pool-table","recreation",2.5,1.4,38}, Family{"ping-pong-table","recreation",2.74,1.525,38},
        Family{"foosball-table","recreation",1.4,0.75,38}, Family{"treadmill","recreation",0.9,2.0,38},
        Family{"elliptical-trainer","recreation",0.75,1.8,38}, Family{"exercise-bike","recreation",0.65,1.2,38},
        Family{"weight-bench","recreation",0.65,1.5,38}, Family{"rowing-machine","recreation",0.65,2.4,38},
        Family{"arcade-cabinet","recreation",0.75,0.9,38}, Family{"activity-table","recreation",1.2,0.8,38},

        Family{"shower-base","fixtures",0.9,0.9,26}, Family{"fold-down-shower-seat","accessibility",0.6,0.45,36},
        Family{"wall-mount-urinal","fixtures",0.4,0.4,26}, Family{"trough-urinal","fixtures",1.5,0.45,26},
        Family{"hand-dryer","fixtures",0.3,0.2,26}, Family{"paper-towel-dispenser","fixtures",0.35,0.15,26},
        Family{"soap-dispenser","fixtures",0.12,0.1,26}, Family{"toilet-paper-holder","fixtures",0.18,0.1,26},
        Family{"floor-sink","plumbing",0.45,0.45,27}, Family{"grease-interceptor","plumbing",1.2,0.8,27},

        Family{"wheelchair","medical",0.75,1.1,36}, Family{"gurney","medical",0.75,2.1,34},
        Family{"stretcher","medical",0.65,2.0,34}, Family{"imaging-table","medical",0.8,2.2,34},
        Family{"xray-equipment","medical",1.2,1.2,24}, Family{"lab-bench","medical",1.8,0.75,34},
        Family{"fume-hood","medical",1.5,0.9,34}, Family{"nurses-station","medical",2.4,1.2,37},
        Family{"privacy-screen","medical",1.8,0.15,23}, Family{"medical-cart","medical",0.65,0.9,33},

        Family{"fireplace","furniture",1.5,0.5,24}, Family{"gas-fireplace","furniture",1.2,0.45,24},
        Family{"wood-stove","furniture",0.75,0.75,24}, Family{"kitchen-island","furniture",1.8,0.9,21},
        Family{"breakfast-bar","furniture",2.1,0.75,21}, Family{"kitchen-cart","furniture",0.9,0.55,21},
        Family{"ironing-board","furniture",0.4,1.4,21}, Family{"sewing-table","furniture",1.2,0.6,21},
        Family{"pet-bed","furniture",0.9,0.7,19}, Family{"dog-crate","furniture",0.9,0.6,23},
        Family{"aquarium","furniture",1.2,0.45,24}, Family{"grandfather-clock","furniture",0.55,0.35,24},
        Family{"floor-mirror","furniture",0.75,0.15,24}, Family{"wall-mirror","fixtures",0.9,0.08,26},
        Family{"coat-closet","storage",1.2,0.6,23}, Family{"linen-closet","storage",0.9,0.55,23},
        Family{"radiator","mechanical",1.2,0.2,27}, Family{"baseboard-heater","mechanical",1.5,0.15,27},
        Family{"ceiling-register","mechanical",0.6,0.3,27}, Family{"floor-register","mechanical",0.4,0.2,27},
        Family{"return-air-grille","mechanical",0.6,0.3,27}, Family{"mini-split","mechanical",0.9,0.25,27},
        Family{"dish-rack","furniture",0.5,0.4,23}, Family{"recycling-bin","furniture",0.45,0.45,23},
        Family{"trash-can","furniture",0.45,0.45,23}, Family{"broom-closet","storage",0.6,0.55,23},
        Family{"vacuum-cleaner","appliances",0.35,0.45,25}, Family{"dehumidifier","mechanical",0.45,0.35,27},
        Family{"humidifier","mechanical",0.4,0.35,27}, Family{"sump-pump","mechanical",0.35,0.35,27},
        Family{"utility-pump","mechanical",0.5,0.4,27}, Family{"standby-generator","mechanical",1.2,0.75,27}};
    std::vector<SymbolDefinition> result;
    for (const auto& f : families) {
      const int preset_count = f.shape <= 18 ? 3 : 1;
      for (int wi = 0; wi < preset_count; ++wi) for (int di = 0; di < preset_count; ++di) {
        const int w = preset_count == 1 ? 1 : wi;
        const int d = preset_count == 1 ? 1 : di;
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
        if (f.shape == 4 || (f.shape == 6 && family_id != "water-heater") ||
            (f.shape >= 8 && f.shape <= 18))
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
        } else if (f.shape == 19) { // upholstered seating
            rounded(-1, -1, 1, 1, 0.16, 0.18);
            rounded(-0.9, -0.82, 0.9, 0.38, 0.12, 0.14);
            rounded(-0.9, 0.48, 0.9, 0.9, 0.12, 0.1);
            line(-0.72, -0.82, -0.72, 0.78);
            line(0.72, -0.82, 0.72, 0.78);
        } else if (f.shape == 20) { // chair or stool
            rounded(-0.74, -0.68, 0.74, 0.46, 0.18, 0.18);
            rounded(-0.72, 0.56, 0.72, 0.95, 0.16, 0.1);
            line(-0.55, -0.68, -0.72, -0.96);
            line(0.55, -0.68, 0.72, -0.96);
            circle(0, -0.08, 0.12);
        } else if (f.shape == 21) { // table
            if (family_id.find("round") != std::string_view::npos ||
                family_id.find("oval") != std::string_view::npos) {
                ellipse(0, 0, 0.96, family_id.find("oval") != std::string_view::npos ? 0.72 : 0.96);
                ellipse(0, 0, 0.84, family_id.find("oval") != std::string_view::npos ? 0.62 : 0.84);
            } else {
                rounded(-1, -1, 1, 1, 0.1, 0.1);
                rounded(-0.88, -0.82, 0.88, 0.82, 0.08, 0.08);
            }
            line(0, -0.82, 0, 0.82);
        } else if (f.shape == 22) { // bed or crib
            rounded(-1, -1, 1, 1, 0.08, 0.06);
            rounded(-0.9, -0.9, 0.9, 0.86, 0.12, 0.08);
            line(-0.9, 0.92, 0.9, 0.92);
            rounded(-0.72, 0.5, -0.04, 0.78, 0.1, 0.08);
            rounded(0.04, 0.5, 0.72, 0.78, 0.1, 0.08);
            line(-0.9, 0.34, 0.9, 0.34);
        } else if (f.shape == 23) { // storage and casework
            rounded(-1, -1, 1, 1, 0.04, 0.04);
            line(0, -0.9, 0, 0.9);
            for (const double level : {-0.55, 0.0, 0.55}) line(-0.9, level, 0.9, level);
            circle(-0.12, 0.27, 0.045);
            circle(0.12, 0.27, 0.045);
        } else if (f.shape == 24) { // office or musical equipment
            rounded(-1, -1, 1, 1, 0.12, 0.12);
            rounded(-0.82, -0.7, 0.82, 0.48, 0.08, 0.08);
            for (int key = -4; key <= 4; ++key)
                line(key * 0.16, 0.48, key * 0.16, 0.86);
        } else if (f.shape == 25) { // appliance
            rounded(-1, -1, 1, 1, 0.06, 0.06);
            line(-0.88, 0.62, 0.88, 0.62);
            circle(-0.55, 0.8, 0.08);
            circle(0.0, 0.8, 0.08);
            circle(0.55, 0.8, 0.08);
            rounded(-0.7, -0.7, 0.7, 0.38, 0.1, 0.1);
        } else if (f.shape == 26) { // plumbing fixture
            rounded(-1, -1, 1, 1, 0.16, 0.16);
            ellipse(0, -0.1, 0.72, 0.62);
            circle(0, -0.1, 0.08);
            polyline({{-0.1, 0.82}, {-0.1, 0.48}, {0.1, 0.48}, {0.1, 0.82}});
        } else if (f.shape == 27) { // mechanical equipment
            rounded(-1, -1, 1, 1, 0.06, 0.06);
            circle(0, 0, 0.62);
            circle(0, 0, 0.18);
            line(-0.8, 0, 0.8, 0);
            line(0, -0.8, 0, 0.8);
        } else if (f.shape == 28) { // lighting and electrical
            circle(0, 0, 0.72);
            line(-0.5, -0.5, 0.5, 0.5);
            line(-0.5, 0.5, 0.5, -0.5);
            rect(-0.16, -0.16, 0.16, 0.16);
        } else if (f.shape == 29) { // doors and windows
            line(-1, -0.78, 1, -0.78);
            line(-1, 0.78, 1, 0.78);
            line(-0.86, -0.78, -0.86, 0.78);
            line(0.86, -0.78, 0.86, 0.78);
            line(-0.82, -0.7, 0.66, 0.56);
            arc(-0.82, -0.7, 1.5, 1.3, 0, std::numbers::pi / 2, 10);
        } else if (f.shape == 30) { // structure and circulation
            rect(-1, -1, 1, 1);
            rect(-0.78, -0.78, 0.78, 0.78);
            line(-0.78, -0.78, 0.78, 0.78);
            line(-0.78, 0.78, 0.78, -0.78);
            for (const double level : {-0.48, -0.16, 0.16, 0.48})
                line(-0.62, level, 0.62, level);
        } else if (f.shape == 31) { // site and landscape
            ellipse(0, 0, 0.9, 0.9);
            ellipse(-0.25, 0.18, 0.48, 0.42);
            ellipse(0.3, 0.08, 0.46, 0.5);
            line(-0.12, -0.58, -0.12, -0.95);
            line(0.12, -0.58, 0.12, -0.95);
        } else if (f.shape == 32) { // vehicle or marked bay
            rounded(-0.78, -1, 0.78, 1, 0.26, 0.18);
            rounded(-0.62, -0.45, 0.62, 0.48, 0.16, 0.14);
            line(-0.78, -0.55, 0.78, -0.55);
            line(-0.78, 0.62, 0.78, 0.62);
            line(0, -0.45, 0, 0.48);
        } else if (f.shape == 33) { // retail and display
            rounded(-1, -1, 1, 1, 0.04, 0.04);
            for (const double level : {-0.62, -0.2, 0.22, 0.64})
                line(-0.9, level, 0.9, level);
            line(-0.72, -0.86, -0.72, 0.86);
            line(0.72, -0.86, 0.72, 0.86);
        } else if (f.shape == 34) { // hospitality and clinical
            rounded(-1, -1, 1, 1, 0.14, 0.12);
            rounded(-0.86, -0.8, 0.86, 0.34, 0.12, 0.12);
            rounded(-0.72, 0.48, 0.72, 0.86, 0.1, 0.08);
            line(-0.82, 0.4, 0.82, 0.4);
        } else if (f.shape == 35) { // life safety
            rounded(-0.86, -1, 0.86, 1, 0.12, 0.12);
            line(-0.58, 0, 0.58, 0);
            line(0, -0.58, 0, 0.58);
            circle(0, 0, 0.72);
        } else if (f.shape == 36) { // accessibility
            circle(-0.12, 0.52, 0.18);
            circle(0.02, -0.28, 0.58);
            polyline({{-0.08, 0.34}, {0.04, 0.02}, {0.5, 0.02}, {0.72, -0.46}});
            line(-0.04, 0.0, -0.4, -0.58);
            line(-0.4, -0.58, 0.36, -0.58);
        } else if (f.shape == 37) { // desk and meeting table
            rounded(-1, -0.72, 1, 0.72, 0.1, 0.12);
            rounded(-0.46, -0.5, 0.46, 0.5, 0.08, 0.08);
            for (const double seat : {-0.68, 0.0, 0.68}) {
                rounded(seat - 0.18, 0.78, seat + 0.18, 1.0, 0.05, 0.05);
                rounded(seat - 0.18, -1.0, seat + 0.18, -0.78, 0.05, 0.05);
            }
        } else if (f.shape == 38) { // recreation
            ellipse(0, 0, 0.96, 0.96);
            line(-0.92, 0, 0.92, 0);
            line(0, -0.92, 0, 0.92);
            circle(0, 0, 0.18);
        } else if (f.shape == 39) { // laundry and utility
            rounded(-1, -1, 1, 1, 0.05, 0.05);
            circle(0, -0.12, 0.58);
            circle(0, -0.12, 0.46);
            line(-0.86, 0.62, 0.86, 0.62);
            circle(-0.58, 0.8, 0.07);
            circle(0.58, 0.8, 0.07);
        } else if (f.shape == 40) { // warehouse
            rect(-1, -1, 1, 1);
            for (const double level : {-0.66, -0.22, 0.22, 0.66})
                line(-0.9, level, 0.9, level);
            for (const double upright : {-0.72, 0.0, 0.72})
                line(upright, -0.9, upright, 0.9);
        }
        result.push_back(std::move(s));
      }
    }
    struct SvgRecord {
        const char* id;
        const char* name;
        const char* category;
        const char* legacy_family;
        const char* path;
        double width;
        double depth;
        std::array<double, 4> view_box;
        std::array<double, 4> footprint_view_box;
        bool nominal;
    };
    static constexpr SvgRecord svg_records[] = {
#include "../../assets/symbols/architectural_v2/catalog_data.inc"
    };
    const auto legacy_count = result.size();
    result.reserve(legacy_count + std::size(svg_records));
    for (const auto& record : svg_records) {
        SymbolDefinition symbol;
        symbol.id = record.id;
        // Namespaced families avoid collisions with legacy/category duplicates.
        symbol.family = record.id;
        symbol.category = record.category;
        symbol.name = record.name;
        symbol.width_metres = record.width;
        symbol.depth_metres = record.depth;
        symbol.svg_asset = SymbolSvgAsset{record.path, record.view_box,
                                         record.footprint_view_box, record.nominal};
        const auto end = result.begin() + static_cast<std::ptrdiff_t>(legacy_count);
        const auto legacy = std::find_if(result.begin(), end, [&](const auto& entry) {
            return entry.family == record.legacy_family && entry.id.ends_with("-w2-d2");
        });
        if (legacy != end) {
            const auto rescale = [&](Vec2 point) {
                return Vec2{(point.x - legacy->anchor.x) * record.width / legacy->width_metres,
                            (point.y - legacy->anchor.y) * record.depth / legacy->depth_metres};
            };
            for (const auto& stroke : legacy->preview)
                symbol.preview.push_back({rescale(stroke.start), rescale(stroke.end)});
        } else {
            // Explicit footprint-only fallback for consumers without SVG support;
            // these strokes are not a tessellation of the supplied artwork.
            const double x = record.width / 2, y = record.depth / 2;
            symbol.preview = {{{-x, -y}, {x, -y}}, {{x, -y}, {x, y}},
                              {{x, y}, {-x, y}}, {{-x, y}, {-x, -y}}};
        }
        result.push_back(std::move(symbol));
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
        if (!definition->name.empty()) encoded_entries.back()["name"] = definition->name;
        if (definition->svg_asset) {
            const auto& asset = *definition->svg_asset;
            encoded_entries.back()["svg_asset"] = {
                {"relative_path", asset.relative_path}, {"view_box", asset.view_box},
                {"footprint_view_box", asset.footprint_view_box},
                {"dimensions_are_nominal", asset.dimensions_are_nominal}};
        }
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
            folded(symbol.name).find(folded_query) == std::string::npos &&
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
        if (s.svg_asset) {
            const auto& asset = *s.svg_asset;
            check(!s.name.empty(), "SVG symbol requires a human name");
            check(asset.relative_path.starts_with("symbols/") && asset.relative_path.ends_with(".svg") &&
                      asset.relative_path.find("..") == std::string::npos &&
                      asset.relative_path.find_first_not_of(
                          "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-/.") == std::string::npos,
                  "Invalid relative SVG asset path");
            for (const auto& box : {asset.view_box, asset.footprint_view_box}) {
                check(std::all_of(box.begin(), box.end(), [](double v) { return std::isfinite(v); }) &&
                          box[2] > 0 && box[3] > 0, "Invalid SVG coordinate bounds");
            }
        }
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
