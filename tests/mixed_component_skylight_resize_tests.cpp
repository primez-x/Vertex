#include "sketch/mixed_selection_property_edit.hpp"
#include "sketch/assembly_dimension.hpp"
#include "sketch/assembly_model.hpp"
#include "sketch/boundary_dimension.hpp"
#include "sketch/document_digest.hpp"
#include "sketch/model_phases.hpp"
#include "sketch/phase_constraint_authoring.hpp"
#include "sketch/project_exchange.hpp"
#include "sketch/project_store.hpp"
#include "sketch/building_entity.hpp"
#include "sketch/roof_entity_codec.hpp"
#include "support/noninteractive_errors.hpp"

#include <sqlite3.h>
#include <cmath>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <numbers>
#include <set>
#include <stdexcept>
#include <string>

namespace {
using namespace sketch;
using Json=nlohmann::json;
using Entities=std::map<std::string,Entity,std::less<>>;
struct ScopedProjectFile {
    std::filesystem::path directory,path;
    ScopedProjectFile() {
        const auto stamp=std::chrono::steady_clock::now().time_since_epoch().count();
        for (unsigned attempt=0;attempt<64;++attempt) {
            directory=std::filesystem::temp_directory_path()/
                ("vertex-mixed-component-skylight-"+std::to_string(stamp)+"-"+std::to_string(attempt));
            if (std::filesystem::create_directory(directory)) {
                path=directory/"history.bldproj";return;
            }
        }
        throw std::runtime_error("unique mixed resize storage directory unavailable");
    }
    ~ScopedProjectFile() {
        std::error_code error;
        std::filesystem::remove_all(directory,error);
    }
};
void require(bool condition,const char* reason) { if (!condition) throw std::runtime_error(reason); }
void require_near(double actual,double expected,const char* reason) {
    if (!std::isfinite(actual) || std::abs(actual-expected)>=1e-8)
        throw std::runtime_error(std::string(reason)+": expected "+std::to_string(expected)+
            ", observed "+std::to_string(actual));
}
template<class F> void invalid(F&& action,const char* reason) {
    try { action(); } catch (const std::invalid_argument&) { return; }
    throw std::runtime_error(reason);
}
bool exact(const Entity& a,const Entity& b) {
    return a==b && a.properties.dump()==b.properties.dump() && a.extensions.dump()==b.extensions.dump();
}
Boundary ring(bool curved=false) {
    return {{{0,0},{4,0},0},{{4,0},{4,3},0},
        {{4,3},{0,3},curved ? std::numbers::pi : 0},{{0,3},{0,0},0}};
}
AssemblyProfileTopology topology(const std::string& prefix) {
    return {{prefix+"outer",{prefix+"e0",prefix+"e1",prefix+"e2",prefix+"e3"},
        {prefix+"v0",prefix+"v1",prefix+"v2",prefix+"v3"}},{}};
}
AssemblyProfile profile(bool curved,bool identified) {
    AssemblyProfile result;
    result.id="profile";result.outer=ring(curved);result.elevation_m=.4;result.height_m=.6;
    if (identified) result.topology=topology("");
    return result;
}
AssemblyProfileDimensionTarget selector(const std::string& prefix) {
    return {"profile",prefix+"outer",AssemblyProfileDimensionMeasurement::edge_length,
        prefix+"e0",std::nullopt,std::nullopt};
}
const Json context={{"property_id","property"},{"building_id","building"},{"floor_id","floor"},{"layer_id","layer"}};
Entity dimension(const std::string& id,const std::string& catalog,const std::string& instance,
    const std::optional<std::string>& owner,bool edge=false) {
    BoundaryDimension value;
    value.id=id;value.boundary_id=owner.value_or(catalog);
    value.kind=edge ? BoundaryDimensionKind::assembly_profile : BoundaryDimensionKind::assembly_extent;
    value.text_position={7.5,-2.25};
    value.presentation=BoundaryDimensionPresentation{3.2,"#713ba2",true,false,true,.31};
    AssemblyDimensionTarget target;
    target.component={catalog,instance,owner,{"child"}};target.axis=AssemblyDimensionAxis::width;
    if (edge) target.profile=selector("");
    value.assembly_target=target;
    auto result=encode_boundary_dimension_entity(value);
    result.extensions={{"vendor",{{"retain",Json::array({7,"opaque"})}}}};
    return result;
}
Entity catalog(const std::string& id,bool curved,bool identified,const std::vector<AssemblyInstance>& rows) {
    AssemblyType child;child.id="body";child.name="Actual profile body";child.profiles={profile(curved,identified)};
    AssemblyType parent;parent.id="parent";parent.name="Nested source";
    AssemblyPart part;part.id="child";part.type_id="body";
    part.transform=AssemblyTransform{{.3,-.2,.75},.17,1,false};parent.parts={part};
    Entity result{id,"assembly_model",{{"model",AssemblyModel::create({}, {child,parent},rows).to_json()},
        {"note","Exact source definitions"}},false};
    result.extensions={{"vendor",{{"retain",Json::array({9,"opaque"})}}}};
    return result;
}
AssemblyInstance hosted(const std::string& id,const std::string& host) {
    AssemblyInstance result;result.id=id;result.type_id="parent";
    result.placement=AssemblyPlacement{host,{.6,-.35},.23,1,true,.2,1};
    return result;
}

Entity roof() {
    SlopedRoofPanel panel{"roof",{2,3,4},.4,40,40,10,std::atan(.25),.2,.15,
        {{"child-a",4,5,3,2,RoofSkylight{.06,.15,.024},.31},
         {"child-b",12,9,2.5,1.75,RoofSkylight{.07,.2,.03},-.27},
         {"sibling",26,24,2,1.5,RoofSkylight{.08,.25,.032},.15}}};
    auto result=encode_building_entity(panel,{{"vendor",{{"retain","roof-envelope"}}}});
    result.properties["version"]=4;
    for (const auto& [key,value]:context.items()) result.properties[key]=value;
    result.properties["opaque_roof_metadata"]={{"retain",Json::array({"body",17,true})}};
    for (auto& row:result.properties.at("roof_openings"))
        row["opaque_child_metadata"]={{"retain",row.at("id")}};
    return result;
}
Document from_entities(const Entities& entities) {
    std::vector<Entity> values;
    for (const auto& [id,entity]:entities) { (void)id;values.push_back(entity); }
    return Document::create(values);
}
Document fixture(int component_version=7,bool baseline=false,std::size_t component_dimensions=1,
    std::size_t hosted_dimensions=1,bool profile_dimensions=true,bool shared_catalog=false) {
    const auto hosted_catalog=shared_catalog ? "component-catalog" : "roof-catalog";
    std::vector<Entity> values{
        {"property","property",{{"site_frame",{{"version",1},{"origin_m",{12.,-9.,3.}},
            {"rotation_radians",.21},{"vertical_datum",{{"identifier","local"},{"height_at_origin_m",0.}}}}}},false},
        {"building","building",{{"property_id","property"},{"site_placement",{{"version",1},
            {"translation_m",{2.,-1.,.5}},{"rotation_radians",-.14}}}},false},
        {"floor","floor",{{"building_id","building"}},false},
        {"layer","layer",{{"floor_id","floor"}},false},roof(),
        catalog("component-catalog",false,component_version==7,
            shared_catalog ? std::vector<AssemblyInstance>{hosted("roof-root","roof")} : std::vector<AssemblyInstance>{})};
    if (!shared_catalog) values.push_back(catalog("roof-catalog",false,true,{hosted("roof-root","roof")}));
    AssemblyInstance independent;independent.id="component";independent.type_id="parent";
    independent.root_transform=AssemblyTransform{{5,1,.75},.29,1,true};
    auto owner=encode_document_assembly_instance({"component","assembly_instance",context},
        {"component-catalog",independent});
    // Independent components opt into their own Site presentation; hosted
    // type-owned geometry remains world geometry and never receives this frame.
    owner.properties["presentation_frame"]={{"version",1},{"mode","building"}};
    owner.extensions={{"vendor",{{"retain","independent-envelope"}}}};
    values.push_back(owner);
    for (std::size_t i=0;i<component_dimensions;++i)
        values.push_back(dimension("component-dim-"+std::to_string(i),"component-catalog","component",std::string{"component"}));
    for (std::size_t i=0;i<hosted_dimensions;++i)
        values.push_back(dimension("roof-dim-"+std::to_string(i),hosted_catalog,"roof-root",std::nullopt));
    if (profile_dimensions && component_version==7)
        values.push_back(dimension("component-profile-dim","component-catalog","component",std::string{"component"},true));
    if (profile_dimensions && (!shared_catalog || component_version==7))
        values.push_back(dimension("roof-profile-dim",hosted_catalog,"roof-root",std::nullopt,true));
    if (baseline) {
        std::vector<std::string> ids{"component"};
        for (std::size_t i=0;i<component_dimensions;++i) ids.push_back("component-dim-"+std::to_string(i));
        if (profile_dimensions && component_version==7) ids.push_back("component-profile-dim");
        const auto phases=ModelPhases::create(ids,ids,
            {RemodelingAlternative{"alternative","Actual saved choice",{}, {}}},"alternative");
        values.push_back({"phases","model_phases",{{"model",phases.to_json()}},false});
    }
    return Document::create(values);
}
std::vector<RoofOpeningDirectionalResizeRequest> axis_requests(bool unit=false) {
    // Intentionally unsorted typed input: the sealed producer canonicalizes
    // the qualified roster without losing either child's distinct operator.
    return {{{"roof","child-b"},unit ? 1.0 : .92,unit ? 1.0 : 1.08,{2,3},-.23},
        {{"roof","child-a"},unit ? 1.0 : 1.15,unit ? 1.0 : .85,{2,3},.37}};
}
PhaseAssemblyInstancePropertyEditAuthoring component_edit(const DocumentSnapshot& source,int version=7,bool retire=false) {
    const auto model=AssemblyModel::from_json(source.entities().at("component-catalog").properties.at("model"));
    const auto type=std::find_if(model.types().begin(),model.types().end(),[](const auto& value) { return value.id=="body"; });
    require(type!=model.types().end(),"fixture must retain its actual own source type");
    auto proposed=type->profiles.front();
    proposed.planar_affine=AssemblyProfilePlanarAffine{{0,0},{0,0},1.8,0,0,.65};
    proposed.topology=topology("new-");
    AssemblyInstancePropertyEditIntent intent;
    intent.target={"component-catalog","component",std::string{"component"},{"child"}};
    intent.profile_geometry.operation=AssemblyInstancePropertyPatchOperation::set;
    intent.profile_geometry.value=std::vector<AssemblyProfile>{proposed};
    intent.review_profile_topology=version==7;
    intent.initialize_deformed_profile_topology=version==8;
    intent.review_initial_profile_geometry=version==9;
    PhaseAssemblyInstancePropertyEditAuthoring authoring;
    authoring.intents={intent};authoring.carry_presentation_references=true;
    for (const auto& [id,target]:inspect_phase_assembly_instance_profile_dimension_review_roster(source.entities(),authoring.intents)) {
        (void)target;
        authoring.profile_dimension_decisions.emplace(id,retire ? std::optional<AssemblyProfileDimensionTarget>{} : selector("new-"));
    }
    const auto plan=inspect_phase_assembly_instance_property_edit_plan(source.entities(),authoring.intents);
    for (const auto& replacement:plan.replacement_roots)
        authoring.allocation.roots.emplace(replacement.root,
            PhaseAssemblyInstanceReplacementIdentity{std::string{"proposed-component"},std::nullopt,"proposed-component"});
    for (const auto& registry:plan.required_membership_registry_ids)
        authoring.allocation.membership_entity_ids.emplace(registry,"proposed-membership");
    for (const auto& [id,target]:inspect_phase_assembly_instance_property_dimension_clone_roster(
            source.entities(),authoring.intents,authoring.profile_dimension_decisions)) {
        (void)target;authoring.dimension_identities.emplace(id,"proposed-"+id);
    }
    for (const auto& id:inspect_phase_assembly_instance_property_presentation_overlay_ids(
            source.entities(),authoring.intents,false,authoring.profile_dimension_decisions))
        authoring.presentation_overlay_identities.emplace(id,"proposed-overlay-"+id);
    return authoring;
}
double span(const Entities& entities,const std::string& id) {
    const auto dimension=decode_boundary_dimension_entity(entities.at(id));
    const std::vector<AssemblyDimensionTarget> target{*dimension.dimension->assembly_target};
    return resolve_assembly_dimensions(entities,target).front().span_metres;
}

const Json& opening_row(const Entity& owner,const std::string& id) {
    const auto& rows=owner.properties.at("roof_openings");
    const auto row=std::find_if(rows.begin(),rows.end(),[&](const auto& value) { return value.at("id")==id; });
    if (row==rows.end()) throw std::runtime_error("missing qualified fixture child");
    return *row;
}
Vec2 rotate(Vec2 point,double angle) {
    const auto c=std::cos(angle),s=std::sin(angle);
    return {c*point.x-s*point.y,s*point.x+c*point.y};
}
Vec2 apply_axis(Vec2 point,const RoofOpeningDirectionalResizeRequest& request) {
    auto aligned=rotate(point,-request.frame_rotation_radians);
    return rotate({aligned.x*request.sx,aligned.y*request.sy},request.frame_rotation_radians);
}
void check_child_math(const Entity& before,const Entity& after,const RoofOpeningDirectionalResizeRequest& request) {
    const auto old_roof=std::get<SlopedRoofPanel>(decode_roof_entity(before));
    const auto new_roof=std::get<SlopedRoofPanel>(decode_roof_entity(after));
    const auto find=[&](const SlopedRoofPanel& owner) -> const RoofOpening& {
        const auto row=std::find_if(owner.openings.begin(),owner.openings.end(),[&](const auto& value) {
            return value.id==request.target.opening_id;
        });
        require(row!=owner.openings.end(),"expected actual selected roof child");return *row;
    };
    const auto& old=find(old_roof);const auto& next=find(new_roof);
    const Vec2 old_local{old.x+old.width*.5,old.y+old.depth*.5};
    const auto old_offset=rotate(old_local,old_roof.orientation_radians);
    const Vec2 old_world{old_roof.base_position.x+old_offset.x,old_roof.base_position.y+old_offset.y};
    const auto moved=apply_axis({old_world.x-request.anchor_m.x,old_world.y-request.anchor_m.y},request);
    const Vec2 new_world{request.anchor_m.x+moved.x,request.anchor_m.y+moved.y};
    const auto local_delta=rotate({new_world.x-old_world.x,new_world.y-old_world.y},-old_roof.orientation_radians);
    const Vec2 new_local{old_local.x+local_delta.x,old_local.y+local_delta.y};
    // Independent facet math: physical width/depth axes project through the
    // pitched reference X metric, then through the roof yaw and world operator.
    const auto facet_x=std::sqrt(1.0+std::pow(old_roof.rise/old_roof.run,2));
    const Vec2 along=rotate({std::cos(old.rotation_radians)/facet_x,std::sin(old.rotation_radians)},old_roof.orientation_radians);
    const Vec2 across=rotate({-std::sin(old.rotation_radians)/facet_x,std::cos(old.rotation_radians)},old_roof.orientation_radians);
    const auto length_factor=[&](Vec2 axis) {
        const auto changed=apply_axis(axis,request);
        return std::hypot(changed.x,changed.y)/std::hypot(axis.x,axis.y);
    };
    const auto width=old.width*length_factor(along),depth=old.depth*length_factor(across);
    require_near(next.x,new_local.x-width*.5,"selected child centre must follow original world operator");
    require_near(next.y,new_local.y-depth*.5,"selected child centre must retain actual roof yaw");
    require_near(next.width,width,"pitched rotated child width must follow its projected physical axis");
    require_near(next.depth,depth,"pitched rotated child depth must follow its independent projected axis");
    require(next.rotation_radians==old.rotation_radians &&
        opening_row(before,old.id).at("skylight").dump()==opening_row(after,old.id).at("skylight").dump(),
        "directional mouth resize must preserve physical facet angle and construction profile");
    require(opening_row(before,old.id).at("opaque_child_metadata").dump()==
        opening_row(after,old.id).at("opaque_child_metadata").dump(),"selected child opaque metadata must remain exact");
}
void same_host_cohort_and_complete_proof() {
    auto document=fixture();const auto source=document.snapshot();
    const auto digest=document_snapshot_digest(source);
    const auto requests=axis_requests();
    const auto prepared=prepare_component_roof_opening_directional_resize(source,component_edit(source),requests,"Two mouths on one original pitched host");
    const auto& final=prepared.preview_snapshot().entities();
    require(prepared.intent().at("version")==40 && prepared.intent().size()==12,
        "mixed40 must have its closed twelve-field envelope");
    const auto& rows=prepared.intent().at("roof_opening_axis_requests");
    require(rows.size()==2 && rows[0].at("opening_id")=="child-a" && rows[1].at("opening_id")=="child-b",
        "typed requests must preserve both operators in sorted qualified rows");
    for (const auto& row:rows) require(row.size()==6 && row.at("anchor_m").size()==2,
        "axis row must have exactly the six reserved fields");
    require(prepared.phase_authorings().size()==1,"ordinary roof cohort must expose one generated complete proof");
    const auto& wire=prepared.phase_authorings().front();
    const auto proof=decode_phase_constraint_authoring_intent(wire);
    require(wire.at("version")==32 && proof.complete_ordinary_roof_component_dimensions &&
        proof.ordinary_roof_edits.size()==1,"one host must have one complete ordinary phase32 leaf");
    const auto& leaf_wire=proof.ordinary_roof_edits.front();
    const auto leaf=decode_roof_edit_intent(leaf_wire);
    require(leaf_wire.at("version")==9 && leaf_wire.at("openings").at("version")==4 &&
        leaf.openings && leaf.openings->upserts.size()==2 && !leaf.coordinate_world_hosted_geometry,
        "actual schema4 must reconstruct child4/composite9 even when angles are unchanged");
    require(!leaf.profile && !leaf.pose && !leaf.form && !leaf.transform && !leaf.resize && !leaf.uniform_transform &&
        leaf.openings->removed_opening_ids.empty(),"roof leaf must have only selected existing child authority");
    for (const auto& row:leaf.openings->upserts)
        require(!row.clone_source && (row.opening_id=="child-a" || row.opening_id=="child-b"),
            "generated openings leaf must have no clones or unselected upserts");
    require(prepared.roof_child_original_to_proposed().size()==2 &&
        prepared.roof_child_original_to_proposed().at({"roof","child-a"}).opening_id=="child-a" &&
        prepared.roof_child_original_to_proposed().at({"roof","child-b"}).roof_id=="roof",
        "qualified child identity correspondence must exactly cover the requested roster");
    require(!prepared.selected_original_to_proposed().contains("roof"),"containing ordinary roof must remain unselected");
    for (const auto& request:requests) check_child_math(source.entities().at("roof"),final.at("roof"),request);
    auto before_body=source.entities().at("roof"),after_body=final.at("roof");
    before_body.properties.erase("roof_openings");after_body.properties.erase("roof_openings");
    // Typed changed-child receipts live beside the opaque roof extension.
    // They are child authority, so exclude only that known receipt container
    // when comparing the unchanged body and its complete opaque envelope.
    before_body.extensions.erase("roof_opening_input");after_body.extensions.erase("roof_opening_input");
    require(exact(before_body,after_body),"roof body, schema, pose, body quantity receipts and opaque metadata must remain exact");
    const auto& receipts=final.at("roof").extensions.at("roof_opening_input").at("entries");
    require(receipts.size()==2 && receipts.contains("child-a") && receipts.contains("child-b"),
        "derived scalar receipts must belong only to the selected existing children");
    require(opening_row(source.entities().at("roof"),"sibling").dump()==opening_row(final.at("roof"),"sibling").dump(),
        "unselected sibling must remain byte exact on a changed host");
    for (const auto* id:{"roof-catalog","roof-dim-0","roof-profile-dim"})
        require(exact(source.entities().at(id),final.at(id)),"child edits must retain type-owned hosted bodies and dimension carriers");
    for (const auto* id:{"expected_revision","source_snapshot_digest","source_authoring_digest","source_entities_digest","source_saved_revision"})
        require(wire.at(id).dump()==prepared.intent().at(id).dump(),"generated proof must bind the exact complete original fences");
    require(document_snapshot_digest(document.snapshot())==digest,"preparation must not change source document or history");
    const auto replayed=replay_phase_constraint_authoring(source.entities(),wire);
    require(exact(replayed.at("roof"),final.at("roof")),"generated phase32 must independently reconstruct complete child cohort");
    require(prepared.required_room_reviews().empty() && prepared.required_phase_room_reviews().empty(),
        "mixed40 grants no room authority");
}
AssemblyPoint3 component_point_world(AssemblyPoint3 point) {
    const auto transform=[](AssemblyPoint3 point,AssemblyPoint3 origin,double yaw,bool mirror=false) {
        if (mirror) point.y=-point.y;
        const auto rotated=rotate({point.x,point.y},yaw);
        return AssemblyPoint3{origin.x+rotated.x,origin.y+rotated.y,origin.z+point.z};
    };
    point=transform(point,{.3,-.2,.75},.17);
    point=transform(point,{5,1,.75},.29,true);
    point=transform(point,{2,-1,.5},-.14);
    return transform(point,{12,-9,3},.21);
}
void component_requests_source_frames_and_phase_partitions() {
    for (int version:{7,8,9}) for (int phase:{0,1,2}) {
        auto seed=fixture(version,phase==1);auto entities=seed.snapshot().entities();
        if (phase==2) {
            std::vector<std::string> proposed{"component","component-dim-0"};
            if (version==7) proposed.push_back("component-profile-dim");
            auto model=ModelPhases::create(proposed,{},
                {RemodelingAlternative{"alternative","Saved proposed component",{},proposed}},"alternative");
            entities.emplace("phases",Entity{"phases","model_phases",{{"model",model.to_json()}},false});
        }
        auto document=from_entities(entities);const auto source=document.snapshot();
        const auto prepared=prepare_component_roof_opening_directional_resize(source,component_edit(source,version),axis_requests(),"Original independent source frames");
        const auto& final=prepared.preview_snapshot().entities();
        const auto destination=phase==1 ? "proposed-component" : "component";
        const auto dimension_id=phase==1 ? "proposed-component-dim-0" : "component-dim-0";
        require(prepared.intent().at("component_request").at("version")==version &&
            prepared.selected_original_to_proposed().at("component")==destination,
            "component requests7/8/9 must preserve actual ordinary/baseline/proposed destinations");
        require_near(span(final,dimension_id),7.2,"nested reflected source width must follow own profile geometry");
        const auto before=decode_boundary_dimension_entity(source.entities().at("component-dim-0"));
        const auto after=decode_boundary_dimension_entity(final.at(dimension_id));
        require(before.dimension->presentation==after.dimension->presentation &&
            before.dimension->text_position.x==after.dimension->text_position.x &&
            before.dimension->text_position.y==after.dimension->text_position.y &&
            after.dimension->assembly_target->component.part_path==std::vector<std::string>{"child"} &&
            final.at(dimension_id).extensions.dump()==source.entities().at("component-dim-0").extensions.dump(),
            "final carry must preserve nested selectors, exact paper presentation and opaque envelopes");
        if (version==7) {
            const auto profile_id=phase==1 ? "proposed-component-profile-dim" : "component-profile-dim";
            const auto decoded=decode_boundary_dimension_entity(final.at(profile_id));
            require(decoded.dimension->assembly_target->profile==selector("new-"),"explicit component profile decision must retain its new edge identity");
            const auto resolved=resolve_assembly_dimension(final,*decoded.dimension->assembly_target);
            const auto start=component_point_world({0,0,.4}),end=component_point_world({7.2,0,.4});
            require_near(resolved.start_world_m.x,start.x,"independent nested profile start must include actual Site translation and conjugation");
            require_near(resolved.start_world_m.y,start.y,"independent profile start must include actual Site yaw once");
            require_near(resolved.end_world_m.x,end.x,"independent reflected edge must use its own Site frame");
            require_near(resolved.end_world_m.y,end.y,"independent reflected endpoint must retain nested parent pose");
        }
        require(mixed_selection_property_edit_has_phase_replacements(prepared.intent())==(phase==1),
            "only actual baseline component replacement may declare allocation authority");
        if (phase==1) {
            for (const auto* id:{"component","component-dim-0"})
                require(exact(source.entities().at(id),final.at(id)),"baseline originals must remain byte exact beside their final copies");
            require(prepared.reserved_component_identity_ids()==
                prepared.intent().at("component_identity_reservations").get<std::vector<std::string>>(),
                "component reservation receipt must exactly expose source-derived body and dimension roles");
            const auto authoring=decode_phase_assembly_instance_property_edit_authoring(prepared.intent().at("component_request").at("authoring"));
            const auto body=authoring.allocation.roots.begin()->second;
            require(body.proposed_entity_id==std::optional<std::string>{"proposed-component"} &&
                !body.proposed_catalog_id && body.proposed_instance_id=="proposed-component",
                "baseline body allocation must retain the independent owner and instance roles");
        }
    }
}
void reviewed_retirements_units_and_association_bound() {
    for (bool baseline:{false,true}) {
        auto document=fixture(7,baseline);const auto source=document.snapshot();
        const auto prepared=prepare_component_roof_opening_directional_resize(source,component_edit(source,7,true),axis_requests(true),"Reviewed component retirement beside unit children");
        const auto& final=prepared.preview_snapshot().entities();
        require(final.contains("component-profile-dim")==baseline && !final.contains("proposed-component-profile-dim"),
            "reviewed retirement must remove only the admitted ordinary carrier or elide the clone");
        require(mixed_selection_property_edit_retires_dependencies(prepared.intent()),"mixed40 must classify its explicit component retirement");
        require(exact(final.at("roof"),source.entities().at("roof")) && prepared.roof_child_original_to_proposed().size()==2,
            "unit children must preserve the exact roof while retaining their full qualified request inventory");
        const auto proof=decode_phase_constraint_authoring_intent(prepared.phase_authorings().front());
        require(proof.ordinary_roof_edits.front().at("openings").at("upserts").size()==2,
            "unit requests must persist in the generated source proof before no-op elimination");
    }
    auto document=fixture(7,false,1,999,false);const auto source=document.snapshot();
    const auto prepared=prepare_component_roof_opening_directional_resize(source,component_edit(source),axis_requests(true),"One thousand shared associations including units");
    require(exact(prepared.preview_snapshot().entities().at("roof-dim-998"),source.entities().at("roof-dim-998")),
        "999 no-upsert hosted associations must share the bound with the independent component association");
    bool refused=false;
    try { (void)fixture(7,false,1,1000,false); }
    catch (const DocumentError& error) {
        require(error.code()==DocumentErrorCode::invalid_entity &&
            std::string(error.what()).find("dimension inventory exceeds its shared resolution bound")!=std::string::npos,
            "1001 associations must refuse at the actual document inventory boundary");refused=true;
    }
    require(refused,"source admission must refuse over-budget associations");
    // No independent association creates a reviewed-node request. Hosted
    // dimensions must still take the ordinary final-carry branch.
    auto seed=fixture(7,false,0,1,false);auto entities=seed.snapshot().entities();
    entities.emplace("roof-profile-dim",dimension("roof-profile-dim","roof-catalog","roof-root",std::nullopt,true));
    auto dimensionless=from_entities(entities);const auto original=dimensionless.snapshot();
    const auto unreviewed=prepare_component_roof_opening_directional_resize(original,component_edit(original),axis_requests(),"Dimensionless component and hosted profile dimensions");
    require(exact(unreviewed.preview_snapshot().entities().at("roof-profile-dim"),original.entities().at("roof-profile-dim")),
        "ordinary carry must retain hosted profile dimensions when no requested root has reviewed nodes");
}
void unit_hosts_refuse_every_registered_phase_role() {
    for (int role=0;role<5;++role) {
        auto seed=fixture(7,false,1,0,false);auto entities=seed.snapshot().entities();
        // Remove hosted roots so inactive roofs can be valid retained owners
        // without adding an inactive analytical dimension to this source.
        entities.at("roof-catalog")=catalog("roof-catalog",false,true,{});
        std::vector<std::string> baseline=role<3 ? std::vector<std::string>{"roof"} : std::vector<std::string>{};
        std::vector<RemodelingAlternative> alternatives;
        alternatives.push_back({"alternative","Registered roof role",role==2 ? std::vector<std::string>{"roof"} : std::vector<std::string>{},
            role>=3 ? std::vector<std::string>{"roof"} : std::vector<std::string>{}});
        std::optional<std::string> active=role==0 || role==4 ? std::nullopt : std::optional<std::string>{"alternative"};
        const auto model=ModelPhases::create({"roof"},baseline,alternatives,active);
        entities.emplace("roof-phases",Entity{"roof-phases","model_phases",{{"model",model.to_json()}},false});
        auto document=from_entities(entities);const auto source=document.snapshot();
        invalid([&] { (void)prepare_component_roof_opening_directional_resize(source,component_edit(source),axis_requests(true),"Registered unit roof refusal"); },
            "unit children must refuse registered baseline, existing, demolished, active proposed and inactive proposed hosts");
    }
}
void shared_catalog_retains_exact_hosted_rows() {
    for (int version:{7,8,9}) {
        auto document=fixture(version,false,1,1,true,true);const auto source=document.snapshot();
        const auto prepared=prepare_component_roof_opening_directional_resize(source,component_edit(source,version),axis_requests(),"Shared definitions and promoted independent own geometry");
        const auto& final=prepared.preview_snapshot().entities();
        require(final.at("component").properties.at("instance").at("schema")=="sketch.assembly-instance.v4",
            "mapped component own geometry must retain its canonical schema promotion");
        require(exact(source.entities().at("component-catalog"),final.at("component-catalog")),
            "independent schema promotion must preserve the shared catalog, definitions and qualified hosted row");
        require(exact(source.entities().at("roof-dim-0"),final.at("roof-dim-0")),
            "shared catalog dimensions must resolve the unchanged hosted own geometry after complete composition");
        require_near(span(final,"roof-dim-0"),4,"shared catalog hosted span must not borrow the independent profile override");
    }
}
void closed_wire_source_and_authority_refusals() {
    auto document=fixture();const auto source=document.snapshot();const auto authoring=component_edit(source);
    const auto prepared=prepare_component_roof_opening_directional_resize(source,authoring,axis_requests(),"Closed original-source roof child operators");
    for (const auto* key:{"suppliedEntities","base_intent","slab_geometry","roof_authoring","roof_edits","roof_child_original_to_proposed"}) {
        auto forged=prepared.intent();forged[key]=Json::object();
        invalid([&] { (void)replay_mixed_selection_property_edit_preparation(source,forged); },"mixed40 must refuse extra caller leaves, maps and historical authority");
    }
    for (const auto* key:{"message","roof_opening_axis_requests","component_identity_reservations","source_saved_revision"}) {
        auto forged=prepared.intent();forged.erase(key);
        invalid([&] { (void)replay_mixed_selection_property_edit_preparation(source,forged); },"mixed40 must refuse missing closed fields");
    }
    for (const auto* key:{"roof_id","opening_id","sx","sy","anchor_m","frame_rotation_radians"}) {
        auto forged=prepared.intent();forged["roof_opening_axis_requests"][0].erase(key);
        invalid([&] { (void)validate_mixed_selection_property_edit_intent(forged); },"axis requests must refuse missing exact row fields");
    }
    auto forged=prepared.intent();forged["roof_opening_axis_requests"][0]["leaf"]=Json::object();
    invalid([&] { (void)validate_mixed_selection_property_edit_intent(forged); },"axis request must refuse appended leaf authority");
    forged=prepared.intent();forged["roof_opening_axis_requests"].push_back(forged["roof_opening_axis_requests"][0]);
    invalid([&] { (void)validate_mixed_selection_property_edit_intent(forged); },"duplicate qualified child operator must refuse");
    forged=prepared.intent();std::swap(forged["roof_opening_axis_requests"][0],forged["roof_opening_axis_requests"][1]);
    invalid([&] { (void)validate_mixed_selection_property_edit_intent(forged); },"persisted child operators must refuse unsorted input");
    for (const auto* lane:{"room_reviews","phase_room_reviews","roof_opening_axis_requests"}) {
        forged=prepared.intent();forged[lane]=lane==std::string_view("roof_opening_axis_requests") ? Json::array() : Json::array({Json::object()});
        invalid([&] { (void)validate_mixed_selection_property_edit_intent(forged); },"child roster is mandatory and room authority remains empty");
    }
    for (double scale:{0.0,-1.0}) {
        forged=prepared.intent();forged["roof_opening_axis_requests"][0]["sx"]=scale;
        invalid([&] { (void)validate_mixed_selection_property_edit_intent(forged); },"nonpositive axis scale must refuse");
    }
    invalid([&] { (void)prepare_component_roof_opening_directional_resize(source,PhaseAssemblyInstancePropertyEditAuthoring{},axis_requests(),"Empty component lane"); },
        "mixed40 must require a nonempty independent component roster");
    invalid([&] { (void)prepare_component_roof_opening_directional_resize(source,authoring,std::span<const RoofOpeningDirectionalResizeRequest>{},"Empty child lane"); },
        "mixed40 must require a nonempty selected child roster");
    auto oversized=std::vector<RoofOpeningDirectionalResizeRequest>(1000,axis_requests().front());
    invalid([&] { (void)prepare_component_roof_opening_directional_resize(source,authoring,oversized,"Combined semantic member bound"); },
        "component intents and qualified children must share the 1000-member bound");
    auto bad=axis_requests();bad.front().frame_rotation_radians=std::numeric_limits<double>::infinity();
    invalid([&] { (void)prepare_component_roof_opening_directional_resize(source,authoring,bad,"Nonfinite operator refusal"); },"nonfinite typed angle must refuse");
    bad=axis_requests();bad.front().anchor_m.x=std::numeric_limits<double>::quiet_NaN();
    invalid([&] { (void)prepare_component_roof_opening_directional_resize(source,authoring,bad,"Nonfinite anchor refusal"); },"nonfinite typed anchor must refuse");
    bad=axis_requests();bad.front().target.opening_id="missing-child";
    invalid([&] { (void)prepare_component_roof_opening_directional_resize(source,authoring,bad,"Missing actual child refusal"); },"every child must be an actual existing profiled opening");
    auto scalar=authoring;scalar.intents[0].properties.emplace("label",AssemblyInstancePropertyPatch<std::string>{AssemblyInstancePropertyPatchOperation::set,"unauthorized"});
    invalid([&] { (void)prepare_component_roof_opening_directional_resize(source,scalar,axis_requests(),"Scalar refusal"); },"mixed40 component lane must refuse scalar authority");
    auto pose=authoring;pose.intents[0].pose.operation=AssemblyInstancePropertyPatchOperation::set;pose.intents[0].pose.x_metres=50;
    invalid([&] { (void)prepare_component_roof_opening_directional_resize(source,pose,axis_requests(),"Pose refusal"); },"mixed40 component lane must refuse pose authority");
    auto material=authoring;material.intents[0].materials.emplace("unauthorized",AssemblyInstancePropertyPatch<std::string>{AssemblyInstancePropertyPatchOperation::set,"material"});
    invalid([&] { (void)prepare_component_roof_opening_directional_resize(source,material,axis_requests(),"Material refusal"); },"mixed40 component lane must refuse material authority");
    auto quantity=authoring;quantity.intents[0].quantities.emplace("unauthorized",AssemblyInstancePropertyPatch<AssemblyQuantityProperty>{
        AssemblyInstancePropertyPatchOperation::set,AssemblyQuantityProperty{2,AssemblyQuantityUnit::count}});
    invalid([&] { (void)prepare_component_roof_opening_directional_resize(source,quantity,axis_requests(),"Quantity refusal"); },"mixed40 component lane must refuse quantity authority");
    auto embedded=authoring;embedded.intents[0].target={"roof-catalog","roof-root",std::nullopt,{"child"}};
    embedded.profile_dimension_decisions.clear();embedded.profile_dimension_decisions.emplace("roof-profile-dim",selector("new-"));
    invalid([&] { (void)prepare_component_roof_opening_directional_resize(source,embedded,axis_requests(),"Embedded refusal"); },"mixed40 component lane must require independent document geometry");
    forged=prepared.intent();forged["component_request"]["source_entities_digest"]=std::string(64,'0');
    invalid([&] { (void)replay_mixed_selection_property_edit_preparation(source,forged); },"component source fences must match complete original snapshot");
    forged=prepared.intent();forged["source_entities_digest"]=std::string(64,'0');forged["component_request"]["source_entities_digest"]=std::string(64,'0');
    invalid([&] { (void)replay_mixed_selection_property_edit_preparation(source,forged); },"matching forged lane fences must still fail actual source authentication");
    forged=prepared.intent();forged["component_identity_reservations"]=Json::array({"unowned-reservation"});
    invalid([&] { (void)replay_mixed_selection_property_edit_preparation(source,forged); },"reservation receipt must equal independently reconstructed identities");
    forged=prepared.intent();forged["version"]=39;
    invalid([&] { (void)validate_mixed_selection_property_edit_intent(forged); },"mixed39 must retain its exact nonempty slab contract");
    auto changed=source.entities().at("property");changed.properties["name"]="Later source";
    document.apply(ApplyEntityChanges{.expected_revision=document.revision(),.entity_changes={EntityChange::upsert(changed)},.message="Change source"});
    invalid([&] { require_mixed_selection_property_edit_source_current(document.snapshot(),prepared); },"stale complete source must refuse prepared operator replay");
}
void reserved_roles_and_history_only_destinations() {
    auto document=fixture(7,true);const auto source=document.snapshot();
    const auto prepared=prepare_component_roof_opening_directional_resize(source,component_edit(source),axis_requests(),"Exact reserved body and dimension roles");
    const auto authoring=decode_phase_assembly_instance_property_edit_authoring(prepared.intent().at("component_request").at("authoring"));
    std::set<std::string> expected{"proposed-component","proposed-component-dim-0","proposed-component-profile-dim"};
    require(std::set<std::string>(prepared.reserved_component_identity_ids().begin(),prepared.reserved_component_identity_ids().end())==expected,
        "body and exact source dimension copies must be the only mixed40 reservation roles");
    require(authoring.dimension_identities.size()==2 && authoring.dimension_identities.at("component-dim-0")=="proposed-component-dim-0" &&
        authoring.dimension_identities.at("component-profile-dim")=="proposed-component-profile-dim",
        "source-derived exact dimensions must retain their independent carrier role");
    auto bad=component_edit(source);bad.dimension_identities.erase("component-dim-0");
    invalid([&] { (void)prepare_component_roof_opening_directional_resize(source,bad,axis_requests(),"Missing reserved dimension"); },
        "baseline source inventory must refuse a missing dimension copy");
    bad=component_edit(source);bad.dimension_identities.emplace("roof-dim-0","unowned-roof-copy");
    invalid([&] { (void)prepare_component_roof_opening_directional_resize(source,bad,axis_requests(),"Extra reserved dimension"); },
        "child lane must not borrow a roof-hosted dimension clone role");
    bad=component_edit(source);bad.dimension_identities["component-dim-0"]="proposed-component";
    invalid([&] { (void)prepare_component_roof_opening_directional_resize(source,bad,axis_requests(),"Overlapping reserved body and dimension"); },
        "fresh body and dimension destination roles must remain injective");
    Entity historical{"history-only-dimension","label",{{"text","Retained undone identity"}},false};
    document.apply(ApplyEntityChanges{.expected_revision=document.revision(),.entity_changes={EntityChange::upsert(historical)},.message="Reserve undone identity"});
    document.undo(document.revision());const auto retained=document.snapshot();
    bad=component_edit(retained);bad.dimension_identities["component-dim-0"]="history-only-dimension";
    invalid([&] { (void)prepare_component_roof_opening_directional_resize(retained,bad,axis_requests(),"Retained history dimension collision"); },
        "component dimension destinations must refuse identities retained only in undone history");
}
void historical_coordinated_roof_child_refusal() {
    auto document=fixture();const auto source=document.snapshot();
    const auto prepared=prepare_component_roof_opening_directional_resize(source,component_edit(source),axis_requests(),"Separate closed child authority");
    const auto generated=decode_phase_constraint_authoring_intent(prepared.phase_authorings().front());
    ConstraintAuthoringIntent semantic;semantic.message="Historical direct coordination refuses rotated roof children";
    auto proof=make_phase_constraint_authoring_intent(source,semantic,nullptr,true);
    proof.coordinated_replacements={{"version",10},{"wall_authoring",nullptr},{"roof_replacement",nullptr},
        {"slab_replacement",nullptr},{"structural_replacement",nullptr},{"stair_replacement",nullptr},
        {"ordinary_roof_edits",generated.ordinary_roof_edits},{"ordinary_slab_geometry",Json::array()},
        {"ordinary_structural_edits",Json::array()},{"ordinary_stair_transforms",Json::array()},
        {"complete_hosted_component_dimensions",true}};
    bool refused=false;
    try { (void)encode_phase_constraint_authoring_intent(proof); }
    catch (const std::invalid_argument& error) {
        require(std::string(error.what()).find("roof rotation authority")!=std::string::npos,
            "outer46/directinner10 must refuse child9 at the historical roof authority boundary");refused=true;
    }
    require(refused,"closed mixed40 must leave historical direct roof-child refusal intact");
}
void downgrade_mixed_reader_markers(const std::filesystem::path& file) {
    sqlite3* raw=nullptr;
    const auto u8=file.u8string();
    const std::string name(reinterpret_cast<const char*>(u8.data()),u8.size());
    const auto opened=sqlite3_open_v2(name.c_str(),&raw,SQLITE_OPEN_READWRITE,nullptr);
    std::unique_ptr<sqlite3,decltype(&sqlite3_close)> database(raw,&sqlite3_close);
    require(opened==SQLITE_OK,"open controlled saved mixed native project");
    const auto check_markers=[&](int expected) {
        sqlite3_stmt* statement=nullptr;
        const auto prepared=sqlite3_prepare_v2(database.get(),
            "SELECT value FROM metadata WHERE key='format_version'",-1,&statement,nullptr);
        std::unique_ptr<sqlite3_stmt,decltype(&sqlite3_finalize)> query(statement,&sqlite3_finalize);
        require(prepared==SQLITE_OK && sqlite3_step(query.get())==SQLITE_ROW,
            "read controlled mixed native metadata marker");
        require(sqlite3_column_type(query.get(),0)==SQLITE_TEXT &&
            std::string(reinterpret_cast<const char*>(sqlite3_column_text(query.get(),0)))==std::to_string(expected),
            "mixed native metadata must advertise the exact expected reader");
        query.reset();statement=nullptr;
        const auto pragma=sqlite3_prepare_v2(database.get(),"PRAGMA user_version",-1,&statement,nullptr);
        query.reset(statement);
        require(pragma==SQLITE_OK && sqlite3_step(query.get())==SQLITE_ROW &&
            sqlite3_column_int(query.get(),0)==expected,
            "mixed native marker pair must agree on the exact expected reader");
    };
    check_markers(263);
    require(sqlite3_exec(database.get(),
        "BEGIN; PRAGMA user_version=262; UPDATE metadata SET value='262' WHERE key='format_version'; COMMIT",
        nullptr,nullptr,nullptr)==SQLITE_OK,"downgrade only the controlled mixed reader marker pair");
    check_markers(262);
    database.reset();
    const auto digest=ProjectStore::file_sha256(file);
    bool refused=false;
    try { (void)ProjectStore::load(file); }
    catch (const StorageError& error) {
        require(error.code()==StorageErrorCode::unsupported_format &&
            std::string(error.what()).find("requires reader 263")!=std::string::npos,
            "valid previous-reader markers must reject actual mixed semantics at the reader263 gate");
        refused=true;
    }
    require(refused,"reader262 markers must refuse retained canonical mixed40 command52 authority");
    require(ProjectStore::file_sha256(file)==digest,"refused mixed reader downgrade must not rewrite any file bytes");
}
void command_history_replay() {
    for (bool baseline:{false,true}) {
        auto document=fixture(7,baseline);const auto source=document.snapshot();
        require(ProjectStore::required_format_version(source)<263 && ProjectStore::required_semantic_format_version(source)<263,
            "mixed fixture must not obtain reader263 from preexisting entity state");
        const auto prepared=prepare_component_roof_opening_directional_resize(source,component_edit(source),axis_requests(),"Atomic mixed directional resize");
        ApplyBoundaryConstraintChanges command;
        command.expected_revision=source.revision();command.message=prepared.intent().at("message").get<std::string>();
        command.mixed_selection_edit_completion=true;
        command.mixed_selection_edit_intent=complete_mixed_selection_property_edit(source,prepared,{},{});
        const auto wire=command_to_json(command);
        require(wire.at("kind")=="apply_boundary_constraint_changes" && wire.at("version")==52 &&
            wire.at("mixed_selection_edit_intent").at("version")==40,
            "reader263 fixture must carry exact canonical command52 mixed40 authority");
        document.apply(command);
        require(document.snapshot().entities()==prepared.preview_snapshot().entities(),"existing command 52 must publish both lanes atomically after root integration");
        const auto final=document.snapshot();
        require(ProjectStore::required_semantic_format_version(final)==263,"retained mixed resize command must require the reserved semantic floor");
        document.undo(document.revision());require(document.snapshot().entities()==source.entities(),"one undo must restore both source families and all dimensions");
        const auto undone=document.snapshot();
        document.redo(document.revision());require(document.snapshot().entities()==final.entities(),"one redo must reconstruct the exact complete final graph");
        for (const auto& snapshot:{final,undone,document.snapshot()}) {
            require(ProjectStore::required_format_version(snapshot)==263 && ProjectStore::required_semantic_format_version(snapshot)==263,
                "applied, undone and redone mixed command history must retain exact native and semantic reader263");
            const ScopedProjectFile file;
            (void)ProjectStore::save(file.path,snapshot);
            const auto loaded=ProjectStore::load(file.path);
            const auto restored=loaded.document.snapshot();
            require(restored.entities()==snapshot.entities() && restored.revision()==snapshot.revision() &&
                restored.history().size()==snapshot.history().size(),
                "storage replay must reproduce both families, source envelopes and retained navigation history");
            require(restored.history().back().undo_stack==snapshot.history().back().undo_stack &&
                restored.history().back().redo_stack==snapshot.history().back().redo_stack,
                "native reopen must retain actual mixed undo and redo destinations");
            const auto& retained=restored.history().at(final.revision()).boundary_constraint_changes;
            require(retained && command_to_json(*retained)==wire,"native reopen must retain exact canonical mixed command authority");
            require(ProjectStore::required_format_version(restored)==263 && ProjectStore::required_semantic_format_version(restored)==263,
                "reopened applied, undone and redone mixed histories must retain reader263");
            const auto extracted=file.directory/"extracted";
            extract_project(snapshot,extracted);
            std::ifstream input(extracted/"project.json");
            const auto manifest=Json::parse(input);
            require(manifest.at("exchange_version")==261 && manifest.at("revisions").size()==snapshot.history().size(),
                "applied, undone and redone mixed histories must extract with exact exchange261");
            require(manifest.at("revisions").at(final.revision()).at("boundary_constraint_changes")==wire,
                "mixed extraction must retain canonical command52 mixed40 rather than infer authority from entity data");
            downgrade_mixed_reader_markers(file.path);
        }
    }
}
void opaque_mixed_shapes_do_not_raise_reader_floor() {
    auto document=fixture();const auto source=document.snapshot();
    const auto floor=ProjectStore::required_format_version(source);
    const auto semantic_floor=ProjectStore::required_semantic_format_version(source);
    require(floor<263 && semantic_floor<263,"opaque metadata fixture must start below the mixed reader floor");
    const auto prepared=prepare_component_roof_opening_directional_resize(source,component_edit(source),axis_requests(),"Opaque command-shaped user metadata");
    ApplyBoundaryConstraintChanges command;
    command.expected_revision=source.revision();command.message=prepared.intent().at("message").get<std::string>();
    command.mixed_selection_edit_completion=true;
    command.mixed_selection_edit_intent=complete_mixed_selection_property_edit(source,prepared,{},{});
    const auto wire=command_to_json(command);
    require(wire.at("version")==52 && command.mixed_selection_edit_intent.at("version")==40,
        "opaque collision must contain genuine canonical command52 and closed mixed40 data");
    auto property=source.entities().at("property");
    property.properties["user_metadata"]={{"version",40},{"intent",command.mixed_selection_edit_intent},{"command",wire}};
    property.extensions["vendor"]=wire;
    Entity opaque{"opaque-mixed-data","vendor",command.mixed_selection_edit_intent,false};
    opaque.extensions={{"version",40},{"boundary_constraint_changes",wire}};
    document.apply(ApplyEntityChanges{.expected_revision=document.revision(),
        .entity_changes={EntityChange::upsert(property),EntityChange::upsert(opaque)},.message="Retain opaque mixed-shaped metadata"});
    const auto head=document.snapshot();
    document.undo(document.revision());
    for (const auto& snapshot:{head,document.snapshot()}) {
        require(ProjectStore::required_format_version(snapshot)==floor &&
            ProjectStore::required_semantic_format_version(snapshot)==semantic_floor,
            "current and undone opaque user/entity command52 mixed40 metadata must retain their original reader floors");
        for (const auto& revision:snapshot.history())
            require(!revision.boundary_constraint_changes,"opaque collision fixture must grant no canonical command authority");
        const ScopedProjectFile file;
        (void)ProjectStore::save(file.path,snapshot);
        const auto restored=ProjectStore::load(file.path).document.snapshot();
        require(restored.entities()==snapshot.entities() && restored.history().at(head.revision()).entities==head.entities(),
            "native reopen must preserve current and retained opaque mixed-shaped metadata exactly");
        require(ProjectStore::required_format_version(restored)==floor &&
            ProjectStore::required_semantic_format_version(restored)==semantic_floor,
            "persisted opaque command-shaped metadata must not acquire mixed reader263 authority");
        const auto extracted=file.directory/"extracted";
        extract_project(snapshot,extracted);
        std::ifstream input(extracted/"project.json");
        const auto manifest=Json::parse(input);
        require(manifest.at("exchange_version")==semantic_floor-2,
            "opaque command-shaped metadata extraction must retain the fixture's prior exchange floor");
        for (const auto& revision:manifest.at("revisions"))
            require(!revision.contains("boundary_constraint_changes"),"opaque metadata extraction must not fabricate canonical command proof slots");
    }
}

} // namespace
int main() {
    sketch::testing::noninteractive_errors();
    const char* active="initialization";
    try {
        const std::pair<const char*,void(*)()> tests[]{
            {"same_host_cohort_and_complete_proof",same_host_cohort_and_complete_proof},
            {"component_requests_source_frames_and_phase_partitions",component_requests_source_frames_and_phase_partitions},
            {"reviewed_retirements_units_and_association_bound",reviewed_retirements_units_and_association_bound},
            {"unit_hosts_refuse_every_registered_phase_role",unit_hosts_refuse_every_registered_phase_role},
            {"shared_catalog_retains_exact_hosted_rows",shared_catalog_retains_exact_hosted_rows},
            {"closed_wire_source_and_authority_refusals",closed_wire_source_and_authority_refusals},
            {"reserved_roles_and_history_only_destinations",reserved_roles_and_history_only_destinations},
            {"historical_coordinated_roof_child_refusal",historical_coordinated_roof_child_refusal},
            {"command_history_replay",command_history_replay},
            {"opaque_mixed_shapes_do_not_raise_reader_floor",opaque_mixed_shapes_do_not_raise_reader_floor}};
        for (const auto& [name,test]:tests) { active=name;test(); }
        std::cout << "mixed_component_skylight_resize_tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << active << ": " << error.what() << '\n';return 1;
    }
}
