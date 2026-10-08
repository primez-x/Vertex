#include "sketch/physical_wall_phase_review.hpp"
#include "sketch/boundary_integrity.hpp"
#include "sketch/document_digest.hpp"
#include "sketch/document_wall.hpp"
#include "sketch/physical_wall_spaces.hpp"

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
} // namespace

PhysicalWallRoomPhaseReviewIntent decode_physical_wall_phase_room_review_intent(const Json& value) {
    try {
        if (value.dump().size()>1024*1024) reject("intent exceeds one MiB");
        keys(value,{"version","source_snapshot_digest","source_authoring_digest","source_saved_revision","source_entities_digest",
            "expected_revision","registry_id","alternative_id","source_registry_entity_digest","registry_command_proof","planes","baseline_only_acknowledgements"});
        if (!value.at("version").is_number_integer() || value.at("version")!=1) reject("unsupported intent version");
        PhysicalWallRoomPhaseReviewIntent result;
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
                keys(old,{"room_id","expected_descriptor_digest","disposition"});
                PhysicalWallRoomPhaseSourceDecision d; d.room_id=old.at("room_id").get<std::string>(); identity(d.room_id);
                if (!source_ids.insert(d.room_id).second) reject("duplicate original room decision");
                d.expected_descriptor_digest=old.at("expected_descriptor_digest").get<std::string>(); digest(d.expected_descriptor_digest);
                const auto action=old.at("disposition").get<std::string>();
                if (action=="supersede_in_target") d.disposition=PhysicalWallRoomPhaseSourceDisposition::supersede_in_target;
                else if (action!="share_unchanged") reject("unsupported source disposition");
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
            else reject("unsupported source disposition");
            source.push_back({{"room_id",d.room_id},{"expected_descriptor_digest",d.expected_descriptor_digest},{"disposition",action}});
        }
        for (const auto& d:plane.fresh) {
            std::string action;
            if (d.disposition==PhysicalWallRoomPhaseFreshDisposition::share_unchanged) action="share_unchanged";
            else if (d.disposition==PhysicalWallRoomPhaseFreshDisposition::create_proposed) action="create_proposed";
            else if (d.disposition==PhysicalWallRoomPhaseFreshDisposition::leave_unclassified) action="leave_unclassified";
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
    Json encoded{{"version",1},{"source_snapshot_digest",intent.source_snapshot_digest},{"source_authoring_digest",intent.source_authoring_digest},
        {"source_saved_revision",intent.source_saved_revision ? Json(*intent.source_saved_revision) : Json(nullptr)},
        {"source_entities_digest",intent.source_entities_digest},{"expected_revision",intent.expected_revision},{"registry_id",intent.registry_id},
        {"alternative_id",intent.alternative_id ? Json(*intent.alternative_id) : Json(nullptr)},
        {"source_registry_entity_digest",intent.source_registry_entity_digest ? Json(*intent.source_registry_entity_digest) : Json(nullptr)},
        {"registry_command_proof",intent.registry_command_proof},{"planes",std::move(planes)},{"baseline_only_acknowledgements",std::move(references)}};
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

ReplayedPhysicalWallPhaseRoomReview replay_physical_wall_phase_room_review(const Entities& source,const Json& encoded) {
    const auto intent=decode_physical_wall_phase_room_review_intent(encoded);
    if (entity_map_digest(source)!=intent.source_entities_digest) reject("original entity map changed");
    auto transition=registry_transition(source,intent);
    const auto coverage=phase_review_coverage(source,intent,transition);
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
    auto replayed=replay_physical_wall_phase_room_review(source.entities(),encoded);
    return {std::move(replayed),std::move(encoded)};
}
} // namespace sketch
