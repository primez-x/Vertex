#include "sketch/physical_wall_room_merge.hpp"
#include "sketch/physical_wall_room.hpp"
#include "sketch/physical_wall_spaces.hpp"
#include "sketch/boundary_dimension.hpp"
#include "sketch/boundary_integrity.hpp"
#include "sketch/constraint_wall_edit.hpp"
#include "sketch/document_wall.hpp"
#include "sketch/geometry_operations.hpp"
#include "sketch/model_phases.hpp"
#include "sketch/wall_merge.hpp"
#include <Standard_Failure.hxx>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <set>
#include <stdexcept>
#include <tuple>

namespace sketch {
namespace {
using Json = nlohmann::json;
using Entities = std::map<std::string, Entity, std::less<>>;
constexpr double tolerance = default_geometry_tolerance_metres;
constexpr std::size_t maximum_pairs = 65536;
[[noreturn]] void reject(const std::string& reason) {
    throw std::invalid_argument("Wall merge physical room: " + reason);
}
bool close(Vec2 a, Vec2 b) { return std::hypot(a.x-b.x,a.y-b.y)<=tolerance; }
bool exact(const Segment& a,const Segment& b) {
    return a.start.x==b.start.x && a.start.y==b.start.y && a.end.x==b.end.x &&
        a.end.y==b.end.y && a.sweep_radians==b.sweep_radians;
}
bool exact(const Boundary& a,const Boundary& b) {
    return a.size()==b.size() && std::equal(a.begin(),a.end(),b.begin(),
        [](const Segment& x,const Segment& y){return exact(x,y);});
}
bool exact(const std::vector<Boundary>& a,const std::vector<Boundary>& b) {
    return a.size()==b.size() && std::equal(a.begin(),a.end(),b.begin(),
        [](const Boundary& x,const Boundary& y){return exact(x,y);});
}
Vec2 center(const Segment& s) {
    const auto dx=s.end.x-s.start.x,dy=s.end.y-s.start.y,k=0.5/std::tan(s.sweep_radians/2);
    return {s.start.x+dx/2-dy*k,s.start.y+dy/2+dx*k};
}
// The complete circle, directed sweep and span length establish support; these
// comparisons never select a room by a centroid, bounding box or area scalar.
bool same_support(const Segment& a,const Segment& b) {
    if(!close(a.start,b.start) || !close(a.end,b.end) ||
        (a.sweep_radians==0)!=(b.sweep_radians==0))return false;
    if(std::abs(segment_length(a)-segment_length(b))>tolerance)return false;
    if(a.sweep_radians==0)return true;
    return std::signbit(a.sweep_radians)==std::signbit(b.sweep_radians) &&
        std::max(segment_length(a),segment_length(b))*std::abs(a.sweep_radians-b.sweep_radians)<=tolerance &&
        close(center(a),center(b));
}
Segment joined(const Segment& a,const Segment& b) {
    if(!close(a.end,b.start) || (a.sweep_radians==0)!=(b.sweep_radians==0))
        reject("source walls are not directed compatible neighbors");
    Segment result{a.start,b.end,a.sweep_radians+b.sweep_radians};
    const auto la=segment_length(a),lb=segment_length(b),length=segment_length(result);
    if(a.sweep_radians==0) {
        const auto ax=(a.end.x-a.start.x)/la,ay=(a.end.y-a.start.y)/la;
        const auto bx=(b.end.x-b.start.x)/lb,by=(b.end.y-b.start.y)/lb;
        const auto dx=(result.end.x-result.start.x)/length,dy=(result.end.y-result.start.y)/length;
        if(ax*bx+ay*by<=0 || std::abs(ax*by-ay*bx)>1e-9 ||
            std::abs((a.end.x-result.start.x)*dy-(a.end.y-result.start.y)*dx)>tolerance)
            reject("source walls leave their common directed straight support");
    } else if(a.sweep_radians*b.sweep_radians<=0 ||
        std::abs(result.sweep_radians)>=2*std::numbers::pi-1e-9 ||
        !close(center(a),center(b)) || !close(center(a),center(result)))
        reject("source walls do not share one directed analytical circle");
    if(!std::isfinite(length) || std::abs(length-la-lb)>tolerance)
        reject("source walls do not reconstruct the complete directed span");
    return result;
}
Wall read_wall(const Entity& entity) {
    Wall wall;std::string error;
    if(entity.type!="wall" || !read_document_wall(entity,{},wall,error))reject(entity.id+": "+error);
    validate_wall_semantics(wall);return wall;
}
void prove_merge(const Entities& original,const Entities& physical,const WallMergeIntent& intent) {
    (void)encode_wall_merge(intent);
    if(intent.first_wall_id==intent.second_wall_id || !original.contains(intent.first_wall_id) ||
        !original.contains(intent.second_wall_id) || !physical.contains(intent.first_wall_id) ||
        physical.contains(intent.second_wall_id))reject("directed source/survivor identities are unavailable");
    const auto& merged_entity=physical.at(intent.first_wall_id);
    validate_wall_merge_archive(merged_entity);
    const auto archive=merged_entity.extensions.find("wall_merge_archive");
    if(archive==merged_entity.extensions.end())reject("merged wall lacks its source-reconstructed archive");
    const auto record=[](const Entity& entity){return Json{{"id",entity.id},{"type",entity.type},
        {"properties",entity.properties},{"required",entity.required},{"extensions",entity.extensions}};};
    if(archive->at("sources")!=Json::array({record(original.at(intent.first_wall_id)),record(original.at(intent.second_wall_id))}))
        reject("merged wall archive does not retain the exact directed source walls");
    const auto organization=organize_project(original),after=organize_project(physical);
    const auto context=organization.drawing_context(intent.first_wall_id);
    if(!context || !context->complete() || organization.drawing_context(intent.second_wall_id)!=context ||
        after.drawing_context(intent.first_wall_id)!=context)reject("merged wall drawing context changed");
    const auto a=read_wall(resolve_vertical_placement(original,original.at(intent.first_wall_id)));
    const auto b=read_wall(resolve_vertical_placement(original,original.at(intent.second_wall_id)));
    const auto c=read_wall(resolve_vertical_placement(physical,physical.at(intent.first_wall_id)));
    if(a.thickness!=b.thickness || a.thickness!=c.thickness || a.layers!=b.layers || a.layers!=c.layers ||
        std::abs(a.elevation-b.elevation)>tolerance || std::abs(a.elevation-c.elevation)>tolerance ||
        !same_support(joined(a.baseline,b.baseline),c.baseline))
        reject("merged wall material envelope is not the original directed union");
    const auto ga=wall_top_gradient(a),gb=wall_top_gradient(b),gc=wall_top_gradient(c);
    const auto span=segment_length(c.baseline)+c.thickness;
    if(std::hypot(ga.x-gb.x,ga.y-gb.y)*span>tolerance ||
        std::hypot(ga.x-gc.x,ga.y-gc.y)*span>tolerance || std::abs(a.height-c.height)>tolerance ||
        std::abs(wall_top_height(a,segment_length(a.baseline))-b.height)>tolerance)
        reject("merged walls do not preserve their absolute top plane");
    // Current source lineage captures all walls in the plane. An unrelated
    // physical change must never become authority through this completion.
    for(const auto& [id,entity]:original)if(entity.type=="wall" && id!=intent.first_wall_id && id!=intent.second_wall_id) {
        const auto found=physical.find(id);
        if(found==physical.end() || found->second!=entity)reject("unrelated physical wall changed: "+id);
    }
    for(const auto& [id,entity]:physical)if(entity.type=="wall" && !original.contains(id))
        reject("unrelated physical wall was added: "+id);
}
bool same_region(const Boundary& a,const std::vector<Boundary>& ah,const Boundary& b,const std::vector<Boundary>& bh) {
    if(exact(a,b) && exact(ah,bh))return true;
    return physical_wall_room_regions_equal(a,ah,b,bh);
}
using Interval=std::tuple<std::string,std::string,bool,double,double>;
std::vector<Interval> intervals(const Json& lineage,const WallMergeIntent& intent,double fraction,bool transform) {
    std::vector<Interval> uses;
    const auto append=[&](const Json& face,bool hole) {
        for(const auto& edge:face.at("edges"))for(const auto& use:edge.at("source_uses")) {
            auto owner=use.at("owner_id").get<std::string>();
            double start=use.at("parameter_start").get<double>(),end=use.at("parameter_end").get<double>();
            if(transform && owner==intent.first_wall_id){start*=fraction;end*=fraction;}
            else if(transform && owner==intent.second_wall_id){owner=intent.first_wall_id;
                start=fraction+(1-fraction)*start;end=fraction+(1-fraction)*end;}
            uses.emplace_back(owner,use.at("segment_id").get<std::string>(),use.at("reversed").get<bool>()!=hole,start,end);
        }
    };
    append(lineage.at("outer"),false);for(const auto& hole:lineage.at("holes"))append(hole,true);
    std::sort(uses.begin(),uses.end());std::vector<Interval> result;
    for(const auto& use:uses) {
        if(!result.empty() && std::get<0>(result.back())==std::get<0>(use) &&
            std::get<1>(result.back())==std::get<1>(use) && std::get<2>(result.back())==std::get<2>(use) &&
            std::get<3>(use)<=std::get<4>(result.back())+1e-12)
            std::get<4>(result.back())=std::max(std::get<4>(result.back()),std::get<4>(use));
        else result.push_back(use);
    }
    return result;
}
bool continues(const Json& old,const Json& next,const WallMergeIntent& intent,double fraction) {
    if(old.at("context")!=next.at("context"))return false;
    const auto a=intervals(old,intent,fraction,true),b=intervals(next,intent,fraction,false);
    if(a.size()!=b.size())return false;
    for(std::size_t i=0;i<a.size();++i)if(std::get<0>(a[i])!=std::get<0>(b[i]) ||
        std::get<1>(a[i])!=std::get<1>(b[i]) || std::get<2>(a[i])!=std::get<2>(b[i]) ||
        std::abs(std::get<3>(a[i])-std::get<3>(b[i]))>1e-12 ||
        std::abs(std::get<4>(a[i])-std::get<4>(b[i]))>1e-12)return false;
    return !a.empty();
}
Segment captured_segment(const Json& value) {
    const auto point=[](const Json& p){return Vec2{p.at(0).get<double>(),p.at(1).get<double>()};};
    return {point(value.at("start")),point(value.at("end")),value.at("sweep_radians").get<double>()};
}
Json segment_json(const Segment& edge) {
    return {{"start",{edge.start.x,edge.start.y}},{"end",{edge.end.x,edge.end.y}},
        {"sweep_radians",edge.sweep_radians}};
}
// Compare complete captured inventories, rather than treating positive overlap
// or matching boundary intervals as permission to alter other physical walls.
double prove_inventory_delta(const Json& old,const Json& next,const WallMergeIntent& intent) {
    if(old.at("context")!=next.at("context"))reject("captured physical drawing context changed");
    const auto inventory=[](const Json& lineage) {
        std::map<std::string,Json,std::less<>> result;
        for(const auto& item:lineage.at("physical_sources"))
            if(!result.emplace(item.at("owner_id").get<std::string>(),item).second)
                reject("captured physical source inventory is duplicated");
        return result;
    };
    auto expected=inventory(old);const auto actual=inventory(next);
    if(!expected.contains(intent.first_wall_id) || !expected.contains(intent.second_wall_id) ||
        !actual.contains(intent.first_wall_id) || actual.contains(intent.second_wall_id))
        reject("captured source inventory lacks the directed merge identities");
    const auto a=expected.at(intent.first_wall_id),b=expected.at(intent.second_wall_id);
    auto nongeometric=[](Json item){item.erase("owner_id");item.erase("baseline");return item;};
    if(nongeometric(a)!=nongeometric(b))reject("captured wall material/plane/context records differ");
    const auto first=captured_segment(a.at("baseline")),second=captured_segment(b.at("baseline"));
    expected.at(intent.first_wall_id)["baseline"]=segment_json(joined(first,second));
    expected.erase(intent.second_wall_id);
    if(expected!=actual)reject("captured merge changed unrelated physical source inventory");
    auto expected_phases=old.at("semantic_phases");
    for(auto& phase:expected_phases) {
        auto& owners=phase.at("owners");
        const auto first_owner=std::find_if(owners.begin(),owners.end(),[&](const auto& item){return item.at("owner_id")==intent.first_wall_id;});
        const auto second_owner=std::find_if(owners.begin(),owners.end(),[&](const auto& item){return item.at("owner_id")==intent.second_wall_id;});
        if((first_owner==owners.end())!=(second_owner==owners.end()))reject("captured wall phase membership differs");
        if(first_owner!=owners.end()) {
            if(first_owner->at("active_state")!=second_owner->at("active_state"))reject("captured wall active phase states differ");
            owners.erase(second_owner);
        }
    }
    if(expected_phases!=next.at("semantic_phases"))reject("captured merge changed unrelated semantic phase evidence");
    return segment_length(first)/(segment_length(first)+segment_length(second));
}
Json metadata(Json edge) {
    for(const auto* key:{"segment_id","start_vertex_id","end_vertex_id","start","end","sweep_radians"})edge.erase(key);
    return edge;
}
struct Seam { std::string vertex,incoming,outgoing; };
std::pair<IdentifiedBoundary,std::vector<Seam>> match_children(const Entity& owner,const Boundary& destination,
    const Wall& first) {
    auto boundary=decode_identified_boundary_entity(owner);std::vector<Seam> seams;
    if(boundary.segments.size()<destination.size() || boundary.segments.size()>destination.size()+2)
        reject("room "+owner.id+" changed unrelated child topology; review explicitly");
    while(boundary.segments.size()>destination.size()) {
        std::optional<std::size_t> chosen;
        for(std::size_t i=0;i<boundary.segments.size();++i) {
            const auto& incoming=boundary.segments[(i+boundary.segments.size()-1)%boundary.segments.size()];
            const auto& outgoing=boundary.segments[i];
            if(std::hypot(outgoing.segment.start.x-first.baseline.end.x,outgoing.segment.start.y-first.baseline.end.y)>
                first.thickness/2+2*tolerance)continue;
            try {
                const auto span=joined(incoming.segment,outgoing.segment);
                if(std::none_of(destination.begin(),destination.end(),[&](const auto& edge){return same_support(span,edge);}))continue;
            } catch(const std::invalid_argument&){continue;}
            if(chosen)reject("room "+owner.id+" has ambiguous seam child correspondence");
            chosen=i;
        }
        if(!chosen)reject("room "+owner.id+" has no proved physical seam child correspondence");
        const auto incoming=boundary.segments[(*chosen+boundary.segments.size()-1)%boundary.segments.size()];
        const auto outgoing=boundary.segments[*chosen];
        const auto raw=owner.properties.at("segments");
        const auto find=[&](const std::string& id)->const Json& {
            return *std::find_if(raw.begin(),raw.end(),[&](const auto& edge){return edge.at("segment_id")==id;});};
        if(metadata(find(incoming.segment_id))!=metadata(find(outgoing.segment_id)))
            reject("room "+owner.id+" seam edge metadata conflicts");
        seams.push_back({outgoing.start_vertex_id,incoming.segment_id,outgoing.segment_id});
        boundary=remove_boundary_vertex(boundary,outgoing.start_vertex_id);
    }
    std::optional<std::size_t> rotation;
    for(std::size_t offset=0;offset<boundary.segments.size();++offset) {
        bool matches=true;
        for(std::size_t i=0;i<destination.size();++i)
            matches=matches&&same_support(boundary.segments[(i+offset)%destination.size()].segment,destination[i]);
        if(matches){if(rotation)reject("room "+owner.id+" has ambiguous surviving child correspondence");rotation=offset;}
    }
    if(!rotation)reject("room "+owner.id+" changed surviving corners or supports; review explicitly");
    std::rotate(boundary.segments.begin(),boundary.segments.begin()+*rotation,boundary.segments.end());
    for(std::size_t i=0;i<destination.size();++i)boundary.segments[i].segment=destination[i];
    return {std::move(boundary),std::move(seams)};
}
bool mentions(const Json& value,const std::string& id) {
    if(value.is_string())return value==id;
    if(value.is_array())for(const auto& member:value)if(mentions(member,id))return true;
    return false;
}
void refuse_retired_refs(const Json& value,const Seam& seam,const std::string& dependent,const std::string& path) {
    static const std::set<std::string,std::less<>> segment_keys={"segment_id","segment_ids","second_segment_id"};
    static const std::set<std::string,std::less<>> vertex_keys={"vertex_id","vertex_ids","start_vertex_id","end_vertex_id"};
    if(value.is_object())for(const auto& [key,child]:value.items()) {
        const bool generic=key=="refs" || key=="references";
        if(((segment_keys.contains(key)||generic)&&mentions(child,seam.outgoing)) ||
            ((vertex_keys.contains(key)||generic)&&mentions(child,seam.vertex)))
            reject("retired room child is pinned by "+dependent+" at "+path+"/"+key+"; review explicitly");
        refuse_retired_refs(child,seam,dependent,path+"/"+key);
    } else if(value.is_array())for(std::size_t i=0;i<value.size();++i)
        refuse_retired_refs(value[i],seam,dependent,path+"/"+std::to_string(i));
}
void dimensions(Entities& result,const std::string& room,const std::vector<Seam>& seams) {
    for(auto& [id,entity]:result) {
        if(!can_recognize_boundary_dimension_entity_type(entity.type))continue;
        const auto decoded=decode_boundary_dimension_entity(entity);
        if(!decoded.supported()) {
            if(entity.properties.contains("target") && entity.properties.at("target").is_object() &&
                entity.properties.at("target").value("entity_id",Json(nullptr))==room)
                for(const auto& seam:seams)if(mentions(entity.properties.at("target").value("segment_id",Json(nullptr)),seam.incoming) ||
                    mentions(entity.properties.at("target").value("segment_ids",Json(nullptr)),seam.incoming))
                    reject("unsupported dimension pins the changing room seam span: "+id);
            continue;
        }
        if(decoded.dimension->boundary_id!=room)continue;
        auto dimension=*decoded.dimension;bool changed=false;
        for(const auto& seam:seams) {
            if(dimension.kind==BoundaryDimensionKind::angle) {
                if(dimension.vertex_id==seam.vertex)reject("room seam angle dimension is pinned: "+id);
                if(dimension.segment_id==seam.outgoing){dimension.segment_id=seam.incoming;changed=true;}
                if(dimension.secondary_segment_id==seam.outgoing){dimension.secondary_segment_id=seam.incoming;changed=true;}
            } else if(dimension.kind==BoundaryDimensionKind::segment_length) {
                if(!dimension.segment_chain_ids.empty()) {
                    auto& chain=dimension.segment_chain_ids;
                    const auto a=std::find(chain.begin(),chain.end(),seam.incoming),b=std::find(chain.begin(),chain.end(),seam.outgoing);
                    if(a==chain.end() && b==chain.end())continue;
                    if(a==chain.end() || b==chain.end() || std::next(a)!=b)
                        reject("room dimension requires its complete directed seam span: "+id);
                    chain.erase(b);dimension.segment_id=chain.front();if(chain.size()==1)chain.clear();changed=true;
                } else if(dimension.segment_id==seam.incoming || dimension.segment_id==seam.outgoing) {
                    if(dimension.placement!=BoundaryDimensionPlacement::automatic)
                        reject("individual manual room-edge dimension requires review: "+id);
                    dimension.segment_id=seam.incoming;changed=true;
                }
            }
        }
        if(changed) {
            if(dimension.placement==BoundaryDimensionPlacement::automatic && dimension.kind==BoundaryDimensionKind::segment_length) {
                const auto segment=dimension.resolve(result).segment;
                const auto dx=segment.end.x-segment.start.x,dy=segment.end.y-segment.start.y,chord=std::hypot(dx,dy);
                const auto sagitta=chord/2*std::tan(segment.sweep_radians/4);
                const Vec2 midpoint{segment.start.x+dx/2+dy/chord*sagitta,segment.start.y+dy/2-dx/chord*sagitta};
                const auto side=dimension.automatic_placement_version.value_or(1)==1?1.0:
                    (signed_area(boundary_geometry(decode_identified_boundary_entity(result.at(room))))>0?-1.0:1.0);
                const auto offset=std::max(0.25,segment_length(segment)*0.1)*side;
                dimension.text_position={midpoint.x-dy/chord*offset,midpoint.y+dx/chord*offset};
                if(!std::isfinite(dimension.text_position.x) || !std::isfinite(dimension.text_position.y))
                    reject("automatic room dimension placement exceeds range: "+id);
            }
            entity=encode_boundary_dimension_entity(dimension,&entity);
        }
        validate_boundary_dimension_target(dimension,result.at(room));
    }
    for(const auto& seam:seams)for(const auto& [id,entity]:result) {
        auto unchecked=entity;
        if (entity.type == "wall" && (entity.extensions.contains("wall_merge_archive") ||
            entity.extensions.contains("wall_split_archive"))) {
            (void)read_wall(entity);
            validate_wall_merge_archive(entity);
            validate_wall_split_archive(entity);
            unchecked.extensions.erase("wall_merge_archive");
            unchecked.extensions.erase("wall_split_archive");
        }
        if (can_recognize_boundary_entity_type(entity.type) &&
            inspect_boundary_entity_version(entity).format == BoundaryEntityFormat::identified_v1 &&
            (entity.extensions.contains("boundary_geometry_derivation") || entity.properties.contains("boundary_authoring"))) {
            // Receipt names on opaque metadata do not exempt live references.
            // A typed owner's complete replay must admit its history first.
            if (const auto diagnostic = validate_boundary_integrity({{id,entity}})) reject(*diagnostic);
            unchecked.extensions.erase("boundary_geometry_derivation");
            unchecked.properties.erase("boundary_authoring");
        }
        refuse_retired_refs(unchecked.properties,seam,id,"/properties");
        refuse_retired_refs(unchecked.extensions,seam,id,"/extensions");
    }
}
} // namespace

Entities complete_wall_merge_physical_room_sources(const Entities& original,const Entities& physical,
    const WallMergeIntent& intent) {
    try {
        auto result=physical;
        const auto organization=organize_project(original);
        const auto target_context=organization.drawing_context(intent.first_wall_id);
        if (!target_context || !target_context->complete()) return result;
        std::set<std::string,std::less<>> inactive;
        for(const auto& [id,entity]:original)if(entity.type=="model_phases") {
            (void)id;const auto phases=ModelPhases::from_json(entity.properties.at("model"));const auto states=phases.active_state();
            for(const auto& member:phases.entity_ids())if(!states.contains(member) || states.at(member)==ModelPhase::demolished)inactive.insert(member);
        }
        if (inactive.contains(intent.first_wall_id) || inactive.contains(intent.second_wall_id)) return result;
        std::set<std::string,std::less<>> affected;
        for(const auto& [id,owner]:original) {
            if(!is_physical_wall_room(owner) || inactive.contains(id) || organization.drawing_context(id)!=target_context)continue;
            try {
                const auto descriptor=decode_physical_wall_room_descriptor(owner);
                const auto sources=descriptor.source_lineage.find("physical_sources");
                if(sources==descriptor.source_lineage.end() || !sources->is_array())continue;
                for(const auto& item:*sources)if(item.is_object() && item.contains("owner_id") && item.at("owner_id").is_string() &&
                    (item.at("owner_id")==intent.first_wall_id || item.at("owner_id")==intent.second_wall_id)) {affected.insert(id);break;}
            } catch(const std::invalid_argument&){continue;}
        }
        if(affected.empty())return result;
        const auto before=detect_physical_wall_spaces(original,intent.first_wall_id);
        std::optional<PhysicalWallSpaces> after;
        const auto first=read_wall(original.at(intent.first_wall_id));
        const auto second=read_wall(original.at(intent.second_wall_id));
        const double fraction=segment_length(first.baseline)/(segment_length(first.baseline)+segment_length(second.baseline));
        std::size_t comparisons=0,owners=0,bytes=0;
        std::set<std::size_t> assigned;
        for(const auto& [id,owner]:original) {
            if(!affected.contains(id) || inactive.contains(id))continue;
            if(++owners>2048)reject("retained room owner budget exceeded");
            std::optional<PhysicalWallRoomDescriptor> descriptor;
            std::optional<IdentifiedBoundary> identified;
            try {
                descriptor=decode_physical_wall_room_descriptor(owner);identified=decode_identified_boundary_entity(owner);
            } catch(const std::invalid_argument&){continue;}
            const PhysicalWallSpace* current=nullptr;
            for(const auto& space:before.spaces)if(space.source_lineage==descriptor->source_lineage &&
                exact(space.boundary,boundary_geometry(*identified)) && exact(space.holes,descriptor->holes)) {
                if(current)reject("current room source component is ambiguous: "+id);current=&space;
            }
            if(!current)continue; // Stale/future owners remain diagnostic and opaque.
            // A currently source-matched owner that exceeds analytical proof
            // budgets must refuse the merge, never silently become stale.
            (void)validate_retained_physical_wall_room_lineage(owner,before.context);
            if (!after) {
                prove_merge(original,physical,intent);
                after=detect_physical_wall_spaces(physical,intent.first_wall_id);
                if(before.context!=after->context)reject("physical source drawing context changed");
            }
            if(!result.contains(id) || result.at(id)!=owner)reject("overlapping room owner edit: "+id);
            std::optional<std::size_t> selected;
            for(std::size_t i=0;i<after->spaces.size();++i) {
                if(++comparisons>maximum_pairs)reject("analytical room correspondence pair budget exceeded");
                const auto& destination=after->spaces[i];
                if(!continues(current->source_lineage,destination.source_lineage,intent,fraction))continue;
                if(!same_region(current->boundary,current->holes,destination.boundary,destination.holes))continue;
                if(selected)reject("room "+id+" has multiple proved destinations; review explicitly");selected=i;
            }
            if(!selected)reject("room "+id+" clear region or boundary-source intervals changed; review explicitly");
            if(!assigned.insert(*selected).second)reject("multiple current rooms own the same merge destination; review explicitly");
            const auto& destination=after->spaces[*selected];
            (void)prove_inventory_delta(current->source_lineage,destination.source_lineage,intent);
            const auto [boundary,seams]=match_children(owner,destination.boundary,first);
            auto metadata=owner;
            const auto selected_wall=descriptor->selected_wall_id==intent.second_wall_id?intent.first_wall_id:descriptor->selected_wall_id;
            metadata.extensions["physical_wall_room"]=encode_physical_wall_room_descriptor(
                {selected_wall,destination.source_lineage,destination.holes});
            if(!metadata.extensions.contains("boundary_geometry_derivation")) {
                if(metadata.properties.contains("boundary_authoring")) {
                    metadata.extensions["boundary_geometry_derivation"]={{"version",1},
                        {"source_boundary_authoring",metadata.properties.at("boundary_authoring")},{"operations",Json::array()}};
                    metadata.properties.erase("boundary_authoring");
                } else metadata.extensions["boundary_geometry_derivation"]={{"version",2},
                    {"source_boundary",{{"boundary_model_version",1},{"segments",owner.properties.at("segments")}}},{"operations",Json::array()}};
            }
            Json vertices=Json::array();for(const auto& seam:seams)vertices.push_back(seam.vertex);
            metadata.extensions["boundary_geometry_derivation"]["operations"].push_back({{"kind","physical_room_wall_merge"},
                {"value",{{"version",1},{"first_wall_id",intent.first_wall_id},{"second_wall_id",intent.second_wall_id},
                    {"source_descriptor",owner.extensions.at("physical_wall_room")},
                    {"descriptor",metadata.extensions.at("physical_wall_room")},{"seam_vertex_ids",vertices},
                    {"segments",encode_identified_boundary_entity(boundary).properties.at("segments")}}}});
            auto encoded=encode_identified_boundary_entity(boundary,&metadata);
            if(encoded.properties.contains("boundary")) {
                Json geometry=Json::array();for(const auto& edge:boundary.segments)geometry.push_back({
                    {"start",{edge.segment.start.x,edge.segment.start.y}},{"end",{edge.segment.end.x,edge.segment.end.y}},
                    {"sweep_radians",edge.segment.sweep_radians}});encoded.properties["boundary"]=std::move(geometry);
            }
            const auto charge=encoded.properties.dump().size()+encoded.extensions.dump().size();
            if(charge>16*1024*1024-bytes)reject("completed room provenance exceeds 16 MiB");bytes+=charge;
            result.at(id)=std::move(encoded);dimensions(result,id,seams);
        }
        return result;
    } catch(const Json::exception& error){reject(std::string("malformed source evidence: ")+error.what());}
    catch(const Standard_Failure& error){reject(std::string("analytical comparison failed: ")+error.what());}
}

IdentifiedBoundary replay_physical_room_wall_merge(const Entity& owner,const IdentifiedBoundary& preceding,
    const Json& value) {
    try {
        static const std::set<std::string,std::less<>> fields={"version","first_wall_id","second_wall_id",
            "source_descriptor","descriptor","seam_vertex_ids","segments"};
        if(!value.is_object() || value.size()!=fields.size())reject("retained operation has unexpected fields");
        for(const auto* field:{"version","first_wall_id","second_wall_id","source_descriptor","descriptor","seam_vertex_ids","segments"})
            if(!value.contains(field))reject("retained operation is missing "+std::string(field));
        if(!value.at("version").is_number_integer() || value.at("version")!=1 ||
            !value.at("first_wall_id").is_string() || !value.at("second_wall_id").is_string() ||
            !value.at("seam_vertex_ids").is_array() || value.at("seam_vertex_ids").size()>2 ||
            !value.at("segments").is_array())reject("retained operation has unsupported version or types");
        const WallMergeIntent intent{value.at("first_wall_id").get<std::string>(),value.at("second_wall_id").get<std::string>()};
        (void)encode_wall_merge(intent);
        if(owner.type!="room_boundary" || preceding.id!=owner.id || preceding.type!=owner.type)
            reject("retained operation requires the same identified room owner");
        // Operation geometry is deliberately pure; opaque metadata remains on
        // the original stable child IDs and is never receipt geometry authority.
        static const std::set<std::string,std::less<>> edge_fields={"segment_id","start_vertex_id","end_vertex_id","start","end","sweep_radians"};
        for(const auto& edge:value.at("segments")) {
            if(!edge.is_object() || edge.size()!=edge_fields.size())reject("retained operation edge has unsupported metadata");
            for(const auto& key:edge_fields)if(!edge.contains(key))reject("retained operation edge is missing "+key);
        }
        auto source_owner=encode_identified_boundary_entity(preceding);
        source_owner.extensions["physical_wall_room"]=value.at("source_descriptor");
        const auto source=decode_physical_wall_room_descriptor(source_owner);
        const auto context_json=source.source_lineage.at("context");
        const DrawingContext context{context_json.at("property_id").get<std::string>(),context_json.at("building_id").get<std::string>(),
            context_json.at("floor_id").get<std::string>(),context_json.at("layer_id").get<std::string>(),context_json.at("level_id").get<std::string>()};
        if(!context.complete())reject("retained operation context is unresolved");
        (void)validate_retained_physical_wall_room_lineage(source_owner,context);
        auto destination_owner=Entity{owner.id,owner.type,{{"boundary_model_version",1},{"segments",value.at("segments")}},false,
            {{"physical_wall_room",value.at("descriptor")}}};
        const auto destination=decode_physical_wall_room_descriptor(destination_owner);
        const auto final_boundary=decode_identified_boundary_entity(destination_owner);
        (void)validate_retained_physical_wall_room_lineage(destination_owner,context);
        const auto expected_selection=source.selected_wall_id==intent.second_wall_id?intent.first_wall_id:source.selected_wall_id;
        if(destination.selected_wall_id!=expected_selection)reject("retained operation changed unrelated selected source identity");
        const auto fraction=prove_inventory_delta(source.source_lineage,destination.source_lineage,intent);
        if(!continues(source.source_lineage,destination.source_lineage,intent,fraction))
            reject("retained operation does not preserve directed boundary-source intervals");
        if(!same_region(boundary_geometry(preceding),source.holes,boundary_geometry(final_boundary),destination.holes))
            reject("retained operation changes the analytical clear region or holes");
        const auto& captured=*std::find_if(source.source_lineage.at("physical_sources").begin(),
            source.source_lineage.at("physical_sources").end(),[&](const auto& item){return item.at("owner_id")==intent.first_wall_id;});
        Wall first;first.baseline=captured_segment(captured.at("baseline"));first.thickness=captured.at("thickness_m").get<double>();
        const auto [matched,seams]=match_children(source_owner,boundary_geometry(final_boundary),first);
        Json vertices=Json::array();for(const auto& seam:seams)vertices.push_back(seam.vertex);
        if(vertices!=value.at("seam_vertex_ids") || matched!=final_boundary)
            reject("retained operation changed surviving child correspondence or seam order");
        return final_boundary;
    } catch(const Json::exception& error){reject(std::string("malformed retained operation: ")+error.what());}
    catch(const Standard_Failure& error){reject(std::string("retained analytical comparison failed: ")+error.what());}
}
} // namespace sketch
