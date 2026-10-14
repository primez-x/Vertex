#include "sketch/mixed_selection_property_edit.hpp"
#include "sketch/assembly_dimension.hpp"
#include "sketch/assembly_model.hpp"
#include "sketch/boundary_dimension.hpp"
#include "sketch/building_entity.hpp"
#include "sketch/document_digest.hpp"
#include "sketch/model_phases.hpp"
#include "sketch/phase_constraint_authoring.hpp"
#include "sketch/plan_axis_resize.hpp"
#include "sketch/structural_hosted_components.hpp"
#include "sketch/assembly_phase_membership.hpp"
#include "sketch/slab_hosted_geometry_edit.hpp"
#include "sketch/slab_plan_geometry.hpp"
#include "sketch/project_store.hpp"
#include "sketch/project_exchange.hpp"
#include "support/noninteractive_errors.hpp"

#include <algorithm>
#include <sqlite3.h>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
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
Document structural_fixture(bool component_baseline=false,bool structural_baseline=false,int component_version=7,
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
std::optional<ComponentStructuralDirectionalResizeAllocation> structural_allocation(const DocumentSnapshot& source) {
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

std::vector<SlabGeometryEditIntent> slab_operators(bool unit=false) {
    SlabGeometryEditIntent edit;edit.slab_id="slab";edit.kind=SlabGeometryEditKind::resize_plan;
    edit.resize=SlabPlanAxisResize{unit ? 1. : 1.8,unit ? 1. : .65,{1,0},0};
    edit.coordinate_world_hosted_geometry=true;return {edit};
}
std::vector<RoofOpeningDirectionalResizeRequest> roof_operators(bool unit=false) {
    return {{{"roof","child"},unit ? 1. : 1.8,unit ? 1. : .65,{1,0},0}};
}
Document fixture(bool baseline=false,bool proposed=false,int version=7) {
    auto base=structural_fixture(baseline,baseline,version);auto entities=base.snapshot().entities();
    auto p=context;p["boundary"]=Json::array({
        {{"start",{0,0}},{"end",{4,0}},{"sweep_radians",0}},
        {{"start",{4,0}},{"end",{4,3}},{"sweep_radians",0}},
        {{"start",{4,3}},{"end",{0,3}},{"sweep_radians",0}},
        {{"start",{0,3}},{"end",{0,0}},{"sweep_radians",0}}});
    p["holes"]=Json::array();p["thickness_m"]=.2;p["elevation_m"]=.4;
    p["layers"]=Json::array({{{"id","slab-core"},{"thickness_m",.2}}});
    entities.emplace("slab",Entity{"slab","slab",p,false});
    AssemblyInstance root;root.id="slab-root";root.type_id="parent";
    root.placement=AssemblyPlacement{"slab",{.6,-.35},.23,1,true,.2,1};
    auto unselected=encode_building_entity(RectangularColumn{"unselected-column",{25,1,.4},2,2,3,0});
    for (const auto& [key,value]:context.items()) unselected.properties[key]=value;
    entities.emplace(unselected.id,unselected);
    AssemblyInstance untouched=root;untouched.id="unselected-root";untouched.placement->host_entity_id="unselected-column";
    auto& shared=entities.at("column-catalog");
    const auto original=AssemblyModel::from_json(shared.properties.at("model"));
    auto rows=original.instances();rows.push_back(root);rows.push_back(untouched);
    shared.properties["model"]=AssemblyModel::create(original.materials(),original.types(),rows).to_json();
    shared.properties["opaque_metadata"]={{"retain",Json::array({17,"shared-source-envelope",true})}};
    entities.emplace("slab-dim",dimension("slab-dim","column-catalog","slab-root",std::nullopt));
    auto roof=encode_building_entity(SlopedRoofPanel{"roof",{0,0,4},0,40,40,10,std::atan(.25),.2,.15,
        {{"child",4,5,3,2,RoofSkylight{.06,.15,.024},0}}});
    roof.properties["version"]=4;
    for (const auto& [key,value]:context.items()) roof.properties[key]=value;
    entities.emplace("roof",roof);
    entities.emplace("roof-catalog",catalog("roof-catalog","roof"));
    entities.emplace("roof-dim",dimension("roof-dim","roof-catalog","roof-root",std::nullopt));
    if (baseline) {
        const auto old=ModelPhases::from_json(entities.at("phases").properties.at("model"));
        auto ids=old.entity_ids();ids.insert(ids.end(),{"slab","slab-dim"});
        auto baseline_ids=old.baseline_ids();baseline_ids.insert(baseline_ids.end(),{"slab","slab-dim"});
        entities.at("phases").properties["model"]=ModelPhases::create(ids,baseline_ids,old.alternatives(),"alternative").to_json();
        const std::vector<AssemblyPhaseRoot> roots{{"column-catalog","column-root"},{"column-catalog","slab-root"}};
        entities.emplace("membership",Entity{"membership","assembly_phase_membership",
            {{"model",AssemblyPhaseMembership::create("phases",roots,roots,{{"alternative",{}, {}}}).to_json()},
             {"opaque_metadata","retain-companion"}},true});
    } else if (proposed) {
        const std::vector<std::string> ids{"slab","column","column-catalog","slab-dim","column-dim","column-profile-dim"};
        entities.emplace("phases",Entity{"phases","model_phases",{{"model",
            ModelPhases::create(ids,{},{{"alternative","Proposed source",{},ids}},"alternative").to_json()}},false});
    }
    return from_entities(entities);
}
std::optional<ComponentSlabDirectionalResizeAllocation> slab_allocation(const DocumentSnapshot& source) {
    auto edits=slab_operators();
    for (auto& edit:edits) edit.preserve_planar_source_geometry=slab_geometry_edit_requires_planar_source(source.entities().at(edit.slab_id),edit);
    const auto partition=partition_phase_slab_geometry_edits(source.entities(),edits);
    if (!partition.replacement) return std::nullopt;
    const auto& replacement=*partition.replacement;
    const auto plan=inspect_phase_slab_replacement_plan(source.entities(),replacement.seed_slab_ids,
        replacement.registry_id,replacement.alternative_id,true);
    require(plan.ready(),"shared baseline slab clone inventory must be admitted");
    ComponentSlabDirectionalResizeAllocation result;
    for (const auto& id:plan.required_entity_ids) result.identities.emplace(id,"slab-copy-"+id);
    for (const auto& id:plan.required_child_ids) result.identities.emplace(id,"slab-copy-"+id);
    for (const auto& binding:plan.required_hosted_instance_ids) result.hosted_instance_identities.emplace(binding,"slab-copy-"+binding.second);
    for (const auto& id:plan.required_dimension_ids) result.dimension_identities.emplace(id,"slab-copy-"+id);
    return result;
}
MixedSelectionPropertyEditPreparation prepare(const DocumentSnapshot& source,int mask=7,bool units=false,int version=7,bool retire=false) {
    return prepare_component_physical_directional_resize(source,component_edit(source,version,retire),
        mask&1 ? slab_operators(units) : std::vector<SlabGeometryEditIntent>{},
        mask&1 && !units ? slab_allocation(source) : std::nullopt,
        mask&2 ? operators(units) : std::vector<StructuralDirectionalResizeRequest>{},
        mask&2 && !units ? structural_allocation(source) : std::nullopt,
        mask&4 ? roof_operators(units) : std::vector<RoofOpeningDirectionalResizeRequest>{},"Combined original-source resize");
}
void family_pairs_and_partitions() {
    for (int mask:{3,5,6,7}) for (int phase:{0,1,2}) for (int version:{7,8,9}) {
        auto document=fixture(phase==1,phase==2,version);const auto source=document.snapshot();
        const auto digest=document_snapshot_digest(source);const auto result=prepare(source,mask,false,version);
        require(result.intent().at("version")==42 && result.intent().size()==16,"Props42 must have sixteen exact mandatory fields");
        require(result.intent().at("component_request").at("version")==version,"component request7/8/9 retains its authority");
        require(document_snapshot_digest(document.snapshot())==digest,"combined preparation must preserve source and history");
        require(replay_mixed_selection_property_edit(source,complete_mixed_selection_property_edit(source,result,{},{})).entities==
            result.preview_snapshot().entities(),"complete replay must reproduce the one combined candidate");
        if (mask&1) require(result.selected_original_to_proposed().contains("slab"),"slab selection correspondence must survive");
        if (mask&2) {
            for (const auto& op:operators()) {
                auto expected=stage_structural_plan_axis_resize_entity(source.entities().at(op.object_id),op.sx,op.sy,op.anchor_m,op.frame_rotation_radians);
                expected.id=result.selected_original_to_proposed().at(op.object_id);
                require(exact(expected,result.preview_snapshot().entities().at(expected.id)),"combined composition must retain exact native structural geometry");
            }
        }
        if (mask&4) require(result.roof_child_original_to_proposed().contains({"roof","child"}),"qualified skylight correspondence must survive");
        require_near(span(result.preview_snapshot().entities(),phase==1 ? "proposed-component-dim-0" : "component-dim-0")/
            span(source.entities(),"component-dim-0"),1.8,"component extent receives its one final carry");
    }
}
void shared_baseline_private_catalogs() {
    auto document=fixture(true);const auto source=document.snapshot();const auto result=prepare(source,3);
    const auto& final=result.preview_snapshot().entities();
    const auto slab=result.hosted_binding_original_to_proposed().at({"column-catalog","slab-root"});
    const auto column=result.hosted_binding_original_to_proposed().at({"column-catalog","column-root"});
    require(slab.first=="slab-copy-column-catalog" && column.first=="proposed-column-catalog" && slab.first!=column.first,
        "two baseline families must keep distinct private catalogs for a shared source catalog");
    require(exact(final.at("column-catalog"),source.entities().at("column-catalog")),"shared baseline catalog and all unselected rows remain exact");
    require(final.at(slab.first).properties.at("model").at("instances").size()==1 &&
        final.at(column.first).properties.at("model").at("instances").size()==1,"private copies contain only each family's selected root");
    for (const auto& id:{slab.first,column.first}) require(final.at(id).properties.at("opaque_metadata").dump()==
        source.entities().at("column-catalog").properties.at("opaque_metadata").dump(),"private copies preserve raw catalog metadata");
    require(!result.original_to_proposed().contains("column-catalog"),"ambiguous family-local catalog allocation is not flattened into global correspondence");
    const auto membership=AssemblyPhaseMembership::from_json(final.at("membership").properties.at("model"));
    require(membership.roots().size()==4 && membership.alternatives().front().demolished_roots.size()==2 &&
        membership.alternatives().front().proposed_roots.size()==2,"typed companion merge retains both independently derived consequences");
    require(result.phase_authorings().size()==2,"both private family proofs must survive");
    for (const auto& proof:result.phase_authorings()) require(decode_phase_constraint_authoring_intent(proof).source_snapshot_digest==
        document_snapshot_digest(source),"every family proof authenticates the original snapshot");
    require(final.at("membership").properties.at("opaque_metadata")=="retain-companion",
        "typed companion merge preserves the retained carrier envelope");
    const auto source_aliases=embedded_assembly_presentation_ids(source.entities());
    const auto final_aliases=embedded_assembly_presentation_ids(final);
    for (const auto& binding:std::vector<AssemblyPhaseRoot>{{"column-catalog","slab-root"},{"column-catalog","column-root"}})
        require(result.original_to_proposed().at(source_aliases.at(binding))==
            final_aliases.at(result.hosted_binding_original_to_proposed().at(binding)),"qualified roots expose single-valued global aliases");
}
void shared_ordinary_catalog_source_rows() {
    auto document=fixture();const auto source=document.snapshot();const auto result=prepare(source,7);
    const auto& before=source.entities().at("column-catalog");
    const auto& after=result.preview_snapshot().entities().at("column-catalog");
    const auto& old_rows=before.properties.at("model").at("instances");
    const auto& new_rows=after.properties.at("model").at("instances");
    require(old_rows.size()==new_rows.size(),"ordinary shared catalog retains complete row inventory");
    for (std::size_t i=0;i<old_rows.size();++i) {
        require(old_rows[i].at("id")==new_rows[i].at("id"),"ordinary union retains source row order");
        if (old_rows[i].at("id")=="unselected-root") require(old_rows[i].dump()==new_rows[i].dump(),
            "unselected hosted row remains byte-exact through disjoint family edits");
    }
    require(before.properties.at("opaque_metadata").dump()==after.properties.at("opaque_metadata").dump() &&
        before.properties.at("model").at("types").dump()==after.properties.at("model").at("types").dump() &&
        before.extensions.dump()==after.extensions.dump(),"ordinary shared catalog definitions and raw envelope remain exact");
    require(exact(source.entities().at("unselected-column"),result.preview_snapshot().entities().at("unselected-column")),
        "unselected physical owner remains exact");
}
void closed_wire_and_fences() {
    auto document=fixture();const auto source=document.snapshot();const auto valid=prepare(source).intent();
    for (const auto& [key,value]:valid.items()) {
        (void)value;auto wire=valid;wire.erase(key);
        invalid([&] {(void)validate_mixed_selection_property_edit_intent(wire);},"every Props42 field is mandatory");
    }
    for (const auto* key:{"base_intent","candidate_entities","correspondence","physical_authoring"}) {
        auto wire=valid;wire[key]=Json::object();
        invalid([&] {(void)validate_mixed_selection_property_edit_intent(wire);},"Props42 rejects caller consequence/proof authority");
    }
    for (const auto* key:{"expected_revision","source_snapshot_digest","source_authoring_digest","source_entities_digest","source_saved_revision"}) {
        auto wire=valid;wire["component_request"][key]=key==std::string_view{"expected_revision"} ? Json(1) :
            key==std::string_view{"source_saved_revision"} ? Json(2) : Json(std::string(64,'a'));
        invalid([&] {(void)validate_mixed_selection_property_edit_intent(wire);},"all five component fences must match outer source fences");
    }
    auto wire=valid;wire["slab_geometry"]=Json::array();wire["structural_axis_requests"]=Json::array();
    invalid([&] {(void)validate_mixed_selection_property_edit_intent(wire);},"at least two physical request lists required");
    wire=valid;wire["component_identity_reservations"]=Json::array({"forged"});
    invalid([&] {(void)replay_mixed_selection_property_edit_preparation(source,wire);},"component receipt reconstructed exactly");
    wire=valid;wire["room_reviews"]=Json::array({Json::object()});
    invalid([&] {(void)validate_mixed_selection_property_edit_intent(wire);},"combined lane grants no room authority");
    wire=valid;wire["structural_axis_requests"][0]["sx"]=0;
    invalid([&] {(void)validate_mixed_selection_property_edit_intent(wire);},"nonpositive factors refuse");
    for (int version:{39,40,41}) {
        wire=valid;wire["version"]=version;
        invalid([&] {(void)validate_mixed_selection_property_edit_intent(wire);},"historical resize dialects reject combined wire");
    }
    const auto authoring=component_edit(source);
    const auto old_slab=prepare_component_slab_directional_resize(source,authoring,slab_operators(),std::nullopt,"Historical39");
    const auto old_roof=prepare_component_roof_opening_directional_resize(source,authoring,roof_operators(),"Historical40");
    const auto old_structure=prepare_component_structural_directional_resize(source,authoring,operators(),std::nullopt,"Historical41");
    require(old_slab.intent().at("version")==39 && old_slab.intent().size()==13 &&
        old_roof.intent().at("version")==40 && old_roof.intent().size()==12 &&
        old_structure.intent().at("version")==41 && old_structure.intent().size()==13,
        "historical producers retain their exact dialects and field counts");
    for (auto old:std::vector<Json>{old_slab.intent(),old_roof.intent(),old_structure.intent()}) {
        const auto version=old.at("version").get<int>();
        old[version==39 ? "slab_geometry" : version==40 ? "roof_opening_axis_requests" : "structural_axis_requests"]=Json::array();
        invalid([&] {(void)validate_mixed_selection_property_edit_intent(old);},"historical dialects retain their nonempty family requirements");
    }
    invalid([&] {(void)prepare(source,1);},"typed producer refuses a single physical family");
}
void units_unknown_references_and_review() {
    auto document=fixture(true);const auto source=document.snapshot();const auto units=prepare(source,7,true);
    require(units.intent().at("slab_allocation").is_null() && units.intent().at("structural_allocation").is_null(),
        "baseline units consume no allocation slots");
    for (const auto* id:{"slab","column","beam","roof","slab-dim","column-dim","roof-dim"})
        require(exact(units.preview_snapshot().entities().at(id),source.entities().at(id)),"physical units retain exact source envelopes");
    auto ordinary=fixture();const auto reviewed=prepare(ordinary.snapshot(),7,false,7,true);
    require(!reviewed.preview_snapshot().entities().contains("component-profile-dim") &&
        mixed_selection_property_edit_retires_dependencies(reviewed.intent()),"explicit component retirement survives combined finalization");
    for (const auto* id:{"slab-dim","column-dim","roof-dim"}) {
        auto entities=ordinary.snapshot().entities();entities.at(id).properties["dimension_version"]=999;
        bool refused=false;
        try { auto future=from_entities(entities);(void)prepare(future.snapshot(),7,true); }
        catch (const std::invalid_argument&) { refused=true; }
        catch (const DocumentError&) { refused=true; }
        require(refused,"unknown affected references refuse before unit elimination");
    }
    auto entities=ordinary.snapshot().entities();
    entities.emplace("roof-phases",Entity{"roof-phases","model_phases",{{"model",
        ModelPhases::create({"roof"},{"roof"},{},std::nullopt).to_json()}},false});
    auto roof_phase=from_entities(entities);
    invalid([&] {(void)prepare(roof_phase.snapshot(),7,true);},"baseline-only registry skylight host refuses including units");
}
void cross_family_freshness() {
    auto document=fixture(true);const auto source=document.snapshot();
    auto slab=slab_allocation(source);auto structural=structural_allocation(source);
    slab->identities.at("column-catalog")=structural->identities.at("column-catalog");
    invalid([&] {(void)prepare_component_physical_directional_resize(source,component_edit(source),slab_operators(),slab,
        operators(),structural,roof_operators(),"Collision");},"cross-family private catalog identity collision refuses");
    slab=slab_allocation(source);slab->identities.at("slab")="proposed-component";
    invalid([&] {(void)prepare_component_physical_directional_resize(source,component_edit(source),slab_operators(),slab,
        operators(),structural,roof_operators(),"Collision");},"physical allocation colliding with component reservation refuses");
    slab=slab_allocation(source);slab->dimension_identities.at("slab-dim")=structural->dimension_identities.at("column-dim");
    invalid([&] {(void)prepare_component_physical_directional_resize(source,component_edit(source),slab_operators(),slab,
        operators(),structural,roof_operators(),"Collision");},"cross-family provisional dimension identity collision refuses");
    slab=slab_allocation(source);slab->hosted_instance_identities.at({"column-catalog","slab-root"})=
        structural->hosted_instance_identities.at({"column-catalog","column-root"});
    invalid([&] {(void)prepare_component_physical_directional_resize(source,component_edit(source),slab_operators(),slab,
        operators(),structural,{},"Cross-family instance allocation");},"fresh allocation slots must remain injective across family-local catalogs");
    const auto control=prepare(source,3);
    const auto alias=embedded_assembly_presentation_ids(control.preview_snapshot().entities()).at(
        {"proposed-column-catalog","proposed-column-root"});
    slab=slab_allocation(source);slab->dimension_identities.at("slab-dim")=alias;
    invalid([&] {(void)prepare_component_physical_directional_resize(source,component_edit(source),slab_operators(),slab,
        operators(),structural,{},"Cross-family computed alias");},"owned dimension destination cannot collide with another family's computed alias");
    Entity historical{alias,"label",{{"text","Undo-retained computed alias collision"}},false};
    document.apply(ApplyEntityChanges{.expected_revision=document.revision(),.entity_changes={EntityChange::upsert(historical)},.message="Retain alias identity"});
    document.undo(document.revision());
    invalid([&] {(void)prepare(document.snapshot(),3);},"computed aliases must be fresh against retained undone history");
    auto long_document=fixture(true);const auto long_source=long_document.snapshot();
    structural=structural_allocation(long_source);
    structural->identities.at("column-catalog")=std::string(56,'k');
    structural->hosted_instance_identities.at({"column-catalog","column-root"})=std::string(66,'r');
    const auto long_result=prepare_component_physical_directional_resize(long_source,component_edit(long_source),
        slab_operators(),slab_allocation(long_source),operators(),structural,{},"Long qualified alias");
    const auto long_alias=embedded_assembly_presentation_ids(long_result.preview_snapshot().entities()).at(
        {std::string(56,'k'),std::string(66,'r')});
    require(long_alias.size()==132,"derived presentation aliases retain their separate namespace above global ID byte limit");
}
void exact_allocations_strict_forms_and_staleness() {
    auto document=fixture(true);const auto source=document.snapshot();
    const auto authoring=component_edit(source);auto slab=slab_allocation(source);auto structural=structural_allocation(source);
    invalid([&] {(void)prepare_component_physical_directional_resize(source,authoring,slab_operators(),std::nullopt,
        operators(),structural,{},"Missing baseline allocation");},"changed slab baseline requires allocation");
    invalid([&] {(void)prepare_component_physical_directional_resize(source,authoring,slab_operators(),slab,
        operators(),std::nullopt,{},"Missing baseline allocation");},"changed structural baseline requires allocation");
    slab->hosted_instance_identities.clear();
    invalid([&] {(void)prepare_component_physical_directional_resize(source,authoring,slab_operators(),slab,
        operators(),structural,{},"Missing slot");},"complete hosted allocation inventory is mandatory");
    auto ordinary=fixture();const auto ordinary_source=ordinary.snapshot();
    for (bool units:{false,true}) {
        auto entities=ordinary_source.entities();auto circular=encode_building_entity(CircularColumn{"column",{2,1,.4},1,3,0});
        for (const auto& [key,value]:context.items()) circular.properties[key]=value;
        entities.at("column")=circular;auto changed=from_entities(entities);
        invalid([&] {(void)prepare(changed.snapshot(),7,units);},"circular columns refuse including units");
        entities=ordinary_source.entities();entities.at("beam").properties["end_m"][2]=3.2;
        auto sloped=from_entities(entities);invalid([&] {(void)prepare(sloped.snapshot(),7,units);},"sloped beams refuse including units");
    }
    auto oblique=operators();oblique[1].frame_rotation_radians=.3;
    invalid([&] {(void)prepare_component_physical_directional_resize(ordinary_source,component_edit(ordinary_source),slab_operators(),std::nullopt,
        oblique,std::nullopt,{},"Oblique");},"oblique anisotropy retains native refusal");
    auto entities=ordinary_source.entities();entities.at("column").properties["radius_m"]=.75;
    auto dormant=from_entities(entities);invalid([&] {(void)prepare(dormant.snapshot());},"source-dormant fields cannot be discarded by typed capture");
    const auto prepared=prepare(ordinary_source);
    ordinary.apply(ApplyEntityChanges{.expected_revision=ordinary.revision(),.entity_changes={EntityChange::upsert(
        Entity{"history-change","label",{{"text","History staleness"}},false})},.message="Change history"});
    ordinary.undo(ordinary.revision());
    invalid([&] { require_mixed_selection_property_edit_source_current(ordinary.snapshot(),prepared); },
        "restoring visible entities does not restore captured source history authority");
}
void asset_lifetime_collisions() {
    auto seed=fixture(true);const auto control=prepare(seed.snapshot(),3);
    const auto alias=embedded_assembly_presentation_ids(control.preview_snapshot().entities()).at(
        {"proposed-column-catalog","proposed-column-root"});
    for (const auto& token:std::vector<std::string>{"slab-copy-slab","proposed-column","proposed-component",alias})
        for (const bool undone:{false,true}) {
            auto document=fixture(true);
            const auto asset=Asset::create(token,"application/octet-stream",{std::byte{0x31}});
            document.apply(ApplyEntityChanges{document.revision(),{}, {AssetChange::upsert(asset)},"Retained asset identity"});
            if (undone) document.undo(document.revision());
            const auto source=document.snapshot();const auto digest=document_snapshot_digest(source);
            bool refused=false;
            try {(void)prepare(source,3);} catch (const std::invalid_argument&) {refused=true;}
            catch (const DocumentError&) {refused=true;}
            require(refused,"combined physical allocations and aliases cannot reuse current or undone asset identities");
            require(document_snapshot_digest(document.snapshot())==digest,"asset identity refusal preserves exact source and retained asset bytes");
        }
}
struct ScopedProjectFile {
    std::filesystem::path directory,path;
    ScopedProjectFile() {
        const auto stamp=std::chrono::steady_clock::now().time_since_epoch().count();
        for (unsigned attempt=0;attempt<64;++attempt) {
            directory=std::filesystem::temp_directory_path()/("vertex-combined-physical-"+std::to_string(stamp)+"-"+std::to_string(attempt));
            if (std::filesystem::create_directory(directory)) {path=directory/"history.bldproj";return;}
        }
        throw std::runtime_error("unique combined storage directory unavailable");
    }
    ~ScopedProjectFile() {std::error_code error;std::filesystem::remove_all(directory,error);}
};
ApplyBoundaryConstraintChanges command(const DocumentSnapshot& source,const MixedSelectionPropertyEditPreparation& prepared) {
    ApplyBoundaryConstraintChanges result;
    result.expected_revision=source.revision();result.message=prepared.intent().at("message").get<std::string>();
    result.mixed_selection_edit_completion=true;
    result.mixed_selection_edit_intent=complete_mixed_selection_property_edit(source,prepared,{},{});
    return result;
}
void persistence_and_reader_floors() {
    for (const bool baseline:{false,true}) {
        auto document=fixture(baseline);const auto source=document.snapshot();const auto prepared=prepare(source);
        const auto operation=command(source,prepared);const auto wire=command_to_json(operation);
        document.apply(operation);const auto applied=document.snapshot();
        require(applied.entities()==prepared.preview_snapshot().entities() && applied.revision()==source.revision()+1,
            "one Document event publishes the exact combined candidate");
        document.undo(document.revision());const auto undone=document.snapshot();
        require(undone.entities()==source.entities(),"one combined Undo restores the complete original source");
        document.redo(document.revision());const auto redone=document.snapshot();
        require(redone.entities()==applied.entities(),"one combined Redo restores the complete result");
        for (const auto& snapshot:{applied,undone,redone}) {
            require(ProjectStore::required_format_version(snapshot)==265 && ProjectStore::required_semantic_format_version(snapshot)==265,
                "applied/undone/redone canonical combined history requires reader265");
            ScopedProjectFile file;(void)ProjectStore::save(file.path,snapshot);
            {
                auto loaded=ProjectStore::load(file.path);const auto restored=loaded.document.snapshot();
                require(restored.entities()==snapshot.entities() && restored.history().size()==snapshot.history().size() &&
                    command_to_json(*restored.history().at(applied.revision()).boundary_constraint_changes)==wire,
                    "native reopen retains exact combined source graph and canonical history authority");
                require(ProjectStore::required_format_version(restored)==265,"reopen retains reader265");
            }
            extract_project(snapshot,file.directory/"extracted");std::ifstream input(file.directory/"extracted"/"project.json");
            const auto manifest=Json::parse(input);
            require(manifest.at("exchange_version")==263 &&
                manifest.at("revisions").at(applied.revision()).at("boundary_constraint_changes")==wire,
                "extraction263 retains exact command52/Props42");
            sqlite3* raw=nullptr;const auto u8=file.path.u8string();
            const std::string name(reinterpret_cast<const char*>(u8.data()),u8.size());
            const auto opened=sqlite3_open_v2(name.c_str(),&raw,SQLITE_OPEN_READWRITE,nullptr);
            std::unique_ptr<sqlite3,decltype(&sqlite3_close)> database(raw,&sqlite3_close);
            require(opened==SQLITE_OK,"controlled combined project opens for marker downgrade");
            require(sqlite3_exec(database.get(),"BEGIN; PRAGMA user_version=264; UPDATE metadata SET value='264' WHERE key='format_version'; COMMIT",
                nullptr,nullptr,nullptr)==SQLITE_OK,"downgrade only the controlled native reader marker pair");database.reset();
            const auto digest=ProjectStore::file_sha256(file.path);bool refused=false;
            try {(void)ProjectStore::load(file.path);} catch (const StorageError& error) {
                require(error.code()==StorageErrorCode::unsupported_format && std::string(error.what()).find("requires reader 265")!=std::string::npos,
                    "reader264 refuses actual retained Props42 with the reader265 reason");refused=true;
            }
            require(refused && ProjectStore::file_sha256(file.path)==digest,"underfloor refusal preserves every original file byte");
        }
    }
    auto document=fixture();const auto source=document.snapshot();const auto prepared=prepare(source);
    const auto wire=command_to_json(command(source,prepared));auto property=source.entities().at("property");
    property.properties["user_metadata"]={{"version",42},{"command",wire},{"intent",prepared.intent()}};property.extensions=wire;
    document.apply(ApplyEntityChanges{source.revision(),{EntityChange::upsert(property)},{},"Opaque combined lookalike"});
    const auto head=document.snapshot();document.undo(document.revision());
    for (const auto& snapshot:{head,document.snapshot()})
        require(ProjectStore::required_format_version(snapshot)==ProjectStore::required_format_version(source) &&
            ProjectStore::required_semantic_format_version(snapshot)==ProjectStore::required_semantic_format_version(source),
            "opaque current and undone command-shaped metadata grants no enclosing reader authority");
}
void aggregate_association_and_member_bounds() {
    auto document=fixture();auto entities=document.snapshot().entities();
    // Existing fixture has 8 affected associations: component2, column2,
    // beam1, unit1, slab1 and roof1. Every added unit association counts.
    for (int i=8;i<1000;++i) {
        const auto id="aggregate-unit-"+std::to_string(i);
        entities.emplace(id,dimension(id,"unit-catalog","unit-column-root",std::nullopt));
    }
    auto thousand=from_entities(entities);(void)prepare(thousand.snapshot(),7,true);
    entities.emplace("over-limit",dimension("over-limit","roof-catalog","roof-root",std::nullopt));
    bool refused=false;
    try { auto thousand_one=from_entities(entities);(void)prepare(thousand_one.snapshot(),7,true); }
    catch (const std::invalid_argument&) { refused=true; }
    catch (const DocumentError&) { refused=true; }
    require(refused,"1001 associations refuse across families including units");
    entities=document.snapshot().entities();std::vector<SlabGeometryEditIntent> slabs;
    for (int i=0;i<995;++i) {
        const auto id="unit-slab-"+std::to_string(i);auto owner=entities.at("slab");owner.id=id;
        owner.properties.at("layers")[0]["id"]=id+"-core";
        entities.emplace(id,owner);auto op=slab_operators(true).front();op.slab_id=id;slabs.push_back(op);
    }
    auto members=from_entities(entities);auto authoring=component_edit(members.snapshot());
    // 995 slabs + 3 structural + 1 roof child + 1 component = 1000.
    (void)prepare_component_physical_directional_resize(members.snapshot(),authoring,slabs,std::nullopt,
        operators(true),std::nullopt,roof_operators(true),"1000 members");
    slabs.push_back(slab_operators(true).front());
    invalid([&] {(void)prepare_component_physical_directional_resize(members.snapshot(),authoring,slabs,std::nullopt,
        operators(true),std::nullopt,roof_operators(true),"1001 members");},"1001 members refuse across all lanes");
}
} // namespace
int main(int argc,char** argv) {
    sketch::testing::noninteractive_errors();const char* active="initialization";
    try {
        const std::pair<const char*,void(*)()> tests[]{
            {"family_pairs_and_partitions",family_pairs_and_partitions},
            {"shared_baseline_private_catalogs",shared_baseline_private_catalogs},
            {"shared_ordinary_catalog_source_rows",shared_ordinary_catalog_source_rows},
            {"closed_wire_and_fences",closed_wire_and_fences},
            {"units_unknown_references_and_review",units_unknown_references_and_review},
            {"cross_family_freshness",cross_family_freshness},
            {"asset_lifetime_collisions",asset_lifetime_collisions},
            {"exact_allocations_strict_forms_and_staleness",exact_allocations_strict_forms_and_staleness},
            {"persistence_and_reader_floors",persistence_and_reader_floors},
            {"aggregate_association_and_member_bounds",aggregate_association_and_member_bounds}};
        for (const auto& [name,test]:tests) {
            if (argc>1 && std::string_view(argv[1])=="--asset-only" && std::string_view(name)!="asset_lifetime_collisions") continue;
            active=name;std::cout << name << " starting\n" << std::flush;
            test();std::cout << name << " passed\n" << std::flush;
        }
        return 0;
    } catch (const std::exception& error) { std::cerr << active << ": " << error.what() << '\n';return 1; }
}
