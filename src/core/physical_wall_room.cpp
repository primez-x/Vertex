#include "sketch/physical_wall_room.hpp"
#include "sketch/boundary_entity.hpp"
#include "sketch/document_wall.hpp"
#include "sketch/model_phases.hpp"
#include "sketch/physical_wall_spaces.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <optional>
#include <set>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace sketch {
namespace {
using Json=nlohmann::json;
constexpr std::size_t maximum_rooms=2048;
constexpr std::size_t maximum_detections=128;
constexpr std::size_t maximum_returned_edges=65536;
constexpr std::size_t maximum_command_bytes=16*1024*1024;
constexpr std::size_t maximum_entity_bytes=1024*1024;
constexpr std::size_t maximum_cache_charge=64*1024*1024;
std::size_t cache_charge(const PhysicalWallSpaces& value) {
    std::size_t charge=0;
    const auto add=[&](std::size_t bytes) {
        if (bytes>maximum_cache_charge-charge) throw std::invalid_argument("physical room query exceeds aggregate cached geometry/lineage budget");
        charge+=bytes;
    };
    const auto json_charge=[&](const auto& self,const Json& item)->void {
        // Account for value/container allocation overhead as well as text.
        add(128);
        if (item.is_string()) add(item.get_ref<const std::string&>().size());
        else if (item.is_object()) for (auto i=item.begin();i!=item.end();++i) { add(i.key().size()+128); self(self,i.value()); }
        else if (item.is_array()) for (const auto& member:item) self(self,member);
    };
    for (const auto& edge:value.graph.edges) {
        add(sizeof(edge)+edge.source_uses.size()*sizeof(MeasurementSourceUse));
        for (const auto& use:edge.source_uses) add(use.owner_id.size()+use.segment_id.size());
    }
    for (const auto& face:value.graph.faces)
        add(sizeof(face)+face.boundary.size()*sizeof(Segment)+face.edge_uses.size()*sizeof(MeasurementFaceEdgeUse));
    for (const auto& room:value.spaces) {
        add(sizeof(room)+room.boundary.size()*sizeof(Segment));
        for (const auto& hole:room.holes) add(sizeof(hole)+hole.size()*sizeof(Segment));
        json_charge(json_charge,room.source_lineage);
    }
    return charge;
}
[[noreturn]] void invalid(const std::string& reason) { throw std::invalid_argument("Physical wall room: "+reason); }
bool same_segment(const Segment& a,const Segment& b) {
    return a.start.x==b.start.x && a.start.y==b.start.y && a.end.x==b.end.x && a.end.y==b.end.y && a.sweep_radians==b.sweep_radians;
}
bool same_boundary(const Boundary& a,const Boundary& b) {
    return a.size()==b.size() && std::equal(a.begin(),a.end(),b.begin(),same_segment);
}
bool same_holes(const std::vector<Boundary>& a,const std::vector<Boundary>& b) {
    return a.size()==b.size() && std::equal(a.begin(),a.end(),b.begin(),same_boundary);
}
Json geometry_json(const Boundary& boundary) {
    Json result=Json::array();
    for (const auto& edge:boundary) result.push_back({{"start",{edge.start.x,edge.start.y}},
        {"end",{edge.end.x,edge.end.y}},{"sweep_radians",edge.sweep_radians}});
    return result;
}
using DetectionKey=std::tuple<std::string,std::string,std::string,std::string,std::string,double>;
struct DetectionEntry { std::optional<PhysicalWallSpaces> value; std::string diagnostic; };
class DetectionCache {
public:
    explicit DetectionCache(const DocumentSnapshot& source):source_(source) {}
    const ProjectOrganization& organization() {
        if (!organization_) organization_=organize_project(source_);
        return *organization_;
    }
    bool active(std::string_view owner_id) {
        if (!inactive_) {
            inactive_.emplace();
            for (const auto& [id,e]:source_.entities()) {
                (void)id; if (e.type!="model_phases") continue;
                const auto phases=ModelPhases::from_json(e.properties.at("model"));
                const auto states=phases.active_state();
                for (const auto& member:phases.entity_ids()) {
                    const auto state=states.find(member);
                    if (state==states.end() || state->second==ModelPhase::demolished) inactive_->insert(member);
                }
            }
        }
        return !inactive_->contains(owner_id);
    }
    DetectionKey key(std::string_view selected_id) {
        const auto found=source_.entities().find(selected_id);
        if (found==source_.entities().end() || found->second.type!="wall") invalid("selected wall no longer exists");
        if (!active(found->first)) invalid("selected wall is inactive in the semantic phase");
        const auto context=organization().drawing_context(selected_id);
        if (!context || !context->complete()) invalid("selected wall has unresolved drawing context");
        const auto resolved=resolve_vertical_placement(source_,found->second);
        Wall wall; std::string error;
        if (!read_document_wall(resolved,{},wall,error)) invalid(error);
        validate_wall_semantics(wall);
        return {context->property_id,context->building_id,context->floor_id,context->layer_id,context->level_id,wall.elevation};
    }
    void preload(std::string_view selected_id,PhysicalWallSpaces value) {
        admit(value);
        entries_.emplace(key(selected_id),DetectionEntry{std::move(value),{}});
    }
    const PhysicalWallSpaces& get(std::string_view selected_id) {
        const auto cache_key=key(selected_id);
        auto found=entries_.find(cache_key);
        if (found==entries_.end()) {
            if (entries_.size()==maximum_detections) invalid("query exceeds the physical-context detection budget");
            DetectionEntry entry;
            try { entry.value=detect_physical_wall_spaces(source_,selected_id); admit(*entry.value); }
            catch (const std::exception& e) { entry.value.reset(); entry.diagnostic=e.what(); }
            catch (...) { entry.value.reset(); entry.diagnostic="physical wall detection failed"; }
            found=entries_.emplace(cache_key,std::move(entry)).first;
        }
        if (!found->second.value) invalid(found->second.diagnostic);
        return *found->second.value;
    }
private:
    void admit(const PhysicalWallSpaces& value) {
        const auto charge=cache_charge(value);
        if (charge>maximum_cache_charge-cached_charge_) invalid("query exceeds aggregate cached geometry/lineage budget");
        cached_charge_+=charge;
    }
    std::size_t cached_charge_{};
    const DocumentSnapshot& source_;
    std::optional<ProjectOrganization> organization_;
    std::optional<std::set<std::string,std::less<>>> inactive_;
    std::map<DetectionKey,DetectionEntry> entries_;
};
const PhysicalWallSpace& matching_space(const PhysicalWallSpaces& detection,const Json& lineage) {
    const PhysicalWallSpace* match=nullptr;
    for (const auto& space:detection.spaces) if (space.source_lineage==lineage) {
        if (match) invalid("source lineage ambiguously matches multiple clear components");
        match=&space;
    }
    if (!match) invalid("physical source evidence changed; redefine this room explicitly");
    return *match;
}
PhysicalWallRoomCheck check_room(const Entity& entity,DetectionCache& cache) {
    try {
        if (!cache.active(entity.id)) invalid("room owner is inactive in the semantic phase");
        const auto descriptor=decode_physical_wall_room_descriptor(entity);
        const auto& detection=cache.get(descriptor.selected_wall_id);
        const auto context=cache.organization().drawing_context(entity.id);
        if (!context || !context->complete() || *context!=detection.context) invalid("room drawing context differs from its physical sources");
        const auto& space=matching_space(detection,descriptor.source_lineage);
        const auto geometry=boundary_geometry(decode_identified_boundary_entity(entity));
        if (!same_boundary(geometry,space.boundary)) invalid("identified room outer differs from physical clear geometry");
        if (!same_holes(descriptor.holes,space.holes)) invalid("inline room holes differ from physical clear geometry");
        if (const auto error=validate_boundary_holes(space.boundary,space.holes)) invalid(*error);
        double area=signed_area(space.boundary);
        for (const auto& hole:space.holes) area-=std::abs(signed_area(hole));
        if (!std::isfinite(area) || !(area>0)) invalid("canonical physical room area is invalid");
        return {true,{},space.boundary,space.holes,area};
    } catch (const std::exception& e) { return {false,e.what(),{},{},0}; }
    catch (...) { return {false,"physical room source validation failed",{},{},0}; }
}
std::map<std::string,PhysicalWallRoomCheck,std::less<>> checks(const DocumentSnapshot& source,DetectionCache& cache,
    const DrawingContext* target=nullptr) {
    std::map<std::string,PhysicalWallRoomCheck,std::less<>> result;
    std::size_t count=0;
    std::size_t returned_edges=0;
    for (const auto& [id,entity]:source.entities()) {
        if (!is_physical_wall_room(entity)) continue;
        if (target) {
            const auto context=cache.organization().drawing_context(id);
            if (!context || *context!=*target) continue;
        }
        if (++count>maximum_rooms) result.emplace(id,PhysicalWallRoomCheck{false,"physical room query exceeds the owner budget",{},{},0});
        else {
            auto check=check_room(entity,cache);
            std::size_t edges=check.boundary.size();
            for (const auto& hole:check.holes) edges+=hole.size();
            if (edges>maximum_returned_edges-returned_edges) check={false,"physical room query exceeds the returned geometry budget",{},{},0};
            else returned_edges+=edges;
            result.emplace(id,std::move(check));
        }
    }
    return result;
}
std::string trimmed(std::string value) {
    const auto blank=[](unsigned char c){return std::isspace(c)!=0;};
    const auto start=std::find_if_not(value.begin(),value.end(),blank);
    const auto end=std::find_if_not(value.rbegin(),value.rend(),blank).base();
    return start<end ? std::string(start,end) : std::string{};
}
}
std::map<std::string,PhysicalWallRoomCheck,std::less<>> physical_wall_room_checks(const DocumentSnapshot& source) {
    DetectionCache cache(source); return checks(source,cache);
}
ApplyEntityChanges prepare_physical_wall_rooms(const DocumentSnapshot& source,std::string_view selected_wall_id,
    const std::vector<std::size_t>& indices,std::string classification) {
    if (!source.is_editable()) invalid("captured document is read-only");
    classification=trimmed(std::move(classification));
    if (classification.empty() || classification.size()>256) invalid("choose a nonblank classification of at most 256 bytes");
    if (indices.size()>maximum_rooms) invalid("proposal exceeds the room-count budget");
    ApplyEntityChanges command{source.revision(),{}, {},"Create rooms from physical wall spaces"};
    if (indices.empty()) return command;
    auto detection=detect_physical_wall_spaces(source,selected_wall_id);
    std::set<std::size_t> selected;
    for (const auto index:indices) {
        if (index>=detection.spaces.size() || !selected.insert(index).second) invalid("indices must uniquely identify freshly detected clear components");
    }
    DetectionCache cache(source); cache.preload(selected_wall_id,std::move(detection));
    const auto& detected=cache.get(selected_wall_id);
    const auto existing=checks(source,cache,&detected.context);
    for (const auto& [id,check]:existing) {
        (void)id;
        if (!check.current && check.diagnostic.find("budget")!=std::string::npos)
            invalid("existing room ownership could not be checked within resource budgets");
    }
    std::size_t command_bytes=0;
    for (const auto index:selected) {
        const auto& space=detected.spaces[index];
        bool reused=false;
        for (const auto& [id,check]:existing) {
            if (!check.current || !same_boundary(check.boundary,space.boundary) || !same_holes(check.holes,space.holes)) continue;
            const auto descriptor=decode_physical_wall_room_descriptor(source.entities().at(id));
            if (descriptor.source_lineage==space.source_lineage) { reused=true; break; }
        }
        if (reused) continue;
        const auto descriptor=encode_physical_wall_room_descriptor({std::string(selected_wall_id),space.source_lineage,space.holes});
        const auto& context=detected.context;
        Entity room{"physical-room-"+make_stable_id(),"room_boundary",{
            {"property_id",context.property_id},{"building_id",context.building_id},{"floor_id",context.floor_id},{"layer_id",context.layer_id},
            {"name","Room "+std::to_string(index+1)},{"classification",classification},{"measurement_classification",classification},
            {"factor",1.0},{"factor_expression","1"},{"factor_numerator",1},{"factor_denominator",1},
            {"segments",geometry_json(space.boundary)}},false,{{"physical_wall_room",descriptor}}};
        room=upgrade_legacy_boundary_entity(room);
        const auto bytes=room.properties.dump().size()+room.extensions.dump().size();
        if (bytes>maximum_entity_bytes || bytes>maximum_command_bytes-command_bytes) invalid("prepared rooms exceed the encoded entity/command byte budget");
        command_bytes+=bytes;
        command.entity_changes.push_back(EntityChange::upsert(std::move(room)));
    }
    return command;
}
} // namespace sketch
