#include "sketch/wall_merge.hpp"
#include "sketch/boundary_integrity.hpp"
#include "sketch/boundary_dimension.hpp"
#include "sketch/boundary_entity.hpp"
#include "sketch/constraint_entity.hpp"
#include "sketch/constraint_integrity.hpp"
#include "sketch/constraint_wall_edit.hpp"
#include "sketch/document_wall.hpp"
#include "sketch/corner_window.hpp"
#include "sketch/model_phases.hpp"
#include "sketch/project_organization.hpp"
#include "sketch/room_relationships.hpp"
#include "sketch/physical_wall_room_data.hpp"
#ifdef VERTEX_HAS_PHYSICAL_ROOM_REVIEW
#include "sketch/physical_wall_room_merge.hpp"
#endif
#include "sketch/sheet_view_entity_codec.hpp"
#include "sketch/wall_measurement.hpp"
#include "sketch/wall_semantics.hpp"
#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <iterator>
#include <numbers>
#include <limits>
#include <numeric>
#include <optional>
#include <set>
#include <stdexcept>

namespace sketch {
namespace {
using Json = nlohmann::json;
using Entities = std::map<std::string, Entity, std::less<>>;
[[noreturn]] void reject(const std::string& message) { throw std::invalid_argument(message); }
bool valid_id(const std::string& id) {
    return !id.empty() && id.size() <= 128 && std::all_of(id.begin(), id.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == ':';
    });
}
void exact(const Json& value, std::initializer_list<const char*> fields) {
    if (!value.is_object() || value.size() != fields.size()) reject("Wall merge proof has unexpected fields");
    for (const auto* key : fields) if (!value.contains(key)) reject("Wall merge proof is missing " + std::string(key));
}
void validate(const WallMergeIntent& intent) {
    if (!valid_id(intent.first_wall_id) || !valid_id(intent.second_wall_id) || intent.first_wall_id == intent.second_wall_id)
        reject("Wall merge requires two distinct valid wall identities");
}
bool selected(const std::string& id, const WallMergeIntent& intent) {
    return id == intent.first_wall_id || id == intent.second_wall_id;
}
Wall read_wall(const Entity& entity) {
    Wall wall; std::string error;
    if (entity.type != "wall" || !read_document_wall(entity, {}, wall, error))
        reject("Wall merge source " + entity.id + ": " + error);
    validate_wall_semantics(wall);
    const auto bounds=segment_bounds(wall.baseline);
    if(std::max({std::abs(bounds.minimum.x),std::abs(bounds.minimum.y),std::abs(bounds.maximum.x),std::abs(bounds.maximum.y)})>1e6)
        reject("Wall merge source exceeds the +/-1e6 metre geometry envelope: "+entity.id);
    validate_wall_curve_input(entity); validate_wall_length_input(entity); validate_wall_split_archive(entity);
    return wall;
}
bool close(Vec2 a, Vec2 b) { return std::hypot(a.x-b.x, a.y-b.y) <= default_geometry_tolerance_metres; }
Vec2 center(const Segment& s) {
    const auto dx=s.end.x-s.start.x, dy=s.end.y-s.start.y, k=0.5/std::tan(s.sweep_radians/2);
    return {s.start.x+dx/2-dy*k, s.start.y+dy/2+dx*k};
}
Vec2 merge_point(const Segment& segment,double fraction) {
    if(segment.sweep_radians==0)return {std::lerp(segment.start.x,segment.end.x,fraction),
        std::lerp(segment.start.y,segment.end.y,fraction)};
    const auto dx=segment.end.x-segment.start.x,dy=segment.end.y-segment.start.y,chord=std::hypot(dx,dy);
    const auto angle=segment.sweep_radians*fraction,half_sine=std::sin(angle/2);
    const auto center_offset=std::abs(segment.sweep_radians)==std::numbers::pi?0.0:chord/(2*std::tan(segment.sweep_radians/2));
    const auto along=chord*half_sine*half_sine+center_offset*std::sin(angle);
    const auto normal=-chord*std::sin(angle)/2+center_offset*(2*half_sine*half_sine);
    const Vec2 result{std::fma(dx/chord,along,std::fma(-dy/chord,normal,segment.start.x)),
        std::fma(dy/chord,along,std::fma(dx/chord,normal,segment.start.y))};
    if(!std::isfinite(result.x)||!std::isfinite(result.y))reject("Wall merge analytical station is unrepresentable");
    return result;
}
Segment joined(const Wall& first, const Wall& second) {
    const auto& a=first.baseline; const auto& b=second.baseline;
    // Angular checks never grant a physical displacement allowance. Every
    // source subspan is checked in metres against the complete merged support.
    if (!close(a.end,b.start)) reject("Wall merge requires first.end to meet second.start");
    const auto la=segment_length(a), lb=segment_length(b);
    Segment result{a.start,b.end,a.sweep_radians+b.sweep_radians};
    if ((a.sweep_radians==0) != (b.sweep_radians==0)) reject("Wall merge cannot join a straight span and an arc");
    if (a.sweep_radians==0) {
        const auto ax=(a.end.x-a.start.x)/la, ay=(a.end.y-a.start.y)/la;
        const auto bx=(b.end.x-b.start.x)/lb, by=(b.end.y-b.start.y)/lb;
        if (ax*bx+ay*by <= 0 || std::abs(ax*by-ay*bx)>1e-9)
            reject("Wall merge requires collinear spans in the same direction");
        const auto dx=result.end.x-result.start.x,dy=result.end.y-result.start.y,length=std::hypot(dx,dy);
        const auto support_distance=std::abs((a.end.x-result.start.x)*(dy/length)-(a.end.y-result.start.y)*(dx/length));
        const auto roundoff=64*std::numeric_limits<double>::epsilon()*std::max({1.0,
            std::abs(a.start.x),std::abs(a.start.y),std::abs(a.end.x),std::abs(a.end.y),std::abs(b.end.x),std::abs(b.end.y)});
        if(!std::isfinite(support_distance) || support_distance>default_geometry_tolerance_metres+roundoff)
            reject("Wall merge seam leaves the merged straight support line");
    } else {
        if (std::signbit(a.sweep_radians)!=std::signbit(b.sweep_radians) || std::abs(result.sweep_radians)>=2*std::numbers::pi)
            reject("Wall merge requires a consistent arc direction and a sweep below one full turn");
        const auto ca=center(a), cb=center(b), cr=center(result);
        if (!close(ca,cb) || !close(ca,cr) ||
            std::abs(std::hypot(a.start.x-ca.x,a.start.y-ca.y)-std::hypot(b.start.x-cb.x,b.start.y-cb.y))>
                default_geometry_tolerance_metres)
            reject("Wall merge arcs do not share one analytical circle");
        const auto fraction=a.sweep_radians/result.sweep_radians;
        const Vec2 seam=merge_point(result,fraction);
        const auto roundoff=64*std::numeric_limits<double>::epsilon()*std::max({1.0,
            std::abs(a.start.x),std::abs(a.start.y),std::abs(a.end.x),std::abs(a.end.y),std::abs(b.end.x),std::abs(b.end.y)});
        if(!std::isfinite(seam.x) || !std::isfinite(seam.y) ||
            std::hypot(seam.x-a.end.x,seam.y-a.end.y)>default_geometry_tolerance_metres+roundoff ||
            std::hypot(seam.x-b.start.x,seam.y-b.start.y)>default_geometry_tolerance_metres+roundoff)
            reject("Wall merge arc sweep does not reconstruct the directed seam");
        for(const auto station:{0.25,0.5,0.75}) {
            const auto expected_first=merge_point(result,fraction*station),actual_first=merge_point(a,station);
            const auto expected_second=merge_point(result,fraction+(1-fraction)*station),actual_second=merge_point(b,station);
            if(std::hypot(expected_first.x-actual_first.x,expected_first.y-actual_first.y)>default_geometry_tolerance_metres+roundoff ||
                std::hypot(expected_second.x-actual_second.x,expected_second.y-actual_second.y)>default_geometry_tolerance_metres+roundoff)
                reject("Wall merge arc subspans leave the complete merged analytical support");
        }
    }
    if (!std::isfinite(segment_length(result)) || segment_length(result)<=default_geometry_tolerance_metres ||
        std::abs(segment_length(result)-la-lb)>default_geometry_tolerance_metres)
        reject("Wall merge does not reconstruct the complete physical length");
    Wall check=first; check.baseline=result; check.openings.clear();
    const auto gradient=wall_top_gradient(first); check.top_gradient_m_per_m=gradient;
    const Vec2 chord{result.end.x-result.start.x,result.end.y-result.start.y};
    check.slope_rise=gradient.x*chord.x+gradient.y*chord.y;
    validate_wall_semantics(check);
    return result;
}
Json archived(const Entity& wall) {
    return {{"id",wall.id},{"type",wall.type},{"properties",wall.properties},{"required",wall.required},{"extensions",wall.extensions}};
}
Entity unarchive(const Json& value) {
    exact(value,{"id","type","properties","required","extensions"});
    if (!value.at("id").is_string() || !valid_id(value.at("id").get<std::string>()) || value.at("type")!="wall" ||
        !value.at("required").is_boolean() || !value.at("properties").is_object() || !value.at("extensions").is_object())
        reject("Malformed wall merge archived source");
    return {value.at("id").get<std::string>(),"wall",value.at("properties"),value.at("required").get<bool>(),value.at("extensions")};
}
Json comparable_properties(const Entity& entity) {
    auto result=entity.properties;
    for (const auto* key:{"baseline","height_m","height","slope_rise_m","slope_rise","top_plane","name"}) result.erase(key);
    if(result.contains("quantity_entries")) {
        auto& entries=result.at("quantity_entries");
        if(!entries.is_object())reject("Wall merge quantity entries are malformed: "+entity.id);
        for(auto it=entries.begin();it!=entries.end();) {
            const auto& key=it.key();
            if(key.starts_with("/baseline/") || key=="/length_m" || key=="/height_m" || key=="/height" ||
                key=="/slope_rise_m" || key=="/slope_rise" || key.starts_with("/top_plane/") ||
                key=="/thickness_m" || key=="/thickness" || key=="/elevation_m" || key=="/elevation")it=entries.erase(it);else ++it;
        }
        if(entries.empty())result.erase("quantity_entries");
    }
    auto baseline=entity.properties.at("baseline");
    for (const auto* key:{"start","end","sweep_radians"}) baseline.erase(key);
    result["baseline"]=std::move(baseline); return result;
}
Json comparable_extensions(const Entity& entity) {
    auto result=entity.extensions;
    for (const auto* key:{"wall_split_archive","wall_merge_archive","curve_input","curve_input_derivation"}) result.erase(key);
    if (result.contains("constraint_authoring")) {
        auto& section=result.at("constraint_authoring");
        if (!section.is_object() || !section.contains("version") || section.at("version")!=1)
            reject("Wall merge cannot archive unsupported length metadata: "+entity.id);
        section.erase("last_length_entry");
        if (section.size()==1) result.erase("constraint_authoring");
    }
    return result;
}
void compatible(const Entity& first, const Entity& second, const Wall& a, const Wall& b) {
    if (first.required!=second.required || comparable_properties(first)!=comparable_properties(second) ||
        comparable_extensions(first)!=comparable_extensions(second))
        reject("Wall merge requires matching dimensions, context, material, phase and appearance metadata");
    const auto ga=wall_top_gradient(a), gb=wall_top_gradient(b);
    const auto span=segment_length(a.baseline)+segment_length(b.baseline)+a.thickness;
    if (std::hypot(ga.x-gb.x,ga.y-gb.y)*span>default_geometry_tolerance_metres ||
        std::abs(a.elevation-b.elevation)>default_geometry_tolerance_metres ||
        std::abs(wall_top_height(a,segment_length(a.baseline))-b.height)>default_geometry_tolerance_metres)
        reject("Wall merge requires the same absolute bottom and top planes");
}
bool mentions(const Json& value, const std::string& id) {
    if (value.is_string()) return value==id;
    if (value.is_array()) return std::any_of(value.begin(),value.end(),[&](const auto& child){return mentions(child,id);});
    return false;
}
void unknown_refs(const Json& value, const WallMergeIntent& intent, const std::string& owner, const std::string& path) {
    static const std::set<std::string,std::less<>> canonical={"wall_id","wall_ids","entity_id","entity_ids","object_id",
        "object_ids","host_id","host_ids","target_id","target_ids","source_entity_id","source_entity_ids",
        "source_id","source_ids","parent_id","parent_ids","owner_id","host_entity_id","host_entity_ids","refs","references","wall_members",
        "wall_join_id","wall_join_ids","join_id","join_ids","dimension_id","dimension_ids",
        "constraint_id","constraint_ids","seam_constraint_id"};
    if (value.is_object()) for (const auto& [key,child]:value.items()) {
        if (canonical.contains(key) && (mentions(child,intent.first_wall_id)||mentions(child,intent.second_wall_id)))
            reject("Wall merge has unsupported reference in "+owner+" at "+path+"/"+key);
        unknown_refs(child,intent,owner,path+"/"+key);
    } else if (value.is_array()) for (std::size_t i=0;i<value.size();++i)
        unknown_refs(value[i],intent,owner,path+"/"+std::to_string(i));
}
Entity without_qualified_history(const Entity& entity) {
    auto result=entity;
    if(entity.type=="wall" &&
        (entity.extensions.contains("wall_merge_archive") || entity.extensions.contains("wall_split_archive"))) {
        // The names alone do not make vendor metadata historical evidence.
        // Require a typed wall and replay both admitted archive envelopes.
        (void)read_wall(entity);validate_wall_merge_archive(entity);
        result.extensions.erase("wall_merge_archive");result.extensions.erase("wall_split_archive");
    }
    if(can_recognize_boundary_entity_type(entity.type) &&
        inspect_boundary_entity_version(entity).format==BoundaryEntityFormat::identified_v1 &&
        (entity.extensions.contains("boundary_geometry_derivation") || entity.properties.contains("boundary_authoring"))) {
        // Integrity replay admits only typed operations and reconciles their
        // origin, surviving children and final source with this actual owner.
        if(const auto diagnostic=validate_boundary_integrity(Entities{{entity.id,entity}}))reject(*diagnostic);
        result.extensions.erase("boundary_geometry_derivation");result.properties.erase("boundary_authoring");
    }
    return result;
}
std::optional<std::string> incoming_retired_reference(const Entities& entities,const std::string& retired) {
    for(const auto& [id,entity]:entities) {
        if(id==retired)continue;
        const auto unchecked=without_qualified_history(entity);
        try {
            const WallMergeIntent target{retired,retired};
            unknown_refs(unchecked.properties,target,id,"/properties");unknown_refs(unchecked.extensions,target,id,"/extensions");
        } catch(const std::invalid_argument& error) { return error.what(); }
    }
    return std::nullopt;
}
Json axis_dimension_metadata(const Entity& entity) {
    auto properties=entity.properties;properties.erase("text_position");properties.at("target").erase("entity_id");
    return {{"type",entity.type},{"required",entity.required},{"properties",std::move(properties)},{"extensions",entity.extensions}};
}
void collapse(Json& list,const WallMergeIntent& intent,const std::string& location) {
    if (!list.is_array()) reject("Wall merge membership is malformed at "+location);
    const bool a=mentions(list,intent.first_wall_id),b=mentions(list,intent.second_wall_id);
    if (a!=b) reject("Wall merge requires consistent membership at "+location);
    if (b) list.erase(std::remove(list.begin(),list.end(),Json(intent.second_wall_id)),list.end());
}
Json corner_station_quantity(double metres) {
    std::array<char,64> buffer{};
    for(int precision=17;precision>0;--precision) {
        const auto converted=std::to_chars(buffer.data(),buffer.data()+buffer.size(),metres,std::chars_format::general,precision);
        if(converted.ec!=std::errc{})continue;
        try {
            const auto quantity=parse_quantity(std::string(buffer.data(),converted.ptr)+" m",Unit::metre);
            if(quantity.metres!=metres)continue;
            return {{"version",1},{"original_expression",quantity.original_expression},{"entered_unit","m"},
                {"exact_metres",{{"numerator",quantity.exact_metres.numerator},{"denominator",quantity.exact_metres.denominator}}}};
        } catch(const std::invalid_argument&) { /* Try a shorter exact spelling. */ }
          catch(const std::overflow_error&) { /* Its rational may fit at lower precision. */ }
    }
    reject("Wall merge corner cut station has no exact bounded quantity input");
}
void corner_cut_station(const Entity& retained,Entity& cut,double offset) {
    const auto& source=retained.properties;
    for(const auto* key:{"offset_m","offset"})if(source.contains(key))
        cut.properties[key]=source.at(key).get<double>()==offset?source.at(key):Json(offset);
    const auto old=source.at(source.contains("offset_m")?"offset_m":"offset").get<double>();
    if(old==offset)return;
    const auto entries=source.find("quantity_entries");
    if(entries==source.end())return;
    if(!entries->is_object())reject("Wall merge corner cut quantity entries must be an object: "+cut.id);
    std::optional<Json> encoded;
    for(const auto* key:{"offset_m","offset"}) {
        const auto pointer="/"+std::string(key);
        const auto found=entries->find(pointer);
        if(found==entries->end())continue;
        if(!source.contains(key) || !found->is_object() || !found->contains("version") ||
            !found->at("version").is_number_integer() || found->at("version")!=1 ||
            !found->contains("original_expression") || !found->at("original_expression").is_string() ||
            found->at("original_expression").get_ref<const std::string&>().size()>4096 || found->dump().size()>1024*1024)
            reject("Wall merge cannot change an unsupported corner cut station receipt: "+cut.id+pointer);
        if(decode_constraint_quantity_receipt(*found).metres!=source.at(key).get<double>())
            reject("Wall merge corner cut station receipt is stale: "+cut.id+pointer);
        if(!encoded)encoded=corner_station_quantity(offset);
        auto& receipt=cut.properties.at("quantity_entries").at(pointer);
        // Preserve opaque receipt and exact_metres siblings; affected future
        // or core-free authority cannot be interpreted and must refuse.
        for(const auto* field:{"version","original_expression","entered_unit"})receipt[field]=encoded->at(field);
        for(const auto* field:{"numerator","denominator"})receipt["exact_metres"][field]=encoded->at("exact_metres").at(field);
    }
}
void complete_corner_window_hosts(const Entities& source,Entities& result,const WallMergeIntent& intent) {
    bool checked_source=false;
    for(const auto& [id,entity]:source) {
        if(entity.type!="corner_window")continue;
        auto window=parse_corner_window(entity);
        if(!std::any_of(window.wall_ids.begin(),window.wall_ids.end(),[&](const auto& host){return selected(host,intent);}))continue;
        if(!checked_source) {validate_corner_window_state(source);checked_source=true;}
        const auto original=window;
        if(selected(window.wall_ids[0],intent) && selected(window.wall_ids[1],intent))
            reject("Wall merge cannot collapse both corner-window hosts: "+id);
        for(std::size_t leg=0;leg<2;++leg)if(selected(original.wall_ids[leg],intent)) {
            // Only first.start and second.end survive, with unchanged roles.
            if((original.wall_ids[leg]==intent.first_wall_id && !original.at_start[leg]) ||
                (original.wall_ids[leg]==intent.second_wall_id && original.at_start[leg]))
                reject("Wall merge cannot remove a corner-window endpoint at its seam: "+id);
            if(result.at(window.opening_ids[leg]).properties.at("wall_id")!=intent.first_wall_id)
                reject("Wall merge cannot preserve corner-window host: "+id);
            window.wall_ids[leg]=intent.first_wall_id;
            if(original.wall_ids[leg]!=intent.first_wall_id)
                result.at(id).properties.at("wall_ids").at(leg)=intent.first_wall_id;
        }
        std::array<Wall,2> hosts;
        for(std::size_t leg=0;leg<2;++leg)hosts[leg]=read_wall(result.at(window.wall_ids[leg]));
        const auto cuts=corner_window_cuts(window,hosts);
        for(std::size_t leg=0;leg<2;++leg)if(selected(original.wall_ids[leg],intent)) {
            // Exact final-host derivation avoids a rounding difference from
            // adding the first span's length to the old child's station.
            corner_cut_station(source.at(window.opening_ids[leg]),result.at(window.opening_ids[leg]),cuts[leg].offset);
        }
    }
    if(checked_source)validate_corner_window_state(result);
}
bool seam(const WallEndpointBinding& b,const WallMergeIntent& intent) {
    return (b.owner_id==intent.first_wall_id && b.role==WallEndpointRole::end) ||
        (b.owner_id==intent.second_wall_id && b.role==WallEndpointRole::start);
}
void plain_binding(const Json& b,const std::string& id) {
    static const std::set<std::string,std::less<>> fields={"owner_id","feature","role","segment_id","vertex_id"};
    for(const auto& [key,value]:b.items()) { (void)value;
        if(!fields.contains(key))reject("Wall merge cannot discard seam binding metadata in "+id+"/"+key);
    }
}
void remap_constraints(const Entities& source,Entities& result,const WallMergeIntent& intent) {
    for(const auto& [id,entity]:source) if(entity.type=="constraint") {
        const auto decoded=decode_constraint_entity(entity);
        if(!decoded.supported()) { unknown_refs(entity.properties,intent,id,"/properties"); continue; }
        auto relation=*decoded.constraint;
        if(!std::any_of(relation.bindings.begin(),relation.bindings.end(),[&](const auto& b){return selected(b.owner_id,intent);}))continue;
        auto raw=entity.properties.at("bindings");
        if(relation.relation==ConstraintRelationKind::coincident && relation.bindings.size()==2 &&
            seam(relation.bindings[0],intent) && seam(relation.bindings[1],intent) &&
            relation.bindings[0].owner_id!=relation.bindings[1].owner_id) {
            const auto canonical=encode_constraint_entity(relation);
            if(entity.required!=canonical.required || entity.extensions!=canonical.extensions || entity.properties!=canonical.properties)
                reject("Wall merge cannot discard authored seam constraint metadata: "+id);
            if(const auto reference=incoming_retired_reference(result,id))
                reject("Wall merge cannot retire seam constraint "+id+": "+*reference);
            result.erase(id);continue;
        }
        std::vector<WallEndpointBinding> bindings; Json persisted=Json::array();
        for(std::size_t i=0;i<relation.bindings.size();) {
            const auto b=relation.bindings[i];
            if(relation.relation==ConstraintRelationKind::tangent && selected(b.owner_id,intent)) {
                if(i%2!=0 || i+1>=relation.bindings.size() || relation.bindings[i+1].owner_id!=b.owner_id ||
                    relation.bindings[i+1].role==b.role || !b.segment_id.empty() || seam(b,intent))
                    reject("Wall merge cannot preserve a seam tangent contact: "+id);
                for(std::size_t j=0;j<2;++j) {
                    auto part=relation.bindings[i+j];auto metadata=raw.at(i+j);
                    part.owner_id=intent.first_wall_id;metadata["owner_id"]=intent.first_wall_id;
                    bindings.push_back(part);persisted.push_back(metadata);
                }
                i+=2;continue;
            }
            if(relation.relation==ConstraintRelationKind::fixed_arc_length && selected(b.owner_id,intent)) {
                if(i+3>=relation.bindings.size())reject("Wall merge cannot preserve individual arc length lock: "+id);
                const auto c=relation.bindings[i+1],d=relation.bindings[i+2],e=relation.bindings[i+3];
                const bool forward=b.owner_id==intent.first_wall_id && b.role==WallEndpointRole::start;
                const bool reverse=b.owner_id==intent.second_wall_id && b.role==WallEndpointRole::end;
                if((!forward&&!reverse) || c.owner_id!=b.owner_id || c.role==b.role ||
                    d.owner_id!=(forward?intent.second_wall_id:intent.first_wall_id) || d.role!=b.role ||
                    e.owner_id!=d.owner_id || e.role==d.role || !b.segment_id.empty() || !c.segment_id.empty() ||
                    !d.segment_id.empty() || !e.segment_id.empty())reject("Wall merge requires a complete directed two-wall arc chain: "+id);
                plain_binding(raw.at(i+1),id);plain_binding(raw.at(i+2),id);
                auto start=b,end=e;start.owner_id=end.owner_id=intent.first_wall_id;
                auto start_json=raw.at(i),end_json=raw.at(i+3);
                start_json["owner_id"]=end_json["owner_id"]=intent.first_wall_id;
                bindings.push_back(start);bindings.push_back(end);persisted.push_back(start_json);persisted.push_back(end_json);i+=4;continue;
            }
            const bool direction_only=relation.relation==ConstraintRelationKind::horizontal || relation.relation==ConstraintRelationKind::vertical ||
                relation.relation==ConstraintRelationKind::parallel || relation.relation==ConstraintRelationKind::perpendicular;
            if(selected(b.owner_id,intent) && direction_only) {
                if(i%2!=0 || i+1>=relation.bindings.size())
                    reject("Wall merge cannot reinterpret partial direction binding: "+id);
                const auto c=relation.bindings[i+1];
                const bool one_wall=c.owner_id==b.owner_id && c.role!=b.role;
                const bool outer_pair=(b.owner_id==intent.first_wall_id && b.role==WallEndpointRole::start &&
                    c.owner_id==intent.second_wall_id && c.role==WallEndpointRole::end) ||
                    (b.owner_id==intent.second_wall_id && b.role==WallEndpointRole::end &&
                    c.owner_id==intent.first_wall_id && c.role==WallEndpointRole::start);
                if((!one_wall&&!outer_pair) || !b.segment_id.empty() || !c.segment_id.empty())
                    reject("Wall merge cannot reinterpret partial direction binding: "+id);
                for(std::size_t j=0;j<2;++j) {auto part=relation.bindings[i+j];auto metadata=raw.at(i+j);
                    part.owner_id=intent.first_wall_id;metadata["owner_id"]=intent.first_wall_id;
                    bindings.push_back(part);persisted.push_back(metadata);}
                i+=2;continue;
            }
            if(seam(b,intent))reject("Wall merge is blocked by a seam-pinned or individual-length constraint: "+id);
            auto part=b;auto metadata=raw.at(i);
            if(b.owner_id==intent.second_wall_id){part.owner_id=intent.first_wall_id;metadata["owner_id"]=intent.first_wall_id;}
            bindings.push_back(part);persisted.push_back(metadata);++i;
        }
        relation.bindings=std::move(bindings);
        auto encoded=encode_constraint_entity(relation,&entity);encoded.properties["bindings"]=std::move(persisted);
        (void)decode_constraint_entity(encoded);result.at(id)=std::move(encoded);
    }
}
}

