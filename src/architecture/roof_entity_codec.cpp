#include "sketch/roof_entity_codec.hpp"
#include "sketch/document.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string_view>
#include <type_traits>

namespace sketch {
namespace {
using Json = nlohmann::json;
[[noreturn]] void invalid(const char* reason) { throw std::invalid_argument(reason); }

void identity(std::string_view value) {
    if (value.empty() || value.size() > 128 ||
        !std::all_of(value.begin(), value.end(), [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == ':';
        })) invalid("Roof identity is empty or invalid");
}
const Json& field(const Json& value, const char* name) {
    const auto found = value.find(name);
    if (found == value.end()) invalid("Roof required field is missing");
    return *found;
}
double number(const Json& value, const char* name) {
    const auto& raw = field(value, name);
    if (!raw.is_number()) invalid("Roof field must be a finite number");
    const double result = raw.get<double>();
    if (!std::isfinite(result)) invalid("Roof field must be a finite number");
    return result;
}
std::string text(const Json& value, const char* name) {
    const auto& raw = field(value, name);
    if (!raw.is_string()) invalid("Roof field must be a string");
    return raw.get<std::string>();
}
Vec3 position(const Json& value) {
    const auto& raw = field(value, "base_position_m");
    if (!raw.is_array() || raw.size() != 3) invalid("Roof position requires three finite coordinates");
    for (const auto& coordinate : raw)
        if (!coordinate.is_number() || !std::isfinite(coordinate.get<double>()))
            invalid("Roof position requires three finite coordinates");
    return {raw[0].get<double>(), raw[1].get<double>(), raw[2].get<double>()};
}
unsigned version(const Json& value) {
    const auto& raw = field(value, "version");
    if ((!raw.is_number_integer() && !raw.is_number_unsigned()) || (raw != 1 && raw != 2))
        invalid("Unsupported roof schema version");
    return raw.get<unsigned>();
}
std::vector<RoofOpening> openings(const Json& value, unsigned schema) {
    if (schema == 1) {
        if (value.contains("roof_openings")) invalid("Roof openings require building schema version 2");
        return {};
    }
    const auto& entries = field(value, "roof_openings");
    if (!entries.is_array() || entries.size() > 256)
        invalid("Roof openings must be an array of at most 256 entries");
    std::vector<RoofOpening> result;
    for (const auto& entry : entries) {
        if (!entry.is_object()) invalid("Roof opening must be an object");
        const auto id = text(entry, "id");
        identity(id);
        result.push_back({id, number(entry, "x_m"), number(entry, "y_m"),
            number(entry, "width_m"), number(entry, "depth_m")});
    }
    return result;
}
template<class Object>
Object decode(const Entity& entity, unsigned schema) {
    const auto& p = entity.properties;
    Object object;
    object.id = entity.id;
    object.base_position = position(p);
    object.orientation_radians = number(p, "orientation_rad");
    if constexpr (std::is_same_v<Object, SlopedRoofPanel>) object.run = number(p, "run_m");
    else object.length = number(p, "length_m");
    object.span = number(p, "span_m");
    object.rise = number(p, "rise_m");
    object.pitch_radians = number(p, "pitch_rad");
    object.overhang = number(p, "overhang_m");
    object.thickness = number(p, "thickness_m");
    object.openings = openings(p, schema);
    return object;
}
} // namespace

RoofObject decode_roof_entity(const Entity& entity) {
    identity(entity.id);
    if (entity.type != "roof" || !entity.properties.is_object())
        invalid("Roof codec requires an existing roof entity");
    const auto schema = version(entity.properties);
    const auto form = text(entity.properties, "form");
    if (form == "sloped_roof_panel") return decode<SlopedRoofPanel>(entity, schema);
    if (form == "gable_roof") return decode<GableRoof>(entity, schema);
    if (form == "hip_roof") return decode<HipRoof>(entity, schema);
    invalid("Unsupported roof building form");
}

nlohmann::json encode_roof_properties(const RoofObject& object) {
    return std::visit([](const auto& roof) {
        using Object = std::decay_t<decltype(roof)>;
        Json p{{"version", 1}, {"base_position_m", {roof.base_position.x,
            roof.base_position.y, roof.base_position.z}}, {"orientation_rad", roof.orientation_radians},
            {"span_m", roof.span}, {"rise_m", roof.rise}, {"pitch_rad", roof.pitch_radians},
            {"overhang_m", roof.overhang}, {"thickness_m", roof.thickness}};
        if constexpr (std::is_same_v<Object, SlopedRoofPanel>) {
            p["form"] = "sloped_roof_panel";
            p["run_m"] = roof.run;
        } else {
            p["form"] = std::is_same_v<Object, GableRoof> ? "gable_roof" : "hip_roof";
            p["length_m"] = roof.length;
        }
        if (!roof.openings.empty()) {
            p["version"] = 2;
            p["roof_openings"] = Json::array();
            for (const auto& opening : roof.openings) {
                identity(opening.id);
                p["roof_openings"].push_back({{"id", opening.id}, {"x_m", opening.x},
                    {"y_m", opening.y}, {"width_m", opening.width}, {"depth_m", opening.depth}});
            }
        }
        return p;
    }, object);
}

TopoDS_Shape make_roof_shape(const RoofObject& object) {
    return std::visit([](const auto& roof) -> TopoDS_Shape {
        using Object = std::decay_t<decltype(roof)>;
        if constexpr (std::is_same_v<Object, SlopedRoofPanel>) return make_sloped_roof_panel(roof);
        else if constexpr (std::is_same_v<Object, GableRoof>) return make_gable_roof(roof);
        else return make_hip_roof(roof);
    }, object);
}
} // namespace sketch
