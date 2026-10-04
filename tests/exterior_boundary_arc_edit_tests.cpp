#include "sketch/constraint_authoring.hpp"
#include "sketch/boundary_dimension.hpp"
#include "sketch/constraint_entity.hpp"
#include "sketch/constraint_wall_edit.hpp"
#include "sketch/geometry_operations.hpp"
#include "sketch/measurement_linework.hpp"
#include "sketch/project_store.hpp"
#include "sketch/wall_measurement.hpp"
#include "support/noninteractive_errors.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace {
using namespace sketch;
std::string case_context, phase_context;
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
void require_near_point(Vec2 actual, Vec2 expected, const char* message) {
    // The analytical offset inverse and solver use a 1e-6 metre envelope.
    require(std::hypot(actual.x-expected.x,actual.y-expected.y)<=1e-6,message);
}
Entity wall(const char* id, Vec2 start, Vec2 end, double sweep=0) {
    Entity entity{id,"wall",{{"baseline",{{"start",{start.x,start.y}},
        {"end",{end.x,end.y}},{"sweep_radians",sweep}}},{"thickness_m",0.2},
        {"height_m",3},{"elevation_m",0},{"property_id","property"},
        {"building_id","building"},{"floor_id","floor"},{"layer_id","layer"}}};
    if (sweep!=0) {
        const auto angle=angle_from_radians(sweep);
        entity.extensions["curve_input"]={{"version",2},{"construction","angle"},
            {"measure",angle.original_expression},{"measure_value",sweep},{"radians",sweep},
            {"clockwise",sweep<0},{"start",{start.x,start.y}},{"end",{end.x,end.y}},
            {"vendor","retain"}};
    }
    return entity;
}
Document fixture(double initial_sweep=0, bool reverse_consumer=false, Vec2 origin={0,0}) {
    const auto translated=[&](Vec2 value){return Vec2{value.x+origin.x,value.y+origin.y};};
    std::vector<Entity> entities{
        {"property","property",{{"calculation_workflow","measurement"}}},
        {"building","building",{{"property_id","property"}}},
        {"floor","floor",{{"building_id","building"}}},
        {"layer","layer",{{"floor_id","floor"}}},
        wall("bottom",translated({0,0}),translated({4,0}),initial_sweep),
        wall("right",translated({4,0}),translated({4,3})),
        wall("top",translated({4,3}),translated({0,3})),
        wall("left",translated({0,3}),translated({0,0})),
        {"opening","opening",{{"wall_id","bottom"},{"offset_m",0.4},
            {"width_m",0.5},{"sill_m",0.4},{"height_m",1}}}};
    for (auto& entity:entities) {
        if (entity.id=="right") entity.properties["thickness_m"]=0.3;
        if (entity.id=="left") entity.properties["thickness_m"]=0.24;
    }
    const auto initial=Document::create(entities);
    const auto measured=derive_exterior_wall_measurement(initial.snapshot(),{"bottom","right","top","left"});
    IdentifiedBoundary outline{"area","measurement_boundary",{}};
    std::string selected;
    for (std::size_t i=0;i<measured.boundary.size();++i) {
        const auto id="edge-"+std::to_string(i);
        outline.segments.push_back({id,"vertex-"+std::to_string(i),
            "vertex-"+std::to_string((i+1)%measured.boundary.size()),measured.boundary[i]});
        if (measured.ordered_wall_ids.at(i)=="bottom") selected=id;
    }
    require(!selected.empty(),"fixture omitted selected physical source identity");
    auto area=encode_identified_boundary_entity(outline);
    area.properties["property_id"]="property";area.properties["building_id"]="building";
    area.properties["floor_id"]="floor";area.properties["layer_id"]="layer";
    area.properties["wall_measurement_source"]=measured.source;area.extensions["vendor"]="retain";
    entities.push_back(area);
    auto consumer=area;consumer.id="consumer";
    if (reverse_consumer) consumer=reverse_identified_boundary_entity(consumer);
    entities.push_back(consumer);
    BoundaryDimension length{"length-dimension","area",selected,translated({2,-0.5})};
    length.placement=BoundaryDimensionPlacement::automatic;length.automatic_placement_version=2;
    entities.push_back(encode_boundary_dimension_entity(length));
    BoundaryDimension area_dimension{"area-dimension","area",{},translated({2,1.5})};
    area_dimension.kind=BoundaryDimensionKind::area;
    entities.push_back(encode_boundary_dimension_entity(area_dimension));
    return Document::create(entities);
}
IdentifiedSegment selected_edge(const DocumentSnapshot& snapshot) {
    const auto owner=decode_identified_boundary_entity(snapshot.entities().at("area"));
    const auto dimension=decode_boundary_dimension_entity(snapshot.entities().at("length-dimension"));
    const auto found=std::find_if(owner.segments.begin(),owner.segments.end(),[&](const auto& edge){
        return edge.segment_id==dimension.dimension->segment_id;});
    require(found!=owner.segments.end(),"fixture dimension lost its physical selected edge");
    return *found;
}
ConstructionReceipt receipt_for(const IdentifiedSegment& edge, BoundaryConstructionKind kind, bool clockwise) {
    ConstructionReceipt receipt;
    receipt.segment_id=edge.segment_id;receipt.kind=kind;
    receipt.start=edge.segment.start;receipt.chord_end=edge.segment.end;
    if (kind==BoundaryConstructionKind::arc_chord_angle)
        receipt.angle=parse_angle(clockwise ? "-30.0 deg" : "30.0 deg");
    else if (kind==BoundaryConstructionKind::arc_chord_height)
        receipt.height=parse_quantity(clockwise ? "-6 in" : "6 in",Unit::inch);
    else {
        receipt.arc_length=parse_quantity("15 1/2 ft",Unit::foot);receipt.clockwise=clockwise;
    }
    return receipt;
}
void compare_outline(const IdentifiedBoundary& actual, const IdentifiedBoundary& expected) {
    require(actual.id==expected.id && actual.type==expected.type && actual.segments.size()==expected.segments.size(),
        "source reconstruction changed measured owner identity or edge count");
    for (std::size_t i=0;i<actual.segments.size();++i) {
        const auto& a=actual.segments[i];const auto& e=expected.segments[i];
        require(a.segment_id==e.segment_id && a.start_vertex_id==e.start_vertex_id && a.end_vertex_id==e.end_vertex_id,
            "source reconstruction replaced stable measured child identities");
        require_near_point(a.segment.start,e.segment.start,"source reconstruction moved a requested measured start");
        require_near_point(a.segment.end,e.segment.end,"source reconstruction moved a requested measured end");
        require(std::abs(a.segment.sweep_radians-e.segment.sweep_radians)<=1e-8,
            "source reconstruction differs from the requested entire measured outline");
    }
}
void reconstruction_is_atomic_current_and_receipt_preserving() {
    for (const double initial_sweep:{0.0,0.6})
    for (const auto kind:{BoundaryConstructionKind::arc_chord_angle,
        BoundaryConstructionKind::arc_chord_height,BoundaryConstructionKind::arc_chord_length})
    for (const bool clockwise:{false,true}) {
        case_context=std::to_string(initial_sweep)+", kind="+std::to_string(static_cast<int>(kind))+", clockwise="+(clockwise ? "true" : "false");
        phase_context="fixture";
        auto document=fixture(initial_sweep,clockwise,clockwise ? Vec2{120,-70} : Vec2{0,0});
        const auto before=document.snapshot();const auto selected=selected_edge(before);
        const auto original=decode_identified_boundary_entity(before.entities().at("area"));
        const auto receipt=receipt_for(selected,kind,clockwise);
        const auto expected=reconstruct_boundary_arc(original,selected.segment_id,receipt);
        ConstraintAuthoringIntent intent;
        intent.exterior_segment_arc=ExteriorSegmentArcIntent{"area",selected.segment_id,receipt,clockwise};
        phase_context="preview";
        const auto preview=preview_constraint_authoring(before,intent);
        if (!preview.accepted()) for (const auto& diagnostic:preview.diagnostics()) std::cerr<<diagnostic<<'\n';
        require(preview.accepted(),"typed exterior arc reconstruction was rejected");
        require(document.snapshot().entities()==before.entities(),"arc preview mutated authoritative document geometry");
        const auto& values=preview.candidate_entities();
        compare_outline(decode_identified_boundary_entity(values.at("area")),expected);
        auto expected_consumer=expected;expected_consumer.id="consumer";
        if (clockwise) expected_consumer=reverse_identified_boundary(expected_consumer);
        compare_outline(decode_identified_boundary_entity(values.at("consumer")),expected_consumer);
        require(wall_measurement_source_current(values,values.at("area")) && wall_measurement_source_current(values,values.at("consumer")),
            "arc reconstruction left a current measured consumer stale");
        require(!preview.changed_walls().empty() && values.at("bottom")!=before.entities().at("bottom"),
            "arc reconstruction did not reconstruct its physical perimeter source with either related-object choice");
        require(values.at("opening")==before.entities().at("opening"),"arc reconstruction changed hosted opening inputs");
        for (const auto* id:{"bottom","right","top","left"}) {
            const auto& physical=values.at(id);
            require(physical.id==id && physical.properties.at("thickness_m")==before.entities().at(id).properties.at("thickness_m"),
                "arc reconstruction changed physical source identity or unequal thickness");
            validate_wall_curve_input(physical);
        }
        const auto length=decode_boundary_dimension_entity(values.at("length-dimension"));
        require(length.supported() && length.dimension->segment_id==selected.segment_id &&
            std::abs(length.dimension->resolve(values.at("area")).segment_length()-
                segment_length(replay_construction_receipt(receipt,{receipt.start}).segment))<=1e-6,
            "selected edge dimension does not derive from the final source curve");
        const auto area_dimension=decode_boundary_dimension_entity(values.at("area-dimension"));
        require(area_dimension.supported() && std::abs(area_dimension.dimension->resolve(values.at("area")).area()-
            std::abs(signed_area(boundary_geometry(expected))))<=1e-6,"area dimension does not derive from final source outline");
        phase_context="typed proof";
        const auto candidate=preview_constraint_authoring_snapshot(before,preview);
        require(candidate.entities()==values,"typed arc replay differs from the displayed candidate");
        const auto& proof=candidate.history().back().boundary_constraint_changes;
        require(proof && proof->exterior_segment_arc && proof->exterior_segment_arc->arc_construction==receipt &&
            proof->exterior_segment_arc->move_connected_objects==clockwise,"arc proof lost exact measured receipt or movement choice");
        const auto encoded=command_to_json(Command{*proof});
        require(encoded.at("version")==14 && command_to_json(command_from_json(encoded))==encoded,
            "arc proof did not round trip its strict dedicated command dialect");
        const auto& archive=values.at("bottom").extensions.at("curve_input_derivation");
        if (initial_sweep==0) require(archive.at("version")==3 && archive.at("source_input").is_null() &&
            archive.at("source_baseline")==before.entities().at("bottom").properties.at("baseline"),
            "straight-to-arc source invented an original curve receipt");
        else require(archive.at("source_input")==before.entities().at("bottom").extensions.at("curve_input"),
            "arc-to-arc reconstruction lost original physical construction input");
        phase_context="apply and history";
        (void)apply_constraint_authoring(document,preview);
        require(document.snapshot().entities()==candidate.entities() && document.snapshot().history().size()==before.history().size()+1,
            "arc Apply did not commit exactly one complete source transaction");
        require(Document::fork(document.snapshot()).snapshot().entities()==candidate.entities(),"arc retained history failed independent replay");
        document.undo(document.revision());require(document.snapshot().entities()==before.entities(),"arc Undo lost exact source and consumer state");
        document.redo(document.revision());require(document.snapshot().entities()==candidate.entities(),"arc Redo lost shown source state");
        if (kind==BoundaryConstructionKind::arc_chord_length && clockwise) {
            struct OwnedFile {
                std::filesystem::path path=std::filesystem::temp_directory_path()/("exterior-source-arc-"+make_stable_id()+".bldproj");
                ~OwnedFile(){std::error_code error;std::filesystem::remove(path,error);}
            } file;
            phase_context="save and reopen";
            (void)ProjectStore::save(file.path,document.snapshot());
            const auto reopened=ProjectStore::load(file.path).document.snapshot();
            const auto& retained=reopened.history().at(static_cast<std::size_t>(candidate.revision())).boundary_constraint_changes;
            require(reopened.entities()==candidate.entities() && retained && command_to_json(Command{*retained})==encoded,
                "native reopen lost exact measured arc proof or source geometry");
        }
    }
}
Document with_additions(const DocumentSnapshot& source, const std::vector<Entity>& additions) {
    std::vector<Entity> entities;
    for (const auto& [id,entity]:source.entities()) {(void)id;entities.push_back(entity);}
    entities.insert(entities.end(),additions.begin(),additions.end());return Document::create(entities);
}
void frozen_contacts_constraints_and_invalid_intents_are_atomic() {
    case_context="contacts and refusals";phase_context="fixture";
    auto document=fixture();const auto before=document.snapshot();const auto edge=selected_edge(before);
    ConstraintAuthoringIntent intent;
    intent.exterior_segment_arc=ExteriorSegmentArcIntent{"area",edge.segment_id,receipt_for(edge,BoundaryConstructionKind::arc_chord_height,false),false};
    auto attached=with_additions(before,{wall("attachment",{4,0},{3,1})});const auto attached_before=attached.snapshot();
    phase_context="frozen physical contact";
    auto preview=preview_constraint_authoring(attached_before,intent);
    require(!preview.accepted() && preview.candidate_entities()==attached_before.entities() && attached.snapshot().entities()==attached_before.entities(),
        "frozen physical endpoint contact detached or published partial arc geometry");
    intent.exterior_segment_arc->move_connected_objects=true;phase_context="movable physical contact";
    preview=preview_constraint_authoring(attached_before,intent);
    if (!preview.accepted()) for (const auto& diagnostic:preview.diagnostics()) std::cerr<<diagnostic<<'\n';
    require(preview.accepted() && preview.candidate_entities().at("attachment")!=attached_before.entities().at("attachment"),
        "related-object arc reconstruction did not carry physical endpoint attachment");
    const auto end=preview.candidate_entities().at("bottom").properties.at("baseline").at("end");
    const auto start=preview.candidate_entities().at("attachment").properties.at("baseline").at("start");
    require_near_point({start.at(0).get<double>(),start.at(1).get<double>()},{end.at(0).get<double>(),end.at(1).get<double>()},
        "movable physical endpoint lost its existing contact");
    (void)preview_constraint_authoring_snapshot(attached_before,preview);
    PersistentConstraint pin;pin.id="source-end-pin";pin.relation=ConstraintRelationKind::fixed_anchor;
    pin.bindings={{"bottom",WallEndpointRole::end}};pin.anchor=Vec2{4,0};
    auto pinned=with_additions(before,{encode_constraint_entity(pin)});const auto pinned_before=pinned.snapshot();
    phase_context="persistent constraint conflict";
    preview=preview_constraint_authoring(pinned_before,intent);
    require(!preview.accepted() && preview.candidate_entities()==pinned_before.entities(),"conflicting persistent source anchor escaped arc solver pins");
    phase_context="malformed and competing authority";
    auto malformed=intent;malformed.exterior_segment_arc->arc_construction.segment_id="missing-edge";
    require(!preview_constraint_authoring(before,malformed).accepted(),"mismatched arc receipt segment was admitted");
    malformed=intent;malformed.exterior_segment_arc->arc_construction.start.x+=0.01;
    require(!preview_constraint_authoring(before,malformed).accepted(),"arc receipt changed fixed measured chord start");
    malformed=intent;malformed.wall_resize=WallResizeIntent{"top",parse_quantity("4 m")};
    require(!preview_constraint_authoring(before,malformed).accepted(),"competing source arc and physical coordinate intents were admitted");
    require(document.snapshot().entities()==before.entities(),"arc refusals mutated authoritative source document");
}
void interior_physical_contact_follows_curve_station() {
    case_context="interior partition";phase_context="fixture";
    auto base=fixture();
    auto document=with_additions(base.snapshot(),{wall("t-partition",{2,0},{2,2})});
    const auto before=document.snapshot();const auto edge=selected_edge(before);
    ConstraintAuthoringIntent intent;
    intent.exterior_segment_arc=ExteriorSegmentArcIntent{"area",edge.segment_id,
        receipt_for(edge,BoundaryConstructionKind::arc_chord_height,false),false};
    require(!preview_constraint_authoring(before,intent).accepted(),"Frozen interior partition must refuse changed wall curvature");
    intent.exterior_segment_arc->move_connected_objects=true;
    const auto preview=preview_constraint_authoring(before,intent);
    if(!preview.accepted())for(const auto& diagnostic:preview.diagnostics())std::cerr<<diagnostic<<'\n';
    require(preview.accepted(),"Interior partition must follow its physical curve station");
    const auto& baseline=preview.candidate_entities().at("bottom").properties.at("baseline");
    const Vec2 a{baseline.at("start")[0].get<double>(),baseline.at("start")[1].get<double>()};
    const Vec2 b{baseline.at("end")[0].get<double>(),baseline.at("end")[1].get<double>()};
    const auto sweep=baseline.at("sweep_radians").get<double>();
    const auto k=0.5/std::tan(sweep/2);
    const Vec2 center{(a.x+b.x)/2-(b.y-a.y)*k,(a.y+b.y)/2+(b.x-a.x)*k};
    const auto x=a.x-center.x,y=a.y-center.y;
    const Vec2 midpoint{center.x+x*std::cos(sweep/2)-y*std::sin(sweep/2),
        center.y+x*std::sin(sweep/2)+y*std::cos(sweep/2)};
    const auto& partition=preview.candidate_entities().at("t-partition").properties.at("baseline");
    require_near_point({partition.at("start")[0].get<double>(),partition.at("start")[1].get<double>()},midpoint,
        "Interior partition lost its normalized station on the reconstructed arc");
    require(partition.at("end")==before.entities().at("t-partition").properties.at("baseline").at("end"),
        "Interior partition's unattached endpoint must remain fixed");
    (void)apply_constraint_authoring(document,preview);
    require(wall_measurement_source_current(document.snapshot(),document.snapshot().entities().at("area")),
        "Interior contact edit must retain a current measured exterior");
    document.undo(document.revision());require(document.snapshot().entities()==before.entities(),"Interior contact Undo must restore all exact geometry");
}
void dependent_measured_stroke_composes_with_arc_authority() {
    case_context="dependent measured stroke";phase_context="fixture";
    auto document=fixture();const auto base=document.snapshot();const auto edge=selected_edge(base);
    MeasurementLinework model;model.stroke_id="dependent-stroke";model.anchor={4,0};
    ConstructionReceipt line;line.segment_id="stroke-edge";line.kind=BoundaryConstructionKind::line_to_point;
    line.start=model.anchor;line.chord_end=Vec2{3,1};
    model.edges.push_back({line.segment_id,"stroke-start","stroke-end",line});model.extensions["vendor"]="retain";
    PersistentConstraint join;join.id="measured-join";join.relation=ConstraintRelationKind::coincident;
    join.bindings={{"bottom",WallEndpointRole::end},{model.stroke_id,WallEndpointRole::start,line.segment_id,"stroke-start"}};
    document=with_additions(base,{{model.stroke_id,"measurement_linework",{{"model",encode_measurement_linework_model(model)},
        {"property_id","property"},{"building_id","building"},{"floor_id","floor"},{"layer_id","layer"}},true},encode_constraint_entity(join)});
    const auto before=document.snapshot();ConstraintAuthoringIntent intent;
    intent.exterior_segment_arc=ExteriorSegmentArcIntent{"area",edge.segment_id,receipt_for(edge,BoundaryConstructionKind::arc_chord_angle,false),true};
    phase_context="preview and composed command";
    const auto preview=preview_constraint_authoring(before,intent);
    if (!preview.accepted()) for (const auto& diagnostic:preview.diagnostics()) std::cerr<<diagnostic<<'\n';
    require(preview.accepted() && !preview.changed_measured_strokes().empty(),"source arc did not solve explicit measured-stroke dependent");
    const auto candidate=preview_constraint_authoring_snapshot(before,preview);const auto& proof=*candidate.history().back().boundary_constraint_changes;
    require(proof.exterior_segment_arc && proof.measured_source_completion && !proof.measured_stroke_edits.empty(),
        "arc command omitted dependent measured-source completion authority");
    const auto decoded=decode_measurement_linework_model(candidate.entities().at(model.stroke_id).properties.at("model"));
    require(decoded.supported() && decoded.model->edges==model.edges && decoded.model->extensions==model.extensions,
        "arc dependent movement rewrote original measured-stroke receipts");
    const auto replay=replay_measurement_linework(*decoded.model);const auto endpoint=candidate.entities().at("bottom").properties.at("baseline").at("end");
    require_near_point(replay.edges.front().segment.start,{endpoint.at(0).get<double>(),endpoint.at(1).get<double>()},
        "explicit measured dependent did not follow final physical source endpoint");
    auto tampered=command_to_json(Command{proof});tampered["exterior_segment_arc"]["move_connected_objects"]=false;
    bool refused=false;try{(void)Document::preview_command(before,command_from_json(tampered));}catch(const std::exception&){refused=true;}
    require(refused,"saved arc authority bypassed frozen dependent measured geometry");
    intent.exterior_segment_arc->move_connected_objects=false;const auto frozen=preview_constraint_authoring(before,intent);
    require(!frozen.accepted() && frozen.candidate_entities()==before.entities(),"frozen explicit measured dependent moved under source arc authority");
    (void)apply_constraint_authoring(document,preview);
    document.undo(document.revision());require(document.snapshot().entities()==before.entities(),"composed arc Undo lost original measured-stroke receipts");
    document.redo(document.revision());require(document.snapshot().entities()==candidate.entities(),"composed arc Redo lost dependent measured completion");
}
}
int main() {
    sketch::testing::noninteractive_errors();
    try {
        reconstruction_is_atomic_current_and_receipt_preserving();
        frozen_contacts_constraints_and_invalid_intents_are_atomic();
        interior_physical_contact_follows_curve_station();
        dependent_measured_stroke_composes_with_arc_authority();
    } catch (const std::exception& error) {
        std::cerr<<"exterior_boundary_arc_edit_tests ["<<case_context<<", "<<phase_context<<"]: "<<error.what()<<'\n';return 1;
    }
    std::cout<<"Source-derived curvature editing passed\n";
}
