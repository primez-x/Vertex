#include "sketch/mixed_selection_property_edit.hpp"
#include "sketch/phase_constraint_authoring.hpp"
#include "sketch/assembly_directional_resize.hpp"
#include "sketch/assembly_geometry.hpp"
#include "sketch/assembly_dimension.hpp"
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
#include <source_location>
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
template<class F> void budget_refuses(F&& action,const char* diagnostic,
    const std::source_location location=std::source_location::current()) {
    try {action();} catch (const std::invalid_argument& error) {
        if (std::string(error.what()).find(diagnostic)==std::string::npos)
            throw std::runtime_error("budget case at line "+std::to_string(location.line())+": expected "+diagnostic+", observed "+error.what());
        return;
    }
    throw std::runtime_error("aggregate admission did not refuse at line "+std::to_string(location.line())+": "+diagnostic);
}
bool exact(const Entity& a,const Entity& b) {
    return a==b && a.properties.dump()==b.properties.dump() && a.extensions.dump()==b.extensions.dump();
}
const Json context={{"property_id","property"},{"building_id","building"},{"floor_id","floor"},{"layer_id","layer"}};
Boundary ring(bool curved=false) {
    return {{{0,0},{4,0},0},{{4,0},{4,3},0},{{4,3},{0,3},curved ? std::numbers::pi : 0},{{0,3},{0,0},0}};
}
Json raw_ring(const Boundary& ring) {
    Json rows=Json::array();for (const auto& edge:ring) rows.push_back({{"start",{edge.start.x,edge.start.y}},
        {"end",{edge.end.x,edge.end.y}},{"sweep_radians",edge.sweep_radians}});return rows;
}
AssemblyProfileTopology topology(const std::string& prefix) {
    return {{prefix+"outer",{prefix+"e0",prefix+"e1",prefix+"e2",prefix+"e3"},
        {prefix+"v0",prefix+"v1",prefix+"v2",prefix+"v3"}},{}};
}
AssemblyProfileDimensionTarget selector(const std::string& prefix) {
    return {"profile",prefix+"outer",AssemblyProfileDimensionMeasurement::edge_length,prefix+"e0",std::nullopt,std::nullopt};
}
Json receipt(int metres) {
    return {{"version",1},{"original_expression",std::to_string(metres)+" m"},{"entered_unit","m"},
        {"exact_metres",{{"numerator",metres},{"denominator",1},{"vendor","raw rational"}}},{"vendor",Json::array({17,"raw",true})}};
}
Entity dimension(std::string id,bool hosted,bool profile=false) {
    BoundaryDimension d;d.id=std::move(id);d.boundary_id=hosted ? "catalog" : "component";
    d.kind=profile ? BoundaryDimensionKind::assembly_profile : BoundaryDimensionKind::assembly_extent;d.text_position={7,-2};
    AssemblyDimensionTarget target;target.component={"catalog",hosted ? "host-root" : "component",
        hosted ? std::nullopt : std::optional<std::string>{"component"},{}};target.axis=AssemblyDimensionAxis::width;
    if (profile) target.profile=selector("");d.assembly_target=target;return encode_boundary_dimension_entity(d);
}
Document fixture(int version=7,bool component_baseline=false,bool slab_baseline=false,bool curved=false,
    std::size_t component_dimensions=1,std::size_t hosted_dimensions=1) {
    AssemblyProfile p;p.id="profile";p.outer=ring(curved);p.elevation_m=.4;p.height_m=.6;
    if (version==7) p.topology=topology("");
    AssemblyType body;body.id="body";body.name="Actual source body";body.profiles={p};
    AssemblyInstance hosted;hosted.id="host-root";hosted.type_id="body";hosted.placement=AssemblyPlacement{"slab",{.6,-.35},.23,1,true,.2,1};
    AssemblyInstance independent;independent.id="component";independent.type_id="body";
    independent.root_transform=AssemblyTransform{{5,1,.75},.29,1,true};
    auto roof=encode_building_entity(SlopedRoofPanel{.id="roof",.base_position={1,0,4},.run=2,.span=2,.rise=0,
        .pitch_radians=0,.overhang=0,.thickness=.25});
    for (const auto& [key,value]:context.items()) roof.properties[key]=value;
    roof.properties["quantity_entries"]={{"/run_m",receipt(2)},{"/base_position_m/2",receipt(4)},
        {"/vendor/numeric",{{"version",81},{"opaque",true}}}};
    roof.properties["vendor"]={{"numeric",7}};roof.extensions={{"vendor",{{"retain","roof-envelope"}}}};
    auto slab_properties=context;slab_properties["boundary"]=raw_ring(ring(curved));slab_properties["holes"]=Json::array();
    slab_properties["thickness_m"]=.2;slab_properties["elevation_m"]=1.25;
    slab_properties["quantity_entries"]={{"/boundary/0/start/0",receipt(0)},
        {"/elevation_m",{{"version",88},{"opaque","unchanged elevation"}}}};
    Entity slab{"slab","slab",slab_properties,false};slab.extensions={{"vendor",{{"retain","slab-envelope"}}}};
    std::vector<Entity> rows{{"property","property",{{"name","Three actual owners"}},false},
        {"building","building",{{"property_id","property"}},false},{"floor","floor",{{"building_id","building"}},false},
        {"layer","layer",{{"floor_id","floor"}},false},
        {"catalog","assembly_model",{{"model",AssemblyModel::create({}, {body},{hosted}).to_json()},{"vendor",{{"retain","shared source"}}}},false},
        encode_document_assembly_instance({"component","assembly_instance",context},{"catalog",independent}),roof,slab};
    for (std::size_t i=0;i<component_dimensions;++i) rows.push_back(dimension("component-dim-"+std::to_string(i),false,version==7));
    for (std::size_t i=0;i<hosted_dimensions;++i) rows.push_back(dimension("host-dim-"+std::to_string(i),true));
    if (component_baseline || slab_baseline) {
        std::vector<std::string> ids;
        if (component_baseline) {ids.push_back("component");for (std::size_t i=0;i<component_dimensions;++i) ids.push_back("component-dim-"+std::to_string(i));}
        if (slab_baseline) {ids.insert(ids.end(),{"slab","catalog"});for (std::size_t i=0;i<hosted_dimensions;++i) ids.push_back("host-dim-"+std::to_string(i));}
        rows.push_back({"phases","model_phases",{{"model",ModelPhases::create(ids,ids,{{"alternative","Choice",{}, {}}},"alternative").to_json()}},false});
    }
    return Document::create(rows);
}
DocumentSnapshot replaced(const DocumentSnapshot& source,const Entities& entities) {
    return sketch::test::DetachedDocumentSnapshotFixture::mutate(source,[&](auto& state) {state.entities()=entities;});
}
PhaseAssemblyInstancePropertyEditAuthoring authoring(const DocumentSnapshot& source,int version=7,bool retire=false) {
    auto p=AssemblyModel::from_json(source.entities().at("catalog").properties.at("model")).types().front().profiles.front();
    p.planar_affine=AssemblyProfilePlanarAffine{{0,0},{0,0},1.8,0,0,.65};p.topology=topology("new-");
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
std::vector<RoofPlanResizeIntent> roofs(bool unit=false) {
    return {{"roof",unit ? 1. : 1.5,1,{1,0},0,PlanAxisResizeAnchorMode::shared_frame}};
}
std::vector<SlabGeometryEditIntent> slabs(const DocumentSnapshot& source,bool unit=false) {
    SlabGeometryEditIntent edit;edit.slab_id="slab";edit.kind=SlabGeometryEditKind::resize_plan;
    edit.resize=SlabPlanAxisResize{unit ? 1. : 1.5,1,{1,0},0};edit.coordinate_world_hosted_geometry=true;
    edit.preserve_planar_source_geometry=slab_geometry_edit_requires_planar_source(source.entities().at("slab"),edit);return {edit};
}
std::vector<AssemblyDirectionalResizeTarget> targets() {return {{"component",{{1,0},1.5,1,0,std::nullopt}}};}
std::optional<ComponentSlabDirectionalResizeAllocation> allocation(const DocumentSnapshot& source,
    const std::vector<SlabGeometryEditIntent>& edits) {
    // The test obeys the same source-only gate before its native inventory.
    require_roof_slab_directional_resize_source_admission(source,targets(),roofs(),edits);
    const auto partition=partition_phase_slab_geometry_edits(source.entities(),edits);
    if (!partition.replacement) return std::nullopt;
    const auto& request=*partition.replacement;
    const auto plan=inspect_phase_slab_replacement_plan(source.entities(),request.seed_slab_ids,request.registry_id,request.alternative_id,true);
    require(plan.ready(),"actual baseline slab inventory must be ready");ComponentSlabDirectionalResizeAllocation result;
    for (const auto& id:plan.required_entity_ids) result.identities.emplace(id,"slab-copy-"+id);
    for (const auto& id:plan.required_child_ids) result.identities.emplace(id,"slab-copy-"+id);
    for (const auto& root:plan.required_hosted_instance_ids) result.hosted_instance_identities.emplace(root,"slab-copy-"+root.second);
    for (const auto& id:plan.required_dimension_ids) result.dimension_identities.emplace(id,"slab-copy-"+id);return result;
}
ApplyBoundaryConstraintChanges command(const DocumentSnapshot& source,const MixedSelectionPropertyEditPreparation& edit) {
    ApplyBoundaryConstraintChanges result;result.expected_revision=source.revision();result.message=edit.intent().at("message").get<std::string>();
    result.mixed_selection_edit_completion=true;result.mixed_selection_edit_intent=complete_mixed_selection_property_edit(source,edit,{},{});return result;
}
void closed_wire_and_original_contributions() {
    for (int version:{7,8,9}) for (bool component_baseline:{false,true}) for (bool slab_baseline:{false,true}) for (bool curved:{false,true}) {
        auto document=fixture(version,component_baseline,slab_baseline,curved);const auto source=document.snapshot();
        const auto edits=slabs(source);const auto pending=allocation(source,edits);
        const auto prepared=prepare_component_roof_slab_directional_resize(source,authoring(source,version),roofs(),edits,pending,"Triple original-source replay");
        const auto& wire=prepared.intent();const auto& out=prepared.preview_snapshot().entities();
        require(wire.at("version")==46 && wire.size()==14 && wire.at("component_request").at("version")==version,"closed46 and geometry7/8/9 exact codec values");
        for (const auto& [key,value]:wire.items()) {(void)value;auto bad=wire;bad.erase(key);refuses([&] {(void)validate_mixed_selection_property_edit_intent(bad);},"every46 field mandatory");}
        for (const auto* key:{"expected_revision","source_snapshot_digest","source_authoring_digest","source_entities_digest","source_saved_revision"})
            require(wire.at(key).dump()==wire.at("component_request").at(key).dump(),"all five source fences equal");
        require(wire.at("roof_plan_resizes")[0].at("version")==2 && wire.at("roof_plan_resizes")[0].size()==7 &&
            wire.at("slab_geometry")[0].at("version")== (curved ? 5 : 4),"actual physical codec leaves retained");
        require(exact(out.at("roof"),replay_roof_plan_resize_entities(source.entities(),roofs()).at("roof")),"roof envelope equals exact original-source replay");
        const auto slab_id=prepared.selected_original_to_proposed().at("slab");
        require(slab_id==(slab_baseline ? "slab-copy-slab" : "slab"),"actual baseline roster determines slab identity");
        require(out.at(slab_id).properties.at("elevation_m")==1.25 && out.at(slab_id).properties.at("thickness_m")==.2,"slab Z and thickness remain authored");
        require(out.at("roof").properties.at("quantity_entries").at("/base_position_m/2").dump()==source.entities().at("roof").properties.at("quantity_entries").at("/base_position_m/2").dump(),"roof unchanged receipt raw exact");
        require(out.at(slab_id).extensions.at("vendor")==source.entities().at("slab").extensions.at("vendor"),"slab opaque metadata survives");
        if (slab_baseline) {
            require(exact(out.at("slab"),source.entities().at("slab")) && exact(out.at("host-dim-0"),source.entities().at("host-dim-0")),"baseline owners/dimensions preserved");
            require(!prepared.original_to_proposed().contains("catalog"),"private catalog copies have qualified correspondence only");
            require(prepared.hosted_binding_original_to_proposed().at({"catalog","host-root"})==std::pair<std::string,std::string>{"slab-copy-catalog","slab-copy-host-root"},"qualified hosted binding retained");
            require(prepared.phase_authorings().size()==1,"slab phase proof is reconstructed internally");
            const auto proof=decode_phase_constraint_authoring_intent(prepared.phase_authorings()[0]);
            require(proof.source_entities_digest==entity_map_digest(source.entities()),"slab proof binds complete original source");
            if (curved) require(proof.slab_replacement.at("version")==12,"mapped slab authoring12 independent of roof archive versions");
            const auto proved=replay_phase_constraint_authoring(source.entities(),prepared.phase_authorings()[0]);
            require(exact(proved.at(slab_id),out.at(slab_id)),"internal slab proof independently replays exact physical output");
        } else require(prepared.phase_authorings().empty(),"ordinary slab creates no fabricated phase proof");
        const auto component=prepared.selected_original_to_proposed().at("component");
        require(component==(component_baseline ? "proposed-component" : "component"),"component phase authority independent of physical source");
        const auto dimension_id=component_baseline ? "component-copy-component-dim-0" : "component-dim-0";
        const auto d=decode_boundary_dimension_entity(out.at(dimension_id));
        require_near(resolve_assembly_dimension(out,*d.dimension->assembly_target).span_metres,7.2,"one final dimension batch measures reviewed source profile");
        require(exact(document.snapshot().entities().at("slab"),source.entities().at("slab")) && document.snapshot().history().size()==source.history().size(),"preparation preserves source/history");
        require(replay_mixed_selection_property_edit(source,complete_mixed_selection_property_edit(source,prepared,{},{})).entities==out,"sealed complete replay produces exact final map");
        for (const auto* key:{"base_intent","candidate_entities","phase_authorings","ordinary_request"}) {
            auto bad=wire;bad[key]=Json::object();refuses([&] {(void)validate_mixed_selection_property_edit_intent(bad);},"supplied authority rejected by closed46");
        }
        for (const auto* key:{"roof_plan_resizes","slab_geometry"}) {
            auto bad=wire;bad[key]=Json::array();refuses([&] {(void)validate_mixed_selection_property_edit_intent(bad);},"rosters nonempty before unit removal");
            bad=wire;bad[key].push_back(bad[key][0]);refuses([&] {(void)validate_mixed_selection_property_edit_intent(bad);},"physical operators sorted unique");
        }
        auto bad=wire;bad["component_identity_reservations"]=Json::array({"counterfeit"});
        refuses([&] {(void)replay_mixed_selection_property_edit_preparation(source,bad);},"reservation inventory is independently reconstructed");
        bad=wire;bad["source_entities_digest"]=std::string(64,'0');bad["component_request"]["source_entities_digest"]=std::string(64,'0');
        refuses([&] {(void)replay_mixed_selection_property_edit_preparation(source,bad);},"fabricated matching fences confer no authority");
    }
}
void units_exclusions_and_retirement() {
    auto document=fixture();const auto source=document.snapshot();const auto edit=authoring(source);
    const auto unit=prepare_component_roof_slab_directional_resize(source,edit,roofs(true),slabs(source,true),std::nullopt,"Units retained");
    require(exact(unit.preview_snapshot().entities().at("roof"),source.entities().at("roof")) &&
        exact(unit.preview_snapshot().entities().at("slab"),source.entities().at("slab")),"unit owners remain exact and selected");
    require(unit.selected_original_to_proposed().contains("roof") && unit.selected_original_to_proposed().contains("slab"),"unit selected correspondence remains complete");
    {
        auto rows=source.entities();auto sibling_host=rows.at("slab");sibling_host.id="unit-slab";rows.emplace(sibling_host.id,sibling_host);
        const auto model=AssemblyModel::from_json(rows.at("catalog").properties.at("model"));auto sibling=model.instances().front();
        sibling.id="sibling-root";sibling.placement->host_entity_id="unit-slab";
        rows.at("catalog").properties["model"]["instances"].push_back(AssemblyModel::create(model.materials(),model.types(),{sibling}).to_json().at("instances")[0]);
        const auto original_sibling=rows.at("catalog").properties.at("model").at("instances").back();const auto captured=replaced(source,rows);
        const auto prepared=prepare_component_roof_slab_directional_resize(captured,edit,roofs(),slabs(captured),std::nullopt,"Unselected catalog sibling");
        require(exact(prepared.preview_snapshot().entities().at("unit-slab"),rows.at("unit-slab")),"unselected host envelope exact");
        require(prepared.preview_snapshot().entities().at("catalog").properties.at("model").at("instances").back().dump()==original_sibling.dump(),"unselected shared-catalog row remains raw exact");
        require(!prepared.selected_original_to_proposed().contains("unit-slab"),"containing catalog grants no sibling selection authority");
    }
    for (const auto* key:{"phase_id","transform","vertical_placement","roof_openings"}) {
        auto rows=source.entities();rows.at("roof").properties[key]=nullptr;const auto modified=replaced(source,rows);
        refuses([&] {require_roof_slab_directional_resize_source_admission(modified,targets(),roofs(true),slabs(modified,true));},"44 literal absence restrictions apply to unit roofs");
    }
    for (bool unit_roster:{false,true}) {
        auto rows=source.entities();rows.at("roof").properties["version"]=4;
        refuses([&] {preflight_component_roof_slab_directional_resize(replaced(source,rows),edit,roofs(unit_roster),slabs(source,unit_roster),std::nullopt);},"schema1 whole-roof restriction retained");
        rows=source.entities();rows.emplace("dependency",Entity{"dependency","future_dependency",{{"dependency_ids",Json::array({"roof"})}},false});
        refuses([&] {require_roof_slab_directional_resize_source_admission(replaced(source,rows),targets(),roofs(unit_roster),slabs(source,unit_roster));},"declared roof dependencies excluded including units");
        rows=source.entities();rows.emplace("join",Entity{"join","roof_join",roof_join_json(RoofJoin{"join",{"roof","absent-roof"}}),false});
        refuses([&] {preflight_component_roof_slab_directional_resize(replaced(source,rows),edit,roofs(unit_roster),slabs(source,unit_roster),std::nullopt);},"affected roof joins excluded including units");
        rows=source.entities();const std::vector<std::string> ids{"roof"};
        rows.emplace("phases",Entity{"phases","model_phases",{{"model",ModelPhases::create(ids,ids,{{"alternative","Choice",{}, {}}},"alternative").to_json()}},false});
        refuses([&] {require_roof_slab_directional_resize_source_admission(replaced(source,rows),targets(),roofs(unit_roster),slabs(source,unit_roster));},"whole roof absent from every registry including units");
        rows=source.entities();auto model=AssemblyModel::from_json(rows.at("catalog").properties.at("model"));auto roots=model.instances();
        roots[0].placement->host_entity_id="roof";rows.at("catalog").properties["model"]=AssemblyModel::create(model.materials(),model.types(),roots).to_json();
        refuses([&] {preflight_component_roof_slab_directional_resize(replaced(source,rows),edit,roofs(unit_roster),slabs(source,unit_roster),std::nullopt);},"roof hosted roots excluded including units");
    }
    for (int historical:{39,40,41,42,43,44,45}) {
        auto bad=unit.intent();bad["version"]=historical;
        refuses([&] {(void)validate_mixed_selection_property_edit_intent(bad);},"historical closed dialect cannot borrow46 fields");
    }
    auto bad=unit.intent();bad["room_reviews"].push_back(Json::object());
    refuses([&] {(void)validate_mixed_selection_property_edit_intent(bad);},"46 has no room review authority");
    bad=unit.intent();bad["roof_plan_resizes"][0]["anchor_mode"]="physical_edge";
    refuses([&] {(void)validate_mixed_selection_property_edit_intent(bad);},"units require explicit shared-frame anchor");
    auto scalar=edit;scalar.intents[0].properties.emplace("label",AssemblyInstancePropertyPatch<std::string>{AssemblyInstancePropertyPatchOperation::set,"bad"});
    refuses([&] {preflight_component_roof_slab_directional_resize(source,scalar,roofs(),slabs(source),std::nullopt);},"component scalar authority excluded");
    auto pose=edit;pose.intents[0].pose.x_metres=7;
    refuses([&] {preflight_component_roof_slab_directional_resize(source,pose,roofs(),slabs(source),std::nullopt);},"component pose authority excluded");
    auto wrong=slabs(source);wrong[0].preserve_planar_source_geometry=true;
    refuses([&] {preflight_component_roof_slab_directional_resize(source,edit,roofs(),wrong,std::nullopt);},"planar-source flag is actual-source derived");
    refuses([&] {(void)prepare_component_roof_slab_directional_resize(source,edit,roofs(),slabs(source),ComponentSlabDirectionalResizeAllocation{},"Empty allocation");},"ordinary slab allocation must be null exactly");
    for (bool baseline:{false,true}) {
        auto retiring=fixture(7,baseline,baseline);const auto original=retiring.snapshot();
        const auto prepared=prepare_component_roof_slab_directional_resize(original,authoring(original,7,true),roofs(),slabs(original),allocation(original,slabs(original)),"Retirement-aware single batch");
        require(!prepared.preview_snapshot().entities().contains(baseline ? "component-copy-component-dim-0" : "component-dim-0"),"reviewed retirement survives one finalizer");
        require(prepared.preview_snapshot().entities().contains(baseline ? "slab-copy-host-dim-0" : "host-dim-0"),"same-catalog hosted dimension remains in final batch");
    }
}
void aggregate_admission() {
    auto document=fixture();const auto original=document.snapshot();const auto edit=authoring(original);
    require_roof_slab_directional_resize_source_admission(original,targets(),roofs(),slabs(original));
    preflight_component_roof_slab_directional_resize(original,edit,roofs(),slabs(original),std::nullopt);
    auto rows=original.entities();
    for (int i=0;i<16;++i) {
        rows.at("roof").properties["vendor"]["payload-"+std::to_string(i)]=std::string(14000,'r');
        rows.at("slab").properties["vendor"]["payload-"+std::to_string(i)]=std::string(4000,'s');
    }
    const auto large=replaced(original,rows);
    const auto old_roof=prepare_component_roof_directional_resize(large,edit,roofs(),"Individually bounded44");
    const auto old_slab=prepare_component_slab_directional_resize(large,edit,slabs(large),std::nullopt,"Individually bounded39");
    require(old_roof.intent().at("version")==44 && old_slab.intent().at("version")==39,"both old lanes independently admit original source");
    budget_refuses([&] {require_roof_slab_directional_resize_source_admission(large,targets(),roofs(),slabs(large));},"aggregate capture byte budget");
    budget_refuses([&] {preflight_component_roof_slab_directional_resize(large,edit,roofs(),slabs(large),std::nullopt);},"aggregate capture byte budget");
    rows.at("roof").properties["thickness_m"]=-1;const auto blocked=replaced(original,rows);
    budget_refuses([&] {(void)prepare_component_roof_slab_directional_resize(blocked,edit,roofs(),slabs(blocked),std::nullopt,"Budget before native");},"aggregate capture byte budget");
    auto sparse=fixture(7,false,false,false,0,0);const auto sparse_source=sparse.snapshot();const auto sparse_edit=authoring(sparse_source);
    rows=sparse_source.entities();
    for (int i=0;i<501;++i) {auto d=dimension("many-component-"+std::to_string(i),false);rows.emplace(d.id,d);}
    for (int i=0;i<500;++i) {auto d=dimension("many-host-"+std::to_string(i),true);rows.emplace(d.id,d);}
    rows.at("roof").properties["thickness_m"]=-1;const auto associations=replaced(sparse_source,rows);
    budget_refuses([&] {require_roof_slab_directional_resize_source_admission(associations,targets(),roofs(true),slabs(associations,true));},"combined dimension association budget");
    budget_refuses([&] {preflight_component_roof_slab_directional_resize(associations,sparse_edit,roofs(true),slabs(associations,true),std::nullopt);},"combined dimension association budget");
    ComponentSlabDirectionalResizeAllocation oversize;
    for (int i=0;i<4097;++i) oversize.identities.emplace("old-"+std::to_string(i),"fresh-"+std::to_string(i));
    budget_refuses([&] {(void)prepare_component_roof_slab_directional_resize(blocked,edit,roofs(),slabs(blocked),oversize,"Actual allocation first");},"aggregate fresh identity budget");
    oversize.identities.clear();
    for (int i=0;i<4000;++i) {
        auto old="old-"+std::to_string(i),fresh="fresh-"+std::to_string(i);old.append(128-old.size(),'o');fresh.append(128-fresh.size(),'f');
        oversize.identities.emplace(std::move(old),std::move(fresh));
    }
    budget_refuses([&] {preflight_component_roof_slab_directional_resize(blocked,edit,roofs(),slabs(blocked),oversize);},"aggregate capture byte budget");
    // Same shared catalog, several qualified hosted roots: every potential
    // private baseline copy is charged independently, including units.
    rows=sparse_source.entities();
    auto model=AssemblyModel::from_json(rows.at("catalog").properties.at("model"));
    auto root=model.instances().front();root.id="host-two";
    rows.at("catalog").properties["model"]["instances"].push_back(AssemblyModel::create({},model.types(),{root}).to_json().at("instances")[0]);
    for (int i=0;i<18;++i) rows.at("catalog").properties["vendor"]["pending-"+std::to_string(i)]=std::string(6000,'c');
    const auto pending=replaced(sparse_source,rows);
    budget_refuses([&] {require_roof_slab_directional_resize_source_admission(pending,targets(),roofs(true),slabs(pending,true));},"aggregate capture byte budget");
    // Repeated paths are real pending override work, even with shared types.
    rows=sparse_source.entities();model=AssemblyModel::from_json(rows.at("catalog").properties.at("model"));
    auto body=model.types().front();AssemblyType parent;parent.id="parent";parent.name="Repeated DAG paths";
    for (int i=0;i<90;++i) {AssemblyPart part;part.id="part-"+std::to_string(i);part.type_id=body.id;parent.parts.push_back(part);}
    root=model.instances().front();root.type_id=parent.id;
    rows.at("catalog").properties["model"]=AssemblyModel::create({}, {body,parent},{root}).to_json();
    const auto dag=replaced(sparse_source,rows);
    // Ninety small repeated paths remain below the aggregate byte ceiling.
    // Keep that admitted control, then cross the ceiling with actual extra work.
    require_roof_slab_directional_resize_source_admission(dag,targets(),roofs(true),slabs(dag,true));
    for (int i=90;i<180;++i) {AssemblyPart part;part.id="part-"+std::to_string(i);part.type_id=body.id;parent.parts.push_back(part);}
    rows.at("catalog").properties["model"]=AssemblyModel::create({}, {body,parent},{root}).to_json();
    const auto oversized_dag=replaced(sparse_source,rows);
    budget_refuses([&] {require_roof_slab_directional_resize_source_admission(oversized_dag,targets(),roofs(true),slabs(oversized_dag,true));},"aggregate capture byte budget");
    // Legacy hosted roots have no profile roster; native replay will borrow
    // slab rings. The pure gate must count that materialization per root.
    rows=sparse_source.entities();body.profiles.clear();std::vector<AssemblyInstance> borrowed;
    for (int i=0;i<100;++i) {root=model.instances().front();root.id="borrowed-"+std::to_string(i);borrowed.push_back(root);}
    rows.emplace("legacy",Entity{"legacy","assembly_model",{{"model",AssemblyModel::create({}, {body},borrowed).to_json()}},false});
    const auto legacy=replaced(sparse_source,rows);
    budget_refuses([&] {require_roof_slab_directional_resize_source_admission(legacy,targets(),roofs(true),slabs(legacy,true));},"aggregate capture byte budget");
    auto invalid_target=targets();invalid_target[0].operation.world_axis_rotation_radians.reset();
    refuses([&] {require_roof_slab_directional_resize_source_admission(original,invalid_target,roofs(),slabs(original));},"source gate needs explicit actual component frame");
    auto too_many=slabs(original,true);for (int i=0;i<998;++i) {auto item=too_many.front();item.slab_id="slab-"+std::to_string(i);too_many.push_back(item);}
    budget_refuses([&] {require_roof_slab_directional_resize_source_admission(original,targets(),roofs(true),too_many);},"aggregate selection bound");
    rows=sparse_source.entities();
    for (int i=0;rows.size()<65537;++i) {const auto id="source-"+std::to_string(i);rows.emplace(id,Entity{id,"label",{{"text","bounded source"}},false});}
    const auto source_ceiling=replaced(sparse_source,rows);
    budget_refuses([&] {require_roof_slab_directional_resize_source_admission(source_ceiling,targets(),roofs(true),slabs(source_ceiling,true));},"source entity bound");
}
void mapped_receipts_pitched_roofs_and_freshness() {
    for (int phase:{0,1,2}) {
        auto seed=fixture(7,false,phase==1);auto rows=seed.snapshot().entities();
        // Canonical source map; changing only it leaves stored endpoints equal.
        auto mapped=slabs(seed.snapshot());mapped[0].preserve_planar_source_geometry=true;
        auto curved_rows=rows;curved_rows.at("slab").properties["boundary"]=raw_ring(ring(true));
        rows.at("slab")=replay_slab_geometry_entity(curved_rows.at("slab"),mapped[0]);
        rows.at("slab").properties["quantity_entries"]["/boundary/0/start/0"]=receipt(0);
        if (phase==2) {
            const std::vector<std::string> ids{"slab","catalog","host-dim-0"};
            rows.emplace("phases",Entity{"phases","model_phases",{{"model",ModelPhases::create(ids,{},{{"alternative","Proposed",{},ids}},"alternative").to_json()}},false});
        }
        const auto source=replaced(seed.snapshot(),rows);const auto edits=slabs(source);const auto pending=allocation(source,edits);
        const auto prepared=prepare_component_roof_slab_directional_resize(source,authoring(source),roofs(),edits,pending,"Map-only receipt retirement");
        const auto id=prepared.selected_original_to_proposed().at("slab");const auto& slab=prepared.preview_snapshot().entities().at(id);
        require(slab.properties.at("boundary").dump()==source.entities().at("slab").properties.at("boundary").dump(),"map-only resize keeps exact stored endpoints");
        require(!slab.properties.at("quantity_entries").contains("/boundary/0/start/0"),"map-only change retires ring receipts in ordinary/baseline/proposed cohorts");
        const auto& archived=slab.extensions.at(std::string(slab_geometry_derivations_key)).at("operations").back().at("receipts").at("quantity_entries");
        require(archived.at("/boundary/0/start/0").dump()==receipt(0).dump(),"map-only retired receipt remains raw exact in archive");
        if (phase==1) require(exact(prepared.preview_snapshot().entities().at("slab"),source.entities().at("slab")),"map-only baseline original unchanged");
    }
    for (double yaw:{0.,.37}) {
        auto seed=fixture();auto rows=seed.snapshot().entities();rows.at("roof").properties["rise_m"]=1.;
        rows.at("roof").properties["pitch_rad"]=std::atan(.5);rows.at("roof").properties["orientation_rad"]=yaw;
        const auto source=replaced(seed.snapshot(),rows);auto ops=roofs();ops[0].frame_rotation_radians=yaw;
        const auto prepared=prepare_component_roof_slab_directional_resize(source,authoring(source),ops,slabs(source),std::nullopt,"Pitched yawed original roof");
        require(exact(prepared.preview_snapshot().entities().at("roof"),replay_roof_plan_resize_entities(source.entities(),ops).at("roof")),"pitched yawed roof final envelope equals original native replay");
        require(prepared.preview_snapshot().entities().at("roof").properties.at("rise_m")==1.,"pitched roof rise unchanged");
    }
    auto document=fixture(7,true,true);const auto source=document.snapshot();const auto edit=authoring(source);auto pending=allocation(source,slabs(source));
    pending->dimension_identities["host-dim-0"]="component-copy-component-dim-0";
    refuses([&] {preflight_component_roof_slab_directional_resize(source,edit,roofs(),slabs(source),pending);},"allocation collision precedes component preparation");
    pending=allocation(source,slabs(source));pending->identities["slab"]="historical-only";
    document.apply(ApplyEntityChanges{.expected_revision=document.revision(),.entity_changes={EntityChange::upsert({"historical-only","label",{{"text","retained"}},false})},.message="Reserve history"});
    document.undo(document.revision());const auto retained=document.snapshot();
    refuses([&] {preflight_component_roof_slab_directional_resize(retained,edit,roofs(),slabs(retained),pending);},"retained undone entity collision admitted before manufacture");
    pending=allocation(source,slabs(source));const auto occupied=sketch::test::DetachedDocumentSnapshotFixture::mutate(source,[&](auto& state) {
        state.assets().emplace("slab-copy-slab",Asset::create("slab-copy-slab","application/octet-stream",{std::byte{1}}));
    });
    refuses([&] {preflight_component_roof_slab_directional_resize(occupied,edit,roofs(),slabs(occupied),pending);},"actual asset namespace participates in early admission");
    const auto prepared=prepare_component_roof_slab_directional_resize(source,edit,roofs(),slabs(source),pending,"Original source fences");
    refuses([&] {require_mixed_selection_property_edit_source_current(retained,prepared);},"same current geometry with different retained history stales46");
    const auto& raw=source.entities().at("catalog").properties.at("model");auto one=raw,two=raw;
    one["instances"][0]["placement"]["translation_m"]={2.,0.};two["instances"][0]["placement"]["translation_m"]={3.,0.};
    refuses([&] {(void)merge_disjoint_hosted_assembly_models(raw,one,two);},"conflicting source row contributions refuse");
}
std::array<double,6> bounds(const TopoDS_Shape& shape) {
    require(!shape.IsNull() && solid_volume(shape)>0,"genuine positive-volume solid required");Bnd_Box box;BRepBndLib::Add(shape,box);
    std::array<double,6> result;box.Get(result[0],result[1],result[2],result[3],result[4],result[5]);return result;
}
TopoDS_Shape slab_shape(const Entity& entity) {
    Slab decoded;std::string error;require(read_document_slab(entity,decoded,error),"decode actual native slab");return make_slab(decoded);
}
void flat_native_hand_geometry() {
    auto seed=fixture(7,false,false,false,0,0);auto rows=seed.snapshot().entities();
    AssemblyProfile p;p.id="profile";p.outer={{{0,0},{2,0},0},{{2,0},{2,1},0},{{2,1},{0,1},0},{{0,1},{0,0},0}};
    p.elevation_m=0;p.height_m=1.5;p.topology=topology("");AssemblyType body;body.id="body";body.name="Hand body";body.profiles={p};
    rows.at("catalog").properties["model"]=AssemblyModel::create({}, {body},{}).to_json();
    auto instance=decode_document_assembly_instance(rows.at("component"));instance.instance.root_transform=AssemblyTransform{{5,1,0},0,1,false};
    rows.at("component")=encode_document_assembly_instance(rows.at("component"),instance);
    rows.at("slab").properties["boundary"]=raw_ring({{{1,0},{4,0},0},{{4,0},{4,3},0},{{4,3},{1,3},0},{{1,3},{1,0},0}});
    rows.at("slab").properties.erase("quantity_entries");const auto source=replaced(seed.snapshot(),rows);
    AssemblyExpansionBudget source_budget;const auto expansion=expand_document_assembly_instance(rows.at("component"),rows,source_budget);
    const auto original_component=make_assembly_geometry(expansion).shape;
    require_near(solid_volume(original_component),3,"hand source component volume3");
    require_near(solid_volume(make_roof_structure_shape(decode_roof_entity(rows.at("roof")))),1,"hand source roof volume1");
    require_near(solid_volume(slab_shape(rows.at("slab"))),1.8,"hand source slab volume1.8");
    const auto model=AssemblyModel::from_json(rows.at("catalog").properties.at("model"));
    for (bool x:{true,false}) for (bool opposite:{false,true}) {
        const Vec2 anchor{x ? (opposite ? 7. : 1.) : 1.,x ? 0. : (opposite ? 3. : 0.)};
        const double sx=x ? 1.5 : 1.,sy=x ? 1. : 1.5;
        auto whole=roofs();whole[0].scale_x=sx;whole[0].scale_y=sy;whole[0].anchor=anchor;
        auto physical=slabs(source);physical[0].resize=SlabPlanAxisResize{sx,sy,anchor,0};
        std::vector<AssemblyDirectionalResizeTarget> actual_targets{{"component",{anchor,sx,sy,0,std::nullopt}}};
        require_roof_slab_directional_resize_source_admission(source,actual_targets,whole,physical);
        AssemblyExpansionBudget available;const auto draft=draft_assembly_directional_resize(model,expansion,actual_targets[0].operation,available);
        auto intent=authoring(source);auto profiles=draft.nodes.front().candidate_profiles;
        // The reviewed redraw keeps the manufactured affine geometry but must
        // assign fresh topology, just like the ordinary authoring helper.
        profiles.front().topology=topology("review-");
        intent.intents[0].profile_geometry.value=std::move(profiles);
        const auto prepared=prepare_component_roof_slab_directional_resize(source,intent,whole,physical,std::nullopt,"Native hand affine");
        const auto& out=prepared.preview_snapshot().entities();AssemblyExpansionBudget budget;
        const std::array<TopoDS_Shape,3> shapes{make_assembly_geometry(expand_document_assembly_instance(out.at("component"),out,budget)).shape,
            make_roof_structure_shape(decode_roof_entity(out.at("roof"))),slab_shape(out.at("slab"))};
        const std::array<std::array<double,6>,3> before{{{5,1,0,7,2,1.5},{1,0,3.75,3,2,4},{1,0,1.25,4,3,1.45}}};
        const std::array<double,3> volumes{4.5,1.5,2.7};
        for (std::size_t i=0;i<3;++i) {
            const auto observed=bounds(shapes[i]);auto expected=before[i];
            expected[0]=anchor.x+sx*(expected[0]-anchor.x);expected[3]=anchor.x+sx*(expected[3]-anchor.x);
            expected[1]=anchor.y+sy*(expected[1]-anchor.y);expected[4]=anchor.y+sy*(expected[4]-anchor.y);
            for (std::size_t j=0;j<6;++j) require(std::abs(observed[j]-expected[j])<1e-6,"native hand bounds on both axes/anchors retain Z");
            require_near(solid_volume(shapes[i]),volumes[i],"native hand output volumes4.5/1.5/2.7");
        }
    }
}
struct ProjectFile {
    std::filesystem::path directory,path;
    ProjectFile() {
        const auto stamp=std::chrono::steady_clock::now().time_since_epoch().count();
        for (unsigned i=0;i<64;++i) {
            directory=std::filesystem::temp_directory_path()/("vertex-component-roof-slab-"+std::to_string(stamp)+"-"+std::to_string(i));
            if (std::filesystem::create_directory(directory)) {path=directory/"history.bldproj";return;}
        }
        throw std::runtime_error("unique triple storage directory unavailable");
    }
    ~ProjectFile() {std::error_code error;std::filesystem::remove_all(directory,error);}
};
void old_reader_refusal(const std::filesystem::path& path) {
    const auto u8=path.u8string();const std::string name(reinterpret_cast<const char*>(u8.data()),u8.size());sqlite3* raw=nullptr;
    const auto opened=sqlite3_open_v2(name.c_str(),&raw,SQLITE_OPEN_READWRITE,nullptr);
    std::unique_ptr<sqlite3,decltype(&sqlite3_close)> db(raw,&sqlite3_close);require(opened==SQLITE_OK,"controlled storage file opens for marker change");
    require(sqlite3_exec(db.get(),"BEGIN; PRAGMA user_version=268; UPDATE metadata SET value='268' WHERE key='format_version'; COMMIT",nullptr,nullptr,nullptr)==SQLITE_OK,"downgrade only reader marker pair");db.reset();
    const auto digest=ProjectStore::file_sha256(path);bool refused=false;
    try {(void)ProjectStore::load(path);} catch (const StorageError& error) {
        require(error.code()==StorageErrorCode::unsupported_format && std::string(error.what()).find("requires reader 269")!=std::string::npos,"reader268 refuses actual retained46 at reader269");refused=true;
    }
    require(refused && ProjectStore::file_sha256(path)==digest,"old-reader refusal preserves all file bytes");
}
void persisted_history_and_controls() {
    for (bool baseline:{false,true}) for (bool earlier45:{false,true}) {
        auto seed=fixture(7,baseline,baseline);auto original_rows=seed.snapshot().entities();
        auto independent=decode_document_assembly_instance(original_rows.at("component"));independent.instance.id="earlier-component";
        original_rows.emplace("earlier-component",encode_document_assembly_instance({"earlier-component","assembly_instance",context},independent));
        auto host=encode_building_entity(SlopedRoofPanel{"host",{-10,-10,3},0,40,40,0,0,0,.2,
            {{"child",11,10.5,2,1,RoofSkylight{.06,.15,.024},0}}});host.properties["version"]=4;
        for (const auto& [key,value]:context.items()) host.properties[key]=value;original_rows.emplace("host",host);
        std::vector<Entity> values;for (const auto& [id,entity]:original_rows) {(void)id;values.push_back(entity);}
        auto document=Document::create(values);const auto initial=document.snapshot();
        auto early_edit=authoring(initial);early_edit.intents[0].target.instance_id="earlier-component";
        early_edit.intents[0].target.document_entity_id="earlier-component";early_edit.profile_dimension_decisions.clear();
        early_edit.dimension_identities.clear();early_edit.allocation={};
        const std::vector<RoofOpeningDirectionalResizeRequest> children{{{"host","child"},1,1,{1,0},0}};
        const auto earlier=earlier45 ? prepare_component_roof_skylight_directional_resize(initial,early_edit,roofs(true),children,"Earlier45") :
            prepare_component_roof_directional_resize(initial,early_edit,roofs(true),"Earlier44");
        document.apply(command(initial,earlier));const auto source=document.snapshot();
        require(ProjectStore::required_format_version(source)==(earlier45 ? 268 : 267),"earlier actual44/45 floor retained");
        auto edit=authoring(source);auto p=*edit.intents[0].profile_geometry.value;
        p[0].planar_affine=AssemblyProfilePlanarAffine{{0,0},{0,0},2.,0,0,.65};edit.intents[0].profile_geometry.value=p;
        const auto prepared=prepare_component_roof_slab_directional_resize(source,edit,roofs(),slabs(source),allocation(source,slabs(source)),"Later46");
        const auto operation=command(source,prepared);const auto wire=command_to_json(operation);
        require(wire.at("version")==52 && wire.at("mixed_selection_edit_intent").at("version")==46,"canonical command52/Properties46");
        document.apply(operation);const auto applied=document.snapshot();document.undo(document.revision());const auto undone=document.snapshot();
        require(undone.entities()==source.entities(),"one Undo restores all three owners");document.redo(document.revision());
        require(document.snapshot().entities()==applied.entities(),"one Redo restores exact triple output");
        for (const auto& snapshot:{applied,undone,document.snapshot()}) {
            require(ProjectStore::required_format_version(snapshot)==269 && ProjectStore::required_semantic_format_version(snapshot)==269,"earlier44/archive4 cannot hide later applied or undone46");
            ProjectFile file;(void)ProjectStore::save(file.path,snapshot);auto loaded=ProjectStore::load(file.path);const auto reopened=loaded.document.snapshot();
            require(reopened.entities()==snapshot.entities() && reopened.assets()==snapshot.assets() && reopened.history().size()==snapshot.history().size(),"fresh editable Open preserves graph/assets/history");
            require(command_to_json(*reopened.history().at(applied.revision()).boundary_constraint_changes)==wire,"fresh Open retains exact canonical52/46 history");
            extract_project(snapshot,file.directory/"extracted");std::ifstream input(file.directory/"extracted"/"project.json");const auto manifest=Json::parse(input);
            require(manifest.at("exchange_version")==267 && manifest.at("revisions").at(applied.revision()).at("boundary_constraint_changes")==wire,"extraction267 retains exact46 proof");
            auto property=reopened.entities().at("property");property.properties["name"]="Fresh editable Open";
            loaded.document.apply(ApplyEntityChanges{.expected_revision=loaded.document.revision(),.entity_changes={EntityChange::upsert(property)},.message="New Undo session"});
            loaded.document.undo(loaded.document.revision());require(loaded.document.snapshot().entities()==reopened.entities(),"fresh Open has a new working Undo session");
            old_reader_refusal(file.path);
        }
    }
    auto document=fixture();const auto source=document.snapshot();const auto prepared=prepare_component_roof_slab_directional_resize(source,authoring(source),roofs(),slabs(source),std::nullopt,"Opaque control");
    auto property=source.entities().at("property");property.properties["opaque_intent"]=prepared.intent();property.extensions=command_to_json(command(source,prepared));
    document.apply(ApplyEntityChanges{.expected_revision=document.revision(),.entity_changes={EntityChange::upsert(property)},.message="Opaque46 lookalike"});
    require(ProjectStore::required_format_version(document.snapshot())==ProjectStore::required_format_version(source),"opaque46 metadata adds no authority");
    const auto archived=replay_roof_plan_resize_entities(source.entities(),roofs());std::vector<Entity> rows;
    for (const auto& [id,entity]:archived) {(void)id;rows.push_back(entity);}const auto archive_only=Document::create(rows).snapshot();
    require(ProjectStore::required_format_version(archive_only)==267,"archive4-only retains reader267");
    ProjectFile file;extract_project(archive_only,file.directory/"extracted");std::ifstream input(file.directory/"extracted"/"project.json");
    require(Json::parse(input).at("exchange_version")==265,"archive4-only retains extraction265");
}
} // namespace
int main(int argc,char** argv) {
    sketch::testing::noninteractive_errors();
    try {
        const std::string_view only=argc>1 ? argv[1] : "";
        if (only.empty() || only=="--core") {closed_wire_and_original_contributions();units_exclusions_and_retirement();mapped_receipts_pitched_roofs_and_freshness();}
        if (only.empty() || only=="--budget") aggregate_admission();
        if (only.empty() || only=="--native") flat_native_hand_geometry();
        if (only.empty() || only=="--storage") persisted_history_and_controls();
        std::cout<<"Mixed component/roof/slab resize contracts passed\n";return 0;
    } catch (const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
