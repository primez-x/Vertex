#include "sketch/physical_wall_room_split.hpp"
#include "sketch/physical_wall_room.hpp"
#include "sketch/physical_wall_spaces.hpp"
#include "sketch/boundary_dimension.hpp"
#include "sketch/boundary_integrity.hpp"
#include "sketch/constraint_entity.hpp"
#include "sketch/constraint_tolerances.hpp"
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
constexpr std::size_t maximum_work = 65536;
constexpr std::size_t maximum_bytes = 16 * 1024 * 1024;
[[noreturn]] void reject(const std::string& reason) {
    throw std::invalid_argument("Wall split physical room: " + reason);
}
struct Budget {
    std::size_t work{}, bytes{}, owners{};
    void step(std::size_t count=1) {
        if(count>maximum_work-work)reject("analytical correspondence work budget exceeded");
        work+=count;
    }
    void charge(std::size_t count) {
        if(count>maximum_bytes-bytes)reject("geometry proof exceeds 16 MiB");
        bytes+=count;
    }
};
void fields(const Json& value,std::initializer_list<const char*> names) {
    if(!value.is_object() || value.size()!=names.size())reject("retained proof has unexpected fields");
    for(const auto* name:names)if(!value.contains(name))reject("retained proof is missing "+std::string(name));
}
bool valid_id(const std::string& id) {
    return !id.empty() && id.size()<=128 && std::all_of(id.begin(),id.end(),[](unsigned char c){
        return (c>='a'&&c<='z') || (c>='A'&&c<='Z') || (c>='0'&&c<='9') || c=='-' || c=='_' || c=='.' || c==':';});
}
void validate_intent(const WallSplitIntent& intent) {
    if(!valid_id(intent.wall_id) || !valid_id(intent.second_wall_id) || intent.wall_id==intent.second_wall_id ||
        !std::isfinite(intent.fraction) || !(intent.fraction>0 && intent.fraction<1))
        reject("partition identities or directed fraction are invalid");
}
bool close(Vec2 a,Vec2 b) { return std::hypot(a.x-b.x,a.y-b.y)<=tolerance; }
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
bool same_nonphase_lineage(Json retained,Json current) {
    if(!retained.is_object() || !current.is_object())return false;
    retained.erase("semantic_phases");current.erase("semantic_phases");
    return retained==current;
}
PhysicalWallRoomDescriptor admitted_phase_refresh(const Entity& source_owner,const DrawingContext& context,
    const Json& marker) {
    // The refreshed descriptor is a second admitted source, never a rewrite of
    // the descriptor that chains this operation to preceding retained history.
    auto current_owner=source_owner;current_owner.extensions["physical_wall_room"]=marker;
    const auto source=decode_physical_wall_room_descriptor(source_owner);
    const auto current=decode_physical_wall_room_descriptor(current_owner);
    (void)validate_retained_physical_wall_room_lineage(current_owner,context);
    if(source.selected_wall_id!=current.selected_wall_id || !exact(source.holes,current.holes) ||
        source.source_lineage==current.source_lineage || !same_nonphase_lineage(source.source_lineage,current.source_lineage))
        reject("current-source descriptor changes more than captured semantic phase evidence");
    PhysicalWallSpace fresh;fresh.source_lineage=current.source_lineage;
    fresh.boundary=boundary_geometry(decode_identified_boundary_entity(current_owner));fresh.holes=current.holes;
    fresh.area_square_metres=std::abs(signed_area(fresh.boundary));
    for(const auto& hole:fresh.holes)fresh.area_square_metres-=std::abs(signed_area(hole));
    if(!physical_wall_room_lineage_matches_current_inventory(source_owner,context,fresh))
        reject("current-source descriptor does not preserve the admitted physical inventory and clear region");
    return current;
}
Vec2 center(const Segment& s) {
    const auto dx=s.end.x-s.start.x,dy=s.end.y-s.start.y,k=0.5/std::tan(s.sweep_radians/2);
    return {s.start.x+dx/2-dy*k,s.start.y+dy/2+dx*k};
}
bool same_support(const Segment& a,const Segment& b) {
    if(!close(a.start,b.start) || !close(a.end,b.end) ||
        (a.sweep_radians==0)!=(b.sweep_radians==0) ||
        std::abs(segment_length(a)-segment_length(b))>tolerance)return false;
    if(a.sweep_radians==0)return true;
    return std::signbit(a.sweep_radians)==std::signbit(b.sweep_radians) &&
        std::max(segment_length(a),segment_length(b))*std::abs(a.sweep_radians-b.sweep_radians)<=tolerance &&
        close(center(a),center(b));
}
Segment joined(const Segment& a,const Segment& b) {
    if(!close(a.end,b.start) || (a.sweep_radians==0)!=(b.sweep_radians==0))reject("child spans leave their directed support");
    const auto la=segment_length(a),lb=segment_length(b);
    Segment result{a.start,b.end,a.sweep_radians+b.sweep_radians};
    const auto length=segment_length(result);
    if(!(la>tolerance && lb>tolerance && length>tolerance))reject("child spans are degenerate");
    if(a.sweep_radians==0) {
        const auto ax=(a.end.x-a.start.x)/la,ay=(a.end.y-a.start.y)/la;
        const auto bx=(b.end.x-b.start.x)/lb,by=(b.end.y-b.start.y)/lb;
        if(ax*bx+ay*by<=0 || std::abs(ax*by-ay*bx)>1e-9 ||
            std::abs((b.end.x-a.start.x)*ay-(b.end.y-a.start.y)*ax)>tolerance)
            reject("child spans do not share one directed analytical line");
    } else if(a.sweep_radians*b.sweep_radians<=0 ||
        std::abs(result.sweep_radians)>=2*std::numbers::pi-1e-9 ||
        !close(center(a),center(b)) || !close(center(a),center(result)))
        reject("child spans do not share one directed analytical circle");
    if(!std::isfinite(length) || std::abs(length-la-lb)>tolerance)reject("child spans do not reconstruct their parent length");
    return result;
}
Vec2 point(const Segment& s,double fraction) {
    if(s.sweep_radians==0)return {std::lerp(s.start.x,s.end.x,fraction),std::lerp(s.start.y,s.end.y,fraction)};
    const auto c=center(s),angle=s.sweep_radians*fraction;
    const auto x=s.start.x-c.x,y=s.start.y-c.y;
    return {c.x+x*std::cos(angle)-y*std::sin(angle),c.y+x*std::sin(angle)+y*std::cos(angle)};
}
std::pair<Segment,Segment> partition(const Segment& s,double fraction) {
    const auto p=point(s,fraction);
    return {{s.start,p,s.sweep_radians*fraction},{p,s.end,s.sweep_radians*(1-fraction)}};
}
Segment captured_segment(const Json& value) {
    const auto p=[](const Json& v){return Vec2{v.at(0).get<double>(),v.at(1).get<double>()};};
    return {p(value.at("start")),p(value.at("end")),value.at("sweep_radians").get<double>()};
}
Wall read_wall(const Entity& entity) {
    Wall result;std::string error;
    if(entity.type!="wall" || !read_document_wall(entity,{},result,error))reject(entity.id+": "+error);
    validate_wall_semantics(result);return result;
}
void prove_physical_partition(const Entities& original,const Entities& physical,const WallSplitIntent& intent) {
    validate_intent(intent);
    if(!original.contains(intent.wall_id) || original.contains(intent.second_wall_id) ||
        !physical.contains(intent.wall_id) || !physical.contains(intent.second_wall_id))reject("physical partition identities are unavailable");
    const auto organization=organize_project(original),after=organize_project(physical);
    const auto context=organization.drawing_context(intent.wall_id);
    if(!context || !context->complete() || after.drawing_context(intent.wall_id)!=context ||
        after.drawing_context(intent.second_wall_id)!=context)reject("physical partition drawing context changed");
    const auto source=read_wall(resolve_vertical_placement(original,original.at(intent.wall_id)));
    const auto first=read_wall(resolve_vertical_placement(physical,physical.at(intent.wall_id)));
    const auto second=read_wall(resolve_vertical_placement(physical,physical.at(intent.second_wall_id)));
    const auto [a,b]=partition(source.baseline,intent.fraction);
    if(!same_support(a,first.baseline) || !same_support(b,second.baseline) ||
        source.thickness!=first.thickness || source.thickness!=second.thickness ||
        source.layers!=first.layers || source.layers!=second.layers ||
        source.elevation!=first.elevation || source.elevation!=second.elevation)
        reject("physical children do not preserve the source material envelope and plane");
    const auto gradient=wall_top_gradient(source),ga=wall_top_gradient(first),gb=wall_top_gradient(second);
    const auto span=segment_length(source.baseline)+source.thickness;
    if(std::hypot(gradient.x-ga.x,gradient.y-ga.y)*span>tolerance ||
        std::hypot(gradient.x-gb.x,gradient.y-gb.y)*span>tolerance ||
        std::abs(source.height-first.height)>tolerance ||
        std::abs(wall_top_height(source,segment_length(source.baseline)*intent.fraction)-second.height)>tolerance)
        reject("physical children change the source absolute top plane");
    for(const auto& id:{intent.wall_id,intent.second_wall_id}) {
        const auto& child=physical.at(id);validate_wall_split_archive(child);
        const auto archive=child.extensions.find("wall_split_archive");
        if(archive==child.extensions.end() || archive->at("pieces").empty())reject("physical child lacks its qualified partition archive");
        const auto& last=archive->at("pieces").back();
        if(last.at("source_wall_id")!=intent.wall_id || last.at("source_baseline")!=original.at(intent.wall_id).properties.at("baseline") ||
            last.at("fraction")!=intent.fraction || last.at("second_piece")!=(id==intent.second_wall_id) ||
            last.at("partition_baseline")!=child.properties.at("baseline"))reject("physical child archive does not bind this source partition");
    }
    for(const auto& [id,entity]:original)if(entity.type=="wall" && id!=intent.wall_id) {
        const auto found=physical.find(id);
        if(found==physical.end() || found->second!=entity)reject("unrelated physical wall changed: "+id);
    }
    for(const auto& [id,entity]:physical)if(entity.type=="wall" && !original.contains(id) && id!=intent.second_wall_id)
        reject("unrelated physical wall was added: "+id);
}
// Exact complete inventory delta, including remote walls and phase owners.
void prove_inventory_delta(const Json& old,const Json& next,const WallSplitIntent& intent,Budget& budget) {
    if(old.at("context")!=next.at("context"))reject("captured drawing context changed");
    const auto inventory=[&](const Json& lineage) {
        std::map<std::string,Json,std::less<>> result;
        for(const auto& item:lineage.at("physical_sources")) {
            budget.step();
            if(!result.emplace(item.at("owner_id").get<std::string>(),item).second)reject("captured physical inventory is duplicated");
        }
        return result;
    };
    auto expected=inventory(old);const auto actual=inventory(next);
    if(!expected.contains(intent.wall_id) || expected.contains(intent.second_wall_id) ||
        !actual.contains(intent.wall_id) || !actual.contains(intent.second_wall_id))reject("captured inventory lacks the exact partition identities");
    const auto source=expected.at(intent.wall_id);
    const auto [first,second]=partition(captured_segment(source.at("baseline")),intent.fraction);
    if(!same_support(first,captured_segment(actual.at(intent.wall_id).at("baseline"))) ||
        !same_support(second,captured_segment(actual.at(intent.second_wall_id).at("baseline"))))reject("captured baselines are not the directed source partition");
    auto a=source,b=source;b["owner_id"]=intent.second_wall_id;
    a["baseline"]=actual.at(intent.wall_id).at("baseline");b["baseline"]=actual.at(intent.second_wall_id).at("baseline");
    expected.at(intent.wall_id)=std::move(a);expected.emplace(intent.second_wall_id,std::move(b));
    if(expected!=actual)reject("partition changed unrelated captured physical source evidence");
    auto phases=old.at("semantic_phases");
    for(auto& phase:phases) {
        auto& owners=phase.at("owners");
        budget.step(owners.size()+1);
        const auto source_owner=std::find_if(owners.begin(),owners.end(),[&](const auto& item){return item.at("owner_id")==intent.wall_id;});
        if(source_owner!=owners.end()) {
            auto child=*source_owner;child["owner_id"]=intent.second_wall_id;owners.push_back(std::move(child));
            std::sort(owners.begin(),owners.end(),[](const Json& a,const Json& b){return a.at("owner_id").template get<std::string>()<b.at("owner_id").template get<std::string>();});
        }
    }
    if(phases!=next.at("semantic_phases"))reject("partition changed unrelated captured semantic phase evidence");
}
using Interval=std::tuple<std::string,std::string,bool,double,double>;
std::vector<Interval> intervals(const Json& lineage,const WallSplitIntent& intent,bool transform,Budget& budget) {
    std::vector<Interval> uses;
    const auto append=[&](const Json& face,bool hole) {
        for(const auto& edge:face.at("edges"))for(const auto& use:edge.at("source_uses")) {
            budget.step();auto owner=use.at("owner_id").get<std::string>();
            double start=use.at("parameter_start").get<double>(),end=use.at("parameter_end").get<double>();
            if(transform && owner==intent.wall_id){start*=intent.fraction;end*=intent.fraction;}
            else if(transform && owner==intent.second_wall_id){owner=intent.wall_id;
                start=intent.fraction+(1-intent.fraction)*start;end=intent.fraction+(1-intent.fraction)*end;}
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
bool continues(const Json& old,const Json& next,const WallSplitIntent& intent,Budget& budget) {
    if(old.at("context")!=next.at("context"))return false;
    const auto a=intervals(old,intent,false,budget),b=intervals(next,intent,true,budget);
    if(a.empty() || a.size()!=b.size())return false;
    for(std::size_t i=0;i<a.size();++i)if(std::get<0>(a[i])!=std::get<0>(b[i]) ||
        std::get<1>(a[i])!=std::get<1>(b[i]) || std::get<2>(a[i])!=std::get<2>(b[i]) ||
        std::abs(std::get<3>(a[i])-std::get<3>(b[i]))>1e-12 ||
        std::abs(std::get<4>(a[i])-std::get<4>(b[i]))>1e-12)return false;
    return true;
}
bool same_region(const Boundary& a,const std::vector<Boundary>& ah,const Boundary& b,const std::vector<Boundary>& bh) {
    if(exact(a,b) && exact(ah,bh))return true;
    return physical_wall_room_regions_equal(a,ah,b,bh);
}
// Each old directed edge must be the complete analytical union of one or more
// successive destination edges. Existing vertices cannot move or disappear.
using Partitions=std::vector<Boundary>;
Partitions match_children(const IdentifiedBoundary& source,const Boundary& destination,Budget& budget) {
    if(destination.size()<source.segments.size() || destination.size()>maximum_work)reject("destination changes retained child topology");
    std::optional<Partitions> selected;
    for(std::size_t rotation=0;rotation<destination.size();++rotation) {
        budget.step();if(!close(destination[rotation].start,source.segments.front().segment.start))continue;
        Partitions parts;std::size_t consumed=0;bool matches=true;
        for(const auto& old:source.segments) {
            Boundary edges;std::optional<Segment> span;
            while(consumed<destination.size()) {
                budget.step();const auto& edge=destination[(rotation+consumed)%destination.size()];
                if(edges.empty() && !close(edge.start,old.segment.start)){matches=false;break;}
                try {span=span?joined(*span,edge):edge;}catch(const std::invalid_argument&){matches=false;break;}
                edges.push_back(edge);++consumed;
                if(close(edge.end,old.segment.end))break;
                if(segment_length(*span)>=segment_length(old.segment)-tolerance){matches=false;break;}
            }
            if(!matches || !span || !same_support(*span,old.segment)){matches=false;break;}
            parts.push_back(std::move(edges));
        }
        if(!matches || consumed!=destination.size())continue;
        if(selected)reject("stable child correspondence is ambiguous");selected=std::move(parts);
    }
    if(!selected)reject("destination changes original corners or analytical edge supports; review explicitly");
    return std::move(*selected);
}
struct Plan {
    std::string id;
    PhysicalWallRoomDescriptor descriptor;
    PhysicalWallSpace destination;
    IdentifiedBoundary source;
    Partitions parts;
    std::size_t insertions{};
    Json current_source_descriptor;
};
std::vector<Plan> correspondence(const Entities& original,const Entities& physical,const WallSplitIntent& intent,
    bool allow_phase_metadata_refresh) {
    std::vector<Plan> result;const auto organization=organize_project(original);
    const auto context=organization.drawing_context(intent.wall_id);
    if(!context || !context->complete())return result;
    std::set<std::string,std::less<>> inactive;
    for(const auto& [id,entity]:original)if(entity.type=="model_phases") {
        (void)id;const auto phases=ModelPhases::from_json(entity.properties.at("model"));const auto active=phases.active_state();
        for(const auto& member:phases.entity_ids())if(!active.contains(member) || active.at(member)==ModelPhase::demolished)inactive.insert(member);
    }
    if(inactive.contains(intent.wall_id))return result;
    // Eligibility precedes physical detection and destination admission. Future,
    // stale, inactive and unrelated owners do not turn an ordinary split into
    // room review work merely by carrying an opaque marker.
    std::vector<std::pair<const Entity*,PhysicalWallRoomDescriptor>> candidates;
    for(const auto& [id,owner]:original) {
        if(!is_physical_wall_room(owner) || inactive.contains(id) || organization.drawing_context(id)!=context)continue;
        try {
            auto descriptor=decode_physical_wall_room_descriptor(owner);
            if(std::any_of(descriptor.source_lineage.at("physical_sources").begin(),descriptor.source_lineage.at("physical_sources").end(),
                [&](const auto& item){return item.at("owner_id")==intent.wall_id;}))candidates.emplace_back(&owner,std::move(descriptor));
        } catch(const Json::exception&){continue;}catch(const std::invalid_argument&){continue;}
        if(candidates.size()>2048)reject("retained owner budget exceeded");
    }
    if(candidates.empty())return result;
    const auto before=detect_physical_wall_spaces(original,intent.wall_id);
    std::optional<PhysicalWallSpaces> after;std::set<std::size_t> assigned;Budget budget;
    for(const auto& [owner,descriptor]:candidates) {
        std::optional<IdentifiedBoundary> identified;
        try {identified=decode_identified_boundary_entity(*owner);}catch(const std::invalid_argument&){continue;}
        const PhysicalWallSpace* current=nullptr;
        for(const auto& space:before.spaces) {
            budget.step();
            if(!exact(space.boundary,boundary_geometry(*identified)) || !exact(space.holes,descriptor.holes))continue;
            if(space.source_lineage!=descriptor.source_lineage && (!allow_phase_metadata_refresh ||
                !same_nonphase_lineage(descriptor.source_lineage,space.source_lineage) ||
                !physical_wall_room_lineage_matches_current_inventory(*owner,before.context,space)))continue;
            if(current)reject("current source component is ambiguous: "+owner->id);current=&space;
        }
        if(!current)continue;
        if(++budget.owners>2048)reject("retained owner budget exceeded");
        budget.charge(owner->properties.dump().size()+owner->extensions.dump().size());
        (void)validate_retained_physical_wall_room_lineage(*owner,before.context);
        if(!after) {
            prove_physical_partition(original,physical,intent);
            after=detect_physical_wall_spaces(physical,intent.wall_id);
            if(before.context!=after->context)reject("physical drawing context changed");
        }
        if(!physical.contains(owner->id) || physical.at(owner->id)!=*owner)reject("overlapping room owner edit: "+owner->id);
        std::optional<std::size_t> selected;
        for(std::size_t i=0;i<after->spaces.size();++i) {
            budget.step();const auto& destination=after->spaces[i];
            if(!continues(current->source_lineage,destination.source_lineage,intent,budget) ||
                !same_region(current->boundary,current->holes,destination.boundary,destination.holes))continue;
            if(selected)reject("current room has multiple proved destinations: "+owner->id);selected=i;
        }
        if(!selected)reject("clear region or directed source intervals changed; review room "+owner->id);
        if(!assigned.insert(*selected).second)reject("multiple current rooms own one destination");
        const auto& destination=after->spaces[*selected];
        prove_inventory_delta(current->source_lineage,destination.source_lineage,intent,budget);
        budget.charge(destination.source_lineage.dump().size());
        auto parts=match_children(*identified,destination.boundary,budget);
        const auto count=destination.boundary.size()-identified->segments.size();
        Json current_descriptor;
        if(current->source_lineage!=descriptor.source_lineage) {
            current_descriptor=encode_physical_wall_room_descriptor(
                {descriptor.selected_wall_id,current->source_lineage,current->holes});
            (void)admitted_phase_refresh(*owner,before.context,current_descriptor);
            budget.charge(current_descriptor.dump().size());
        }
        result.push_back({owner->id,descriptor,destination,*identified,std::move(parts),count,std::move(current_descriptor)});
    }
    return result;
}
struct Children {
    IdentifiedBoundary boundary;
    Json insertions=Json::array();
    std::map<std::string,std::vector<std::string>,std::less<>> chains;
};
Children materialize(const Plan& plan,const WallSplitPhysicalRoomIds& ids,Budget& budget) {
    if(ids.boundary_id!=plan.id || ids.new_segment_ids.size()!=plan.insertions || ids.new_vertex_ids.size()!=plan.insertions)
        reject("frozen child allocation does not match required insertion count: "+plan.id);
    Children result;result.boundary=plan.source;std::set<std::string,std::less<>> used{plan.id};
    for(const auto& edge:plan.source.segments){used.insert(edge.segment_id);used.insert(edge.start_vertex_id);}
    for(const auto* list:{&ids.new_segment_ids,&ids.new_vertex_ids})for(const auto& id:*list)
        if(!valid_id(id) || !used.insert(id).second)reject("frozen child identities are invalid or reused: "+plan.id);
    std::size_t next=0;
    for(std::size_t i=0;i<plan.parts.size();++i) {
        budget.step(plan.parts[i].size()+1);
        const auto& old=plan.source.segments[i];const auto& pieces=plan.parts[i];
        auto target=old.segment_id;std::vector<std::string> chain{target};
        double remaining=0;for(const auto& edge:pieces)remaining+=segment_length(edge);
        for(std::size_t j=0;j+1<pieces.size();++j) {
            const auto length=segment_length(pieces[j]);const auto fraction=length/remaining;
            const auto& vertex=ids.new_vertex_ids[next];const auto& segment=ids.new_segment_ids[next++];
            result.insertions.push_back({{"segment_id",target},{"new_vertex_id",vertex},{"new_segment_id",segment},{"fraction",fraction}});
            budget.step(result.boundary.segments.size());
            result.boundary=insert_boundary_vertex(result.boundary,target,fraction,vertex,segment);
            target=segment;chain.push_back(segment);remaining-=length;
        }
        if(chain.size()>1)result.chains.emplace(old.segment_id,std::move(chain));
    }
    std::size_t edge=0;for(const auto& pieces:plan.parts)for(const auto& segment:pieces)result.boundary.segments.at(edge++).segment=segment;
    // Detection owns the canonical cyclic start. Identity order follows the
    // detected destination so an exact current-source check remains current.
    if(!plan.destination.boundary.empty()) {
        const auto first=std::find_if(result.boundary.segments.begin(),result.boundary.segments.end(),
            [&](const auto& segment){return exact(segment.segment,plan.destination.boundary.front());});
        if(first==result.boundary.segments.end())reject("destination cyclic start lost its analytical child");
        std::rotate(result.boundary.segments.begin(),first,result.boundary.segments.end());
        if(!exact(boundary_geometry(result.boundary),plan.destination.boundary))reject("destination cyclic order changed");
    }
    (void)encode_identified_boundary_entity(result.boundary);
    return result;
}
void refuse_refs(const Json& value,const std::set<std::string,std::less<>>& changing,const std::string& dependent,
    const std::string& path,Budget& budget,std::size_t depth=0) {
    budget.step();if(depth>64)reject("reference nesting exceeds its proof budget");
    // Unknown field names cannot hide a live span binding. After qualified
    // typed definitions/targets/history have been removed, every exact child
    // identity occurrence is opaque dependent authority and requires review.
    if(value.is_string() && changing.contains(value.get_ref<const std::string&>()))
        reject("changing room span is pinned by "+dependent+" at "+path+"; review explicitly");
    if(value.is_object())for(const auto& [key,child]:value.items()) {
        refuse_refs(child,changing,dependent,path+"/"+key,budget,depth+1);
    } else if(value.is_array())for(std::size_t i=0;i<value.size();++i)
        refuse_refs(value[i],changing,dependent,path+"/"+std::to_string(i),budget,depth+1);
}
void refuse_used_ids(const Json& value,const std::set<std::string,std::less<>>& fresh,Budget& budget,std::size_t depth=0) {
    budget.step();if(depth>64)reject("identity evidence nesting exceeds its proof budget");
    if(value.is_string() && fresh.contains(value.get_ref<const std::string&>()))reject("frozen child identity is already retained in source evidence");
    if(value.is_object())for(const auto& [key,child]:value.items()) {
        if(fresh.contains(key))reject("frozen child identity is already retained as a source metadata key");
        refuse_used_ids(child,fresh,budget,depth+1);
    } else if(value.is_array())for(const auto& child:value)refuse_used_ids(child,fresh,budget,depth+1);
}
// Point relations retain their actual original corner, rather than treating a
// now-partitioned segment ID as authority to shorten a locked span. Arc length
// retains the complete chain; tangent retains the child at its original contact.
std::optional<Json> migrate_constraint_bindings(Entity& entity,const Entities& entities,
    const Plan& plan,const Children& children,Budget& budget) {
    const auto decoded=decode_constraint_entity(entity);
    if(!decoded.supported()) {
        const auto& bindings=entity.properties.at("bindings");
        for(std::size_t i=0;i<bindings.size();++i) {
            budget.step();const auto segment=bindings[i].find("segment_id");
            if(bindings[i].at("owner_id")==plan.id && segment!=bindings[i].end() && segment->is_string() &&
                children.chains.contains(segment->get<std::string>()))
                reject("unsupported constraint pins the changing room span: "+entity.id+" at /properties/bindings/"+
                    std::to_string(i)+"/segment_id: "+decoded.unsupported_reason);
        }
        return std::nullopt; // Other opaque changing-span refs are refused below.
    }
    const auto& constraint=*decoded.constraint;
    const auto affected=[&](const WallEndpointBinding& binding) {
        return binding.owner_id==plan.id && children.chains.contains(binding.segment_id);
    };
    if(std::none_of(constraint.bindings.begin(),constraint.bindings.end(),affected))return std::nullopt;
    const auto old_bindings=entity.properties.at("bindings");
    const auto child=[&](const std::string& id)->const IdentifiedSegment& {
        budget.step(children.boundary.segments.size());
        const auto found=std::find_if(children.boundary.segments.begin(),children.boundary.segments.end(),
            [&](const auto& edge){return edge.segment_id==id;});
        if(found==children.boundary.segments.end())reject("constraint lost its proved incident child: "+entity.id);
        return *found;
    };
    const auto incident=[&](const WallEndpointBinding& binding)->const IdentifiedSegment& {
        budget.step(plan.source.segments.size());
        const auto old=std::find_if(plan.source.segments.begin(),plan.source.segments.end(),
            [&](const auto& edge){return edge.segment_id==binding.segment_id;});
        if(old==plan.source.segments.end())reject("constraint has no original room edge: "+entity.id);
        const auto& chain=children.chains.at(binding.segment_id);
        const bool start=binding.role==WallEndpointRole::start;
        const auto& piece=child(start?chain.front():chain.back());
        const auto old_vertex=start?old->start_vertex_id:old->end_vertex_id;
        const auto new_vertex=start?piece.start_vertex_id:piece.end_vertex_id;
        const auto old_point=start?old->segment.start:old->segment.end;
        const auto new_point=start?piece.segment.start:piece.segment.end;
        if(binding.vertex_id!=old_vertex || new_vertex!=old_vertex || old_point.x!=new_point.x || old_point.y!=new_point.y)
            reject("constraint original outer endpoint is not exactly preserved: "+entity.id);
        return piece;
    };
    Json bindings=Json::array();std::vector<bool> handled;
    if(constraint.relation==ConstraintRelationKind::tangent) {
        // Resolve the original contact/other pairs with just their actual
        // owners. The already-completed room must be replaced by its preceding
        // boundary here; its shortened retained segment is not source proof.
        Entities before;
        for(const auto& binding:constraint.bindings) {
            budget.step();
            if(before.contains(binding.owner_id))continue;
            if(binding.owner_id==plan.id) {
                budget.step(plan.source.segments.size());
                before.emplace(plan.id,encode_identified_boundary_entity(plan.source));
            }
            else {
                const auto owner=entities.find(binding.owner_id);
                if(owner==entities.end())reject("tangent constraint source owner is unavailable: "+entity.id);
                budget.charge(owner->second.properties.dump().size()+owner->second.extensions.dump().size());
                before.emplace(owner->first,owner->second);
            }
        }
        const auto original=resolve_constraint_tangent_segments(constraint,before);
        bindings=old_bindings;handled.resize(constraint.bindings.size());
        for(const std::size_t i:{0U,2U}) {
            const auto& contact=constraint.bindings[i];
            if(!affected(contact))continue;
            const auto& piece=incident(contact);const auto& old=original[i/2];
            if((old.sweep_radians==0)!=(piece.segment.sweep_radians==0))
                reject("tangent incident child changed analytical support kind: "+entity.id);
            if(old.sweep_radians==0) {
                const auto length=segment_length(old);
                const auto dx=(old.end.x-old.start.x)/length,dy=(old.end.y-old.start.y)/length;
                const auto offset=[&](Vec2 p){return std::abs((p.x-old.start.x)*dy-(p.y-old.start.y)*dx);};
                if(offset(piece.segment.start)>tolerance || offset(piece.segment.end)>tolerance ||
                    (piece.segment.end.x-piece.segment.start.x)*dx+(piece.segment.end.y-piece.segment.start.y)*dy<=0)
                    reject("tangent incident child leaves the original directed line support: "+entity.id);
            } else {
                const auto old_center=center(old),next_center=center(piece.segment);
                const auto old_radius=std::hypot(old.start.x-old_center.x,old.start.y-old_center.y);
                const auto next_radius=std::hypot(piece.segment.start.x-next_center.x,piece.segment.start.y-next_center.y);
                if(std::signbit(old.sweep_radians)!=std::signbit(piece.segment.sweep_radians) ||
                    !close(old_center,next_center) || std::abs(old_radius-next_radius)>tolerance ||
                    std::abs(piece.segment.sweep_radians)>std::abs(old.sweep_radians)+constraint_angular_tolerance_radians)
                    reject("tangent incident child leaves the original directed circle support: "+entity.id);
            }
            const auto a=constraint_tangent_endpoint_direction(old,contact.role);
            const auto b=constraint_tangent_endpoint_direction(piece.segment,contact.role);
            const auto dot=a.x*b.x+a.y*b.y;
            if(dot<=0 || std::abs(std::atan2(a.x*b.y-a.y*b.x,dot))>constraint_angular_tolerance_radians)
                reject("tangent incident child changed the directed contact tangent: "+entity.id);
            for(const auto j:{i,i+1}) {
                const auto& endpoint=constraint.bindings[j];
                bindings[j]["segment_id"]=piece.segment_id;
                bindings[j]["vertex_id"]=endpoint.role==WallEndpointRole::start?piece.start_vertex_id:piece.end_vertex_id;
                handled[j]=true;
            }
        }
    } else if(constraint.relation==ConstraintRelationKind::fixed_arc_length) {
        for(std::size_t i=0;i<constraint.bindings.size();i+=2) {
            budget.step();const auto& first=constraint.bindings[i];const auto& second=constraint.bindings[i+1];
            if(!affected(first)) {
                bindings.push_back(old_bindings.at(i));bindings.push_back(old_bindings.at(i+1));
                handled.push_back(false);handled.push_back(false);continue;
            }
            (void)incident(first);(void)incident(second);
            auto chain=children.chains.at(first.segment_id);
            if(first.role==WallEndpointRole::end)std::reverse(chain.begin(),chain.end());
            for(std::size_t j=0;j<chain.size();++j) {
                const auto& piece=child(chain[j]);
                // Only original outer endpoint metadata follows an original
                // endpoint. New inner endpoints receive pure typed bindings;
                // opaque outer metadata is never duplicated onto new points.
                auto a=j==0?old_bindings.at(i):Json{{"owner_id",first.owner_id},{"feature","boundary_segment"},
                    {"role",std::string(wall_endpoint_role_name(first.role))}};
                auto b=j+1==chain.size()?old_bindings.at(i+1):Json{{"owner_id",second.owner_id},{"feature","boundary_segment"},
                    {"role",std::string(wall_endpoint_role_name(second.role))}};
                a["segment_id"]=piece.segment_id;b["segment_id"]=piece.segment_id;
                a["vertex_id"]=first.role==WallEndpointRole::start?piece.start_vertex_id:piece.end_vertex_id;
                b["vertex_id"]=second.role==WallEndpointRole::start?piece.start_vertex_id:piece.end_vertex_id;
                bindings.push_back(std::move(a));bindings.push_back(std::move(b));
                handled.push_back(true);handled.push_back(true);
            }
        }
        if(bindings.size()>256)reject("full directed room arc-length constraint chain exceeds 128 edges: "+entity.id);
        entity.properties["version"]=4;
    } else {
        bindings=old_bindings;handled.resize(constraint.bindings.size());
        for(std::size_t i=0;i<constraint.bindings.size();++i)if(affected(constraint.bindings[i])) {
            bindings[i]["segment_id"]=incident(constraint.bindings[i]).segment_id;handled[i]=true;
        }
    }
    entity.properties["bindings"]=std::move(bindings);
    const auto remapped=decode_constraint_entity(entity);
    if(!remapped.supported())reject("constraint migration lost supported semantics: "+entity.id);
    if(constraint.relation==ConstraintRelationKind::fixed_arc_length)
        (void)resolve_constraint_arc_length(*remapped.constraint,entities);
    else if(constraint.relation==ConstraintRelationKind::tangent)
        (void)resolve_constraint_tangent_segments(*remapped.constraint,entities);
    auto unchecked=entity.properties.at("bindings");
    for(std::size_t i=0;i<handled.size();++i)if(handled[i])
        for(const auto* key:{"owner_id","feature","role","segment_id","vertex_id"})unchecked[i].erase(key);
    return unchecked;
}
void dimensions_and_references(Entities& result,const Plan& plan,const Children& children,Budget& budget) {
    if(children.chains.empty())return;
    std::set<std::string,std::less<>> changing;for(const auto& [id,chain]:children.chains){(void)chain;changing.insert(id);}
    for(auto& [id,entity]:result) {
        auto unchecked=entity;
        if(entity.type=="constraint") {
            if(const auto bindings=migrate_constraint_bindings(entity,result,plan,children,budget)) {
                unchecked=entity;unchecked.properties["bindings"]=*bindings;
            }
        }
        if(can_recognize_boundary_dimension_entity_type(entity.type)) {
            const auto decoded=decode_boundary_dimension_entity(entity);
            if(decoded.supported() && decoded.dimension->boundary_id==plan.id) {
                auto dimension=*decoded.dimension;bool changed=false;
                if(dimension.kind==BoundaryDimensionKind::segment_length) {
                    const auto old=dimension.segment_chain_ids.empty()?std::vector<std::string>{dimension.segment_id}:dimension.segment_chain_ids;
                    std::vector<std::string> chain;
                    for(const auto& segment:old) {
                        if(const auto found=children.chains.find(segment);found!=children.chains.end()) {
                            chain.insert(chain.end(),found->second.begin(),found->second.end());changed=true;
                        } else chain.push_back(segment);
                    }
                    if(chain.size()>128)reject("full room dimension chain exceeds 128 edges: "+id);
                    if(changed){dimension.segment_id=chain.front();dimension.segment_chain_ids=std::move(chain);}
                } else if(dimension.kind==BoundaryDimensionKind::angle) {
                    for(auto* target:{&dimension.segment_id,&dimension.secondary_segment_id}) {
                        const auto found=children.chains.find(*target);if(found==children.chains.end())continue;
                        const auto old=std::find_if(plan.source.segments.begin(),plan.source.segments.end(),[&](const auto& edge){return edge.segment_id==*target;});
                        if(dimension.vertex_id==old->end_vertex_id){*target=found->second.back();changed=true;}
                        else if(dimension.vertex_id!=old->start_vertex_id)reject("room angle has an unproved original corner target: "+id);
                    }
                }
                if(changed)entity=encode_boundary_dimension_entity(dimension,&entity);
                validate_boundary_dimension_target(dimension,result.at(plan.id));
                // Only the supported typed target is handled. Unknown adjacent
                // reference metadata remains subject to conservative refusal.
                unchecked=entity;
                for(const auto* key:{"entity_id","segment_id","segment_ids","second_segment_id","vertex_id"})
                    unchecked.properties["target"].erase(key);
            }
        }
        if(entity.type=="wall" && (entity.extensions.contains("wall_split_archive") || entity.extensions.contains("wall_merge_archive"))) {
            (void)read_wall(entity);validate_wall_split_archive(entity);validate_wall_merge_archive(entity);
            unchecked.extensions.erase("wall_split_archive");unchecked.extensions.erase("wall_merge_archive");
        }
        if(can_recognize_boundary_entity_type(entity.type) && inspect_boundary_entity_version(entity).format==BoundaryEntityFormat::identified_v1) {
            if(entity.extensions.contains("boundary_geometry_derivation") || entity.properties.contains("boundary_authoring")) {
                if(const auto diagnostic=validate_boundary_integrity({{id,entity}}))reject(*diagnostic);
                unchecked.extensions.erase("boundary_geometry_derivation");unchecked.properties.erase("boundary_authoring");
            }
            // Stable child identity fields are definitions, not dependent refs.
            for(auto& edge:unchecked.properties.at("segments")) {
                edge.erase("segment_id");edge.erase("start_vertex_id");edge.erase("end_vertex_id");
            }
        }
        refuse_refs(unchecked.properties,changing,id,"/properties",budget);
        refuse_refs(unchecked.extensions,changing,id,"/extensions",budget);
    }
}
Entity completed_owner(const Entity& owner,const Plan& plan,const Children& children,const WallSplitIntent& intent) {
    auto metadata=owner;
    metadata.extensions["physical_wall_room"]=encode_physical_wall_room_descriptor(
        {plan.descriptor.selected_wall_id,plan.destination.source_lineage,plan.destination.holes});
    if(!metadata.extensions.contains("boundary_geometry_derivation")) {
        if(metadata.properties.contains("boundary_authoring")) {
            metadata.extensions["boundary_geometry_derivation"]={{"version",1},
                {"source_boundary_authoring",metadata.properties.at("boundary_authoring")},{"operations",Json::array()}};
            metadata.properties.erase("boundary_authoring");
        } else metadata.extensions["boundary_geometry_derivation"]={{"version",2},
            {"source_boundary",{{"boundary_model_version",1},{"segments",owner.properties.at("segments")}}},{"operations",Json::array()}};
    }
    Json proof={{"version",1},{"wall_id",intent.wall_id},{"second_wall_id",intent.second_wall_id},{"fraction",intent.fraction},
            {"source_descriptor",owner.extensions.at("physical_wall_room")},{"descriptor",metadata.extensions.at("physical_wall_room")},
            {"insertions",children.insertions},{"segments",encode_identified_boundary_entity(children.boundary).properties.at("segments")}};
    if(!plan.current_source_descriptor.is_null()) {
        proof["version"]=2;proof["current_source_descriptor"]=plan.current_source_descriptor;
    }
    metadata.extensions["boundary_geometry_derivation"]["operations"].push_back(
        {{"kind","physical_room_wall_split"},{"value",std::move(proof)}});
    auto result=encode_identified_boundary_entity(children.boundary,&metadata);
    if(result.properties.contains("boundary")) {
        Json geometry=Json::array();for(const auto& edge:children.boundary.segments)geometry.push_back({
            {"start",{edge.segment.start.x,edge.segment.start.y}},{"end",{edge.segment.end.x,edge.segment.end.y}},
            {"sweep_radians",edge.segment.sweep_radians}});result.properties["boundary"]=std::move(geometry);
    }
    return result;
}
} // namespace

std::vector<WallSplitPhysicalRoomIds> prepare_wall_split_physical_room_ids(
    const Entities& original,const Entities& physical,const WallSplitIntent& intent,bool allow_phase_metadata_refresh) {
    try {
        const auto plans=correspondence(original,physical,intent,allow_phase_metadata_refresh);std::vector<WallSplitPhysicalRoomIds> result;
        for(const auto& plan:plans) {
            WallSplitPhysicalRoomIds ids;ids.boundary_id=plan.id;
            for(std::size_t i=0;i<plan.insertions;++i){ids.new_segment_ids.push_back(make_stable_id());ids.new_vertex_ids.push_back(make_stable_id());}
            result.push_back(std::move(ids));
        }
        return result;
    } catch(const Json::exception& error){reject(std::string("malformed source evidence: ")+error.what());}
    catch(const Standard_Failure& error){reject(std::string("analytical comparison failed: ")+error.what());}
}

Entities complete_wall_split_physical_room_sources(const Entities& original,const Entities& physical,const WallSplitIntent& intent,
    bool allow_phase_metadata_refresh) {
    try {
        const auto plans=correspondence(original,physical,intent,allow_phase_metadata_refresh);
        if(plans.size()!=intent.physical_room_owners.size())reject("frozen owner list does not match initially current room inventory");
        auto result=physical;Budget budget;std::set<std::string,std::less<>> fresh;
        for(const auto& ids:intent.physical_room_owners)for(const auto* list:{&ids.new_segment_ids,&ids.new_vertex_ids})for(const auto& id:*list)
            if(!valid_id(id) || original.contains(id) || physical.contains(id) || !fresh.insert(id).second)
                reject("fresh child identity collides with an existing owner or another allocation");
        if(!fresh.empty())for(const auto* entities:{&original,&physical})for(const auto& [id,entity]:*entities) {
            (void)id;refuse_used_ids(entity.properties,fresh,budget);refuse_used_ids(entity.extensions,fresh,budget);
        }
        for(std::size_t i=0;i<plans.size();++i) {
            const auto& plan=plans[i];const auto& ids=intent.physical_room_owners[i];
            const auto children=materialize(plan,ids,budget);
            auto owner=completed_owner(original.at(plan.id),plan,children,intent);
            budget.charge(owner.properties.dump().size()+owner.extensions.dump().size());
            result.at(plan.id)=std::move(owner);dimensions_and_references(result,plan,children,budget);
        }
        return result;
    } catch(const Json::exception& error){reject(std::string("malformed source evidence: ")+error.what());}
    catch(const Standard_Failure& error){reject(std::string("analytical comparison failed: ")+error.what());}
}

IdentifiedBoundary replay_physical_room_wall_split(const Entity& owner,const IdentifiedBoundary& preceding,const Json& value) {
    try {
        Budget budget;budget.charge(value.dump().size());
        if(!value.is_object() || !value.contains("version") || !value.at("version").is_number_integer() ||
            (value.at("version")!=1 && value.at("version")!=2))reject("retained operation has unsupported version");
        const bool refreshed=value.at("version")==2;
        if(refreshed)fields(value,{"version","wall_id","second_wall_id","fraction","source_descriptor",
            "current_source_descriptor","descriptor","insertions","segments"});
        else fields(value,{"version","wall_id","second_wall_id","fraction","source_descriptor","descriptor","insertions","segments"});
        if(!value.at("wall_id").is_string() ||
            !value.at("second_wall_id").is_string() || !value.at("fraction").is_number() || !value.at("insertions").is_array() ||
            !value.at("segments").is_array())reject("retained operation has unsupported version or types");
        WallSplitIntent intent;intent.wall_id=value.at("wall_id").get<std::string>();
        intent.second_wall_id=value.at("second_wall_id").get<std::string>();intent.fraction=value.at("fraction").get<double>();validate_intent(intent);
        if(owner.type!="room_boundary" || preceding.id!=owner.id || preceding.type!=owner.type)reject("retained operation requires the same identified room owner");
        budget.step(value.at("insertions").size()+value.at("segments").size()+preceding.segments.size());
        for(const auto& edge:value.at("segments"))fields(edge,{"segment_id","start_vertex_id","end_vertex_id","start","end","sweep_radians"});
        auto source_owner=encode_identified_boundary_entity(preceding);
        source_owner.extensions["physical_wall_room"]=value.at("source_descriptor");
        const auto source=decode_physical_wall_room_descriptor(source_owner);const auto& c=source.source_lineage.at("context");
        const DrawingContext context{c.at("property_id").get<std::string>(),c.at("building_id").get<std::string>(),
            c.at("floor_id").get<std::string>(),c.at("layer_id").get<std::string>(),c.at("level_id").get<std::string>()};
        if(!context.complete())reject("retained operation context is unresolved");
        (void)validate_retained_physical_wall_room_lineage(source_owner,context);
        const auto current=refreshed?admitted_phase_refresh(source_owner,context,value.at("current_source_descriptor")):source;
        const Entity destination_owner{owner.id,owner.type,{{"boundary_model_version",1},{"segments",value.at("segments")}},false,
            {{"physical_wall_room",value.at("descriptor")}}};
        const auto destination=decode_physical_wall_room_descriptor(destination_owner);
        const auto final_boundary=decode_identified_boundary_entity(destination_owner);
        (void)validate_retained_physical_wall_room_lineage(destination_owner,context);
        if(source.selected_wall_id!=destination.selected_wall_id)reject("retained operation changed the selected source identity");
        prove_inventory_delta(current.source_lineage,destination.source_lineage,intent,budget);
        if(!continues(current.source_lineage,destination.source_lineage,intent,budget))reject("retained operation changes directed source intervals");
        if(!same_region(boundary_geometry(preceding),source.holes,boundary_geometry(final_boundary),destination.holes))
            reject("retained operation changes the analytical clear region or holes");
        auto parts=match_children(preceding,boundary_geometry(final_boundary),budget);
        WallSplitPhysicalRoomIds ids;ids.boundary_id=owner.id;
        for(const auto& insertion:value.at("insertions")) {
            fields(insertion,{"segment_id","new_vertex_id","new_segment_id","fraction"});
            if(!insertion.at("segment_id").is_string() || !insertion.at("new_vertex_id").is_string() ||
                !insertion.at("new_segment_id").is_string() || !insertion.at("fraction").is_number())reject("retained insertion types are invalid");
            ids.new_segment_ids.push_back(insertion.at("new_segment_id").get<std::string>());
            ids.new_vertex_ids.push_back(insertion.at("new_vertex_id").get<std::string>());
        }
        PhysicalWallSpace space;space.boundary=boundary_geometry(final_boundary);
        const Plan plan{owner.id,source,std::move(space),preceding,std::move(parts),
            final_boundary.segments.size()-preceding.segments.size(),Json{}};
        const auto expected=materialize(plan,ids,budget);
        if(expected.insertions!=value.at("insertions") || expected.boundary!=final_boundary)
            reject("retained operation changed sequential insertion proof or stable child correspondence");
        return final_boundary;
    } catch(const Json::exception& error){reject(std::string("malformed retained operation: ")+error.what());}
    catch(const Standard_Failure& error){reject(std::string("retained analytical comparison failed: ")+error.what());}
}
} // namespace sketch
