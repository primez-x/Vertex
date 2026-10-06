#include "sketch/physical_wall_room_review.hpp"
#include "sketch/physical_wall_spaces.hpp"
#include "sketch/boundary_integrity.hpp"
#include "sketch/boundary_dimension.hpp"
#include "sketch/constraint_entity.hpp"
#include "sketch/document_digest.hpp"
#include "sketch/model_phases.hpp"
#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>

namespace sketch {
namespace {
using Json=nlohmann::json;
using Entities=std::map<std::string,Entity,std::less<>>;
[[noreturn]] void invalid(const std::string& reason) { throw std::invalid_argument("Physical room review: "+reason); }
void keys(const Json& value,std::initializer_list<const char*> expected) {
    if (!value.is_object() || value.size()!=expected.size()) invalid("unsupported intent fields");
    for (const auto* key:expected) if (!value.contains(key)) invalid("missing intent field");
}
bool valid_id(const std::string& id) {
    return !id.empty() && id.size()<=128 && std::all_of(id.begin(),id.end(),[](unsigned char c) {
        return (c>='a' && c<='z') || (c>='A' && c<='Z') || (c>='0' && c<='9') || c=='-' || c=='_' || c=='.' || c==':';
    });
}
void id(const std::string& value) { if (!valid_id(value)) invalid("invalid identity"); }
void digest(const std::string& value) {
    if (value.size()!=64 || !std::all_of(value.begin(),value.end(),[](unsigned char c) {
        return (c>='0' && c<='9') || (c>='a' && c<='f');
    })) invalid("invalid source digest");
}
Json context_json(const DrawingContext& c) {
    return {{"property_id",c.property_id},{"building_id",c.building_id},{"floor_id",c.floor_id},{"layer_id",c.layer_id},{"level_id",c.level_id}};
}
DrawingContext context(const Json& value) {
    keys(value,{"property_id","building_id","floor_id","layer_id","level_id"});
    DrawingContext c{value.at("property_id").get<std::string>(),value.at("building_id").get<std::string>(),
        value.at("floor_id").get<std::string>(),value.at("layer_id").get<std::string>(),value.at("level_id").get<std::string>()};
    for (const auto* v:{&c.property_id,&c.building_id,&c.floor_id,&c.layer_id,&c.level_id}) if (!v->empty()) id(*v);
    return c;
}
void ids(const std::vector<std::string>& values) {
    if (values.size()>65536) invalid("reference/identity budget exceeded");
    std::set<std::string> unique;for (const auto& value:values) { id(value);if (!unique.insert(value).second) invalid("duplicate identity decision"); }
}
std::string relation_name(RoomRelationKind kind) {
    if (kind==RoomRelationKind::independent) return "independent";
    if (kind==RoomRelationKind::follows) return "follows";
    if (kind==RoomRelationKind::derived_from) return "derived_from";
    invalid("unsupported graph relation");
}
Json relation_json(const RoomRelation& relation) {
    return {{"source_id",relation.source_id},{"target_id",relation.target_id},{"kind",relation_name(relation.kind)}};
}
RoomRelation relation(const Json& value) {
    keys(value,{"source_id","target_id","kind"});
    RoomRelation r{value.at("source_id").get<std::string>(),value.at("target_id").get<std::string>(),RoomRelationKind::independent};
    id(r.source_id);id(r.target_id);const auto kind=value.at("kind").get<std::string>();
    if (kind=="follows") r.kind=RoomRelationKind::follows;
    else if (kind=="derived_from") r.kind=RoomRelationKind::derived_from;
    else if (kind!="independent") invalid("unsupported graph relation");
    return r;
}
bool mentions(const Json& value,const std::set<std::string>& tokens) {
    if (value.is_string()) return tokens.contains(value.get_ref<const std::string&>());
    if (value.is_object()) {
        for (const auto& [key,child]:value.items())
            if (tokens.contains(key) || mentions(child,tokens)) return true;
    } else if (value.is_array()) {
        for (const auto& child:value) if (mentions(child,tokens)) return true;
    }
    return false;
}
void witness(const PhysicalWallSpace& space,Vec2 point) {
    constexpr double r=8*default_geometry_tolerance_metres;
    Boundary probe{{{point.x-r,point.y-r},{point.x+r,point.y-r},0},{{point.x+r,point.y-r},{point.x,point.y+r},0},
        {{point.x,point.y+r},{point.x-r,point.y-r},0}};
    auto holes=space.holes;holes.push_back(std::move(probe));
    if (const auto error=validate_boundary_holes(space.boundary,holes)) invalid("witness is not strictly within the chosen clear space: "+*error);
}
IdentifiedBoundary identified(const PhysicalWallSpace& space,const PhysicalWallRoomFreshDecision& decision) {
    if (decision.fresh_ids.segment_ids.size()!=space.boundary.size() || decision.fresh_ids.vertex_ids.size()!=space.boundary.size())
        invalid("every assigned edge requires explicit fresh segment and vertex identities");
    IdentifiedBoundary result{decision.room_id,"room_boundary",{}};
    for (std::size_t i=0;i<space.boundary.size();++i)
        result.segments.push_back({decision.fresh_ids.segment_ids[i],decision.fresh_ids.vertex_ids[i],
            decision.fresh_ids.vertex_ids[(i+1)%space.boundary.size()],space.boundary[i]});
    return result;
}
std::set<std::string> current_source_owners(const PhysicalWallSpaces& detection,const std::string& selected) {
    std::set<std::string> result{selected};
    for (const auto& space:detection.spaces) for (const auto& source:space.source_lineage.at("physical_sources"))
        result.insert(source.at("owner_id").get<std::string>());
    return result;
}
void validate_active(const Entities& source,const std::set<std::string>& rooms) {
    for (const auto& [entity_id,e]:source) {
        (void)entity_id;if (e.type!="model_phases") continue;
        const auto model=ModelPhases::from_json(e.properties.at("model"));const auto active=model.active_state();
        for (const auto& member:model.entity_ids()) if (rooms.contains(member) && (!active.contains(member) || active.at(member)==ModelPhase::demolished))
            invalid("retained room is inactive in the semantic phase");
    }
}
} // namespace

PhysicalWallRoomReviewIntent decode_physical_wall_room_review_intent(const Json& value) {
    try {
        if (value.dump().size()>16*1024*1024) invalid("intent exceeds evidence budget");
        keys(value,{"version","selected_wall_id","source_snapshot_digest","source_authoring_digest","source_saved_revision","source_entities_digest","context","effective_elevation_m",
            "retained","fresh","removed_reference_ids","kept_reference_ids","relationship_removals"});
        if (!value.at("version").is_number_integer() || value.at("version")!=1) invalid("unsupported intent version");
        PhysicalWallRoomReviewIntent result;
        result.selected_wall_id=value.at("selected_wall_id").get<std::string>();id(result.selected_wall_id);
        result.source_snapshot_digest=value.at("source_snapshot_digest").get<std::string>();digest(result.source_snapshot_digest);
        result.source_authoring_digest=value.at("source_authoring_digest").get<std::string>();digest(result.source_authoring_digest);
        const auto& saved=value.at("source_saved_revision");
        if (!saved.is_null()) {
            if ((!saved.is_number_integer() && !saved.is_number_unsigned()) ||
                (saved.is_number_integer() && saved.get<std::int64_t>()<0)) invalid("invalid captured save revision");
            result.source_saved_revision=saved.get<Revision>();
        }
        result.source_entities_digest=value.at("source_entities_digest").get<std::string>();digest(result.source_entities_digest);
        result.context=context(value.at("context"));if (!result.context.complete()) invalid("incomplete drawing context");
        if (!value.at("effective_elevation_m").is_number()) invalid("invalid elevation");
        result.effective_elevation_m=value.at("effective_elevation_m").get<double>();if (!std::isfinite(result.effective_elevation_m)) invalid("nonfinite elevation");
        for (const auto* name:{"retained","fresh","removed_reference_ids","kept_reference_ids","relationship_removals"})
            if (!value.at(name).is_array() || value.at(name).size()>2048) invalid("decision collection exceeds budget or is malformed");
        for (const auto& row:value.at("retained")) {
            keys(row,{"room_id","expected_descriptor_digest","disposition","child_mapping","replacement_dimension_ids"});
            PhysicalWallRoomRetainedDecision d;d.room_id=row.at("room_id").get<std::string>();id(d.room_id);
            d.expected_descriptor_digest=row.at("expected_descriptor_digest").get<std::string>();digest(d.expected_descriptor_digest);
            const auto disposition=row.at("disposition").get<std::string>();
            if (disposition=="retire") d.disposition=PhysicalWallRoomRetainedDisposition::retire;
            else if (disposition!="retain") invalid("unsupported retained disposition");
            d.child_mapping=row.at("child_mapping");if (!d.child_mapping.is_object()) invalid("malformed child mapping");
            d.replacement_dimension_ids=row.at("replacement_dimension_ids").get<std::vector<std::string>>();ids(d.replacement_dimension_ids);
            if (d.disposition==PhysicalWallRoomRetainedDisposition::retire && !d.child_mapping.empty()) invalid("retirement cannot map children");
            if (d.disposition==PhysicalWallRoomRetainedDisposition::retire && !d.replacement_dimension_ids.empty()) invalid("retirement cannot regenerate dimensions");
            result.retained.push_back(std::move(d));
        }
        for (const auto& row:value.at("fresh")) {
            keys(row,{"candidate_index","reviewed_source_lineage","disposition","room_id","interior_witness","segment_ids","vertex_ids","name","classification","context"});
            PhysicalWallRoomFreshDecision d;
            if (!row.at("candidate_index").is_number_integer() || row.at("candidate_index")<0 || row.at("candidate_index")>=2048) invalid("invalid candidate index");
            d.candidate_index=row.at("candidate_index").get<std::size_t>();d.reviewed_source_lineage=row.at("reviewed_source_lineage");
            if (!d.reviewed_source_lineage.is_object()) invalid("malformed reviewed lineage");
            const auto disposition=row.at("disposition").get<std::string>();
            if (disposition=="retained") d.disposition=PhysicalWallRoomFreshDisposition::retained;
            else if (disposition=="create") d.disposition=PhysicalWallRoomFreshDisposition::create;
            else if (disposition!="unclassified") invalid("unsupported fresh disposition");
            d.room_id=row.at("room_id").get<std::string>();
            const auto& point=row.at("interior_witness");if (!point.is_array() || point.size()!=2 || !point[0].is_number() || !point[1].is_number()) invalid("malformed witness");
            d.interior_witness={point[0].get<double>(),point[1].get<double>()};
            if (!std::isfinite(d.interior_witness.x) || !std::isfinite(d.interior_witness.y)) invalid("nonfinite witness");
            d.fresh_ids.segment_ids=row.at("segment_ids").get<std::vector<std::string>>();ids(d.fresh_ids.segment_ids);
            d.fresh_ids.vertex_ids=row.at("vertex_ids").get<std::vector<std::string>>();ids(d.fresh_ids.vertex_ids);
            d.name=row.at("name").get<std::string>();d.classification=row.at("classification").get<std::string>();d.context=context(row.at("context"));
            if (d.disposition==PhysicalWallRoomFreshDisposition::unclassified) {
                if (!d.room_id.empty() || !d.fresh_ids.segment_ids.empty() || !d.fresh_ids.vertex_ids.empty()) invalid("unclassified space has identity decisions");
            } else id(d.room_id);
            if (d.disposition==PhysicalWallRoomFreshDisposition::create) {
                const auto text=[](const std::string& s) { return !s.empty() && s.size()<=4096 && s.find_first_not_of(" \t\r\n")!=std::string::npos; };
                if (!text(d.name) || !text(d.classification) || d.context!=result.context) invalid("creation requires explicit name, classification and matching context");
            } else if (!d.name.empty() || !d.classification.empty() || d.context!=DrawingContext{}) invalid("noncreation decision cannot transfer metadata");
            result.fresh.push_back(std::move(d));
        }
        result.removed_reference_ids=value.at("removed_reference_ids").get<std::vector<std::string>>();ids(result.removed_reference_ids);
        result.kept_reference_ids=value.at("kept_reference_ids").get<std::vector<std::string>>();ids(result.kept_reference_ids);
        for (const auto& kept:result.kept_reference_ids)
            if (std::find(result.removed_reference_ids.begin(),result.removed_reference_ids.end(),kept)!=result.removed_reference_ids.end())
                invalid("reference cannot be both kept and removed");
        for (const auto& row:value.at("relationship_removals")) {
            keys(row,{"entity_id","removed_room_ids","acknowledged_relations"});
            PhysicalWallRoomRelationshipRemoval d;d.entity_id=row.at("entity_id").get<std::string>();id(d.entity_id);
            d.removed_room_ids=row.at("removed_room_ids").get<std::vector<std::string>>();ids(d.removed_room_ids);
            if (d.removed_room_ids.empty() || !row.at("acknowledged_relations").is_array() || row.at("acknowledged_relations").size()>65536) invalid("invalid graph removal decisions");
            std::set<std::string> unique;
            for (const auto& r:row.at("acknowledged_relations")) {
                if (!unique.insert(r.dump()).second) invalid("duplicate graph relation acknowledgement");
                d.acknowledged_relations.push_back(relation(r));
            }
            result.relationship_removals.push_back(std::move(d));
        }
        return result;
    } catch (const Json::exception&) { invalid("malformed intent value types"); }
}

Json encode_physical_wall_room_review_intent(const PhysicalWallRoomReviewIntent& intent) {
    Json retained=Json::array(),fresh=Json::array(),relationships=Json::array();
    for (const auto& d:intent.retained) {
        std::string action;
        if (d.disposition==PhysicalWallRoomRetainedDisposition::retain) action="retain";
        else if (d.disposition==PhysicalWallRoomRetainedDisposition::retire) action="retire";
        else invalid("unsupported retained disposition");
        retained.push_back({{"room_id",d.room_id},{"expected_descriptor_digest",d.expected_descriptor_digest},{"disposition",action},{"child_mapping",d.child_mapping},
            {"replacement_dimension_ids",d.replacement_dimension_ids}});
    }
    for (const auto& d:intent.fresh) {
        std::string action;
        if (d.disposition==PhysicalWallRoomFreshDisposition::retained) action="retained";
        else if (d.disposition==PhysicalWallRoomFreshDisposition::create) action="create";
        else if (d.disposition==PhysicalWallRoomFreshDisposition::unclassified) action="unclassified";
        else invalid("unsupported fresh disposition");
        fresh.push_back({{"candidate_index",d.candidate_index},{"reviewed_source_lineage",d.reviewed_source_lineage},{"disposition",action},
            {"room_id",d.room_id},{"interior_witness",{d.interior_witness.x,d.interior_witness.y}},
            {"segment_ids",d.fresh_ids.segment_ids},{"vertex_ids",d.fresh_ids.vertex_ids},{"name",d.name},{"classification",d.classification},{"context",context_json(d.context)}});
    }
    for (const auto& d:intent.relationship_removals) {
        Json rows=Json::array();for (const auto& r:d.acknowledged_relations) rows.push_back(relation_json(r));
        relationships.push_back({{"entity_id",d.entity_id},{"removed_room_ids",d.removed_room_ids},{"acknowledged_relations",std::move(rows)}});
    }
    Json result{{"version",1},{"selected_wall_id",intent.selected_wall_id},{"source_snapshot_digest",intent.source_snapshot_digest},
        {"source_authoring_digest",intent.source_authoring_digest},
        {"source_saved_revision",intent.source_saved_revision ? Json(*intent.source_saved_revision) : Json(nullptr)},
        {"source_entities_digest",intent.source_entities_digest},{"context",context_json(intent.context)},{"effective_elevation_m",intent.effective_elevation_m},
        {"retained",std::move(retained)},{"fresh",std::move(fresh)},{"removed_reference_ids",intent.removed_reference_ids},
        {"kept_reference_ids",intent.kept_reference_ids},{"relationship_removals",std::move(relationships)}};
    (void)decode_physical_wall_room_review_intent(result);return result;
}

ReplayedPhysicalWallRoomReview replay_physical_wall_room_review(const Entities& source,const Json& encoded) {
    const auto intent=decode_physical_wall_room_review_intent(encoded);
    if (entity_map_digest(source)!=intent.source_entities_digest) invalid("preceding entity map differs from reviewed source");
    const auto detection=detect_physical_wall_spaces(source,intent.selected_wall_id);
    if (detection.context!=intent.context) invalid("source drawing context changed");
    const auto selected=resolve_vertical_placement(source,source.at(intent.selected_wall_id));
    const auto elevation=selected.properties.at("elevation_m").get<double>();
    if (elevation!=intent.effective_elevation_m) invalid("source effective plane changed");
    const auto organization=organize_project(source);const auto current_owners=current_source_owners(detection,intent.selected_wall_id);
    std::set<std::string> expected_rooms,retiring,affected_tokens,occupied;
    std::size_t retained_bytes=0,retained_contacts=0;
    for (const auto& [entity_id,e]:source) {
        occupied.insert(entity_id);
        if (can_recognize_boundary_entity_type(e.type) && inspect_boundary_entity_version(e).format==BoundaryEntityFormat::identified_v1) {
            for (const auto& edge:decode_identified_boundary_entity(e).segments) {
                occupied.insert(edge.segment_id);occupied.insert(edge.start_vertex_id);occupied.insert(edge.end_vertex_id);
            }
        }
        if (!is_physical_wall_room(e)) continue;
        const auto c=organization.drawing_context(entity_id);
        if (!c || !c->complete()) invalid("retained room has unresolved drawing context");
        if (*c!=intent.context) continue;
        const auto descriptor=decode_physical_wall_room_descriptor(e);
        const auto bytes=e.properties.dump().size()+e.extensions.dump().size();
        if (bytes>16*1024*1024-retained_bytes) invalid("retained evidence exceeds aggregate budget");
        retained_bytes+=bytes;
        std::size_t edge_count=decode_identified_boundary_entity(e).segments.size();
        for (const auto& hole:descriptor.holes) edge_count+=hole.size();
        if (edge_count>16384) invalid("retained geometry exceeds edge budget");
        const auto contacts=edge_count ? edge_count*(edge_count-1) : 0;
        if (contacts>65536-retained_contacts) invalid("retained geometry exceeds aggregate contact budget");
        retained_contacts+=contacts;
        const auto captured=validate_retained_physical_wall_room_lineage(e,*c);
        if (std::abs(captured.effective_elevation_m-elevation)>default_geometry_tolerance_metres) {
            if (std::any_of(captured.source_owner_ids.begin(),captured.source_owner_ids.end(),[&](const auto& v){return current_owners.contains(v);}))
                invalid("surviving source identity moved between effective planes");
            continue;
        }
        expected_rooms.insert(entity_id);
        for (const auto& edge:decode_identified_boundary_entity(e).segments) {
            affected_tokens.insert(edge.segment_id);affected_tokens.insert(edge.start_vertex_id);affected_tokens.insert(edge.end_vertex_id);
        }
    }
    if (expected_rooms.size()>2048 || detection.spaces.size()>2048) invalid("room review exceeds budget");
    validate_active(source,expected_rooms);
    std::map<std::string,const PhysicalWallRoomRetainedDecision*,std::less<>> old;
    for (const auto& d:intent.retained) {
        if (!expected_rooms.contains(d.room_id) || !old.emplace(d.room_id,&d).second) invalid("duplicate or out-of-context retained decision");
        if (physical_wall_room_descriptor_digest(source.at(d.room_id))!=d.expected_descriptor_digest) invalid("retained descriptor changed");
        if (d.disposition==PhysicalWallRoomRetainedDisposition::retire) { retiring.insert(d.room_id);affected_tokens.insert(d.room_id); }
    }
    if (old.size()!=expected_rooms.size()) invalid("every retained room requires an explicit complete disposition");
    std::map<std::size_t,const PhysicalWallRoomFreshDecision*> fresh;
    std::set<std::string> assigned,retained_assigned;
    for (const auto& d:intent.fresh) {
        if (d.candidate_index>=detection.spaces.size() || !fresh.emplace(d.candidate_index,&d).second) invalid("duplicate or unknown fresh space decision");
        const auto& space=detection.spaces[d.candidate_index];
        if (d.reviewed_source_lineage!=space.source_lineage) invalid("reviewed fresh lineage is no longer exact");
        if (d.disposition==PhysicalWallRoomFreshDisposition::unclassified) continue;
        if (!assigned.insert(d.room_id).second) invalid("one room identity cannot own multiple fresh spaces");
        if (d.disposition==PhysicalWallRoomFreshDisposition::retained) {
            if (!old.contains(d.room_id) || old.at(d.room_id)->disposition!=PhysicalWallRoomRetainedDisposition::retain) invalid("fresh assignment lacks matching retained decision");
            retained_assigned.insert(d.room_id);
        } else if (!occupied.insert(d.room_id).second) invalid("new room identity is already occupied");
        witness(space,d.interior_witness);(void)identified(space,d);
        for (const auto* group:{&d.fresh_ids.segment_ids,&d.fresh_ids.vertex_ids}) for (const auto& child:*group)
            if (!occupied.insert(child).second) invalid("fresh child identity is already occupied");
    }
    if (fresh.size()!=detection.spaces.size()) invalid("every fresh space requires an explicit complete disposition");
    for (const auto& [room_id,d]:old) if (d->disposition==PhysicalWallRoomRetainedDisposition::retain && !retained_assigned.contains(room_id))
        invalid("retained identity has no explicitly assigned fresh space");
    Entities result=source;std::vector<BoundaryGeometryEdit> retained_edits;std::vector<std::string> created_room_ids;
    std::set<std::string> removed(intent.removed_reference_ids.begin(),intent.removed_reference_ids.end());
    const std::set<std::string> kept(intent.kept_reference_ids.begin(),intent.kept_reference_ids.end());
    std::set<std::string> expected_references;
    for (const auto& [reference_id,e]:source) {
        if (can_recognize_boundary_dimension_entity_type(e.type)) {
            const auto decoded=decode_boundary_dimension_entity(e);
            if (decoded.supported() && expected_rooms.contains(decoded.dimension->boundary_id)) expected_references.insert(reference_id);
        } else if (e.type=="constraint") {
            const auto decoded=decode_constraint_entity(e);
            if (decoded.supported() && std::any_of(decoded.constraint->bindings.begin(),decoded.constraint->bindings.end(),
                [&](const auto& binding){return expected_rooms.contains(binding.owner_id);})) expected_references.insert(reference_id);
        }
    }
    auto reviewed_references=removed;
    reviewed_references.insert(kept.begin(),kept.end());
    if (reviewed_references!=expected_references) invalid("every affected reference requires an explicit Keep or Remove decision");
    // Include every identity that this review can retire, not only room and
    // boundary-child IDs. Unknown incoming references must never be stranded.
    affected_tokens.insert(removed.begin(),removed.end());
    for (const auto& [reference_id,e]:source) {
        if (!kept.contains(reference_id) || !can_recognize_boundary_dimension_entity_type(e.type)) continue;
        const auto decoded=decode_boundary_dimension_entity(e);
        if (!decoded.supported()) continue;
        const auto decision=old.find(decoded.dimension->boundary_id);
        if (decision!=old.end() && decision->second->disposition==PhysicalWallRoomRetainedDisposition::retain &&
            decoded.dimension->kind==BoundaryDimensionKind::segment_length &&
            decoded.dimension->placement==BoundaryDimensionPlacement::automatic)
            affected_tokens.insert(reference_id);
    }
    for (const auto& [room_id,d]:old) {
        (void)room_id;
        for (const auto& id:d->replacement_dimension_ids)
            if (!occupied.insert(id).second) invalid("replacement dimension identity is already occupied");
    }
    for (const auto& reference_id:removed) {
        const auto found=source.find(reference_id);if (found==source.end()) invalid("removed reference is missing");
        bool affected=false;
        if (can_recognize_boundary_dimension_entity_type(found->second.type)) {
            const auto decoded=decode_boundary_dimension_entity(found->second);
            if (!decoded.supported()) invalid("unknown dimension cannot be removed by room review");
            affected=expected_rooms.contains(decoded.dimension->boundary_id);
        } else if (found->second.type=="constraint") {
            const auto decoded=decode_constraint_entity(found->second);
            if (!decoded.supported()) invalid("unknown constraint cannot be removed by room review");
            affected=std::any_of(decoded.constraint->bindings.begin(),decoded.constraint->bindings.end(),[&](const auto& b){return expected_rooms.contains(b.owner_id);});
        }
        if (!affected) invalid("removal must name a supported affected dimension or endpoint constraint");
        result.erase(reference_id);
    }
    std::set<std::string> graph_decisions;
    for (const auto& d:intent.relationship_removals) {
        if (!graph_decisions.insert(d.entity_id).second) invalid("duplicate relationship graph decision");
        const auto found=result.find(d.entity_id);if (found==result.end() || found->second.type!="room_relationships") invalid("graph removal target is not a relationship model");
        const auto& model=found->second.properties.at("model");
        if (room_relationship_model_version(model)>2) invalid("unknown relationship model cannot be rewritten");
        const auto graph=RoomRelationshipSnapshot::from_json(model);
        std::set<std::string> remove_rooms(d.removed_room_ids.begin(),d.removed_room_ids.end()),present;
        for (const auto& r:graph.references()) if (remove_rooms.contains(r.id)) present.insert(r.id);
        if (present!=remove_rooms || !std::all_of(remove_rooms.begin(),remove_rooms.end(),[&](const auto& v){return retiring.contains(v);})) invalid("graph removals must name present retired room identities");
        std::vector<RoomReference> references;std::vector<RoomRelation> relations;std::set<std::string> incident,acknowledged;
        for (const auto& r:graph.references()) if (!remove_rooms.contains(r.id)) references.push_back(r);
        for (const auto& r:graph.relations()) {
            if (remove_rooms.contains(r.source_id) || remove_rooms.contains(r.target_id)) incident.insert(relation_json(r).dump());
            else relations.push_back(r);
        }
        for (const auto& r:d.acknowledged_relations) acknowledged.insert(relation_json(r).dump());
        if (incident!=acknowledged) invalid("every removed relationship row requires exact acknowledgement");
        auto properties=found->second.properties;properties.erase("model");
        if (mentions(properties,affected_tokens) || mentions(found->second.extensions,affected_tokens)) invalid("unsupported relationship metadata reference");
        found->second.properties["model"]=RoomRelationshipSnapshot::create(std::move(references),std::move(relations)).to_json();
    }
    for (const auto& [entity_id,e]:result) {
        if (expected_rooms.contains(entity_id)) {
            if (!retiring.contains(entity_id)) {
                auto opaque_properties=e.properties;
                if (opaque_properties.contains("segments")) {
                    for (auto segment:opaque_properties.at("segments")) {
                        for (const auto* key:{"segment_id","start_vertex_id","end_vertex_id","start","end","sweep_radians"})
                            segment.erase(key);
                        if (mentions(segment,affected_tokens)) invalid("retained owner has unsupported segment metadata reference");
                    }
                }
                opaque_properties.erase("segments");
                // These validated receipts deliberately preserve historical
                // analytical identities; they are not live opaque references.
                opaque_properties.erase("boundary_authoring");
                auto opaque_extensions=e.extensions;
                opaque_extensions.erase("boundary_geometry_derivation");
                opaque_extensions.erase("physical_wall_room");
                if (mentions(opaque_properties,affected_tokens) || mentions(opaque_extensions,affected_tokens))
                    invalid("retained owner has unsupported metadata reference to a retired identity");
            }
            continue;
        }
        if (can_recognize_boundary_dimension_entity_type(e.type)) {
            const auto d=decode_boundary_dimension_entity(e);
            if (!d.supported()) { if (mentions(e.properties,affected_tokens) || mentions(e.extensions,affected_tokens)) invalid("unknown affected dimension");continue; }
            if (retiring.contains(d.dimension->boundary_id)) invalid("retired room dimension needs explicit removal");
            if (mentions(e.properties,retiring)) invalid("unsupported dimension property reference to a retired room");
            if (mentions(e.extensions,affected_tokens)) invalid("unsupported dimension extension reference");
            auto extra=e.properties;
            if (extra.contains("target") && extra.at("target").is_object()) {
                for (const auto* field:{"entity_id","segment_id","segment_ids","second_segment_id","vertex_id"}) extra["target"].erase(field);
            }
            extra.erase("boundary_id");
            if (mentions(extra,affected_tokens)) invalid("unsupported dimension metadata reference to a retired child");
            continue;
        }
        if (e.type=="constraint") {
            const auto d=decode_constraint_entity(e);
            if (!d.supported()) { if (mentions(e.properties,affected_tokens) || mentions(e.extensions,affected_tokens)) invalid("unknown affected constraint");continue; }
            if (std::any_of(d.constraint->bindings.begin(),d.constraint->bindings.end(),[&](const auto& b){return retiring.contains(b.owner_id);})) invalid("retired room constraint needs explicit removal");
            if (mentions(e.properties,retiring)) invalid("unsupported constraint property reference to a retired room");
            if (mentions(e.extensions,affected_tokens)) invalid("unsupported constraint extension reference");
            auto extra=e.properties;extra.erase("entity_ids");extra.erase("wall_ids");
            if (extra.contains("bindings") && extra.at("bindings").is_array()) for (auto& binding:extra["bindings"])
                for (const auto* field:{"owner_id","feature","segment_id","vertex_id","role"}) binding.erase(field);
            if (mentions(extra,affected_tokens)) invalid("unsupported constraint metadata reference to a retired child");
            continue;
        }
        if (mentions(e.properties,affected_tokens) || mentions(e.extensions,affected_tokens)) invalid("unsupported dependent reference in "+entity_id);
    }
    // Retire all reviewed old owners before lower retained repair admission, so
    // a deliberately merged destination cannot conflict with a retiring owner.
    for (const auto& room_id:retiring) result.erase(room_id);
    std::set<std::string> retained_owners;
    for (const auto& [room_id,d]:old)
        if (d->disposition==PhysicalWallRoomRetainedDisposition::retain) retained_owners.insert(room_id);
    for (const auto& [index,d]:fresh) {
        if (d->disposition==PhysicalWallRoomFreshDisposition::unclassified) continue;
        const auto& space=detection.spaces[index];const auto replacement=identified(space,*d);
        if (d->disposition==PhysicalWallRoomFreshDisposition::retained) {
            BoundaryGeometryEdit edit;edit.boundary_id=d->room_id;edit.target_id=d->room_id;
            edit.kind=BoundaryGeometryEditKind::redefine_boundary;edit.fresh_topology=true;
            edit.replacement_segments=encode_identified_boundary_entity(replacement).properties.at("segments");
            edit.replacement_child_mapping=old.at(d->room_id)->child_mapping;
            edit.replacement_dimension_ids=old.at(d->room_id)->replacement_dimension_ids;
            edit.physical_wall_room_repair=PhysicalWallRoomRepairIntent{intent.selected_wall_id,d->interior_witness,
                d->reviewed_source_lineage,old.at(d->room_id)->expected_descriptor_digest};
            result=edited_boundary_entities_for_room_review(result,edit,retained_owners);
            retained_edits.push_back(std::move(edit));
        } else {
            const auto& c=d->context;
            Entity room=encode_identified_boundary_entity(replacement);
            room.properties.update({{"property_id",c.property_id},{"building_id",c.building_id},{"floor_id",c.floor_id},{"layer_id",c.layer_id},
                {"name",d->name},{"classification",d->classification},{"measurement_classification",d->classification},
                {"factor",1.0},{"factor_expression","1"},{"factor_numerator",1},{"factor_denominator",1}});
            room.extensions["physical_wall_room"]=encode_physical_wall_room_descriptor({intent.selected_wall_id,space.source_lineage,space.holes});
            result.emplace(room.id,std::move(room));
            created_room_ids.push_back(d->room_id);
        }
    }
    if (const auto error=validate_boundary_integrity(result)) invalid(*error);
    for (const auto& id:kept) {
        if (!result.contains(id)) {
            // Explicitly kept automatic edge dimensions may have been replaced
            // by the reviewed full-edge set with fresh lifetime-safe identities.
            const auto decoded=decode_boundary_dimension_entity(source.at(id));
            if (!decoded.supported() || decoded.dimension->placement!=BoundaryDimensionPlacement::automatic ||
                decoded.dimension->kind!=BoundaryDimensionKind::segment_length ||
                old.at(decoded.dimension->boundary_id)->replacement_dimension_ids.empty())
                invalid("kept reference disappeared from the reviewed candidate");
        } else if (can_recognize_boundary_dimension_entity_type(result.at(id).type)) {
            const auto decoded=decode_boundary_dimension_entity(result.at(id));
            (void)resolve_boundary_dimension(*decoded.dimension,result);
        }
    }
    return {std::move(result),std::move(retained_edits),std::move(created_room_ids),{retiring.begin(),retiring.end()}};
}

Entities replay_physical_wall_room_review_entities(const Entities& source,const Json& encoded) {
    return replay_physical_wall_room_review(source,encoded).entities;
}

PreparedPhysicalWallRoomReview prepare_physical_wall_room_review(const DocumentSnapshot& source,
    const PhysicalWallRoomCorrespondenceReport& report,const PhysicalWallRoomReviewIntent& intent) {
    if (!source.is_editable()) invalid("captured document is read-only");
    if (!physical_wall_room_correspondence_is_current(report,source) || intent.source_snapshot_digest!=document_snapshot_digest(source) ||
        intent.source_authoring_digest!=document_authoring_source_digest_v2(source) ||
        intent.source_saved_revision!=source.saved_revision_optional())
        invalid("complete captured snapshot changed");
    if (report.selected_wall_id!=intent.selected_wall_id || report.context!=intent.context || report.effective_elevation_m!=intent.effective_elevation_m)
        invalid("intent does not belong to the displayed context/plane report");
    auto encoded=encode_physical_wall_room_review_intent(intent);
    auto replayed=replay_physical_wall_room_review(source.entities(),encoded);
    return {std::move(replayed),std::move(encoded)};
}
} // namespace sketch
