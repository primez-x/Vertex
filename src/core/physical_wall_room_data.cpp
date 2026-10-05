#include "sketch/physical_wall_room_data.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <stdexcept>
#include <utility>

namespace sketch {
namespace {
using Json=nlohmann::json;
constexpr std::size_t maximum_bytes=16*1024*1024;
constexpr std::size_t maximum_edges=16384;
constexpr std::size_t maximum_checks=65536;
[[noreturn]] void invalid(const std::string& reason) { throw std::invalid_argument("Physical wall room descriptor: "+reason); }
bool positive_integer(const Json& value) {
    return value.is_number_integer() && (value.is_number_unsigned() ? value.get<std::uint64_t>()>0 : value.get<std::int64_t>()>0);
}
void exact_keys(const Json& value,std::initializer_list<const char*> keys) {
    if (!value.is_object() || value.size()!=keys.size()) invalid("unexpected or missing fields");
    for (const auto* key:keys) if (!value.contains(key)) invalid(std::string("missing ")+key);
}
double number(const Json& value) {
    if (!value.is_number()) invalid("hole coordinates and sweeps must be numbers");
    const auto result=value.get<double>();
    if (!std::isfinite(result)) invalid("hole coordinates and sweeps must be finite");
    return result;
}
Vec2 point(const Json& value) {
    if (!value.is_array() || value.size()!=2) invalid("hole points require [x,y]");
    return {number(value[0]),number(value[1])};
}
const Json& marker(const Entity& entity) {
    if (!entity.extensions.is_object() || !entity.extensions.contains("physical_wall_room")) invalid("marker is missing");
    if (entity.type!="room_boundary") invalid("marker requires a room_boundary owner");
    const auto& value=entity.extensions.at("physical_wall_room");
    if (!value.is_object() || !value.contains("version") || !positive_integer(value.at("version")))
        invalid("version must be a positive integer");
    if (value.dump().size()>maximum_bytes) invalid("encoded descriptor exceeds 16 MiB");
    return value;
}
PhysicalWallRoomDescriptor decode(const Json& value) {
    if (value.at("version")!=1) invalid("unsupported descriptor version");
    exact_keys(value,{"version","selected_wall_id","source_lineage","holes"});
    const auto& selected=value.at("selected_wall_id");
    if (!selected.is_string()) invalid("selected wall identity must be a string");
    const auto id=selected.get<std::string>();
    if (id.empty() || id.size()>128 || !std::all_of(id.begin(),id.end(),[](unsigned char c) {
        return (c>='a' && c<='z') || (c>='A' && c<='Z') || (c>='0' && c<='9') || c=='-' || c=='_' || c=='.' || c==':';
    })) invalid("selected wall identity is invalid");
    const auto& lineage=value.at("source_lineage");
    if (!lineage.is_object() || !lineage.contains("version") || !positive_integer(lineage.at("version")) || lineage.at("version")!=1)
        invalid("a supported version-one source lineage is required");
    const auto& holes=value.at("holes");
    if (!holes.is_array() || holes.size()>maximum_edges) invalid("holes must be a bounded array");
    PhysicalWallRoomDescriptor result{id,lineage,{}};
    std::size_t edge_count=0,checks=0;
    for (const auto& encoded:holes) {
        if (!encoded.is_array() || encoded.empty() || encoded.size()>maximum_edges-edge_count)
            invalid("hole edges exceed the analytical geometry budget");
        edge_count+=encoded.size();
        const auto pairs=encoded.size()*(encoded.size()-1)/2;
        if (pairs>maximum_checks-checks) invalid("hole validation exceeds the contact budget");
        checks+=pairs;
        Boundary hole; hole.reserve(encoded.size());
        for (const auto& edge:encoded) {
            exact_keys(edge,{"start","end","sweep_radians"});
            hole.push_back({point(edge.at("start")),point(edge.at("end")),number(edge.at("sweep_radians"))});
        }
        for (std::size_t i=0;i<hole.size();++i) {
            const auto a=hole[i].end,b=hole[(i+1)%hole.size()].start;
            if (a.x!=b.x || a.y!=b.y) invalid("hole endpoints must join exactly");
        }
        if (const auto issues=validate_boundary(hole); !issues.empty()) invalid(issues.front().message);
        result.holes.push_back(std::move(hole));
    }
    return result;
}
}
bool is_physical_wall_room(const Entity& entity) noexcept {
    return entity.type=="room_boundary" && entity.extensions.is_object() && entity.extensions.contains("physical_wall_room");
}
std::optional<std::string> validate_physical_wall_room_descriptor(const Entity& entity) {
    if (!entity.extensions.is_object() || !entity.extensions.contains("physical_wall_room")) return std::nullopt;
    try {
        const auto& value=marker(entity);
        if (value.at("version")!=1) return "unsupported physical wall room descriptor version "+value.at("version").dump();
        (void)decode(value); return std::nullopt;
    } catch (const Json::exception& e) { invalid(std::string("malformed JSON: ")+e.what()); }
}
PhysicalWallRoomDescriptor decode_physical_wall_room_descriptor(const Entity& entity) {
    try { return decode(marker(entity)); }
    catch (const Json::exception& e) { invalid(std::string("malformed JSON: ")+e.what()); }
}
Json encode_physical_wall_room_descriptor(const PhysicalWallRoomDescriptor& descriptor) {
    Json holes=Json::array();
    for (const auto& hole:descriptor.holes) {
        Json boundary=Json::array();
        for (const auto& edge:hole) boundary.push_back({{"start",{edge.start.x,edge.start.y}},
            {"end",{edge.end.x,edge.end.y}},{"sweep_radians",edge.sweep_radians}});
        holes.push_back(std::move(boundary));
    }
    Json value{{"version",1},{"selected_wall_id",descriptor.selected_wall_id},
        {"source_lineage",descriptor.source_lineage},{"holes",std::move(holes)}};
    Entity owner{"physical-room-validation","room_boundary",Json::object(),false,{{"physical_wall_room",value}}};
    (void)decode_physical_wall_room_descriptor(owner); return value;
}
} // namespace sketch