Json encode_wall_merge(const WallMergeIntent& intent) {
    validate(intent);
    Json result={{"version",1},{"first_wall_id",intent.first_wall_id},{"second_wall_id",intent.second_wall_id}};
    if (intent.physical_room_phase_completion) {
        result["version"]=2;result["physical_room_phase_completion"]=true;
    }
    return result;
}
WallMergeIntent decode_wall_merge(const Json& value) {
    if (!value.is_object() || !value.contains("version") || !value.at("version").is_number_integer() ||
        (value.at("version")!=1 && value.at("version")!=2)) reject("Wall merge proof has unsupported version");
    const bool phase_completion=value.at("version")==2;
    if (phase_completion) exact(value,{"version","first_wall_id","second_wall_id","physical_room_phase_completion"});
    else exact(value,{"version","first_wall_id","second_wall_id"});
    if(!value.at("first_wall_id").is_string() ||
        !value.at("second_wall_id").is_string())reject("Wall merge proof has invalid version or identities");
    WallMergeIntent intent{value.at("first_wall_id").get<std::string>(),value.at("second_wall_id").get<std::string>()};
    if (phase_completion) {
        if (!value.at("physical_room_phase_completion").is_boolean() ||
            !value.at("physical_room_phase_completion").get<bool>())
            reject("Wall merge phase completion requires its explicit current-source dialect");
        intent.physical_room_phase_completion=true;
    }
    validate(intent);return intent;
}
void validate_wall_merge_archive(const Entity& wall) {
    const auto found=wall.extensions.find("wall_merge_archive");if(found==wall.extensions.end())return;
    if(found->dump().size()>1024*1024)reject("Wall merge source archive exceeds its one MiB budget: "+wall.id);
    std::vector<std::pair<Entity,std::size_t>> pending{{wall,0}};
    while(!pending.empty()) {
        auto [owner,depth]=std::move(pending.back());pending.pop_back();
        const auto archive=owner.extensions.find("wall_merge_archive");if(archive==owner.extensions.end())continue;
        if(depth>=64)reject("Wall merge archive exceeds its 64-level nesting budget");
        exact(*archive,{"version","sources"});
        if(!archive->at("version").is_number_integer() || archive->at("version")!=1 ||
            !archive->at("sources").is_array() || archive->at("sources").size()!=2)reject("Unsupported wall merge archive: "+owner.id);
        auto first=unarchive(archive->at("sources").at(0)),second=unarchive(archive->at("sources").at(1));
        if(first.id==second.id)reject("Wall merge archive requires distinct historical source identities");
        const auto a=read_wall(first),b=read_wall(second);compatible(first,second,a,b);(void)joined(a,b);
        pending.emplace_back(std::move(first),depth+1);pending.emplace_back(std::move(second),depth+1);
    }
}

