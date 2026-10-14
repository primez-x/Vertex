#include "sketch/mixed_selection_property_edit.hpp"
#include "sketch/assembly_model.hpp"
#include "sketch/boundary_dimension.hpp"
#include "sketch/building_entity.hpp"
#include "sketch/document_digest.hpp"
#include "sketch/phase_constraint_authoring.hpp"
#include "sketch/model_phases.hpp"
#include "sketch/plan_axis_resize.hpp"
#include "sketch/stair_object_edit.hpp"
#include "sketch/project_store.hpp"
#include "sketch/project_exchange.hpp"
#include "sketch/sheet_view_entity_codec.hpp"
#include "support/noninteractive_errors.hpp"
#include "support/detached_document_snapshot.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <numbers>
#include <memory>
#include <stdexcept>
#include <sqlite3.h>

namespace {
using namespace sketch;
using Json=nlohmann::json;
using Entities=std::map<std::string,Entity,std::less<>>;
void require(bool value,const char* message) { if (!value) throw std::runtime_error(message); }
void require_near(double actual,double expected,const char* message) {
    require(std::isfinite(actual) && std::abs(actual-expected)<1e-8,message);
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
Entity stair(const std::string& id="stair") {
    auto value=encode_building_entity(StairFlight{.id=id,.base_position={2,3,4},.orientation_radians=0,
        .riser_count=4,.total_rise=2,.going=1,.width=2,.top_landing=StairLanding{1,.25}});
    for (const auto& [key,row]:context.items()) value.properties[key]=row;
    value.properties["base_position_m"]={2,3,4};
    value.properties["going_m"]=1;value.properties["width_m"]=2;value.properties["total_rise_m"]=2;
    value.properties["orientation_rad"]=0;
    value.properties["top_landing"]["depth_m"]=1;
    value.properties["top_landing"]["vendor"]="landing metadata";
    value.properties["mark"]="S-1";value.properties["vendor"]={{"host",{{"stair_id","stair"}}},{"numeric",7}};
    value.properties["quantity_entries"]={{"/base_position_m/0",receipt(2)},{"/base_position_m/1",receipt(3)},
        {"/base_position_m/2",receipt(4)},{"/going_m",receipt(1)},{"/total_rise_m",receipt(2)},
        {"/vendor/numeric",{{"version",81},{"opaque",true}}}};
    value.extensions={{"vendor",{{"retain",Json::array({2,"raw"})}}}};return value;
}
Document fixture(int version=7,bool baseline=false,std::size_t dimensions=1) {
    AssemblyProfile profile;profile.id="profile";
    profile.outer={{{0,0},{4,0},0},{{4,0},{4,3},0},{{4,3},{0,3},0},{{0,3},{0,0},0}};
    profile.elevation_m=.4;profile.height_m=.6;if (version==7) profile.topology=topology("");
    AssemblyType body;body.id="body";body.name="Source body";body.profiles={profile};
    AssemblyInstance instance;instance.id="component";instance.type_id="body";
    instance.root_transform=AssemblyTransform{{5,1,.75},.29,1,true};
    auto owner=encode_document_assembly_instance({"component","assembly_instance",context},{"catalog",instance});
    std::vector<Entity> rows{{"property","property",{{"name","Mixed stair source"}},false},
        {"building","building",{{"property_id","property"}},false},
        {"floor","floor",{{"building_id","building"}},false},{"layer","layer",{{"floor_id","floor"}},false},
        {"catalog","assembly_model",{{"model",AssemblyModel::create({}, {body},{}).to_json()}},false},owner,stair(),stair("unit")};
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
std::vector<StairDirectionalResizeRequest> operators(bool units=false) {
    return {{"unit",1,1,{1,1},0},{"stair",units ? 1. : 2.,units ? 1. : 3.,{1,1},0}};
}
void hand_geometry_and_raw_receipts() {
    const auto source=stair();const auto resized=stage_stair_plan_axis_resize_entity(source,2,3,{1,1},0);
    const auto layout=derive_stair_layout(decode_stair_properties("stair",resized.properties));
    const auto& flight=layout.flights.front();
    require_near(flight.base_position.x,3,"anchored base X must move");require_near(flight.base_position.y,7,"anchored base Y must move");
    require_near(flight.end_position.x,11,"flight end X must follow four goings");require_near(flight.end_position.y,7,"flight end Y");
    require_near(flight.end_position.z,6,"flight end elevation is retained");
    require(flight.treads.size()==4 && layout.landings.size()==1,"four treads and existing top landing must survive");
    for (std::size_t i=0;i<4;++i) {
        const auto& tread=flight.treads[i];
        require_near(tread.footprint[0].x,3+2*i,"hand tread start X");require_near(tread.footprint[1].x,5+2*i,"hand tread end X");
        require_near(tread.footprint[0].y,7,"hand tread near Y");require_near(tread.footprint[2].y,13,"hand tread far Y");
        require_near(tread.elevation,4+.5*(i+1),"hand tread Z");
    }
    require_near(layout.landings[0].footprint[0].x,11,"landing starts after last tread");
    require_near(layout.landings[0].footprint[1].x,13,"landing retained along scale");
    require_near(layout.landings[0].footprint[2].y,13,"landing width scale");require_near(layout.landings[0].elevation,6,"landing elevation");
    for (const auto* key:{"total_rise_m","riser_count","orientation_rad","vendor","mark"})
        require(resized.properties.at(key).dump()==source.properties.at(key).dump(),"unaffected raw fields must remain exact");
    require(resized.properties.at("base_position_m").at(2).dump()=="4","base Z integer representation must survive");
    require(resized.properties.at("top_landing").at("thickness_m").dump()==source.properties.at("top_landing").at("thickness_m").dump(),"thickness representation must survive");
    require(resized.extensions.dump()==source.extensions.dump(),"extensions must survive");
    require(!resized.properties.at("quantity_entries").contains("/going_m"),"changed known owned going receipt must retire");
    require(!resized.properties.at("quantity_entries").contains("/base_position_m/0") &&
        !resized.properties.at("quantity_entries").contains("/base_position_m/1"),"changed known owned base XY receipts must retire");
    require(resized.properties.at("quantity_entries").at("/base_position_m/2").dump()==source.properties.at("quantity_entries").at("/base_position_m/2").dump(),"unchanged base Z receipt must survive");
    require(resized.properties.at("quantity_entries").at("/total_rise_m").dump()==source.properties.at("quantity_entries").at("/total_rise_m").dump(),"unaffected receipt must survive");
    require(resized.properties.at("quantity_entries").at("/vendor/numeric").dump()==source.properties.at("quantity_entries").at("/vendor/numeric").dump(),"opaque receipt must survive");
    const auto capture=capture_stair_object_edit(source,resized);require(capture.has_value(),"full pose/profile capture is required");
    auto document=fixture();auto entities=document.snapshot().entities();entities.at("stair")=source;
    require(exact(replay_stair_object_edit_entities(entities,{*capture}).at("stair"),resized),"full original-map replay must exactly equal raw staging");
    require(exact(stage_stair_plan_axis_resize_entity(source,1,1,{1,1},0),source),"native unit must return exact raw source");
    auto dormant=source;dormant.properties["flights"]=Json::array();dormant.properties["landings"]=Json::array();
    invalid([&] { (void)stage_stair_plan_axis_resize_entity(dormant,2,3,{1,1},0); },"dormant topology outside v1 typed authority must explicitly refuse");
    auto opaque=source;opaque.properties["quantity_entries"]["/going_m"]={{"version",81},{"opaque",true}};
    invalid([&] { (void)stage_stair_plan_axis_resize_entity(opaque,2,3,{1,1},0); },"affected opaque scalar receipt must refuse");
    opaque=source;opaque.properties["quantity_entries"]["/top_landing"]={{"version",81}};
    invalid([&] { (void)stage_stair_plan_axis_resize_entity(opaque,2,3,{1,1},0); },"affected opaque landing-domain receipt must refuse");
    const auto distinct=std::nextafter(1.,2.);
    const auto tiny=stage_stair_plan_axis_resize_entity(source,1,distinct,{2,3},0);
    require(tiny.properties.at("width_m").get<double>()==2*distinct && tiny.properties.at("going_m").dump()=="1","nextafter factors must stay independent");
    invalid([&] { (void)stage_stair_plan_axis_resize_entity(source,1,distinct,{2,3},.2); },"oblique distinct factors must refuse");
    for (const auto* pointer:{"/base_position_m/0","/base_position_m/1","/width_m","/top_landing/depth_m"}) {
        opaque=source;opaque.properties["quantity_entries"][pointer]={{"version",81},{"opaque",true}};
        invalid([&] { (void)stage_stair_plan_axis_resize_entity(opaque,2,3,{1,1},0); },"every affected opaque owned binding refuses");
    }
    auto near_only=stage_stair_plan_axis_resize_entity(source,2,1,{2,3},0);
    require(near_only.properties.at("base_position_m").dump()==source.properties.at("base_position_m").dump() &&
        near_only.properties.at("width_m").dump()=="2","unchanged base and across integer representations survive");
    for (const auto* pointer:{"/base_position_m/0","/base_position_m/1","/base_position_m/2"})
        require(near_only.properties.at("quantity_entries").at(pointer).dump()==source.properties.at("quantity_entries").at(pointer).dump(),"unaffected base receipts survive an along resize");
    auto swapped=stage_stair_plan_axis_resize_entity(source,3,2,{1,1},std::numbers::pi/2);
    require_near(swapped.properties.at("going_m").get<double>(),2,"quarter-turn frame swaps along factor");
    require_near(swapped.properties.at("width_m").get<double>(),6,"quarter-turn frame swaps across factor");
}
void mixed_wire_and_retirement() {
    for (int version:{7,8,9}) for (bool baseline:{false,true}) {
        auto document=fixture(version,baseline);const auto source=document.snapshot();const auto digest=document_snapshot_digest(source);
        const auto preparation=prepare_component_stair_directional_resize(source,component_edit(source,version),operators(),"Mixed stair geometry");
        const auto& wire=preparation.intent();const auto& final=preparation.preview_snapshot().entities();
        require(wire.at("version")==43 && wire.size()==12,"Properties43 must have exactly twelve mandatory fields");
        require(wire.at("component_request").at("version")==version,"retain component request 7/8/9 authority");
        require(wire.at("stair_axis_requests").at(0).at("stair_id")=="stair","stair operators must sort independently");
        require(exact(final.at("stair"),stage_stair_plan_axis_resize_entity(source.entities().at("stair"),2,3,{1,1},0)),"composed stair must equal native raw stage exactly");
        require(exact(final.at("unit"),source.entities().at("unit")),"unit owner must stay byte exact");
        require(preparation.selected_original_to_proposed().at("stair")=="stair" && preparation.selected_original_to_proposed().at("unit")=="unit","physical lane introduces no owner IDs");
        require(document_snapshot_digest(document.snapshot())==digest,"source/history must remain unchanged");
        require(replay_mixed_selection_property_edit_preparation(source,wire).intent()==wire,"wire replays independently");
        auto bad=wire;bad["candidate_entities"]=Json::object();
        invalid([&] { (void)validate_mixed_selection_property_edit_intent(bad); },"closed wire rejects supplied candidates");
        for (const auto* key:{"version","expected_revision","source_snapshot_digest","source_authoring_digest","source_entities_digest",
            "source_saved_revision","component_request","component_identity_reservations","stair_axis_requests","room_reviews","phase_room_reviews","message"}) {
            bad=wire;bad.erase(key);invalid([&] { (void)validate_mixed_selection_property_edit_intent(bad); },"all twelve fields are mandatory");
        }
        for (const auto* key:{"expected_revision","source_snapshot_digest","source_authoring_digest","source_entities_digest","source_saved_revision"}) {
            bad=wire;bad["component_request"][key]=std::string_view(key).find("digest")!=std::string_view::npos ? Json(std::string(64,'a')) : Json(913);
            invalid([&] { (void)validate_mixed_selection_property_edit_intent(bad); },"all five component fences must match outer source");
        }
        bad=wire;bad["stair_axis_requests"].push_back(bad["stair_axis_requests"][0]);
        invalid([&] { (void)validate_mixed_selection_property_edit_intent(bad); },"duplicate/unsorted stair operators refuse");
        bad=wire;bad["stair_axis_requests"][0]["sx"]=0;
        invalid([&] { (void)validate_mixed_selection_property_edit_intent(bad); },"nonpositive factors refuse");
        bad=wire;bad["stair_axis_requests"][0]["sy"]=std::numeric_limits<double>::infinity();
        invalid([&] { (void)validate_mixed_selection_property_edit_intent(bad); },"nonfinite factors refuse before wire manufacture");
        bad=wire;bad["stair_axis_requests"][0]["anchor_m"]=Json::array({1,2,3});
        invalid([&] { (void)validate_mixed_selection_property_edit_intent(bad); },"anchor has exactly two coordinates");
        bad=wire;bad["stair_axis_requests"][0]["profile_fields"]=Json::object();
        invalid([&] { (void)validate_mixed_selection_property_edit_intent(bad); },"closed stair rows reject caller profiles");
        bad=wire;bad["stair_axis_requests"]=Json::array();
        invalid([&] { (void)validate_mixed_selection_property_edit_intent(bad); },"stair roster stays nonempty");
        bad=wire;bad["message"]=std::string(1025,'x');
        invalid([&] { (void)validate_mixed_selection_property_edit_intent(bad); },"message byte bound refuses");
        bad=wire;bad["room_reviews"].push_back(Json::object());
        invalid([&] { (void)validate_mixed_selection_property_edit_intent(bad); },"no room review authority");
        for (int historical:{39,40,41,42}) {
            bad=wire;bad["version"]=historical;
            invalid([&] { (void)validate_mixed_selection_property_edit_intent(bad); },"historical dialects cannot borrow stair operators");
        }
        if (version==7) {
            const auto retired=prepare_component_stair_directional_resize(source,component_edit(source,version,true),operators(),"Explicit retirement");
            require(mixed_selection_property_edit_retires_dependencies(retired.intent()),"explicit component retirement remains visible");
            require(!retired.preview_snapshot().entities().contains(baseline ? "proposed-dimension-0" : "dimension-0"),"reviewed component dimension retires in one composition");
            if (baseline) require(exact(retired.preview_snapshot().entities().at("dimension-0"),source.entities().at("dimension-0")),"baseline original dimension remains exact");
        }
    }
}
DocumentSnapshot replaced(const DocumentSnapshot& original,const Entities& entities) {
    return sketch::test::DetachedDocumentSnapshotFixture::mutate(original,[&](auto& fixture) { fixture.entities()=entities; });
}
void actual_dependencies_refuse_units() {
    auto document=fixture();const auto original=document.snapshot();
    for (bool unit:{false,true}) {
        const auto id=unit ? "unit" : "stair";
        auto entities=original.entities();
        auto rail=encode_building_entity(Railing{.id="rail",.height=1,.thickness=.1,.post_spacing=.5,
            .host=StairRailingHost{id,id,StairRailingSide::left,0,1}});
        for (const auto& [key,value]:context.items()) rail.properties[key]=value;
        entities.emplace("rail",rail);
        auto source=replaced(original,entities);
        invalid([&] { (void)prepare_component_stair_directional_resize(source,component_edit(source),operators(),"Attached rail"); },"typed attached rail refuses including units");
        entities=original.entities();entities.emplace("future-rail",Entity{"future-rail","railing",
            {{"version",91},{"form","future_railing"},{"host",{{"stair_id",id}}}},false});
        source=replaced(original,entities);
        invalid([&] { preflight_component_stair_directional_resize(source,component_edit(source),operators()); },"affected opaque rail binding must refuse preservation before native work");
        entities=original.entities();auto model=AssemblyModel::from_json(entities.at("catalog").properties.at("model"));
        AssemblyInstance hosted;hosted.id="hosted";hosted.type_id="body";hosted.placement=AssemblyPlacement{id,{0,0},0,1,false,0,1};
        entities.at("catalog").properties["model"]=AssemblyModel::create({},model.types(),{hosted}).to_json();
        source=replaced(original,entities);
        invalid([&] { (void)prepare_component_stair_directional_resize(source,component_edit(source),operators(),"Hosted root"); },"typed hosted component root refuses including units");
        entities=original.entities();entities.emplace("future-dependency",Entity{"future-dependency","future_dependency",
            {{"version",91},{"entity_ids",Json::array({id})}},false});
        source=replaced(original,entities);
        invalid([&] { (void)prepare_component_stair_directional_resize(source,component_edit(source),operators(),"Future affected dependency"); },"future affected owner references refuse including units");
        entities=original.entities();entities.emplace("future-dimension",Entity{"future-dimension","dimension",
            {{"dimension_version",91},{"target",{{"entity_id",id}}}},false});
        source=replaced(original,entities);
        invalid([&] { (void)prepare_component_stair_directional_resize(source,component_edit(source),operators(),"Future affected dimension"); },"future stair-owned dimensions refuse including units");
        entities=original.entities();entities.emplace("affected-constraint",Entity{"affected-constraint","constraint",
            {{"version",91},{"relation","future"},{"entity_ids",Json::array({id})}},false});
        source=replaced(original,entities);
        invalid([&] { (void)prepare_component_stair_directional_resize(source,component_edit(source),operators(),"Affected constraint"); },"affected constraints refuse including units");
        entities=original.entities();entities.at(id).properties["version"]=2;entities.at(id).properties["form"]="multi_flight_stair";
        source=replaced(original,entities);
        invalid([&] { (void)prepare_component_stair_directional_resize(source,component_edit(source),operators(),"Multi codec"); },"other stair codecs refuse including units");
    }
    // Metadata containing identical host spellings has no typed relationship.
    auto entities=original.entities();entities.at("catalog").properties["vendor"]={{"placement",{{"host_entity_id","unit"}}}};
    entities.emplace("opaque-rail",Entity{"opaque-rail","railing",{{"vendor",{{"host",{{"stair_id","unit"}}}}}},false});
    const auto source=replaced(original,entities);
    const auto control=prepare_component_stair_directional_resize(source,component_edit(source),operators(),"Opaque metadata control");
    require(exact(control.preview_snapshot().entities().at("opaque-rail"),entities.at("opaque-rail")),"opaque host lookalikes grant no relationship and stay exact");
    entities=original.entities();entities.at("dimension-0").properties["dimension_version"]=91;
    const auto future_dimension=replaced(original,entities);const auto authoring=component_edit(original);
    invalid([&] { preflight_component_stair_directional_resize(future_dimension,authoring,operators()); },"future affected component dimension refuses before native manufacture");
}
void source_history_assets_and_reservations() {
    auto document=fixture();const auto source=document.snapshot();
    const auto prepared=prepare_component_stair_directional_resize(source,component_edit(source),operators(),"Source fences");
    auto scalar=component_edit(source);scalar.intents[0].properties.emplace("label",
        AssemblyInstancePropertyPatch<std::string>{AssemblyInstancePropertyPatchOperation::set,"forbidden"});
    invalid([&] { (void)prepare_component_stair_directional_resize(source,scalar,operators(),"Scalar refusal"); },"component scalar authority refuses");
    auto pose=component_edit(source);pose.intents[0].pose.operation=AssemblyInstancePropertyPatchOperation::set;pose.intents[0].pose.x_metres=50;
    invalid([&] { (void)prepare_component_stair_directional_resize(source,pose,operators(),"Pose refusal"); },"component pose authority refuses");
    auto old=component_edit(source);old.intents[0].review_profile_topology=false;
    invalid([&] { (void)prepare_component_stair_directional_resize(source,old,operators(),"Old request refusal"); },"historical component request cannot borrow lane43 authority");
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
    const auto unsaved=prepare_component_stair_directional_resize(unsaved_source,component_edit(unsaved_source),operators(),"Save fence");
    saving.mark_saved(saving.revision());
    invalid([&] { require_mixed_selection_property_edit_source_current(saving.snapshot(),unsaved); },"saved revision alone stales complete source authority");
    auto baseline=fixture(7,true);const auto base=baseline.snapshot();const auto authoring=component_edit(base);
    const auto reservation_control=prepare_component_stair_directional_resize(base,authoring,operators(),"Reservation control");
    require(!reservation_control.reserved_component_identity_ids().empty(),"baseline component reserves actual proposed identities");
    baseline.apply(ApplyEntityChanges{.expected_revision=baseline.revision(),
        .asset_changes={AssetChange::upsert(Asset::create("proposed-component","application/octet-stream",{std::byte{1}}))},
        .message="Retain actual asset identity"});
    const auto occupied=baseline.snapshot();
    invalid([&] { (void)prepare_component_stair_directional_resize(occupied,authoring,operators(),"Current asset collision"); },"component reservations cannot collide with current assets");
    baseline.undo(baseline.revision());const auto history=baseline.snapshot();
    invalid([&] { (void)prepare_component_stair_directional_resize(history,authoring,operators(),"Retained asset collision"); },"component reservations cannot collide with undone retained assets");
    auto borrowed=prepare_phase_assembly_instance_property_edit(base,authoring);
    invalid([&] { (void)compose_component_stair_directional_resize(history,borrowed,
        std::vector<StairDirectionalResizeRequest>{{"stair",1,1,{1,1},0}},"Borrowed preparation"); },"composer authenticates the full original source of component preparation");
}
void admission_includes_units_and_aggregate_bounds() {
    auto document=fixture();const auto original=document.snapshot().entities();
    for (const auto* key:{"transform","vertical_placement","level_connection","phase_id"}) {
        auto entities=original;entities.at("unit").properties[key]=Json::object();
        // Document admission is deliberately avoided: replay must refuse the raw original-source owner itself.
        auto source=sketch::test::DetachedDocumentSnapshotFixture::mutate(document.snapshot(),[&](auto& fixture) { fixture.entities()=entities; });
        invalid([&] { (void)prepare_component_stair_directional_resize(source,component_edit(source),operators(),"Unit admission"); },"unsupported unit field must refuse before no-op elimination");
    }
    auto entities=original;entities.at("unit").properties["top_landing"]=nullptr;
    auto source=sketch::test::DetachedDocumentSnapshotFixture::mutate(document.snapshot(),[&](auto& fixture) { fixture.entities()=entities; });
    invalid([&] { (void)prepare_component_stair_directional_resize(source,component_edit(source),operators(),"Missing landing"); },"unit missing landing refuses");
    entities=original;const auto phases=ModelPhases::create({"unit"},{"unit"},{});
    entities.emplace("baseline-only",Entity{"baseline-only","model_phases",{{"model",phases.to_json()}},false});
    source=sketch::test::DetachedDocumentSnapshotFixture::mutate(document.snapshot(),[&](auto& fixture) { fixture.entities()=entities; });
    invalid([&] { (void)prepare_component_stair_directional_resize(source,component_edit(source),operators(),"Baseline unit"); },"baseline-only registry membership refuses");
    entities=original;entities.at("unit").properties["riser_count"]=100000;
    source=sketch::test::DetachedDocumentSnapshotFixture::mutate(document.snapshot(),[&](auto& fixture) { fixture.entities()=entities; });
    invalid([&] { (void)prepare_component_stair_directional_resize(source,component_edit(source),operators(),"Work preflight"); },"aggregate riser plus landing work refuses before native manufacture");
    std::vector<StairDirectionalResizeRequest> too_many(1000,{"unit",1,1,{1,1},0});
    source=document.snapshot();invalid([&] { (void)prepare_component_stair_directional_resize(source,component_edit(source),too_many,"Roster"); },"combined membership exceeds 1000");
    entities=original;
    for (int i=0;i<20;++i) {
        auto& raw=entities.at("unit").properties["quantity_entries"]["/opaque-"+std::to_string(i)];raw=Json::array();
        for (int j=0;j<1000;++j) raw.push_back(std::string(64,'x'));
    }
    source=sketch::test::DetachedDocumentSnapshotFixture::mutate(document.snapshot(),[&](auto& fixture) { fixture.entities()=entities; });
    invalid([&] { (void)prepare_component_stair_directional_resize(source,component_edit(source),operators(),"Capture preflight"); },"unit aggregate capture bytes refuse before native manufacture");
    entities=original;
    for (int i=1;i<1001;++i) {
        auto dimension=entities.at("dimension-0");dimension.id="dimension-"+std::to_string(i);
        entities.emplace(dimension.id,std::move(dimension));
    }
    source=replaced(document.snapshot(),entities);const auto original_authoring=component_edit(document.snapshot());
    invalid([&] { preflight_component_stair_directional_resize(source,original_authoring,operators()); },"component associations preflight at 1000 before native replay");
    entities=original;
    for (const auto* id:{"stair","unit"}) for (int i=0;i<16;++i) {
        auto& raw=entities.at(id).properties["quantity_entries"]["/opaque-"+std::to_string(i)];
        raw=Json::array();for (int j=0;j<2500;++j) raw.push_back(true);
    }
    source=replaced(document.snapshot(),entities);
    invalid([&] { preflight_component_stair_directional_resize(source,original_authoring,operators()); },"aggregate capture node work includes unit owners");
    // Eleven individually bounded v1 stairs exceed the aggregate work cap.
    // Preflight must reject before decoding/manufacturing any 10000-tread owner.
    entities=original;std::vector<StairDirectionalResizeRequest> heavy;
    for (int i=0;i<11;++i) {
        const auto id="heavy-"+std::to_string(i);auto owner=stair(id);owner.properties["riser_count"]=10000;
        entities.emplace(id,std::move(owner));heavy.push_back({id,1,1,{1,1},0});
    }
    source=replaced(document.snapshot(),entities);
    invalid([&] { preflight_component_stair_directional_resize(source,component_edit(source),heavy); },"aggregate work rejects individually bounded owners");
    source=document.snapshot();preflight_component_stair_directional_resize(source,component_edit(source),operators(true));
    const auto units=prepare_component_stair_directional_resize(source,component_edit(source),operators(true),"Same raw units control");
    require(exact(units.preview_snapshot().entities().at("stair"),source.entities().at("stair")),"same unit/raw/natively-valid control retains source stair");
}
void saved_view_stair_dimensions_refuse() {
    auto document=fixture();const auto original=document.snapshot();const auto authoring=component_edit(original);
    for (const auto* owner:{"stair","unit"}) {
        CoordinatedView view;view.id="plan";view.name="Stair plan";view.object_ids={owner};
        SectionOverlay overlay;overlay.id="stair-width";overlay.kind=SectionOverlayKind::dimension;
        overlay.dimension_binding=SectionDimensionBinding{owner,SectionDimensionAxis::horizontal,.5};
        view.overlays={overlay};DrawingSheet sheet;sheet.id="sheet";sheet.number="A1";
        auto entities=original.entities();
        entities.emplace("saved-view",make_sheet_view_entity("saved-view",SheetViewModel::create({view},{sheet})));
        const auto source=replaced(original,entities);
        for (bool units:{false,true}) {
            invalid([&] { preflight_component_stair_directional_resize(source,authoring,operators(units)); },"saved-view stair dimensions refuse changed and unit requests before native work");
            invalid([&] { require_stair_directional_resize_source_admission(source,operators(units)); },"frame admission shares typed saved-view dimension exclusion");
        }
    }
    CoordinatedView view;view.id="plan";view.name="Unbound plan";view.object_ids={"stair"};
    DrawingSheet sheet;sheet.id="sheet";sheet.number="A1";
    auto carrier=make_sheet_view_entity("saved-view",SheetViewModel::create({view},{sheet}));
    carrier.extensions["vendor"]={{"dimension_binding",{{"object_id","stair"}}}};
    auto entities=original.entities();entities.emplace(carrier.id,carrier);
    const auto source=replaced(original,entities);
    const auto prepared=prepare_component_stair_directional_resize(source,component_edit(source),operators(),"Opaque saved-view control");
    require(exact(prepared.preview_snapshot().entities().at(carrier.id),carrier),"unbound saved views and opaque binding lookalikes retain exact envelopes");
}
void schema_keys_are_not_owner_references() {
    auto document=fixture();const auto original=document.snapshot();auto entities=original.entities();
    auto node=entities.extract("stair");node.key()="entity_id";node.mapped().id="entity_id";entities.insert(std::move(node));
    const auto source=replaced(original,entities);auto requests=operators();requests[1].stair_id="entity_id";
    require_stair_directional_resize_source_admission(source,requests);
    const auto prepared=prepare_component_stair_directional_resize(source,component_edit(source),requests,"Schema key owner");
    require(prepared.preview_snapshot().entities().contains("entity_id") && prepared.preview_snapshot().entities().contains("dimension-0"),"valid schema-spelling stair ID retains its unrelated component dimension");
}
struct ScopedProjectFile {
    std::filesystem::path directory,path;
    ScopedProjectFile() {
        const auto stamp=std::chrono::steady_clock::now().time_since_epoch().count();
        for (unsigned attempt=0;attempt<64;++attempt) {
            directory=std::filesystem::temp_directory_path()/("vertex-mixed-stair-"+std::to_string(stamp)+"-"+std::to_string(attempt));
            if (std::filesystem::create_directory(directory)) { path=directory/"history.bldproj";return; }
        }
        throw std::runtime_error("unique stair storage directory unavailable");
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
    require(sqlite3_exec(database.get(),"BEGIN; PRAGMA user_version=265; UPDATE metadata SET value='265' WHERE key='format_version'; COMMIT",
        nullptr,nullptr,nullptr)==SQLITE_OK,"downgrade only controlled reader marker pair");database.reset();
    const auto digest=ProjectStore::file_sha256(file);bool refused=false;
    try { (void)ProjectStore::load(file); }
    catch (const StorageError& error) {
        require(error.code()==StorageErrorCode::unsupported_format && std::string(error.what()).find("requires reader 266")!=std::string::npos,
            "reader265 refuses actual retained Properties43 at reader266");refused=true;
    }
    require(refused && ProjectStore::file_sha256(file)==digest,"reader265 refusal retains every original file byte");
}
void retained_history_and_storage_floors() {
    for (bool baseline:{false,true}) {
        auto document=fixture(7,baseline);const auto source=document.snapshot();
        require(ProjectStore::required_format_version(source)<266,"source starts below reader266");
        const auto prepared=prepare_component_stair_directional_resize(source,component_edit(source),operators(),"Atomic Properties43");
        const auto operation=command(source,prepared);const auto wire=command_to_json(operation);
        require(wire.at("version")==52 && wire.at("mixed_selection_edit_intent").at("version")==43,"canonical command52 retains Properties43");
        document.apply(operation);const auto applied=document.snapshot();
        require(applied.entities()==prepared.preview_snapshot().entities() && applied.revision()==source.revision()+1,"one command publishes both complete lanes");
        document.undo(document.revision());const auto undone=document.snapshot();require(undone.entities()==source.entities(),"one Undo restores both exact families");
        document.redo(document.revision());require(document.snapshot().entities()==applied.entities(),"one Redo reproduces complete result");
        for (const auto& snapshot:{applied,undone,document.snapshot()}) {
            require(ProjectStore::required_format_version(snapshot)==266 && ProjectStore::required_semantic_format_version(snapshot)==266,"retained applied/undone/redone authority requires reader266");
            ScopedProjectFile file;(void)ProjectStore::save(file.path,snapshot);auto loaded=ProjectStore::load(file.path);
            const auto restored=loaded.document.snapshot();
            require(restored.entities()==snapshot.entities() && restored.assets()==snapshot.assets() && restored.history().size()==snapshot.history().size(),"native reopen retains exact graph, assets and history");
            require(command_to_json(*restored.history().at(applied.revision()).boundary_constraint_changes)==wire,"native reopen retains exact command52 authority");
            const auto extracted=file.directory/"extracted";extract_project(snapshot,extracted);
            std::ifstream input(extracted/"project.json");const auto manifest=Json::parse(input);
            require(manifest.at("exchange_version")==264 && manifest.at("revisions").at(applied.revision()).at("boundary_constraint_changes")==wire,"extraction264 retains actual Properties43 authority");
            previous_reader_markers_refuse(file.path);
        }
    }
    auto document=fixture();const auto source=document.snapshot();
    const auto native_floor=ProjectStore::required_format_version(source),semantic_floor=ProjectStore::required_semantic_format_version(source);
    const auto prepared=prepare_component_stair_directional_resize(source,component_edit(source),operators(),"Opaque control");
    auto property=source.entities().at("property");property.extensions=command_to_json(command(source,prepared));property.properties["opaque_intent"]=prepared.intent();
    document.apply(ApplyEntityChanges{.expected_revision=document.revision(),.entity_changes={EntityChange::upsert(property)},.message="Opaque metadata"});
    const auto head=document.snapshot();document.undo(document.revision());
    for (const auto& snapshot:{head,document.snapshot()}) {
        require(ProjectStore::required_format_version(snapshot)==native_floor && ProjectStore::required_semantic_format_version(snapshot)==semantic_floor,"opaque command metadata creates no reader266 authority");
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
        if (argc==2 && std::string_view(argv[1])=="--saved-view-only") saved_view_stair_dimensions_refuse();
        else if (argc==2 && std::string_view(argv[1])=="--schema-key-only") schema_keys_are_not_owner_references();
        else { hand_geometry_and_raw_receipts();mixed_wire_and_retirement();actual_dependencies_refuse_units();
            source_history_assets_and_reservations();admission_includes_units_and_aggregate_bounds();retained_history_and_storage_floors();
            saved_view_stair_dimensions_refuse();schema_keys_are_not_owner_references(); }
    }
    catch (const std::exception& error) { std::cerr<<error.what()<<'\n';return 1; }
    std::cout<<"Mixed component stair resize tests passed\n";return 0;
}
