#include "sketch/mixed_selection_property_edit.hpp"
#include "sketch/phase_constraint_authoring.hpp"
#include "sketch/assembly_directional_resize.hpp"
#include "sketch/assembly_geometry.hpp"
#include "sketch/assembly_dimension.hpp"
#include "sketch/assembly_phase_membership.hpp"
#include "sketch/boundary_dimension.hpp"
#include "sketch/building_entity.hpp"
#include "sketch/document_digest.hpp"
#include "sketch/document_solid.hpp"
#include "sketch/model_phases.hpp"
#include "sketch/project_exchange.hpp"
#include "sketch/project_store.hpp"
#include "sketch/roof_entity_codec.hpp"
#include "sketch/roof_join_semantics.hpp"
#include "sketch/slab_hosted_geometry_edit.hpp"
#include "sketch/slab_plan_geometry.hpp"
#include "sketch/structural_hosted_components.hpp"
#include "sketch/architecture.hpp"
#include "support/detached_document_snapshot.hpp"
#include "support/noninteractive_errors.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <numbers>
#include <stdexcept>
#include <sqlite3.h>
#include <BRepBndLib.hxx>
#include <Bnd_Box.hxx>

namespace {
using namespace sketch;
using Json=nlohmann::json;
using Entities=std::map<std::string,Entity,std::less<>>;
void require(bool value,const char* message) {if (!value) throw std::runtime_error(message);}
void require_near(double actual,double expected,const char* message) {
    if (!std::isfinite(actual) || std::abs(actual-expected)>1e-8) throw std::runtime_error(message);
}
template<class F> void refuses(F&& action,const char* message) {
    try {action();} catch (const std::invalid_argument&) {return;} throw std::runtime_error(message);
}
template<class F> void budget_refuses(F&& action,std::string_view diagnostic) {
    try {action();} catch (const std::invalid_argument& error) {
        if (std::string_view(error.what()).find(diagnostic)==std::string_view::npos)
            throw std::runtime_error(std::string("expected ")+std::string(diagnostic)+", observed "+error.what());
        return;
    }
    throw std::runtime_error("combined analytical admission failed to refuse");
}
bool exact(const Entity& a,const Entity& b) {
    return a==b && a.properties.dump()==b.properties.dump() && a.extensions.dump()==b.extensions.dump();
}
const Json context={{"property_id","property"},{"building_id","building"},{"floor_id","floor"},{"layer_id","layer"}};
Boundary rectangle(double x=0,double y=0,double w=2,double d=1) {
    return {{{x,y},{x+w,y},0},{{x+w,y},{x+w,y+d},0},{{x+w,y+d},{x,y+d},0},{{x,y+d},{x,y},0}};
}
Json raw_ring(const Boundary& ring) {
    Json result=Json::array();for (const auto& e:ring) result.push_back({{"start",{e.start.x,e.start.y}},
        {"end",{e.end.x,e.end.y}},{"sweep_radians",e.sweep_radians}});return result;
}
AssemblyProfileTopology topology(const std::string& prefix) {
    return {{prefix+"outer",{prefix+"e0",prefix+"e1",prefix+"e2",prefix+"e3"},
        {prefix+"v0",prefix+"v1",prefix+"v2",prefix+"v3"}},{}};
}
AssemblyProfileDimensionTarget selector(const std::string& prefix) {
    return {"profile",prefix+"outer",AssemblyProfileDimensionMeasurement::edge_length,prefix+"e0",std::nullopt,std::nullopt};
}
Json receipt(int value) {
    return {{"version",1},{"original_expression",std::to_string(value)+" m"},{"entered_unit","m"},
        {"exact_metres",{{"numerator",value},{"denominator",1},{"opaque","rational"}}},{"opaque",Json::array({17,true,"raw"})}};
}
Entity dimension(std::string id,std::string catalog,std::string instance,std::optional<std::string> owner,bool edge=false) {
    BoundaryDimension d;d.id=std::move(id);d.boundary_id=owner.value_or(catalog);
    d.kind=edge ? BoundaryDimensionKind::assembly_profile : BoundaryDimensionKind::assembly_extent;d.text_position={7,-2};
    AssemblyDimensionTarget target;target.component={catalog,instance,owner,{}};target.axis=AssemblyDimensionAxis::width;
    if (edge) target.profile=selector("");d.assembly_target=target;return encode_boundary_dimension_entity(d);
}
Document fixture(bool baseline=false,int version=7,bool curved=false) {
    AssemblyProfile p;p.id="profile";p.outer=rectangle();p.height_m=1.5;
    if (version==7) p.topology=topology("");
    AssemblyType body;body.id="body";body.name="Shared physical source";body.profiles={p};
    AssemblyInstance slab;slab.id="slab-root";slab.type_id="body";slab.placement=AssemblyPlacement{"slab",{.6,-.35},.23,1,true,.2,1};
    AssemblyInstance column=slab;column.id="column-root";column.placement->host_entity_id="column";
    AssemblyInstance child=slab;child.id="child-root";child.placement->host_entity_id="host";
    AssemblyInstance independent;independent.id="component";independent.type_id="body";
    independent.root_transform=AssemblyTransform{{5,1,0},0,1};
    auto roof=encode_building_entity(SlopedRoofPanel{.id="roof",.base_position={1,0,4},.run=2,.span=2,.rise=0,
        .pitch_radians=0,.overhang=0,.thickness=.25});
    auto host=encode_building_entity(SlopedRoofPanel{"host",{-5,-5,3},0,20,20,0,0,0,.2,
        {{"child",7,6,2,1,RoofSkylight{.06,.15,.024},0},{"sibling",15,15,1,1,RoofSkylight{.06,.15,.024},0}}});
    host.properties["version"]=4;
    auto structural=encode_building_entity(RectangularColumn{"column",{4,3.5,.5},2,1,2,0});
    for (auto* entity:{&roof,&host,&structural}) {
        for (const auto& [key,value]:context.items()) entity->properties[key]=value;
        entity->extensions={{"vendor",{{"retain","raw physical envelope"}}}};
    }
    roof.properties["quantity_entries"]={{"/run_m",receipt(2)},{"/base_position_m/2",receipt(4)}};
    structural.properties["quantity_entries"]={{"/width_m",receipt(2)},{"/height_m",receipt(2)}};
    auto boundary=rectangle(1,0,3,3);if (curved) boundary[2].sweep_radians=std::numbers::pi;
    auto sp=context;sp["boundary"]=raw_ring(boundary);sp["holes"]=Json::array();sp["thickness_m"]=.2;sp["elevation_m"]=1.25;
    sp["layers"]=Json::array({{{"id","slab-core"},{"thickness_m",.2}}});
    sp["quantity_entries"]={{"/boundary/0/start/0",receipt(1)},{"/elevation_m",{{"version",88},{"opaque","retain"}}}};
    std::vector<Entity> rows{{"property","property",{{"name","Roof physical resize"}},false},
        {"building","building",{{"property_id","property"}},false},{"floor","floor",{{"building_id","building"}},false},
        {"layer","layer",{{"floor_id","floor"}},false},
        {"catalog","assembly_model",{{"model",AssemblyModel::create({}, {body},{slab,column}).to_json()},{"opaque","shared catalog metadata"}},false},
        {"child-catalog","assembly_model",{{"model",AssemblyModel::create({}, {body},{child}).to_json()}},false},
        encode_document_assembly_instance({"component","assembly_instance",context},{"catalog",independent}),
        roof,host,structural,{"slab","slab",sp,false},
        dimension("component-dim","catalog","component",std::string{"component"},version==7),
        dimension("slab-dim","catalog","slab-root",std::nullopt),
        dimension("column-dim","catalog","column-root",std::nullopt),
        dimension("child-dim","child-catalog","child-root",std::nullopt)};
    if (baseline) {
        const std::vector<std::string> ids{"component","component-dim","slab","slab-dim","column","column-dim","catalog"};
        rows.push_back({"phases","model_phases",{{"model",ModelPhases::create(ids,ids,{{"alternative","Choice",{}, {}}},"alternative").to_json()}},false});
        const std::vector<AssemblyPhaseRoot> roots{{"catalog","slab-root"},{"catalog","column-root"}};
        rows.push_back({"membership","assembly_phase_membership",{{"model",AssemblyPhaseMembership::create("phases",roots,roots,{{"alternative",{}, {}}}).to_json()},
            {"opaque","retained membership"}},true});
    }
    return Document::create(rows);
}
DocumentSnapshot replaced(const DocumentSnapshot& source,const Entities& entities) {
    return sketch::test::DetachedDocumentSnapshotFixture::mutate(source,[&](auto& state) {state.entities()=entities;});
}
PhaseAssemblyInstancePropertyEditAuthoring authoring(const DocumentSnapshot& source,int version=7,bool retire=false) {
    auto p=AssemblyModel::from_json(source.entities().at("catalog").properties.at("model")).types().front().profiles.front();
    p.planar_affine=AssemblyProfilePlanarAffine{{0,0},{0,0},1.5,0,0,1};p.topology=topology("new-");
    AssemblyInstancePropertyEditIntent intent;intent.target={"catalog","component",std::string{"component"},{}};
    intent.profile_geometry.operation=AssemblyInstancePropertyPatchOperation::set;intent.profile_geometry.value=std::vector<AssemblyProfile>{p};
    intent.review_profile_topology=version==7;intent.initialize_deformed_profile_topology=version==8;intent.review_initial_profile_geometry=version==9;
    PhaseAssemblyInstancePropertyEditAuthoring result;result.intents={intent};result.carry_presentation_references=true;
    for (const auto& [id,target]:inspect_phase_assembly_instance_profile_dimension_review_roster(source.entities(),result.intents)) {
        (void)target;result.profile_dimension_decisions.emplace(id,retire ? std::optional<AssemblyProfileDimensionTarget>{} : selector("new-"));
    }
    const auto plan=inspect_phase_assembly_instance_property_edit_plan(source.entities(),result.intents);
    for (const auto& root:plan.replacement_roots) result.allocation.roots.emplace(root.root,
        PhaseAssemblyInstanceReplacementIdentity{std::string{"proposed-component"},std::nullopt,"proposed-component"});
    for (const auto& id:plan.required_membership_registry_ids) result.allocation.membership_entity_ids.emplace(id,"proposed-membership");
    for (const auto& [id,target]:inspect_phase_assembly_instance_property_dimension_clone_roster(source.entities(),result.intents,result.profile_dimension_decisions)) {
        (void)target;result.dimension_identities.emplace(id,"component-copy-"+id);
    }
    return result;
}
std::vector<RoofPlanResizeIntent> roofs(bool unit=false) {return {{"roof",unit ? 1. : 1.5,1,{1,0},0,PlanAxisResizeAnchorMode::shared_frame}};}
std::vector<StructuralDirectionalResizeRequest> structures(bool unit=false) {return {{"column",unit ? 1. : 1.5,1,{1,0},0}};}
std::vector<RoofOpeningDirectionalResizeRequest> children(bool unit=false) {return {{{"host","child"},unit ? 1. : 1.5,1,{1,0},0}};}
std::vector<AssemblyDirectionalResizeTarget> targets(bool unit=false) {return {{"component",{{1,0},unit ? 1. : 1.5,1,0,std::nullopt}}};}
std::vector<SlabGeometryEditIntent> slabs(const DocumentSnapshot& source,bool unit=false) {
    SlabGeometryEditIntent edit;edit.slab_id="slab";edit.kind=SlabGeometryEditKind::resize_plan;
    edit.resize=SlabPlanAxisResize{unit ? 1. : 1.5,1,{1,0},0};edit.coordinate_world_hosted_geometry=true;
    edit.preserve_planar_source_geometry=slab_geometry_edit_requires_planar_source(source.entities().at("slab"),edit);return {edit};
}
struct Cohort {
    std::vector<RoofPlanResizeIntent> roof;
    std::vector<SlabGeometryEditIntent> slab;
    std::optional<ComponentSlabDirectionalResizeAllocation> slab_allocation;
    std::vector<StructuralDirectionalResizeRequest> structural;
    std::optional<ComponentStructuralDirectionalResizeAllocation> structural_allocation;
    std::vector<RoofOpeningDirectionalResizeRequest> child;
};
Cohort cohort(const DocumentSnapshot& source,int mask,bool unit=false) {
    Cohort c;c.roof=roofs(unit);if (mask&1) c.slab=slabs(source,unit);if (mask&2) c.structural=structures(unit);if (mask&4) c.child=children(unit);
    require_component_roof_physical_directional_resize_source_admission(source,targets(unit),c.roof,c.slab,c.structural,c.child);
    if (!c.slab.empty()) {
        const auto partition=partition_phase_slab_geometry_edits(source.entities(),c.slab);
        if (partition.replacement) {
            const auto& r=*partition.replacement;const auto plan=inspect_phase_slab_replacement_plan(source.entities(),r.seed_slab_ids,r.registry_id,r.alternative_id,true);
            require(plan.ready(),"actual baseline slab inventory ready");c.slab_allocation.emplace();auto& a=*c.slab_allocation;
            for (const auto& id:plan.required_entity_ids) a.identities.emplace(id,"slab-copy-"+id);
            for (const auto& id:plan.required_child_ids) a.identities.emplace(id,"slab-copy-"+id);
            for (const auto& root:plan.required_hosted_instance_ids) a.hosted_instance_identities.emplace(root,"slab-copy-"+root.second);
            for (const auto& id:plan.required_dimension_ids) a.dimension_identities.emplace(id,"slab-copy-"+id);
        }
    }
    if (!c.structural.empty()) {
        std::vector<StructuralObjectEditIntent> edits;
        for (const auto& op:c.structural) if (const auto captured=capture_structural_object_edit(source.entities().at(op.object_id),
            stage_structural_plan_axis_resize_entity(source.entities().at(op.object_id),op.sx,op.sy,op.anchor_m,op.frame_rotation_radians))) edits.push_back(*captured);
        if (const auto r=phase_structural_edit_replacement_request(source.entities(),edits)) {
            const auto plan=inspect_phase_structural_replacement_plan(source.entities(),r->seed_object_ids,r->registry_id,r->alternative_id,true,true);
            require(plan.ready(),"actual baseline structural inventory ready");c.structural_allocation.emplace();auto& a=*c.structural_allocation;
            for (const auto& id:plan.required_entity_ids) a.identities.emplace(id,"structural-copy-"+id);
            for (const auto& id:plan.required_child_ids) a.identities.emplace(id,"structural-copy-"+id);
            for (const auto& root:plan.required_hosted_instance_ids) a.hosted_instance_identities.emplace(root,"structural-copy-"+root.second);
            for (const auto& id:plan.required_dimension_ids) a.dimension_identities.emplace(id,"structural-copy-"+id);
        }
    }
    return c;
}
MixedSelectionPropertyEditPreparation prepare(const DocumentSnapshot& source,const PhaseAssemblyInstancePropertyEditAuthoring& a,const Cohort& c) {
    return prepare_component_roof_physical_directional_resize(source,a,c.roof,c.slab,c.slab_allocation,
        c.structural,c.structural_allocation,c.child,"One original-source roof physical resize");
}
ApplyBoundaryConstraintChanges command(const DocumentSnapshot& source,const MixedSelectionPropertyEditPreparation& edit) {
    ApplyBoundaryConstraintChanges result;result.expected_revision=source.revision();result.message=edit.intent().at("message").get<std::string>();
    result.mixed_selection_edit_completion=true;result.mixed_selection_edit_intent=complete_mixed_selection_property_edit(source,edit,{},{});return result;
}
double dimension_span(const Entities& entities,const std::string& id) {
    const auto d=decode_boundary_dimension_entity(entities.at(id));require(d.supported() && d.dimension->assembly_target.has_value(),"supported final dimension");
    return resolve_assembly_dimension(entities,*d.dimension->assembly_target).span_metres;
}
void closed_wire_original_union_and_dimensions() {
    for (int version:{7,8,9}) for (bool baseline:{false,true}) for (int mask:{2,3,5,6}) {
        auto document=fixture(baseline,version);const auto source=document.snapshot();const auto before=document_snapshot_digest(source);
        const auto c=cohort(source,mask);const auto result=prepare(source,authoring(source,version),c);
        const auto& wire=result.intent();const auto& out=result.preview_snapshot().entities();
        require(wire.at("version")==47 && wire.size()==17 && wire.at("component_request").at("version")==version,"closed17-field Properties47 with component7/8/9");
        for (const auto& [key,value]:wire.items()) {
            (void)value;auto bad=wire;bad.erase(key);refuses([&] {(void)validate_mixed_selection_property_edit_intent(bad);},"all seventeen fields mandatory");
        }
        auto extra=wire;extra["physical_candidate"]=Json::object();refuses([&] {(void)validate_mixed_selection_property_edit_intent(extra);},"candidate map has no wire authority");
        for (const auto* key:{"expected_revision","source_snapshot_digest","source_authoring_digest","source_entities_digest","source_saved_revision"}) {
            require(wire.at(key).dump()==wire.at("component_request").at(key).dump(),"all original source fences equal");
            auto bad=wire;bad[key]=key==std::string_view("expected_revision") || key==std::string_view("source_saved_revision") ? Json(999) : Json(std::string(64,'0'));
            refuses([&] {(void)replay_mixed_selection_property_edit_preparation(source,bad);},"every original fence authenticates");
        }
        require(wire.at("room_reviews").empty() && wire.at("phase_room_reviews").empty(),"47 grants no review authority");
        auto reviews=wire;reviews["room_reviews"].push_back(Json::object());refuses([&] {(void)validate_mixed_selection_property_edit_intent(reviews);},"room review smuggling refuses");
        require(exact(out.at("roof"),replay_roof_plan_resize_entities(source.entities(),c.roof).at("roof")),"final roof envelope exactly equals original-source native replay");
        require(out.at("roof").properties.at("quantity_entries").at("/base_position_m/2").dump()==source.entities().at("roof").properties.at("quantity_entries").at("/base_position_m/2").dump(),"unchanged roof input receipt remains raw exact");
        if (mask&2) {
            auto expected=stage_structural_plan_axis_resize_entity(source.entities().at("column"),1.5,1,{1,0},0);
            expected.id=result.selected_original_to_proposed().at("column");require(exact(expected,out.at(expected.id)),"exact final structural native envelope");
            require(out.at(expected.id).properties.at("quantity_entries").at("/height_m").dump()==source.entities().at("column").properties.at("quantity_entries").at("/height_m").dump(),"unchanged structural receipt exact");
        }
        if (mask&1) {
            const auto id=result.selected_original_to_proposed().at("slab");
            require(out.at(id).properties.at("elevation_m")==1.25 && out.at(id).properties.at("thickness_m")==.2,"slab Z and thickness preserved");
            require(out.at(id).properties.at("quantity_entries").at("/elevation_m").dump()==source.entities().at("slab").properties.at("quantity_entries").at("/elevation_m").dump(),"opaque unchanged slab receipt exact");
        }
        if (mask&4) {
            const auto expected=prepare_roof_opening_directional_resizes(source.entities(),c.child);
            require(exact(out.at("host"),expected.candidate_entities.at("host")),"exact qualified child-host envelope from original source");
            const auto child=result.roof_child_original_to_proposed().at({"host","child"});
            require(child.roof_id=="host" && child.opening_id=="child","qualified selected child correspondence exact");
        }
        const auto component_dimension=baseline ? "component-copy-component-dim" : "component-dim";
        require_near(dimension_span(out,component_dimension)/dimension_span(source.entities(),"component-dim"),1.5,"one final component dimension carry");
        if ((mask&3)==3 && baseline) {
            const auto slab=result.hosted_binding_original_to_proposed().at({"catalog","slab-root"});
            const auto column=result.hosted_binding_original_to_proposed().at({"catalog","column-root"});
            require(slab.first=="slab-copy-catalog" && column.first=="structural-copy-catalog" && slab.first!=column.first,"two actual baseline allocations share only the original catalog");
            require(!result.original_to_proposed().contains("catalog"),"ambiguous private catalogs never flatten to a source catalog identity");
            require(exact(out.at("catalog"),source.entities().at("catalog")),"shared source catalog retained exact");
            for (const auto& id:{slab.first,column.first}) {
                require(out.at(id).properties.at("model").at("instances").size()==1,"private catalog contains only its own family root");
                require(out.at(id).properties.at("opaque")=="shared catalog metadata","raw private catalog metadata retained");
            }
            require(result.phase_authorings().size()==2,"both actual private contributions retain their own source-bound proof");
            const auto source_aliases=embedded_assembly_presentation_ids(source.entities());const auto final_aliases=embedded_assembly_presentation_ids(out);
            for (const auto& binding:std::vector<AssemblyPhaseRoot>{{"catalog","slab-root"},{"catalog","column-root"}})
                require(result.original_to_proposed().at(source_aliases.at(binding))==final_aliases.at(result.hosted_binding_original_to_proposed().at(binding)),"qualified aliases use the actual private family destination");
            require(out.at("membership").properties.at("opaque")=="retained membership","shared typed phase companion preserves raw metadata");
        }
        if ((mask&3)==3 && !baseline) {
            const auto& old=source.entities().at("catalog").properties.at("model");const auto& now=out.at("catalog").properties.at("model");
            require(old.at("types").dump()==now.at("types").dump() && old.at("instances").size()==now.at("instances").size(),"ordinary shared catalog merges rows without changing definitions or row inventory");
        }
        if (!(mask&1) && !baseline) {
            const auto& old=source.entities().at("catalog").properties.at("model").at("instances");
            const auto& now=out.at("catalog").properties.at("model").at("instances");
            const auto source_row=std::find_if(old.begin(),old.end(),[](const auto& row) {return row.at("id")=="slab-root";});
            const auto final_row=std::find_if(now.begin(),now.end(),[](const auto& row) {return row.at("id")=="slab-root";});
            require(source_row!=old.end() && final_row!=now.end() && source_row->dump()==final_row->dump(),"unselected shared ordinary catalog row stays raw exact");
        }
        for (const auto& proof:result.phase_authorings()) require(decode_phase_constraint_authoring_intent(proof).source_snapshot_digest==before,"every reconstructed proof binds the same complete original source");
        require(replay_mixed_selection_property_edit(source,complete_mixed_selection_property_edit(source,result,{},{})).entities==out,"closed47 completion replays exact one candidate");
        require(document_snapshot_digest(document.snapshot())==before,"detached prepare preserves source, history, receipts and assets");
        auto reservations=wire;reservations["component_identity_reservations"].push_back("invented-reservation");
        refuses([&] {(void)replay_mixed_selection_property_edit_preparation(source,reservations);},"reservation receipt is exact replay inventory");
    }
}
void units_exclusions_retirement_and_freshness() {
    auto document=fixture();const auto source=document.snapshot();
    for (int mask:{2,3,5,6}) {
        const auto c=cohort(source,mask,true);const auto result=prepare(source,authoring(source),c);
        require(result.intent().at("version")==47 && result.selected_original_to_proposed().contains("roof"),"physical units remain in source roster and selection");
        require(exact(result.preview_snapshot().entities().at("roof"),source.entities().at("roof")),"whole roof unit remains byte exact");
    }
    // Family cardinality is evaluated before eliminating every unit operator.
    budget_refuses([&] {require_component_roof_physical_directional_resize_source_admission(source,targets(true),roofs(true),slabs(source,true),structures(true),children(true));},"selection bound");
    auto four=prepare(source,authoring(source),cohort(source,3,true)).intent();
    four["roof_opening_axis_requests"]=Json::array({{{"roof_id","host"},{"opening_id","child"},{"sx",1},{"sy",1},{"anchor_m",{1,0}},{"frame_rotation_radians",0}}});
    refuses([&] {(void)validate_mixed_selection_property_edit_intent(four);},"closed47 wire rejects four families before eliminating units");
    refuses([&] {(void)prepare_component_roof_physical_directional_resize(source,authoring(source),roofs(true),{},std::nullopt,{},std::nullopt,{},"One family");},"roof-only47 refuses");
    auto c=cohort(source,6);c.child[0].target.roof_id="roof";
    refuses([&] {(void)prepare(source,authoring(source),c);},"same whole owner and child host refuses, including units");
    c=cohort(source,2);c.slab_allocation.emplace();refuses([&] {(void)prepare(source,authoring(source),c);},"absent slab family cannot allocate even an empty allocation object");
    auto bad=source.entities();bad.at("host").properties["phase_id"]=nullptr;
    refuses([&] {require_component_roof_physical_directional_resize_source_admission(replaced(source,bad),targets(),roofs(),{},structures(),children());},"null phase_id violates literal child-host absence");
    auto scalar=authoring(source);scalar.intents[0].properties.emplace("name",AssemblyInstancePropertyPatch<std::string>{AssemblyInstancePropertyPatchOperation::set,"scalar"});
    refuses([&] {(void)prepare(source,scalar,cohort(source,2));},"47 does not acquire scalar component authority");
    // Pose is independently disallowed even when geometry is also requested.
    scalar=authoring(source);scalar.intents[0].pose.operation=AssemblyInstancePropertyPatchOperation::set;
    refuses([&] {(void)prepare(source,scalar,cohort(source,2));},"47 does not acquire component pose authority");
    const auto retired=prepare(source,authoring(source,7,true),cohort(source,2));
    require(!retired.preview_snapshot().entities().contains("component-dim") && mixed_selection_property_edit_retires_dependencies(retired.intent()),"reviewed source dimension retires in one finalizer");
    for (bool curved:{false,true}) {
        auto base=fixture(true,7,curved);const auto original=base.snapshot();const auto actual=cohort(original,3);
        const auto mapped=prepare(original,authoring(original),actual);
        require(mapped.intent().at("slab_geometry")[0].at("version")== (curved ? 5 : 4),"coordinated slab leaf4/5 retained");
        auto clash=actual;clash.structural_allocation->identities.at("catalog")=clash.slab_allocation->identities.at("catalog");
        budget_refuses([&] {preflight_component_roof_physical_directional_resize(original,authoring(original),clash.roof,clash.slab,clash.slab_allocation,clash.structural,clash.structural_allocation,clash.child);},"collides");
        clash=actual;clash.structural_allocation->dimension_identities.emplace("slab-dim","another-dimension-copy");
        budget_refuses([&] {preflight_component_roof_physical_directional_resize(original,authoring(original),clash.roof,clash.slab,clash.slab_allocation,clash.structural,clash.structural_allocation,clash.child);},"provisional source dimension");
        clash=actual;clash.slab_allocation->identities.at("slab")=embedded_assembly_presentation_ids(original.entities()).at({"child-catalog","child-root"});
        refuses([&] {preflight_component_roof_physical_directional_resize(original,authoring(original),clash.roof,clash.slab,clash.slab_allocation,clash.structural,clash.structural_allocation,clash.child);},"actual qualified presentation aliases reserve the shared identity namespace");
        clash=actual;clash.structural_allocation->identities.emplace("not-in-actual-inventory","invented-copy");
        refuses([&] {(void)prepare(original,authoring(original),clash);},"actual baseline allocation inventories must be exact");
        const auto with_asset=sketch::test::DetachedDocumentSnapshotFixture::mutate(original,[&](auto& state) {
            state.assets().emplace("structural-copy-column",Asset::create("structural-copy-column","application/octet-stream",{std::byte{1}}));
        });
        budget_refuses([&] {preflight_component_roof_physical_directional_resize(with_asset,authoring(with_asset),actual.roof,actual.slab,actual.slab_allocation,actual.structural,actual.structural_allocation,actual.child);},"current asset");
        const auto with_history=sketch::test::DetachedDocumentSnapshotFixture::mutate(original,[&](auto& state) {
            auto historical=state.history().front();historical.entities.emplace("slab-copy-slab",Entity{"slab-copy-slab","label",Json::object(),false});
            state.history().push_back(std::move(historical));
        });
        // Current revision remains the original; the retained extra record is
        // deliberately adversarial and must still reserve its old identity.
        refuses([&] {preflight_component_roof_physical_directional_resize(with_history,authoring(original),actual.roof,actual.slab,actual.slab_allocation,actual.structural,actual.structural_allocation,actual.child);},"retained history identity collision refuses");
    }
    const auto c46=cohort(source,1);require(prepare_component_roof_slab_directional_resize(source,authoring(source),c46.roof,c46.slab,std::nullopt,"Historical46").intent().at("version")==46,"historical46 wire meaning preserved");
    require(prepare_component_roof_skylight_directional_resize(source,authoring(source),roofs(),children(),"Historical45").intent().at("version")==45,"historical45 wire meaning preserved");
    for (bool baseline:{false,true}) {
        auto seed=fixture(baseline,7,true);auto actual=seed.snapshot().entities();auto initial=slabs(seed.snapshot());
        actual.at("slab")=replay_slab_geometry_entity(actual.at("slab"),initial[0]);
        actual.at("slab").properties["quantity_entries"]["/boundary/0/start/0"]=receipt(1);
        const auto mapped=replaced(seed.snapshot(),actual);const auto result=prepare(mapped,authoring(mapped),cohort(mapped,3));
        const auto id=result.selected_original_to_proposed().at("slab");const auto& slab=result.preview_snapshot().entities().at(id);
        require(slab.properties.at("boundary").dump()==actual.at("slab").properties.at("boundary").dump(),"map-only slab retains stored source endpoints");
        require(!slab.properties.at("quantity_entries").contains("/boundary/0/start/0"),"map-only slab receipt retirement unchanged in47");
        require(slab.extensions.at(std::string(slab_geometry_derivations_key)).at("operations").back().at("receipts").at("quantity_entries").at("/boundary/0/start/0").dump()==receipt(1).dump(),"retired mapped receipt stays raw exact in archive");
    }
}
void combined_analytical_admission() {
    auto document=fixture();const auto source=document.snapshot();
    const auto gate=[&](const DocumentSnapshot& s,int mask=6,bool unit=false) {
        require_component_roof_physical_directional_resize_source_admission(s,targets(unit),roofs(unit),
            mask&1 ? slabs(s,unit) : std::vector<SlabGeometryEditIntent>{},
            mask&2 ? structures(unit) : std::vector<StructuralDirectionalResizeRequest>{},
            mask&4 ? children(unit) : std::vector<RoofOpeningDirectionalResizeRequest>{});
    };
    auto rows=source.entities();rows.at("roof").extensions[std::string(roof_plan_resize_derivations_key)]={{"version",999},{"unmanufacturable",true}};
    const auto analytical=replaced(source,rows);
    gate(analytical); // A pure gate must not validate the native/archive roof.
    refuses([&] {(void)replay_roof_plan_resize_entities(analytical.entities(),roofs());},"control roof archive is genuinely unmanufacturable");
    const auto override_source=[&](const std::string& catalog,std::size_t segments,bool nested) {
        auto actual=rows;auto model=AssemblyModel::from_json(actual.at(catalog).properties.at("model"));
        auto types=model.types();auto instances=model.instances();AssemblyProfile p;p.id="override-profile";p.height_m=1;
        for (std::size_t i=0;i<segments;++i) {
            const auto a=2*std::numbers::pi*static_cast<double>(i)/segments;
            const auto b=2*std::numbers::pi*static_cast<double>(i+1)/segments;
            p.outer.push_back({{std::cos(a),std::sin(a)},{std::cos(b),std::sin(b)},0});
        }
        for (auto& instance:instances) {
            if (nested) {
                AssemblyPathOverride override;override.part_path={"part"};override.profile_overrides=std::vector<AssemblyProfile>{p};
                instance.nested_overrides={override};
            } else instance.profile_overrides=std::vector<AssemblyProfile>{p};
        }
        if (nested) {
            AssemblyType parent;parent.id="parent";parent.name="Actual repeated override parent";
            AssemblyPart part;part.id="part";part.type_id="body";parent.parts={part};types.push_back(parent);
            for (auto& instance:instances) instance.type_id="parent";
        }
        actual.at(catalog).properties["model"]=AssemblyModel::create(model.materials(),types,instances).to_json();
        return replaced(source,actual);
    };
    for (const auto& catalog:std::vector<std::string>{"catalog","child-catalog"}) for (bool nested:{false,true}) {
        gate(override_source(catalog,4,nested),6,true);
        budget_refuses([&] {gate(override_source(catalog,768,nested),6,true);},"aggregate capture");
    }
    const auto repeated=[&](std::size_t count) {
        auto actual=rows;const auto original=AssemblyModel::from_json(actual.at("catalog").properties.at("model"));
        auto types=original.types();AssemblyType parent;parent.id="parent";parent.name="Repeated DAG paths";
        for (std::size_t i=0;i<count;++i) {AssemblyPart part;part.id="part-"+std::to_string(i);part.type_id="body";parent.parts.push_back(part);}
        types.push_back(parent);auto instances=original.instances();for (auto& instance:instances) instance.type_id="parent";
        actual.at("catalog").properties["model"]=AssemblyModel::create(original.materials(),types,instances).to_json();
        auto independent=decode_document_assembly_instance(actual.at("component"));independent.instance.type_id="parent";
        actual.at("component")=encode_document_assembly_instance(actual.at("component"),independent);
        actual.erase("component-dim");return replaced(source,actual);
    };
    gate(repeated(2),3,true);
    budget_refuses([&] {gate(repeated(100),3,true);},"aggregate capture");
    const auto borrowed=[&](std::size_t count) {
        auto actual=rows;AssemblyType body;body.id="borrowed-body";body.name="Legacy borrowed slab profiles";
        std::vector<AssemblyInstance> roots;
        for (std::size_t i=0;i<count;++i) {AssemblyInstance root;root.id="borrowed-"+std::to_string(i);root.type_id=body.id;
            root.placement=AssemblyPlacement{"slab",{0,0},0,1};roots.push_back(root);}
        actual.emplace("legacy",Entity{"legacy","assembly_model",{{"model",AssemblyModel::create({}, {body},roots).to_json()}},false});
        return replaced(source,actual);
    };
    gate(borrowed(2),3,true);
    budget_refuses([&] {gate(borrowed(100),3,true);},"aggregate capture");
    auto dimensions=rows;
    for (unsigned i=0;i<1000;++i) {
        const bool child=i%2;const auto id="extra-dim-"+std::to_string(i);
        dimensions.emplace(id,dimension(id,child ? "child-catalog" : "catalog",child ? "child-root" : "column-root",std::nullopt));
    }
    budget_refuses([&] {gate(replaced(source,dimensions));},"combined dimension association");
    const auto full=[&](const DocumentSnapshot& s,const PhaseAssemblyInstancePropertyEditAuthoring& a,const Cohort& c) {
        preflight_component_roof_physical_directional_resize(s,a,c.roof,c.slab,c.slab_allocation,c.structural,c.structural_allocation,c.child);
    };
    auto base=fixture(true);const auto original=base.snapshot();const auto c=cohort(original,3);const auto a=authoring(original);
    auto many=c;
    for (unsigned i=0;i<2100;++i) {
        many.slab_allocation->identities.emplace("slab-source-"+std::to_string(i),"slab-fresh-"+std::to_string(i));
        many.structural_allocation->identities.emplace("structural-source-"+std::to_string(i),"structural-fresh-"+std::to_string(i));
    }
    budget_refuses([&] {full(original,a,many);},"fresh identity budget");
    const auto filled=[&](std::size_t size) {
        auto actual=original.entities();for (std::size_t i=actual.size();i<size;++i) {
            const auto id="padding-"+std::to_string(i);actual.emplace(id,Entity{id,"label",Json::object(),false});
        }
        return replaced(original,actual);
    };
    const auto below=filled(65500);gate(below,3);full(below,a,c);
    // Each allocation fits its family's map. Their combined peak does not.
    const auto crowded=filled(65530);auto only_slab=c;only_slab.structural.clear();only_slab.structural_allocation.reset();
    auto only_structural=c;only_structural.slab.clear();only_structural.slab_allocation.reset();
    full(crowded,a,only_slab);full(crowded,a,only_structural);
    budget_refuses([&] {full(crowded,a,c);},"combined peak entity");
    budget_refuses([&] {gate(crowded,3);},"combined peak entity");
    budget_refuses([&] {gate(filled(65537),2,true);},"source entity bound");
    std::vector<AssemblyDirectionalResizeTarget> overselected(1000,targets()[0]);
    budget_refuses([&] {require_component_roof_physical_directional_resize_source_admission(source,overselected,roofs(),{},structures(),{});},"selection bound");
}
std::array<double,6> bounds(const TopoDS_Shape& shape) {
    require(!shape.IsNull() && solid_volume(shape)>0,"genuine positive volume solid");Bnd_Box box;BRepBndLib::Add(shape,box);
    std::array<double,6> result;box.Get(result[0],result[1],result[2],result[3],result[4],result[5]);return result;
}
void native_hand_geometry() {
    auto document=fixture();const auto source=document.snapshot();
    AssemblyExpansionBudget source_budget;const auto expansion=expand_document_assembly_instance(source.entities().at("component"),source.entities(),source_budget);
    const auto model=AssemblyModel::from_json(source.entities().at("catalog").properties.at("model"));
    require_near(solid_volume(make_assembly_geometry(expansion).shape),3,"source component hand volume3");
    require_near(solid_volume(make_roof_structure_shape(decode_roof_entity(source.entities().at("roof")))),1,"source roof hand volume1");
    require_near(solid_volume(make_building_shape(decode_building_entity(source.entities().at("column")))) ,4,"source column hand volume4");
    for (int mask:{2,3,5,6}) for (bool x:{false,true}) for (bool opposite:{false,true}) {
        const Vec2 anchor{x ? (opposite ? 7. : 1.) : 1.,x ? 0. : (opposite ? 4. : 0.)};
        const auto sx=x ? 1.5 : 1.,sy=x ? 1. : 1.5;auto c=cohort(source,mask);
        c.roof[0].scale_x=sx;c.roof[0].scale_y=sy;c.roof[0].anchor=anchor;
        for (auto& slab:c.slab) slab.resize=SlabPlanAxisResize{sx,sy,anchor,0};
        for (auto& structural:c.structural) {structural.sx=sx;structural.sy=sy;structural.anchor_m=anchor;}
        for (auto& child:c.child) {child.sx=sx;child.sy=sy;child.anchor_m=anchor;}
        std::vector<AssemblyDirectionalResizeTarget> selected{{"component",{anchor,sx,sy,0,std::nullopt}}};
        require_component_roof_physical_directional_resize_source_admission(source,selected,c.roof,c.slab,c.structural,c.child);
        AssemblyExpansionBudget available;const auto draft=draft_assembly_directional_resize(model,expansion,selected[0].operation,available);
        auto a=authoring(source);auto profiles=draft.nodes.front().candidate_profiles;profiles.front().topology=topology("hand-");
        a.intents[0].profile_geometry.value=profiles;a.profile_dimension_decisions.at("component-dim")=selector("hand-");
        const auto result=prepare(source,a,c);const auto& out=result.preview_snapshot().entities();AssemblyExpansionBudget budget;
        std::vector<TopoDS_Shape> shapes{make_assembly_geometry(expand_document_assembly_instance(out.at("component"),out,budget)).shape,
            make_roof_structure_shape(decode_roof_entity(out.at("roof")))};
        std::vector<std::array<double,6>> before{{5,1,0,7,2,1.5},{1,0,3.75,3,2,4}};std::vector<double> volumes{4.5,1.5};
        if (mask&2) {shapes.push_back(make_building_shape(decode_building_entity(out.at("column"))));before.push_back({3,3,.5,5,4,2.5});volumes.push_back(6);}
        if (mask&1) {Slab slab;std::string error;require(read_document_slab(out.at("slab"),slab,error),"native decoded slab");shapes.push_back(make_slab(slab));before.push_back({1,0,1.25,4,3,1.45});volumes.push_back(2.7);}
        for (std::size_t i=0;i<shapes.size();++i) {
            const auto observed=bounds(shapes[i]);auto expected=before[i];
            expected[0]=anchor.x+sx*(expected[0]-anchor.x);expected[3]=anchor.x+sx*(expected[3]-anchor.x);
            expected[1]=anchor.y+sy*(expected[1]-anchor.y);expected[4]=anchor.y+sy*(expected[4]-anchor.y);
            for (std::size_t j=0;j<6;++j) require(std::abs(observed[j]-expected[j])<1e-6,"native hand bounds on both axes and anchors preserve Z");
            require_near(solid_volume(shapes[i]),volumes[i],"native hand volume after one common affine");
        }
    }
}
struct ProjectFile {
    std::filesystem::path directory,path;
    ProjectFile() {
        const auto stamp=std::chrono::steady_clock::now().time_since_epoch().count();
        for (unsigned i=0;i<64;++i) {
            directory=std::filesystem::temp_directory_path()/("vertex-roof-physical-"+std::to_string(stamp)+"-"+std::to_string(i));
            if (std::filesystem::create_directory(directory)) {path=directory/"history.bldproj";return;}
        }
        throw std::runtime_error("unique roof physical storage directory unavailable");
    }
    ~ProjectFile() {std::error_code error;std::filesystem::remove_all(directory,error);}
};
void old_reader_refusal(const std::filesystem::path& path) {
    const auto u8=path.u8string();const std::string name(reinterpret_cast<const char*>(u8.data()),u8.size());sqlite3* raw=nullptr;
    const auto opened=sqlite3_open_v2(name.c_str(),&raw,SQLITE_OPEN_READWRITE,nullptr);
    std::unique_ptr<sqlite3,decltype(&sqlite3_close)> db(raw,&sqlite3_close);require(opened==SQLITE_OK,"open controlled storage marker fixture");
    require(sqlite3_exec(db.get(),"BEGIN; PRAGMA user_version=269; UPDATE metadata SET value='269' WHERE key='format_version'; COMMIT",nullptr,nullptr,nullptr)==SQLITE_OK,"downgrade only reader marker pair");db.reset();
    const auto before=ProjectStore::file_sha256(path);bool refused=false;
    try {(void)ProjectStore::load(path);} catch (const StorageError& error) {
        require(error.code()==StorageErrorCode::unsupported_format && std::string(error.what()).find("requires reader 270")!=std::string::npos,"reader269 refuses canonical retained47 at270");refused=true;
    }
    require(refused && ProjectStore::file_sha256(path)==before,"old reader refusal preserves file bytes");
}
void persistence_and_historical_floors() {
    auto seed=fixture();auto rows=seed.snapshot().entities();auto instance=decode_document_assembly_instance(rows.at("component"));
    instance.instance.id="earlier-component";rows.emplace("earlier-component",encode_document_assembly_instance({"earlier-component","assembly_instance",context},instance));
    std::vector<Entity> values;for (const auto& [id,entity]:rows) {(void)id;values.push_back(entity);}auto document=Document::create(values);
    const auto initial=document.snapshot();auto earlier_authoring=authoring(initial);
    earlier_authoring.intents[0].target.instance_id="earlier-component";earlier_authoring.intents[0].target.document_entity_id="earlier-component";
    earlier_authoring.profile_dimension_decisions.clear();earlier_authoring.dimension_identities.clear();earlier_authoring.allocation={};
    const auto earlier=prepare_component_roof_slab_directional_resize(initial,earlier_authoring,roofs(true),slabs(initial,true),std::nullopt,"Earlier46");
    document.apply(command(initial,earlier));const auto source=document.snapshot();
    require(ProjectStore::required_format_version(source)==269,"earlier canonical46 retains historical reader269");
    const auto result=prepare(source,authoring(source),cohort(source,3));const auto operation=command(source,result);const auto wire=command_to_json(operation);
    require(wire.at("version")==52 && wire.at("mixed_selection_edit_intent").at("version")==47,"canonical command52 Properties47");
    document.apply(operation);const auto applied=document.snapshot();document.undo(document.revision());const auto undone=document.snapshot();
    require(undone.entities()==source.entities(),"one Undo restores every47 source owner and dimension");
    document.redo(document.revision());require(document.snapshot().entities()==applied.entities(),"one Redo restores exact combined result");
    for (const auto& snapshot:{applied,undone}) {
        require(ProjectStore::required_format_version(snapshot)==270 && ProjectStore::required_semantic_format_version(snapshot)==270,"history scanner passes earlier46 to find applied or undone47");
        ProjectFile file;(void)ProjectStore::save(file.path,snapshot);auto loaded=ProjectStore::load(file.path);const auto reopened=loaded.document.snapshot();
        require(reopened.entities()==snapshot.entities() && reopened.assets()==snapshot.assets() && reopened.history().size()==snapshot.history().size(),"editable fresh Open preserves graph assets and complete history");
        require(command_to_json(*reopened.history().at(applied.revision()).boundary_constraint_changes)==wire,"fresh Open retains exact canonical52/47 proof");
        require(reopened.history().back().undo_stack==snapshot.history().back().undo_stack &&
            reopened.history().back().redo_stack==snapshot.history().back().redo_stack,
            "storage retains navigation evidence; desktop Open owns the new session boundary");
        auto property=reopened.entities().at("property");property.properties["name"]="Editable fresh Open";
        loaded.document.apply(ApplyEntityChanges{.expected_revision=loaded.document.revision(),.entity_changes={EntityChange::upsert(property)},.message="New Undo session"});
        loaded.document.undo(loaded.document.revision());require(loaded.document.snapshot().entities()==reopened.entities(),"fresh Open supports new edits and a new Undo session");
        extract_project(snapshot,file.directory/"extracted");std::ifstream input(file.directory/"extracted"/"project.json");const auto manifest=Json::parse(input);
        require(manifest.at("exchange_version")==268 && manifest.at("revisions").at(applied.revision()).at("boundary_constraint_changes")==wire,"extraction268 retains applied or undone47 behind46");
        old_reader_refusal(file.path);
    }
    auto opaque=fixture();const auto original=opaque.snapshot();const auto actual=prepare(original,authoring(original),cohort(original,2));
    auto property=original.entities().at("property");property.properties["opaque_47"]=actual.intent();property.extensions["opaque_command"]=command_to_json(command(original,actual));
    opaque.apply(ApplyEntityChanges{.expected_revision=opaque.revision(),.entity_changes={EntityChange::upsert(property)},.message="Opaque47 control"});
    require(ProjectStore::required_format_version(opaque.snapshot())==ProjectStore::required_format_version(original),"opaque47 lookalikes do not raise reader floor");
    ProjectFile opaque_file;extract_project(opaque.snapshot(),opaque_file.directory/"extracted");std::ifstream opaque_input(opaque_file.directory/"extracted"/"project.json");
    ProjectFile original_file;extract_project(original,original_file.directory/"extracted");std::ifstream original_input(original_file.directory/"extracted"/"project.json");
    require(Json::parse(opaque_input).at("exchange_version")==Json::parse(original_input).at("exchange_version"),"opaque47 lookalikes do not raise extraction floor");
    const auto archived=replay_roof_plan_resize_entities(original.entities(),roofs());std::vector<Entity> archived_values;
    for (const auto& [id,entity]:archived) {(void)id;archived_values.push_back(entity);}const auto archive_only=Document::create(archived_values).snapshot();
    require(ProjectStore::required_format_version(archive_only)==267,"archive4-only historical reader267 retained");
    ProjectFile archive_file;extract_project(archive_only,archive_file.directory/"extracted");std::ifstream archive_input(archive_file.directory/"extracted"/"project.json");
    require(Json::parse(archive_input).at("exchange_version")==265,"archive4-only historical extraction265 retained");
}
} // namespace
int main(int argc,char** argv) {
    sketch::testing::noninteractive_errors();
    try {
        const std::string_view only=argc>1 ? argv[1] : "";
        if (only.empty() || only=="--core") {closed_wire_original_union_and_dimensions();units_exclusions_retirement_and_freshness();}
        if (only.empty() || only=="--budget") combined_analytical_admission();
        if (only.empty() || only=="--native") native_hand_geometry();
        if (only.empty() || only=="--storage") persistence_and_historical_floors();
        std::cout<<"Mixed component/roof/physical resize contracts passed\n";return 0;
    } catch (const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