Entities replayed_wall_merge_entities(const Entities& source,const WallMergeIntent& intent) {
    validate(intent);
    if(!source.contains(intent.first_wall_id) || !source.contains(intent.second_wall_id))reject("Wall merge source wall is unavailable");
    const auto& first=source.at(intent.first_wall_id);const auto& second=source.at(intent.second_wall_id);
    const auto a=read_wall(first),b=read_wall(second);compatible(first,second,a,b);
    validate_wall_merge_archive(first);validate_wall_merge_archive(second);
    validate_constraint_wall_host(first.id,source);validate_constraint_wall_host(second.id,source);
    const auto organization=organize_project(source);
    if(organization.drawing_context(first.id)!=organization.drawing_context(second.id))reject("Wall merge source hierarchy differs");
    const auto effective_first=read_wall(resolve_vertical_placement(source,first));
    const auto effective_second=read_wall(resolve_vertical_placement(source,second));
    compatible(first,second,effective_first,effective_second);
    const auto axis=joined(a,b);
    auto merged=reconstruct_exterior_corner_wall(first,axis);
    // Geometry entries are historical authored inputs. Archive both originals
    // and retain only unchanged non-geometric entries on the active wall.
    if(merged.properties.contains("quantity_entries")) {
        auto& entries=merged.properties.at("quantity_entries");
        for(auto it=entries.begin();it!=entries.end();) {
            const auto& key=it.key();
            if(key.starts_with("/baseline/") || key=="/length_m" || key=="/height_m" || key=="/height" ||
                key=="/slope_rise_m" || key=="/slope_rise" || key.starts_with("/top_plane/"))it=entries.erase(it);else ++it;
        }
        if(entries.empty())merged.properties.erase("quantity_entries");
    }
    merged.extensions.erase("wall_split_archive");
    merged.extensions["wall_merge_archive"]={{"version",1},{"sources",Json::array({archived(first),archived(second)})}};
    const auto gradient=wall_top_gradient(a);
    const Vec2 chord{axis.end.x-axis.start.x,axis.end.y-axis.start.y};
    merged.properties["height_m"]=a.height;
    if(merged.properties.contains("height"))merged.properties["height"]=a.height;
    merged.properties["top_plane"]=wall_top_plane_json(gradient);
    merged.properties["slope_rise_m"]=gradient.x*chord.x+gradient.y*chord.y;
    if(merged.properties.contains("slope_rise"))merged.properties["slope_rise"]=merged.properties.at("slope_rise_m");
    validate_wall_merge_archive(merged);
    auto result=source;result.at(first.id)=std::move(merged);result.erase(second.id);
    std::set<std::string> retired_joins;
    std::vector<std::string> first_automatic_axes,second_automatic_axes;
    for(const auto& [id,entity]:source) {
        auto unhandled=without_qualified_history(entity);
        if(can_recognize_boundary_dimension_entity_type(entity.type)) {
            const auto decoded=decode_boundary_dimension_entity(entity);
            if(decoded.supported() && selected(decoded.dimension->boundary_id,intent)) {
                const auto& dimension=*decoded.dimension;
                if(dimension.kind!=BoundaryDimensionKind::wall_axis_length)
                    reject("Wall merge cannot reinterpret an unsupported physical dimension target: "+id);
                auto& updated=result.at(id);
                updated.properties.at("target")["entity_id"]=first.id;
                if(dimension.placement==BoundaryDimensionPlacement::automatic) {
                    const auto midpoint=merge_point(axis,0.5);
                    const auto dx=axis.end.x-axis.start.x,dy=axis.end.y-axis.start.y,chord=std::hypot(dx,dy);
                    const auto clearance=a.thickness/2+0.25;
                    const Vec2 position{midpoint.x-dy/chord*clearance,midpoint.y+dx/chord*clearance};
                    if(!std::isfinite(position.x)||!std::isfinite(position.y))reject("Wall merge automatic axis dimension exceeds range: "+id);
                    updated.properties["text_position"]={position.x,position.y};
                    (dimension.boundary_id==first.id?first_automatic_axes:second_automatic_axes).push_back(id);
                }
                // Change only this admitted target. Manual coordinates,
                // presentation numbers and opaque metadata remain verbatim.
                unhandled.properties.at("target").erase("entity_id");
                const auto rehosted=decode_boundary_dimension_entity(updated);
                (void)rehosted.dimension->resolve(result);
            } else if(!decoded.supported()) {
                // Future dimensions have no admitted target semantics. The
                // reference scan below refuses any dependency on either wall.
                unknown_refs(entity.properties,intent,id,"/properties");
                unknown_refs(entity.extensions,intent,id,"/extensions");
            }
        }
        if(entity.type=="opening" && entity.properties.value("wall_id",std::string{})==second.id) {
            auto& opening=result.at(id);opening.properties["wall_id"]=first.id;
            if(!opening.properties.contains("corner_window_id")) {
                const auto key=opening.properties.contains("offset_m")?"offset_m":"offset";
                const auto station=opening.properties.at(key).get<double>()+segment_length(a.baseline);
                if(!std::isfinite(station))reject("Wall merge opening station exceeds range: "+id);
                opening.properties["offset_m"]=station;if(opening.properties.contains("offset"))opening.properties["offset"]=station;
            }
        }
        if(entity.type=="opening")unhandled.properties.erase("wall_id");
        if(entity.type=="corner_window") {
            (void)parse_corner_window(entity);
            unhandled.properties.erase("wall_ids");
        }
        if(entity.type=="room_relationships") {
            const auto relationships=RoomRelationshipSnapshot::from_json(entity.properties.at("model"));
            auto& model=result.at(id).properties.at("model");
            for(const auto& reference:relationships.references())if(reference.kind==RoomReferenceKind::architectural_wall) {
                auto members=room_reference_wall_ids(reference);
                if(std::find(members.begin(),members.end(),first.id)==members.end() && std::find(members.begin(),members.end(),second.id)==members.end())continue;
                const auto ai=std::find(members.begin(),members.end(),first.id),bi=std::find(members.begin(),members.end(),second.id);
                if(ai==members.end() || bi==members.end() || (std::next(ai)!=bi && std::next(bi)!=ai))
                    reject("Wall merge room reference must contain both adjacent directed members: "+reference.id);
                members.erase(bi);
                for(auto& encoded:model.at("references"))if(encoded.at("id")==reference.id) {
                    const auto previous_id=reference.id;
                    encoded["id"]=members.front();
                    if(members.size()==1)encoded.erase("wall_members");else encoded["wall_members"]=members;
                    if(previous_id!=members.front())for(auto& relation:model.at("relations"))
                        for(const auto* key:{"source_id","target_id"})if(relation.at(key)==previous_id)relation[key]=members.front();
                }
                model["schema_version"]=2;
            }
            (void)RoomRelationshipSnapshot::from_json(model);unhandled.properties.erase("model");
        }
        if(entity.type=="wall_join") {
            auto join=parse_wall_join(entity.properties,id);
            std::vector<std::string> members;
            for(const auto& member:join.wall_ids) {
                const auto& replacement=member==second.id?first.id:member;
                if(std::find(members.begin(),members.end(),replacement)==members.end())members.push_back(replacement);
            }
            join.wall_ids=std::move(members);
            if(join.wall_ids.size()==1) {
                // The explicitly previewed merge replaces this pair's fused
                // physical body. Its complete entity remains in source history.
                retired_joins.insert(id);result.erase(id);
            } else {
                validate_wall_join_semantics(join);
                for(const auto& member:join.wall_ids) {
                    const auto wall=result.find(member);
                    if(wall==result.end() || wall->second.type!="wall")reject("Wall merge join has an unavailable wall: "+id+"/"+member);
                    (void)read_wall(wall->second);
                }
                result.at(id).properties["wall_ids"]=join.wall_ids;
            }
            unhandled.properties.erase("wall_ids");
        }
        if(entity.type=="model_phases") {
            auto& model=result.at(id).properties.at("model");
            collapse(model.at("entity_ids"),intent,id+"/entity_ids");collapse(model.at("baseline_ids"),intent,id+"/baseline_ids");
            for(auto& alternative:model.at("alternatives"))for(const auto* key:{"demolished_ids","proposed_ids"})
                collapse(alternative.at(key),intent,id+"/alternatives/"+key);
            (void)ModelPhases::from_json(model);unhandled.properties.erase("model");
        }
        if(entity.type=="sheet_view_model") {
            auto& model=result.at(id).properties.at("model");
            for(auto& view:model.at("views")) {
                collapse(view.at("object_ids"),intent,id+"/views/object_ids");
                if(view.at("presentation").contains("appearance") && !view.at("presentation").at("appearance").is_null()) {
                    auto& objects=view["presentation"]["appearance"]["objects"];
                    auto ai=std::find_if(objects.begin(),objects.end(),[&](const auto& style){return style.at("object_id")==first.id;});
                    auto bi=std::find_if(objects.begin(),objects.end(),[&](const auto& style){return style.at("object_id")==second.id;});
                    if((ai==objects.end())!=(bi==objects.end()))reject("Wall merge saved-view appearance membership differs: "+id);
                    if(ai!=objects.end()) {auto sa=*ai,sb=*bi;sa.erase("object_id");sb.erase("object_id");
                        if(sa!=sb)reject("Wall merge saved-view appearances conflict: "+id);objects.erase(bi);}
                }
            }
            (void)decode_sheet_view_entity(result.at(id));
            for(auto& view:unhandled.properties.at("model").at("views")) {
                view.erase("object_ids");
                if(view.at("presentation").contains("appearance") && !view.at("presentation").at("appearance").is_null())
                    for(auto& style:view["presentation"]["appearance"]["objects"])style.erase("object_id");
            }
        }
        if(entity.type=="constraint") {
            if(unhandled.properties.contains("bindings"))for(auto& binding:unhandled.properties.at("bindings"))binding.erase("owner_id");
            unhandled.properties.erase("wall_ids");unhandled.properties.erase("entity_ids");
        }
        if(entity.type=="measurement_boundary")unhandled.properties.erase("wall_measurement_source");
        if (is_physical_wall_room(entity)) {
            const auto& marker = entity.extensions.at("physical_wall_room");
            if (marker.is_object() && marker.contains("version") && marker.at("version") == 1) {
                (void)decode_physical_wall_room_descriptor(entity);
#ifdef VERTEX_HAS_PHYSICAL_ROOM_REVIEW
                // A typed room marker captures historical source inventory.
                // The completion below independently continues only initially
                // current rooms; stale evidence remains visible and untouched.
                unhandled.extensions.erase("physical_wall_room");
#else
                try {
                    unknown_refs(marker, intent, id, "/extensions/physical_wall_room");
                } catch (const std::invalid_argument&) {
                    reject("Wall merge requires the architectural geometry engine to preserve physical room " + id);
                }
#endif
            }
        }
        if(retired_joins.contains(id))continue;
        unknown_refs(unhandled.properties,intent,id,"/properties");unknown_refs(unhandled.extensions,intent,id,"/extensions");
    }
    complete_corner_window_hosts(source,result,intent);
    for(const auto& id:retired_joins)
        if(const auto reference=incoming_retired_reference(result,id))reject("Wall merge cannot retire join "+id+": "+*reference);
    for(const auto& second_dimension:second_automatic_axes) {
        const auto metadata=axis_dimension_metadata(result.at(second_dimension)).dump();
        const auto equivalent=std::find_if(first_automatic_axes.begin(),first_automatic_axes.end(),[&](const auto& id) {
            return axis_dimension_metadata(result.at(id)).dump()==metadata;
        });
        // Referenced or differently styled automatic dimensions remain useful
        // as distinct placements. Only an unreferenced equivalent duplicate
        // can retire; all manual placements are retained.
        if(equivalent!=first_automatic_axes.end() && !incoming_retired_reference(result,second_dimension))
            result.erase(second_dimension);
    }
    remap_constraints(source,result,intent);
    validate_constraint_wall_host(first.id,result);
    result=complete_wall_merge_measurement_sources(source,result,intent);
#ifdef VERTEX_HAS_PHYSICAL_ROOM_REVIEW
    result=complete_wall_merge_physical_room_sources(source,result,intent,intent.physical_room_phase_completion);
#else
    if (intent.physical_room_phase_completion)
        reject("Current physical room wall merging requires the architectural geometry engine");
#endif
    if(const auto diagnostic=validate_boundary_integrity(result))reject(*diagnostic);
    if(const auto diagnostic=validate_constraint_integrity(result))reject(*diagnostic);
    validate_constraint_edit_topology(wall_merge_validation_source(source,result,intent),result);
    return result;
}

Entities wall_merge_validation_source(const Entities& source,const Entities& reconstructed,const WallMergeIntent& intent) {
    auto normalized=source;normalized.erase(intent.second_wall_id);
    for(const auto& [id,entity]:source) {
        const auto after=reconstructed.find(id);
        if(after==reconstructed.end())normalized.erase(id);
        else if(entity!=after->second)normalized.at(id)=after->second;
    }
    return normalized;
}
Command make_wall_merge_command(const DocumentSnapshot& source,const WallMergeIntent& intent) {
    auto captured=intent;
#ifdef VERTEX_HAS_PHYSICAL_ROOM_REVIEW
    captured.physical_room_phase_completion=true;
#endif
    (void)replayed_wall_merge_entities(source.entities(),captured);
    ApplyBoundaryConstraintChanges command;command.expected_revision=source.revision();command.message="Merge walls";command.wall_merge=std::move(captured);
    const Command result{command};(void)Document::preview_command(source,result);return result;
}
}
