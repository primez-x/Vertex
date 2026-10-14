#include "sketch/mixed_selection_property_edit.hpp"
#include "sketch/assembly_dimension.hpp"
#include "sketch/assembly_model.hpp"
#include "sketch/boundary_dimension.hpp"
#include "sketch/building_entity.hpp"
#include "sketch/document_digest.hpp"
#include "sketch/model_phases.hpp"
#include "sketch/phase_constraint_authoring.hpp"
#include "sketch/phase_wall_replacement_command.hpp"
#include "sketch/plan_axis_resize.hpp"
#include "sketch/project_exchange.hpp"
#include "sketch/project_store.hpp"
#include "sketch/structural_hosted_components.hpp"
#include "support/noninteractive_errors.hpp"

#include <sqlite3.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>

namespace {
using namespace sketch;
using Json=nlohmann::json;
using Entities=std::map<std::string,Entity,std::less<>>;
void require(bool value,const char* message) { if (!value) throw std::runtime_error(message); }
void require_near(double actual,double expected,const char* message) {
    if (!std::isfinite(actual) || std::abs(actual-expected)>1e-8) throw std::runtime_error(message);
}
template<class F> void invalid(F&& action,const char* message) {
    try { action(); } catch (const std::invalid_argument&) { return; }
    throw std::runtime_error(message);
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
AssemblyProfile profile(bool identified) {
    AssemblyProfile result;
    result.id="profile";result.outer={{{0,0},{4,0},0},{{4,0},{4,3},0},{{4,3},{0,3},0},{{0,3},{0,0},0}};
    result.elevation_m=.4;result.height_m=.6;
    if (identified) result.topology=topology("");
    return result;
}
Entity catalog(const std::string& id,const std::string& host,bool identified=true) {
    AssemblyType child;child.id="body";child.name="Source body";child.profiles={profile(identified)};
    AssemblyType parent;parent.id="parent";parent.name="Nested source";
    AssemblyPart part;part.id="child";part.type_id="body";
    part.transform=AssemblyTransform{{.3,-.2,.75},.17,1,false};parent.parts={part};
    std::vector<AssemblyInstance> rows;
    if (!host.empty()) {
        AssemblyInstance instance;instance.id=host+"-root";instance.type_id="parent";
        instance.placement=AssemblyPlacement{host,{.6,-.35},.23,1,true,.2,1};rows.push_back(instance);
    }
    Entity result{id,"assembly_model",{{"model",AssemblyModel::create({}, {child,parent},rows).to_json()},{"note","Retain definitions"}},false};
    result.extensions={{"vendor",{{"retain",Json::array({9,"opaque"})}}}};return result;
}
Entity dimension(const std::string& id,const std::string& catalog_id,const std::string& instance,
    std::optional<std::string> owner,bool edge=false) {
    BoundaryDimension value;value.id=id;value.boundary_id=owner.value_or(catalog_id);
    value.kind=edge ? BoundaryDimensionKind::assembly_profile : BoundaryDimensionKind::assembly_extent;
    value.text_position={7.5,-2.25};value.presentation=BoundaryDimensionPresentation{3.2,"#713ba2",true,false,true,.31};
    AssemblyDimensionTarget target;target.component={catalog_id,instance,owner,{"child"}};target.axis=AssemblyDimensionAxis::width;
    if (edge) target.profile=selector("");value.assembly_target=target;
    auto result=encode_boundary_dimension_entity(value);result.extensions={{"vendor",{{"retain","dimension-envelope"}}}};return result;
}
Document from_entities(const Entities& entities) {
    std::vector<Entity> values;for (const auto& [id,entity]:entities) { (void)id;values.push_back(entity); }
    return Document::create(values);
}
Document fixture(bool component_baseline=false,bool structural_baseline=false,int component_version=7,
    std::size_t component_dimensions=1,std::size_t unit_dimensions=1,bool profile_dimensions=true) {
    auto column=encode_building_entity(RectangularColumn{"column",{2,1,.4},2,2,3,0});
    auto unit=encode_building_entity(RectangularColumn{"unit-column",{12,1,.4},2,2,3,0});
    auto beam=encode_building_entity(Beam{.id="beam",.start={2,3,2.2},.end={6,3,2.2},.up={0,0,1},.width=2,.depth=.4});
    for (auto* entity:{&column,&beam,&unit}) {
        for (const auto& [key,value]:context.items()) entity->properties[key]=value;
        entity->properties["mark"]=entity->type=="column" ? "C-1" : "B-1";
        entity->extensions={{"vendor",{{"retain","structural-envelope"}}}};
    }
    column.properties["quantity_entries"]={{"/width_m",{{"version",1},{"original_expression","2 m"},
        {"entered_unit","m"},{"exact_metres",{{"numerator",2},{"denominator",1}}},{"opaque","receipt-sibling"}}},
        {"/height_m",{{"version",1},{"original_expression","3 m"},{"entered_unit","m"},
            {"exact_metres",{{"numerator",3},{"denominator",1},{"opaque_exact",true}}},{"opaque","unchanged-receipt"}}},
        {"/vendor",{{"version",81},{"opaque","future"}}}};
    std::vector<Entity> values{
        {"property","property",{{"name","Mixed structural source"}},false},
        {"building","building",{{"property_id","property"}},false},
        {"floor","floor",{{"building_id","building"}},false},{"layer","layer",{{"floor_id","floor"}},false},
        column,beam,unit,catalog("component-catalog","",component_version==7),catalog("column-catalog","column"),
        catalog("beam-catalog","beam"),catalog("unit-catalog","unit-column")};
    AssemblyInstance independent;independent.id="component";independent.type_id="parent";
    independent.root_transform=AssemblyTransform{{5,1,.75},.29,1,true};
    auto owner=encode_document_assembly_instance({"component","assembly_instance",context},{"component-catalog",independent});
    owner.extensions={{"vendor",{{"retain","component-envelope"}}}};values.push_back(owner);
    for (std::size_t i=0;i<component_dimensions;++i)
        values.push_back(dimension("component-dim-"+std::to_string(i),"component-catalog","component",std::string{"component"}));
    for (std::size_t i=0;i<unit_dimensions;++i)
        values.push_back(dimension("unit-dim-"+std::to_string(i),"unit-catalog","unit-column-root",std::nullopt));
    if (profile_dimensions) {
        if (component_version==7) values.push_back(dimension("component-profile-dim","component-catalog","component",std::string{"component"},true));
        values.push_back(dimension("column-dim","column-catalog","column-root",std::nullopt));
        values.push_back(dimension("column-profile-dim","column-catalog","column-root",std::nullopt,true));
        values.push_back(dimension("beam-dim","beam-catalog","beam-root",std::nullopt));
    }
    if (component_baseline || structural_baseline) {
        std::vector<std::string> baseline;
        if (component_baseline) {
            baseline.push_back("component");
            for (std::size_t i=0;i<component_dimensions;++i) baseline.push_back("component-dim-"+std::to_string(i));
            if (profile_dimensions && component_version==7) baseline.push_back("component-profile-dim");
        }
        if (structural_baseline) {
            baseline.insert(baseline.end(),{"column","column-catalog"});
            if (profile_dimensions) baseline.insert(baseline.end(),{"column-dim","column-profile-dim"});
        }
        const auto phases=ModelPhases::create(baseline,baseline,{RemodelingAlternative{"alternative","Source choice",{}, {}}},"alternative");
        values.push_back({"phases","model_phases",{{"model",phases.to_json()}},false});
    }
    return Document::create(values);
}
PhaseAssemblyInstancePropertyEditAuthoring component_edit(const DocumentSnapshot& source,int version=7,bool retire=false) {
    const auto model=AssemblyModel::from_json(source.entities().at("component-catalog").properties.at("model"));
    const auto body=std::find_if(model.types().begin(),model.types().end(),[](const auto& type) { return type.id=="body"; });
    require(body!=model.types().end(),"component fixture must retain its source body");
    auto proposed=body->profiles.front();
    proposed.planar_affine=AssemblyProfilePlanarAffine{{0,0},{0,0},1.8,0,0,.65};proposed.topology=topology("new-");
    AssemblyInstancePropertyEditIntent intent;intent.target={"component-catalog","component",std::string{"component"},{"child"}};
    intent.profile_geometry.operation=AssemblyInstancePropertyPatchOperation::set;
    intent.profile_geometry.value=std::vector<AssemblyProfile>{proposed};
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
    for (const auto& id:inspect_phase_assembly_instance_property_presentation_overlay_ids(source.entities(),result.intents,false,result.profile_dimension_decisions))
        result.presentation_overlay_identities.emplace(id,"proposed-overlay-"+id);
    return result;
}
std::vector<StructuralDirectionalResizeRequest> operators(bool units=false) {
    return {{"unit-column",1,1,{1,0},0},{"column",units ? 1. : 1.8,units ? 1. : .65,{1,0},0},
        {"beam",units ? 1. : 1.8,units ? 1. : .65,{1,0},0}};
}
std::vector<StructuralObjectEditIntent> captured(const DocumentSnapshot& source,const std::vector<StructuralDirectionalResizeRequest>& requests) {
    std::vector<StructuralObjectEditIntent> result;
    for (const auto& request:requests) {
        const auto& owner=source.entities().at(request.object_id);
        const auto candidate=stage_structural_plan_axis_resize_entity(owner,request.sx,request.sy,request.anchor_m,request.frame_rotation_radians);
        if (const auto edit=capture_structural_object_edit(owner,candidate)) result.push_back(*edit);
    }
    std::sort(result.begin(),result.end(),[](const auto& a,const auto& b) { return a.object_id<b.object_id; });return result;
}
std::optional<ComponentStructuralDirectionalResizeAllocation> allocation(const DocumentSnapshot& source) {
    const auto replacement=phase_structural_edit_replacement_request(source.entities(),captured(source,operators()));
    if (!replacement) return std::nullopt;
    const auto plan=inspect_phase_structural_replacement_plan(source.entities(),replacement->seed_object_ids,
        replacement->registry_id,replacement->alternative_id,true,true);
    require(plan.ready(),"fixture replacement inventory must be ready");
    ComponentStructuralDirectionalResizeAllocation result;
    for (const auto& id:plan.required_entity_ids) result.identities.emplace(id,"proposed-"+id);
    for (const auto& id:plan.required_child_ids) result.identities.emplace(id,"proposed-"+id);
    for (const auto& root:plan.required_hosted_instance_ids) result.hosted_instance_identities.emplace(root,"proposed-"+root.second);
    for (const auto& id:plan.required_dimension_ids) result.dimension_identities.emplace(id,"proposed-"+id);return result;
}
double span(const Entities& entities,const std::string& id) {
    const auto decoded=decode_boundary_dimension_entity(entities.at(id));
    require(decoded.supported() && decoded.dimension->assembly_target.has_value(),"dimension must remain supported");
    return resolve_assembly_dimension(entities,*decoded.dimension->assembly_target).span_metres;
}
void supported_geometry_and_phase_partitions() {
    for (int version:{7,8,9}) for (bool cb:{false,true}) for (bool sb:{false,true}) {
        auto document=fixture(cb,sb,version);const auto source=document.snapshot();const auto digest=document_snapshot_digest(source);
        const auto prepared=prepare_component_structural_directional_resize(source,component_edit(source,version),operators(),allocation(source),"Source-derived mixed structural geometry");
        const auto& final=prepared.preview_snapshot().entities();
        require(prepared.intent().at("version")==41 && prepared.intent().size()==13,"Properties41 must retain its thirteen exact fields");
        require(prepared.intent().at("component_request").at("version")==version,"component geometry request must retain its actual authority");
        require(document_snapshot_digest(document.snapshot())==digest,"preparation must preserve the complete source history");
        const auto component=cb ? "proposed-component" : "component";
        const auto column=sb ? "proposed-column" : "column";
        require(prepared.selected_original_to_proposed().at("component")==component &&
            prepared.selected_original_to_proposed().at("column")==column && prepared.selected_original_to_proposed().at("beam")=="beam" &&
            prepared.selected_original_to_proposed().at("unit-column")=="unit-column","baseline and ordinary/unit selected maps must derive independently");
        for (const auto& request:operators()) {
            auto expected=stage_structural_plan_axis_resize_entity(source.entities().at(request.object_id),request.sx,request.sy,request.anchor_m,request.frame_rotation_radians);
            expected.id=prepared.selected_original_to_proposed().at(request.object_id);
            require(exact(expected,final.at(expected.id)),"final owner must exactly match the native anchored operator");
        }
        require_near(final.at(column).properties.at("width_m").get<double>(),3.6,"column X size must follow anisotropy");
        require_near(final.at(column).properties.at("depth_m").get<double>(),1.3,"column Y size must follow anisotropy");
        require_near(final.at("beam").properties.at("width_m").get<double>(),1.3,"beam section width must follow the across factor");
        require(final.at(column).properties.at("base_center_m").at(2)==.4 && final.at("beam").properties.at("start_m").at(2)==2.2,
            "structural resize must preserve Z");
        require(!final.at(column).properties.at("quantity_entries").contains("/width_m") &&
            final.at(column).properties.at("quantity_entries").at("/vendor")==source.entities().at("column").properties.at("quantity_entries").at("/vendor"),
            "changed width receipt must invalidate while opaque siblings survive");
        require(final.at(column).properties.at("quantity_entries").at("/height_m").dump()==
            source.entities().at("column").properties.at("quantity_entries").at("/height_m").dump(),
            "unchanged entered height receipt and its opaque siblings must remain exact");
        require(decode_document_assembly_instance(final.at(component)).instance.root_transform==
            decode_document_assembly_instance(source.entities().at("component")).instance.root_transform,"component geometry must retain nested reflected root pose");
        require_near(span(final,cb ? "proposed-component-dim-0" : "component-dim-0")/span(source.entities(),"component-dim-0"),1.8,
            "independent component extent must follow its own geometry");
        for (const auto* id:{"unit-column","unit-catalog","unit-dim-0"}) require(exact(final.at(id),source.entities().at(id)),"unit source envelopes must remain exact");
        // Full profile edits retain type-owned world roots; legacy host-derived
        // geometry follows the changed host through the existing coordinator.
        require_near(span(final,sb ? "proposed-column-profile-dim" : "column-profile-dim"),4,"hosted own profile must retain its exact scalar-edit semantics");
        if (sb) {
            require(exact(final.at("column"),source.entities().at("column")) && exact(final.at("column-catalog"),source.entities().at("column-catalog")) &&
                exact(final.at("column-dim"),source.entities().at("column-dim")),"baseline original closure must remain exact");
            require(prepared.hosted_binding_original_to_proposed().at({"column-catalog","column-root"})==
                std::pair<std::string,std::string>{"proposed-column-catalog","proposed-column-root"},"qualified hosted copied binding must be exposed");
            require(prepared.phase_authorings().size()==1,"changed baseline must expose one internally derived proof");
            const auto proof=decode_phase_constraint_authoring_intent(prepared.phase_authorings().front());
            require(proof.structural_replacement.at("version")==5 && proof.structural_replacement.at("authoring").at("version")==3,
                "anchor motion must derive full leaf5/body3, never scalar body4");
            require(proof.source_snapshot_digest==digest,"physical proof must authenticate the complete source");
        } else require(prepared.phase_authorings().empty(),"ordinary lane must not invent a replacement proof");
        require(replay_mixed_selection_property_edit(source,complete_mixed_selection_property_edit(source,prepared,{},{})).entities==final,
            "complete replay must reproduce both families and the one final dimension batch");
    }
    auto document=fixture();auto entities=document.snapshot().entities();
    const std::vector<std::string> ids{"component","column","beam","component-catalog","column-catalog","beam-catalog",
        "component-dim-0","component-profile-dim","column-dim","column-profile-dim","beam-dim"};
    const auto phases=ModelPhases::create(ids,{}, {RemodelingAlternative{"alternative","Existing proposed",{},ids}},"alternative");
    entities.emplace("phases",Entity{"phases","model_phases",{{"model",phases.to_json()}},false});
    auto proposed=from_entities(entities);const auto source=proposed.snapshot();
    const auto prepared=prepare_component_structural_directional_resize(source,component_edit(source),operators(),std::nullopt,"Retain proposed identities");
    require(!mixed_selection_property_edit_has_phase_replacements(prepared.intent()) && prepared.selected_original_to_proposed().at("column")=="column",
        "active proposed targets must retain IDs without allocations");
}
void closed_wire_and_source_fences() {
    auto document=fixture();const auto source=document.snapshot();const auto authoring=component_edit(source);
    const auto prepared=prepare_component_structural_directional_resize(source,authoring,operators(),std::nullopt,"Closed operators");
    for (const auto* key:{"base_intent","structural_edits","structural_replacement","candidate_entities","slab_geometry"}) {
        auto wire=prepared.intent();wire[key]=Json::object();invalid([&] { (void)validate_mixed_selection_property_edit_intent(wire); },"extra authority must refuse");
    }
    for (const auto& [key,value]:prepared.intent().items()) {
        (void)value;auto wire=prepared.intent();wire.erase(key);invalid([&] { (void)validate_mixed_selection_property_edit_intent(wire); },"every closed field is mandatory");
    }
    for (const auto* key:{"object_id","sx","sy","anchor_m","frame_rotation_radians"}) {
        auto wire=prepared.intent();wire["structural_axis_requests"][0].erase(key);
        invalid([&] { (void)validate_mixed_selection_property_edit_intent(wire); },"every axis row field is mandatory");
    }
    auto wire=prepared.intent();wire["structural_axis_requests"][0]["profile_fields"]=Json::object();
    invalid([&] { (void)validate_mixed_selection_property_edit_intent(wire); },"caller profiles must refuse");
    wire=prepared.intent();std::swap(wire["structural_axis_requests"][0],wire["structural_axis_requests"][1]);
    invalid([&] { (void)validate_mixed_selection_property_edit_intent(wire); },"wire operators must be sorted");
    wire=prepared.intent();wire["structural_axis_requests"].push_back(wire["structural_axis_requests"].back());
    invalid([&] { (void)validate_mixed_selection_property_edit_intent(wire); },"duplicate operators must refuse");
    for (double factor:{0.,-1.,std::numeric_limits<double>::infinity()}) {
        auto bad=operators();bad[0].sx=factor;invalid([&] { (void)prepare_component_structural_directional_resize(source,authoring,bad,std::nullopt,"Invalid factor"); },"nonpositive/nonfinite factors must refuse");
    }
    auto bad=operators();bad[0].anchor_m.x=std::numeric_limits<double>::quiet_NaN();
    invalid([&] { (void)prepare_component_structural_directional_resize(source,authoring,bad,std::nullopt,"Invalid anchor"); },"nonfinite anchor must refuse");
    for (const auto* key:{"expected_revision","source_snapshot_digest","source_authoring_digest","source_entities_digest","source_saved_revision"}) {
        wire=prepared.intent();wire["component_request"][key]=std::string_view(key).find("digest")!=std::string_view::npos ? Json(std::string(64,'0')) : Json(999);
        invalid([&] { (void)replay_mixed_selection_property_edit_preparation(source,wire); },"nested source fences must match outer source fences");
    }
    wire=prepared.intent();wire["source_entities_digest"]=std::string(64,'0');wire["component_request"]["source_entities_digest"]=std::string(64,'0');
    invalid([&] { (void)replay_mixed_selection_property_edit_preparation(source,wire); },"matching fabricated fences must fail actual authentication");
    wire=prepared.intent();wire["component_identity_reservations"]=Json::array({"counterfeit"});
    invalid([&] { (void)replay_mixed_selection_property_edit_preparation(source,wire); },"reservation receipt must derive from actual replay");
    for (const auto* key:{"room_reviews","phase_room_reviews"}) {
        wire=prepared.intent();wire[key]=Json::array({Json::object()});invalid([&] { (void)validate_mixed_selection_property_edit_intent(wire); },"room authority must remain empty");
    }
    invalid([&] { (void)prepare_component_structural_directional_resize(source,authoring,std::vector<StructuralDirectionalResizeRequest>(1000,operators().front()),std::nullopt,"Shared roster bound"); },"1001 combined selected members must refuse");
    invalid([&] { (void)prepare_component_structural_directional_resize(source,authoring,std::span<const StructuralDirectionalResizeRequest>{},std::nullopt,"Empty structures"); },"both families must be nonempty");
    invalid([&] { (void)prepare_component_structural_directional_resize(source,PhaseAssemblyInstancePropertyEditAuthoring{},operators(),std::nullopt,"Empty component"); },"both families must be nonempty");
    auto scalar=authoring;scalar.intents[0].properties.emplace("label",AssemblyInstancePropertyPatch<std::string>{AssemblyInstancePropertyPatchOperation::set,"forbidden"});
    invalid([&] { (void)prepare_component_structural_directional_resize(source,scalar,operators(),std::nullopt,"Scalar authority"); },"component scalar authority must refuse");
    auto pose=authoring;pose.intents[0].pose.operation=AssemblyInstancePropertyPatchOperation::set;pose.intents[0].pose.x_metres=50;
    invalid([&] { (void)prepare_component_structural_directional_resize(source,pose,operators(),std::nullopt,"Pose authority"); },"component pose authority must refuse");
    auto property=source.entities().at("property");property.properties["name"]="Changed source";
    document.apply(ApplyEntityChanges{.expected_revision=document.revision(),.entity_changes={EntityChange::upsert(property)},.message="Change complete source"});
    invalid([&] { require_mixed_selection_property_edit_source_current(document.snapshot(),prepared); },"unselected source changes must stale the preparation");
    document.undo(document.revision());
    invalid([&] { require_mixed_selection_property_edit_source_current(document.snapshot(),prepared); },"restored visible entities with changed history must remain stale");
    auto saved=fixture();const auto before=saved.snapshot();const auto unsaved=prepare_component_structural_directional_resize(before,component_edit(before),operators(),std::nullopt,"Saved fence");
    saved.mark_saved(saved.revision());invalid([&] { require_mixed_selection_property_edit_source_current(saved.snapshot(),unsaved); },"saved revision alone must stale source authority");
    const auto borrowed=prepare_phase_assembly_instance_property_edit(source,authoring);
    invalid([&] { (void)compose_component_structural_directional_resize(document.snapshot(),borrowed,
        std::vector<StructuralDirectionalResizeRequest>{{"column",1,1,{0,0},0}},std::nullopt,"Borrowed preparation"); },
        "composer must independently authenticate a borrowed component preparation against the actual complete source");
}

void allocation_and_freshness_refusals() {
    auto document=fixture(true,true);const auto source=document.snapshot();const auto authoring=component_edit(source);
    const auto ids=allocation(source);require(ids.has_value(),"baseline fixture must need allocation");
    invalid([&] { (void)prepare_component_structural_directional_resize(source,authoring,operators(),std::nullopt,"Missing allocation"); },"changed baseline seeds must require allocation");
    invalid([&] { (void)prepare_component_structural_directional_resize(source,authoring,operators(true),ids,"Unit allocation"); },"units must gain no replacement allocation");
    auto ordinary=fixture();const auto ordinary_source=ordinary.snapshot();
    invalid([&] { (void)prepare_component_structural_directional_resize(ordinary_source,component_edit(ordinary_source),operators(),ids,"Ordinary allocation"); },"ordinary targets must gain no replacement allocation");
    auto bad=*ids;bad.identities.erase("column");
    invalid([&] { (void)prepare_component_structural_directional_resize(source,authoring,operators(),bad,"Missing owner"); },"missing source owner mapping must refuse");
    bad=*ids;bad.identities.emplace("unit-column","proposed-unit");
    invalid([&] { (void)prepare_component_structural_directional_resize(source,authoring,operators(),bad,"Extra unit owner"); },"unchanged unit owner allocation must refuse");
    bad=*ids;bad.hosted_instance_identities.clear();
    invalid([&] { (void)prepare_component_structural_directional_resize(source,authoring,operators(),bad,"Missing qualified root"); },"missing source hosted binding must refuse");
    bad=*ids;bad.dimension_identities.erase("column-dim");
    invalid([&] { (void)prepare_component_structural_directional_resize(source,authoring,operators(),bad,"Missing dimension"); },"missing source dimension allocation must refuse");
    bad=*ids;bad.dimension_identities.emplace("unit-dim-0","proposed-unit-dim");
    invalid([&] { (void)prepare_component_structural_directional_resize(source,authoring,operators(),bad,"Extra dimension"); },"unit dimension allocation must refuse");
    for (const auto& collision:std::vector<std::string>{"beam","proposed-component","proposed-component-dim-0",
        embedded_assembly_presentation_ids(source.entities()).at({"unit-catalog","unit-column-root"})}) {
        bad=*ids;bad.identities.at("column")=collision;
        invalid([&] { (void)prepare_component_structural_directional_resize(source,authoring,operators(),bad,"Fresh destination collision"); },
            "fresh destinations must reject actual, alias and cross-component reservations");
    }
    const auto control=prepare_component_structural_directional_resize(source,authoring,operators(),ids,"Computed alias control");
    const auto proposed_alias=embedded_assembly_presentation_ids(control.preview_snapshot().entities()).at(
        {"proposed-column-catalog","proposed-column-root"});
    require(proposed_alias.size()<=128,"collision control must fit the global identity limit");
    bad=*ids;bad.dimension_identities.at("column-dim")=proposed_alias;
    bool alias_refused=false;
    try { (void)prepare_component_structural_directional_resize(source,authoring,operators(),bad,"Computed destination alias collision"); }
    catch (const std::invalid_argument& error) {
        require(std::string(error.what()).find("fresh dimension collides with complete body or computed alias")!=std::string::npos,
            "short destination alias must refuse against the actual candidate, not the global identity limit");alias_refused=true;
    }
    require(alias_refused,"fresh dimensions must not borrow a newly computed hosted alias");
    require(document_snapshot_digest(document.snapshot())==document_snapshot_digest(source),
        "computed alias collision refusal must preserve the complete source");
    Entity historical{"history-only-destination","label",{{"text","Retained identity"}},false};
    document.apply(ApplyEntityChanges{.expected_revision=document.revision(),.entity_changes={EntityChange::upsert(historical)},.message="Retain undone identity"});
    document.undo(document.revision());const auto retained=document.snapshot();
    require(!retained.entities().contains(historical.id),"history-only identity must be absent at the current head");
    bad=*allocation(retained);bad.identities.at("column")=historical.id;
    invalid([&] { (void)prepare_component_structural_directional_resize(retained,component_edit(retained),operators(),bad,"History identity collision"); },
        "fresh destinations must reject retained undone identities");
}
void strict_structural_forms_and_exact_capture() {
    auto document=fixture();const auto source=document.snapshot();
    for (bool unit:{false,true}) {
        auto entities=source.entities();auto circular=encode_building_entity(CircularColumn{"column",{2,1,.4},1,3,0});
        for (const auto& [key,value]:context.items()) circular.properties[key]=value;
        entities.at("column")=circular;auto changed=from_entities(entities);const auto actual=changed.snapshot();
        invalid([&] { (void)prepare_component_structural_directional_resize(actual,component_edit(actual),operators(unit),std::nullopt,"Circular refusal"); },
            "circular columns must refuse including isotropic units");
        entities=source.entities();entities.at("beam").properties["end_m"][2]=3.2;
        auto sloped=from_entities(entities);const auto slope=sloped.snapshot();
        invalid([&] { (void)prepare_component_structural_directional_resize(slope,component_edit(slope),operators(unit),std::nullopt,"Sloped refusal"); },
            "sloped beams must refuse including units");
    }
    auto oblique=operators();oblique[1].frame_rotation_radians=.3;
    invalid([&] { (void)prepare_component_structural_directional_resize(source,component_edit(source),oblique,std::nullopt,"Oblique anisotropy"); },
        "oblique anisotropy must retain native rectangular alignment refusal");
    auto entities=source.entities();entities.at("column").properties["radius_m"]=.75;
    auto dormant=from_entities(entities);const auto actual=dormant.snapshot();const auto before=document_snapshot_digest(actual);
    bool refused=false;
    try { (void)prepare_component_structural_directional_resize(actual,component_edit(actual),operators(),std::nullopt,"Dormant exact capture"); }
    catch (const std::invalid_argument& error) {
        require(std::string(error.what()).find("exact typed full capture cannot preserve the source")!=std::string::npos,
            "dormant form-inapplicable fields must receive explicit exact-capture refusal");refused=true;
    }
    require(refused && document_snapshot_digest(dormant.snapshot())==before,"exact capture refusal must retain dormant metadata and source history");
    auto future=source.entities();future.at("column").properties["version"]=2;
    // Future structural form admission may already be refused by Document;
    // either source admission or the closed lane must reject that authority.
    bool future_refused=false;
    try {
        auto unknown=from_entities(future);const auto snapshot=unknown.snapshot();
        (void)prepare_component_structural_directional_resize(snapshot,component_edit(snapshot),operators(true),std::nullopt,"Future unit refusal");
    } catch (const std::invalid_argument&) { future_refused=true; }
      catch (const DocumentError&) { future_refused=true; }
    require(future_refused,"future structural versions must never borrow Properties41 unit authority");
}
void complete_units_and_shared_dimension_bound() {
    auto document=fixture();const auto source=document.snapshot();
    const auto prepared=prepare_component_structural_directional_resize(source,component_edit(source),operators(true),std::nullopt,"Complete units");
    for (const auto* id:{"column","beam","unit-column","column-catalog","unit-catalog","column-dim","unit-dim-0"})
        require(exact(prepared.preview_snapshot().entities().at(id),source.entities().at(id)),"structural units and their associations must remain exact");
    require(prepared.intent().at("structural_allocation").is_null(),"unit selection must allocate nothing");
    auto broken=source.entities();broken.at("unit-catalog").properties["model"]["instances"][0]["placement"]["scale"]=0;
    bool host_refused=false;
    try {
        auto bad=from_entities(broken);const auto snapshot=bad.snapshot();
        (void)prepare_component_structural_directional_resize(snapshot,component_edit(snapshot),operators(),std::nullopt,"Invalid unit hosted geometry");
    } catch (const std::invalid_argument&) { host_refused=true; }
      catch (const DocumentError&) { host_refused=true; }
    require(host_refused,"malformed hosted geometry on a unit host must refuse before no-op elision");
    auto unsupported=source.entities();unsupported.at("unit-dim-0").properties["dimension_version"]=999;
    bool dimension_refused=false;
    try {
        auto bad=from_entities(unsupported);const auto snapshot=bad.snapshot();
        (void)prepare_component_structural_directional_resize(snapshot,component_edit(snapshot),operators(),std::nullopt,"Unsupported unit dimension");
    } catch (const std::invalid_argument&) { dimension_refused=true; }
      catch (const DocumentError&) { dimension_refused=true; }
    require(dimension_refused,"unsupported affected dimension on a unit host must refuse");
    auto boundary=fixture(false,false,7,1,999,false);const auto actual=boundary.snapshot();
    const auto at_limit=prepare_component_structural_directional_resize(actual,component_edit(actual),operators(true),std::nullopt,"Shared 1000 associations");
    require(exact(at_limit.preview_snapshot().entities().at("unit-dim-998"),actual.entities().at("unit-dim-998")),
        "999 no-upsert unit associations plus one component association must succeed exactly");
    bool overflow=false;
    try { (void)fixture(false,false,7,1,1000,false); }
    catch (const DocumentError& error) {
        require(std::string(error.what()).find("dimension inventory exceeds its shared resolution bound")!=std::string::npos,
            "1001 associations must fail the actual shared source inventory gate");overflow=true;
    }
    require(overflow,"1001 source associations must refuse before mixed preparation");
    auto retirement=prepare_component_structural_directional_resize(source,component_edit(source,7,true),operators(),std::nullopt,"Reviewed retirement");
    require(mixed_selection_property_edit_retires_dependencies(retirement.intent()) &&
        !retirement.preview_snapshot().entities().contains("component-profile-dim"),"component reviewed dimension retirement must remain explicit");
}
void historical_coordinated_structural_rejects_profiles() {
    auto entities=fixture().snapshot().entities();
    for (const auto& [id,x]:std::vector<std::pair<std::string,double>>{{"ordinary-wall",20},{"baseline-wall",30}}) {
        Entity wall{id,"wall",{{"baseline",{{"start",{x,0.0}},{"end",{x+4,0.0}},{"sweep_radians",0.0}}},
            {"thickness_m",.14},{"height_m",2.4},{"elevation_m",0.0}},false};
        for (const auto& [key,value]:context.items()) wall.properties[key]=value;
        entities.emplace(id,std::move(wall));
    }
    entities.emplace("wall-catalog",catalog("wall-catalog","baseline-wall"));
    const std::vector<std::string> baseline{"baseline-wall","wall-catalog"};
    const auto phases=ModelPhases::create(baseline,baseline,{RemodelingAlternative{"alternative","Wall replacement",{}, {}}},"alternative");
    entities.emplace("phases",Entity{"phases","model_phases",{{"model",phases.to_json()}},false});
    auto document=from_entities(entities);const auto source=document.snapshot();
    const auto edits=captured(source,operators());require(!edits.empty() && !edits.front().profile_fields.is_null(),"fixture must capture actual full profiles");
    for (int version:{3,4,5,6}) {
        ConstraintAuthoringIntent semantic;semantic.message="Historical structural authority remains transform-only";
        auto proof=make_phase_constraint_authoring_intent(source,semantic,nullptr,true);
        ConstraintAuthoringIntent wall_semantic;wall_semantic.message=semantic.message;
        wall_semantic.wall_geometry_move=WallGeometryMoveIntent{{{"ordinary-wall",{21,2},{25,2}}},false};
        if (version==5) wall_semantic.wall_geometry_move->targets.push_back({"baseline-wall",{31,2},{35,2}});
        auto wall_child=make_phase_constraint_authoring_intent(source,wall_semantic,nullptr,true);
        if (version==5) {
            const auto plan=inspect_phase_wall_replacement_plan(source.entities(),{"baseline-wall"},"phases","alternative",true,true,true);
            require(plan.ready(),"historical qualified wall fixture must have a complete replacement inventory");
            PhaseWallReplacementAuthoring leaf;leaf.registry_id="phases";leaf.alternative_id="alternative";leaf.seed_wall_ids={"baseline-wall"};
            leaf.complete_presentations=true;leaf.complete_corner_windows=true;leaf.complete_hosted_instances=true;
            for (const auto& id:plan.required_entity_ids) leaf.identities.emplace(id,"proposed-"+id);
            for (const auto& id:plan.required_child_ids) leaf.identities.emplace(id,"proposed-"+id);
            for (const auto& root:plan.required_hosted_instance_ids) leaf.hosted_instance_identities.emplace(root,"proposed-"+root.second);
            wall_child.wall_replacement=encode_phase_wall_replacement_authoring(leaf);
        }
        Json rows=Json::array();for (const auto& edit:edits) rows.push_back(encode_structural_object_edit_intent(edit));
        Json body={{"version",version==6 ? 4 : version},{"wall_authoring",encode_phase_constraint_authoring_intent(wall_child)},
            {"roof_replacement",nullptr},{"slab_replacement",nullptr},
            {"structural_replacement",nullptr},{"ordinary_roof_edits",Json::array()},{"ordinary_slab_geometry",Json::array()},
            {"ordinary_structural_edits",rows}};
        if (version!=3) { body["stair_replacement"]=nullptr;body["ordinary_stair_transforms"]=Json::array(); }
        // Historical dimension completion is wrapper6 over body4. Direct inner10
        // is the newer slab geometry dialect, not a historical wrapper.
        const auto envelope=[&](const Json& authored) -> Json {
            if (version==6) return {{"version",6},{"authoring",authored},{"complete_hosted_component_dimensions",true}};
            return authored;
        };
        const auto diagnostic="historical coordinated version "+std::to_string(version)+": ";
        auto control=body;
        StructuralObjectEditIntent transform;transform.object_id="column";
        transform.transform=ArchitecturalGroupTransform{{0,0,0},{1,0,0},0,1,false,false};
        control["ordinary_structural_edits"]=Json::array({encode_structural_object_edit_intent(transform)});
        proof.coordinated_replacements=envelope(control);
        try { (void)encode_phase_constraint_authoring_intent(proof); }
        catch (const std::invalid_argument& error) {
            throw std::runtime_error(diagnostic+"transform-only control must be valid; actual error: "+error.what());
        }
        proof.coordinated_replacements=envelope(body);
        bool refused=false;
        try { (void)encode_phase_constraint_authoring_intent(proof); }
        catch (const std::invalid_argument& error) {
            if (std::string(error.what()).find("requires only typed transforms")==std::string::npos)
                throw std::runtime_error(diagnostic+"expected structural profile authority refusal; actual error: "+error.what());
            refused=true;
        }
        if (!refused) throw std::runtime_error(diagnostic+"full profile edits were accepted");
    }
}

struct ScopedProjectFile {
    std::filesystem::path directory,path;
    ScopedProjectFile() {
        const auto stamp=std::chrono::steady_clock::now().time_since_epoch().count();
        for (unsigned attempt=0;attempt<64;++attempt) {
            directory=std::filesystem::temp_directory_path()/("vertex-mixed-structural-"+std::to_string(stamp)+"-"+std::to_string(attempt));
            if (std::filesystem::create_directory(directory)) { path=directory/"history.bldproj";return; }
        }
        throw std::runtime_error("unique structural storage directory unavailable");
    }
    ~ScopedProjectFile() { std::error_code error;std::filesystem::remove_all(directory,error); }
};
void previous_reader_markers_refuse(const std::filesystem::path& file) {
    sqlite3* raw=nullptr;const auto u8=file.u8string();const std::string name(reinterpret_cast<const char*>(u8.data()),u8.size());
    const auto opened=sqlite3_open_v2(name.c_str(),&raw,SQLITE_OPEN_READWRITE,nullptr);
    std::unique_ptr<sqlite3,decltype(&sqlite3_close)> database(raw,&sqlite3_close);
    require(opened==SQLITE_OK,"controlled saved mixed project must open for marker downgrade");
    require(sqlite3_exec(database.get(),"BEGIN; PRAGMA user_version=263; UPDATE metadata SET value='263' WHERE key='format_version'; COMMIT",
        nullptr,nullptr,nullptr)==SQLITE_OK,"downgrade only controlled reader marker pair");database.reset();
    const auto digest=ProjectStore::file_sha256(file);bool refused=false;
    try { (void)ProjectStore::load(file); }
    catch (const StorageError& error) {
        require(error.code()==StorageErrorCode::unsupported_format && std::string(error.what()).find("requires reader 264")!=std::string::npos,
            "previous valid reader markers must refuse actual retained Properties41 at reader264");refused=true;
    }
    require(refused && ProjectStore::file_sha256(file)==digest,"reader263 refusal must retain every original file byte");
}
ApplyBoundaryConstraintChanges command(const DocumentSnapshot& source,const MixedSelectionPropertyEditPreparation& prepared) {
    ApplyBoundaryConstraintChanges result;result.expected_revision=source.revision();result.message=prepared.intent().at("message").get<std::string>();
    result.mixed_selection_edit_completion=true;result.mixed_selection_edit_intent=complete_mixed_selection_property_edit(source,prepared,{},{});return result;
}
void long_derived_hosted_alias_document_lifecycle() {
    auto document=fixture(true,true);const auto source=document.snapshot();const auto digest=document_snapshot_digest(source);
    auto ids=allocation(source);require(ids.has_value(),"long alias baseline must require structural allocation");
    require(ids->identities.size()==2 && ids->identities.contains("column") && ids->identities.contains("column-catalog") &&
        ids->hosted_instance_identities.size()==1 && ids->hosted_instance_identities.contains({"column-catalog","column-root"}) &&
        ids->dimension_identities.size()==2 && ids->dimension_identities.contains("column-dim") &&
        ids->dimension_identities.contains("column-profile-dim"),"long alias allocation must match the exact source replacement inventory");
    const std::string proposed_catalog="proposed-structural-00000000-0000-0000-0000-000000000001";
    const std::string proposed_instance="proposed-structural-component-00000000-0000-0000-0000-000000000002";
    ids->identities.at("column-catalog")=proposed_catalog;
    ids->hosted_instance_identities.at({"column-catalog","column-root"})=proposed_instance;
    const auto expected_alias=proposed_catalog+":instance:"+proposed_instance;
    // Native UUID destinations are individually portable IDs. Their derived
    // 56 + 10 + 66 byte presentation alias is a lifetime token, not an entity ID.
    require(proposed_catalog.size()==56 && proposed_instance.size()==66 && expected_alias.size()==132,
        "long alias regression must cross 128 bytes using individually valid destinations");
    const auto prepared=prepare_component_structural_directional_resize(source,component_edit(source),operators(),ids,"Long derived hosted alias");
    const auto& expected=prepared.preview_snapshot().entities();
    require(embedded_assembly_presentation_ids(expected).at({proposed_catalog,proposed_instance})==expected_alias,
        "long alias must derive from the actual qualified copied binding");
    require(prepared.hosted_binding_original_to_proposed().at({"column-catalog","column-root"})==
        std::pair<std::string,std::string>{proposed_catalog,proposed_instance},"long alias must retain exact qualified copy identities");
    const auto operation=command(source,prepared);const auto wire=command_to_json(operation);
    // The sealed preparer's preview bypasses Document publication admission;
    // this preview and live apply must both pass its replacement lifetime gate.
    const auto preview=Document::preview_command(source,operation);
    require(preview.entities()==expected && preview.revision()==source.revision()+1,
        "Document preview must admit the complete baseline result with a long derived alias");
    require(document_snapshot_digest(document.snapshot())==digest,"preparation and Document preview must preserve the full live source");
    document.apply(operation);const auto final=document.snapshot();
    require(final.entities()==preview.entities() && final.revision()==source.revision()+1,
        "one live command must publish the exact long-alias preview");
    for (const auto* id:{"component","component-dim-0","component-profile-dim","column","column-catalog","column-dim",
        "column-profile-dim","unit-column","unit-catalog","unit-dim-0"})
        require(exact(final.entities().at(id),source.entities().at(id)),"long alias publication must preserve original baseline and unit envelopes");
    document.undo(document.revision());require(document.snapshot().entities()==source.entities(),"one Undo must restore the full long-alias source");
    document.redo(document.revision());const auto redone=document.snapshot();
    require(redone.entities()==final.entities(),"one Redo must reproduce the exact long-alias result");
    ScopedProjectFile file;(void)ProjectStore::save(file.path,redone);auto loaded=ProjectStore::load(file.path);
    const auto restored=loaded.document.snapshot();
    require(restored.entities()==redone.entities() && restored.history().size()==redone.history().size() &&
        command_to_json(*restored.history().at(final.revision()).boundary_constraint_changes)==wire,
        "native reopen must replay the exact long-alias graph and retained command");
    require(embedded_assembly_presentation_ids(restored.entities()).at({proposed_catalog,proposed_instance})==expected_alias,
        "native reopen must retain the long qualified presentation alias");
    loaded.document.undo(loaded.document.revision());require(loaded.document.snapshot().entities()==source.entities(),
        "native reopened Undo must restore the entire original source");
    loaded.document.redo(loaded.document.revision());require(loaded.document.snapshot().entities()==final.entities(),
        "native reopened Redo must retain the exact long-alias result");
}
void retained_undo_redo_and_storage_floors() {
    for (bool baseline:{false,true}) {
        auto document=fixture(baseline,baseline);const auto source=document.snapshot();
        require(ProjectStore::required_format_version(source)<264 && ProjectStore::required_semantic_format_version(source)<264,
            "source fixture must start below reader264");
        const auto prepared=prepare_component_structural_directional_resize(source,component_edit(source),operators(),allocation(source),"Atomic Properties41");
        const auto operation=command(source,prepared);const auto wire=command_to_json(operation);
        require(wire.at("version")==52 && wire.at("mixed_selection_edit_intent").at("version")==41,"closed mixed41 must retain canonical command52");
        document.apply(operation);const auto final=document.snapshot();
        require(final.entities()==prepared.preview_snapshot().entities() && final.revision()==source.revision()+1,
            "one command must publish both physical lanes and their exact final dimensions");
        document.undo(document.revision());const auto undone=document.snapshot();require(undone.entities()==source.entities(),"one Undo must restore both families");
        document.redo(document.revision());require(document.snapshot().entities()==final.entities(),"one Redo must reproduce the exact mixed result");
        for (const auto& snapshot:{final,undone,document.snapshot()}) {
            require(ProjectStore::required_format_version(snapshot)==264 && ProjectStore::required_semantic_format_version(snapshot)==264,
                "canonical retained mixed41 must require exact native and semantic reader264");
            ScopedProjectFile file;(void)ProjectStore::save(file.path,snapshot);auto loaded=ProjectStore::load(file.path);
            const auto restored=loaded.document.snapshot();require(restored.entities()==snapshot.entities() && restored.history().size()==snapshot.history().size(),
                "native reopen must replay the exact retained mixed graph and history");
            require(command_to_json(*restored.history().at(final.revision()).boundary_constraint_changes)==wire,
                "native reopen must preserve exact canonical mixed command52 authority");
            require(ProjectStore::required_format_version(restored)==264,"native reopen must retain reader264");
            const auto extracted=file.directory/"extracted";extract_project(snapshot,extracted);
            std::ifstream input(extracted/"project.json");const auto manifest=Json::parse(input);
            require(manifest.at("exchange_version")==262 && manifest.at("revisions").at(final.revision()).at("boundary_constraint_changes")==wire,
                "extraction262 must retain the exact command52/Properties41 pair");
            previous_reader_markers_refuse(file.path);
        }
    }
}
void opaque_counterfeit_does_not_raise_floors() {
    auto document=fixture();const auto source=document.snapshot();const auto floor=ProjectStore::required_format_version(source);
    const auto semantic_floor=ProjectStore::required_semantic_format_version(source);
    const auto prepared=prepare_component_structural_directional_resize(source,component_edit(source),operators(),std::nullopt,"Opaque counterfeit shape");
    const auto wire=command_to_json(command(source,prepared));auto property=source.entities().at("property");
    property.properties["user_metadata"]={{"version",41},{"command",wire},{"intent",prepared.intent()}};property.extensions=wire;
    Entity opaque{"opaque-data","vendor",prepared.intent(),false};opaque.extensions={{"boundary_constraint_changes",wire}};
    document.apply(ApplyEntityChanges{.expected_revision=document.revision(),.entity_changes={EntityChange::upsert(property),EntityChange::upsert(opaque)},.message="Opaque metadata"});
    const auto head=document.snapshot();document.undo(document.revision());
    for (const auto& snapshot:{head,document.snapshot()}) {
        require(ProjectStore::required_format_version(snapshot)==floor && ProjectStore::required_semantic_format_version(snapshot)==semantic_floor,
            "opaque command-shaped metadata must retain prior reader floors");
        for (const auto& revision:snapshot.history()) require(!revision.boundary_constraint_changes,"opaque metadata must grant no canonical authority");
        ScopedProjectFile file;(void)ProjectStore::save(file.path,snapshot);const auto restored=ProjectStore::load(file.path).document.snapshot();
        require(restored.entities()==snapshot.entities() && ProjectStore::required_format_version(restored)==floor,"native storage must preserve opaque shapes without authority");
        const auto extracted=file.directory/"extracted";extract_project(snapshot,extracted);std::ifstream input(extracted/"project.json");
        require(Json::parse(input).at("exchange_version")==semantic_floor-2,"opaque mixed41 must retain its original extraction floor");
    }
}
} // namespace
int main() {
    sketch::testing::noninteractive_errors();const char* active="initialization";
    try {
        const std::pair<const char*,void(*)()> tests[]{
            {"supported_geometry_and_phase_partitions",supported_geometry_and_phase_partitions},
            {"closed_wire_and_source_fences",closed_wire_and_source_fences},
            {"allocation_and_freshness_refusals",allocation_and_freshness_refusals},
            {"strict_structural_forms_and_exact_capture",strict_structural_forms_and_exact_capture},
            {"complete_units_and_shared_dimension_bound",complete_units_and_shared_dimension_bound},
            {"historical_coordinated_structural_rejects_profiles",historical_coordinated_structural_rejects_profiles},
            {"long_derived_hosted_alias_document_lifecycle",long_derived_hosted_alias_document_lifecycle},
            {"retained_undo_redo_and_storage_floors",retained_undo_redo_and_storage_floors},
            {"opaque_counterfeit_does_not_raise_floors",opaque_counterfeit_does_not_raise_floors}};
        for (const auto& [name,test]:tests) { active=name;test(); }
        std::cout << "mixed_component_structural_resize_tests passed\n";return 0;
    } catch (const std::exception& error) { std::cerr << active << ": " << error.what() << '\n';return 1; }
}
