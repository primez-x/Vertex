#include "sketch/mixed_selection_property_edit.hpp"
#include "sketch/assembly_dimension.hpp"
#include "sketch/assembly_model.hpp"
#include "sketch/boundary_dimension.hpp"
#include "sketch/building_entity.hpp"
#include "sketch/document_digest.hpp"
#include "sketch/model_phases.hpp"
#include "sketch/phase_constraint_authoring.hpp"
#include "sketch/project_exchange.hpp"
#include "sketch/project_store.hpp"
#include "sketch/roof_entity_codec.hpp"
#include "sketch/roof_join_semantics.hpp"
#include "sketch/plan_axis_resize.hpp"
#include "sketch/architecture.hpp"
#include "sketch/assembly_geometry.hpp"
#include "sketch/assembly_directional_resize.hpp"
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
void require(bool value,const char* message) { if (!value) throw std::runtime_error(message); }
void require_near(double observed,double expected,const char* message) {
    if (!std::isfinite(observed) || std::abs(observed-expected)>1e-8) throw std::runtime_error(message);
}
template<class F> void refuses(F&& action,const char* message) {
    try { action(); } catch (const std::invalid_argument&) { return; }
    throw std::runtime_error(message);
}
template<class F> void budget_refuses(F&& action,const char* diagnostic) {
    try { action(); } catch (const std::invalid_argument& error) {
        require(std::string(error.what()).find(diagnostic)!=std::string::npos,"aggregate admission precedes unmanufacturable geometry");return;
    }
    throw std::runtime_error("aggregate resource bound did not refuse");
}
bool exact(const Entity& a,const Entity& b) {
    return a==b && a.properties.dump()==b.properties.dump() && a.extensions.dump()==b.extensions.dump();
}
const Json context={{"property_id","property"},{"building_id","building"},{"floor_id","floor"},{"layer_id","layer"}};
AssemblyProfileTopology topology(const std::string& prefix) {
    return {{prefix+"outer",{prefix+"e0",prefix+"e1",prefix+"e2",prefix+"e3"},
        {prefix+"v0",prefix+"v1",prefix+"v2",prefix+"v3"}},{}};
}
AssemblyProfileDimensionTarget selector(const std::string& prefix) {
    return {"profile",prefix+"outer",AssemblyProfileDimensionMeasurement::edge_length,prefix+"e0",std::nullopt,std::nullopt};
}
Json receipt(int metres,bool child=false) {
    Json value{{"version",1},{"original_expression",std::to_string(metres)+" m"},
        {"exact_metres",{{"numerator",metres},{"denominator",1},{"vendor","rational sibling"}}},
        {"vendor",{{"retain",Json::array({17,"raw",true})}}}};
    value[child ? "default_unit" : "entered_unit"]="m";return value;
}
Entity dimension(std::string id,bool hosted,bool profile=false) {
    BoundaryDimension d;d.id=std::move(id);d.boundary_id=hosted ? "catalog" : "component";
    d.kind=profile ? BoundaryDimensionKind::assembly_profile : BoundaryDimensionKind::assembly_extent;
    d.text_position={7,-2};AssemblyDimensionTarget target;
    target.component={"catalog",hosted ? "host-root" : "component",hosted ? std::nullopt : std::optional<std::string>{"component"},{}};
    target.axis=AssemblyDimensionAxis::width;if (profile) target.profile=selector("");
    d.assembly_target=target;return encode_boundary_dimension_entity(d);
}
Document fixture(int version=7,bool baseline=false,std::size_t component_dimensions=1,std::size_t host_dimensions=1) {
    AssemblyProfile p;p.id="profile";p.outer={{{0,0},{4,0},0},{{4,0},{4,3},0},{{4,3},{0,3},0},{{0,3},{0,0},0}};
    p.elevation_m=.4;p.height_m=.6;if (version==7) p.topology=topology("");
    AssemblyType body;body.id="body";body.name="Actual source body";body.profiles={p};
    AssemblyInstance root;root.id="host-root";root.type_id="body";
    root.placement=AssemblyPlacement{"host",{1,0},0,1};
    AssemblyInstance independent;independent.id="component";independent.type_id="body";
    independent.root_transform=AssemblyTransform{{5,1,.75},.29,1,true};
    auto whole=encode_building_entity(SlopedRoofPanel{.id="whole",.base_position={1,0,4},.run=2,.span=2,.rise=0,.pitch_radians=0,.overhang=0,.thickness=.25});
    auto host=encode_building_entity(SlopedRoofPanel{"host",{-10,-10,3},0,40,40,0,0,0,.2,
        {{"child",11,10.5,2,1,RoofSkylight{.06,.15,.024},0},
         {"sibling",22,18,2,1,RoofSkylight{.06,.15,.024},0}}});host.properties["version"]=4;
    for (auto* entity:{&whole,&host}) {
        for (const auto& [key,value]:context.items()) entity->properties[key]=value;
        entity->properties["vendor"]={{"retain",Json::array({17,"raw",true})},{"numeric",7}};
        entity->extensions={{"vendor",{{"retain","envelope"}}}};
    }
    whole.properties["quantity_entries"]={{"/run_m",receipt(2)},{"/base_position_m/2",receipt(4)},
        {"/vendor/numeric",{{"version",81},{"opaque",true}}}};
    host.extensions["roof_opening_input"]={{"version",1},{"entries",{
        {"child",{{"x_m",receipt(11,true)},{"width_m",receipt(2,true)},{"depth_m",receipt(1,true)}}},
        {"sibling",{{"width_m",receipt(2,true)}}}}}};
    std::vector<Entity> rows{{"property","property",{{"name","Triple source"}},false},
        {"building","building",{{"property_id","property"}},false},{"floor","floor",{{"building_id","building"}},false},
        {"layer","layer",{{"floor_id","floor"}},false},
        {"catalog","assembly_model",{{"model",AssemblyModel::create({}, {body},{root}).to_json()},{"vendor",{{"retain","shared catalog"}}}},false},
        encode_document_assembly_instance({"component","assembly_instance",context},{"catalog",independent}),whole,host};
    for (std::size_t i=0;i<component_dimensions;++i) rows.push_back(dimension("component-dim-"+std::to_string(i),false,version==7));
    for (std::size_t i=0;i<host_dimensions;++i) rows.push_back(dimension("host-dim-"+std::to_string(i),true));
    if (baseline) {
        std::vector<std::string> ids{"component"};for (std::size_t i=0;i<component_dimensions;++i) ids.push_back("component-dim-"+std::to_string(i));
        rows.push_back({"phases","model_phases",{{"model",ModelPhases::create(ids,ids,{{"alternative","Choice",{}, {}}},"alternative").to_json()}},false});
    }
    return Document::create(rows);
}
DocumentSnapshot replaced(const DocumentSnapshot& source,const Entities& entities) {
    return sketch::test::DetachedDocumentSnapshotFixture::mutate(source,[&](auto& state) { state.entities()=entities; });
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
        (void)target;result.dimension_identities.emplace(id,"proposed-"+id);
    }
    return result;
}
std::vector<RoofPlanResizeIntent> roofs(bool unit=false) {
    return {{"whole",unit ? 1. : 1.5,1,{1,0},0,PlanAxisResizeAnchorMode::shared_frame}};
}
std::vector<RoofOpeningDirectionalResizeRequest> children(bool unit=false) {
    return {{{"host","child"},unit ? 1. : 1.5,1,{1,0},0}};
}
std::vector<AssemblyDirectionalResizeTarget> targets() {
    return {{"component",{{1,0},1.5,1,0,std::nullopt}}};
}
void closed_wire_and_original_map() {
    for (int version:{7,8,9}) for (bool baseline:{false,true}) for (bool unit:{false,true}) {
        auto document=fixture(version,baseline);const auto source=document.snapshot();const auto edit=authoring(source,version);
        const auto prepared=prepare_component_roof_skylight_directional_resize(source,edit,roofs(unit),children(unit),"Triple resize");
        const auto& wire=prepared.intent();require(wire.at("version")==45 && wire.size()==13,"Properties45 has exactly thirteen fields");
        for (const auto& [field,value]:wire.items()) {
            (void)value;auto missing=wire;missing.erase(field);
            refuses([&] {(void)validate_mixed_selection_property_edit_intent(missing);},"every closed45 field is mandatory");
        }
        require(wire.at("component_request").at("version")==version,"component geometry dialect retained");
        for (const auto* key:{"expected_revision","source_snapshot_digest","source_authoring_digest","source_entities_digest","source_saved_revision"})
            require(wire.at(key).dump()==wire.at("component_request").at(key).dump(),"all five original fences retained");
        require(wire.at("roof_plan_resizes")[0].size()==7 && wire.at("roof_plan_resizes")[0].at("version")==2 &&
            wire.at("roof_opening_axis_requests")[0].size()==6,"physical operators stay closed and qualified");
        const auto whole=replay_roof_plan_resize_entities(source.entities(),roofs(unit));
        const auto child=prepare_roof_opening_directional_resizes(source.entities(),children(unit));
        const auto& out=prepared.preview_snapshot().entities();
        require(exact(out.at("whole"),whole.at("whole")),"whole final envelope is actual original-map replay");
        require(exact(out.at("host"),child.candidate_entities.at("host")),"child host exact original-map replay");
        require(out.at("whole").properties.at("quantity_entries").at("/base_position_m/2").dump()==source.entities().at("whole").properties.at("quantity_entries").at("/base_position_m/2").dump() &&
            out.at("whole").properties.at("quantity_entries").at("/vendor/numeric").dump()==source.entities().at("whole").properties.at("quantity_entries").at("/vendor/numeric").dump(),"unchanged authored Z and opaque receipts raw exact");
        require(out.at("host").extensions.at("roof_opening_input").at("entries").at("sibling").dump()==source.entities().at("host").extensions.at("roof_opening_input").at("entries").at("sibling").dump(),"unselected sibling receipt remains raw exact");
        require(out.at("host").properties.at("roof_openings")[1].dump()==source.entities().at("host").properties.at("roof_openings")[1].dump(),"unselected sibling raw envelope retained");
        require(prepared.selected_original_to_proposed().at("whole")=="whole" && prepared.roof_child_original_to_proposed().contains({"host","child"}),"two independent correspondence inventories");
        require(prepared.phase_authorings().size()==1 && prepared.phase_authorings()[0].at("version")==32,"internally reconstructed retained phase32 proof");
        const auto proof=replay_phase_constraint_authoring(source.entities(),prepared.phase_authorings()[0]);
        require(exact(proof.at("host"),out.at("host")),"retained phase32 independently proves exact qualified child output");
        require(source.entities()==document.snapshot().entities() && source.history().size()==document.snapshot().history().size(),"preparation preserves source history");
        const auto component_id=baseline ? "proposed-component" : "component";
        const auto d=decode_boundary_dimension_entity(out.at(baseline ? "proposed-component-dim-0" : "component-dim-0"));
        require(d.dimension->assembly_target->component.document_entity_id==component_id,"one final batch uses correct component destination");
        require_near(resolve_assembly_dimension(out,*d.dimension->assembly_target).span_metres,7.2,"independent profile/extent width after reviewed map");
        const auto hosted=decode_boundary_dimension_entity(out.at("host-dim-0"));
        require_near(resolve_assembly_dimension(out,*hosted.dimension->assembly_target).span_metres,4,"same-catalog hosted root unchanged in final batch");
        if (baseline) require(exact(out.at("component"),source.entities().at("component")) && exact(out.at("component-dim-0"),source.entities().at("component-dim-0")),"baseline originals remain raw exact");
        for (const auto* field:{"base_intent","candidate_entities","phase_authorings","ordinary_request"}) {
            auto bad=wire;bad[field]=Json::object();refuses([&] {(void)validate_mixed_selection_property_edit_intent(bad);},"closed45 rejects supplied authority");
        }
        for (const auto* field:{"roof_plan_resizes","roof_opening_axis_requests"}) {
            auto bad=wire;bad[field]=Json::array();refuses([&] {(void)validate_mixed_selection_property_edit_intent(bad);},"both physical rosters required before unit elimination");
            bad=wire;bad[field].push_back(bad[field][0]);refuses([&] {(void)validate_mixed_selection_property_edit_intent(bad);},"duplicate operator refuses");
        }
        auto bad=wire;bad["roof_plan_resizes"][0]["anchor_mode"]="physical_edge";
        refuses([&] {(void)validate_mixed_selection_property_edit_intent(bad);},"unit and changed rows require explicit shared frame");
        bad=wire;bad["roof_plan_resizes"][0]["roof_id"]="host";
        refuses([&] {(void)replay_mixed_selection_property_edit_preparation(source,bad);},"same host is refused before units");
    }
}
void literal_phase_absence_and_retirement() {
    auto document=fixture();const auto source=document.snapshot();const auto edit=authoring(source);
    for (const Json value:{Json(nullptr),Json("phase")}) {
        auto rows=source.entities();rows.at("host").properties["phase_id"]=value;const auto modified=replaced(source,rows);
        refuses([&] {require_roof_skylight_directional_resize_source_admission(modified,targets(),roofs(true),children(true));},"45 source gate excludes explicit child-host phase null");
        refuses([&] {(void)prepare_component_roof_skylight_directional_resize(modified,edit,roofs(true),children(true),"Phase null");},"45 full gate excludes explicit child-host phase null including units");
        if (value.is_null()) {
            const auto historical=prepare_component_roof_opening_directional_resize(modified,edit,children(true),"Historical40 null control");
            require(exact(historical.preview_snapshot().entities().at("host"),rows.at("host")),"new literal absence restriction does not widen historical40 units");
        }
    }
    for (int membership:{0,1,2}) {
        auto rows=source.entities();const std::vector<std::string> ids{"host"};
        rows.emplace("phases",Entity{"phases","model_phases",{{"model",ModelPhases::create(ids,membership!=2 ? ids : std::vector<std::string>{},
            {{"alternative","Choice",membership==1 ? ids : std::vector<std::string>{},membership==2 ? ids : std::vector<std::string>{}}},"alternative").to_json()}},false});
        const auto modified=replaced(source,rows);
        refuses([&] {preflight_component_roof_skylight_directional_resize(modified,edit,roofs(true),children(true));},"all registry memberships excluded including units");
    }
    for (bool baseline:{false,true}) {
        auto retiring=fixture(7,baseline);const auto before=retiring.snapshot();
        const auto prepared=prepare_component_roof_skylight_directional_resize(before,authoring(before,7,true),roofs(),children(),"Explicit retirement");
        require(mixed_selection_property_edit_retires_dependencies(prepared.intent()),"45 retained proof reports explicit retirement");
        require(!prepared.preview_snapshot().entities().contains(baseline ? "proposed-component-dim-0" : "component-dim-0"),"explicit retirement applied once");
        require(prepared.preview_snapshot().entities().contains("host-dim-0"),"hosted dimension remains ordinary carry");
    }
}
void aggregate_budget_and_source_only_controls() {
    auto document=fixture();const auto original=document.snapshot();const auto edit=authoring(original);
    require_roof_skylight_directional_resize_source_admission(original,targets(),roofs(),children());
    preflight_component_roof_skylight_directional_resize(original,edit,roofs(),children());
    auto rows=original.entities();
    for (int i=0;i<16;++i) {
        rows.at("whole").properties["vendor"]["payload-"+std::to_string(i)]=std::string(14000,'w');
        rows.at("host").properties["vendor"]["payload-"+std::to_string(i)]=std::string(4000,'h');
    }
    const auto source=replaced(original,rows);
    const auto whole=prepare_component_roof_directional_resize(source,edit,roofs(),"Independently bounded44");
    const auto child=prepare_component_roof_opening_directional_resize(source,edit,children(),"Independently bounded40");
    require(whole.intent().at("version")==44 && child.intent().at("version")==40,"both historical lanes independently admit the same original map");
    budget_refuses([&] {require_roof_skylight_directional_resize_source_admission(source,targets(),roofs(),children());},"aggregate capture byte budget");
    budget_refuses([&] {preflight_component_roof_skylight_directional_resize(source,edit,roofs(),children());},"aggregate capture byte budget");
    // The component operator is initially a unit. It must still reserve its
    // possible reviewed change/allocation, not defer the bound until native.
    auto unit_targets=targets();unit_targets[0].operation.x_factor=1;
    budget_refuses([&] {require_roof_skylight_directional_resize_source_admission(source,unit_targets,roofs(),children());},"aggregate capture byte budget");
    rows.at("whole").properties["thickness_m"]=-1;const auto unmanufacturable=replaced(original,rows);
    budget_refuses([&] {require_roof_skylight_directional_resize_source_admission(unmanufacturable,targets(),roofs(),children());},"aggregate capture byte budget");
    budget_refuses([&] {preflight_component_roof_skylight_directional_resize(unmanufacturable,edit,roofs(),children());},"aggregate capture byte budget");
    auto sparse=fixture(7,false,0,0);const auto sparse_source=sparse.snapshot();const auto sparse_edit=authoring(sparse_source);
    rows=sparse_source.entities();
    for (int i=0;i<501;++i) {auto d=dimension("component-many-"+std::to_string(i),false);rows.emplace(d.id,d);}
    for (int i=0;i<500;++i) {auto d=dimension("host-many-"+std::to_string(i),true);rows.emplace(d.id,d);}
    rows.at("whole").properties["thickness_m"]=-1;
    const auto associations=replaced(sparse_source,rows);
    budget_refuses([&] {require_roof_skylight_directional_resize_source_admission(associations,targets(),roofs(true),children(true));},"combined dimension association budget");
    budget_refuses([&] {preflight_component_roof_skylight_directional_resize(associations,sparse_edit,roofs(true),children(true));},"combined dimension association budget");
    auto invalid_targets=targets();invalid_targets[0].independent_entity_id="whole";
    refuses([&] {require_roof_skylight_directional_resize_source_admission(original,invalid_targets,roofs(),children());},"source-only gate resolves actual component identities");
    invalid_targets=targets();invalid_targets[0].operation.world_axis_rotation_radians.reset();
    refuses([&] {require_roof_skylight_directional_resize_source_admission(original,invalid_targets,roofs(),children());},"initial unit/component source operator requires explicit frame");
    auto duplicate_children=children(true);duplicate_children.push_back(duplicate_children.front());
    refuses([&] {require_roof_skylight_directional_resize_source_admission(original,targets(),roofs(true),duplicate_children);},"duplicate child units refused before elimination");
    std::vector<RoofOpeningDirectionalResizeRequest> too_many;
    for (int i=0;i<999;++i) too_many.push_back({{"host","child-"+std::to_string(i)},1,1,{1,0},0});
    budget_refuses([&] {require_roof_skylight_directional_resize_source_admission(original,targets(),roofs(true),too_many);},"aggregate selection bound");
    budget_refuses([&] {preflight_component_roof_skylight_directional_resize(original,edit,roofs(true),too_many);},"aggregate selection bound");
    auto multi=children();multi.push_back({{"host","sibling"},1,1,{1,0},0});
    require_roof_skylight_directional_resize_source_admission(original,targets(),roofs(),multi);
    const auto prepared=prepare_component_roof_skylight_directional_resize(original,edit,roofs(),multi,"One original host leaf");
    require(prepared.phase_authorings().size()==1 && prepared.phase_authorings()[0].at("ordinary_roof_edits").size()==1 &&
        prepared.roof_child_original_to_proposed().size()==2,"multiple selected children capture each requested host once");
    // A shared source catalog is captured once, but possible private catalog
    // allocations remain per independent root even at an initial unit frame.
    rows=original.entities();auto second=decode_document_assembly_instance(rows.at("component"));second.instance.id="component-two";
    rows.emplace("component-two",encode_document_assembly_instance({"component-two","assembly_instance",context},second));
    for (int i=0;i<18;++i) rows.at("catalog").properties["vendor"]["pending-"+std::to_string(i)]=std::string(6000,'c');
    const auto pending=replaced(original,rows);auto one_target=targets();one_target[0].operation.x_factor=1;
    require_roof_skylight_directional_resize_source_admission(pending,one_target,roofs(true),children(true));
    auto both_targets=one_target;both_targets.push_back({"component-two",one_target[0].operation});
    budget_refuses([&] {require_roof_skylight_directional_resize_source_admission(pending,both_targets,roofs(true),children(true));},"aggregate capture byte budget");
    auto two_edit=edit;auto second_intent=edit.intents[0];second_intent.target.instance_id="component-two";second_intent.target.document_entity_id="component-two";
    two_edit.intents.push_back(second_intent);
    budget_refuses([&] {preflight_component_roof_skylight_directional_resize(pending,two_edit,roofs(true),children(true));},"aggregate capture byte budget");
}
void whole_exclusions_and_component_authority() {
    auto document=fixture();const auto source=document.snapshot();const auto edit=authoring(source);
    for (bool unit:{false,true}) {
        for (const auto* key:{"phase_id","roof_openings","transform","vertical_placement"}) {
            auto rows=source.entities();rows.at("whole").properties[key]=nullptr;const auto modified=replaced(source,rows);
            refuses([&] {require_roof_skylight_directional_resize_source_admission(modified,targets(),roofs(unit),children(unit));},"whole44 restriction retained including units");
        }
        auto rows=source.entities();rows.at("whole").properties["version"]=4;
        refuses([&] {preflight_component_roof_skylight_directional_resize(replaced(source,rows),edit,roofs(unit),children(unit));},"whole roof schema1 retained");
        rows=source.entities();rows.emplace("dependency",Entity{"dependency","future_dependency",{{"dependency_ids",Json::array({"whole"})}},false});
        refuses([&] {require_roof_skylight_directional_resize_source_admission(replaced(source,rows),targets(),roofs(unit),children(unit));},"declared whole dependencies excluded");
        rows=source.entities();rows.emplace("join",Entity{"join","roof_join",roof_join_json(RoofJoin{"join",{"whole","host"}}),false});
        refuses([&] {preflight_component_roof_skylight_directional_resize(replaced(source,rows),edit,roofs(unit),children(unit));},"whole affected actual join excluded");
        rows=source.entities();auto model=AssemblyModel::from_json(rows.at("catalog").properties.at("model"));
        auto instances=model.instances();instances[0].placement->host_entity_id="whole";
        rows.at("catalog").properties["model"]=AssemblyModel::create(model.materials(),model.types(),instances).to_json();
        refuses([&] {preflight_component_roof_skylight_directional_resize(replaced(source,rows),edit,roofs(unit),children(unit));},"whole hosted roots excluded even for units");
    }
    auto scalar=edit;scalar.intents[0].properties.emplace("label",AssemblyInstancePropertyPatch<std::string>{AssemblyInstancePropertyPatchOperation::set,"forbidden"});
    refuses([&] {(void)prepare_component_roof_skylight_directional_resize(source,scalar,roofs(),children(),"Scalar refusal");},"no component scalar widening");
    auto pose=edit;pose.intents[0].pose.x_metres=7;
    refuses([&] {preflight_component_roof_skylight_directional_resize(source,pose,roofs(),children());},"no component pose widening");
    const auto prepared=prepare_component_roof_skylight_directional_resize(source,edit,roofs(),children(),"Wire controls");
    for (int historical:{39,40,41,42,43,44}) {
        auto wire=prepared.intent();wire["version"]=historical;
        refuses([&] {(void)validate_mixed_selection_property_edit_intent(wire);},"historical closed fields cannot borrow45 authority");
    }
    auto bad=prepared.intent();bad["roof_opening_axis_requests"][0]["child_leaf"]=Json::object();
    refuses([&] {(void)validate_mixed_selection_property_edit_intent(bad);},"child rows remain closed6");
    bad=prepared.intent();bad["room_reviews"].push_back(Json::object());
    refuses([&] {(void)validate_mixed_selection_property_edit_intent(bad);},"45 supplies no room review authority");
    // Different-host child edits retain the complete existing native join
    // admission. A broken unselected join member cannot be filtered away.
    auto joined=source.entities();joined.emplace("child-join",Entity{"child-join","roof_join",
        roof_join_json(RoofJoin{"child-join",{"host","absent-host"}}),false});
    const auto broken=replaced(source,joined);
    const auto refuses_broken_join=[](auto&& action) {
        try { action(); }
        catch (const std::invalid_argument&) { return; }
        catch (const DocumentError& error) {
            require(error.code()==DocumentErrorCode::dangling_reference,"broken child join has a dangling source member");return;
        }
        throw std::runtime_error("broken affected child join must refuse");
    };
    for (bool unit:{false,true}) {
        refuses_broken_join([&] {(void)prepare_component_roof_opening_directional_resize(broken,edit,children(unit),"Historical child join");});
        refuses_broken_join([&] {(void)prepare_component_roof_skylight_directional_resize(broken,edit,roofs(unit),children(unit),"Combined child join");});
    }
}
void proposed_component_and_single_dimension_batch() {
    auto seed=fixture();auto rows=seed.snapshot().entities();
    const std::vector<std::string> ids{"component","component-dim-0"};
    rows.emplace("phases",Entity{"phases","model_phases",{{"model",ModelPhases::create(ids,{},{{"alternative","Proposed",{},ids}},"alternative").to_json()}},false});
    std::vector<Entity> values;for (const auto& [id,entity]:rows) {(void)id;values.push_back(entity);}
    auto document=Document::create(values);const auto source=document.snapshot();
    const auto prepared=prepare_component_roof_skylight_directional_resize(source,authoring(source),roofs(),children(),"Proposed single batch");
    require(!mixed_selection_property_edit_has_phase_replacements(prepared.intent()) && prepared.reserved_component_identity_ids().empty(),"already-proposed independent root keeps its actual identity");
    const auto& out=prepared.preview_snapshot().entities();
    require(out.contains("component-dim-0") && out.contains("host-dim-0") && !out.contains("proposed-component-dim-0"),"one final batch has no stray provisional dimension copy");
    const auto independent=decode_boundary_dimension_entity(out.at("component-dim-0"));
    const auto hosted=decode_boundary_dimension_entity(out.at("host-dim-0"));
    require_near(resolve_assembly_dimension(out,*independent.dimension->assembly_target).span_metres,7.2,"proposed profile dimension targets reviewed final geometry");
    require_near(resolve_assembly_dimension(out,*hosted.dimension->assembly_target).span_metres,4,"ordinary hosted root participates beside proposed independent root");
    require(out.at("catalog").properties.at("vendor").dump()==rows.at("catalog").properties.at("vendor").dump(),"shared catalog envelope is carried exactly");
}
void pitched_rotated_and_retained_archives() {
    for (const auto* form:{"gable_roof","hip_roof"}) {
        auto document=fixture();const auto original=document.snapshot();auto rows=original.entities();auto& p=rows.at("whole").properties;
        p["form"]=form;p.erase("run_m");p["length_m"]=6.;p["rise_m"]=1.;p["pitch_rad"]=std::atan(1.);p["overhang_m"]=.2;
        p["quantity_entries"].erase("/run_m");p["quantity_entries"]["/length_m"]=receipt(6);
        const auto source=replaced(original,rows);
        const auto prepared=prepare_component_roof_skylight_directional_resize(source,authoring(source),roofs(),children(),"Gable/hip source contribution");
        require(exact(prepared.preview_snapshot().entities().at("whole"),replay_roof_plan_resize_entities(source.entities(),roofs()).at("whole")),"all admitted schema1 forms use exact original-source roof replay");
        require(solid_volume(make_roof_structure_shape(decode_roof_entity(prepared.preview_snapshot().entities().at("whole"))))>0,"pitched gable/hip result retains genuine native solid");
    }
    for (int archive_version:{1,2,3,4}) {
        auto document=fixture();const auto original=document.snapshot();auto rows=original.entities();
        // V1 whole sources generate archive1; typed host schemas3/4 generate
        // archive2/3. Shared-frame whole proof generates archive4.
        if (archive_version==1 || archive_version==4) {
            rows=replay_roof_plan_resize_entities(rows,{{"whole",1.2,1.1,{1,0},0,archive_version==1 ?
                PlanAxisResizeAnchorMode::physical_edge : PlanAxisResizeAnchorMode::shared_frame}});
        } else {
            if (archive_version==2) {
                rows.at("host").properties["version"]=3;
                for (auto& child:rows.at("host").properties["roof_openings"]) child.erase("rotation_rad");
            }
            rows=replay_roof_plan_resize_entities(rows,{{"host",1.1,1.1,{0,0},0,PlanAxisResizeAnchorMode::physical_edge}});
        }
        const std::string owner=archive_version==2 || archive_version==3 ? "host" : "whole";
        const auto key=std::string(roof_plan_resize_derivations_key);
        require(rows.at(owner).extensions.at(key).at("version")==archive_version,"actual retained archive generation has intended old dialect");
        const auto source=replaced(original,rows);const auto prepared=prepare_component_roof_skylight_directional_resize(source,authoring(source),roofs(),children(),"Retained archives");
        const auto& archive=prepared.preview_snapshot().entities().at(owner).extensions.at(key);
        require(archive.at("operations")[0].dump()==rows.at(owner).extensions.at(key).at("operations")[0].dump(),"raw original archive/operator/receipt unchanged");
        require(exact(prepared.preview_snapshot().entities().at("whole"),replay_roof_plan_resize_entities(source.entities(),roofs()).at("whole")),"archived whole exact source replay");
    }
    for (double yaw:{0.,.37}) for (double angle:{0.,.23}) for (bool swapped:{false,true}) {
        auto document=fixture();const auto original=document.snapshot();auto rows=original.entities();
        rows.at("whole").properties["rise_m"]=1.;rows.at("whole").properties["pitch_rad"]=std::atan(.5);
        rows.at("whole").properties["orientation_rad"]=yaw;
        rows.at("host").properties["rise_m"]=10.;rows.at("host").properties["pitch_rad"]=std::atan(.25);
        rows.at("host").properties["orientation_rad"]=yaw;
        rows.at("host").properties["roof_openings"][0]["rotation_rad"]=angle;
        const auto source=replaced(original,rows);
        const auto before=roof_opening_group_member_center_world(source.entities(),{"host","child"});
        const double frame=yaw+(swapped ? std::numbers::pi/2 : 0.);
        std::vector<RoofPlanResizeIntent> whole{{"whole",1.2,1.1,{-1,-2},frame,PlanAxisResizeAnchorMode::shared_frame}};
        std::vector<RoofOpeningDirectionalResizeRequest> child{{{"host","child"},1.2,1.1,{-1,-2},frame}};
        const auto prepared=prepare_component_roof_skylight_directional_resize(source,authoring(source),whole,child,"Pitched frames");
        const auto after=roof_opening_group_member_center_world(prepared.preview_snapshot().entities(),{"host","child"});
        const auto c=std::cos(frame),s=std::sin(frame);const auto dx=before.x+1,dy=before.y+2;
        require_near(after.x,-1+c*1.2*(c*dx+s*dy)-s*1.1*(-s*dx+c*dy),"qualified mouth centre uses actual world frame X");
        require_near(after.y,-2+s*1.2*(c*dx+s*dy)+c*1.1*(-s*dx+c*dy),"qualified mouth centre uses actual world frame Y");
        for (const auto* id:{"whole","host"}) for (const auto* key:{"rise_m","thickness_m","vendor"})
            require(prepared.preview_snapshot().entities().at(id).properties.at(key).dump()==rows.at(id).properties.at(key).dump(),"pitched yawed roofs retain authored source fields");
        const auto& selected=prepared.preview_snapshot().entities().at("host").properties.at("roof_openings")[0];
        require(selected.at("rotation_rad").dump()==rows.at("host").properties.at("roof_openings")[0].at("rotation_rad").dump() &&
            selected.at("skylight").dump()==rows.at("host").properties.at("roof_openings")[0].at("skylight").dump(),"individual angle and construction profile retained");
    }
}
void freshness_namespaces_and_catalog_conflicts() {
    auto document=fixture(7,true);const auto source=document.snapshot();const auto edit=authoring(source);
    const auto prepared=prepare_component_roof_skylight_directional_resize(source,edit,roofs(),children(),"Fresh source");
    auto wire=prepared.intent();wire["source_entities_digest"]=std::string(64,'0');wire["component_request"]["source_entities_digest"]=std::string(64,'0');
    refuses([&] {(void)replay_mixed_selection_property_edit_preparation(source,wire);},"matching fabricated component and bridge fences do not authenticate");
    wire=prepared.intent();wire["component_identity_reservations"]=Json::array({"counterfeit"});
    refuses([&] {(void)replay_mixed_selection_property_edit_preparation(source,wire);},"fresh receipt must be reconstructed");
    auto property=source.entities().at("property");property.properties["name"]="Unselected source update";
    document.apply(ApplyEntityChanges{.expected_revision=document.revision(),.entity_changes={EntityChange::upsert(property)},.message="Change source"});
    refuses([&] {require_mixed_selection_property_edit_source_current(document.snapshot(),prepared);},"unselected source changes stale full proof");
    document.undo(document.revision());
    refuses([&] {require_mixed_selection_property_edit_source_current(document.snapshot(),prepared);},"identical current entities with retained changed history stale proof");
    const auto component=prepare_phase_assembly_instance_property_edit(source,edit);
    refuses([&] {(void)compose_component_roof_skylight_directional_resize(document.snapshot(),component,roofs(),children(),"Borrowed preparation");},"composer independently authenticates component source history");
    const auto occupied=sketch::test::DetachedDocumentSnapshotFixture::mutate(source,[&](auto& state) {
        state.assets().emplace("proposed-component",Asset::create("proposed-component","application/octet-stream",{std::byte{1}}));
    });
    refuses([&] {(void)prepare_component_roof_skylight_directional_resize(occupied,edit,roofs(),children(),"Asset collision");},"current assets reserve lifetime namespace");
    auto retained=fixture(7,true);retained.apply(ApplyEntityChanges{.expected_revision=retained.revision(),
        .asset_changes={AssetChange::upsert(Asset::create("proposed-component","application/octet-stream",{std::byte{1}}))},.message="Retain asset"});
    retained.undo(retained.revision());
    refuses([&] {(void)prepare_component_roof_skylight_directional_resize(retained.snapshot(),edit,roofs(),children(),"Undone asset collision");},"undone retained assets reserve fresh namespace");
    auto saving=fixture();const auto unsaved=saving.snapshot();
    const auto save_prepared=prepare_component_roof_skylight_directional_resize(unsaved,authoring(unsaved),roofs(),children(),"Save fence");
    saving.mark_saved(saving.revision());
    refuses([&] {require_mixed_selection_property_edit_source_current(saving.snapshot(),save_prepared);},"saved revision alone stales45");
    const auto& actual=source.entities().at("catalog").properties.at("model");
    auto one=actual,two=actual;
    one["instances"][0]["placement"]["translation_m"]={2.,0.};
    two["instances"][0]["placement"]["translation_m"]={3.,0.};
    refuses([&] {(void)merge_disjoint_hosted_assembly_models(actual,one,two);},"conflicting exact row contributions refuse shared catalog union");
}
std::array<double,6> native_bounds(const TopoDS_Shape& shape) {
    require(!shape.IsNull() && solid_volume(shape)>0,"source/result geometry is a positive-volume native solid");
    Bnd_Box box;BRepBndLib::Add(shape,box);std::array<double,6> result;
    box.Get(result[0],result[1],result[2],result[3],result[4],result[5]);return result;
}
void flat_native_hand_geometry() {
    auto document=fixture();const auto source=document.snapshot();
    const auto whole_source=native_bounds(make_roof_structure_shape(decode_roof_entity(source.entities().at("whole"))));
    const std::array<double,6> hand_source{1,0,3.75,3,2,4};
    for (std::size_t i=0;i<6;++i) require(std::abs(whole_source[i]-hand_source[i])<1e-6,"genuine source hand roof bounds");
    const auto child_source=roof_opening_group_member_center_world(source.entities(),{"host","child"});
    require_near(child_source.x,2,"genuine selected mouth hand centre X");require_near(child_source.y,1,"genuine selected mouth hand centre Y");
    for (bool vertical:{false,true}) for (bool moving_anchor:{false,true}) {
        const double sx=vertical ? 1. : 1.5,sy=vertical ? 1.5 : 1.;
        const Vec2 anchor=moving_anchor ? Vec2{4,2} : Vec2{1,0};
        const std::vector<RoofPlanResizeIntent> whole{{"whole",sx,sy,anchor,0,PlanAxisResizeAnchorMode::shared_frame}};
        const std::vector<RoofOpeningDirectionalResizeRequest> child{{{"host","child"},sx,sy,anchor,0}};
        const auto prepared=prepare_component_roof_skylight_directional_resize(source,authoring(source),whole,child,"Native flat hand proof");
        const auto& out=prepared.preview_snapshot().entities();
        const auto bounds=native_bounds(make_roof_structure_shape(decode_roof_entity(out.at("whole"))));
        const std::array<double,6> expected{anchor.x+sx*(1-anchor.x),anchor.y+sy*(0-anchor.y),3.75,
            anchor.x+sx*(3-anchor.x),anchor.y+sy*(2-anchor.y),4};
        for (std::size_t i=0;i<6;++i) require(std::abs(bounds[i]-expected[i])<1e-6,"whole output native bounds agree with independent hand affine");
        require_near(solid_volume(make_roof_structure_shape(decode_roof_entity(out.at("whole")))),1.5,"whole volume from hand extrusion");
        const auto& row=out.at("host").properties.at("roof_openings")[0];
        require_near(row.at("x_m").get<double>()-10,anchor.x+sx*(1-anchor.x),"mouth hand left edge within broad native host");
        require_near(row.at("y_m").get<double>()-10,anchor.y+sy*(.5-anchor.y),"mouth hand bottom edge within broad native host");
        require_near(row.at("width_m").get<double>(),2*sx,"mouth hand projected width");
        require_near(row.at("depth_m").get<double>(),sy,"mouth hand projected depth");
        (void)native_bounds(make_roof_skylight_shape(decode_roof_entity(out.at("host")),"child"));
        (void)native_bounds(make_roof_structure_shape(decode_roof_entity(out.at("host"))));
        AssemblyExpansionBudget budget;(void)native_bounds(make_assembly_geometry(expand_document_assembly_instance(out.at("component"),out,budget)).shape);
    }
}
struct ProjectFile {
    std::filesystem::path directory,path;
    ProjectFile() {
        const auto stamp=std::chrono::steady_clock::now().time_since_epoch().count();
        for (unsigned attempt=0;attempt<64;++attempt) {
            directory=std::filesystem::temp_directory_path()/("vertex-triple-resize-"+std::to_string(stamp)+"-"+std::to_string(attempt));
            if (std::filesystem::create_directory(directory)) {path=directory/"history.bldproj";return;}
        }
        throw std::runtime_error("unique triple resize storage directory unavailable");
    }
    ~ProjectFile() {std::error_code error;std::filesystem::remove_all(directory,error);}
};
ApplyBoundaryConstraintChanges command(const DocumentSnapshot& source,const MixedSelectionPropertyEditPreparation& prepared) {
    ApplyBoundaryConstraintChanges result;result.expected_revision=source.revision();result.message=prepared.intent().at("message").get<std::string>();
    result.mixed_selection_edit_completion=true;result.mixed_selection_edit_intent=complete_mixed_selection_property_edit(source,prepared,{},{});return result;
}
void old_marker_refusal(const std::filesystem::path& path) {
    const auto u8=path.u8string();const std::string name(reinterpret_cast<const char*>(u8.data()),u8.size());sqlite3* raw=nullptr;
    const auto opened=sqlite3_open_v2(name.c_str(),&raw,SQLITE_OPEN_READWRITE,nullptr);
    std::unique_ptr<sqlite3,decltype(&sqlite3_close)> db(raw,&sqlite3_close);require(opened==SQLITE_OK,"saved controlled file opens for markers");
    require(sqlite3_exec(db.get(),"BEGIN; PRAGMA user_version=267; UPDATE metadata SET value='267' WHERE key='format_version'; COMMIT",nullptr,nullptr,nullptr)==SQLITE_OK,"only marker pair downgraded");db.reset();
    const auto digest=ProjectStore::file_sha256(path);bool refused=false;
    try {(void)ProjectStore::load(path);} catch (const StorageError& error) {
        require(error.code()==StorageErrorCode::unsupported_format && std::string(error.what()).find("requires reader 268")!=std::string::npos,"reader267 refuses retained canonical45 at reader268");refused=true;
    }
    require(refused && ProjectStore::file_sha256(path)==digest,"reader267 refusal preserves every file byte");
}
void persisted_history_and_old_authority_precedence() {
    for (bool baseline:{false,true}) {
        auto seed=fixture(7,baseline);auto rows=seed.snapshot().entities();
        auto earlier=decode_document_assembly_instance(rows.at("component"));earlier.instance.id="earlier-component";
        rows.emplace("earlier-component",encode_document_assembly_instance({"earlier-component","assembly_instance",context},earlier));
        std::vector<Entity> values;for (const auto& [id,entity]:rows) {(void)id;values.push_back(entity);}
        auto document=Document::create(values);const auto start=document.snapshot();
        auto early_edit=authoring(start);early_edit.intents[0].target.instance_id="earlier-component";
        early_edit.intents[0].target.document_entity_id="earlier-component";
        early_edit.profile_dimension_decisions.clear();early_edit.dimension_identities.clear();early_edit.allocation={};
        const auto older=prepare_component_roof_directional_resize(start,early_edit,roofs(),"Earlier Properties44");
        document.apply(command(start,older));const auto previous=document.snapshot();
        require(ProjectStore::required_format_version(previous)==267,"actual earlier44 floor is267");
        const auto prepared=prepare_component_roof_skylight_directional_resize(previous,authoring(previous),roofs(),children(),"Later Properties45");
        const auto operation=command(previous,prepared);const auto wire=command_to_json(operation);
        require(wire.at("version")==52 && wire.at("mixed_selection_edit_intent").at("version")==45,"canonical52/45 command");
        document.apply(operation);const auto applied=document.snapshot();
        require(applied.entities()==prepared.preview_snapshot().entities() && applied.revision()==previous.revision()+1,"one command publishes all three rosters");
        document.undo(document.revision());const auto undone=document.snapshot();require(undone.entities()==previous.entities(),"one Undo restores all three original envelopes");
        document.redo(document.revision());const auto redone=document.snapshot();require(redone.entities()==applied.entities(),"one Redo returns exact result");
        for (const auto& snapshot:{applied,undone,redone}) {
            require(ProjectStore::required_format_version(snapshot)==268 && ProjectStore::required_semantic_format_version(snapshot)==268,"earlier44 cannot hide later applied or undone45 reader268");
            ProjectFile file;(void)ProjectStore::save(file.path,snapshot);auto loaded=ProjectStore::load(file.path);const auto reopened=loaded.document.snapshot();
            require(reopened.entities()==snapshot.entities() && reopened.assets()==snapshot.assets() && reopened.history().size()==snapshot.history().size(),"fresh editable Open retains graph, assets and persisted history");
            require(command_to_json(*reopened.history().at(applied.revision()).boundary_constraint_changes)==wire,"fresh Open retains exact52/45 proof");
            const auto extracted=file.directory/"extracted";extract_project(snapshot,extracted);std::ifstream input(extracted/"project.json");const auto manifest=Json::parse(input);
            require(manifest.at("exchange_version")==266 && manifest.at("revisions").at(applied.revision()).at("boundary_constraint_changes")==wire,"extraction266 retains actual45 history");
            auto property=reopened.entities().at("property");property.properties["name"]="Fresh Open editable";
            loaded.document.apply(ApplyEntityChanges{.expected_revision=loaded.document.revision(),.entity_changes={EntityChange::upsert(property)},.message="New Undo session"});
            loaded.document.undo(loaded.document.revision());require(loaded.document.snapshot().entities()==reopened.entities(),"fresh editable Open begins new working Undo session");
            old_marker_refusal(file.path);
        }
    }
    auto document=fixture();const auto source=document.snapshot();
    const auto prepared=prepare_component_roof_skylight_directional_resize(source,authoring(source),roofs(),children(),"Opaque metadata control");
    auto property=source.entities().at("property");property.extensions=command_to_json(command(source,prepared));property.properties["opaque_intent"]=prepared.intent();
    document.apply(ApplyEntityChanges{.expected_revision=document.revision(),.entity_changes={EntityChange::upsert(property)},.message="Opaque lookalike"});
    const auto opaque=document.snapshot();document.undo(document.revision());
    for (const auto& snapshot:{opaque,document.snapshot()}) {
        require(ProjectStore::required_format_version(snapshot)==ProjectStore::required_format_version(source),"opaque current/history45 lookalikes do not gain authority");
        ProjectFile file;extract_project(snapshot,file.directory/"extracted");std::ifstream input(file.directory/"extracted"/"project.json");
        require(Json::parse(input).at("exchange_version")==ProjectStore::required_semantic_format_version(source)-2,"opaque lookalikes do not raise extraction floor");
    }
    const auto archived=replay_roof_plan_resize_entities(source.entities(),roofs());
    std::vector<Entity> rows;for (const auto& [id,entity]:archived) {(void)id;rows.push_back(entity);}
    const auto archive_only=Document::create(rows).snapshot();
    require(ProjectStore::required_format_version(archive_only)==267,"canonical archive4-only control retains reader267");
    ProjectFile file;extract_project(archive_only,file.directory/"extracted");std::ifstream input(file.directory/"extracted"/"project.json");
    require(Json::parse(input).at("exchange_version")==265,"archive4-only retains extraction265");
}
} // namespace
int main(int argc,char** argv) {
    sketch::testing::noninteractive_errors();
    try {
        const std::string_view only=argc>1 ? argv[1] : "";
        if (only.empty() || only=="--core") {
            closed_wire_and_original_map();literal_phase_absence_and_retirement();whole_exclusions_and_component_authority();proposed_component_and_single_dimension_batch();
            pitched_rotated_and_retained_archives();freshness_namespaces_and_catalog_conflicts();
        }
        if (only.empty() || only=="--budget") aggregate_budget_and_source_only_controls();
        if (only.empty() || only=="--native") flat_native_hand_geometry();
        if (only.empty() || only=="--storage") persisted_history_and_old_authority_precedence();
        std::cout<<"Mixed component/roof/skylight resize contracts passed\n";return 0;
    } catch (const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
