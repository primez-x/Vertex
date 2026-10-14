#include "sketch/mixed_selection_property_edit.hpp"
#include "sketch/assembly_model.hpp"
#include "sketch/boundary_dimension.hpp"
#include "sketch/building_entity.hpp"
#include "sketch/document_digest.hpp"
#include "sketch/phase_constraint_authoring.hpp"
#include "sketch/model_phases.hpp"
#include "sketch/plan_axis_resize.hpp"
#include "sketch/phase_roof_resize.hpp"
#include "sketch/roof_entity_codec.hpp"
#include "sketch/assembly_geometry.hpp"
#include "sketch/architecture.hpp"
#include "sketch/roof_join_semantics.hpp"
#include "sketch/roof_clone.hpp"
#include <BRepBndLib.hxx>
#include <Bnd_Box.hxx>
#include <Precision.hxx>
#include "sketch/project_store.hpp"
#include "sketch/project_exchange.hpp"
#include "sketch/sheet_view_entity_codec.hpp"
#include "support/noninteractive_errors.hpp"
#include "support/detached_document_snapshot.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <limits>
#include <numbers>
#include <memory>
#include <stdexcept>
#include <sstream>
#include <sqlite3.h>

namespace {
using namespace sketch;
using Json=nlohmann::json;
using Entities=std::map<std::string,Entity,std::less<>>;
void require(bool value,const char* message) { if (!value) throw std::runtime_error(message); }
void require_near(double actual,double expected,const char* message) {
    if (!std::isfinite(actual) || std::abs(actual-expected)>=1e-8)
        throw std::runtime_error(std::string(message)+": expected "+std::to_string(expected)+", observed "+std::to_string(actual));
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
Json receipt(int metres) {
    return {{"version",1},{"original_expression",std::to_string(metres)+" m"},{"entered_unit","m"},
        {"exact_metres",{{"numerator",metres},{"denominator",1}}},{"vendor","receipt metadata"}};
}
Entity roof(const std::string& id="roof",int form=0,bool flat=false) {
    Entity value;
    if (form==0) value=encode_building_entity(SlopedRoofPanel{.id=id,.base_position={1,0,4},
        .run=2,.span=2,.rise=flat ? 0. : 1.,.pitch_radians=flat ? 0. : std::atan(.5),.overhang=flat ? 0. : .2,.thickness=.25});
    else if (form==1) value=encode_building_entity(GableRoof{.id=id,.base_position={1,0,4},
        .length=6,.span=2,.rise=1,.pitch_radians=std::atan(1.),.overhang=.2,.thickness=.25});
    else value=encode_building_entity(HipRoof{.id=id,.base_position={1,0,4},
        .length=6,.span=2,.rise=1,.pitch_radians=std::atan(1.),.overhang=.2,.thickness=.25});
    for (const auto& [key,row]:context.items()) value.properties[key]=row;
    value.properties["vendor"]={{"host",{{"roof_id",id}}},{"numeric",7}};
    value.properties["quantity_entries"]={{"/base_position_m/2",receipt(4)},
        {form==0 ? "/run_m" : "/length_m",receipt(form==0 ? 2 : 6)},
        {"/vendor/numeric",{{"version",81},{"opaque",true}}}};
    value.extensions={{"vendor",{{"retain",Json::array({2,"raw"})}}}};
    return value;
}
Document fixture(int version=7,bool baseline=false,std::size_t dimensions=1,int form=0,bool flat=false) {
    AssemblyProfile profile;profile.id="profile";
    profile.outer={{{0,0},{4,0},0},{{4,0},{4,3},0},{{4,3},{0,3},0},{{0,3},{0,0},0}};
    profile.elevation_m=.4;profile.height_m=.6;if (version==7) profile.topology=topology("");
    AssemblyType body;body.id="body";body.name="Source body";body.profiles={profile};
    AssemblyInstance instance;instance.id="component";instance.type_id="body";
    instance.root_transform=AssemblyTransform{{5,1,.75},.29,1,true};
    auto owner=encode_document_assembly_instance({"component","assembly_instance",context},{"catalog",instance});
    std::vector<Entity> rows{{"property","property",{{"name","Mixed roof source"}},false},
        {"building","building",{{"property_id","property"}},false},
        {"floor","floor",{{"building_id","building"}},false},{"layer","layer",{{"floor_id","floor"}},false},
        {"catalog","assembly_model",{{"model",AssemblyModel::create({}, {body},{}).to_json()}},false},owner,roof("roof",form,flat),roof("unit",form,flat)};
    for (std::size_t i=0;i<dimensions;++i) {
        BoundaryDimension d;d.id="dimension-"+std::to_string(i);d.boundary_id="component";
        d.kind=version==7 ? BoundaryDimensionKind::assembly_profile : BoundaryDimensionKind::assembly_extent;
        d.text_position={7,-2};AssemblyDimensionTarget target;target.component={"catalog","component",std::string{"component"},{}};
        target.axis=AssemblyDimensionAxis::width;if (version==7) target.profile=selector("");
        d.assembly_target=target;rows.push_back(encode_boundary_dimension_entity(d));
    }
    if (baseline) {
        std::vector<std::string> ids{"component"};for (std::size_t i=0;i<dimensions;++i) ids.push_back("dimension-"+std::to_string(i));
        const auto phases=ModelPhases::create(ids,ids,{RemodelingAlternative{"alternative","Choice",{}, {}}},"alternative");
        rows.push_back({"phases","model_phases",{{"model",phases.to_json()}},false});
    }
    return Document::create(rows);
}
PhaseAssemblyInstancePropertyEditAuthoring component_edit(const DocumentSnapshot& source,int version=7,bool retire=false) {
    const auto model=AssemblyModel::from_json(source.entities().at("catalog").properties.at("model"));
    auto profile=model.types().front().profiles.front();
    profile.planar_affine=AssemblyProfilePlanarAffine{{0,0},{0,0},1.8,0,0,.65};profile.topology=topology("new-");
    AssemblyInstancePropertyEditIntent intent;intent.target={"catalog","component",std::string{"component"},{}};
    intent.profile_geometry.operation=AssemblyInstancePropertyPatchOperation::set;intent.profile_geometry.value=std::vector<AssemblyProfile>{profile};
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
std::vector<RoofPlanResizeIntent> operators(bool units=false) {
    return {{"unit",1,1,{1,1},0,PlanAxisResizeAnchorMode::shared_frame},
        {"roof",units ? 1. : 2.,units ? 1. : 3.,{1,1},0,PlanAxisResizeAnchorMode::shared_frame}};
}

std::array<double,6> native_box(const Entity& owner) {
    const auto shape=make_roof_structure_shape(decode_roof_entity(owner));
    require(!shape.IsNull() && solid_volume(shape)>0,"roof is a genuine positive-volume native solid");
    Bnd_Box bounds;BRepBndLib::Add(shape,bounds);
    std::array<double,6> result;bounds.Get(result[0],result[1],result[2],result[3],result[4],result[5]);return result;
}
void hand_geometry_and_native_extent_solver() {
    const auto flat=roof("roof",0,true);
    const auto bounds=native_box(flat);
    const std::array<double,6> expected{1,0,3.75,3,2,4};
    for (std::size_t i=0;i<6;++i) require(std::abs(bounds[i]-expected[i])<1e-6,"flat source native hand bounds");
    require_near(solid_volume(make_roof_structure_shape(decode_roof_entity(flat))),1,"flat hand source volume");
    for (const auto& op:std::vector<RoofPlanResizeIntent>{{"roof",1.5,1,{1,0},0,PlanAxisResizeAnchorMode::shared_frame},
        {"roof",1,1.5,{1,0},0,PlanAxisResizeAnchorMode::shared_frame}}) {
        const auto result=replay_roof_plan_resize_entity(flat,op);const auto box=native_box(result);
        require(std::abs(box[0]-1)<1e-6 && std::abs(box[1])<1e-6,"moving-base hand anchors retain near edges");
        require(std::abs(box[3]-(op.scale_x==1.5 ? 4 : 3))<1e-6 &&
            std::abs(box[4]-(op.scale_y==1.5 ? 3 : 2))<1e-6,"hand side resize reaches requested far edge");
        require_near(solid_volume(make_roof_structure_shape(decode_roof_entity(result))),1.5,"flat resized hand volume");
        require(capture_roof_plan_resize(flat,result,op).has_value(),"actual operator captures full hand roof");
    }
    for (int form:{0,1,2}) for (bool swapped:{false,true}) for (int axis:{0,1,2}) {
        const auto owner=roof("roof",form);const auto before=plan_axis_resize_bounds(owner);
        const RoofPlanResizeIntent op{"roof",axis==1 ? 1. : 1.4,axis==0 ? 1. : 1.6,{-.5,-1},
            swapped ? std::numbers::pi/2 : 0.,PlanAxisResizeAnchorMode::shared_frame};
        const auto result=replay_roof_plan_resize_entity(owner,op);const auto after=plan_axis_resize_bounds(result);
        const auto sx=swapped ? op.scale_y : op.scale_x,sy=swapped ? op.scale_x : op.scale_y;
        require_near(after.maximum.x-after.minimum.x,sx*(before.maximum.x-before.minimum.x),"native roof X extent includes retained overhang and normal thickness");
        require_near(after.maximum.y-after.minimum.y,sy*(before.maximum.y-before.minimum.y),"native roof Y extent includes retained overhang and normal thickness");
        require_near(after.minimum.x,op.anchor.x+sx*(before.minimum.x-op.anchor.x),"source-derived anchored native X");
        require_near(after.minimum.y,op.anchor.y+sy*(before.minimum.y-op.anchor.y),"source-derived anchored native Y");
        for (const auto* key:{"rise_m","thickness_m","overhang_m","vendor"})
            require(result.properties.at(key).dump()==owner.properties.at(key).dump(),"retained authored roof fields remain raw exact");
        require(result.properties.at("base_position_m").at(2).dump()==owner.properties.at("base_position_m").at(2).dump(),"authored base Z remains exact");
        require(result.properties.at("quantity_entries").at("/base_position_m/2").dump()==owner.properties.at("quantity_entries").at("/base_position_m/2").dump(),"unaffected Z receipt remains exact");
        require(result.properties.at("quantity_entries").at("/vendor/numeric").dump()==owner.properties.at("quantity_entries").at("/vendor/numeric").dump(),"opaque unowned receipt survives");
        require(result.extensions.at("vendor")==owner.extensions.at("vendor"),"opaque extensions survive native resize");
        const auto capture=capture_roof_plan_resize(owner,result,op);
        require(capture && encode_roof_plan_resize_intent(*capture)==encode_roof_plan_resize_intent(op),"actual independent factors/frame remain authority");
        const auto& archive=result.extensions.at(std::string(roof_plan_resize_derivations_key));
        require(archive.at("version")==4 && archive.at("operations").size()==1,"shared-frame native resize owns archive4");
        require(archive.at("operations")[0].at("operation")==encode_roof_plan_resize_intent(op),"archive retains the actual v2 shared-frame operator");
        if (form==0 && sx!=1) require(std::abs(native_box(result)[2]-native_box(owner)[2])>1e-5,"sloped normal extrusion Z bounds change with pitch");
        auto counterfeit=result;counterfeit.extensions["vendor"]["counterfeit"]=true;
        invalid([&] { (void)capture_roof_plan_resize(owner,counterfeit,op); },"full raw capture excludes opaque candidate changes");
    }
    auto opaque=flat;opaque.properties["quantity_entries"]["/run_m"]={{"version",81},{"opaque",true}};
    invalid([&] { (void)replay_roof_plan_resize_entity(opaque,{"roof",2,1,{1,0},0,PlanAxisResizeAnchorMode::shared_frame}); },"affected opaque receipt refuses");
    const double distinct=std::nextafter(1.,2.);
    const RoofPlanResizeIntent tiny{"roof",1,distinct,{1,0},0,PlanAxisResizeAnchorMode::shared_frame};
    const auto result=replay_roof_plan_resize_entity(flat,tiny);
    const auto capture=capture_roof_plan_resize(flat,result,tiny);
    require(capture && capture->scale_x==1 && capture->scale_y==distinct,"existing roof proof preserves distinct nextafter factors");
    invalid([&] { (void)replay_roof_plan_resize_entity(flat,{"roof",1,distinct,{1,0},.2,PlanAxisResizeAnchorMode::shared_frame}); },"distinct oblique anisotropy refuses");
}

void hand_mixed_native_boxes() {
    auto seed=fixture(7,false,0,0,true);auto entities=seed.snapshot().entities();
    AssemblyProfile profile;profile.id="profile";
    profile.outer={{{0,0},{2,0},0},{{2,0},{2,1},0},{{2,1},{0,1},0},{{0,1},{0,0},0}};
    profile.elevation_m=.25;profile.height_m=1.5;profile.topology=topology("");
    AssemblyType body;body.id="body";body.name="Hand source component";body.profiles={profile};
    entities.at("catalog").properties["model"]=AssemblyModel::create({}, {body},{}).to_json();
    auto instance=decode_document_assembly_instance(entities.at("component"));
    instance.instance.root_transform=AssemblyTransform{{5,1,.75},0,1};
    entities.at("component")=encode_document_assembly_instance(entities.at("component"),instance);
    std::vector<Entity> rows;for (const auto& [id,entity]:entities) { (void)id;rows.push_back(entity); }
    auto document=Document::create(rows);const auto source=document.snapshot();
    const auto component_shape=[&](const Entities& actual) {
        AssemblyExpansionBudget budget;
        return make_assembly_geometry(expand_document_assembly_instance(actual.at("component"),actual,budget)).shape;
    };
    const auto verify=[&](const TopoDS_Shape& shape,const std::array<double,6>& expected,double volume) {
        require_near(solid_volume(shape),volume,"mixed hand native volume");
        Bnd_Box box;BRepBndLib::AddOptimal(shape,box,false,false);
        box.SetGap(0.0);std::array<double,6> bounds;
        box.Get(bounds[0],bounds[1],bounds[2],bounds[3],bounds[4],bounds[5]);
        // AddOptimal still uses Precision::Confusion for numerical bounds
        // when shape tolerances are disabled. Include coordinate roundoff.
        for (std::size_t i=0;i<6;++i) if (std::abs(bounds[i]-expected[i])>
            Precision::Confusion()+64*std::numeric_limits<double>::epsilon()*
                std::max({1.0,std::abs(bounds[i]),std::abs(expected[i])})) {
            std::ostringstream failure;
            failure<<std::setprecision(17)<<"mixed hand actual native solid bound "<<i
                <<": expected "<<expected[i]<<", observed "<<bounds[i];
            throw std::runtime_error(failure.str());
        }
    };
    verify(component_shape(source.entities()),{5,1,1,7,2,2.5},3);
    for (bool width:{false,true}) {
        auto authoring=component_edit(source);
        authoring.intents[0].profile_geometry.value->at(0).planar_affine=
            width ? AssemblyProfilePlanarAffine{{0,0},{2,0},1.5,0,0,1} :
                AssemblyProfilePlanarAffine{{0,0},{0,.5},1,0,0,1.5};
        const std::vector<RoofPlanResizeIntent> requests{{"roof",width ? 1.5 : 1.,width ? 1. : 1.5,{1,0},0,PlanAxisResizeAnchorMode::shared_frame}};
        const auto prepared=prepare_component_roof_directional_resize(source,authoring,requests,"Hand side gesture");
        const auto& result=prepared.preview_snapshot().entities();
        verify(component_shape(result),width ? std::array<double,6>{7,1,1,10,2,2.5} :
            std::array<double,6>{5,1.5,1,7,3,2.5},4.5);
        verify(make_roof_structure_shape(decode_roof_entity(result.at("roof"))),
            width ? std::array<double,6>{1,0,3.75,4,2,4} : std::array<double,6>{1,0,3.75,3,3,4},1.5);
        require(exact(result.at("unit"),source.entities().at("unit")),"unselected roof is untouched");
        require(document.snapshot().entities()==source.entities(),"detached native hand preview does not publish");
    }
}
void mixed_wire_and_retirement() {
    for (int version:{7,8,9}) for (bool baseline:{false,true}) {
        auto document=fixture(version,baseline);const auto source=document.snapshot();const auto digest=document_snapshot_digest(source);
        const auto preparation=prepare_component_roof_directional_resize(source,component_edit(source,version),operators(),"Mixed roof geometry");
        const auto& wire=preparation.intent();const auto& final=preparation.preview_snapshot().entities();
        require(wire.at("version")==44 && wire.size()==12,"Properties44 must have exactly twelve mandatory fields");
        require(wire.at("component_request").at("version")==version,"retain component request 7/8/9 authority");
        require(wire.at("roof_plan_resizes").at(0).at("roof_id")=="roof","roof operators must sort independently");
        for (const auto& row:wire.at("roof_plan_resizes"))
            require(row.size()==7 && row.at("version")==2 && row.at("anchor_mode")=="shared_frame","Properties44 requires closed seven-field v2 shared-frame operators including units");
        require(exact(final.at("roof"),replay_roof_plan_resize_entities(source.entities(),std::vector<RoofPlanResizeIntent>{{"roof",2,3,{1,1},0,PlanAxisResizeAnchorMode::shared_frame}}).at("roof")),"composed roof must equal native raw stage exactly");
        require(exact(final.at("unit"),source.entities().at("unit")),"unit owner must stay byte exact");
        require(preparation.selected_original_to_proposed().at("roof")=="roof" && preparation.selected_original_to_proposed().at("unit")=="unit","physical lane introduces no owner IDs");
        require(document_snapshot_digest(document.snapshot())==digest,"source/history must remain unchanged");
        require(replay_mixed_selection_property_edit_preparation(source,wire).intent()==wire,"wire replays independently");
        auto bad=wire;bad["candidate_entities"]=Json::object();
        invalid([&] { (void)validate_mixed_selection_property_edit_intent(bad); },"closed wire rejects supplied candidates");
        for (const auto* key:{"version","expected_revision","source_snapshot_digest","source_authoring_digest","source_entities_digest",
            "source_saved_revision","component_request","component_identity_reservations","roof_plan_resizes","room_reviews","phase_room_reviews","message"}) {
            bad=wire;bad.erase(key);invalid([&] { (void)validate_mixed_selection_property_edit_intent(bad); },"all twelve fields are mandatory");
        }
        for (const auto* key:{"expected_revision","source_snapshot_digest","source_authoring_digest","source_entities_digest","source_saved_revision"}) {
            bad=wire;bad["component_request"][key]=std::string_view(key).find("digest")!=std::string_view::npos ? Json(std::string(64,'a')) : Json(913);
            invalid([&] { (void)validate_mixed_selection_property_edit_intent(bad); },"all five component fences must match outer source");
        }
        bad=wire;bad["roof_plan_resizes"].push_back(bad["roof_plan_resizes"][0]);
        invalid([&] { (void)validate_mixed_selection_property_edit_intent(bad); },"duplicate/unsorted roof operators refuse");
        bad=wire;bad["roof_plan_resizes"][0]["scale_x"]=0;
        invalid([&] { (void)validate_mixed_selection_property_edit_intent(bad); },"nonpositive factors refuse");
        bad=wire;bad["roof_plan_resizes"][0]["scale_y"]=std::numeric_limits<double>::infinity();
        invalid([&] { (void)validate_mixed_selection_property_edit_intent(bad); },"nonfinite factors refuse before wire manufacture");
        bad=wire;bad["roof_plan_resizes"][0]["anchor_m"]=Json::array({1,2,3});
        invalid([&] { (void)validate_mixed_selection_property_edit_intent(bad); },"anchor has exactly two coordinates");
        bad=wire;bad["roof_plan_resizes"][0]["profile_fields"]=Json::object();
        invalid([&] { (void)validate_mixed_selection_property_edit_intent(bad); },"closed roof rows reject caller profiles");
        bad=wire;bad["roof_plan_resizes"]=Json::array();
        invalid([&] { (void)validate_mixed_selection_property_edit_intent(bad); },"roof roster stays nonempty");
        bad=wire;bad["message"]=std::string(1025,'x');
        invalid([&] { (void)validate_mixed_selection_property_edit_intent(bad); },"message byte bound refuses");
        bad=wire;bad["room_reviews"].push_back(Json::object());
        invalid([&] { (void)validate_mixed_selection_property_edit_intent(bad); },"no room review authority");
        for (int historical:{39,40,41,42,43}) {
            bad=wire;bad["version"]=historical;
            invalid([&] { (void)validate_mixed_selection_property_edit_intent(bad); },"historical dialects cannot borrow roof operators");
        }
        if (version==7) {
            const auto retired=prepare_component_roof_directional_resize(source,component_edit(source,version,true),operators(),"Explicit retirement");
            require(mixed_selection_property_edit_retires_dependencies(retired.intent()),"explicit component retirement remains visible");
            require(!retired.preview_snapshot().entities().contains(baseline ? "proposed-dimension-0" : "dimension-0"),"reviewed component dimension retires in one composition");
            if (baseline) require(exact(retired.preview_snapshot().entities().at("dimension-0"),source.entities().at("dimension-0")),"baseline original dimension remains exact");
        }
    }
}

DocumentSnapshot replaced(const DocumentSnapshot& original,const Entities& entities) {
    return sketch::test::DetachedDocumentSnapshotFixture::mutate(original,[&](auto& fixture) { fixture.entities()=entities; });
}
void shared_frame_lane_entrances() {
    auto document=fixture();const auto source=document.snapshot();const auto authoring=component_edit(source);
    const auto component=prepare_phase_assembly_instance_property_edit(source,authoring);
    for (bool units:{false,true}) {
        const auto prepared=prepare_component_roof_directional_resize(source,authoring,operators(units),"Shared-frame entrance control");
        require_roof_directional_resize_source_admission(source,operators(units));
        preflight_component_roof_directional_resize(source,authoring,operators(units));
        for (std::size_t index:{std::size_t{0},std::size_t{1}}) {
            for (const auto mode:{PlanAxisResizeAnchorMode::physical_edge,static_cast<PlanAxisResizeAnchorMode>(81)}) {
                auto requests=operators(units);requests[index].anchor_mode=mode;
                invalid([&] { require_roof_directional_resize_source_admission(source,requests); },"source-only admission rejects legacy or invalid anchor modes including units");
                invalid([&] { preflight_component_roof_directional_resize(source,authoring,requests); },"full admission rejects legacy or invalid anchor modes including units");
                invalid([&] { (void)prepare_component_roof_directional_resize(source,authoring,requests,"Wrong anchor mode"); },"typed Properties44 producer rejects legacy or invalid anchor modes including units");
                std::sort(requests.begin(),requests.end(),[](const auto& a,const auto& b) { return a.roof_id<b.roof_id; });
                invalid([&] { (void)compose_component_roof_directional_resize(source,component,requests,"Wrong anchor mode"); },"composer cannot bypass shared-frame source admission");
            }
            const auto reject_wire=[&](const Json& row) {
                auto wire=prepared.intent();wire["roof_plan_resizes"][index]=row;
                invalid([&] { (void)validate_mixed_selection_property_edit_intent(wire); },"closed Properties44 rejects legacy or invalid roof wire including units");
                invalid([&] { (void)replay_mixed_selection_property_edit_preparation(source,wire); },"replay cannot borrow legacy roof authority including units");
            };
            auto legacy=decode_roof_plan_resize_intent(prepared.intent().at("roof_plan_resizes")[index]);
            legacy.anchor_mode=PlanAxisResizeAnchorMode::physical_edge;
            const auto legacy_wire=encode_roof_plan_resize_intent(legacy);
            require(legacy_wire.size()==6 && legacy_wire.at("version")==1,"historical physical-edge codec remains exact six-field v1");
            reject_wire(legacy_wire);
            auto row=prepared.intent().at("roof_plan_resizes")[index];
            row["anchor_mode"]="physical_edge";reject_wire(row);
            row["anchor_mode"]="future_mode";reject_wire(row);
            row["anchor_mode"]=81;reject_wire(row);
            row["anchor_mode"]=nullptr;reject_wire(row);
            row=prepared.intent().at("roof_plan_resizes")[index];row.erase("anchor_mode");reject_wire(row);
            row=prepared.intent().at("roof_plan_resizes")[index];row["version"]=1;reject_wire(row);
        }
    }
}
void admission_units_dependencies_and_budgets() {
    auto document=fixture();const auto original=document.snapshot();const auto authoring=component_edit(original);
    for (const auto* key:{"roof_openings","transform","vertical_placement","phase_id"}) {
        auto entities=original.entities();entities.at("unit").properties[key]=Json::array();
        const auto source=replaced(original,entities);
        invalid([&] { preflight_component_roof_directional_resize(source,authoring,operators()); },"unit forbidden source field refuses before manufacture");
        invalid([&] { require_roof_directional_resize_source_admission(source,operators()); },"source-only gate uses actual roofs without synthetic component requests");
    }
    for (const auto* id:{"roof","unit"}) {
        auto entities=original.entities();const auto phases=ModelPhases::create({id},{id},{});
        entities.emplace("baseline-only",Entity{"baseline-only","model_phases",{{"model",phases.to_json()}},false});
        invalid([&] { preflight_component_roof_directional_resize(replaced(original,entities),authoring,operators()); },"baseline-only roof membership refuses including units");
        entities=original.entities();entities.at(id).properties["version"]=2;entities.at(id).properties["roof_openings"]=Json::array();
        invalid([&] { require_roof_directional_resize_source_admission(replaced(original,entities),operators()); },"opening schema refuses even empty");
        entities=original.entities();entities.emplace("future-dimension",Entity{"future-dimension","dimension",
            {{"dimension_version",81},{"target",{{"entity_id",id}}}},false});
        invalid([&] { preflight_component_roof_directional_resize(replaced(original,entities),authoring,operators()); },"unsupported roof-owned dimension refuses");
        entities=original.entities();entities.emplace("constraint",Entity{"constraint","constraint",
            {{"version",81},{"bindings",Json::array({{{"owner_id",id}}})}},false});
        invalid([&] { require_roof_directional_resize_source_admission(replaced(original,entities),operators()); },"unknown affected persistent constraint refuses");
    }
    auto entities=original.entities();
    entities.emplace("join",Entity{"join","roof_join",roof_join_json(RoofJoin{"join",{"roof","unit"}}),false});
    invalid([&] { require_roof_directional_resize_source_admission(replaced(original,entities),operators()); },"affected actual roof join refuses");
    entities=original.entities();
    auto model=AssemblyModel::from_json(entities.at("catalog").properties.at("model"));
    AssemblyInstance hosted;hosted.id="hosted";hosted.type_id="body";
    hosted.placement=AssemblyPlacement{.host_entity_id="unit"};
    entities.at("catalog").properties["model"]=AssemblyModel::create({},model.types(),{hosted}).to_json();
    invalid([&] { preflight_component_roof_directional_resize(replaced(original,entities),authoring,operators()); },"actual typed embedded hosted root refuses units");
    entities=original.entities();entities.at("unit").properties["vendor"]["roof_openings"]=Json::array();
    entities.at("catalog").extensions["vendor"]={{"placement",{{"host_entity_id","unit"}}}};
    const auto opaque=replaced(original,entities);
    require_roof_directional_resize_source_admission(opaque,operators());
    const auto prepared=prepare_component_roof_directional_resize(opaque,component_edit(opaque),operators(),"Opaque control");
    require(prepared.preview_snapshot().entities().at("catalog").extensions==entities.at("catalog").extensions,"opaque host lookalikes preserve exact metadata");
    std::vector<RoofPlanResizeIntent> too_many(1000,{"unit",1,1,{0,0},0,PlanAxisResizeAnchorMode::shared_frame});
    invalid([&] { preflight_component_roof_directional_resize(original,authoring,too_many); },"combined selected members include units");
    entities=original.entities();
    for (int i=1;i<1001;++i) { auto d=entities.at("dimension-0");d.id="dimension-"+std::to_string(i);entities.emplace(d.id,d); }
    invalid([&] { preflight_component_roof_directional_resize(replaced(original,entities),authoring,operators()); },"complete component association budget precedes native staging");
    // Source envelopes remain individually below 1 MiB; the batch plus
    // conservative pending archive records must refuse before per-owner replay.
    entities=original.entities();
    for (const auto* id:{"roof","unit"}) for (int i=0;i<20;++i)
        entities.at(id).properties["vendor"]["payload-"+std::to_string(i)]=std::string(14000,'x');
    invalid([&] { preflight_component_roof_directional_resize(replaced(original,entities),authoring,operators()); },"aggregate captures charge selected proofs and pending archive records");
    entities=original.entities();entities.at("unit").extensions["invalid"]=std::numeric_limits<double>::infinity();
    invalid([&] { require_roof_directional_resize_source_admission(replaced(original,entities),operators()); },"nonportable complete source refuses");
    entities=original.entities();auto* nested=&entities.at("property").extensions["deep"];
    for (int i=0;i<70;++i) { (*nested)["child"]=Json::object();nested=&(*nested)["child"]; }
    invalid([&] { require_roof_directional_resize_source_admission(replaced(original,entities),operators()); },"complete source nesting bound precedes native manufacture");
    require_roof_directional_resize_source_admission(original,operators(true));
    const auto units=prepare_component_roof_directional_resize(original,authoring,operators(true),"Raw units");
    for (const auto* id:{"roof","unit"}) require(exact(units.preview_snapshot().entities().at(id),original.entities().at(id)),"valid roof units preserve every source byte");
}
void retained_archives_and_all_roof_forms() {
    for (int form:{0,1,2}) {
        auto document=fixture(7,false,1,form);const auto original=document.snapshot();
        // An intentional historical v1 source record keeps its exact edge meaning
        // when a new shared-frame operation upgrades the owned archive to v4.
        auto entities=replay_roof_plan_resize_entities(original.entities(),{{"roof",1.4,1.6,{0,0},0,PlanAxisResizeAnchorMode::physical_edge}});
        const auto source=replaced(original,entities);
        const auto prepared=prepare_component_roof_directional_resize(source,component_edit(source),operators(),"Second actual resize");
        const auto expected=replay_roof_plan_resize_entities(source.entities(),{{"roof",2,3,{1,1},0,PlanAxisResizeAnchorMode::shared_frame},
            {"unit",1,1,{1,1},0,PlanAxisResizeAnchorMode::shared_frame}});
        for (const auto* id:{"roof","unit"}) require(exact(prepared.preview_snapshot().entities().at(id),expected.at(id)),"all forms retain exact native original-source replay including prior archives");
        require(prepared.preview_snapshot().entities().at("roof").extensions.at(std::string(roof_plan_resize_derivations_key)).at("operations").size()==2,"retained archive receives only the actual second operator");
        const auto& first=prepared.preview_snapshot().entities().at("roof").extensions.at(std::string(roof_plan_resize_derivations_key)).at("operations")[0];
        require(first.dump()==source.entities().at("roof").extensions.at(std::string(roof_plan_resize_derivations_key)).at("operations")[0].dump(),"prior archive raw receipts remain byte exact");
        const auto& archive=prepared.preview_snapshot().entities().at("roof").extensions.at(std::string(roof_plan_resize_derivations_key));
        require(archive.at("version")==4 && first.at("operation").at("version")==1,"archive4 retains historical v1 records without acquiring shared-frame meaning");
        require(archive.at("operations")[1].at("operation").at("version")==2 &&
            archive.at("operations")[1].at("operation").at("anchor_mode")=="shared_frame","new cohort records use explicit shared-frame operation2");
    }
}
void pending_archive_capture_budget() {
    auto document=fixture(7,false,1,0,true);const auto original=document.snapshot();
    auto entities=original.entities();
    Json payload=Json::array();for (int i=0;i<20;++i) payload.push_back(std::string(12000,'x'));
    entities.at("roof").properties["quantity_entries"]["/run_m"]["retained_payload"]=payload;
    entities=replay_roof_plan_resize_entities(entities,{{"roof",2,1,{1,0},0,PlanAxisResizeAnchorMode::shared_frame}});
    auto current=receipt(4);current["retained_payload"]=payload;
    entities.at("roof").properties["quantity_entries"]["/run_m"]=current;
    const auto source=replaced(original,entities);
    validate_roof_plan_resize_source_entity(source.entities().at("roof"));
    const auto independently_valid=replay_roof_plan_resize_entities(source.entities(),{{"roof",1.5,1,{1,0},0,PlanAxisResizeAnchorMode::shared_frame}});
    require(independently_valid.at("roof").extensions.dump().size()<1024*1024,"pending archive is individually valid under the historical roof proof");
    require(source.entities().at("roof").properties.dump().size()+source.entities().at("roof").extensions.dump().size()<1024*1024,"selected original roof envelope is individually bounded");
    invalid([&] { preflight_component_roof_directional_resize(source,component_edit(source),
        std::vector<RoofPlanResizeIntent>{{"roof",1.5,1,{1,0},0,PlanAxisResizeAnchorMode::shared_frame}}); },"batch budget includes pending receipt archive records before native replay");
}
void source_history_assets_and_reservations() {
    auto document=fixture();const auto source=document.snapshot();
    const auto prepared=prepare_component_roof_directional_resize(source,component_edit(source),operators(),"Source fences");
    auto scalar=component_edit(source);scalar.intents[0].properties.emplace("label",
        AssemblyInstancePropertyPatch<std::string>{AssemblyInstancePropertyPatchOperation::set,"forbidden"});
    invalid([&] { (void)prepare_component_roof_directional_resize(source,scalar,operators(),"Scalar refusal"); },"component scalar authority refuses");
    auto pose=component_edit(source);pose.intents[0].pose.operation=AssemblyInstancePropertyPatchOperation::set;pose.intents[0].pose.x_metres=50;
    invalid([&] { (void)prepare_component_roof_directional_resize(source,pose,operators(),"Pose refusal"); },"component pose authority refuses");
    auto old=component_edit(source);old.intents[0].review_profile_topology=false;
    invalid([&] { (void)prepare_component_roof_directional_resize(source,old,operators(),"Old request refusal"); },"historical component request cannot borrow lane44 authority");
    auto wire=prepared.intent();wire["source_entities_digest"]=std::string(64,'0');wire["component_request"]["source_entities_digest"]=std::string(64,'0');
    invalid([&] { (void)replay_mixed_selection_property_edit_preparation(source,wire); },"matching fabricated fences must fail actual source authentication");
    wire=prepared.intent();wire["component_identity_reservations"]=Json::array({"counterfeit"});
    invalid([&] { (void)replay_mixed_selection_property_edit_preparation(source,wire); },"reservations must independently derive");
    auto property=source.entities().at("property");property.properties["name"]="Changed source";
    document.apply(ApplyEntityChanges{.expected_revision=document.revision(),.entity_changes={EntityChange::upsert(property)},.message="Unselected source change"});
    invalid([&] { require_mixed_selection_property_edit_source_current(document.snapshot(),prepared); },"unselected source change stales capture");
    document.undo(document.revision());
    invalid([&] { require_mixed_selection_property_edit_source_current(document.snapshot(),prepared); },"identical restored entities with changed history remain stale");
    const auto saved=sketch::test::DetachedDocumentSnapshotFixture::mutate(source,[&](auto& fixture) {
        fixture.assets().emplace("new-asset",Asset::create("new-asset","application/octet-stream",{std::byte{1}}));
    });
    invalid([&] { require_mixed_selection_property_edit_source_current(saved,prepared); },"asset changes stale complete source authority");
    auto saving=fixture();const auto unsaved_source=saving.snapshot();
    const auto unsaved=prepare_component_roof_directional_resize(unsaved_source,component_edit(unsaved_source),operators(),"Save fence");
    saving.mark_saved(saving.revision());
    invalid([&] { require_mixed_selection_property_edit_source_current(saving.snapshot(),unsaved); },"saved revision alone stales complete source authority");
    auto baseline=fixture(7,true);const auto base=baseline.snapshot();const auto authoring=component_edit(base);
    const auto reservation_control=prepare_component_roof_directional_resize(base,authoring,operators(),"Reservation control");
    require(!reservation_control.reserved_component_identity_ids().empty(),"baseline component reserves actual proposed identities");
    baseline.apply(ApplyEntityChanges{.expected_revision=baseline.revision(),
        .asset_changes={AssetChange::upsert(Asset::create("proposed-component","application/octet-stream",{std::byte{1}}))},
        .message="Retain actual asset identity"});
    const auto occupied=baseline.snapshot();
    invalid([&] { (void)prepare_component_roof_directional_resize(occupied,authoring,operators(),"Current asset collision"); },"component reservations cannot collide with current assets");
    baseline.undo(baseline.revision());const auto history=baseline.snapshot();
    invalid([&] { (void)prepare_component_roof_directional_resize(history,authoring,operators(),"Retained asset collision"); },"component reservations cannot collide with undone retained assets");
    auto borrowed=prepare_phase_assembly_instance_property_edit(base,authoring);
    invalid([&] { (void)compose_component_roof_directional_resize(history,borrowed,
        std::vector<RoofPlanResizeIntent>{{"roof",1,1,{1,1},0,PlanAxisResizeAnchorMode::shared_frame}},"Borrowed preparation"); },"composer authenticates the full original source of component preparation");
}
void saved_view_roof_dimensions_refuse() {
    auto document=fixture();const auto original=document.snapshot();const auto authoring=component_edit(original);
    for (const auto* owner:{"roof","unit"}) {
        CoordinatedView view;view.id="plan";view.name="Stair plan";view.object_ids={owner};
        SectionOverlay overlay;overlay.id="roof-width";overlay.kind=SectionOverlayKind::dimension;
        overlay.dimension_binding=SectionDimensionBinding{owner,SectionDimensionAxis::horizontal,.5};
        view.overlays={overlay};DrawingSheet sheet;sheet.id="sheet";sheet.number="A1";
        auto entities=original.entities();
        entities.emplace("saved-view",make_sheet_view_entity("saved-view",SheetViewModel::create({view},{sheet})));
        const auto source=replaced(original,entities);
        for (bool units:{false,true}) {
            invalid([&] { preflight_component_roof_directional_resize(source,authoring,operators(units)); },"saved-view roof dimensions refuse changed and unit requests before native work");
            invalid([&] { require_roof_directional_resize_source_admission(source,operators(units)); },"frame admission shares typed saved-view dimension exclusion");
        }
    }
    CoordinatedView view;view.id="plan";view.name="Unbound plan";view.object_ids={"roof"};
    DrawingSheet sheet;sheet.id="sheet";sheet.number="A1";
    auto carrier=make_sheet_view_entity("saved-view",SheetViewModel::create({view},{sheet}));
    carrier.extensions["vendor"]={{"dimension_binding",{{"object_id","roof"}}}};
    auto entities=original.entities();entities.emplace(carrier.id,carrier);
    const auto source=replaced(original,entities);
    const auto prepared=prepare_component_roof_directional_resize(source,component_edit(source),operators(),"Opaque saved-view control");
    require(exact(prepared.preview_snapshot().entities().at(carrier.id),carrier),"unbound saved views and opaque binding lookalikes retain exact envelopes");
}
void schema_keys_are_not_owner_references() {
    auto document=fixture();const auto original=document.snapshot();
    for (const auto* id:{"entity_id","assembly","profile","width","version","object_id","owner_id"}) {
        auto entities=original.entities();auto node=entities.extract("roof");node.key()=id;node.mapped().id=id;
        entities.insert(std::move(node));const auto source=replaced(original,entities);auto requests=operators();requests[1].roof_id=id;
        require_roof_directional_resize_source_admission(source,requests);
        const auto prepared=prepare_component_roof_directional_resize(source,component_edit(source),requests,"Schema spelling owner");
        require(prepared.preview_snapshot().entities().contains(id) && prepared.preview_snapshot().entities().contains("dimension-0"),"schema keys and nonidentity enum values cannot establish a roof relationship");
    }
}
struct ScopedProjectFile {
    std::filesystem::path directory,path;
    ScopedProjectFile() {
        const auto stamp=std::chrono::steady_clock::now().time_since_epoch().count();
        for (unsigned attempt=0;attempt<64;++attempt) {
            directory=std::filesystem::temp_directory_path()/("vertex-mixed-roof-"+std::to_string(stamp)+"-"+std::to_string(attempt));
            if (std::filesystem::create_directory(directory)) { path=directory/"history.bldproj";return; }
        }
        throw std::runtime_error("unique roof storage directory unavailable");
    }
    ~ScopedProjectFile() { std::error_code error;std::filesystem::remove_all(directory,error); }
};
ApplyBoundaryConstraintChanges command(const DocumentSnapshot& source,const MixedSelectionPropertyEditPreparation& prepared) {
    ApplyBoundaryConstraintChanges result;result.expected_revision=source.revision();result.message=prepared.intent().at("message").get<std::string>();
    result.mixed_selection_edit_completion=true;result.mixed_selection_edit_intent=complete_mixed_selection_property_edit(source,prepared,{},{});return result;
}
void previous_reader_markers_refuse(const std::filesystem::path& file) {
    sqlite3* raw=nullptr;const auto u8=file.u8string();const std::string name(reinterpret_cast<const char*>(u8.data()),u8.size());
    const auto opened=sqlite3_open_v2(name.c_str(),&raw,SQLITE_OPEN_READWRITE,nullptr);
    std::unique_ptr<sqlite3,decltype(&sqlite3_close)> database(raw,&sqlite3_close);
    require(opened==SQLITE_OK,"controlled saved mixed project opens for marker downgrade");
    require(sqlite3_exec(database.get(),"BEGIN; PRAGMA user_version=266; UPDATE metadata SET value='266' WHERE key='format_version'; COMMIT",
        nullptr,nullptr,nullptr)==SQLITE_OK,"downgrade only controlled reader marker pair");database.reset();
    const auto digest=ProjectStore::file_sha256(file);bool refused=false;
    try { (void)ProjectStore::load(file); }
    catch (const StorageError& error) {
        require(error.code()==StorageErrorCode::unsupported_format && std::string(error.what()).find("requires reader 267")!=std::string::npos,
            "reader266 refuses actual retained shared-frame authority at reader267");refused=true;
    }
    require(refused && ProjectStore::file_sha256(file)==digest,"reader266 refusal retains every original file byte");
}
void declared_dependencies_refuse() {
    auto document=fixture();const auto original=document.snapshot();const auto authoring=component_edit(original);
    for (bool units:{false,true}) {
        require_roof_directional_resize_source_admission(original,operators(units));
        preflight_component_roof_directional_resize(original,authoring,operators(units));
        for (const auto* owner:{"roof","unit"}) for (const auto* key:{"entity_ids","wall_ids","owner_id","boundary_id","dependency_ids","dependencies"}) {
            auto entities=original.entities();
            const Json reference=std::string_view(key)=="owner_id" || std::string_view(key)=="boundary_id" ? Json(owner) : Json::array({owner});
            entities.emplace("future-dependency",Entity{"future-dependency","future_dependency",{{key,reference}},false});
            const auto source=replaced(original,entities);
            invalid([&] { require_roof_directional_resize_source_admission(source,operators(units)); },"declared dependencies outside constraint entities refuse source admission");
            invalid([&] { preflight_component_roof_directional_resize(source,authoring,operators(units)); },"declared dependencies refuse full preflight including units");
            invalid([&] { (void)prepare_component_roof_directional_resize(source,authoring,operators(units),"Declared dependency refusal"); },"declared dependencies refuse preparation including units");
        }
    }
    auto entities=original.entities();
    Entity carrier{"opaque-reference","vendor",{{"vendor",{{"entity_ids",Json::array({"roof"})}}}},false};
    carrier.extensions={{"dependencies",Json::array({"roof","unit"})}};entities.emplace(carrier.id,carrier);
    const auto source=replaced(original,entities);
    const auto prepared=prepare_component_roof_directional_resize(source,component_edit(source),operators(),"Opaque dependency control");
    require(exact(prepared.preview_snapshot().entities().at(carrier.id),carrier),"opaque dependency lookalikes preserve exact envelopes without authority");
}
void archive_only_storage_and_document_admission() {
    auto seed=fixture(7,false,0);
    require(!inspect_roof_clone_plan(seed.snapshot().entities(),{"roof"}).ready(),
        "opaque live owner reference correctly prevents unreviewed cloning");
    auto clone_source=seed.snapshot().entities().at("roof");
    clone_source.properties.at("vendor").at("host").erase("roof_id");
    seed.apply(ApplyEntityChanges{.expected_revision=seed.revision(),
        .entity_changes={EntityChange::upsert(clone_source)},.message="Source without opaque live clone dependency"});
    const auto original=seed.snapshot();
    const RoofPlanResizeIntent request{"roof",1.4,1.6,{-.5,-1},0,PlanAxisResizeAnchorMode::shared_frame};
    const auto staged=replay_roof_plan_resize_entities(original.entities(),{request});
    const auto key=std::string(roof_plan_resize_derivations_key);
    const auto archive=staged.at("roof").extensions.at(key);
    require(archive.at("version")==4 && archive.at("operations")[0].at("operation").at("version")==2,"archive-only source genuinely retains shared-frame authority");
    const auto rows=[](const Entities& entities) {
        std::vector<Entity> result;for (const auto& [id,entity]:entities) { (void)id;result.push_back(entity); }return result;
    };
    auto imported=Document::create(rows(staged));const auto source=imported.snapshot();
    require(source.entities()==staged && !source.history().front().boundary_constraint_changes,"entity-only import retains exact owned archive without Properties44 history");
    const auto plan=inspect_roof_clone_plan(source.entities(),{"roof"});
    const auto cloned=replay_roof_clone(source.entities(),plan,{{"roof","cloned-roof"}});
    require(cloned.entities.at("cloned-roof").extensions.at(key).dump()==archive.dump(),"clone retains archive4 historical owner and receipts byte exact");
    imported.apply(ApplyEntityChanges{.expected_revision=imported.revision(),
        .entity_changes={EntityChange::upsert(cloned.entities.at("cloned-roof"))},.message="Clone archive-only roof"});
    const auto applied=imported.snapshot();
    imported.undo(imported.revision());const auto undone=imported.snapshot();
    require(undone.entities()==source.entities(),"clone Undo restores exact imported archive-only source");
    imported.redo(imported.revision());const auto redone=imported.snapshot();
    require(redone.entities()==applied.entities(),"clone Redo retains exact archive-only authority");
    const auto forked=Document::fork(redone).snapshot();
    require(forked.entities()==redone.entities(),"fork imports archive4 without modifying historical provenance");
    for (const auto& snapshot:{source,applied,undone,redone,forked}) {
        require(ProjectStore::required_format_version(snapshot)==267 && ProjectStore::required_semantic_format_version(snapshot)==267,"canonical archive4 alone requires reader267 even across clone Undo and import");
        ScopedProjectFile file;(void)ProjectStore::save(file.path,snapshot);const auto restored=ProjectStore::load(file.path).document.snapshot();
        require(restored.entities()==snapshot.entities(),"archive-only save/Open preserves exact envelopes");
        const auto extracted=file.directory/"extracted";extract_project(snapshot,extracted);std::ifstream input(extracted/"project.json");
        require(Json::parse(input).at("exchange_version")==265,"canonical archive4 alone requires extraction265");
        previous_reader_markers_refuse(file.path);
    }
    for (int defect:{0,1,2}) {
        auto malformed=source.entities();auto& retained=malformed.at("roof").extensions.at(key);
        if (defect==0) retained["operations"][0]["operation"]["anchor_mode"]="physical_edge";
        if (defect==1) retained["operations"][0]["result"]["run_m"]=99;
        if (defect==2) retained["version"]=3;
        bool refused=false;try { (void)Document::create(rows(malformed)); }
        catch (const DocumentError& error) { require(error.code()==DocumentErrorCode::invalid_entity,"malformed canonical archive is an invalid entity");refused=true; }
        require(refused,"Document entity-only admission rejects malformed v4 and v2 in older archives");
        refused=false;try {
            imported.apply(ApplyEntityChanges{.expected_revision=imported.revision(),
                .entity_changes={EntityChange::upsert(malformed.at("roof"))},.message="Malformed archive refusal"});
        } catch (const DocumentError& error) { require(error.code()==DocumentErrorCode::invalid_entity,"malformed archive update is an invalid entity");refused=true; }
        require(refused && imported.snapshot().entities()==redone.entities() && imported.revision()==redone.revision(),"Document refuses malformed archive updates without publication");
    }
    for (const auto* unsupported_key:{"roof_plan_resize_derivations","roof_rigid_transform_derivations","roof_uniform_transform_derivations"}) {
        auto future=original.entities();auto payload=archive;
        payload["version"]=std::string_view(unsupported_key)=="roof_plan_resize_derivations" ? 5 : 4;
        payload["future_payload"]={{"retain",Json::array({"opaque",19,true})}};
        future.at("roof").extensions[unsupported_key]=payload;
        auto preserved=Document::create(rows(future));
        require(preserved.snapshot().entities()==future && !preserved.read_only_reason().empty(),
            "future roof-plan and other-key archive4 payloads remain exact and read-only");
        bool refused=false;try {
            preserved.apply(ApplyEntityChanges{.expected_revision=preserved.revision(),
                .entity_changes={EntityChange::upsert(original.entities().at("roof"))},.message="Unsupported proof refusal"});
        } catch (const DocumentError& error) { require(error.code()==DocumentErrorCode::read_only,"unsupported proof refuses editing as read-only");refused=true; }
        require(refused && preserved.snapshot().entities()==future,"unsupported proof refusal preserves complete payload");
    }
    auto opaque=original.entities();
    opaque.at("property").extensions[key]=archive;
    opaque.at("unit").properties["vendor"][key]=archive;
    opaque.at("unit").extensions["vendor"][key]=archive;
    const auto control=Document::create(rows(opaque)).snapshot();
    require(control.entities()==opaque && ProjectStore::required_format_version(control)==ProjectStore::required_format_version(original) &&
        ProjectStore::required_semantic_format_version(control)==ProjectStore::required_semantic_format_version(original),"nonroof and nested archive lookalikes retain the previous floor and raw bytes");
    ScopedProjectFile file;const auto extracted=file.directory/"extracted";extract_project(control,extracted);std::ifstream input(extracted/"project.json");
    require(Json::parse(input).at("exchange_version")==ProjectStore::required_semantic_format_version(original)-2,"opaque archive4 lookalikes retain the prior extraction floor");
}
void retained_history_and_storage_floors() {
    for (bool baseline:{false,true}) {
        auto document=fixture(7,baseline);const auto source=document.snapshot();
        require(ProjectStore::required_format_version(source)<267,"source starts below reader267");
        const auto prepared=prepare_component_roof_directional_resize(source,component_edit(source),operators(),"Atomic Properties44");
        const auto operation=command(source,prepared);const auto wire=command_to_json(operation);
        require(wire.at("version")==52 && wire.at("mixed_selection_edit_intent").at("version")==44,"canonical command52 retains Properties44");
        document.apply(operation);const auto applied=document.snapshot();
        require(applied.entities()==prepared.preview_snapshot().entities() && applied.revision()==source.revision()+1,"one command publishes both complete lanes");
        document.undo(document.revision());const auto undone=document.snapshot();require(undone.entities()==source.entities(),"one Undo restores both exact families");
        document.redo(document.revision());require(document.snapshot().entities()==applied.entities(),"one Redo reproduces complete result");
        for (const auto& snapshot:{applied,undone,document.snapshot()}) {
            require(ProjectStore::required_format_version(snapshot)==267 && ProjectStore::required_semantic_format_version(snapshot)==267,"retained applied/undone/redone authority requires reader267");
            ScopedProjectFile file;(void)ProjectStore::save(file.path,snapshot);auto loaded=ProjectStore::load(file.path);
            const auto restored=loaded.document.snapshot();
            require(restored.entities()==snapshot.entities() && restored.assets()==snapshot.assets() && restored.history().size()==snapshot.history().size(),"native reopen retains exact graph, assets and history");
            require(command_to_json(*restored.history().at(applied.revision()).boundary_constraint_changes)==wire,"native reopen retains exact command52 authority");
            const auto extracted=file.directory/"extracted";extract_project(snapshot,extracted);
            std::ifstream input(extracted/"project.json");const auto manifest=Json::parse(input);
            require(manifest.at("exchange_version")==265 && manifest.at("revisions").at(applied.revision()).at("boundary_constraint_changes")==wire,"extraction265 retains actual Properties44 authority");
            previous_reader_markers_refuse(file.path);
        }
    }
    auto document=fixture();const auto source=document.snapshot();
    const auto native_floor=ProjectStore::required_format_version(source),semantic_floor=ProjectStore::required_semantic_format_version(source);
    const auto prepared=prepare_component_roof_directional_resize(source,component_edit(source),operators(),"Opaque control");
    auto property=source.entities().at("property");property.extensions=command_to_json(command(source,prepared));property.properties["opaque_intent"]=prepared.intent();
    document.apply(ApplyEntityChanges{.expected_revision=document.revision(),.entity_changes={EntityChange::upsert(property)},.message="Opaque metadata"});
    const auto head=document.snapshot();document.undo(document.revision());
    for (const auto& snapshot:{head,document.snapshot()}) {
        require(ProjectStore::required_format_version(snapshot)==native_floor && ProjectStore::required_semantic_format_version(snapshot)==semantic_floor,"opaque command metadata creates no reader267 authority");
        ScopedProjectFile file;(void)ProjectStore::save(file.path,snapshot);const auto restored=ProjectStore::load(file.path).document.snapshot();
        require(restored.entities()==snapshot.entities() && ProjectStore::required_format_version(restored)==native_floor,"native storage retains opaque metadata without authority");
        const auto extracted=file.directory/"extracted";extract_project(snapshot,extracted);std::ifstream input(extracted/"project.json");
        require(Json::parse(input).at("exchange_version")==semantic_floor-2,"opaque metadata retains previous extraction floor");
    }
}

} // namespace
int main(int argc,char** argv) {
    sketch::testing::noninteractive_errors();
    try {
        const std::string_view only=argc>1 ? argv[1] : "";
        if (only=="--dependencies") declared_dependencies_refuse();
        if (only.empty() || only=="--native") { hand_geometry_and_native_extent_solver();hand_mixed_native_boxes(); }
        if (only.empty() || only=="--core") {
            mixed_wire_and_retirement();shared_frame_lane_entrances();admission_units_dependencies_and_budgets();retained_archives_and_all_roof_forms();pending_archive_capture_budget();
            source_history_assets_and_reservations();saved_view_roof_dimensions_refuse();schema_keys_are_not_owner_references();declared_dependencies_refuse();
        }
        if (only.empty() || only=="--storage") { archive_only_storage_and_document_admission();retained_history_and_storage_floors(); }
        std::cout<<"Mixed component/roof resize contracts passed\n";return 0;
    } catch (const std::exception& error) { std::cerr<<error.what()<<'\n';return 1; }
}
