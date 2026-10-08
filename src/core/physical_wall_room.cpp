#include "sketch/physical_wall_room.hpp"
#include "sketch/boundary_entity.hpp"
#include "sketch/document_wall.hpp"
#include "sketch/model_phases.hpp"
#include "sketch/physical_wall_spaces.hpp"
#include "sketch/architecture.hpp"
#include "sketch/document_digest.hpp"

#include <BRepAlgoAPI_Common.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <NCollection_List.hxx>
#include <Standard_Failure.hxx>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <new>
#include <optional>
#include <set>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace sketch {
std::vector<std::string> active_physical_wall_room_ids(
    const std::map<std::string,Entity,std::less<>>& entities) {
    std::set<std::string,std::less<>> inactive;
    const auto registry=std::find_if(entities.begin(),entities.end(),[](const auto& entry) {
        return entry.second.type=="model_phases";
    });
    if (registry!=entities.end()) {
        const auto saved=ModelPhases::from_json(registry->second.properties.at("model")).active_alternative();
        // Selecting the first registry's actual saved choice leaves every
        // other registry at its independently admitted actual saved choice.
        for (const auto& phase:physical_wall_phase_states(entities,{registry->first,saved}))
            for (const auto& member:phase.registered_entity_ids) {
                const auto state=phase.states.find(member);
                if (state==phase.states.end() || state->second==ModelPhase::demolished) inactive.insert(member);
            }
    }
    std::vector<std::string> result;
    for (const auto& [id,entity]:entities) {
        if (!is_physical_wall_room(entity)) continue;
        if (id.empty() || std::all_of(id.begin(),id.end(),[](unsigned char c){return std::isspace(c)!=0;}) || entity.id!=id)
            throw std::invalid_argument("Physical wall room: room key differs from its actual identity");
        if (!inactive.contains(id)) result.push_back(id);
    }
    return result;
}
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
bool same_physical_inventory_lineage(const Json& retained,const Json& fresh) {
    if (!retained.is_object() || !fresh.is_object()) return false;
    auto retained_inventory=retained;
    auto current_inventory=fresh;
    retained_inventory.erase("semantic_phases");
    current_inventory.erase("semantic_phases");
    return retained_inventory==current_inventory;
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
        const PhysicalWallSpace* current_space=nullptr;
        if (std::any_of(detection.spaces.begin(),detection.spaces.end(),[&](const auto& space) {
            return space.source_lineage==descriptor.source_lineage;
        })) current_space=&matching_space(detection,descriptor.source_lineage);
        else {
            // Current-value resolution may survive phase bookkeeping alone.
            // The inexpensive inventory comparison only bounds candidate
            // work; the strict predicate independently admits both evidences.
            for (const auto& candidate:detection.spaces) {
                if (!same_physical_inventory_lineage(descriptor.source_lineage,candidate.source_lineage) ||
                    !physical_wall_room_lineage_matches_current_inventory(entity,*context,candidate)) continue;
                if (current_space) invalid("source inventory ambiguously matches multiple clear components");
                current_space=&candidate;
            }
            if (!current_space) invalid("physical source evidence changed; redefine this room explicitly");
        }
        const auto& space=*current_space;
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
EditBoundaryGeometry prepare_physical_wall_room_repair(const DocumentSnapshot& source,
    std::string_view room_id,std::string_view selected_wall_id,Vec2 interior_witness,
    const Json& reviewed_source_lineage,std::string expected_descriptor_digest,
    const LegacyBoundaryIdentityOptions& fresh_ids,const PhysicalWallRoomRepairReferences& references) {
    if (!source.is_editable()) invalid("captured document is read-only");
    const auto owner=source.entities().find(room_id);
    if (owner==source.entities().end() || !is_physical_wall_room(owner->second))
        invalid("repair requires a retained source-bound room");
    const auto detection=detect_physical_wall_spaces(source,selected_wall_id);
    const auto& destination=matching_space(detection,reviewed_source_lineage);
    if (fresh_ids.segment_ids.size()!=destination.boundary.size() || fresh_ids.vertex_ids.size()!=destination.boundary.size())
        invalid("repair requires an explicit fresh segment and vertex identity for every destination edge");
    IdentifiedBoundary replacement{std::string(room_id),owner->second.type,{}};
    for (std::size_t i=0;i<destination.boundary.size();++i)
        replacement.segments.push_back({fresh_ids.segment_ids[i],fresh_ids.vertex_ids[i],
            fresh_ids.vertex_ids[(i+1)%destination.boundary.size()],destination.boundary[i]});
    BoundaryGeometryEdit edit;
    edit.boundary_id=std::string(room_id);edit.target_id=edit.boundary_id;
    edit.kind=BoundaryGeometryEditKind::redefine_boundary;edit.fresh_topology=true;
    edit.replacement_segments=encode_identified_boundary_entity(replacement).properties.at("segments");
    edit.replacement_child_mapping=references.child_mapping;
    edit.replacement_removed_reference_ids=references.removed_reference_ids;
    edit.replacement_dimension_ids=references.replacement_dimension_ids;
    edit.allow_automatic_angle_removal=references.allow_automatic_angle_removal;
    edit.physical_wall_room_repair=PhysicalWallRoomRepairIntent{std::string(selected_wall_id),interior_witness,
        reviewed_source_lineage,std::move(expected_descriptor_digest)};
    EditBoundaryGeometry command{source.revision(),std::move(edit)};
    // Preparation validates actual dependency decisions and lifetime admission
    // through the same guarded command that Apply will independently replay.
    (void)Document::preview_command(source,Command{command});
    return command;
}

namespace {
constexpr std::size_t maximum_correspondence_pairs=65536;
struct CorrespondenceBudgetFailure:std::invalid_argument {
    explicit CorrespondenceBudgetFailure(const char* reason):std::invalid_argument(reason) {}
};
struct CorrespondenceBudget {
    std::size_t bytes{},edges{},contacts{};
    void charge(std::size_t value) {
        if (value>maximum_cache_charge-bytes) throw CorrespondenceBudgetFailure("correspondence exceeds aggregate evidence budget");
        bytes+=value;
    }
    void geometry_edge_count(std::size_t count) {
        if (count>maximum_returned_edges) throw CorrespondenceBudgetFailure("correspondence region exceeds the edge budget");
        if (count>maximum_returned_edges-edges) throw CorrespondenceBudgetFailure("correspondence exceeds aggregate returned-edge budget");
        edges+=count;
        const auto pairs=count ? count*(count-1)/2 : 0;
        // Global and local geometry are both validated before Boolean work.
        if (pairs>(maximum_correspondence_pairs-contacts)/2)
            throw CorrespondenceBudgetFailure("correspondence exceeds aggregate geometry-contact budget");
        contacts+=pairs*2;
        charge(count*sizeof(Segment));
    }
    void geometry(const Boundary& outer,const std::vector<Boundary>& holes) {
        std::size_t count=outer.size();
        if (count>maximum_returned_edges) throw CorrespondenceBudgetFailure("correspondence region exceeds the edge budget");
        for (const auto& hole:holes) {
            if (hole.size()>maximum_returned_edges-count) throw CorrespondenceBudgetFailure("correspondence region exceeds the edge budget");
            count+=hole.size();
        }
        geometry_edge_count(count);
    }
};
double correspondence_tolerance(double area) { return std::max(1e-9,std::abs(area)*1e-9); }
Boundary local_boundary(Boundary value,Vec2 origin) {
    for (auto& edge:value) for (auto* p:{&edge.start,&edge.end}) { p->x-=origin.x; p->y-=origin.y; }
    return value;
}
template<class Operation> TopoDS_Shape correspondence_boolean(const TopoDS_Shape& first,const TopoDS_Shape& second) {
    Operation operation;
    NCollection_List<TopoDS_Shape> arguments,tools;
    arguments.Append(first); tools.Append(second);
    operation.SetArguments(arguments); operation.SetTools(tools); operation.SetRunParallel(false);
    operation.Build();
    if (!operation.IsDone() || operation.HasErrors()) invalid("analytical room comparison could not be resolved");
    auto shape=operation.Shape();
    if (!shape.IsNull() && !BRepCheck_Analyzer(shape).IsValid()) invalid("analytical room comparison produced invalid geometry");
    return shape;
}
struct CorrespondenceRegion { std::optional<TopoDS_Shape> shape; double area{}; };
CorrespondenceRegion correspondence_region(const Boundary& outer,const std::vector<Boundary>& holes,Vec2 origin) {
    if (const auto error=validate_boundary_holes(outer,holes)) invalid(*error);
    const auto local=local_boundary(outer,origin);
    std::vector<Boundary> local_holes;
    for (const auto& hole:holes) local_holes.push_back(local_boundary(hole,origin));
    if (const auto error=validate_boundary_holes(local,local_holes)) invalid(*error);
    const auto make=[&](const Boundary& boundary) {
        auto face=make_planar_face(boundary);
        const auto area=std::abs(signed_area(boundary));
        if (!std::isfinite(area) || !(area>default_geometry_tolerance_metres*default_geometry_tolerance_metres) ||
            std::abs(surface_area(face)-area)>correspondence_tolerance(area))
            invalid("analytical room outline disagrees with its planar face");
        return face;
    };
    TopoDS_Shape shape=make(local);
    double area=std::abs(signed_area(local));
    for (const auto& hole:local_holes) {
        area-=std::abs(signed_area(hole));
        shape=correspondence_boolean<BRepAlgoAPI_Cut>(shape,make(hole));
    }
    double represented=std::abs(signed_area(outer));
    for (const auto& hole:holes) represented-=std::abs(signed_area(hole));
    if (!std::isfinite(area) || !(area>default_geometry_tolerance_metres*default_geometry_tolerance_metres) ||
        std::abs(represented-area)>correspondence_tolerance(area) ||
        std::abs(surface_area(shape)-area)>correspondence_tolerance(area))
        invalid("analytical room region disagrees with its holes or planar subtraction");
    return {std::move(shape),represented};
}
auto source_use_key(const MeasurementSourceUse& use) {
    return std::tuple{use.owner_id,use.segment_id,use.parameter_start,use.parameter_end,use.reversed};
}
struct CorrespondenceLineage { double elevation{}; std::vector<MeasurementSourceUse> uses; std::set<std::string> owners; };
void lineage_keys(const Json& value,std::initializer_list<const char*> keys) {
    if (!value.is_object() || value.size()!=keys.size()) invalid("retained source evidence has malformed fields");
    for (const auto* key:keys) if (!value.contains(key)) invalid("retained source evidence is missing a captured field");
}
double lineage_number(const Json& value) {
    if (!value.is_number()) invalid("retained source evidence contains a malformed number");
    const auto number=value.get<double>();
    if (!std::isfinite(number)) invalid("retained source evidence contains a nonfinite number");
    return number;
}
std::string lineage_id(const Json& value) {
    if (!value.is_string()) invalid("retained source evidence contains a malformed identity");
    auto id=value.get<std::string>();
    if (id.empty() || id.size()>128) invalid("retained source evidence contains an invalid identity");
    return id;
}
CorrespondenceLineage correspondence_lineage(const Json& lineage,std::string_view selected_wall_id,
    const DrawingContext& context,CorrespondenceBudget& budget) {
    lineage_keys(lineage,{"version","basis","context","physical_sources","semantic_phases","outer","holes","component_index"});
    if (!lineage.at("version").is_number_integer() || lineage.at("version")!=1 || lineage.at("basis")!="physical_wall_clear")
        invalid("retained room has unsupported source lineage");
    const auto& recorded_context=lineage.at("context");
    if (recorded_context!=Json{{"property_id",context.property_id},{"building_id",context.building_id},
        {"floor_id",context.floor_id},{"layer_id",context.layer_id},{"level_id",context.level_id}})
        invalid("retained source lineage belongs to a different drawing context");
    if (!lineage.at("component_index").is_number_integer() || lineage.at("component_index")<0)
        invalid("retained component index is invalid");
    const auto& sources=lineage.at("physical_sources");
    if (!sources.is_array() || sources.empty()) invalid("retained physical source inventory is missing");
    if (sources.size()>maximum_rooms) throw CorrespondenceBudgetFailure("retained physical source inventory exceeds its budget");
    std::set<std::pair<std::string,std::string>> source_ids;
    CorrespondenceLineage result; bool selected=false;
    double minimum_plane=std::numeric_limits<double>::infinity(),maximum_plane=-minimum_plane;
    std::optional<double> level_datum;
    for (const auto& item:sources) {
        lineage_keys(item,{"owner_id","segment_id","baseline","thickness_m","source_elevation_m","effective_elevation_m","source_context","vertical_placement"});
        const auto owner=lineage_id(item.at("owner_id")),segment=lineage_id(item.at("segment_id"));
        if (segment!="baseline")
            invalid("retained physical source identity/plane is invalid");
        const auto elevation=lineage_number(item.at("effective_elevation_m"));
        const auto source_elevation=lineage_number(item.at("source_elevation_m"));
        if (!source_ids.emplace(owner,segment).second)
            invalid("retained physical source identity/plane is ambiguous");
        result.owners.insert(owner);
        minimum_plane=std::min(minimum_plane,elevation); maximum_plane=std::max(maximum_plane,elevation);
        const auto& baseline=item.at("baseline"); lineage_keys(baseline,{"start","end","sweep_radians"});
        const auto point=[&](const Json& encoded) {
            if (!encoded.is_array() || encoded.size()!=2) invalid("retained captured baseline point is malformed");
            return Vec2{lineage_number(encoded.at(0)),lineage_number(encoded.at(1))};
        };
        const Segment captured{point(baseline.at("start")),point(baseline.at("end")),lineage_number(baseline.at("sweep_radians"))};
        const auto thickness=lineage_number(item.at("thickness_m"));
        if (!(thickness>default_geometry_tolerance_metres) || !std::isfinite(thickness/2) ||
            !(segment_length(captured)>default_geometry_tolerance_metres)) invalid("retained captured wall geometry is invalid");
        (void)segment_bounds(captured);
        if (captured.sweep_radians!=0) {
            const auto radius=std::hypot(captured.end.x-captured.start.x,captured.end.y-captured.start.y)/
                (2*std::abs(std::sin(captured.sweep_radians/2)));
            if (!std::isfinite(radius) || !(radius-thickness/2>default_geometry_tolerance_metres))
                invalid("retained captured wall thickness collapses its arc");
        }
        const auto& source_context=item.at("source_context");
        lineage_keys(source_context,{"property_id","building_id","floor_id","layer_id"});
        for (const auto* key:{"property_id","building_id","floor_id","layer_id"})
            if (!source_context.at(key).is_null() && lineage_id(source_context.at(key))!=recorded_context.at(key).get<std::string>())
                invalid("retained captured wall context conflicts with its resolved context");
        const auto& placement=item.at("vertical_placement");
        bool absolute=true;
        if (!placement.is_null()) {
            lineage_keys(placement,{"version","mode","offset_m"});
            if (!placement.at("version").is_number_integer() || placement.at("version")!=1 || !placement.at("mode").is_string())
                invalid("retained vertical placement is malformed");
            const auto mode=placement.at("mode").get<std::string>(); const auto offset=lineage_number(placement.at("offset_m"));
            if ((mode!="level" && mode!="absolute") || std::abs(offset)>1e9) invalid("retained vertical placement is invalid");
            absolute=mode=="absolute";
            if (!absolute) {
                if (context.level_id.empty()) invalid("retained level placement has no captured level context");
                const auto datum=elevation-source_elevation-offset;
                if (!std::isfinite(datum) || (level_datum && std::abs(*level_datum-datum)>default_geometry_tolerance_metres))
                    invalid("retained level placements disagree on their captured datum");
                level_datum=datum;
            }
        }
        if (absolute && elevation!=source_elevation) invalid("retained absolute source elevation is inconsistent");
        if (owner==selected_wall_id) { result.elevation=elevation; selected=true; }
    }
    if (!selected) invalid("retained selected wall is absent from its source evidence");
    if (maximum_plane-minimum_plane>default_geometry_tolerance_metres) invalid("retained source planes are ambiguous");
    const auto& phases=lineage.at("semantic_phases");
    if (!phases.is_array()) invalid("retained semantic phase evidence is malformed");
    std::set<std::string> phase_ids; std::size_t phase_entries=0;
    for (const auto& phase:phases) {
        if (++phase_entries>maximum_correspondence_pairs) throw CorrespondenceBudgetFailure("retained semantic phase evidence exceeds its budget");
        lineage_keys(phase,{"id","active_alternative","owners"});
        if (!phase_ids.insert(lineage_id(phase.at("id"))).second) invalid("retained semantic phase registry is duplicated");
        const auto& alternative=phase.at("active_alternative");
        if (!alternative.is_null() && (!alternative.is_string() || alternative.get_ref<const std::string&>().empty()))
            invalid("retained active alternative is malformed");
        const auto& owners=phase.at("owners");
        if (!owners.is_array() || owners.empty()) invalid("retained semantic phase owners are missing");
        std::set<std::string> phase_owners;
        for (const auto& owner:owners) {
            if (++phase_entries>maximum_correspondence_pairs) throw CorrespondenceBudgetFailure("retained semantic phase evidence exceeds its budget");
            lineage_keys(owner,{"owner_id","active_state"}); const auto id=lineage_id(owner.at("owner_id"));
            if (!phase_owners.insert(id).second || !owner.at("active_state").is_string()) invalid("retained semantic phase owner is malformed");
            const auto state=owner.at("active_state").get<std::string>();
            if (state!="existing" && state!="proposed" && state!="demolished" && state!="absent") invalid("retained semantic phase state is invalid");
            if ((state=="demolished" || state=="absent") && source_ids.contains({id,"baseline"}))
                invalid("retained physical source is inactive in its captured semantic phase");
        }
    }
    const auto append=[&](const Json& face,bool hole) {
        lineage_keys(face,{"baseline_face_index","edges"});
        if (!face.is_object() || !face.at("baseline_face_index").is_number_integer() || face.at("baseline_face_index")<0 ||
            !face.at("edges").is_array() || face.at("edges").empty()) invalid("retained baseline-face lineage is malformed");
        std::set<std::size_t> edge_indices;
        for (const auto& edge:face.at("edges")) {
            lineage_keys(edge,{"edge_index","source_uses"});
            if (!edge.is_object() || !edge.at("edge_index").is_number_integer() || edge.at("edge_index")<0 ||
                !edge.at("source_uses").is_array() || edge.at("source_uses").empty())
                invalid("retained boundary-source lineage is malformed");
            if (!edge_indices.insert(edge.at("edge_index").get<std::size_t>()).second) invalid("retained boundary-source edge is duplicated");
            if (edge.at("source_uses").size()>maximum_correspondence_pairs-result.uses.size())
                throw CorrespondenceBudgetFailure("room source uses exceed the correspondence budget");
            for (const auto& encoded:edge.at("source_uses")) {
                lineage_keys(encoded,{"owner_id","segment_id","parameter_start","parameter_end","reversed"});
                MeasurementSourceUse use;
                use.owner_id=encoded.at("owner_id").get<std::string>();
                use.segment_id=encoded.at("segment_id").get<std::string>();
                if (!encoded.at("parameter_start").is_number() || !encoded.at("parameter_end").is_number() ||
                    !encoded.at("reversed").is_boolean()) invalid("retained source interval is malformed");
                use.parameter_start=encoded.at("parameter_start").get<double>();
                use.parameter_end=encoded.at("parameter_end").get<double>();
                // Hole lineage records the child's CCW baseline face, while
                // the parent's clear-region hole traverses it clockwise.
                use.reversed=encoded.at("reversed").get<bool>()!=hole;
                if (!std::isfinite(use.parameter_start) || !std::isfinite(use.parameter_end) || use.parameter_start<0 ||
                    use.parameter_end>1 || !(use.parameter_start<use.parameter_end) || !source_ids.contains({use.owner_id,use.segment_id}))
                    invalid("retained boundary interval has no valid physical owner");
                budget.charge(sizeof(use)+use.owner_id.size()+use.segment_id.size());
                result.uses.push_back(std::move(use));
            }
        }
    };
    append(lineage.at("outer"),false);
    if (!lineage.at("holes").is_array()) invalid("retained hole lineage is malformed");
    for (const auto& hole:lineage.at("holes")) append(hole,true);
    std::sort(result.uses.begin(),result.uses.end(),[](const auto& a,const auto& b){return source_use_key(a)<source_use_key(b);});
    result.uses.erase(std::unique(result.uses.begin(),result.uses.end(),[](const auto& a,const auto& b){
        return source_use_key(a)==source_use_key(b);
    }),result.uses.end());
    return result;
}
std::vector<MeasurementSourceUse> surviving_sources(const CorrespondenceLineage& old,const CorrespondenceLineage& fresh,
    CorrespondenceBudget& budget,std::size_t& comparisons) {
    std::vector<MeasurementSourceUse> result;
    for (const auto& a:old.uses) {
        const auto key=std::pair{a.owner_id,a.segment_id};
        auto start_use=std::lower_bound(fresh.uses.begin(),fresh.uses.end(),key,[](const auto& use,const auto& identity) {
            return std::pair{use.owner_id,use.segment_id}<identity;
        });
        for (auto b=start_use;b!=fresh.uses.end() && std::pair{b->owner_id,b->segment_id}==key;++b) {
            if (++comparisons>maximum_correspondence_pairs) throw CorrespondenceBudgetFailure("correspondence exceeds aggregate source-interval comparison budget");
            if (a.reversed!=b->reversed) continue;
            const auto start=std::max(a.parameter_start,b->parameter_start),end=std::min(a.parameter_end,b->parameter_end);
            if (!(end>start)) continue;
            MeasurementSourceUse use{a.owner_id,a.segment_id,start,end,a.reversed};
            budget.charge(sizeof(use)+use.owner_id.size()+use.segment_id.size());
            result.push_back(std::move(use));
        }
    }
    std::sort(result.begin(),result.end(),[](const auto& a,const auto& b){return source_use_key(a)<source_use_key(b);});
    result.erase(std::unique(result.begin(),result.end(),[](const auto& a,const auto& b){return source_use_key(a)==source_use_key(b);}),result.end());
    return result;
}
void classify_correspondence(PhysicalWallRoomCorrespondenceReport& report) {
    using Kind=PhysicalWallRoomCorrespondenceKind;
    std::map<std::string,std::size_t,std::less<>> owner_indices;
    for (std::size_t i=0;i<report.retained.size();++i) owner_indices.emplace(report.retained[i].room.id,i);
    std::vector<std::vector<std::size_t>> old_links(report.retained.size()),fresh_links(report.fresh.size());
    for (std::size_t i=0;i<report.overlaps.size();++i) {
        const auto& link=report.overlaps[i]; const auto old=owner_indices.at(link.room_id);
        old_links[old].push_back(i); fresh_links.at(link.candidate_index).push_back(i);
        report.retained[old].candidate_indices.push_back(link.candidate_index);
        report.fresh.at(link.candidate_index).retained_room_ids.push_back(link.room_id);
    }
    std::vector<bool> seen_old(report.retained.size()),seen_fresh(report.fresh.size());
    for (std::size_t seed=0;seed<report.retained.size();++seed) {
        if (seen_old[seed]) continue;
        std::vector<std::size_t> owners{seed},candidates;
        seen_old[seed]=true; bool uncertain=!report.retained[seed].diagnostic.empty();
        for (std::size_t cursor=0;cursor<owners.size();++cursor) {
            const auto old=owners[cursor]; uncertain=uncertain || !report.retained[old].diagnostic.empty();
            for (const auto link_index:old_links[old]) {
                const auto& link=report.overlaps[link_index]; uncertain=uncertain || !link.reliable;
                const auto fresh=link.candidate_index;
                if (seen_fresh[fresh]) continue;
                seen_fresh[fresh]=true; candidates.push_back(fresh);
                uncertain=uncertain || !report.fresh[fresh].diagnostic.empty();
                for (const auto incoming:fresh_links[fresh]) {
                    const auto owner=owner_indices.at(report.overlaps[incoming].room_id);
                    uncertain=uncertain || !report.overlaps[incoming].reliable;
                    if (!seen_old[owner]) { seen_old[owner]=true; owners.push_back(owner); }
                }
            }
        }
        Kind kind=Kind::ambiguous;
        if (!uncertain) {
            if (candidates.empty()) kind=Kind::retired;
            else if (owners.size()==1 && candidates.size()==1) kind=Kind::unique_continuation;
            else if (owners.size()==1) kind=Kind::split;
            else if (candidates.size()==1) kind=Kind::merge;
        }
        for (const auto old:owners) {
            report.retained[old].kind=kind;
            if (kind==Kind::ambiguous && report.retained[old].diagnostic.empty())
                report.retained[old].diagnostic="room correspondence is uncertain or many-to-many; review explicitly";
        }
        for (const auto fresh:candidates) {
            report.fresh[fresh].kind=kind;
            if (kind==Kind::ambiguous && report.fresh[fresh].diagnostic.empty())
                report.fresh[fresh].diagnostic="room correspondence is uncertain or many-to-many; review explicitly";
        }
    }
    for (std::size_t i=0;i<report.fresh.size();++i) if (!seen_fresh[i])
        report.fresh[i].kind=report.fresh[i].diagnostic.empty() ? Kind::new_space : Kind::ambiguous;
}
} // namespace

bool physical_wall_room_regions_equal(const Boundary& first,const std::vector<Boundary>& first_holes,
    const Boundary& second,const std::vector<Boundary>& second_holes) {
    try {
        CorrespondenceBudget budget;budget.geometry(first,first_holes);budget.geometry(second,second_holes);
        if(first.empty() || second.empty())invalid("region equality requires nonempty analytical outlines");
        const auto origin=first.front().start;
        const auto a=correspondence_region(first,first_holes,origin);
        const auto b=correspondence_region(second,second_holes,origin);
        const auto ab=correspondence_boolean<BRepAlgoAPI_Cut>(*a.shape,*b.shape);
        const auto ba=correspondence_boolean<BRepAlgoAPI_Cut>(*b.shape,*a.shape);
        // No tolerance allowance converts a positive sliver into continuation.
        // The authoritative caller also proves physical source lineage.
        return surface_area(ab)==0 && surface_area(ba)==0;
    } catch(const Standard_Failure& e) { invalid(std::string("analytical region equality failed: ")+e.what()); }
}

PhysicalWallRoomLineageCheck validate_retained_physical_wall_room_lineage(
    const Entity& room,const DrawingContext& context) {
    try {
        const auto descriptor=decode_physical_wall_room_descriptor(room);
        CorrespondenceBudget budget;
        const auto lineage=correspondence_lineage(descriptor.source_lineage,descriptor.selected_wall_id,context,budget);
        const auto boundary=boundary_geometry(decode_identified_boundary_entity(room));
        budget.geometry(boundary,descriptor.holes);
        if (boundary.empty()) invalid("retained room outline is empty");
        (void)correspondence_region(boundary,descriptor.holes,boundary.front().start);
        return {lineage.elevation,{lineage.owners.begin(),lineage.owners.end()}};
    } catch (const Json::exception&) { invalid("retained source evidence contains malformed value types"); }
    catch (const Standard_Failure& e) { invalid(std::string("retained room planar validation failed: ")+e.what()); }
}

bool physical_wall_room_lineage_matches_current_inventory(
    const Entity& room,const DrawingContext& context,const PhysicalWallSpace& fresh) {
    try {
        // Original evidence is admitted in its captured phase, independently
        // of the caller's fresh phase selection or current active registry.
        (void)validate_retained_physical_wall_room_lineage(room,context);
        const auto descriptor=decode_physical_wall_room_descriptor(room);
        CorrespondenceBudget budget;
        const auto fresh_source=fresh.source_lineage.at("physical_sources").at(0).at("owner_id").get<std::string>();
        const auto lineage=correspondence_lineage(fresh.source_lineage,fresh_source,context,budget);
        budget.geometry(fresh.boundary,fresh.holes);
        if (fresh.boundary.empty()) invalid("fresh room outline is empty");
        const auto region=correspondence_region(fresh.boundary,fresh.holes,fresh.boundary.front().start);
        if (!std::isfinite(fresh.area_square_metres) ||
            std::abs(region.area-fresh.area_square_metres)>correspondence_tolerance(region.area))
            invalid("fresh detection area disagrees with its analytical comparison region");
        if (!lineage.owners.contains(descriptor.selected_wall_id)) return false;
        // Both complete objects were strictly admitted before removing this
        // one field. No source, placement, topology or other metadata is lost.
        return same_physical_inventory_lineage(descriptor.source_lineage,fresh.source_lineage) &&
            same_boundary(boundary_geometry(decode_identified_boundary_entity(room)),fresh.boundary) &&
            same_holes(descriptor.holes,fresh.holes);
    } catch (const Json::exception&) { invalid("room inventory evidence contains malformed value types"); }
    catch (const Standard_Failure& e) { invalid(std::string("room inventory planar validation failed: ")+e.what()); }
}

namespace {
struct OrdinaryRoomScope {
    std::set<std::string,std::less<>> active;
    bool uses_saved_phases{};
};
OrdinaryRoomScope ordinary_room_scope(const std::map<std::string,Entity,std::less<>>& entities) {
    const auto ids=active_physical_wall_room_ids(entities);
    OrdinaryRoomScope result;
    result.active.insert(ids.begin(),ids.end());
    for (const auto& [id,entity]:entities) {
        (void)id;
        if (entity.type=="model_phases") result.uses_saved_phases=true;
    }
    return result;
}
PhysicalWallRoomCorrespondenceReport correspondence_report(
    const std::map<std::string,Entity,std::less<>>& entities,const DocumentSnapshot* source,
    PhysicalWallSpaces detection,std::string_view selected_wall_id,double elevation,Vec2 origin,
    const std::vector<std::string>* retained_room_ids=nullptr) {
    if (!source && !retained_room_ids) invalid("ordinary correspondence requires an actual source snapshot");
    if (detection.spaces.size()>maximum_rooms) invalid("correspondence exceeds the fresh-component budget");
    CorrespondenceBudget budget; budget.charge(cache_charge(detection));
    PhysicalWallRoomCorrespondenceReport report;
    if (source) {
        report.document_id=source->document_id(); report.revision=source->revision();
        report.source_snapshot_digest=document_snapshot_digest(*source);
    }
    report.selected_wall_id=selected_wall_id; report.context=detection.context;
    report.context_plane_selection=selected_wall_id.empty();
    const auto organization=organize_project(entities);
    std::optional<OrdinaryRoomScope> ordinary_scope;
    if (!retained_room_ids) {
        ordinary_scope=ordinary_room_scope(entities);
        report.active_phase_room_scope=ordinary_scope->uses_saved_phases;
    }
    std::set<std::string,std::less<>> retained_roster;
    std::map<std::string,CorrespondenceLineage,std::less<>> retained_roster_lineage;
    if (retained_room_ids) {
        if (retained_room_ids->size()>maximum_rooms) invalid("phase correspondence exceeds the retained-owner budget");
        for (const auto& id:*retained_room_ids) {
            if (id.empty() || id.size()>128 || !retained_roster.insert(id).second)
                invalid("phase correspondence roster contains an invalid or duplicate owner identity");
            const auto found=entities.find(id);
            if (found==entities.end()) invalid("phase correspondence roster owner no longer exists");
            if (found->second.id!=id) invalid("phase correspondence roster owner identity is inconsistent");
            if (!is_physical_wall_room(found->second)) invalid("phase correspondence roster contains a nonphysical room owner");
            const auto context=organization.drawing_context(id);
            if (!context || !context->complete()) invalid("phase correspondence roster owner has unresolved drawing context");
            if (*context!=report.context) invalid("phase correspondence roster owner belongs to a different drawing context");
            budget.charge(sizeof(std::string)+id.size());
            budget.charge(found->second.id.size()+found->second.type.size()+found->second.properties.dump().size()+
                found->second.extensions.dump().size()+sizeof(RetainedPhysicalWallRoomCorrespondence));
            const auto descriptor=decode_physical_wall_room_descriptor(found->second);
            auto lineage=correspondence_lineage(descriptor.source_lineage,descriptor.selected_wall_id,report.context,budget);
            if (std::abs(lineage.elevation-elevation)>default_geometry_tolerance_metres)
                invalid("phase correspondence roster owner belongs to a different effective plane");
            // Owner identity/topology must be admitted before Boolean work
            // can conservatively report numerical comparison uncertainty.
            // Charge the encoded count before identified-boundary admission
            // performs its contact checks; geometry is charged only once.
            const auto& segments=found->second.properties.at("segments");
            if (!segments.is_array()) invalid("phase correspondence roster owner has malformed boundary segments");
            auto edge_count=segments.size();
            if (edge_count>maximum_returned_edges)
                throw CorrespondenceBudgetFailure("correspondence region exceeds the edge budget");
            for (const auto& hole:descriptor.holes) {
                if (hole.size()>maximum_returned_edges-edge_count)
                    throw CorrespondenceBudgetFailure("correspondence region exceeds the edge budget");
                edge_count+=hole.size();
            }
            budget.geometry_edge_count(edge_count);
            (void)decode_identified_boundary_entity(found->second);
            retained_roster_lineage.emplace(id,std::move(lineage));
        }
    }
    report.effective_elevation_m=elevation;
    std::vector<CorrespondenceLineage> fresh_lineage;
    std::vector<CorrespondenceRegion> fresh_regions;
    for (std::size_t i=0;i<detection.spaces.size();++i) {
        auto& space=detection.spaces[i]; budget.geometry(space.boundary,space.holes);
        FreshPhysicalWallRoomCorrespondence fresh;
        fresh.index=i; fresh.baseline_face_index=space.baseline_face_index;
        fresh.boundary=std::move(space.boundary); fresh.holes=std::move(space.holes);
        fresh.source_lineage=std::move(space.source_lineage); fresh.area_square_metres=space.area_square_metres;
        CorrespondenceLineage lineage; CorrespondenceRegion region;
        try {
            // A context review uses an actual admitted source from the fresh
            // lineage for validation. A deleted selection is never inserted
            // into that evidence or used to manufacture a detector input.
            const auto fresh_source=selected_wall_id.empty()
                ? fresh.source_lineage.at("physical_sources").at(0).at("owner_id").get<std::string>()
                : std::string(selected_wall_id);
            lineage=correspondence_lineage(fresh.source_lineage,fresh_source,report.context,budget);
            region=correspondence_region(fresh.boundary,fresh.holes,origin);
            if (std::abs(region.area-fresh.area_square_metres)>correspondence_tolerance(region.area))
                invalid("fresh detection area disagrees with its analytical comparison region");
        } catch (const CorrespondenceBudgetFailure&) { throw; }
        catch (const std::bad_alloc&) { throw; }
        catch (const Standard_Failure& e) { fresh.diagnostic=std::string("planar comparison failed: ")+e.what(); }
        catch (const std::exception& e) { fresh.diagnostic=e.what(); }
        fresh_lineage.push_back(std::move(lineage)); fresh_regions.push_back(std::move(region));
        report.fresh.push_back(std::move(fresh));
    }
    std::vector<CorrespondenceLineage> old_lineage;
    std::vector<CorrespondenceRegion> old_regions;
    std::set<std::string> current_owners;
    if (!selected_wall_id.empty()) current_owners.insert(std::string(selected_wall_id));
    for (const auto& lineage:fresh_lineage) current_owners.insert(lineage.owners.begin(),lineage.owners.end());
    std::optional<DetectionCache> active_cache;
    if (!retained_room_ids) active_cache.emplace(*source);
    for (const auto& [id,entity]:entities) {
        if (retained_room_ids && !retained_roster.contains(id)) continue;
        if (!is_physical_wall_room(entity)) continue;
        if (ordinary_scope && !ordinary_scope->active.contains(id)) continue;
        const auto context=organization.drawing_context(id);
        if (context && context->complete() && *context!=report.context) continue;
        if (report.retained.size()==maximum_rooms) invalid("correspondence exceeds the retained-owner budget");
        RetainedPhysicalWallRoomCorrespondence old;
        CorrespondenceLineage lineage; CorrespondenceRegion region;
        try {
            if (!context || !context->complete()) invalid("retained room has unresolved drawing context");
            const auto descriptor=decode_physical_wall_room_descriptor(entity);
            lineage=retained_room_ids ? std::move(retained_roster_lineage.at(id)) :
                correspondence_lineage(descriptor.source_lineage,descriptor.selected_wall_id,report.context,budget);
            const auto changed_plane=std::abs(lineage.elevation-report.effective_elevation_m)>default_geometry_tolerance_metres;
            if (!retained_room_ids && changed_plane && std::none_of(lineage.owners.begin(),lineage.owners.end(),[&](const auto& owner) {
                return current_owners.contains(owner);
            })) continue;
            old.descriptor_digest=physical_wall_room_descriptor_digest(entity);
            old.boundary=boundary_geometry(decode_identified_boundary_entity(entity)); old.holes=descriptor.holes;
            if (!retained_room_ids) budget.geometry(old.boundary,old.holes);
            // An explicit phase roster describes the admitted original owner
            // evidence, even when that owner is inactive in the destination.
            // Ordinary correspondence retains its current active-owner fence.
            if (!retained_room_ids && !active_cache->active(id)) invalid("retained room owner is inactive in the semantic phase");
            if (changed_plane) invalid("surviving physical source identity moved to a different effective plane; review explicitly");
            region=correspondence_region(old.boundary,old.holes,origin);
        } catch (const CorrespondenceBudgetFailure&) { throw; }
        catch (const std::bad_alloc&) { throw; }
        catch (const Standard_Failure& e) { old.diagnostic=std::string("planar comparison failed: ")+e.what(); }
        catch (const std::exception& e) { old.diagnostic=e.what(); }
        if (!retained_room_ids)
            budget.charge(entity.id.size()+entity.type.size()+entity.properties.dump().size()+entity.extensions.dump().size()+sizeof(old));
        old.room=entity;
        report.retained.push_back(std::move(old)); old_lineage.push_back(std::move(lineage)); old_regions.push_back(std::move(region));
    }
    const auto owner_pairs=report.retained.size()<2 ? 0 : report.retained.size()*(report.retained.size()-1)/2;
    if (owner_pairs>maximum_correspondence_pairs || (!report.retained.empty() &&
        report.fresh.size()>(maximum_correspondence_pairs-owner_pairs)/report.retained.size()))
        invalid("correspondence exceeds aggregate region-pair budget");
    // A many-owner/one-candidate component is a merge only when its retained
    // regions were disjoint. Duplicate or overlapping stale owners carry
    // conflicting facts and must not be mistaken for a partition removal.
    for (std::size_t i=0;i<report.retained.size();++i) for (std::size_t j=i+1;j<report.retained.size();++j) {
        if (!old_regions[i].shape || !old_regions[j].shape) continue;
        std::string diagnostic;
        try {
            const auto common=correspondence_boolean<BRepAlgoAPI_Common>(*old_regions[i].shape,*old_regions[j].shape);
            const auto area=surface_area(common);
            if (area>0) diagnostic=area>correspondence_tolerance(std::min(old_regions[i].area,old_regions[j].area)) ?
                "retained room regions overlap; conflicting owner facts need explicit review" :
                "retained room regions have numerically uncertain overlap";
        } catch (const std::bad_alloc&) { throw; }
        catch (const Standard_Failure& e) { diagnostic=std::string("retained-owner comparison failed: ")+e.what(); }
        catch (const std::exception& e) { diagnostic=e.what(); }
        if (!diagnostic.empty()) {
            if (report.retained[i].diagnostic.empty()) report.retained[i].diagnostic=diagnostic;
            if (report.retained[j].diagnostic.empty()) report.retained[j].diagnostic=diagnostic;
        }
    }
    std::size_t source_comparisons=0;
    for (std::size_t i=0;i<report.retained.size();++i) for (std::size_t j=0;j<report.fresh.size();++j) {
        const auto& old=report.retained[i]; const auto& fresh=report.fresh[j];
        PhysicalWallRoomOverlap link; link.room_id=old.room.id; link.candidate_index=j;
        if (!old.diagnostic.empty() || !fresh.diagnostic.empty()) link.diagnostic="one or both analytical regions/source lineages could not be compared";
        else {
            link.surviving_sources=surviving_sources(old_lineage[i],fresh_lineage[j],budget,source_comparisons);
            link.exact_lineage_match=old.room.extensions.at("physical_wall_room").at("source_lineage")==fresh.source_lineage;
            try {
                const auto common=correspondence_boolean<BRepAlgoAPI_Common>(*old_regions[i].shape,*fresh_regions[j].shape);
                auto area=surface_area(common);
                const auto limit=std::min(old_regions[i].area,fresh_regions[j].area),uncertainty=correspondence_tolerance(limit);
                if (!std::isfinite(area) || area>limit+uncertainty) invalid("room overlap exceeds either analytical region");
                area=std::clamp(area,0.0,limit); link.area_square_metres=area;
                if (area==0 && link.surviving_sources.empty()) continue;
                if (!(area>uncertainty)) link.diagnostic="surviving lineage has zero or numerically uncertain regional overlap";
                else if (link.surviving_sources.empty()) link.diagnostic="positive regional overlap has no surviving boundary-source interval";
                else link.reliable=true;
            } catch (const std::bad_alloc&) { throw; }
            catch (const Standard_Failure& e) { link.diagnostic=std::string("planar overlap failed: ")+e.what(); }
            catch (const std::exception& e) { link.diagnostic=e.what(); }
        }
        budget.charge(sizeof(link)+link.room_id.size()+link.diagnostic.size());
        report.overlaps.push_back(std::move(link));
    }
    classify_correspondence(report);
    return report;
}
} // namespace

PhysicalWallRoomCorrespondenceReport physical_wall_room_correspondence(const DocumentSnapshot& source,
    std::string_view selected_wall_id) {
    auto detection=detect_physical_wall_spaces(source,selected_wall_id);
    const auto selected=resolve_vertical_placement(source,source.entities().at(std::string(selected_wall_id)));
    Wall wall;std::string error;
    if (!read_document_wall(selected,{},wall,error)) invalid(error);
    // The source point supplies only the numerical comparison origin; it has
    // no role in room identity or candidate assignment.
    return correspondence_report(source.entities(),&source,std::move(detection),selected_wall_id,wall.elevation,wall.baseline.start);
}

PhysicalWallRoomCorrespondenceReport physical_wall_room_correspondence(const DocumentSnapshot& source,
    const DrawingContext& context,double effective_elevation_m) {
    auto detection=detect_physical_wall_spaces(source,context,effective_elevation_m);
    // Use a real detected boundary point when available. The empty result
    // needs no physical seed; zero is only a coordinate origin for comparing
    // retained regions and never supplies geometry or ownership evidence.
    const auto origin=!detection.spaces.empty() && !detection.spaces.front().boundary.empty()
        ? detection.spaces.front().boundary.front().start : Vec2{};
    return correspondence_report(source.entities(),&source,std::move(detection),{},effective_elevation_m,origin);
}
PhasePhysicalWallRoomCorrespondenceReport phase_physical_wall_room_correspondence(
    const DocumentSnapshot& source,const DrawingContext& context,double effective_elevation_m,
    const PhysicalWallPhaseSelection& destination_selection,const std::vector<std::string>& retained_room_ids) {
    if (retained_room_ids.size()>maximum_rooms) invalid("phase correspondence exceeds the retained-owner budget");
    auto detection=detect_physical_wall_spaces(source,context,effective_elevation_m,destination_selection);
    const auto origin=!detection.spaces.empty() && !detection.spaces.front().boundary.empty()
        ? detection.spaces.front().boundary.front().start : Vec2{};
    auto correspondence=correspondence_report(source.entities(),&source,std::move(detection),{},effective_elevation_m,origin,&retained_room_ids);
    correspondence.explicit_phase_evaluation=true;
    PhasePhysicalWallRoomCorrespondenceReport report;
    report.destination_selection=destination_selection;
    report.retained_room_ids=retained_room_ids;
    report.source_entities_digest=entity_map_digest(source.entities());
    report.correspondence=std::move(correspondence);
    return report;
}
PhasePhysicalWallRoomCorrespondenceReport phase_physical_wall_room_correspondence(
    const std::map<std::string,Entity,std::less<>>& entities,const DrawingContext& context,
    double effective_elevation_m,const PhysicalWallPhaseSelection& destination_selection,
    const std::vector<std::string>& retained_room_ids) {
    if (retained_room_ids.size()>maximum_rooms) invalid("phase correspondence exceeds the retained-owner budget");
    auto detection=detect_physical_wall_spaces(entities,context,effective_elevation_m,destination_selection);
    const auto origin=!detection.spaces.empty() && !detection.spaces.front().boundary.empty()
        ? detection.spaces.front().boundary.front().start : Vec2{};
    auto correspondence=correspondence_report(entities,nullptr,std::move(detection),{},effective_elevation_m,origin,&retained_room_ids);
    correspondence.explicit_phase_evaluation=true;
    PhasePhysicalWallRoomCorrespondenceReport report;
    report.destination_selection=destination_selection;
    report.retained_room_ids=retained_room_ids;
    report.source_entities_digest=entity_map_digest(entities);
    report.correspondence=std::move(correspondence);
    return report;
}
bool physical_wall_room_correspondence_is_current(const PhysicalWallRoomCorrespondenceReport& report,
    const DocumentSnapshot& source) {
    if (report.explicit_phase_evaluation || report.document_id!=source.document_id() || report.revision!=source.revision() ||
        report.source_snapshot_digest!=document_snapshot_digest(source)) return false;
    try {
        return report.active_phase_room_scope==ordinary_room_scope(source.entities()).uses_saved_phases;
    } catch (const std::exception&) { return false; }
}
} // namespace sketch
