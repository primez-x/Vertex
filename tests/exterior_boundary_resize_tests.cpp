#include "sketch/constraint_authoring.hpp"
#include "sketch/boundary_dimension.hpp"
#include "sketch/wall_measurement.hpp"
#include "sketch/project_store.hpp"
#include "sketch/constraint_entity.hpp"
#include "sketch/constraint_wall_edit.hpp"
#include "sketch/measurement_linework.hpp"
#include "support/noninteractive_errors.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <filesystem>
#include <stdexcept>

namespace {
using namespace sketch;
std::string case_context;
std::string phase_context;

void require(bool condition,const char* message) {
    if(!condition)throw std::runtime_error(message);
}

Entity wall(const char* id,Vec2 start,Vec2 end,double sweep=0) {
    Entity entity{id,"wall",{{"baseline",{{"start",{start.x,start.y}},{"end",{end.x,end.y}},{"sweep_radians",sweep}}},
        {"thickness_m",0.2},{"height_m",3},{"elevation_m",0},{"property_id","property"},
        {"building_id","building"},{"floor_id","floor"},{"layer_id","layer"}}};
    if(sweep!=0) {
        const auto angle=angle_from_radians(sweep);
        entity.extensions["curve_input"]={{"version",2},{"construction","angle"},
            {"measure",angle.original_expression},{"measure_value",sweep},{"radians",sweep},
            {"clockwise",sweep<0},{"start",{start.x,start.y}},{"end",{end.x,end.y}},{"vendor","retain"}};
    }
    return entity;
}

Document fixture(bool reverse_consumer=false) {
    std::vector<Entity> entities{
        {"property","property",{{"calculation_workflow","measurement"}}},
        {"building","building",{{"property_id","property"}}},
        {"floor","floor",{{"building_id","building"}}},
        {"layer","layer",{{"floor_id","floor"}}},
        wall("bottom",{0,0},{4,0},0.6),wall("right",{4,0},{4,3}),
        wall("top",{4,3},{0,3}),wall("left",{0,3},{0,0}),
        {"opening","opening",{{"wall_id","bottom"},{"offset_m",0.4},{"width_m",0.5},{"sill_m",0.4},{"height_m",1}}}};
    for(auto& entity:entities) {
        if(entity.id=="right")entity.properties["thickness_m"]=0.3;
        if(entity.id=="left")entity.properties["thickness_m"]=0.24;
    }
    const auto initial=Document::create(entities);
    const auto measured=derive_exterior_wall_measurement(initial.snapshot(),{"bottom","right","top","left"});
    IdentifiedBoundary outline{"area","measurement_boundary",{}};
    for(std::size_t i=0;i<measured.boundary.size();++i)
        outline.segments.push_back({"edge-"+std::to_string(i),"vertex-"+std::to_string(i),
            "vertex-"+std::to_string((i+1)%measured.boundary.size()),measured.boundary[i]});
    auto owner=encode_identified_boundary_entity(outline);
    owner.properties["property_id"]="property";owner.properties["building_id"]="building";
    owner.properties["floor_id"]="floor";owner.properties["layer_id"]="layer";
    owner.properties["wall_measurement_source"]=measured.source;
    owner.extensions["vendor"]="retain";
    entities.push_back(owner);
    auto consumer=owner;consumer.id="consumer";
    if(reverse_consumer) {
        auto reversed=decode_identified_boundary_entity(consumer);
        std::reverse(reversed.segments.begin(),reversed.segments.end());
        for(auto& edge:reversed.segments) {
            std::swap(edge.start_vertex_id,edge.end_vertex_id);std::swap(edge.segment.start,edge.segment.end);
            edge.segment.sweep_radians=-edge.segment.sweep_radians;
        }
        consumer=encode_identified_boundary_entity(reversed,&consumer);
    }
    entities.push_back(consumer);
    const auto curved=std::find_if(outline.segments.begin(),outline.segments.end(),
        [](const auto& edge){return edge.segment.sweep_radians!=0;});
    require(curved!=outline.segments.end(),"physical fixture omitted its curved measured exterior edge");
    BoundaryDimension dimension{"length-dimension","area",curved->segment_id,{2,-0.5}};
    dimension.placement=BoundaryDimensionPlacement::automatic;dimension.automatic_placement_version=2;
    entities.push_back(encode_boundary_dimension_entity(dimension));
    return Document::create(entities);
}

void selected_curved_source_edge_resizes_physical_sources_and_every_consumer() {
    for(const auto anchor:{BoundaryFixedEndpoint::start,BoundaryFixedEndpoint::end})
    for(const bool local_chain:{false,true}) {
    case_context=std::string(anchor==BoundaryFixedEndpoint::start ? "start" : "end")+
        ", local_chain="+(local_chain ? "true" : "false");
    phase_context="fixture";
    auto document=fixture(anchor==BoundaryFixedEndpoint::end);
    const bool measured_dependent=anchor==BoundaryFixedEndpoint::start && !local_chain;
    if(measured_dependent) {
        phase_context="dependent fixture";
        const auto base=document.snapshot();
        const auto boundary=decode_identified_boundary_entity(base.entities().at("area"));
        const auto curve=std::find_if(boundary.segments.begin(),boundary.segments.end(),
            [](const auto& edge){return edge.segment.sweep_radians!=0;});
        MeasurementLinework model;model.stroke_id="dependent-stroke";model.anchor=curve->segment.end;
        ConstructionReceipt receipt;receipt.segment_id="stroke-edge";receipt.kind=BoundaryConstructionKind::line_to_point;
        receipt.start=model.anchor;receipt.chord_end=Vec2{model.anchor.x,model.anchor.y+1};
        model.edges.push_back({receipt.segment_id,"stroke-start","stroke-end",receipt});
        model.extensions["vendor"]="retain";
        PersistentConstraint join;join.id="measured-join";join.relation=ConstraintRelationKind::coincident;
        join.bindings={{"area",WallEndpointRole::end,curve->segment_id,curve->end_vertex_id},
            {model.stroke_id,WallEndpointRole::start,receipt.segment_id,"stroke-start"}};
        std::vector<Entity> entities;
        for(const auto& [id,entity]:base.entities()){(void)id;entities.push_back(entity);}
        entities.push_back({model.stroke_id,"measurement_linework",{{"model",encode_measurement_linework_model(model)},
            {"property_id","property"},{"building_id","building"},{"floor_id","floor"},{"layer_id","layer"}},true});
        entities.push_back(encode_constraint_entity(join));document=Document::create(entities);
    }
    const auto before=document.snapshot();
    const auto original=decode_identified_boundary_entity(before.entities().at("area"));
    const auto selected=std::find_if(original.segments.begin(),original.segments.end(),
        [](const auto& edge){return edge.segment.sweep_radians!=0;});
    require(wall_measurement_source_current(before,before.entities().at("area")) &&
        wall_measurement_source_current(before,before.entities().at("consumer")),"source fixture must begin current");
    const auto exact=parse_quantity("16 1/2 ft");
    BoundaryGeometryEdit edit;
    edit.boundary_id="area";edit.kind=BoundaryGeometryEditKind::resize_segment;edit.target_id=selected->segment_id;
    edit.target_length_metres=exact.metres;edit.fixed_endpoint=anchor;edit.move_connected=local_chain;
    // With no external attachments, perimeter reconstruction remains mandatory
    // even when related objects are frozen. Keep that flag independent of the
    // selected outline's local-chain choice.
    const bool move_related=!local_chain;
    ConstraintAuthoringIntent intent;intent.boundary_resize=BoundaryResizeIntent{edit,move_related,exact};
    phase_context="preview";
    const auto preview=preview_constraint_authoring(before,intent);
    if(!preview.accepted())for(const auto& diagnostic:preview.diagnostics())std::cerr<<diagnostic<<'\n';
    require(preview.accepted(),"selected source-derived curved edge physical-length resize must be accepted");
    const auto current=decode_identified_boundary_entity(preview.candidate_entities().at("area"));
    const auto resized=std::find_if(current.segments.begin(),current.segments.end(),
        [&](const auto& edge){return edge.segment_id==selected->segment_id;});
    require(resized!=current.segments.end() && std::abs(segment_length(resized->segment)-exact.metres)<=1e-6,
        "resize must satisfy the actual selected measured edge, not merely move a source wall");
    const auto original_anchor=anchor==BoundaryFixedEndpoint::start ? selected->segment.start : selected->segment.end;
    const auto resized_anchor=anchor==BoundaryFixedEndpoint::start ? resized->segment.start : resized->segment.end;
    require(std::hypot(resized_anchor.x-original_anchor.x,resized_anchor.y-original_anchor.y)<=1e-6 &&
        std::abs(resized->segment.sweep_radians-selected->segment.sweep_radians)<=1e-8,"selected measured resize must retain its anchored endpoint and signed sweep");
    require(wall_measurement_source_current(preview.candidate_entities(),preview.candidate_entities().at("area")) &&
        wall_measurement_source_current(preview.candidate_entities(),preview.candidate_entities().at("consumer")),
        "physical resize must regenerate every eligible current measured consumer");
    require(!preview.changed_walls().empty() && preview.candidate_entities().at("consumer")!=before.entities().at("consumer"),
        "source-aware resize must preview physical walls and the complete dependent consumer");
    require(preview.candidate_entities().at("opening")==before.entities().at("opening") &&
        document.snapshot().entities()==before.entities(),"preview must preserve hosted opening and authoritative source state");
    const auto dimension=decode_boundary_dimension_entity(preview.candidate_entities().at("length-dimension"));
    require(dimension.supported() && dimension.dimension->segment_id==selected->segment_id,
        "source completion must retain the attached selected-edge dimension identity");
    phase_context="candidate and proof";
    const auto candidate=preview_constraint_authoring_snapshot(before,preview);
    require(candidate.entities()==preview.candidate_entities(),"typed source resize replay differs from shown complete candidate");
    require(candidate.history().back().boundary_constraint_changes.has_value(),"candidate omitted typed source resize proof");
    const auto proof=*candidate.history().back().boundary_constraint_changes;
    require(proof.exterior_segment_resize.has_value() && proof.exterior_segment_resize->exact_length.original_expression==exact.original_expression &&
        proof.exterior_segment_resize->exact_length.exact_metres==exact.exact_metres &&
        proof.exterior_segment_resize->fixed_endpoint==anchor && proof.exterior_segment_resize->move_boundary_chain==local_chain &&
        proof.exterior_segment_resize->move_connected_objects==move_related,
        "source proof lost entered fraction/unit quantity or distinct anchor/local-chain choices");
    const auto encoded=command_to_json(Command{proof});
    require(encoded.at("version")==13 && command_to_json(command_from_json(encoded))==encoded,
        "source resize requires an exact dedicated proof dialect");
    if(measured_dependent) {
        const auto old_model=decode_measurement_linework_model(before.entities().at("dependent-stroke").properties.at("model"));
        const auto new_model=decode_measurement_linework_model(candidate.entities().at("dependent-stroke").properties.at("model"));
        require(proof.measured_source_completion && !proof.measured_stroke_edits.empty() &&
            !preview.changed_measured_strokes().empty() && new_model.supported() &&
            new_model.model->edges==old_model.model->edges && new_model.model->extensions==old_model.model->extensions,
            "dialect thirteen lost dependent measured-stroke movement or original construction receipts");
        const auto replay=replay_measurement_linework(*new_model.model);
        require(std::hypot(replay.edges.front().segment.start.x-resized->segment.end.x,
            replay.edges.front().segment.start.y-resized->segment.end.y)<=1e-6,
            "related measured-stroke endpoint did not follow the selected measured edge");
        auto frozen_tamper=encoded;
        frozen_tamper["exterior_segment_resize"]["move_connected_objects"]=false;
        bool frozen_refused=false;
        try{(void)Document::preview_command(before,command_from_json(frozen_tamper));}
        catch(const std::exception&){frozen_refused=true;}
        require(frozen_refused && document.snapshot().entities()==before.entities(),
            "saved source resize bypassed a frozen related-object choice for its measured dependent");
    }
    auto tampered=encoded;tampered["exterior_segment_resize"]["segment_id"]="missing-edge";
    bool refused=false;try{(void)Document::preview_command(before,command_from_json(tampered));}catch(const std::exception&){refused=true;}
    require(refused,"tampered selected-edge authority was accepted against retained source redraws");
    const auto& physical=candidate.entities().at("bottom");
    require(physical.extensions.at("curve_input_derivation").at("source_input")==before.entities().at("bottom").extensions.at("curve_input"),
        "source resize lost the original physical curve construction receipt");
    validate_wall_curve_input(physical);
    for(const auto* id:{"bottom","right","top","left"})
        require(candidate.entities().at(id).properties.at("thickness_m")==before.entities().at(id).properties.at("thickness_m"),
            "source resize changed unequal physical wall thickness metadata");
    phase_context="commit";
    (void)apply_constraint_authoring(document,preview);
    require(document.snapshot().history().size()==before.history().size()+1 && document.snapshot().entities()==candidate.entities(),
        "source resize did not apply exactly one complete transaction");
    phase_context="history replay";
    require(Document::fork(document.snapshot()).snapshot().entities()==candidate.entities(),"source resize retained history failed independent replay");
    phase_context="Undo/Redo";
    document.undo(document.revision());require(document.snapshot().entities()==before.entities(),"source resize Undo lost exact source/consumer/receipt state");
    document.redo(document.revision());require(document.snapshot().entities()==candidate.entities(),"source resize Redo did not restore the shown candidate");
    if(anchor==BoundaryFixedEndpoint::end && !local_chain) {
        struct OwnedFile {
            std::filesystem::path path=std::filesystem::temp_directory_path()/("exterior-source-resize-"+make_stable_id()+".bldproj");
            ~OwnedFile(){std::error_code error;std::filesystem::remove(path,error);}
        } file;
        phase_context="save";
        (void)ProjectStore::save(file.path,document.snapshot());
        phase_context="reopen";
        auto reopened=ProjectStore::load(file.path);
        const auto reopened_snapshot=reopened.document.snapshot();
        // Undo/Redo append their own records; the typed edit proof remains at
        // the original applied revision rather than the new history head.
        const auto& retained_proof=reopened_snapshot.history().at(static_cast<std::size_t>(candidate.revision())).boundary_constraint_changes;
        require(reopened_snapshot.entities()==candidate.entities() && retained_proof.has_value() &&
            command_to_json(Command{*retained_proof})==encoded,
            "source resize native reopen changed geometry or exact typed proof");
    }
    }
}

void missing_receipts_and_frozen_conflicts_reject_atomically() {
    case_context="atomic refusals";phase_context="fixture";
    auto document=fixture();const auto before=document.snapshot();
    const auto owner=decode_identified_boundary_entity(before.entities().at("area"));
    const auto selected=std::find_if(owner.segments.begin(),owner.segments.end(),[](const auto& edge){return edge.segment.sweep_radians!=0;});
    BoundaryGeometryEdit edit;edit.boundary_id="area";edit.kind=BoundaryGeometryEditKind::resize_segment;
    edit.target_id=selected->segment_id;edit.target_length_metres=5;edit.move_connected=true;
    ConstraintAuthoringIntent intent;intent.boundary_resize=BoundaryResizeIntent{edit,true};
    phase_context="missing receipt preview";
    auto rejected=preview_constraint_authoring(before,intent);
    require(!rejected.accepted() && rejected.candidate_entities()==before.entities() &&
        std::any_of(rejected.diagnostics().begin(),rejected.diagnostics().end(),[](const auto& value){return value.find("exact entered length quantity")!=std::string::npos;}),
        "source resize synthesized an entered receipt from a double-only request");
    intent.boundary_resize->exact_length=parse_quantity("4 m");
    require(!preview_constraint_authoring(before,intent).accepted(),"mismatched exact entry and measured target was admitted");
    intent.boundary_resize->exact_length=parse_quantity("5 m");
    auto entities=before.entities();
    entities.emplace("attachment",wall("attachment",{4,0},{3,1}));
    std::vector<Entity> values;for(const auto& [id,entity]:entities){(void)id;values.push_back(entity);}
    phase_context="attached fixture";
    auto attached=Document::create(values);const auto attached_before=attached.snapshot();
    intent.boundary_resize->move_related_objects=false;
    phase_context="frozen attachment preview";
    rejected=preview_constraint_authoring(attached_before,intent);
    require(!rejected.accepted() && rejected.candidate_entities()==attached_before.entities() && attached.snapshot().entities()==attached_before.entities(),
        "frozen attached wall detached or published a partial source resize");
    intent.boundary_resize->move_related_objects=true;
    phase_context="movable attachment preview";
    const auto movable=preview_constraint_authoring(attached_before,intent);
    require(movable.accepted() && movable.candidate_entities().at("attachment")!=attached_before.entities().at("attachment"),
        "related-object source resize did not carry the attached physical endpoint");
    auto frozen_entities=before.entities();
    PersistentConstraint pin;pin.id="source-end-pin";pin.relation=ConstraintRelationKind::fixed_anchor;
    pin.bindings={{"bottom",WallEndpointRole::end}};pin.anchor=Vec2{4,0};
    frozen_entities.emplace(pin.id,encode_constraint_entity(pin));values.clear();
    for(const auto& [id,entity]:frozen_entities){(void)id;values.push_back(entity);}
    phase_context="pinned fixture";
    auto pinned=Document::create(values);const auto pinned_before=pinned.snapshot();
    phase_context="pinned source preview";
    rejected=preview_constraint_authoring(pinned_before,intent);
    require(!rejected.accepted() && rejected.candidate_entities()==pinned_before.entities() && pinned.snapshot().entities()==pinned_before.entities(),
        "conflicting persistent source anchor escaped the pinned physical solve");
}
}

int main() {
    sketch::testing::noninteractive_errors();
    try{selected_curved_source_edge_resizes_physical_sources_and_every_consumer();missing_receipts_and_frozen_conflicts_reject_atomically();return 0;}
    catch(const std::exception& error){std::cerr<<"exterior_boundary_resize_tests ["<<case_context<<", "<<phase_context<<"]: "<<error.what()<<'\n';return 1;}
}
