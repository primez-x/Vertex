#include "sketch/wall_split.hpp"
#include "sketch/wall_merge.hpp"
#include "sketch/constraint_entity.hpp"
#include "sketch/constraint_integrity.hpp"
#include "sketch/constraint_wall_edit.hpp"
#include "sketch/wall_measurement.hpp"
#include "sketch/wall_semantics.hpp"
#include "sketch/document_wall.hpp"
#include "sketch/boundary_integrity.hpp"
#include "sketch/boundary_dimension.hpp"
#include "sketch/model_phases.hpp"
#include "sketch/sheet_view_entity_codec.hpp"
#include "sketch/room_relationships.hpp"
#include "sketch/physical_wall_room_data.hpp"
#ifdef VERTEX_HAS_PHYSICAL_ROOM_REVIEW
#include "sketch/physical_wall_room_split.hpp"
#endif
#include <algorithm>
#include <cmath>
#include <iterator>
#include <set>
#include <stdexcept>

namespace sketch {
namespace {
using Json=nlohmann::json;
using Entities=std::map<std::string,Entity,std::less<>>;
[[noreturn]] void reject(const std::string& message){throw std::invalid_argument(message);}
bool valid_id(const std::string& id) {
    if(id.empty() || id.size()>128)return false;
    return std::all_of(id.begin(),id.end(),[](unsigned char c){return (c>='a'&&c<='z') ||
        (c>='A'&&c<='Z') || (c>='0'&&c<='9') || c=='-' || c=='_' || c=='.' || c==':';});
}
void exact(const Json& value,std::initializer_list<const char*> fields) {
    if(!value.is_object() || value.size()!=fields.size())reject("Wall split proof has unexpected fields");
    for(const auto* key:fields)if(!value.contains(key))reject("Wall split proof is missing "+std::string(key));
}
void validate(const WallSplitIntent& intent) {
    if(!valid_id(intent.wall_id) || !valid_id(intent.second_wall_id) || !valid_id(intent.seam_constraint_id) ||
        intent.wall_id==intent.second_wall_id || intent.wall_id==intent.seam_constraint_id ||
        intent.second_wall_id==intent.seam_constraint_id || !std::isfinite(intent.fraction) ||
        !(intent.fraction>0 && intent.fraction<1) || intent.measured_owners.size()>10000)
        reject("Wall split requires distinct identities and a strict interior fraction");
    std::set<std::string> owners;
    for(const auto& ids:intent.measured_owners)
        if(!valid_id(ids.boundary_id) || !valid_id(ids.vertex_id) || !valid_id(ids.segment_id) ||
            (!ids.automatic_dimension_id.empty() && !valid_id(ids.automatic_dimension_id)) ||
            !owners.insert(ids.boundary_id).second)reject("Wall split measured identities are invalid or repeated");
    if ((!intent.physical_room_completion && !intent.physical_room_owners.empty()) ||
        intent.physical_room_owners.size()>2048)
        reject("Wall split room continuation requires its explicit bounded completion dialect");
    std::set<std::string> fresh{intent.second_wall_id,intent.seam_constraint_id};
    const auto reserve=[&](const std::string& id) {
        if (!valid_id(id) || id==intent.wall_id || !fresh.insert(id).second)
            reject("Wall split requires distinct fresh child identities");
    };
    for (const auto& ids:intent.measured_owners) {
        reserve(ids.vertex_id);reserve(ids.segment_id);
        if (!ids.automatic_dimension_id.empty()) reserve(ids.automatic_dimension_id);
    }
    for (const auto& ids:intent.physical_room_owners) {
        if (!valid_id(ids.boundary_id) || !owners.insert(ids.boundary_id).second ||
            ids.new_segment_ids.size()!=ids.new_vertex_ids.size() || ids.new_segment_ids.size()>10000)
            reject("Wall split room child allocation is invalid or repeated");
        for (const auto& id:ids.new_segment_ids) reserve(id);
        for (const auto& id:ids.new_vertex_ids) reserve(id);
    }
}
Segment baseline(const Entity& wall) {
    const auto& b=wall.properties.at("baseline");
    return {{b.at("start").at(0).get<double>(),b.at("start").at(1).get<double>()},
        {b.at("end").at(0).get<double>(),b.at("end").at(1).get<double>()},b.at("sweep_radians").get<double>()};
}
Vec2 split_point(const Segment& segment,double fraction) {
    Vec2 p{std::lerp(segment.start.x,segment.end.x,fraction),std::lerp(segment.start.y,segment.end.y,fraction)};
    if(segment.sweep_radians!=0) {
        const auto dx=segment.end.x-segment.start.x,dy=segment.end.y-segment.start.y;
        const auto k=0.5/std::tan(segment.sweep_radians/2);
        const Vec2 center{segment.start.x+dx/2-dy*k,segment.start.y+dy/2+dx*k};
        const auto angle=segment.sweep_radians*fraction;
        const auto x=segment.start.x-center.x,y=segment.start.y-center.y;
        p={center.x+x*std::cos(angle)-y*std::sin(angle),center.y+x*std::sin(angle)+y*std::cos(angle)};
    }
    if(!std::isfinite(p.x) || !std::isfinite(p.y))reject("Wall split seam exceeds analytical range");
    return p;
}
bool contains(const Json& value,const std::string& id) {
    return value.is_array() && std::find(value.begin(),value.end(),Json(id))!=value.end();
}
void append(Json& list,const WallSplitIntent& intent) {
    if(contains(list,intent.wall_id)) {list.push_back(intent.second_wall_id);
        std::sort(list.begin(),list.end(),[](const Json& a,const Json& b){return a.get<std::string>()<b.get<std::string>();});}
}
bool canonical(std::string_view key) {
    static const std::set<std::string,std::less<>> keys={"wall_id","wall_ids","entity_id","entity_ids",
        "object_id","object_ids","host_id","host_ids","target_id","target_ids","source_entity_id",
        "source_entity_ids","source_id","source_ids","parent_id","parent_ids","owner_id",
        "host_entity_id","host_entity_ids","refs","references","wall_members"};
    return keys.contains(key);
}
bool mentions(const Json& value,const std::string& id) {
    if(value.is_string())return value==id;
    if(value.is_array())return std::any_of(value.begin(),value.end(),[&](const auto& child){return mentions(child,id);});
    return false;
}
void unknown_refs(const Json& value,const WallSplitIntent& intent,const std::string& owner,std::string path) {
    if(value.is_object())for(const auto& [key,child]:value.items()) {
        if(canonical(key) && mentions(child,intent.wall_id))
            reject("Wall split has unsupported whole-object reference in "+owner+" at "+path+"/"+key);
        unknown_refs(child,intent,owner,path+"/"+key);
    } else if(value.is_array())for(std::size_t i=0;i<value.size();++i)unknown_refs(value[i],intent,owner,path+"/"+std::to_string(i));
}
void remap_constraints(const Entities& source,Entities& result,const WallSplitIntent& intent) {
    for(const auto& [id,entity]:source)if(entity.type=="constraint") {
        const auto decoded=decode_constraint_entity(entity);
        if(!decoded.supported())reject("Wall split cannot preserve unsupported constraint "+id+": "+decoded.unsupported_reason);
        auto relation=*decoded.constraint;
        auto raw=entity.properties.at("bindings");
        std::vector<WallEndpointBinding> bindings; Json persisted=Json::array();
        for(std::size_t i=0;i<relation.bindings.size();) {
            const auto b=relation.bindings[i];
            if(relation.relation==ConstraintRelationKind::fixed_arc_length && b.owner_id==intent.wall_id) {
                if(i+1>=relation.bindings.size() || relation.bindings[i+1].owner_id!=intent.wall_id ||
                    !b.segment_id.empty() || b.role==relation.bindings[i+1].role)
                    reject("Wall split cannot partition malformed arc binding: "+id);
                const bool forward=b.role==WallEndpointRole::start;
                const std::string first=forward ? intent.wall_id : intent.second_wall_id;
                const std::string second=forward ? intent.second_wall_id : intent.wall_id;
                std::size_t piece_index=0;
                for(const auto& owner:{first,second}) {
                    for(std::size_t j=0;j<2;++j) {
                        auto part=relation.bindings[i+j];part.owner_id=owner;
                        const bool original_endpoint=(piece_index==0 && j==0) || (piece_index==1 && j==1);
                        auto metadata=original_endpoint ? raw.at(i+j) : Json{{"feature","baseline"},
                            {"role",part.role==WallEndpointRole::start ? "start" : "end"}};
                        metadata["owner_id"]=owner;
                        bindings.push_back(part);persisted.push_back(std::move(metadata));
                    }
                    ++piece_index;
                }
                i+=2;
            } else {
                auto part=b;auto metadata=raw.at(i);
                if(b.owner_id==intent.wall_id && b.role==WallEndpointRole::end) {
                    part.owner_id=intent.second_wall_id;metadata["owner_id"]=intent.second_wall_id;
                }
                bindings.push_back(part);persisted.push_back(std::move(metadata));++i;
            }
        }
        relation.bindings=std::move(bindings);
        auto encoded=encode_constraint_entity(relation,&entity);
        encoded.properties["bindings"]=std::move(persisted);
        (void)decode_constraint_entity(encoded);result.at(id)=std::move(encoded);
    }
}
}

Json encode_wall_split(const WallSplitIntent& intent) {
    validate(intent);auto owners=Json::array();
    for(const auto& ids:intent.measured_owners)owners.push_back({{"boundary_id",ids.boundary_id},{"vertex_id",ids.vertex_id},
        {"segment_id",ids.segment_id},{"automatic_dimension_id",ids.automatic_dimension_id}});
    Json result={{"version",1},{"wall_id",intent.wall_id},{"second_wall_id",intent.second_wall_id},
        {"fraction",intent.fraction},{"seam_constraint_id",intent.seam_constraint_id},{"measured_owners",std::move(owners)}};
    if (intent.physical_room_completion) {
        auto rooms=Json::array();
        for (const auto& ids:intent.physical_room_owners) rooms.push_back({{"boundary_id",ids.boundary_id},
            {"new_segment_ids",ids.new_segment_ids},{"new_vertex_ids",ids.new_vertex_ids}});
        result["version"]=2;result["physical_room_owners"]=std::move(rooms);
    }
    if(result.dump().size()>1024*1024)reject("Wall split proof exceeds the persisted proof budget");
    return result;
}
WallSplitIntent decode_wall_split(const Json& value) {
    if(value.dump().size()>1024*1024)reject("Wall split proof exceeds the persisted proof budget");
    if (!value.is_object() || !value.contains("version") || !value.at("version").is_number_integer() ||
        (value.at("version")!=1 && value.at("version")!=2)) reject("Wall split proof has unsupported version");
    const bool room_completion=value.at("version")==2;
    if (room_completion) exact(value,{"version","wall_id","second_wall_id","fraction","seam_constraint_id","measured_owners","physical_room_owners"});
    else exact(value,{"version","wall_id","second_wall_id","fraction","seam_constraint_id","measured_owners"});
    if(!value.at("fraction").is_number() ||
        !value.at("measured_owners").is_array())reject("Wall split proof has invalid version or identities");
    WallSplitIntent intent{value.at("wall_id").get<std::string>(),value.at("second_wall_id").get<std::string>(),
        value.at("fraction").get<double>(),value.at("seam_constraint_id").get<std::string>(),{}};
    for(const auto& ids:value.at("measured_owners")) {
        exact(ids,{"boundary_id","vertex_id","segment_id","automatic_dimension_id"});
        intent.measured_owners.push_back({ids.at("boundary_id").get<std::string>(),ids.at("vertex_id").get<std::string>(),
            ids.at("segment_id").get<std::string>(),ids.at("automatic_dimension_id").get<std::string>()});
    }
    intent.physical_room_completion=room_completion;
    if (room_completion) {
        if (!value.at("physical_room_owners").is_array()) reject("Wall split room allocation must be an array");
        for (const auto& ids:value.at("physical_room_owners")) {
            exact(ids,{"boundary_id","new_segment_ids","new_vertex_ids"});
            intent.physical_room_owners.push_back({ids.at("boundary_id").get<std::string>(),
                ids.at("new_segment_ids").get<std::vector<std::string>>(),ids.at("new_vertex_ids").get<std::vector<std::string>>()});
        }
    }
    validate(intent);return intent;
}

static Entities replay_wall_split(const Entities& source,const WallSplitIntent& intent,
    bool retained_replay,bool preparation_only) {
    validate(intent);
    const auto found=source.find(intent.wall_id);
    if(found==source.end() || found->second.type!="wall")reject("Wall split source wall is unavailable: "+intent.wall_id);
    if(source.contains(intent.second_wall_id) || source.contains(intent.seam_constraint_id))reject("Wall split requires fresh entity identities");
    validate_constraint_wall_host(intent.wall_id,source);
    const auto old=baseline(found->second);
    const auto point=split_point(old,intent.fraction);
    const Segment first{old.start,point,old.sweep_radians*intent.fraction};
    const Segment second{point,old.end,old.sweep_radians*(1-intent.fraction)};
    if(segment_length(first)<=default_geometry_tolerance_metres || segment_length(second)<=default_geometry_tolerance_metres ||
        std::abs(segment_length(first)+segment_length(second)-segment_length(old))>default_geometry_tolerance_metres)
        reject("Wall split children do not reconstruct a nondegenerate analytical span");
    auto result=source;
    result.at(intent.wall_id)=reconstruct_split_wall(found->second,first,intent.fraction,false);
    auto child=reconstruct_split_wall(found->second,second,intent.fraction,true);child.id=intent.second_wall_id;
    const auto station=segment_length(old)*intent.fraction;
    const bool retained_plane=found->second.properties.contains("top_plane");
    const auto scalar=found->second.properties.find("slope_rise_m");
    const auto legacy_scalar=found->second.properties.find("slope_rise");
    const auto* rise_value=scalar!=found->second.properties.end() ? &*scalar :
        legacy_scalar!=found->second.properties.end() ? &*legacy_scalar : nullptr;
    if (retained_plane || (old.sweep_radians!=0.0 && rise_value &&
        std::abs(rise_value->get<double>())>default_geometry_tolerance_metres)) {
        Wall wall;
        std::string diagnostic;
        if (!read_document_wall(found->second,{},wall,diagnostic)) reject(diagnostic);
        validate_wall_semantics(wall);
        const auto gradient=wall_top_gradient(wall);
        const auto inherited=wall_top_plane_json(gradient);
        const auto set_top=[&](Entity& piece,const Segment& axis,double height) {
            const Vec2 chord{axis.end.x-axis.start.x,axis.end.y-axis.start.y};
            const auto rise=gradient.x*chord.x+gradient.y*chord.y;
            if (!std::isfinite(height) || !std::isfinite(rise))
                reject("Split wall top height exceeds the supported range");
            piece.properties["top_plane"]=inherited;
            piece.properties["height_m"]=height;
            piece.properties["slope_rise_m"]=rise;
            if (piece.properties.contains("height")) piece.properties["height"]=height;
            if (piece.properties.contains("slope_rise")) piece.properties["slope_rise"]=rise;
        };
        set_top(result.at(intent.wall_id),first,wall.height);
        set_top(child,second,wall_top_height(wall,station));
    } else if(rise_value) {
        Wall wall;
        std::string diagnostic;
        if (!read_document_wall(found->second,{},wall,diagnostic)) reject(diagnostic);
        const auto rise=wall.slope_rise.value_or(0.0);
        result.at(intent.wall_id).properties["slope_rise_m"]=rise*intent.fraction;
        child.properties["slope_rise_m"]=rise*(1-intent.fraction);
        if (result.at(intent.wall_id).properties.contains("slope_rise"))
            result.at(intent.wall_id).properties["slope_rise"]=rise*intent.fraction;
        if (child.properties.contains("slope_rise"))
            child.properties["slope_rise"]=rise*(1-intent.fraction);
        child.properties["height_m"]=wall.height+rise*intent.fraction;
        if (child.properties.contains("height")) child.properties["height"]=child.properties["height_m"];
    }
    result.emplace(child.id,std::move(child));
    for(const auto& [id,entity]:source) {
        auto unhandled=entity;
        if ((intent.physical_room_completion || preparation_only) &&
            can_recognize_boundary_dimension_entity_type(entity.type)) {
            const auto decoded=decode_boundary_dimension_entity(entity);
            if (decoded.supported() && decoded.dimension->boundary_id==intent.wall_id) {
                const auto& dimension=*decoded.dimension;
                if (dimension.kind!=BoundaryDimensionKind::wall_axis_length)
                    reject("Wall split cannot reinterpret an unsupported physical dimension target: "+id);
                // A wall-axis dimension measures one physical owner. The
                // surviving first wall retains that identity; it must not
                // silently become a dimension of the complete two-wall span.
                auto& updated=result.at(id);
                if (dimension.placement==BoundaryDimensionPlacement::automatic) {
                    Wall wall;std::string diagnostic;
                    if (!read_document_wall(result.at(intent.wall_id),{},wall,diagnostic)) reject(diagnostic);
                    validate_wall_semantics(wall);
                    const auto midpoint=split_point(first,0.5);
                    const auto dx=first.end.x-first.start.x,dy=first.end.y-first.start.y,chord=std::hypot(dx,dy);
                    const auto clearance=wall.thickness/2+0.25;
                    const Vec2 position{midpoint.x-dy/chord*clearance,midpoint.y+dx/chord*clearance};
                    if (!std::isfinite(position.x) || !std::isfinite(position.y))
                        reject("Wall split automatic axis dimension exceeds range: "+id);
                    updated.properties["text_position"]={position.x,position.y};
                }
                // Manual placement, presentation and adjacent opaque data stay
                // verbatim. Only the admitted canonical target is exempted.
                unhandled.properties.at("target").erase("entity_id");
                const auto rehosted=decode_boundary_dimension_entity(updated);
                (void)rehosted.dimension->resolve(result);
            }
        }
        if(entity.type=="room_relationships") {
            const auto relationships=RoomRelationshipSnapshot::from_json(entity.properties.at("model"));
            auto& model=result.at(id).properties.at("model");
            for(const auto& reference:relationships.references()) {
                if(reference.kind!=RoomReferenceKind::architectural_wall)continue;
                auto members=room_reference_wall_ids(reference);
                const auto selected=std::find(members.begin(),members.end(),intent.wall_id);
                if(selected==members.end())continue;
                members.insert(std::next(selected),intent.second_wall_id);
                for(auto& encoded:model.at("references"))if(encoded.at("id")==reference.id)encoded["wall_members"]=members;
                model["schema_version"]=2;
            }
            (void)RoomRelationshipSnapshot::from_json(model);
            unhandled.properties.erase("model");
        }
        if(entity.type=="opening" && entity.properties.value("wall_id",std::string{})==intent.wall_id) {
            const auto offset=entity.properties.at("offset_m").get<double>();
            const auto end=offset+entity.properties.at("width_m").get<double>();
            if(offset<station && end>station)reject("Wall split opening "+id+" straddles seam "+std::to_string(station)+
                " in range ["+std::to_string(offset)+","+std::to_string(end)+"]");
            if(offset>=station){result.at(id).properties["wall_id"]=intent.second_wall_id;result.at(id).properties["offset_m"]=offset-station;}
            unhandled.properties.erase("wall_id");
        }
        if(entity.type=="wall_join") {
            auto join=parse_wall_join(entity.properties,id);
            if(std::find(join.wall_ids.begin(),join.wall_ids.end(),intent.wall_id)!=join.wall_ids.end()) {
                if(join.wall_ids.size()>=32)reject("Wall split join exceeds 32 walls: "+id);
                join.wall_ids.push_back(intent.second_wall_id);validate_wall_join_semantics(join);
                result.at(id).properties["wall_ids"]=join.wall_ids;
            }
            unhandled.properties.erase("wall_ids");
        }
        if(entity.type=="model_phases") {
            auto& model=result.at(id).properties.at("model");
            append(model["entity_ids"],intent);append(model["baseline_ids"],intent);
            for(auto& alternative:model["alternatives"]){append(alternative["demolished_ids"],intent);append(alternative["proposed_ids"],intent);}
            (void)ModelPhases::from_json(model);unhandled.properties.erase("model");
        }
        if(entity.type=="sheet_view_model") {
            auto& model=result.at(id).properties.at("model");
            for(auto& view:model.at("views")) {
                append(view["object_ids"],intent);
                if(view.at("presentation").contains("appearance") && !view.at("presentation").at("appearance").is_null()) {
                    auto& objects=view["presentation"]["appearance"]["objects"];
                    auto additions=Json::array();
                    for(const auto& appearance:objects)if(appearance.at("object_id")==intent.wall_id) {
                        auto copied=appearance;copied["object_id"]=intent.second_wall_id;additions.push_back(std::move(copied));}
                    for(auto& addition:additions)objects.push_back(std::move(addition));
                }
                // Overlay object references, particularly section dimension
                // bindings, describe the whole old object and cannot shorten.
                unknown_refs(view.at("overlays"),intent,id,"/model/views/overlays");
            }
            (void)decode_sheet_view_entity(result.at(id));
            for(auto& view:unhandled.properties.at("model").at("views")) {
                view.erase("object_ids");
                if(view.at("presentation").contains("appearance") && !view.at("presentation").at("appearance").is_null())
                    for(auto& appearance:view["presentation"]["appearance"]["objects"])appearance.erase("object_id");
            }
        }
        if(entity.type=="constraint") {
            for(auto& binding:unhandled.properties.at("bindings"))binding.erase("owner_id");
            unhandled.properties.erase("wall_ids");unhandled.properties.erase("entity_ids");
        }
        if(entity.type=="measurement_boundary")unhandled.properties.erase("wall_measurement_source");
        if (can_recognize_boundary_entity_type(entity.type) &&
            inspect_boundary_entity_version(entity).format==BoundaryEntityFormat::identified_v1 &&
            (entity.extensions.contains("boundary_geometry_derivation") || entity.properties.contains("boundary_authoring"))) {
            if (const auto diagnostic=validate_boundary_integrity({{id,entity}})) reject(*diagnostic);
            unhandled.extensions.erase("boundary_geometry_derivation");unhandled.properties.erase("boundary_authoring");
        }
        if (is_physical_wall_room(entity)) {
            const auto& marker=entity.extensions.at("physical_wall_room");
            if (marker.is_object() && marker.contains("version") && marker.at("version")==1) {
                (void)decode_physical_wall_room_descriptor(entity);
#ifdef VERTEX_HAS_PHYSICAL_ROOM_REVIEW
                unhandled.extensions.erase("physical_wall_room");
#else
                try { unknown_refs(marker,intent,id,"/extensions/physical_wall_room"); }
                catch (const std::invalid_argument&) {
                    reject("Wall split requires the architectural geometry engine to preserve physical room "+id);
                }
#endif
            }
        }
        if(entity.type=="wall" && entity.extensions.contains("wall_split_archive")) {
            validate_wall_split_archive(entity);
            // These identities identify historical source receipts. They are
            // independently validated, rather than current model references.
            unhandled.extensions.erase("wall_split_archive");
        }
        if(entity.type=="wall" && entity.extensions.contains("wall_merge_archive")) {
            validate_wall_merge_archive(entity);
            // Full originals are historical construction evidence, not live
            // hosts. Both split children retain that evidence unchanged.
            unhandled.extensions.erase("wall_merge_archive");
        }
        unknown_refs(unhandled.properties,intent,id,"/properties");unknown_refs(unhandled.extensions,intent,id,"/extensions");
    }
    remap_constraints(source,result,intent);
    PersistentConstraint seam{intent.seam_constraint_id,ConstraintRelationKind::coincident,
        {{intent.wall_id,WallEndpointRole::end},{intent.second_wall_id,WallEndpointRole::start}},std::nullopt,std::nullopt};
    result.emplace(seam.id,encode_constraint_entity(seam));
    validate_constraint_wall_host(intent.wall_id,result);validate_constraint_wall_host(intent.second_wall_id,result);
    result=complete_wall_split_measurement_sources(source,result,intent);
#ifdef VERTEX_HAS_PHYSICAL_ROOM_REVIEW
    if (!preparation_only) {
        if (intent.physical_room_completion)
            result=complete_wall_split_physical_room_sources(source,result,intent);
        else if (!retained_replay && !prepare_wall_split_physical_room_ids(source,result,intent).empty())
            reject("Wall split requires captured room continuation; prepare the split command before applying it");
    }
#else
    (void)retained_replay;(void)preparation_only;
    if (intent.physical_room_completion)
        reject("Physical room wall splitting requires the architectural geometry engine");
#endif
    if(const auto unsupported=validate_boundary_integrity(result))reject(*unsupported);
    if(const auto unsupported=validate_constraint_integrity(result))reject(*unsupported);
    // Existing source topology is replaced by its proved directed children in
    // a detached comparison; the ordinary topology rules remain unchanged.
    auto normalized=wall_split_validation_source(source,result,intent);
    validate_constraint_edit_topology(normalized,result);
    return result;
}

Entities replayed_wall_split_entities(const Entities& source,const WallSplitIntent& intent,bool retained_replay) {
    return replay_wall_split(source,intent,retained_replay,false);
}

Entities wall_split_validation_source(const Entities& source,const Entities& reconstructed,const WallSplitIntent& intent) {
    auto normalized=source;
    for(const auto& [id,entity]:reconstructed) {
        if(id==intent.wall_id || id==intent.second_wall_id || entity.type=="constraint" || entity.type=="room_relationships" ||
            std::any_of(intent.measured_owners.begin(),intent.measured_owners.end(),[&](const auto& owner){return owner.boundary_id==id;}) ||
            std::any_of(intent.physical_room_owners.begin(),intent.physical_room_owners.end(),[&](const auto& owner){return owner.boundary_id==id;}))
            normalized.insert_or_assign(id,entity);
    }
    return normalized;
}

Command make_wall_split_command(const DocumentSnapshot& source,const WallSplitIntent& intent) {
    auto captured=intent;
#ifdef VERTEX_HAS_PHYSICAL_ROOM_REVIEW
    if (!captured.physical_room_completion) {
        const auto physical=replay_wall_split(source.entities(),captured,true,true);
        captured.physical_room_owners=prepare_wall_split_physical_room_ids(source.entities(),physical,captured);
        captured.physical_room_completion=true;
    }
#endif
    (void)replayed_wall_split_entities(source.entities(),captured);
    ApplyBoundaryConstraintChanges command;command.expected_revision=source.revision();
    command.message="Insert wall vertex";command.wall_split=std::move(captured);
    const Command result{command};(void)Document::preview_command(source,result);return result;
}
}
