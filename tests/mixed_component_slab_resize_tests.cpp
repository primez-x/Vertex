#include "sketch/mixed_selection_property_edit.hpp"
#include "sketch/assembly_dimension.hpp"
#include "sketch/assembly_model.hpp"
#include "sketch/boundary_dimension.hpp"
#include "sketch/document_digest.hpp"
#include "sketch/model_phases.hpp"
#include "sketch/phase_constraint_authoring.hpp"
#include "sketch/project_exchange.hpp"
#include "sketch/project_store.hpp"
#include "sketch/slab_plan_geometry.hpp"
#include "sketch/slab_hosted_geometry_edit.hpp"
#include "support/noninteractive_errors.hpp"

#include <sqlite3.h>
#include <cmath>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <numbers>
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
                ("vertex-mixed-component-slab-"+std::to_string(stamp)+"-"+std::to_string(attempt));
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
Json raw_ring(const Boundary& value) {
    auto result=Json::array();
    for (const auto& edge:value) result.push_back({{"start",{edge.start.x,edge.start.y}},
        {"end",{edge.end.x,edge.end.y}},{"sweep_radians",edge.sweep_radians}});
    return result;
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
Entity slab(const std::string& id,bool curved) {
    auto p=context;p["boundary"]=raw_ring(ring(curved));p["holes"]=Json::array();
    p["thickness_m"]=.2;p["elevation_m"]=1.25;
    p["layers"]=Json::array({{{"id",id+"-core"},{"thickness_m",.2}}});
    Entity result{id,"slab",p,false};result.extensions={{"vendor",{{"retain","slab-envelope"}}}};
    return result;
}
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
Document fixture(bool curved=false,bool component_baseline=false,bool slab_baseline=false,
    int component_version=7,std::size_t component_dimensions=1,std::size_t unit_dimensions=1,
    bool profile_dimensions=true) {
    std::vector<Entity> values{
        {"property","property",{{"site_frame",{{"version",1},{"origin_m",{12.,-9.,3.}},
            {"rotation_radians",.21},{"vertical_datum",{{"identifier","local"},{"height_at_origin_m",0.}}}}}},false},
        {"building","building",{{"property_id","property"},{"site_placement",{{"version",1},
            {"translation_m",{2.,-1.,.5}},{"rotation_radians",-.14}}}},false},
        {"floor","floor",{{"building_id","building"}},false},
        {"layer","layer",{{"floor_id","floor"}},false},slab("slab",curved),slab("unit-slab",false),
        catalog("component-catalog",curved,component_version==7,{}),
        catalog("slab-catalog",curved,true,{hosted("slab-root","slab")}),
        catalog("unit-catalog",false,true,{hosted("unit-root","unit-slab")})};
    AssemblyInstance independent;independent.id="component";independent.type_id="parent";
    independent.root_transform=AssemblyTransform{{5,1,.75},.29,1,true};
    auto owner=encode_document_assembly_instance({"component","assembly_instance",context},
        {"component-catalog",independent});
    owner.extensions={{"vendor",{{"retain","independent-envelope"}}}};values.push_back(owner);
    for (std::size_t i=0;i<component_dimensions;++i)
        values.push_back(dimension("component-dim-"+std::to_string(i),"component-catalog","component",std::string{"component"}));
    for (std::size_t i=0;i<unit_dimensions;++i)
        values.push_back(dimension("unit-dim-"+std::to_string(i),"unit-catalog","unit-root",std::nullopt));
    if (profile_dimensions && component_version==7)
        values.push_back(dimension("component-profile-dim","component-catalog","component",std::string{"component"},true));
    if (profile_dimensions) {
        values.push_back(dimension("slab-dim","slab-catalog","slab-root",std::nullopt));
        values.push_back(dimension("slab-profile-dim","slab-catalog","slab-root",std::nullopt,true));
    }
    if (component_baseline || slab_baseline) {
        std::vector<std::string> baseline;
        if (component_baseline) {
            baseline.push_back("component");
            for (std::size_t i=0;i<component_dimensions;++i) baseline.push_back("component-dim-"+std::to_string(i));
            if (profile_dimensions && component_version==7) baseline.push_back("component-profile-dim");
        }
        if (slab_baseline) {
            baseline.insert(baseline.end(),{"slab","slab-catalog"});
            if (profile_dimensions) baseline.insert(baseline.end(),{"slab-dim","slab-profile-dim"});
        }
        const auto model=ModelPhases::create(baseline,baseline,
            {RemodelingAlternative{"alternative","Actual saved choice",{}, {}}},"alternative");
        values.push_back({"phases","model_phases",{{"model",model.to_json()}},false});
    }
    return Document::create(values);
}
std::vector<SlabGeometryEditIntent> slab_edits(bool unit_only=false) {
    SlabGeometryEditIntent changed;
    changed.slab_id="slab";changed.kind=SlabGeometryEditKind::resize_plan;
    changed.resize=SlabPlanAxisResize{1.8,.65,{.75,-.25},.37};changed.coordinate_world_hosted_geometry=true;
    auto unit=changed;unit.slab_id="unit-slab";unit.resize->scale_x=1;unit.resize->scale_y=1;
    return unit_only ? std::vector<SlabGeometryEditIntent>{unit} : std::vector<SlabGeometryEditIntent>{unit,changed};
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
std::optional<ComponentSlabDirectionalResizeAllocation> slab_allocation(const DocumentSnapshot& source) {
    auto slabs=slab_edits();
    for (auto& intent:slabs) intent.preserve_planar_source_geometry=
        slab_geometry_edit_requires_planar_source(source.entities().at(intent.slab_id),intent);
    const auto partition=partition_phase_slab_geometry_edits(source.entities(),slabs);
    if (!partition.replacement) return std::nullopt;
    const auto& request=*partition.replacement;
    const auto plan=inspect_phase_slab_replacement_plan(source.entities(),request.seed_slab_ids,
        request.registry_id,request.alternative_id,true);
    require(plan.ready(),"actual baseline slab fixture must have a ready replacement plan");
    ComponentSlabDirectionalResizeAllocation result;
    for (const auto& id:plan.required_entity_ids) result.identities.emplace(id,"proposed-"+id);
    for (const auto& id:plan.required_child_ids) result.identities.emplace(id,"proposed-"+id);
    for (const auto& root:plan.required_hosted_instance_ids) result.hosted_instance_identities.emplace(root,"proposed-"+root.second);
    for (const auto& id:plan.required_dimension_ids) result.dimension_identities.emplace(id,"proposed-"+id);
    return result;
}
double span(const Entities& entities,const std::string& id) {
    const auto dimension=decode_boundary_dimension_entity(entities.at(id));
    const std::vector<AssemblyDimensionTarget> target{*dimension.dimension->assembly_target};
    return resolve_assembly_dimensions(entities,target).front().span_metres;
}
void ordinary_proposed_and_baseline_pairs() {
    for (bool curved:{false,true}) for (bool component_baseline:{false,true}) for (bool slab_baseline:{false,true}) {
        auto document=fixture(curved,component_baseline,slab_baseline);
        const auto source=document.snapshot();const auto before_digest=document_snapshot_digest(source);
        const auto prepared=prepare_component_slab_directional_resize(source,component_edit(source),slab_edits(),
            slab_allocation(source),"Resize the two actual selected families");
        const auto& final=prepared.preview_snapshot().entities();
        require(document_snapshot_digest(document.snapshot())==before_digest,"preparation must leave original document and history untouched");
        require(prepared.intent().at("version")==39 && prepared.intent().size()==13,"mixed resize must have the closed thirteen-field envelope");
        require(prepared.intent().at("slab_geometry").at(0).at("version")== (curved ? 5 : 4),"curved slabs require source-preserving leaves only when actual geometry needs them");
        const auto component=prepared.selected_original_to_proposed().at("component");
        const auto slab_id=prepared.selected_original_to_proposed().at("slab");
        require(component==(component_baseline ? "proposed-component" : "component") &&
            slab_id==(slab_baseline ? "proposed-slab" : "slab"),"actual saved membership must determine replacement destinations independently");
        require(prepared.component_original_to_proposed().begin()->second.part_path==std::vector<std::string>{"child"},"nested selected component paths must survive replacement");
        const auto independent=decode_document_assembly_instance(final.at(component));
        require(independent.instance.root_transform==decode_document_assembly_instance(source.entities().at("component")).instance.root_transform,
            "geometry resize must retain independent root pose, including reflection and Z");
        require_near(span(final,component_baseline ? "proposed-component-profile-dim" : "component-profile-dim"),7.2,
            "reviewed edge remap must measure the exact final affine geometry");
        require(final.at(component).extensions==source.entities().at("component").extensions,"independent opaque source envelope must survive");
        require(final.at(slab_id).properties.at("elevation_m")==1.25 && final.at(slab_id).properties.at("thickness_m")==.2,
            "slab directional resize must preserve physical height and thickness");
        require(exact(final.at("unit-slab"),source.entities().at("unit-slab")) &&
            exact(final.at("unit-dim-0"),source.entities().at("unit-dim-0")),"unit hosts and dimensions must remain byte-exact while being authenticated");
        if (component_baseline) require(exact(final.at("component"),source.entities().at("component")) &&
            exact(final.at("component-profile-dim"),source.entities().at("component-profile-dim")),"component baseline original body and dimension must stay exact");
        if (slab_baseline) {
            require(exact(final.at("slab"),source.entities().at("slab")) && exact(final.at("slab-dim"),source.entities().at("slab-dim")),
                "baseline slab and dimensions must remain exact");
            require(prepared.phase_authorings().size()==1,"sealed slab replacement proof must remain visible to alternative target walkers");
            const auto proof=decode_phase_constraint_authoring_intent(prepared.phase_authorings().front());
            require(proof.source_entities_digest==entity_map_digest(source.entities()),"slab proof must bind the complete original source");
            require(proof.slab_replacement.at("version")== (curved ? 12 : 10),"replacement must use the existing canonical directional or mapped dimension wrapper");
            require(prepared.hosted_binding_original_to_proposed().at({"slab-catalog","slab-root"})==
                std::pair<std::string,std::string>{"proposed-slab-catalog","proposed-slab-root"},"slab hosted copies must expose qualified correspondence");
        } else require(prepared.phase_authorings().empty(),"ordinary slabs must have no fabricated phase proof");
        const auto wire=complete_mixed_selection_property_edit(source,prepared,{},{});
        require(replay_mixed_selection_property_edit(source,wire).entities==final,"public complete replay must reproduce the detached final map");
    }
}
void component_request_eight_and_nine() {
    for (int version:{8,9}) for (bool baseline:{false,true}) {
        auto document=fixture(true,baseline,baseline,version);
        const auto source=document.snapshot();
        const auto prepared=prepare_component_slab_directional_resize(source,component_edit(source,version),slab_edits(),slab_allocation(source),"Anonymous mapped own geometry");
        require(prepared.intent().at("component_request").at("version")==version,"mixed lane must preserve the actual request eight or nine authority");
        const auto id=baseline ? "proposed-component-dim-0" : "component-dim-0";
        require_near(span(prepared.preview_snapshot().entities(),id)/span(source.entities(),"component-dim-0"),1.8,
            "component extent must follow mapped own geometry under nested reflected poses");
    }
}
void dimensionless_component_with_hosted_slab_dimensions() {
    for (int version:{8,9,7}) for (bool slab_baseline:{false,true}) {
        auto seed=fixture(false,false,slab_baseline,version,0,1,true);
        auto entities=seed.snapshot().entities();
        // Request seven's identified fixture normally adds a component profile
        // dimension independently of component_dimensions. Remove that carrier
        // before Document admission so all three requests have no component
        // associations, while both hosted slab dimensions remain actual source.
        if (version==7) require(entities.erase("component-profile-dim")==1,
            "identified fixture must remove its only independent component dimension");
        std::vector<Entity> values;
        for (const auto& [id,entity]:entities) { (void)id;values.push_back(entity); }
        auto document=Document::create(values);
        const auto source=document.snapshot();const auto before_digest=document_snapshot_digest(source);
        for (const auto& [id,entity]:source.entities()) {
            (void)id;
            if (!can_recognize_boundary_dimension_entity_type(entity.type)) continue;
            const auto decoded=decode_boundary_dimension_entity(entity);
            require(decoded.supported() && decoded.dimension->assembly_target &&
                decoded.dimension->assembly_target->component.document_entity_id!=std::optional<std::string>{"component"},
                "regression source must contain hosted dimensions but no independent component association");
        }
        const auto authoring=component_edit(source,version);
        require(authoring.profile_dimension_decisions.empty() && authoring.dimension_identities.empty(),
            "dimensionless component geometry must require no dimension review or clone identities");
        const auto prepared=prepare_component_slab_directional_resize(source,authoring,slab_edits(),
            slab_allocation(source),"Dimensionless component and actual hosted slab dimensions");
        const auto& final=prepared.preview_snapshot().entities();
        require(prepared.intent().at("component_request").at("version")==version,
            "dimensionless geometry must retain the actual request seven, eight or nine authority");
        require(document_snapshot_digest(document.snapshot())==before_digest,
            "dimensionless mixed preparation must leave the complete source and history untouched");
        require(prepared.selected_original_to_proposed().at("component")=="component" &&
            !exact(final.at("component"),source.entities().at("component")),
            "independent component must actually resize despite having no dimension associations");
        const auto catalog_id=slab_baseline ? "proposed-slab-catalog" : "slab-catalog";
        const auto root_id=slab_baseline ? "proposed-slab-root" : "slab-root";
        const auto slab_id=slab_baseline ? "proposed-slab" : "slab";
        require(prepared.selected_original_to_proposed().at("slab")==slab_id &&
            !exact(final.at(slab_id),source.entities().at("slab")),
            "hosted slab must resize at its actual ordinary or baseline-copy destination");
        // Type-owned hosted geometry is stored in world coordinates. The slab
        // resize frame is .37 + (.21-.14)=.44 after its Site rotation, while the
        // nested width axis is .23-.17=.06 after the hosted reflection.
        // Their relative angle is -.38. Project the resized
        // 4-by-3 rectangle onto that axis; the selected e0 has length four.
        const auto c=std::cos(.38),s=std::sin(.38);
        const auto expected_width=4*(1.8*c*c+.65*s*s)+3*(1.8-.65)*s*c;
        const auto expected_edge=4*std::hypot(1.8*c,.65*s);
        for (const auto* original_id:{"slab-dim","slab-profile-dim"}) {
            const auto final_id=slab_baseline ? "proposed-"+std::string(original_id) : std::string(original_id);
            const auto before=decode_boundary_dimension_entity(source.entities().at(original_id));
            const auto after=decode_boundary_dimension_entity(final.at(final_id));
            require(before.supported() && after.supported() && before.version==after.version &&
                before.dimension->kind==after.dimension->kind &&
                before.dimension->placement==after.dimension->placement &&
                before.dimension->automatic_placement_version==after.dimension->automatic_placement_version &&
                before.dimension->presentation==after.dimension->presentation &&
                before.dimension->text_position.x==after.dimension->text_position.x &&
                before.dimension->text_position.y==after.dimension->text_position.y,
                "ordinary carry must retain hosted dimension kind, codec and exact paper presentation");
            auto expected_target=*before.dimension->assembly_target;
            expected_target.component.catalog_id=catalog_id;expected_target.component.instance_id=root_id;
            require(after.dimension->id==final_id && after.dimension->boundary_id==catalog_id &&
                after.dimension->assembly_target==expected_target,
                "hosted dimension carry must retain nested selectors while qualifying the exact destination root");
            require(final.at(final_id).type==source.entities().at(original_id).type &&
                final.at(final_id).required==source.entities().at(original_id).required &&
                final.at(final_id).extensions.dump()==source.entities().at(original_id).extensions.dump(),
                "hosted dimension carry must retain entity type and exact opaque metadata");
            const auto resolved_before=resolve_assembly_dimension(source.entities(),*before.dimension->assembly_target);
            const auto resolved_after=resolve_assembly_dimension(final,*after.dimension->assembly_target);
            require_near(resolved_before.span_metres,4,"source hosted dimension must resolve the actual four-metre own profile");
            require_near(resolved_after.span_metres,after.dimension->kind==BoundaryDimensionKind::assembly_profile ?
                expected_edge : expected_width,"carried hosted dimensions must resolve actual resized spans rather than retained labels");
            require(resolved_before.selected_node.type_id=="body" && resolved_after.selected_node.type_id=="body" &&
                resolved_after.placement_host_id==std::optional<std::string>{slab_id},
                "hosted dimension resolution must preserve the actual nested body type and destination slab association");
            if (after.dimension->kind==BoundaryDimensionKind::assembly_profile)
                require(resolved_after.profile_geometry && resolved_after.profile_geometry->affine_edge,
                    "hosted profile dimension must resolve the exact mapped source edge");
            if (slab_baseline) {
                require(exact(final.at(original_id),source.entities().at(original_id)),
                    "baseline original dimension envelope must remain exact beside its carried copy");
                require_near(span(final,original_id),4,"baseline original dimension must retain its original physical span");
            }
        }
        for (const auto* id:{"unit-slab","unit-catalog","unit-dim-0"})
            require(exact(final.at(id),source.entities().at(id)),
                "unchanged selected unit host, catalog and association must remain byte-exact");
        require_near(span(final,"unit-dim-0"),4,"unchanged unit association must retain its actual resolved span");
        if (slab_baseline) {
            require(exact(final.at("slab"),source.entities().at("slab")) &&
                exact(final.at("slab-catalog"),source.entities().at("slab-catalog")),
                "baseline original slab and hosted catalog must remain exact");
            const auto phases=ModelPhases::from_json(final.at("phases").properties.at("model"));
            const auto state=phases.active_state();
            const auto baseline_state=phases.state(std::nullopt);
            for (const auto* id:{"slab","slab-catalog","slab-dim","slab-profile-dim"})
                require(state.at(id)==(std::string(id)=="slab" ? ModelPhase::demolished : ModelPhase::existing) &&
                    state.at("proposed-"+std::string(id))==ModelPhase::proposed &&
                    baseline_state.at(id)==ModelPhase::existing,
                    "baseline hosted originals and carried copies must retain their actual phase membership");
        }
        ApplyBoundaryConstraintChanges command;
        command.expected_revision=source.revision();command.message=prepared.intent().at("message").get<std::string>();
        command.mixed_selection_edit_completion=true;
        command.mixed_selection_edit_intent=complete_mixed_selection_property_edit(source,prepared,{},{});
        require(replay_mixed_selection_property_edit(source,command.mixed_selection_edit_intent).entities==final,
            "dimensionless mixed completion must replay the exact final graph");
        document.apply(command);
        require(document.snapshot().entities()==final && document.revision()==source.revision()+1 &&
            document.snapshot().history().size()==source.history().size()+1,
            "dimensionless component, slab geometry and carried dimensions must publish as one history event");
        document.undo(document.revision());
        require(document.snapshot().entities()==source.entities(),"one undo must restore all dimensionless mixed source families");
        document.redo(document.revision());
        require(document.snapshot().entities()==final,"one redo must reconstruct the complete dimensionless mixed graph");
    }
}
void proposed_membership_and_already_mapped_sources() {
    auto ordinary=fixture(true);
    auto entities=ordinary.snapshot().entities();
    const std::vector<std::string> proposed{"component","slab","component-catalog","slab-catalog",
        "component-dim-0","component-profile-dim","slab-dim","slab-profile-dim"};
    const auto phases=ModelPhases::create(proposed,{},
        {RemodelingAlternative{"alternative","Actual proposed rows",{},proposed}},"alternative");
    entities.emplace("phases",Entity{"phases","model_phases",{{"model",phases.to_json()}},false});
    std::vector<Entity> values;
    for (const auto& [id,entity]:entities) { (void)id;values.push_back(entity); }
    auto document=Document::create(values);const auto source=document.snapshot();
    const auto prepared=prepare_component_slab_directional_resize(source,component_edit(source),slab_edits(),std::nullopt,"Existing proposed identities");
    require(prepared.selected_original_to_proposed().at("component")=="component" &&
        prepared.selected_original_to_proposed().at("slab")=="slab" && !mixed_selection_property_edit_has_phase_replacements(prepared.intent()),
        "actual active proposed members must edit their retained identities without allocation");
    auto prior=slab_edits();
    for (auto& intent:prior) intent.preserve_planar_source_geometry=
        slab_geometry_edit_requires_planar_source(ordinary.snapshot().entities().at(intent.slab_id),intent);
    const auto mapped=replay_slab_geometry_with_hosted_entities(ordinary.snapshot().entities(),prior);
    values.clear();for (const auto& [id,entity]:mapped) { (void)id;values.push_back(entity); }
    auto mapped_document=Document::create(values);const auto mapped_source=mapped_document.snapshot();
    const auto repeated=prepare_component_slab_directional_resize(mapped_source,component_edit(mapped_source),slab_edits(),std::nullopt,"Actual retained planar map");
    require(repeated.intent().at("slab_geometry")[0].at("version")==5,
        "producer must derive leaf five from an actual preexisting planar map");
    require(repeated.preview_snapshot().entities().at("slab").properties.at("boundary").dump()==
        mapped_source.entities().at("slab").properties.at("boundary").dump(),
        "repeated anisotropic curve resize must retain the analytical source ring exactly");
    auto forged=repeated.intent();forged["slab_geometry"][0]=encode_slab_geometry_edit_intent(slab_edits()[1]);
    invalid([&] { (void)replay_mixed_selection_property_edit_preparation(mapped_source,forged); },
        "recorded historical leaf four must refuse actual mapped source geometry");
}
void reviewed_retirements_and_shared_association_bound() {
    for (bool baseline:{false,true}) {
        auto document=fixture(false,baseline,false);
        const auto source=document.snapshot();
        const auto prepared=prepare_component_slab_directional_resize(source,component_edit(source,7,true),slab_edits(),std::nullopt,"Explicit reviewed retirement");
        const auto& final=prepared.preview_snapshot().entities();
        require(final.contains("component-profile-dim")==baseline,"ordinary retirement must erase only its actual source carrier; clones preserve baseline originals");
        require(!final.contains("proposed-component-profile-dim"),"reviewed clone retirement must allocate no provisional copy");
        require(mixed_selection_property_edit_retires_dependencies(prepared.intent()),"retirement classifier must retain reviewed component decisions");
    }
    // All 999 unit dimensions have unchanged output. They still share the one
    // measurement bound with an independently changed component dimension.
    auto document=fixture(false,false,false,7,1,999,false);
    const auto source=document.snapshot();
    const auto prepared=prepare_component_slab_directional_resize(source,component_edit(source),slab_edits(true),std::nullopt,"Shared unit association limit");
    require(exact(prepared.preview_snapshot().entities().at("unit-dim-998"),source.entities().at("unit-dim-998")),
        "exact boundary success must retain all no-upsert unit envelopes");
    // A live source with 1001 associations cannot reach mixed preparation: the
    // earlier Document inventory gate already refuses it. Prove that boundary
    // explicitly rather than treating an unadmitted source as a live fixture.
    bool refused=false;
    try { (void)fixture(false,false,false,7,1,1000,false); }
    catch (const DocumentError& error) {
        require(error.code()==DocumentErrorCode::invalid_entity &&
            std::string(error.what()).find("dimension inventory exceeds its shared resolution bound")!=std::string::npos,
            "1001 source associations must fail the actual Document inventory guard");
        refused=true;
    }
    require(refused,"Document admission must refuse 1001 source associations");
}
void closed_source_and_allocation_refusals() {
    auto document=fixture();const auto source=document.snapshot();
    const auto authoring=component_edit(source);
    const auto prepared=prepare_component_slab_directional_resize(source,authoring,slab_edits(),std::nullopt,"Source authentication");
    for (const auto* key:{"suppliedEntities","base_intent","coordinate_component_hosts"}) {
        auto forged=prepared.intent();forged[key]=Json::object();
        invalid([&] { (void)replay_mixed_selection_property_edit_preparation(source,forged); },"closed mixed payload must refuse appended historical or supplied candidate authority");
    }
    auto forged=prepared.intent();forged["version"]=38;
    invalid([&] { (void)validate_mixed_selection_property_edit_intent(forged); },"historical dialect must not borrow the new closed field set");
    forged=prepared.intent();forged["component_request"]["source_entities_digest"]=std::string(64,'0');
    invalid([&] { (void)replay_mixed_selection_property_edit_preparation(source,forged); },"component fences must bind the complete same original source");
    forged=prepared.intent();forged["slab_geometry"][0]["version"]=1;
    invalid([&] { (void)replay_mixed_selection_property_edit_preparation(source,forged); },"historical slab leaf cannot acquire coordinated hosted geometry");
    forged=prepared.intent();forged["slab_allocation"]={{"identities",Json::object()},
        {"hosted_instance_identities",Json::array()},{"dimension_identities",Json::object()}};
    invalid([&] { (void)replay_mixed_selection_property_edit_preparation(source,forged); },"ordinary source must require null allocation even for an empty fabricated map");
    auto scalar=authoring;scalar.intents[0].properties.emplace("label",AssemblyInstancePropertyPatch<std::string>{AssemblyInstancePropertyPatchOperation::set,"unauthorized"});
    invalid([&] { (void)prepare_component_slab_directional_resize(source,scalar,slab_edits(),std::nullopt,"Scalar refusal"); },"geometry lane must refuse scalar authority");
    auto pose=authoring;pose.intents[0].pose.operation=AssemblyInstancePropertyPatchOperation::set;pose.intents[0].pose.x_metres=50;
    invalid([&] { (void)prepare_component_slab_directional_resize(source,pose,slab_edits(),std::nullopt,"Pose refusal"); },"geometry lane must refuse pose authority");
    auto embedded=authoring;embedded.intents[0].target={"slab-catalog","slab-root",std::nullopt,{"child"}};
    embedded.profile_dimension_decisions.clear();embedded.profile_dimension_decisions.emplace("slab-profile-dim",selector("new-"));
    invalid([&] { (void)prepare_component_slab_directional_resize(source,embedded,slab_edits(),std::nullopt,"Embedded refusal"); },"mixed component geometry must refuse an embedded row even when independently replayable");
    auto baseline=fixture(false,true,true);const auto original=baseline.snapshot();
    auto allocation=slab_allocation(original);allocation->identities.emplace("unit-slab","unexpected-unit-copy");
    invalid([&] { (void)prepare_component_slab_directional_resize(original,component_edit(original),slab_edits(),allocation,"Exact source inventory"); },"baseline allocation must refuse extra nonreplacement source owners");
    allocation=slab_allocation(original);allocation->dimension_identities["slab-dim"]="proposed-component-dim-0";
    invalid([&] { (void)prepare_component_slab_directional_resize(original,component_edit(original),slab_edits(),allocation,"Disjoint fresh destinations"); },"source-independent lanes must refuse overlapping fresh dimension identities");
}
void stale_and_retained_history_destinations() {
    auto document=fixture(false,true,true);const auto source=document.snapshot();
    const auto prepared=prepare_component_slab_directional_resize(source,component_edit(source),slab_edits(),slab_allocation(source),"Captured immutable source");
    auto changed=source.entities().at("property");changed.properties["name"]="Updated actual source";
    document.apply(ApplyEntityChanges{.expected_revision=document.revision(),
        .entity_changes={EntityChange::upsert(changed)},.message="Source changed"});
    invalid([&] { require_mixed_selection_property_edit_source_current(document.snapshot(),prepared); },"changed complete source must refuse stale preparation");
    document.undo(document.revision());
    const auto restored=document.snapshot();
    Entity historical{"history-only-dimension","label",{{"text","Retained undone identity"}},false};
    document.apply(ApplyEntityChanges{.expected_revision=document.revision(),
        .entity_changes={EntityChange::upsert(historical)},.message="Reserve actual undone identity"});
    document.undo(document.revision());
    const auto with_history=document.snapshot();
    auto allocation=slab_allocation(with_history);allocation->dimension_identities["slab-dim"]="history-only-dimension";
    invalid([&] { (void)prepare_component_slab_directional_resize(with_history,component_edit(with_history),slab_edits(),allocation,"History lifetime refusal"); },
        "slab copies must reject an identity retained only in undone history");
    require(restored.entities()==with_history.entities(),"history-only reservation fixture must preserve exact current source geometry");
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
    check_markers(262);
    require(sqlite3_exec(database.get(),
        "BEGIN; PRAGMA user_version=261; UPDATE metadata SET value='261' WHERE key='format_version'; COMMIT",
        nullptr,nullptr,nullptr)==SQLITE_OK,"downgrade only the controlled mixed reader marker pair");
    check_markers(261);
    database.reset();
    const auto digest=ProjectStore::file_sha256(file);
    bool refused=false;
    try { (void)ProjectStore::load(file); }
    catch (const StorageError& error) {
        require(error.code()==StorageErrorCode::unsupported_format &&
            std::string(error.what()).find("requires reader 262")!=std::string::npos,
            "valid previous-reader markers must reject actual mixed semantics at the reader262 gate");
        refused=true;
    }
    require(refused,"reader261 markers must refuse retained canonical mixed39 command52 authority");
    require(ProjectStore::file_sha256(file)==digest,"refused mixed reader downgrade must not rewrite any file bytes");
}
void command_history_replay() {
    for (bool baseline:{false,true}) {
        auto document=fixture(true,baseline,baseline);const auto source=document.snapshot();
        require(ProjectStore::required_format_version(source)<262 && ProjectStore::required_semantic_format_version(source)<262,
            "mixed fixture must not obtain reader262 from preexisting entity state");
        const auto prepared=prepare_component_slab_directional_resize(source,component_edit(source),slab_edits(),slab_allocation(source),"Atomic mixed directional resize");
        ApplyBoundaryConstraintChanges command;
        command.expected_revision=source.revision();command.message="Atomic mixed directional resize";
        command.mixed_selection_edit_completion=true;
        command.mixed_selection_edit_intent=complete_mixed_selection_property_edit(source,prepared,{},{});
        const auto wire=command_to_json(command);
        require(wire.at("kind")=="apply_boundary_constraint_changes" && wire.at("version")==52 &&
            wire.at("mixed_selection_edit_intent").at("version")==39,
            "reader262 fixture must carry exact canonical command52 mixed39 authority");
        document.apply(command);
        require(document.snapshot().entities()==prepared.preview_snapshot().entities(),"existing command 52 must publish both lanes atomically after root integration");
        const auto final=document.snapshot();
        require(ProjectStore::required_semantic_format_version(final)==262,"retained mixed resize command must require the reserved semantic floor");
        document.undo(document.revision());require(document.snapshot().entities()==source.entities(),"one undo must restore both source families and all dimensions");
        const auto undone=document.snapshot();
        document.redo(document.revision());require(document.snapshot().entities()==final.entities(),"one redo must reconstruct the exact complete final graph");
        for (const auto& snapshot:{final,undone,document.snapshot()}) {
            require(ProjectStore::required_format_version(snapshot)==262 && ProjectStore::required_semantic_format_version(snapshot)==262,
                "applied, undone and redone mixed command history must retain exact native and semantic reader262");
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
            require(ProjectStore::required_format_version(restored)==262 && ProjectStore::required_semantic_format_version(restored)==262,
                "reopened applied, undone and redone mixed histories must retain reader262");
            const auto extracted=file.directory/"extracted";
            extract_project(snapshot,extracted);
            std::ifstream input(extracted/"project.json");
            const auto manifest=Json::parse(input);
            require(manifest.at("exchange_version")==260 && manifest.at("revisions").size()==snapshot.history().size(),
                "applied, undone and redone mixed histories must extract with exact exchange260");
            require(manifest.at("revisions").at(final.revision()).at("boundary_constraint_changes")==wire,
                "mixed extraction must retain canonical command52 mixed39 rather than infer authority from entity data");
            downgrade_mixed_reader_markers(file.path);
        }
    }
}
void opaque_mixed_shapes_do_not_raise_reader_floor() {
    auto document=fixture();const auto source=document.snapshot();
    const auto floor=ProjectStore::required_format_version(source);
    const auto semantic_floor=ProjectStore::required_semantic_format_version(source);
    require(floor<262 && semantic_floor<262,"opaque metadata fixture must start below the mixed reader floor");
    const auto prepared=prepare_component_slab_directional_resize(source,component_edit(source),slab_edits(),std::nullopt,"Opaque command-shaped user metadata");
    ApplyBoundaryConstraintChanges command;
    command.expected_revision=source.revision();command.message="Opaque command-shaped user metadata";
    command.mixed_selection_edit_completion=true;
    command.mixed_selection_edit_intent=complete_mixed_selection_property_edit(source,prepared,{},{});
    const auto wire=command_to_json(command);
    require(wire.at("version")==52 && command.mixed_selection_edit_intent.at("version")==39,
        "opaque collision must contain genuine canonical command52 and closed mixed39 data");
    auto property=source.entities().at("property");
    property.properties["user_metadata"]={{"version",39},{"intent",command.mixed_selection_edit_intent},{"command",wire}};
    property.extensions["vendor"]=wire;
    Entity opaque{"opaque-mixed-data","vendor",command.mixed_selection_edit_intent,false};
    opaque.extensions={{"version",39},{"boundary_constraint_changes",wire}};
    document.apply(ApplyEntityChanges{.expected_revision=document.revision(),
        .entity_changes={EntityChange::upsert(property),EntityChange::upsert(opaque)},.message="Retain opaque mixed-shaped metadata"});
    const auto head=document.snapshot();
    document.undo(document.revision());
    for (const auto& snapshot:{head,document.snapshot()}) {
        require(ProjectStore::required_format_version(snapshot)==floor &&
            ProjectStore::required_semantic_format_version(snapshot)==semantic_floor,
            "current and undone opaque user/entity command52 mixed39 metadata must retain their original reader floors");
        for (const auto& revision:snapshot.history())
            require(!revision.boundary_constraint_changes,"opaque collision fixture must grant no canonical command authority");
        const ScopedProjectFile file;
        (void)ProjectStore::save(file.path,snapshot);
        const auto restored=ProjectStore::load(file.path).document.snapshot();
        require(restored.entities()==snapshot.entities() && restored.history().at(head.revision()).entities==head.entities(),
            "native reopen must preserve current and retained opaque mixed-shaped metadata exactly");
        require(ProjectStore::required_format_version(restored)==floor &&
            ProjectStore::required_semantic_format_version(restored)==semantic_floor,
            "persisted opaque command-shaped metadata must not acquire mixed reader262 authority");
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
            {"ordinary_proposed_and_baseline_pairs",ordinary_proposed_and_baseline_pairs},
            {"component_request_eight_and_nine",component_request_eight_and_nine},
            {"dimensionless_component_with_hosted_slab_dimensions",dimensionless_component_with_hosted_slab_dimensions},
            {"proposed_membership_and_already_mapped_sources",proposed_membership_and_already_mapped_sources},
            {"reviewed_retirements_and_shared_association_bound",reviewed_retirements_and_shared_association_bound},
            {"closed_source_and_allocation_refusals",closed_source_and_allocation_refusals},
            {"stale_and_retained_history_destinations",stale_and_retained_history_destinations},
            {"command_history_replay",command_history_replay},
            {"opaque_mixed_shapes_do_not_raise_reader_floor",opaque_mixed_shapes_do_not_raise_reader_floor}};
        for (const auto& test:tests) { active=test.first;test.second(); }
        std::cout<<"Mixed component/slab resize core regression tests passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr<<active<<": "<<error.what()<<'\n';return 1; }
}
