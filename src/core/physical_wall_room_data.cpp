#include "sketch/physical_wall_room_data.hpp"
#include "sketch/boundary_entity.hpp"
#include "sketch/document_wall.hpp"
#include "sketch/model_phases.hpp"
#include "sketch/physical_wall_phase.hpp"
#include "sketch/physical_wall_spaces.hpp"
#include "sketch/project_organization.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <set>
#include <span>
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

std::string physical_wall_room_descriptor_digest(const Entity& entity) {
    (void)decode_physical_wall_room_descriptor(entity);
    const auto encoded=entity.extensions.at("physical_wall_room").dump();
    return sha256_hex(std::as_bytes(std::span<const char>{encoded.data(),encoded.size()}));
}

static PhysicalWallRoomDescriptor validate_physical_wall_room_repair_impl(
    const std::map<std::string,Entity,std::less<>>& source,const BoundaryGeometryEdit& edit,
    const std::set<std::string>& reviewed_owners, const PhysicalWallPhaseSelection* selection) {
    validate_boundary_geometry_edit(edit);
    if (!edit.physical_wall_room_repair) invalid("repair authority is missing");
    const auto found=source.find(edit.boundary_id);
    if (found==source.end() || !is_physical_wall_room(found->second)) invalid("repair requires a retained physical room owner");
    const auto& original=found->second;
    const auto& repair=*edit.physical_wall_room_repair;
    if (physical_wall_room_descriptor_digest(original)!=repair.expected_descriptor_digest)
        invalid("retained descriptor changed after review");
    std::set<std::string,std::less<>> inactive;
    if (selection) {
        for (const auto& registry:physical_wall_phase_states(source,*selection))
            for (const auto& member:registry.registered_entity_ids) {
                const auto state=registry.states.find(member);
                if (state==registry.states.end() || state->second==ModelPhase::demolished) inactive.insert(member);
            }
    } else {
        for (const auto& [id,entity]:source) {
            (void)id;
            if (entity.type!="model_phases") continue;
            const auto phases=ModelPhases::from_json(entity.properties.at("model"));
            const auto active=phases.active_state();
            for (const auto& member:phases.entity_ids())
                if (!active.contains(member) || active.at(member)==ModelPhase::demolished) inactive.insert(member);
        }
    }
    if (inactive.contains(original.id)) invalid("room owner is inactive in the semantic phase");
    const auto detection=[&] {
        if (!selection) return detect_physical_wall_spaces(source,repair.selected_wall_id);
        const auto selected=source.find(repair.selected_wall_id);
        if (selected==source.end() || selected->second.type!="wall") invalid("selected source is not a wall");
        if (inactive.contains(selected->first)) invalid("selected wall is inactive in the evaluated phase");
        const auto selected_context=organize_project(source).drawing_context(selected->first);
        if (!selected_context || !selected_context->complete()) invalid("selected wall needs a complete drawing context");
        const auto placed=resolve_vertical_placement(source,selected->second);
        Wall wall; std::string error;
        if (!read_document_wall(placed,{},wall,error)) invalid(error);
        validate_wall_semantics(wall);
        return detect_physical_wall_spaces(source,*selected_context,wall.elevation,*selection);
    }();
    const auto organization=organize_project(source);
    const auto context=organization.drawing_context(original.id);
    if (!context || !context->complete() || *context!=detection.context)
        invalid("reviewed destination differs from the retained room drawing context");
    if (!reviewed_owners.empty()) {
        if (!reviewed_owners.contains(original.id) || reviewed_owners.size()>2048)
            invalid("batch ownership scope must contain the repaired retained room");
        for (const auto& id:reviewed_owners) {
            const auto other=source.find(id);
            if (other==source.end() || !is_physical_wall_room(other->second) || inactive.contains(id) ||
                organization.drawing_context(id)!=context)
                invalid("batch ownership scope contains a missing, inactive or foreign room");
        }
    }
    const PhysicalWallSpace* selected=nullptr;
    for (const auto& space:detection.spaces) if (space.source_lineage==repair.reviewed_source_lineage) {
        if (selected) invalid("reviewed lineage ambiguously identifies multiple destinations");
        selected=&space;
    }
    if (!selected) invalid("reviewed destination lineage is no longer current");
    // A small analytical probe makes the existing strict containment kernel
    // prove interior membership, including all inline holes and its local
    // metre envelope. It never establishes room identity or changes sources.
    constexpr double radius=8*default_geometry_tolerance_metres;
    const auto p=repair.interior_witness;
    const Boundary probe{{{p.x-radius,p.y-radius},{p.x+radius,p.y-radius},0},
        {{p.x+radius,p.y-radius},{p.x,p.y+radius},0},{{p.x,p.y+radius},{p.x-radius,p.y-radius},0}};
    auto holes=selected->holes;holes.push_back(probe);
    if (const auto error=validate_boundary_holes(selected->boundary,holes))
        invalid("reviewed witness is not strictly inside the chosen clear component: "+*error);
    const auto replacement=decode_identified_boundary_entity(Entity{original.id,original.type,
        {{"boundary_model_version",1},{"segments",edit.replacement_segments}}});
    const auto exact_boundary=[](const Boundary& a,const Boundary& b) {
        return a.size()==b.size() && std::equal(a.begin(),a.end(),b.begin(),[](const Segment& x,const Segment& y) {
            return x.start.x==y.start.x && x.start.y==y.start.y && x.end.x==y.end.x && x.end.y==y.end.y && x.sweep_radians==y.sweep_radians;
        });
    };
    if (!exact_boundary(boundary_geometry(replacement),selected->boundary))
        invalid("replacement outer differs from independently detected physical clear geometry");
    std::size_t room_count=0;
    for (const auto& [id,entity]:source) {
        if (id==original.id || reviewed_owners.contains(id) || !is_physical_wall_room(entity) || inactive.contains(id)) continue;
        if (organization.drawing_context(id)!=context) continue;
        if (++room_count>2048) invalid("destination ownership check exceeds the room budget");
        const auto descriptor=decode_physical_wall_room_descriptor(entity);
        // Explicit evaluation may change only captured phase evidence while
        // another active owner still occupies the exact clear destination.
        // Inactive baseline owners were excluded using the evaluated state.
        if (!selection && descriptor.source_lineage!=selected->source_lineage) continue;
        if (exact_boundary(boundary_geometry(decode_identified_boundary_entity(entity)),selected->boundary) &&
            descriptor.holes.size()==selected->holes.size() &&
            std::equal(descriptor.holes.begin(),descriptor.holes.end(),selected->holes.begin(),exact_boundary))
            invalid("reviewed clear destination is already assigned to another current room: "+id);
    }
    return {repair.selected_wall_id,selected->source_lineage,selected->holes};
}

PhysicalWallRoomDescriptor validate_physical_wall_room_repair(
    const std::map<std::string,Entity,std::less<>>& source,const BoundaryGeometryEdit& edit,
    const std::set<std::string>& reviewed_owners) {
    return validate_physical_wall_room_repair_impl(source,edit,reviewed_owners,nullptr);
}

PhysicalWallRoomDescriptor validate_physical_wall_room_repair(
    const std::map<std::string,Entity,std::less<>>& source,const BoundaryGeometryEdit& edit,
    const std::set<std::string>& reviewed_owners,const PhysicalWallPhaseSelection& selection) {
    return validate_physical_wall_room_repair_impl(source,edit,reviewed_owners,&selection);
}
} // namespace sketch
