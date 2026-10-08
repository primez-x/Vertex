#include "sketch/physical_wall_phase_review.hpp"
#include "sketch/boundary_integrity.hpp"
#include "sketch/boundary_dimension.hpp"
#include "sketch/constraint_entity.hpp"
#include "sketch/constraint_integrity.hpp"
#include "sketch/annotation_entity_codec.hpp"
#include "sketch/sheet_view_entity_codec.hpp"
#include "sketch/document_digest.hpp"
#include "sketch/document_wall.hpp"
#include "sketch/physical_wall_spaces.hpp"
#include "sketch/wall_split.hpp"
#include "sketch/wall_merge.hpp"
#include "sketch/wall_measurement.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <span>
#include <stdexcept>
#include <utility>

namespace sketch {
namespace {
using Json=nlohmann::json;
using Entities=std::map<std::string,Entity,std::less<>>;
constexpr std::size_t maximum_rows=2048;
[[noreturn]] void reject(const std::string& message) {
    throw std::invalid_argument("Physical phase room review: "+message);
}
void keys(const Json& value,std::initializer_list<const char*> names) {
    if (!value.is_object() || value.size()!=names.size()) reject("unsupported intent fields");
    for (const auto* name:names) if (!value.contains(name)) reject("missing intent field");
}
void identity(const std::string& value) {
    if (value.empty() || value.size()>128 || !std::all_of(value.begin(),value.end(),[](unsigned char c) {
        return (c>='a' && c<='z') || (c>='A' && c<='Z') || (c>='0' && c<='9') || c=='-' || c=='_' || c=='.' || c==':';
    })) reject("invalid identity");
}
void digest(const std::string& value) {
    if (value.size()!=64 || !std::all_of(value.begin(),value.end(),[](unsigned char c) {
        return (c>='0' && c<='9') || (c>='a' && c<='f');
    })) reject("invalid digest");
}
void identities(const std::vector<std::string>& values) {
    if (values.size()>65536) reject("identity collection exceeds budget");
    std::set<std::string> found;
    for (const auto& value:values) { identity(value); if (!found.insert(value).second) reject("duplicate identity"); }
}
bool text(const std::string& value) {
    return !value.empty() && value.size()<=4096 && value.find('\0')==std::string::npos && value.find_first_not_of(" \t\r\n")!=std::string::npos;
}
Revision revision(const Json& value) {
    if (!value.is_number_unsigned() && (!value.is_number_integer() || value.get<std::int64_t>()<0))
        reject("invalid revision");
    return value.get<Revision>();
}
Json context_json(const DrawingContext& c) {
    return {{"property_id",c.property_id},{"building_id",c.building_id},{"floor_id",c.floor_id},{"layer_id",c.layer_id},{"level_id",c.level_id}};
}
DrawingContext context(const Json& value) {
    keys(value,{"property_id","building_id","floor_id","layer_id","level_id"});
    DrawingContext c{value.at("property_id").get<std::string>(),value.at("building_id").get<std::string>(),
        value.at("floor_id").get<std::string>(),value.at("layer_id").get<std::string>(),value.at("level_id").get<std::string>()};
    if (!c.complete()) reject("incomplete context");
    for (const auto* token:{&c.property_id,&c.building_id,&c.floor_id,&c.layer_id,&c.level_id}) if (!token->empty()) identity(*token);
    return c;
}
std::string entity_digest(const Entity& entity) {
    return entity_map_digest(Entities{{entity.id,entity}});
}
bool exact_entity(const Entity& a,const Entity& b) {
    return a==b && a.properties.dump()==b.properties.dump() && a.extensions.dump()==b.extensions.dump();
}
ApplyEntityChanges registry_command(const Json& proof,Revision expected,const std::string& registry_id) {
    const auto command=command_from_json(proof);
    const auto* raw=std::get_if<ApplyEntityChanges>(&command);
    if (!raw || command_to_json(command).dump()!=proof.dump() || proof.at("version")!=1 ||
        raw->expected_revision!=expected || !raw->asset_changes.empty() || raw->entity_changes.size()!=1)
        reject("registry proof requires one canonical raw upsert at the expected revision");
    const auto& change=raw->entity_changes.front();
    if (change.kind!=EntityChangeKind::upsert || change.entity.id!=registry_id || change.entity_id!=registry_id ||
        change.entity.type!="model_phases") reject("registry proof must upsert only the named registry");
    (void)ModelPhases::from_json(change.entity.properties.at("model"));
    return *raw;
}
struct RegistryTransition {
    Entity registry;
    ModelPhases original;
    ModelPhases destination;
    bool created{};
};
RegistryTransition registry_transition(const Entities& source,const PhysicalWallRoomPhaseReviewIntent& intent) {
    const auto command=registry_command(intent.registry_command_proof,intent.expected_revision,intent.registry_id);
    const auto next=command.entity_changes.front().entity;
    const auto destination=ModelPhases::from_json(next.properties.at("model"));
    const auto found=source.find(intent.registry_id);
    if (found==source.end()) {
        if (destination.to_json().dump()!=next.properties.at("model").dump()) reject("new registry seed must be canonical version one");
        if (intent.source_registry_entity_digest) reject("new registry cannot bind an existing registry digest");
        std::vector<std::string> eligible;
        for (const auto& [id,e]:source) if (is_model_phase_entity_type(e.type)) eligible.push_back(id);
        // Seeding assigns all actual eligible existing owners to the shared
        // baseline; proposals in this child only partition actual source IDs.
        if (destination.entity_ids()!=eligible) reject("new registry seed differs from actual eligible model inventory");
        std::set<std::string> baseline(destination.baseline_ids().begin(),destination.baseline_ids().end());
        for (const auto& alternative:destination.alternatives()) {
            if (!intent.alternative_id || alternative.id!=*intent.alternative_id) reject("new registry contains an unrelated alternative");
            if (!text(alternative.name)) reject("target alternative requires an explicit name");
            for (const auto& id:alternative.proposed_ids) if (is_physical_wall_room(source.at(id)))
                reject("registry seed cannot reclassify an original room as a proposal");
        }
        for (const auto& [id,e]:source) if (is_physical_wall_room(e) && !baseline.contains(id))
            reject("original physical rooms must remain in the seeded baseline");
        (void)destination.state(intent.alternative_id);
        return {next,ModelPhases::create(eligible,eligible,{}),destination,true};
    }
    if (found->second.type!="model_phases" || found->second.id!=intent.registry_id ||
        !intent.source_registry_entity_digest || entity_digest(found->second)!=*intent.source_registry_entity_digest)
        reject("original registry entity changed");
    auto metadata=next; metadata.properties["model"]=found->second.properties.at("model");
    if (!exact_entity(metadata,found->second)) reject("registry command changed unrelated metadata");
    const auto original=ModelPhases::from_json(found->second.properties.at("model"));
    if (original.entity_ids()!=destination.entity_ids() || original.baseline_ids()!=destination.baseline_ids())
        reject("registry command changed baseline or registered inventory");
    const auto& original_json=found->second.properties.at("model");
    const auto& destination_json=next.properties.at("model");
    if (original_json.at("entity_ids").dump()!=destination_json.at("entity_ids").dump() ||
        original_json.at("baseline_ids").dump()!=destination_json.at("baseline_ids").dump())
        reject("registry command rewrote original membership ordering");
    if (destination.active_alternative()!=original.active_alternative() && destination.active_alternative()!=intent.alternative_id)
        reject("registry command selected an unrelated alternative");
    for (const auto& old:original.alternatives()) {
        const auto replacement=std::find_if(destination.alternatives().begin(),destination.alternatives().end(),[&](const auto& row){return row.id==old.id;});
        if (replacement==destination.alternatives().end()) reject("registry command removed an alternative");
        if (!intent.alternative_id || old.id!=*intent.alternative_id) {
            if (*replacement!=old) reject("registry command changed an unrelated alternative");
        } else if (replacement->proposed_ids!=old.proposed_ids) reject("existing target proposals require a separate explicit review");
    }
    for (const auto& old:original_json.at("alternatives")) {
        const auto replacement=std::find_if(destination_json.at("alternatives").begin(),destination_json.at("alternatives").end(),
            [&](const auto& row){return row.at("id")==old.at("id");});
        if (replacement==destination_json.at("alternatives").end()) reject("registry command removed an original alternative");
        auto before=old,after=*replacement;
        if (intent.alternative_id && old.at("id")==*intent.alternative_id) {
            before.erase("name"); before.erase("demolished_ids"); after.erase("name"); after.erase("demolished_ids");
        }
        if (before.dump()!=after.dump()) reject("registry command rewrote unrelated alternative bytes");
    }
    std::vector<std::string> original_order,retained_order;
    for (const auto& row:original_json.at("alternatives")) original_order.push_back(row.at("id").get<std::string>());
    for (const auto& row:destination_json.at("alternatives")) {
        const auto id=row.at("id").get<std::string>();
        if (std::find(original_order.begin(),original_order.end(),id)!=original_order.end()) retained_order.push_back(id);
    }
    if (retained_order!=original_order) reject("registry command reordered original alternatives");
    for (const auto& row:destination.alternatives()) {
        if (intent.alternative_id && row.id==*intent.alternative_id) { if (!text(row.name)) reject("target alternative requires a name"); continue; }
        if (std::none_of(original.alternatives().begin(),original.alternatives().end(),[&](const auto& old){return old.id==row.id;}))
            reject("registry command added an unrelated alternative");
    }
    (void)destination.state(intent.alternative_id);
    return {next,original,destination,false};
}
struct Plane { DrawingContext context; double elevation{}; };
bool same_plane(const Plane& a,const Plane& b) {
    return a.context==b.context && std::abs(a.elevation-b.elevation)<=default_geometry_tolerance_metres;
}
void add_plane(std::vector<Plane>& planes,Plane value) {
    if (!std::isfinite(value.elevation)) reject("nonfinite effective plane");
    if (std::none_of(planes.begin(),planes.end(),[&](const auto& old){return same_plane(old,value);})) planes.push_back(std::move(value));
    if (planes.size()>32) reject("affected plane coverage exceeds budget");
}
std::set<std::string> inactive_owners(const std::vector<PhysicalWallPhaseState>& phases) {
    std::set<std::string> inactive;
    for (const auto& phase:phases) for (const auto& id:phase.registered_entity_ids) {
        const auto state=phase.states.find(id);
        if (state==phase.states.end() || state->second==ModelPhase::demolished) inactive.insert(id);
    }
    return inactive;
}
struct PhaseReviewCoverage {
    Entities destination;
    std::vector<Plane> affected;
    std::set<std::string> after_inactive;
    std::map<std::string,std::pair<DrawingContext,PhysicalWallRoomLineageCheck>,std::less<>> captured;
};
PhaseReviewCoverage phase_review_coverage(const Entities& source,const PhysicalWallRoomPhaseReviewIntent& intent,
    const RegistryTransition& transition) {
    PhaseReviewCoverage result; result.destination=source;
    result.destination.insert_or_assign(intent.registry_id,transition.registry);
    Entities baseline=source;
    if (transition.created) {
        auto registry=transition.registry; registry.properties["model"]=transition.original.to_json(); baseline.emplace(intent.registry_id,std::move(registry));
    }
    const PhysicalWallPhaseSelection baseline_selection{intent.registry_id,std::nullopt};
    const PhysicalWallPhaseSelection destination_selection{intent.registry_id,intent.alternative_id};
    const auto before_inactive=inactive_owners(physical_wall_phase_states(baseline,baseline_selection));
    result.after_inactive=inactive_owners(physical_wall_phase_states(result.destination,destination_selection));
    auto preceding_alternative=transition.original.active_alternative();
    if (intent.alternative_id) {
        preceding_alternative=std::nullopt;
        for (const auto& alternative:transition.original.alternatives())
            if (alternative.id==*intent.alternative_id) preceding_alternative=intent.alternative_id;
    }
    const auto preceding_inactive=inactive_owners(physical_wall_phase_states(baseline,{intent.registry_id,preceding_alternative}));
    const auto organization=organize_project(source);
    std::set<std::string> changed_walls;
    std::size_t wall_count=0;
    for (const auto& [id,e]:source) {
        if (id!=e.id) reject("source entity identity differs from its key");
        if (e.type!="wall") continue;
        if (++wall_count>maximum_rows) reject("physical source inventory exceeds budget");
        if (before_inactive.contains(id)==result.after_inactive.contains(id) && preceding_inactive.contains(id)==result.after_inactive.contains(id)) continue;
        changed_walls.insert(id);
        const auto c=organization.drawing_context(id);
        if (!c || !c->complete()) reject("affected wall has unresolved context");
        const auto placed=resolve_vertical_placement(source,e); Wall wall; std::string error;
        if (!read_document_wall(placed,{},wall,error)) reject(error);
        validate_wall_semantics(wall); add_plane(result.affected,{*c,wall.elevation});
    }
    const auto baseline_roster=physical_wall_phase_room_roster(baseline,baseline_selection);
    std::set<std::string> eligible_rooms(baseline_roster.active_room_ids.begin(),baseline_roster.active_room_ids.end());
    std::set<std::string> target_proposed_rooms;
    if (intent.alternative_id) for (const auto& alternative:transition.destination.alternatives()) if (alternative.id==*intent.alternative_id)
        for (const auto& id:alternative.proposed_ids) if (is_physical_wall_room(source.at(id))) {
            eligible_rooms.insert(id); target_proposed_rooms.insert(id);
        }
    if (eligible_rooms.size()>maximum_rows || wall_count>maximum_rows-eligible_rooms.size())
        reject("combined room and physical source inventory exceeds budget");
    for (const auto& id:eligible_rooms) {
        const auto c=organization.drawing_context(id); if (!c || !c->complete()) reject("original room has unresolved context");
        auto lineage=validate_retained_physical_wall_room_lineage(source.at(id),*c);
        if (before_inactive.contains(id)!=result.after_inactive.contains(id) || preceding_inactive.contains(id)!=result.after_inactive.contains(id) || target_proposed_rooms.contains(id) ||
            std::any_of(lineage.source_owner_ids.begin(),lineage.source_owner_ids.end(),[&](const auto& wall){return changed_walls.contains(wall);}))
            add_plane(result.affected,{*c,lineage.effective_elevation_m});
        result.captured.emplace(id,std::make_pair(*c,std::move(lineage)));
    }
    return result;
}
std::set<std::string> phase_review_plane_rooms(const PhaseReviewCoverage& coverage,const Plane& plane) {
    std::set<std::string> result;
    for (const auto& [id,entry]:coverage.captured)
        if (same_plane({entry.first,entry.second.effective_elevation_m},plane)) result.insert(id);
    return result;
}
PhasePhysicalWallRoomCorrespondenceReport phase_review_plane_report(const Entities& source,const PhaseReviewCoverage& coverage,
    const PhysicalWallPhaseSelection& destination_selection,const Plane& plane,const std::set<std::string>& expected_rooms) {
    auto report=phase_physical_wall_room_correspondence(coverage.destination,plane.context,plane.elevation,
        destination_selection,std::vector<std::string>(expected_rooms.begin(),expected_rooms.end()));
    if (!report.correspondence.explicit_phase_evaluation || report.destination_selection!=destination_selection ||
        report.source_entities_digest!=entity_map_digest(coverage.destination) || report.correspondence.context!=plane.context ||
        report.correspondence.effective_elevation_m!=plane.elevation) reject("explicit phase correspondence source differs");
    std::set<std::string> reported_rooms;
    for (const auto& retained:report.correspondence.retained) {
        if (!expected_rooms.contains(retained.room.id) || !reported_rooms.insert(retained.room.id).second ||
            !exact_entity(retained.room,source.at(retained.room.id)) ||
            retained.descriptor_digest!=physical_wall_room_descriptor_digest(source.at(retained.room.id)))
            reject("explicit phase correspondence original owner evidence differs");
    }
    if (reported_rooms!=expected_rooms) reject("explicit phase correspondence omitted an original owner");
    if (report.correspondence.fresh.size()>maximum_rows) reject("fresh candidate coverage is incomplete");
    return report;
}
void admit_phase_review_components(const PhasePhysicalWallRoomCorrespondenceReport& report,std::set<std::string>& occupied) {
    const auto boundary_json=[](const Boundary& boundary) {
        auto result=Json::array();
        for (const auto& edge:boundary) result.push_back({{"start",{edge.start.x,edge.start.y}},
            {"end",{edge.end.x,edge.end.y}},{"sweep_radians",edge.sweep_radians}});
        return result;
    };
    for (const auto& candidate:report.correspondence.fresh) {
        auto holes=Json::array();
        for (const auto& hole:candidate.holes) holes.push_back(boundary_json(hole));
        std::set<std::string> support_ids;
        const auto capture_support=[&](const Json& face) {
            for (const auto& edge:face.at("edges")) for (const auto& use:edge.at("source_uses"))
                support_ids.insert(use.at("owner_id").get<std::string>());
        };
        capture_support(candidate.source_lineage.at("outer"));
        for (const auto& hole:candidate.source_lineage.at("holes")) capture_support(hole);
        auto physical_sources=Json::array();
        std::set<std::string> captured_support;
        for (const auto& source:candidate.source_lineage.at("physical_sources")) {
            const auto id=source.at("owner_id").get<std::string>();
            if (!support_ids.contains(id)) continue;
            if (!captured_support.insert(id).second) reject("destination component repeats supporting physical evidence");
            physical_sources.push_back(source);
        }
        if (support_ids.empty() || captured_support!=support_ids) reject("destination component lacks exact supporting physical inventory");
        // Query elevations within detector tolerance can discover the same
        // actual component twice. Bind actual supporting source evidence,
        // including source/effective elevations. Unrelated plane walls can
        // change inventory/graph indices without changing this component, so
        // neither those walls, query elevation, detector-local face indices nor
        // semantic phase bookkeeping distinguish it. All dispositions pass here.
        const auto encoded=Json{{"context",context_json(report.correspondence.context)},
            {"physical_sources",std::move(physical_sources)},
            {"boundary",boundary_json(candidate.boundary)},{"holes",std::move(holes)}}.dump();
        const auto key=sha256_hex(std::as_bytes(std::span(encoded.data(),encoded.size())));
        if (!occupied.insert(key).second) reject("destination component is repeated across reviewed context/planes");
    }
}
std::set<std::string> source_identity_inventory(const Entities& source) {
    std::set<std::string> result;
    for (const auto& [id,e]:source) {
        result.insert(id);
        if (!can_recognize_boundary_entity_type(e.type) || inspect_boundary_entity_version(e).format!=BoundaryEntityFormat::identified_v1) continue;
        for (const auto& edge:decode_identified_boundary_entity(e).segments) {
            result.insert(edge.segment_id); result.insert(edge.start_vertex_id); result.insert(edge.end_vertex_id);
        }
    }
    return result;
}
void collect_mentions(const Json& value,const std::set<std::string>& tokens,std::set<std::string>& result) {
    if (value.is_string()) { const auto& token=value.get_ref<const std::string&>(); if (tokens.contains(token)) result.insert(token); }
    else if (value.is_object()) for (const auto& [key,child]:value.items()) {
        if (tokens.contains(key)) result.insert(key);
        collect_mentions(child,tokens,result);
    } else if (value.is_array()) for (const auto& child:value) collect_mentions(child,tokens,result);
}
std::set<std::string> superseded_room_tokens(const Entities& source,const std::set<std::string>& superseded) {
    std::set<std::string> result;
    for (const auto& id:superseded) {
        result.insert(id);
        for (const auto& edge:decode_identified_boundary_entity(source.at(id)).segments) {
            result.insert(edge.segment_id); result.insert(edge.start_vertex_id); result.insert(edge.end_vertex_id);
        }
    }
    return result;
}
std::vector<PhysicalWallRoomPhaseBaselineAcknowledgement> baseline_dependents(const Entities& source,
    const std::string& registry_id,const std::set<std::string>& superseded) {
    const auto affected_tokens=superseded_room_tokens(source,superseded);
    std::vector<PhysicalWallRoomPhaseBaselineAcknowledgement> result;
    for (const auto& [id,e]:source) {
        auto properties=e.properties;
        if (superseded.contains(id)) {
            // Its own retained boundary topology is identity, not an incoming
            // dependent. Keep scanning all opaque segment/owner metadata and
            // extensions, including references to another superseded owner.
            for (auto& edge:properties.at("segments")) {
                edge.erase("segment_id"); edge.erase("start_vertex_id"); edge.erase("end_vertex_id");
            }
        }
        // The one named phase registry is the independently bound membership
        // authority. Its opaque fields still need explicit acknowledgement.
        if (id==registry_id) properties.erase("model");
        std::set<std::string> mentions;
        collect_mentions(properties,affected_tokens,mentions); collect_mentions(e.extensions,affected_tokens,mentions);
        if (mentions.empty()) continue;
        result.push_back({id,entity_digest(e),std::vector<std::string>(mentions.begin(),mentions.end())});
    }
    return result;
}
void witness(const PhysicalWallSpace& space,Vec2 point) {
    constexpr double radius=8*default_geometry_tolerance_metres;
    Boundary probe{{{point.x-radius,point.y-radius},{point.x+radius,point.y-radius},0},
        {{point.x+radius,point.y-radius},{point.x,point.y+radius},0},{{point.x,point.y+radius},{point.x-radius,point.y-radius},0}};
    auto holes=space.holes; holes.push_back(std::move(probe));
    if (const auto error=validate_boundary_holes(space.boundary,holes)) reject("witness is not strictly interior: "+*error);
}
Json relation_json(const RoomRelation& r) {
    std::string kind;
    if (r.kind==RoomRelationKind::independent) kind="independent";
    else if (r.kind==RoomRelationKind::follows) kind="follows";
    else if (r.kind==RoomRelationKind::derived_from) kind="derived_from";
    else reject("unsupported relationship kind");
    return {{"source_id",r.source_id},{"target_id",r.target_id},{"kind",kind}};
}
RoomRelation relation(const Json& value) {
    keys(value,{"source_id","target_id","kind"});
    RoomRelation result{value.at("source_id").get<std::string>(),value.at("target_id").get<std::string>(),RoomRelationKind::independent};
    identity(result.source_id); identity(result.target_id);
    const auto kind=value.at("kind").get<std::string>();
    if (kind=="follows") result.kind=RoomRelationKind::follows;
    else if (kind=="derived_from") result.kind=RoomRelationKind::derived_from;
    else if (kind!="independent") reject("unsupported relationship kind");
    return result;
}
bool mentions(const Json& value,const std::set<std::string>& tokens) {
    std::set<std::string> found; collect_mentions(value,tokens,found); return !found.empty();
}
std::set<std::string> original_target_proposals(const Entities& source,const PhysicalWallPhaseSelection& selection) {
    identity(selection.registry_id);
    if (!selection.alternative_id) reject("proposed-room editing requires a target alternative");
    identity(*selection.alternative_id);
    const auto registry=source.find(selection.registry_id);
    if (registry==source.end() || registry->second.type!="model_phases") reject("original target registry is missing");
    const auto model=ModelPhases::from_json(registry->second.properties.at("model"));
    (void)physical_wall_phase_states(source,selection);
    std::set<std::string> result;
    for (const auto& alternative:model.alternatives()) if (alternative.id==*selection.alternative_id)
        for (const auto& id:alternative.proposed_ids) if (is_physical_wall_room(source.at(id))) result.insert(id);
    return result;
}
PhysicalWallRoomPhaseProposedDependents proposed_dependents(const Entities& source,const std::set<std::string>& changed) {
    PhysicalWallRoomPhaseProposedDependents result;
    for (const auto& [id,e]:source) {
        bool affected=false;
        if (can_recognize_boundary_dimension_entity_type(e.type)) {
            const auto decoded=decode_boundary_dimension_entity(e);
            affected=decoded.supported() && changed.contains(decoded.dimension->boundary_id);
        } else if (e.type=="constraint") {
            const auto decoded=decode_constraint_entity(e);
            affected=decoded.supported() && std::any_of(decoded.constraint->bindings.begin(),decoded.constraint->bindings.end(),
                [&](const auto& binding){return changed.contains(binding.owner_id);});
        } else if (e.type=="room_relationships" && room_relationship_model_version(e.properties.at("model"))<=2) {
            const auto graph=RoomRelationshipSnapshot::from_json(e.properties.at("model"));
            PhysicalWallRoomRelationshipRemoval d; d.entity_id=id;
            for (const auto& ref:graph.references()) if (changed.contains(ref.id)) d.removed_room_ids.push_back(ref.id);
            if (!d.removed_room_ids.empty()) {
                const std::set<std::string> removed(d.removed_room_ids.begin(),d.removed_room_ids.end());
                for (const auto& r:graph.relations()) if (removed.contains(r.source_id) || removed.contains(r.target_id))
                    d.acknowledged_relations.push_back(r);
                result.relationship_removals.push_back(std::move(d));
            }
        }
        if (affected) result.reference_ids.push_back(id);
    }
    if (result.reference_ids.size()>maximum_rows || result.relationship_removals.size()>maximum_rows)
        reject("proposed dependent evidence exceeds budget");
    return result;
}
void filter_members(Json& array,const std::set<std::string>& removed) {
    array.erase(std::remove_if(array.begin(),array.end(),[&](const auto& value) {
        return removed.contains(value.template get<std::string>());
    }),array.end());
}
std::vector<PhysicalWallRoomPhasePresentationRemoval> derive_presentation_removals(Entities& result,const std::set<std::string>& removed) {
    std::vector<PhysicalWallRoomPhasePresentationRemoval> evidence;
    if (removed.empty()) return evidence;
    for (auto& [id,e]:result) {
        if (e.type!=kSheetViewEntityType && e.type!=kAnnotationEntityType) continue;
        identity(id);
        if (e.id!=id) reject("saved presentation identity differs from its source key");
        const auto before=e;
        std::set<std::string> affected;
        if (e.type==kSheetViewEntityType) {
            (void)decode_sheet_view_entity(e);
            bool changed=false;
            for (auto& view:e.properties.at("model").at("views")) {
                if (view.contains("object_ids")) {
                    const auto count=view.at("object_ids").size();
                    collect_mentions(view.at("object_ids"),removed,affected);
                    filter_members(view.at("object_ids"),removed);
                    if (view.at("object_ids").size()!=count) {
                        // An emptied restriction must remain restricted. Preserve
                        // the saved model dialect and every untouched raw row.
                        if (view.at("object_ids").empty()) view["restrict_to_objects"]=true;
                        changed=true;
                    }
                }
                if (view.contains("overlays")) {
                    auto& rows=view.at("overlays"); const auto count=rows.size();
                    rows.erase(std::remove_if(rows.begin(),rows.end(),[&](const auto& row) {
                        bool prune=false;
                        if (row.contains("object_id") && removed.contains(row.at("object_id").template get<std::string>())) {
                            affected.insert(row.at("object_id").template get<std::string>()); prune=true;
                        }
                        if (row.contains("dimension_binding") && row.at("dimension_binding").is_object() &&
                            removed.contains(row.at("dimension_binding").at("object_id").template get<std::string>())) {
                            affected.insert(row.at("dimension_binding").at("object_id").template get<std::string>()); prune=true;
                        }
                        return prune;
                    }),rows.end());
                    changed=changed || count!=rows.size();
                }
                auto& presentation=view.at("presentation");
                if (presentation.contains("appearance") && presentation.at("appearance").is_object() &&
                    presentation.at("appearance").contains("objects")) {
                    auto& rows=presentation.at("appearance").at("objects"); const auto count=rows.size();
                    rows.erase(std::remove_if(rows.begin(),rows.end(),[&](const auto& row) {
                        const auto token=row.at("object_id").template get<std::string>();
                        if (!removed.contains(token)) return false;
                        affected.insert(token); return true;
                    }),rows.end());
                    changed=changed || count!=rows.size();
                }
            }
            if (changed) validate_sheet_view_entity(e);
        } else if (e.type==kAnnotationEntityType) {
            validate_annotation_entity(e);
            auto& state=e.properties.at("state");
            if (!state.contains("overrides")) continue;
            auto& rows=state.at("overrides"); const auto count=rows.size();
            rows.erase(std::remove_if(rows.begin(),rows.end(),[&](const auto& row) {
                const auto token=row.at("target_id").template get<std::string>();
                if (!removed.contains(token)) return false;
                affected.insert(token); return true;
            }),rows.end());
            if (count!=rows.size()) validate_annotation_entity(e);
        }
        if (affected.empty()) continue;
        if (evidence.size()==maximum_rows) reject("presentation removal evidence exceeds budget");
        evidence.push_back({id,entity_digest(before),entity_digest(e),std::vector<std::string>(affected.begin(),affected.end())});
    }
    return evidence;
}
bool same_presentation_removal(const PhysicalWallRoomPhasePresentationRemoval& a,const PhysicalWallRoomPhasePresentationRemoval& b) {
    return a.entity_id==b.entity_id && a.expected_entity_digest==b.expected_entity_digest &&
        a.expected_replacement_entity_digest==b.expected_replacement_entity_digest && a.removed_entity_ids==b.removed_entity_ids;
}
void apply_reviewed_presentation_removals(const Entities& source,Entities& result,const PhysicalWallRoomPhaseReviewIntent& intent,
    const std::set<std::string>& removed,std::set<std::string>& allowed) {
    auto replacement=source;
    const auto expected=derive_presentation_removals(replacement,removed);
    if (expected.size()!=intent.presentation_removals.size()) reject("every affected saved presentation requires an exact removal acknowledgement");
    for (std::size_t i=0;i<expected.size();++i) {
        const auto& d=expected[i];
        if (!same_presentation_removal(d,intent.presentation_removals[i]))
            reject("saved presentation removal evidence changed or is incomplete: "+d.entity_id);
        const auto found=result.find(d.entity_id);
        if (found==result.end() || !exact_entity(source.at(d.entity_id),found->second))
            reject("saved presentation changed before its reviewed removal: "+d.entity_id);
        result.at(d.entity_id)=replacement.at(d.entity_id); allowed.insert(d.entity_id);
    }
}
void refuse_opaque_removed_presentation_rows(const Entities& source,
    const std::vector<PhysicalWallRoomPhasePresentationRemoval>& decisions,const std::set<std::string>& affected) {
    for (const auto& d:decisions) {
        const auto& e=source.at(d.entity_id);
        const std::set<std::string> removed(d.removed_entity_ids.begin(),d.removed_entity_ids.end());
        const auto check=[&](const Json& row) {
            if (mentions(row,affected)) reject("removed saved presentation row contains an unsupported incoming reference: "+d.entity_id);
        };
        if (e.type==kSheetViewEntityType) {
            for (const auto& view:e.properties.at("model").at("views")) {
                if (view.contains("overlays")) for (auto row:view.at("overlays")) {
                    const bool owner=row.contains("object_id") && removed.contains(row.at("object_id").get<std::string>());
                    const bool dimension=row.contains("dimension_binding") && row.at("dimension_binding").is_object() &&
                        removed.contains(row.at("dimension_binding").at("object_id").get<std::string>());
                    if (!owner && !dimension) continue;
                    row.erase("object_id");
                    if (row.contains("dimension_binding") && row.at("dimension_binding").is_object()) row.at("dimension_binding").erase("object_id");
                    check(row);
                }
                const auto& p=view.at("presentation");
                if (p.contains("appearance") && p.at("appearance").is_object() && p.at("appearance").contains("objects"))
                    for (auto row:p.at("appearance").at("objects")) if (removed.contains(row.at("object_id").get<std::string>())) {
                        row.erase("object_id"); check(row);
                    }
            }
        } else if (e.type==kAnnotationEntityType) {
            const auto& state=e.properties.at("state");
            if (state.contains("overrides")) for (auto row:state.at("overrides")) if (removed.contains(row.at("target_id").get<std::string>())) {
                row.erase("target_id"); check(row);
            }
        }
    }
}
Json baseline_referencing_rows(const Json& rows,const std::set<std::string>& baseline_tokens) {
    auto result=Json::array();
    for (const auto& row:rows) if (mentions(row,baseline_tokens)) result.push_back(row);
    return result;
}
// Qualified presentation pruning preserves all surviving raw rows and metadata.
// A row mentioning both a baseline token and a removed proposal/reference must
// nevertheless survive byte-exact; reviewing its removal does not transfer the
// independently acknowledged baseline ownership authority.
void validate_baseline_presentation_preservation(const Entity& before,const Entity& after,const std::set<std::string>& baseline_tokens) {
    const auto preserve_rows=[&](const Json& old_rows,const Json& next_rows) {
        if (baseline_referencing_rows(old_rows,baseline_tokens).dump()!=baseline_referencing_rows(next_rows,baseline_tokens).dump())
            reject("reviewed presentation removal would change a baseline-only referenced row: "+before.id);
    };
    if (before.type==kSheetViewEntityType) {
        const auto& old_views=before.properties.at("model").at("views");
        const auto& next_views=after.properties.at("model").at("views");
        if (old_views.size()!=next_views.size()) reject("baseline-only view coverage changed");
        for (std::size_t i=0;i<old_views.size();++i) {
            const auto& old=old_views.at(i); const auto& next=next_views.at(i);
            for (const auto* field:{"object_ids","overlays"}) if (old.contains(field)) preserve_rows(old.at(field),next.at(field));
            const auto& p=old.at("presentation");
            if (p.contains("appearance") && p.at("appearance").is_object() && p.at("appearance").contains("objects"))
                preserve_rows(p.at("appearance").at("objects"),next.at("presentation").at("appearance").at("objects"));
            if (mentions(old,baseline_tokens) && old.value("restrict_to_objects",false)!=next.value("restrict_to_objects",false))
                reject("reviewed presentation removal would change a baseline-only view restriction: "+before.id);
        }
    } else if (before.type==kAnnotationEntityType) {
        const auto& state=before.properties.at("state");
        if (state.contains("overrides")) preserve_rows(state.at("overrides"),after.properties.at("state").at("overrides"));
    } else reject("baseline presentation exception requires an actual supported presentation entity");
}
void validate_baseline_dependent_preservation(const Entities& source,const Entities& result,const std::string& registry_id,
    const std::vector<PhysicalWallRoomPhaseBaselineAcknowledgement>& baseline,
    const std::vector<PhysicalWallRoomPhasePresentationRemoval>& presentation) {
    std::map<std::string,const PhysicalWallRoomPhasePresentationRemoval*,std::less<>> presentation_decisions;
    for (const auto& d:presentation) presentation_decisions.emplace(d.entity_id,&d);
    for (const auto& d:baseline) {
        const auto found=result.find(d.entity_id);
        if (found==result.end()) reject("baseline-only acknowledged dependent cannot be removed: "+d.entity_id);
        auto before=source.at(d.entity_id),after=found->second;
        if (exact_entity(before,after)) continue;
        if (d.entity_id==registry_id) {
            // Named registry membership has separate exact child/replay
            // authority; its acknowledged opaque fields remain unchanged.
            before.properties.erase("model"); after.properties.erase("model");
            if (!exact_entity(before,after)) reject("baseline-only registry metadata changed");
            continue;
        }
        const auto reviewed=presentation_decisions.find(d.entity_id);
        if (reviewed==presentation_decisions.end() ||
            (before.type!=kSheetViewEntityType && before.type!=kAnnotationEntityType) ||
            entity_digest(found->second)!=reviewed->second->expected_replacement_entity_digest)
            reject("baseline-only acknowledged dependent must remain exact; its proposal reference cannot be removed or remapped: "+d.entity_id);
        validate_baseline_presentation_preservation(before,after,std::set<std::string>(d.referenced_ids.begin(),d.referenced_ids.end()));
    }
}
void prune_relationships(Entities& result,const PhysicalWallRoomPhaseReviewIntent& intent,
    const std::set<std::string>& retiring,std::set<std::string>& allowed) {
    for (const auto& d:intent.relationship_removals) {
        const auto found=result.find(d.entity_id);
        if (found==result.end() || found->second.type!="room_relationships") reject("relationship removal target is not an actual graph");
        auto& model=found->second.properties.at("model");
        if (room_relationship_model_version(model)>2) reject("unknown relationship model cannot be rewritten");
        const auto graph=RoomRelationshipSnapshot::from_json(model);
        const std::set<std::string> removed(d.removed_room_ids.begin(),d.removed_room_ids.end());
        std::set<std::string> present;
        for (const auto& ref:graph.references()) if (removed.contains(ref.id)) present.insert(ref.id);
        if (present!=removed || !std::all_of(removed.begin(),removed.end(),[&](const auto& id){return retiring.contains(id);}))
            reject("relationship removals must name actual retiring proposed owners");
        std::set<std::string> incident,acknowledged;
        for (const auto& r:graph.relations()) if (removed.contains(r.source_id) || removed.contains(r.target_id)) incident.insert(relation_json(r).dump());
        for (const auto& r:d.acknowledged_relations) acknowledged.insert(relation_json(r).dump());
        if (incident!=acknowledged) reject("every removed relationship row needs exact acknowledgement");
        auto& references=model.at("references");
        references.erase(std::remove_if(references.begin(),references.end(),[&](const auto& row) {
            return removed.contains(row.at("id").template get<std::string>());
        }),references.end());
        auto& relations=model.at("relations");
        relations.erase(std::remove_if(relations.begin(),relations.end(),[&](const auto& row) {
            return removed.contains(row.at("source_id").template get<std::string>()) || removed.contains(row.at("target_id").template get<std::string>());
        }),relations.end());
        (void)RoomRelationshipSnapshot::from_json(model); allowed.insert(d.entity_id);
    }
}
// Before lower repair, known analytical target fields may retain old children
// only for explicitly kept, supported references. Everything else is scanned,
// including JSON keys. Historical exemptions require exact source receipts and
// independent source qualification, never a historical-looking field name.
void refuse_opaque_proposed_dependents(const Entities& source,const Entities& candidate,
    const std::string& registry_id,const std::set<std::string>& changed,const std::set<std::string>& retiring,
    const std::set<std::string>& affected,const std::set<std::string>& kept) {
    if (const auto error=validate_boundary_integrity(source)) reject(*error);
    const auto organization=organize_project(source);
    const auto unchanged=[](const Json& before,const Json& after,const char* field) {
        return before.is_object() && after.is_object() && before.contains(field) && after.contains(field) &&
            before.at(field).dump()==after.at(field).dump();
    };
    for (const auto& [id,e]:candidate) {
        auto properties=e.properties,extensions=e.extensions;
        const auto previous=source.find(id);
        if (previous!=source.end() && previous->second.type==e.type) {
            const bool identified=can_recognize_boundary_entity_type(e.type) &&
                inspect_boundary_entity_version(e).format==BoundaryEntityFormat::identified_v1;
            if (identified) {
                if (unchanged(previous->second.properties,properties,"boundary_authoring")) properties.erase("boundary_authoring");
                if (unchanged(previous->second.extensions,extensions,"boundary_geometry_derivation")) extensions.erase("boundary_geometry_derivation");
                if (e.type=="measurement_boundary" && unchanged(previous->second.properties,properties,"wall_measurement_source")) {
                    (void)exterior_wall_measurement_source_ids(e); properties.erase("wall_measurement_source");
                }
                if (is_physical_wall_room(e) && unchanged(previous->second.extensions,extensions,"physical_wall_room")) {
                    const auto c=organization.drawing_context(id);
                    if (!c || !c->complete()) reject("historical room evidence has unresolved context");
                    (void)validate_retained_physical_wall_room_lineage(e,*c); extensions.erase("physical_wall_room");
                }
            }
            if (e.type=="wall") {
                if (unchanged(previous->second.extensions,extensions,"wall_split_archive") && extensions.at("wall_split_archive").value("version",0)==1) {
                    validate_wall_split_archive(e); extensions.erase("wall_split_archive");
                }
                if (unchanged(previous->second.extensions,extensions,"wall_merge_archive") && extensions.at("wall_merge_archive").value("version",0)==1) {
                    validate_wall_merge_archive(e); extensions.erase("wall_merge_archive");
                }
            }
        }
        if (id==registry_id) properties.erase("model");
        if (changed.contains(id)) {
            for (auto& segment:properties.at("segments"))
                for (const auto* field:{"segment_id","start_vertex_id","end_vertex_id","start","end","sweep_radians"}) segment.erase(field);
        }
        if (can_recognize_boundary_dimension_entity_type(e.type)) {
            const auto decoded=decode_boundary_dimension_entity(e);
            if (decoded.supported() && changed.contains(decoded.dimension->boundary_id)) {
                if (!kept.contains(id) || retiring.contains(decoded.dimension->boundary_id)) reject("retired room dimension needs explicit removal");
                properties.erase("boundary_id");
                if (properties.contains("target") && properties.at("target").is_object())
                    for (const auto* field:{"entity_id","segment_id","segment_ids","second_segment_id","vertex_id"}) properties.at("target").erase(field);
            }
        } else if (e.type=="constraint") {
            const auto decoded=decode_constraint_entity(e);
            if (decoded.supported()) {
                const bool touched=std::any_of(decoded.constraint->bindings.begin(),decoded.constraint->bindings.end(),[&](const auto& b){return changed.contains(b.owner_id);});
                if (touched) {
                    if (!kept.contains(id) || std::any_of(decoded.constraint->bindings.begin(),decoded.constraint->bindings.end(),[&](const auto& b){return retiring.contains(b.owner_id);}))
                        reject("retired room constraint needs explicit removal");
                    properties.erase("entity_ids"); properties.erase("wall_ids");
                    for (auto& binding:properties.at("bindings"))
                        for (const auto* field:{"owner_id","feature","segment_id","vertex_id","role"}) binding.erase(field);
                }
            }
        }
        if (mentions(properties,affected) || mentions(extensions,affected)) reject("unsupported incoming proposed-room reference in "+id);
    }
}
} // namespace

PhysicalWallRoomPhaseReviewIntent decode_physical_wall_phase_room_review_intent(const Json& value) {
    try {
        if (value.dump().size()>1024*1024) reject("intent exceeds one MiB");
        if (!value.is_object() || !value.contains("version") || !value.at("version").is_number_integer() ||
            (value.at("version")!=1 && value.at("version")!=2)) reject("unsupported intent version");
        const bool completion=value.at("version")==2;
        if (completion) keys(value,{"version","source_snapshot_digest","source_authoring_digest","source_saved_revision","source_entities_digest",
            "expected_revision","registry_id","alternative_id","source_registry_entity_digest","registry_command_proof","planes","baseline_only_acknowledgements",
            "proposed_room_completion","removed_reference_ids","kept_reference_ids","relationship_removals","presentation_removals"});
        else keys(value,{"version","source_snapshot_digest","source_authoring_digest","source_saved_revision","source_entities_digest",
            "expected_revision","registry_id","alternative_id","source_registry_entity_digest","registry_command_proof","planes","baseline_only_acknowledgements"});
        PhysicalWallRoomPhaseReviewIntent result;
        result.proposed_room_completion=completion;
        if (completion && (!value.at("proposed_room_completion").is_boolean() || value.at("proposed_room_completion")!=true))
            reject("version two requires proposed-room completion authority");
        result.source_snapshot_digest=value.at("source_snapshot_digest").get<std::string>(); digest(result.source_snapshot_digest);
        result.source_authoring_digest=value.at("source_authoring_digest").get<std::string>(); digest(result.source_authoring_digest);
        result.source_entities_digest=value.at("source_entities_digest").get<std::string>(); digest(result.source_entities_digest);
        if (!value.at("source_saved_revision").is_null()) result.source_saved_revision=revision(value.at("source_saved_revision"));
        result.expected_revision=revision(value.at("expected_revision"));
        if (result.source_saved_revision && *result.source_saved_revision>result.expected_revision) reject("captured save revision exceeds source head");
        result.registry_id=value.at("registry_id").get<std::string>(); identity(result.registry_id);
        if (!value.at("alternative_id").is_null()) { result.alternative_id=value.at("alternative_id").get<std::string>(); identity(*result.alternative_id); }
        if (!value.at("source_registry_entity_digest").is_null()) {
            result.source_registry_entity_digest=value.at("source_registry_entity_digest").get<std::string>(); digest(*result.source_registry_entity_digest);
        }
        result.registry_command_proof=value.at("registry_command_proof");
        (void)registry_command(result.registry_command_proof,result.expected_revision,result.registry_id);
        if (!value.at("planes").is_array() || value.at("planes").size()>32 ||
            !value.at("baseline_only_acknowledgements").is_array() || value.at("baseline_only_acknowledgements").size()>maximum_rows)
            reject("malformed or excessive plane/reference collection");
        std::size_t rooms=0,fresh=0;
        std::set<std::string> source_ids,assigned_ids;
        std::vector<Plane> planes;
        for (const auto& row:value.at("planes")) {
            keys(row,{"context","effective_elevation_m","source_rooms","fresh"});
            PhysicalWallRoomPhasePlaneReview plane; plane.context=context(row.at("context"));
            if (!row.at("effective_elevation_m").is_number()) reject("invalid elevation");
            plane.effective_elevation_m=row.at("effective_elevation_m").get<double>();
            const auto count=planes.size(); add_plane(planes,{plane.context,plane.effective_elevation_m});
            if (count==planes.size()) reject("duplicate reviewed context/plane");
            if (!row.at("source_rooms").is_array() || !row.at("fresh").is_array()) reject("malformed decision collection");
            rooms+=row.at("source_rooms").size(); fresh+=row.at("fresh").size();
            if (rooms>maximum_rows || fresh>maximum_rows) reject("room decision budget exceeded");
            for (const auto& old:row.at("source_rooms")) {
                if (completion) keys(old,{"room_id","expected_descriptor_digest","disposition","child_mapping","replacement_dimension_ids"});
                else keys(old,{"room_id","expected_descriptor_digest","disposition"});
                PhysicalWallRoomPhaseSourceDecision d; d.room_id=old.at("room_id").get<std::string>(); identity(d.room_id);
                if (!source_ids.insert(d.room_id).second) reject("duplicate original room decision");
                d.expected_descriptor_digest=old.at("expected_descriptor_digest").get<std::string>(); digest(d.expected_descriptor_digest);
                const auto action=old.at("disposition").get<std::string>();
                if (action=="supersede_in_target") d.disposition=PhysicalWallRoomPhaseSourceDisposition::supersede_in_target;
                else if (completion && action=="redefine_proposed") d.disposition=PhysicalWallRoomPhaseSourceDisposition::redefine_proposed;
                else if (completion && action=="retire_proposed") d.disposition=PhysicalWallRoomPhaseSourceDisposition::retire_proposed;
                else if (action!="share_unchanged") reject("unsupported source disposition");
                if (completion) {
                    d.child_mapping=old.at("child_mapping");
                    if (!d.child_mapping.is_object()) reject("malformed child mapping");
                    if (!d.child_mapping.empty()) {
                        keys(d.child_mapping,{"segments","vertices"});
                        for (const auto* group:{"segments","vertices"}) {
                            const auto& mappings=d.child_mapping.at(group);
                            if (!mappings.is_object() || mappings.size()>65536) reject("malformed child mapping group");
                            std::set<std::string> targets;
                            for (const auto& [old_id,target]:mappings.items()) {
                                identity(old_id); const auto new_id=target.get<std::string>(); identity(new_id);
                                if (!targets.insert(new_id).second) reject("child mapping cannot merge identities");
                            }
                        }
                    }
                    d.replacement_dimension_ids=old.at("replacement_dimension_ids").get<std::vector<std::string>>(); identities(d.replacement_dimension_ids);
                    if (d.disposition!=PhysicalWallRoomPhaseSourceDisposition::redefine_proposed &&
                        (!d.child_mapping.empty() || !d.replacement_dimension_ids.empty())) reject("only redefinition can map children or regenerate dimensions");
                    if ((d.disposition==PhysicalWallRoomPhaseSourceDisposition::redefine_proposed ||
                        d.disposition==PhysicalWallRoomPhaseSourceDisposition::retire_proposed) && !result.alternative_id)
                        reject("proposed-room editing requires a target alternative");
                }
                plane.source_rooms.push_back(std::move(d));
            }
            std::set<std::size_t> candidates;
            for (const auto& next:row.at("fresh")) {
                keys(next,{"candidate_index","reviewed_source_lineage","disposition","room_id","interior_witness","segment_ids","vertex_ids","name","classification","factor"});
                PhysicalWallRoomPhaseFreshDecision d;
                const auto index=revision(next.at("candidate_index")); if (index>=maximum_rows) reject("invalid candidate index");
                d.candidate_index=static_cast<std::size_t>(index); if (!candidates.insert(d.candidate_index).second) reject("duplicate candidate decision");
                d.reviewed_source_lineage=next.at("reviewed_source_lineage"); if (!d.reviewed_source_lineage.is_object()) reject("malformed reviewed lineage");
                const auto action=next.at("disposition").get<std::string>();
                if (action=="create_proposed") d.disposition=PhysicalWallRoomPhaseFreshDisposition::create_proposed;
                else if (completion && action=="redefine_proposed") d.disposition=PhysicalWallRoomPhaseFreshDisposition::redefine_proposed;
                else if (action=="leave_unclassified") d.disposition=PhysicalWallRoomPhaseFreshDisposition::leave_unclassified;
                else if (action!="share_unchanged") reject("unsupported fresh disposition");
                d.room_id=next.at("room_id").get<std::string>();
                if (d.disposition==PhysicalWallRoomPhaseFreshDisposition::leave_unclassified) {
                    if (!d.room_id.empty()) reject("unclassified candidate cannot claim an owner");
                } else {
                    identity(d.room_id);
                    if (!assigned_ids.insert(d.room_id).second) reject("one owner cannot cover multiple candidates");
                }
                const auto& point=next.at("interior_witness");
                if (!point.is_array() || point.size()!=2 || !point[0].is_number() || !point[1].is_number()) reject("malformed witness");
                d.interior_witness={point[0].get<double>(),point[1].get<double>()};
                if (!std::isfinite(d.interior_witness.x) || !std::isfinite(d.interior_witness.y)) reject("nonfinite witness");
                d.fresh_ids.segment_ids=next.at("segment_ids").get<std::vector<std::string>>(); identities(d.fresh_ids.segment_ids);
                d.fresh_ids.vertex_ids=next.at("vertex_ids").get<std::vector<std::string>>(); identities(d.fresh_ids.vertex_ids);
                d.name=next.at("name").get<std::string>(); d.classification=next.at("classification").get<std::string>();
                if (!next.at("factor").is_null()) {
                    if (!next.at("factor").is_number()) reject("factor must be explicit numeric metadata");
                    d.factor=next.at("factor").get<double>();
                    if (!std::isfinite(*d.factor) || *d.factor<0 || *d.factor>1000000.0) reject("invalid factor");
                }
                if (d.disposition==PhysicalWallRoomPhaseFreshDisposition::create_proposed) {
                    if (!text(d.name) || !text(d.classification) || !d.factor || !result.alternative_id) reject("creation requires explicit facts and target alternative");
                } else if (d.disposition==PhysicalWallRoomPhaseFreshDisposition::redefine_proposed) {
                    if (!result.alternative_id || !d.name.empty() || !d.classification.empty() || d.factor)
                        reject("redefinition preserves the original owner facts");
                } else if (!d.name.empty() || !d.classification.empty() || d.factor || !d.fresh_ids.segment_ids.empty() || !d.fresh_ids.vertex_ids.empty())
                    reject("sharing cannot redefine identity or facts");
                plane.fresh.push_back(std::move(d));
            }
            result.planes.push_back(std::move(plane));
        }
        std::set<std::string> references;
        for (const auto& row:value.at("baseline_only_acknowledgements")) {
            keys(row,{"entity_id","expected_entity_digest","referenced_ids","disposition"});
            if (row.at("disposition")!="preserve_baseline_only") reject("unsupported dependent disposition");
            PhysicalWallRoomPhaseBaselineAcknowledgement d;
            d.entity_id=row.at("entity_id").get<std::string>(); identity(d.entity_id);
            if (!references.insert(d.entity_id).second) reject("duplicate dependent acknowledgement");
            d.expected_entity_digest=row.at("expected_entity_digest").get<std::string>(); digest(d.expected_entity_digest);
            d.referenced_ids=row.at("referenced_ids").get<std::vector<std::string>>(); identities(d.referenced_ids);
            if (d.referenced_ids.empty() || !std::is_sorted(d.referenced_ids.begin(),d.referenced_ids.end())) reject("acknowledgement needs canonical exact affected tokens");
            result.baseline_only_acknowledgements.push_back(std::move(d));
        }
        if (completion) {
            result.removed_reference_ids=value.at("removed_reference_ids").get<std::vector<std::string>>(); identities(result.removed_reference_ids);
            result.kept_reference_ids=value.at("kept_reference_ids").get<std::vector<std::string>>(); identities(result.kept_reference_ids);
            const std::set<std::string> removed(result.removed_reference_ids.begin(),result.removed_reference_ids.end());
            for (const auto& id:result.kept_reference_ids) if (removed.contains(id)) reject("reference cannot be both kept and removed");
            const auto& rows=value.at("relationship_removals");
            if (!rows.is_array() || rows.size()>maximum_rows) reject("malformed relationship removal collection");
            std::set<std::string> graphs;
            for (const auto& row:rows) {
                keys(row,{"entity_id","removed_room_ids","acknowledged_relations"});
                PhysicalWallRoomRelationshipRemoval d;
                d.entity_id=row.at("entity_id").get<std::string>(); identity(d.entity_id);
                if (!graphs.insert(d.entity_id).second) reject("duplicate relationship decision");
                d.removed_room_ids=row.at("removed_room_ids").get<std::vector<std::string>>(); identities(d.removed_room_ids);
                if (d.removed_room_ids.empty() || !row.at("acknowledged_relations").is_array() || row.at("acknowledged_relations").size()>65536)
                    reject("malformed relationship removal");
                std::set<std::string> relations;
                for (const auto& r:row.at("acknowledged_relations")) {
                    auto decoded=relation(r);
                    if (!relations.insert(relation_json(decoded).dump()).second) reject("duplicate relationship acknowledgement");
                    d.acknowledged_relations.push_back(std::move(decoded));
                }
                result.relationship_removals.push_back(std::move(d));
            }
            const auto& presentations=value.at("presentation_removals");
            if (!presentations.is_array() || presentations.size()>maximum_rows) reject("malformed presentation removal collection");
            std::string previous;
            for (const auto& row:presentations) {
                keys(row,{"entity_id","expected_entity_digest","expected_replacement_entity_digest","removed_entity_ids"});
                PhysicalWallRoomPhasePresentationRemoval d;
                d.entity_id=row.at("entity_id").get<std::string>(); identity(d.entity_id);
                if (!previous.empty() && d.entity_id<=previous) reject("presentation removal records must have unique sorted entity identities");
                previous=d.entity_id;
                d.expected_entity_digest=row.at("expected_entity_digest").get<std::string>(); digest(d.expected_entity_digest);
                d.expected_replacement_entity_digest=row.at("expected_replacement_entity_digest").get<std::string>(); digest(d.expected_replacement_entity_digest);
                if (d.expected_entity_digest==d.expected_replacement_entity_digest) reject("presentation removal must change a known saved membership");
                d.removed_entity_ids=row.at("removed_entity_ids").get<std::vector<std::string>>(); identities(d.removed_entity_ids);
                if (d.removed_entity_ids.empty() || !std::is_sorted(d.removed_entity_ids.begin(),d.removed_entity_ids.end()))
                    reject("presentation removal needs sorted exact affected identities");
                result.presentation_removals.push_back(std::move(d));
            }
        }
        return result;
    } catch (const Json::exception&) { reject("malformed intent value types"); }
}

Json encode_physical_wall_phase_room_review_intent(const PhysicalWallRoomPhaseReviewIntent& intent) {
    Json planes=Json::array(),references=Json::array();
    for (const auto& plane:intent.planes) {
        Json source=Json::array(),fresh=Json::array();
        for (const auto& d:plane.source_rooms) {
            std::string action;
            if (d.disposition==PhysicalWallRoomPhaseSourceDisposition::share_unchanged) action="share_unchanged";
            else if (d.disposition==PhysicalWallRoomPhaseSourceDisposition::supersede_in_target) action="supersede_in_target";
            else if (intent.proposed_room_completion && d.disposition==PhysicalWallRoomPhaseSourceDisposition::redefine_proposed) action="redefine_proposed";
            else if (intent.proposed_room_completion && d.disposition==PhysicalWallRoomPhaseSourceDisposition::retire_proposed) action="retire_proposed";
            else reject("unsupported source disposition");
            source.push_back({{"room_id",d.room_id},{"expected_descriptor_digest",d.expected_descriptor_digest},{"disposition",action}});
            if (intent.proposed_room_completion) {
                source.back()["child_mapping"]=d.child_mapping;
                source.back()["replacement_dimension_ids"]=d.replacement_dimension_ids;
            } else if (!d.child_mapping.empty() || !d.replacement_dimension_ids.empty()) reject("version one cannot carry redefinition reference decisions");
        }
        for (const auto& d:plane.fresh) {
            std::string action;
            if (d.disposition==PhysicalWallRoomPhaseFreshDisposition::share_unchanged) action="share_unchanged";
            else if (d.disposition==PhysicalWallRoomPhaseFreshDisposition::create_proposed) action="create_proposed";
            else if (d.disposition==PhysicalWallRoomPhaseFreshDisposition::leave_unclassified) action="leave_unclassified";
            else if (intent.proposed_room_completion && d.disposition==PhysicalWallRoomPhaseFreshDisposition::redefine_proposed) action="redefine_proposed";
            else reject("unsupported fresh disposition");
            fresh.push_back({{"candidate_index",d.candidate_index},{"reviewed_source_lineage",d.reviewed_source_lineage},{"disposition",action},
                {"room_id",d.room_id},{"interior_witness",{d.interior_witness.x,d.interior_witness.y}},
                {"segment_ids",d.fresh_ids.segment_ids},{"vertex_ids",d.fresh_ids.vertex_ids},{"name",d.name},{"classification",d.classification},
                {"factor",d.factor ? Json(*d.factor) : Json(nullptr)}});
        }
        planes.push_back({{"context",context_json(plane.context)},{"effective_elevation_m",plane.effective_elevation_m},
            {"source_rooms",std::move(source)},{"fresh",std::move(fresh)}});
    }
    for (const auto& d:intent.baseline_only_acknowledgements)
        references.push_back({{"entity_id",d.entity_id},{"expected_entity_digest",d.expected_entity_digest},{"referenced_ids",d.referenced_ids},{"disposition","preserve_baseline_only"}});
    Json encoded{{"version",intent.proposed_room_completion ? 2 : 1},{"source_snapshot_digest",intent.source_snapshot_digest},{"source_authoring_digest",intent.source_authoring_digest},
        {"source_saved_revision",intent.source_saved_revision ? Json(*intent.source_saved_revision) : Json(nullptr)},
        {"source_entities_digest",intent.source_entities_digest},{"expected_revision",intent.expected_revision},{"registry_id",intent.registry_id},
        {"alternative_id",intent.alternative_id ? Json(*intent.alternative_id) : Json(nullptr)},
        {"source_registry_entity_digest",intent.source_registry_entity_digest ? Json(*intent.source_registry_entity_digest) : Json(nullptr)},
        {"registry_command_proof",intent.registry_command_proof},{"planes",std::move(planes)},{"baseline_only_acknowledgements",std::move(references)}};
    if (intent.proposed_room_completion) {
        encoded["proposed_room_completion"]=true;
        encoded["removed_reference_ids"]=intent.removed_reference_ids;
        encoded["kept_reference_ids"]=intent.kept_reference_ids;
        auto relationships=Json::array();
        for (const auto& d:intent.relationship_removals) {
            auto rows=Json::array(); for (const auto& r:d.acknowledged_relations) rows.push_back(relation_json(r));
            relationships.push_back({{"entity_id",d.entity_id},{"removed_room_ids",d.removed_room_ids},{"acknowledged_relations",std::move(rows)}});
        }
        encoded["relationship_removals"]=std::move(relationships);
        auto presentations=Json::array();
        for (const auto& d:intent.presentation_removals)
            presentations.push_back({{"entity_id",d.entity_id},{"expected_entity_digest",d.expected_entity_digest},
                {"expected_replacement_entity_digest",d.expected_replacement_entity_digest},{"removed_entity_ids",d.removed_entity_ids}});
        encoded["presentation_removals"]=std::move(presentations);
    } else if (!intent.removed_reference_ids.empty() || !intent.kept_reference_ids.empty() || !intent.relationship_removals.empty() || !intent.presentation_removals.empty())
        reject("version one cannot carry proposed-room reference decisions");
    (void)decode_physical_wall_phase_room_review_intent(encoded); return encoded;
}

PhysicalWallRoomPhaseReviewInventory inspect_physical_wall_phase_room_review(
    const DocumentSnapshot& source,const ApplyEntityChanges& command,const PhysicalWallPhaseSelection& destination) {
    if (!source.is_editable()) reject("captured document is read-only");
    PhysicalWallRoomPhaseReviewInventory result;
    auto& intent=result.intent;
    intent.source_snapshot_digest=document_snapshot_digest(source);
    intent.source_authoring_digest=document_authoring_source_digest_v2(source);
    intent.source_saved_revision=source.saved_revision_optional();
    intent.source_entities_digest=entity_map_digest(source.entities());
    intent.expected_revision=source.revision();
    intent.registry_id=destination.registry_id;
    intent.alternative_id=destination.alternative_id;
    if (const auto found=source.entities().find(intent.registry_id); found!=source.entities().end())
        intent.source_registry_entity_digest=entity_digest(found->second);
    intent.registry_command_proof=command_to_json(Command{command});
    // Admit the full source binding and canonical child before deriving any
    // evidence. Empty decision collections are preparation, not acceptance.
    (void)encode_physical_wall_phase_room_review_intent(intent);
    const auto transition=registry_transition(source.entities(),intent);
    if (!transition.created && intent.alternative_id)
        intent.proposed_room_completion=!original_target_proposals(source.entities(),destination).empty();
    const auto coverage=phase_review_coverage(source.entities(),intent,transition);
    const auto occupied=source_identity_inventory(source.entities());
    if (transition.created && occupied.contains(intent.registry_id)) reject("fresh identity is already occupied: "+intent.registry_id);
    std::set<std::string> reviewed_components;
    std::size_t fresh_count=0;
    for (const auto& plane:coverage.affected) {
        const auto rooms=phase_review_plane_rooms(coverage,plane);
        auto report=phase_review_plane_report(source.entities(),coverage,destination,plane,rooms);
        admit_phase_review_components(report,reviewed_components);
        if (report.correspondence.fresh.size()>maximum_rows-fresh_count) reject("room decision budget exceeded");
        fresh_count+=report.correspondence.fresh.size();
        intent.planes.push_back({plane.context,plane.elevation,{},{}});
        result.reports.push_back(std::move(report));
    }
    (void)encode_physical_wall_phase_room_review_intent(intent);
    return result;
}

PhysicalWallRoomPhaseProposedDependents physical_wall_phase_room_proposed_dependents(
    const DocumentSnapshot& source,const PhysicalWallPhaseSelection& destination,
    const std::vector<std::string>& changed_proposed_room_ids) {
    if (!source.is_editable()) reject("captured document is read-only");
    identities(changed_proposed_room_ids);
    if (changed_proposed_room_ids.size()>maximum_rows) reject("proposed room evidence exceeds budget");
    const auto proposals=original_target_proposals(source.entities(),destination);
    const std::set<std::string> changed(changed_proposed_room_ids.begin(),changed_proposed_room_ids.end());
    const auto organization=organize_project(source.entities());
    for (const auto& id:changed) {
        if (!proposals.contains(id)) reject("changed owner is not an original proposal of the named target");
        const auto c=organization.drawing_context(id);
        if (!c || !c->complete()) reject("proposed room context is unresolved");
        (void)validate_retained_physical_wall_room_lineage(source.entities().at(id),*c);
    }
    return proposed_dependents(source.entities(),changed);
}

std::vector<PhysicalWallRoomPhasePresentationRemoval> physical_wall_phase_room_presentation_removals(
    const DocumentSnapshot& source,const std::vector<std::string>& removed_entity_ids) {
    if (!source.is_editable()) reject("captured document is read-only");
    identities(removed_entity_ids);
    const auto& entities=source.entities();
    for (const auto& id:removed_entity_ids) {
        const auto found=entities.find(id);
        if (found==entities.end() || found->second.id!=id) reject("presentation removal evidence requires actual original entity identities");
    }
    auto detached=entities;
    auto result=derive_presentation_removals(detached,std::set<std::string>(removed_entity_ids.begin(),removed_entity_ids.end()));
    auto budget=Json::array();
    for (const auto& d:result) budget.push_back({{"entity_id",d.entity_id},{"expected_entity_digest",d.expected_entity_digest},
        {"expected_replacement_entity_digest",d.expected_replacement_entity_digest},{"removed_entity_ids",d.removed_entity_ids}});
    if (budget.dump().size()>1024*1024) reject("presentation removal evidence exceeds one MiB");
    return result;
}

std::vector<PhysicalWallRoomPhaseBaselineAcknowledgement> physical_wall_phase_room_baseline_dependents(
    const DocumentSnapshot& source,const std::string& registry_id,const std::vector<std::string>& superseded_room_ids) {
    if (!source.is_editable()) reject("captured document is read-only");
    identity(registry_id); identities(superseded_room_ids);
    if (superseded_room_ids.size()>maximum_rows) reject("room decision budget exceeded");
    const auto& entities=source.entities();
    const auto registry=entities.find(registry_id);
    std::optional<ModelPhases> model;
    std::set<std::string> foreign_registered;
    if (registry!=entities.end()) {
        if (registry->second.type!="model_phases" || registry->second.id!=registry_id) reject("named registry is not an actual model_phases entity");
        model=ModelPhases::from_json(registry->second.properties.at("model"));
        (void)physical_wall_phase_states(entities,{registry_id,std::nullopt});
    } else {
        if (source_identity_inventory(entities).contains(registry_id)) reject("fresh identity is already occupied: "+registry_id);
        // A not-yet-created registry has no synthetic selection here. Admit
        // every actual existing registry using its saved alternative instead.
        for (const auto& [id,e]:entities) if (e.type=="model_phases") {
            const auto existing=ModelPhases::from_json(e.properties.at("model"));
            for (const auto& phase:physical_wall_phase_states(entities,{id,existing.active_alternative()}))
                foreign_registered.insert(phase.registered_entity_ids.begin(),phase.registered_entity_ids.end());
            break;
        }
    }
    const auto organization=organize_project(entities);
    const std::set<std::string> superseded(superseded_room_ids.begin(),superseded_room_ids.end());
    for (const auto& [id,e]:entities) if (id!=e.id) reject("source entity identity differs from its key");
    for (const auto& id:superseded) {
        const auto room=entities.find(id);
        if (room==entities.end() || !is_physical_wall_room(room->second)) reject("superseded owner is not an actual physical room");
        if (model && !std::binary_search(model->baseline_ids().begin(),model->baseline_ids().end(),id))
            reject("only shared baseline rooms can be superseded in a target alternative");
        if (!model && foreign_registered.contains(id)) reject("superseded owner belongs to another phase registry");
        const auto c=organization.drawing_context(id);
        if (!c || !c->complete()) reject("original room has unresolved context");
        (void)validate_retained_physical_wall_room_lineage(room->second,*c);
    }
    auto result=baseline_dependents(entities,registry_id,superseded);
    if (result.size()>maximum_rows) reject("baseline-only acknowledgement budget exceeded");
    for (const auto& dependent:result) {
        identity(dependent.entity_id); digest(dependent.expected_entity_digest); identities(dependent.referenced_ids);
    }
    return result;
}

namespace {
ReplayedPhysicalWallPhaseRoomReview replay_proposed_room_completion(const Entities& source,
    const PhysicalWallRoomPhaseReviewIntent& intent,const RegistryTransition& transition,const PhaseReviewCoverage& coverage,
    bool active_phase_constraints) {
    const PhysicalWallPhaseSelection destination{intent.registry_id,intent.alternative_id};
    // A v2 review may also contain only baseline decisions. Editing authority is
    // independently restricted to the original actual target proposals below.
    std::set<std::string> proposals;
    if (!transition.created && intent.alternative_id) proposals=original_target_proposals(source,destination);
    for (const auto& affected:coverage.affected) {
        std::size_t matches=0;
        for (const auto& row:intent.planes) if (same_plane(affected,{row.context,row.effective_elevation_m})) ++matches;
        if (matches!=1) reject("semantic wall change lacks exact affected context/plane coverage");
    }
    ReplayedPhysicalWallPhaseRoomReview result; result.entities=coverage.destination;
    auto occupied=source_identity_inventory(source);
    const auto reserve=[&](const std::string& id) {
        if (!occupied.insert(id).second) reject("fresh identity is already occupied: "+id);
        result.fresh_identity_ids.push_back(id);
    };
    if (transition.created) reserve(intent.registry_id);
    std::set<std::string> all_reviewed,shared,redefined,retiring,superseded,reviewed_components;
    std::map<std::string,const PhysicalWallRoomPhaseSourceDecision*,std::less<>> changed_decisions;
    std::map<std::string,Plane,std::less<>> redefinition_planes;
    struct Assignment {
        const PhysicalWallRoomPhaseFreshDecision* decision{};
        DrawingContext context;
        double effective_elevation_m{};
        PhysicalWallSpace space;
        IdentifiedBoundary boundary;
        std::string selected_wall_id;
    };
    std::vector<Assignment> assignments;
    for (const auto& plane:intent.planes) {
        const auto expected_rooms=phase_review_plane_rooms(coverage,{plane.context,plane.effective_elevation_m});
        std::map<std::string,const PhysicalWallRoomPhaseSourceDecision*,std::less<>> decisions;
        for (const auto& d:plane.source_rooms) {
            if (!expected_rooms.contains(d.room_id) || !decisions.emplace(d.room_id,&d).second || !all_reviewed.insert(d.room_id).second)
                reject("source room roster is duplicate, foreign or incomplete");
            if (physical_wall_room_descriptor_digest(source.at(d.room_id))!=d.expected_descriptor_digest) reject("original room descriptor changed");
            if (d.disposition==PhysicalWallRoomPhaseSourceDisposition::supersede_in_target) {
                if (!intent.alternative_id || !std::binary_search(transition.destination.baseline_ids().begin(),transition.destination.baseline_ids().end(),d.room_id))
                    reject("only shared baseline rooms can be superseded in a target alternative");
                superseded.insert(d.room_id);
            } else if (d.disposition==PhysicalWallRoomPhaseSourceDisposition::redefine_proposed ||
                d.disposition==PhysicalWallRoomPhaseSourceDisposition::retire_proposed) {
                if (!proposals.contains(d.room_id)) reject("only an actual original proposal of the named target can be edited");
                changed_decisions.emplace(d.room_id,&d);
                if (d.disposition==PhysicalWallRoomPhaseSourceDisposition::redefine_proposed) {
                    redefined.insert(d.room_id);
                    redefinition_planes.emplace(d.room_id,Plane{plane.context,plane.effective_elevation_m});
                    for (const auto& id:d.replacement_dimension_ids) reserve(id);
                } else retiring.insert(d.room_id);
            }
            if (d.disposition!=PhysicalWallRoomPhaseSourceDisposition::retire_proposed) result.preserved_room_ids.push_back(d.room_id);
        }
        if (decisions.size()!=expected_rooms.size()) reject("every original source room needs an explicit disposition");
        const auto report=phase_review_plane_report(source,coverage,destination,{plane.context,plane.effective_elevation_m},expected_rooms);
        admit_phase_review_components(report,reviewed_components);
        if (plane.fresh.size()!=report.correspondence.fresh.size()) reject("fresh candidate coverage is incomplete");
        std::set<std::size_t> candidate_ids;
        std::set<std::string> assigned_redefinitions;
        for (const auto& d:plane.fresh) {
            if (d.candidate_index>=report.correspondence.fresh.size() || !candidate_ids.insert(d.candidate_index).second)
                reject("unknown or repeated fresh candidate");
            const auto& candidate=report.correspondence.fresh.at(d.candidate_index);
            if (candidate.index!=d.candidate_index) reject("fresh correspondence index is inconsistent");
            const PhysicalWallSpace space{candidate.baseline_face_index,candidate.boundary,candidate.holes,candidate.area_square_metres,candidate.source_lineage};
            if (d.reviewed_source_lineage.dump()!=space.source_lineage.dump()) reject("fresh source lineage differs from current explicit detection");
            if (d.disposition==PhysicalWallRoomPhaseFreshDisposition::leave_unclassified) continue;
            if (!candidate.diagnostic.empty()) reject("fresh clear region has unresolved analytical evidence");
            witness(space,d.interior_witness);
            if (d.disposition==PhysicalWallRoomPhaseFreshDisposition::share_unchanged) {
                const auto found=decisions.find(d.room_id);
                if (found==decisions.end() || found->second->disposition!=PhysicalWallRoomPhaseSourceDisposition::share_unchanged)
                    reject("shared owner lacks an unchanged source disposition");
                if (coverage.after_inactive.contains(d.room_id) || !physical_wall_room_lineage_matches_current_inventory(source.at(d.room_id),plane.context,space))
                    reject("shared owner changed inventory, lineage or clear geometry");
                if (!shared.insert(d.room_id).second) reject("shared owner has more than one fresh candidate");
                continue;
            }
            if (d.disposition==PhysicalWallRoomPhaseFreshDisposition::redefine_proposed) {
                const auto found=decisions.find(d.room_id);
                if (found==decisions.end() || found->second->disposition!=PhysicalWallRoomPhaseSourceDisposition::redefine_proposed ||
                    !proposals.contains(d.room_id) || !assigned_redefinitions.insert(d.room_id).second)
                    reject("fresh redefinition needs one matching original target proposal");
            } else reserve(d.room_id);
            if (d.fresh_ids.segment_ids.size()!=space.boundary.size() || d.fresh_ids.vertex_ids.size()!=space.boundary.size())
                reject("new topology requires explicit fresh identities for every boundary edge and vertex");
            IdentifiedBoundary boundary{d.room_id,"room_boundary",{}};
            for (std::size_t i=0;i<space.boundary.size();++i) {
                reserve(d.fresh_ids.segment_ids[i]); reserve(d.fresh_ids.vertex_ids[i]);
                boundary.segments.push_back({d.fresh_ids.segment_ids[i],d.fresh_ids.vertex_ids[i],d.fresh_ids.vertex_ids[(i+1)%space.boundary.size()],space.boundary[i]});
            }
            const auto& physical=space.source_lineage.at("physical_sources");
            if (physical.empty()) reject("fresh room has no actual physical source");
            const auto selected=physical.front().at("owner_id").get<std::string>();
            if (!source.contains(selected) || source.at(selected).type!="wall") reject("descriptor source must resolve to an actual wall");
            assignments.push_back({&d,plane.context,plane.effective_elevation_m,space,std::move(boundary),selected});
            if (d.disposition==PhysicalWallRoomPhaseFreshDisposition::create_proposed) result.created_room_ids.push_back(d.room_id);
        }
        for (const auto& [id,d]:decisions) {
            if (d->disposition==PhysicalWallRoomPhaseSourceDisposition::share_unchanged && !shared.contains(id))
                reject("unchanged original owner has no explicitly shared fresh candidate");
            if (d->disposition==PhysicalWallRoomPhaseSourceDisposition::redefine_proposed && !assigned_redefinitions.contains(id))
                reject("redefined owner has no explicitly assigned fresh candidate");
        }
    }
    std::map<std::string,const PhysicalWallRoomPhaseBaselineAcknowledgement*,std::less<>> acknowledgements;
    for (const auto& d:intent.baseline_only_acknowledgements) acknowledgements.emplace(d.entity_id,&d);
    const auto expected_baseline=baseline_dependents(source,intent.registry_id,superseded);
    for (const auto& d:expected_baseline) {
        const auto found=acknowledgements.find(d.entity_id);
        if (found==acknowledgements.end() || found->second->expected_entity_digest!=d.expected_entity_digest || found->second->referenced_ids!=d.referenced_ids)
            reject("saved dependent needs an exact baseline-only acknowledgement: "+d.entity_id);
    }
    if (acknowledgements.size()!=expected_baseline.size()) reject("acknowledgement set contains unrelated or fabricated dependents");

    std::set<std::string> changed=redefined; changed.insert(retiring.begin(),retiring.end());
    const auto evidence=proposed_dependents(source,changed);
    const std::set<std::string> expected_references(evidence.reference_ids.begin(),evidence.reference_ids.end());
    const std::set<std::string> removed(intent.removed_reference_ids.begin(),intent.removed_reference_ids.end());
    const std::set<std::string> kept(intent.kept_reference_ids.begin(),intent.kept_reference_ids.end());
    auto reviewed_references=removed; reviewed_references.insert(kept.begin(),kept.end());
    if (reviewed_references!=expected_references) reject("every changed proposed-room reference needs an explicit Keep or Remove decision");
    // Child retirement and regenerated automatic dimensions also participate in
    // incoming opaque-reference refusal and known membership cleanup.
    auto affected=superseded_room_tokens(source,changed);
    for (const auto& id:redefined) affected.erase(id);
    affected.insert(removed.begin(),removed.end());
    std::set<std::string> replaced_dimensions;
    for (const auto& id:kept) {
        const auto& e=source.at(id);
        if (!can_recognize_boundary_dimension_entity_type(e.type)) continue;
        const auto decoded=decode_boundary_dimension_entity(e);
        if (decoded.supported() && redefined.contains(decoded.dimension->boundary_id) &&
            decoded.dimension->kind==BoundaryDimensionKind::segment_length && decoded.dimension->placement==BoundaryDimensionPlacement::automatic)
            replaced_dimensions.insert(id);
    }
    affected.insert(replaced_dimensions.begin(),replaced_dimensions.end());
    std::set<std::string> allowed{intent.registry_id};
    allowed.insert(changed.begin(),changed.end()); allowed.insert(expected_references.begin(),expected_references.end());
    for (const auto& id:removed) result.entities.erase(id);
    prune_relationships(result.entities,intent,retiring,allowed);
    for (const auto& id:retiring) result.entities.erase(id);

    // Reconstruct canonical membership independently, then change only the raw
    // named target lists. Baseline and all other alternative bytes/order survive.
    auto entity_ids=transition.destination.entity_ids(); auto alternatives=transition.destination.alternatives();
    std::erase_if(entity_ids,[&](const auto& id){return retiring.contains(id);});
    if (intent.alternative_id) {
        auto target=std::find_if(alternatives.begin(),alternatives.end(),[&](const auto& row){return row.id==*intent.alternative_id;});
        if (target==alternatives.end()) reject("target alternative is missing");
        std::erase_if(target->proposed_ids,[&](const auto& id){return retiring.contains(id);});
        target->demolished_ids.insert(target->demolished_ids.end(),superseded.begin(),superseded.end());
        std::sort(target->demolished_ids.begin(),target->demolished_ids.end());
        target->demolished_ids.erase(std::unique(target->demolished_ids.begin(),target->demolished_ids.end()),target->demolished_ids.end());
        target->proposed_ids.insert(target->proposed_ids.end(),result.created_room_ids.begin(),result.created_room_ids.end());
        entity_ids.insert(entity_ids.end(),result.created_room_ids.begin(),result.created_room_ids.end());
    } else if (!result.created_room_ids.empty() || !superseded.empty() || !changed.empty()) reject("baseline selection cannot redefine room ownership");
    const auto final_model=ModelPhases::create(std::move(entity_ids),transition.destination.baseline_ids(),std::move(alternatives),transition.destination.active_alternative());
    auto final_json=transition.registry.properties.at("model"); filter_members(final_json.at("entity_ids"),retiring);
    for (const auto& id:result.created_room_ids) final_json.at("entity_ids").push_back(id);
    if (intent.alternative_id) for (auto& alternative:final_json.at("alternatives")) if (alternative.at("id")==*intent.alternative_id) {
        filter_members(alternative.at("proposed_ids"),retiring);
        for (const auto& id:superseded)
            if (std::find(alternative.at("demolished_ids").begin(),alternative.at("demolished_ids").end(),Json(id))==alternative.at("demolished_ids").end())
                alternative.at("demolished_ids").push_back(id);
        for (const auto& id:result.created_room_ids) alternative.at("proposed_ids").push_back(id);
    }
    if (ModelPhases::from_json(final_json).to_json()!=final_model.to_json()) reject("rederived target registry membership is inconsistent");
    result.entities.at(intent.registry_id).properties["model"]=std::move(final_json);
    for (const auto& assignment:assignments) if (assignment.decision->disposition==PhysicalWallRoomPhaseFreshDisposition::create_proposed) {
        const auto& d=*assignment.decision; const auto& c=assignment.context;
        auto room=encode_identified_boundary_entity(assignment.boundary);
        room.properties.update({{"property_id",c.property_id},{"building_id",c.building_id},{"floor_id",c.floor_id},{"layer_id",c.layer_id},
            {"name",d.name},{"classification",d.classification},{"measurement_classification",d.classification},{"factor",*d.factor}});
        room.extensions["physical_wall_room"]=encode_physical_wall_room_descriptor({assignment.selected_wall_id,assignment.space.source_lineage,assignment.space.holes});
        (void)validate_retained_physical_wall_room_lineage(room,c);
        if (!result.entities.emplace(d.room_id,std::move(room)).second) reject("fresh proposed owner collides with an existing entity");
    }
    (void)physical_wall_phase_states(result.entities,destination);
    auto removed_members=retiring; removed_members.insert(removed.begin(),removed.end());
    removed_members.insert(replaced_dimensions.begin(),replaced_dimensions.end());
    apply_reviewed_presentation_removals(source,result.entities,intent,removed_members,allowed);
    // Reviewing known membership pruning cannot conceal opaque incoming
    // references inside a removed row; inspect those original portions too.
    refuse_opaque_removed_presentation_rows(source,intent.presentation_removals,affected);
    refuse_opaque_proposed_dependents(source,result.entities,intent.registry_id,changed,retiring,affected,kept);
    for (const auto& assignment:assignments) if (assignment.decision->disposition==PhysicalWallRoomPhaseFreshDisposition::redefine_proposed) {
        const auto& d=*assignment.decision; const auto& original=*changed_decisions.at(d.room_id);
        BoundaryGeometryEdit edit; edit.boundary_id=d.room_id; edit.target_id=d.room_id;
        edit.kind=BoundaryGeometryEditKind::redefine_boundary; edit.fresh_topology=true;
        edit.replacement_segments=encode_identified_boundary_entity(assignment.boundary).properties.at("segments");
        edit.replacement_child_mapping=original.child_mapping; edit.replacement_dimension_ids=original.replacement_dimension_ids;
        edit.physical_wall_room_repair=PhysicalWallRoomRepairIntent{assignment.selected_wall_id,d.interior_witness,d.reviewed_source_lineage,original.expected_descriptor_digest};
        // Empty replacement facts preserve every authored owner property. The
        // lower seam owns descriptor/provenance and explicit reference remap.
        std::set<std::string> plane_owners;
        for (const auto& [id,plane]:redefinition_planes)
            if (same_plane(plane,{assignment.context,assignment.effective_elevation_m})) plane_owners.insert(id);
        result.entities=edited_boundary_entities_for_phase_room_review(result.entities,edit,plane_owners,destination);
    }
    if (const auto error=validate_boundary_integrity(result.entities)) reject(*error);
    if (const auto error=active_phase_constraints ? validate_active_phase_constraint_integrity(result.entities) :
        validate_constraint_integrity(result.entities)) reject(*error);
    (void)physical_wall_phase_states(result.entities,destination);
    // Numeric resolution alone evaluates the named target on a detached map.
    // This saved-choice adjustment never becomes source or published authority.
    auto numerical=result.entities;
    numerical.at(intent.registry_id).properties.at("model")["active_alternative"]=intent.alternative_id ? Json(*intent.alternative_id) : Json(nullptr);
    for (const auto& id:kept) {
        const auto found=result.entities.find(id);
        if (found==result.entities.end()) {
            if (!replaced_dimensions.contains(id)) reject("kept reference disappeared from the reviewed candidate");
            const auto decoded=decode_boundary_dimension_entity(source.at(id));
            if (changed_decisions.at(decoded.dimension->boundary_id)->replacement_dimension_ids.empty()) reject("kept automatic dimensions lack reviewed replacements");
        } else if (can_recognize_boundary_dimension_entity_type(found->second.type)) {
            const auto decoded=decode_boundary_dimension_entity(found->second);
            if (!decoded.supported()) reject("kept reference became unsupported");
            (void)resolve_boundary_dimension(*decoded.dimension,numerical);
        }
    }
    for (const auto& [id,d]:changed_decisions) for (const auto& dimension_id:d->replacement_dimension_ids) {
        (void)id;
        const auto found=result.entities.find(dimension_id);
        if (found==result.entities.end() || !can_recognize_boundary_dimension_entity_type(found->second.type)) reject("reviewed replacement dimension is missing");
        const auto decoded=decode_boundary_dimension_entity(found->second);
        if (!decoded.supported()) reject("reviewed replacement dimension is unsupported");
        (void)resolve_boundary_dimension(*decoded.dimension,numerical);
    }
    for (const auto& id:removed) if (result.entities.contains(id)) reject("removed reference survived lower reconstruction");
    for (const auto& id:retiring) if (result.entities.contains(id)) reject("retired proposal survived lower reconstruction");
    for (const auto& d:intent.presentation_removals) {
        const auto found=result.entities.find(d.entity_id);
        if (found==result.entities.end() || entity_digest(found->second)!=d.expected_replacement_entity_digest)
            reject("reviewed saved presentation replacement changed during reconstruction: "+d.entity_id);
    }
    validate_baseline_dependent_preservation(source,result.entities,intent.registry_id,expected_baseline,intent.presentation_removals);
    for (const auto& id:redefined) {
        auto before=source.at(id),after=result.entities.at(id);
        // These are the only geometry/provenance fields the qualified lower
        // repair may replace. All authored facts/context and opaque owner fields
        // retain their original complete values, independently of that helper.
        for (const auto* field:{"segments","boundary","boundary_authoring"}) {
            before.properties.erase(field); after.properties.erase(field);
        }
        for (const auto* field:{"physical_wall_room","boundary_geometry_derivation"}) {
            before.extensions.erase(field); after.extensions.erase(field);
        }
        if (!exact_entity(before,after)) reject("redefinition changed original authored facts or opaque owner fields");
    }
    // Only actual changed owners/references and qualified membership rows may
    // differ. Every baseline owner and unrelated original entity remains exact.
    for (const auto& [id,e]:source) if (!allowed.contains(id)) {
        const auto found=result.entities.find(id);
        if (found==result.entities.end() || !exact_entity(e,found->second)) reject("unrelated original entity changed: "+id);
    }
    const std::set<std::string> fresh(result.fresh_identity_ids.begin(),result.fresh_identity_ids.end());
    for (const auto& [id,e]:result.entities) { (void)e; if (!source.contains(id) && !fresh.contains(id)) reject("unreviewed fresh entity appeared"); }
    result.redefined_room_ids.assign(redefined.begin(),redefined.end()); result.retired_room_ids.assign(retiring.begin(),retiring.end());
    std::sort(result.created_room_ids.begin(),result.created_room_ids.end());
    std::sort(result.preserved_room_ids.begin(),result.preserved_room_ids.end());
    std::sort(result.fresh_identity_ids.begin(),result.fresh_identity_ids.end());
    return result;
}
} // namespace

ReplayedPhysicalWallPhaseRoomReview replay_physical_wall_phase_room_review(const Entities& source,const Json& encoded,
    bool active_phase_constraints) {
    const auto intent=decode_physical_wall_phase_room_review_intent(encoded);
    if (entity_map_digest(source)!=intent.source_entities_digest) reject("original entity map changed");
    auto transition=registry_transition(source,intent);
    const auto coverage=phase_review_coverage(source,intent,transition);
    if (intent.proposed_room_completion) return replay_proposed_room_completion(source,intent,transition,coverage,active_phase_constraints);
    const PhysicalWallPhaseSelection destination_selection{intent.registry_id,intent.alternative_id};
    // Extra rows are allowed for explicit review of an unchanged plane, but
    // every semantic inventory/retained-lineage affected plane is mandatory.
    for (const auto& plane:coverage.affected) {
        std::size_t matches=0;
        for (const auto& row:intent.planes) if (same_plane(plane,{row.context,row.effective_elevation_m})) ++matches;
        if (matches!=1) reject("semantic wall change lacks exact affected context/plane coverage");
    }
    auto occupied=source_identity_inventory(source);
    std::set<std::string> superseded,shared,reviewed_components;
    ReplayedPhysicalWallPhaseRoomReview result; result.entities=coverage.destination;
    const auto reserve=[&](const std::string& id) {
        if (!occupied.insert(id).second) reject("fresh identity is already occupied: "+id);
        result.fresh_identity_ids.push_back(id);
    };
    if (transition.created) reserve(intent.registry_id);
    std::set<std::string> all_reviewed;
    for (const auto& plane:intent.planes) {
        const auto expected_rooms=phase_review_plane_rooms(coverage,{plane.context,plane.effective_elevation_m});
        std::map<std::string,const PhysicalWallRoomPhaseSourceDecision*,std::less<>> decisions;
        for (const auto& d:plane.source_rooms) {
            if (!expected_rooms.contains(d.room_id) || !decisions.emplace(d.room_id,&d).second || !all_reviewed.insert(d.room_id).second)
                reject("source room roster is duplicate, foreign or incomplete");
            if (physical_wall_room_descriptor_digest(source.at(d.room_id))!=d.expected_descriptor_digest) reject("original room descriptor changed");
            result.preserved_room_ids.push_back(d.room_id);
            if (d.disposition==PhysicalWallRoomPhaseSourceDisposition::supersede_in_target) {
                if (!intent.alternative_id || !std::binary_search(transition.destination.baseline_ids().begin(),transition.destination.baseline_ids().end(),d.room_id))
                    reject("only shared baseline rooms can be superseded in a target alternative");
                superseded.insert(d.room_id);
            }
        }
        if (decisions.size()!=expected_rooms.size()) reject("every original source room needs an explicit disposition");
        const auto report=phase_review_plane_report(source,coverage,destination_selection,
            {plane.context,plane.effective_elevation_m},expected_rooms);
        admit_phase_review_components(report,reviewed_components);
        if (report.correspondence.fresh.size()>maximum_rows || plane.fresh.size()!=report.correspondence.fresh.size()) reject("fresh candidate coverage is incomplete");
        std::set<std::size_t> candidate_ids;
        for (const auto& d:plane.fresh) {
            if (d.candidate_index>=report.correspondence.fresh.size() || !candidate_ids.insert(d.candidate_index).second) reject("unknown or repeated fresh candidate");
            const auto& candidate=report.correspondence.fresh.at(d.candidate_index);
            if (candidate.index!=d.candidate_index) reject("fresh correspondence index is inconsistent");
            const PhysicalWallSpace space{candidate.baseline_face_index,candidate.boundary,candidate.holes,candidate.area_square_metres,candidate.source_lineage};
            if (d.reviewed_source_lineage.dump()!=space.source_lineage.dump()) reject("fresh source lineage differs from current explicit detection");
            if (d.disposition==PhysicalWallRoomPhaseFreshDisposition::leave_unclassified) continue;
            if (!candidate.diagnostic.empty()) reject("fresh clear region has unresolved analytical evidence");
            witness(space,d.interior_witness);
            if (d.disposition==PhysicalWallRoomPhaseFreshDisposition::share_unchanged) {
                const auto found=decisions.find(d.room_id);
                if (found==decisions.end() || found->second->disposition!=PhysicalWallRoomPhaseSourceDisposition::share_unchanged)
                    reject("shared owner lacks an unchanged source disposition");
                if (coverage.after_inactive.contains(d.room_id) || !physical_wall_room_lineage_matches_current_inventory(source.at(d.room_id),plane.context,space))
                    reject("shared owner changed inventory, lineage or clear geometry");
                if (!shared.insert(d.room_id).second) reject("shared owner has more than one fresh candidate");
                continue;
            }
            reserve(d.room_id);
            if (d.fresh_ids.segment_ids.size()!=space.boundary.size() || d.fresh_ids.vertex_ids.size()!=space.boundary.size())
                reject("creation requires explicit fresh identities for every boundary edge and vertex");
            IdentifiedBoundary boundary{d.room_id,"room_boundary",{}};
            for (std::size_t i=0;i<space.boundary.size();++i) {
                reserve(d.fresh_ids.segment_ids[i]); reserve(d.fresh_ids.vertex_ids[i]);
                boundary.segments.push_back({d.fresh_ids.segment_ids[i],d.fresh_ids.vertex_ids[i],
                    d.fresh_ids.vertex_ids[(i+1)%space.boundary.size()],space.boundary[i]});
            }
            auto room=encode_identified_boundary_entity(boundary);
            room.properties.update({{"property_id",plane.context.property_id},{"building_id",plane.context.building_id},
                {"floor_id",plane.context.floor_id},{"layer_id",plane.context.layer_id},{"name",d.name},
                {"classification",d.classification},{"measurement_classification",d.classification},{"factor",*d.factor}});
            const auto& sources=space.source_lineage.at("physical_sources");
            if (sources.empty()) reject("fresh room has no actual physical source");
            const auto selected=sources.front().at("owner_id").get<std::string>();
            if (!source.contains(selected) || source.at(selected).type!="wall") reject("descriptor source must resolve to an actual wall");
            room.extensions["physical_wall_room"]=encode_physical_wall_room_descriptor({selected,space.source_lineage,space.holes});
            (void)validate_retained_physical_wall_room_lineage(room,plane.context);
            result.entities.emplace(d.room_id,std::move(room)); result.created_room_ids.push_back(d.room_id);
        }
        for (const auto& [id,d]:decisions)
            if (d->disposition==PhysicalWallRoomPhaseSourceDisposition::share_unchanged && !shared.contains(id))
                reject("unchanged original owner has no explicitly shared fresh candidate");
    }
    std::map<std::string,const PhysicalWallRoomPhaseBaselineAcknowledgement*,std::less<>> acknowledgements;
    for (const auto& d:intent.baseline_only_acknowledgements) acknowledgements.emplace(d.entity_id,&d);
    const auto expected_dependents=baseline_dependents(source,intent.registry_id,superseded);
    for (const auto& dependent:expected_dependents) {
        const auto found=acknowledgements.find(dependent.entity_id);
        if (found==acknowledgements.end() || found->second->expected_entity_digest!=dependent.expected_entity_digest ||
            found->second->referenced_ids!=dependent.referenced_ids)
            reject("saved dependent needs an exact baseline-only acknowledgement: "+dependent.entity_id);
    }
    if (expected_dependents.size()!=acknowledgements.size()) reject("acknowledgement set contains unrelated or fabricated dependents");
    auto entity_ids=transition.destination.entity_ids(); auto alternatives=transition.destination.alternatives();
    if (intent.alternative_id) {
        auto target=std::find_if(alternatives.begin(),alternatives.end(),[&](const auto& row){return row.id==*intent.alternative_id;});
        if (target==alternatives.end()) reject("target alternative is missing");
        target->demolished_ids.insert(target->demolished_ids.end(),superseded.begin(),superseded.end());
        std::sort(target->demolished_ids.begin(),target->demolished_ids.end());
        target->demolished_ids.erase(std::unique(target->demolished_ids.begin(),target->demolished_ids.end()),target->demolished_ids.end());
        target->proposed_ids.insert(target->proposed_ids.end(),result.created_room_ids.begin(),result.created_room_ids.end());
        entity_ids.insert(entity_ids.end(),result.created_room_ids.begin(),result.created_room_ids.end());
    } else if (!result.created_room_ids.empty() || !superseded.empty()) reject("baseline selection cannot redefine room ownership");
    const auto final_model=ModelPhases::create(std::move(entity_ids),transition.destination.baseline_ids(),std::move(alternatives),transition.destination.active_alternative());
    auto final_json=transition.registry.properties.at("model");
    for (const auto& id:result.created_room_ids) final_json["entity_ids"].push_back(id);
    if (intent.alternative_id) for (auto& alternative:final_json.at("alternatives")) if (alternative.at("id")==*intent.alternative_id) {
        for (const auto& id:superseded)
            if (std::find(alternative.at("demolished_ids").begin(),alternative.at("demolished_ids").end(),Json(id))==alternative.at("demolished_ids").end())
                alternative["demolished_ids"].push_back(id);
        for (const auto& id:result.created_room_ids) alternative["proposed_ids"].push_back(id);
    }
    if (ModelPhases::from_json(final_json).to_json()!=final_model.to_json()) reject("rederived registry membership is inconsistent");
    result.entities.at(intent.registry_id).properties["model"]=std::move(final_json);
    (void)physical_wall_phase_states(result.entities,destination_selection);
    if (const auto error=validate_boundary_integrity(result.entities)) reject(*error);
    for (const auto& [id,e]:source) if (id!=intent.registry_id && !exact_entity(e,result.entities.at(id))) reject("original entity changed");
    std::sort(result.created_room_ids.begin(),result.created_room_ids.end());
    std::sort(result.preserved_room_ids.begin(),result.preserved_room_ids.end());
    std::sort(result.fresh_identity_ids.begin(),result.fresh_identity_ids.end());
    return result;
}

PreparedPhysicalWallPhaseRoomReview prepare_physical_wall_phase_room_review(
    const DocumentSnapshot& source,const PhysicalWallRoomPhaseReviewIntent& intent) {
    if (!source.is_editable()) reject("captured document is read-only");
    if (intent.expected_revision!=source.revision() || intent.source_snapshot_digest!=document_snapshot_digest(source) ||
        intent.source_authoring_digest!=document_authoring_source_digest_v2(source) ||
        intent.source_saved_revision!=source.saved_revision_optional() || intent.source_entities_digest!=entity_map_digest(source.entities()))
        reject("complete original captured source changed");
    auto encoded=encode_physical_wall_phase_room_review_intent(intent);
    auto replayed=replay_physical_wall_phase_room_review(source.entities(),encoded,source.uses_active_phase_constraints());
    return {std::move(replayed),std::move(encoded)};
}
} // namespace sketch
