#include "sketch/constraint_authoring.hpp"
#include "sketch/joint_translation_replay.hpp"
#include "sketch/boundary_dimension.hpp"
#include "sketch/boundary_integrity.hpp"
#include "sketch/annotation_entity_codec.hpp"
#include "sketch/document_digest.hpp"
#include "sketch/wall_measurement.hpp"
#include "sketch/wall_split.hpp"
#include "sketch/project_store.hpp"
#include "support/noninteractive_errors.hpp"

#include <algorithm>
#include <cmath>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <iomanip>
#include <limits>
#include <numbers>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace {
using namespace sketch;
using Json=nlohmann::json;
void require(bool condition,const char* message) { if (!condition) throw std::runtime_error(message); }
void expect_close(double actual,double expected) {
    if (std::isfinite(actual) && std::abs(actual-expected)<1e-7) return;
    std::ostringstream detail; detail<<std::setprecision(17)<<"unexpected independently specified coordinate: expected "<<expected<<", got "<<actual;
    throw std::runtime_error(detail.str());
}
void accepted(const ConstraintAuthoringPreview& preview) {
    if (preview.accepted()) return;
    for (const auto& diagnostic:preview.diagnostics()) std::cerr<<diagnostic<<'\n';
    throw std::runtime_error("joint translation was rejected");
}
std::vector<Entity> drawing_fixture(std::vector<Entity> values) {
    const std::vector<Entity> context{{"p","property",Json::object(),false},
        {"b","building",{{"property_id","p"}},false},{"f","floor",{{"building_id","b"}},false},
        {"l","layer",{{"floor_id","f"}},false}};
    for (const auto& owner:context)
        if (std::none_of(values.begin(),values.end(),[&](const auto& value){return value.id==owner.id;})) values.push_back(owner);
    for (auto& value:values) {
        if (value.type!="wall" && value.type!="opening" && value.type!="measurement_linework" &&
            value.type!="dimension" && value.type!="reference_asset" && !can_recognize_boundary_entity_type(value.type)) continue;
        value.properties["property_id"]="p"; value.properties["building_id"]="b";
        value.properties["floor_id"]="f"; value.properties["layer_id"]="l";
    }
    return values;
}
Entity wall(std::string id,Vec2 start,Vec2 end,double sweep=0) {
    return {std::move(id),"wall",{{"baseline",{{"start",{start.x,start.y}},{"end",{end.x,end.y}},{"sweep_radians",sweep}}},
        {"thickness_m",.14},{"height_m",2.4},{"elevation_m",0.0}},false,Json::object()};
}
Segment baseline(const Entity& entity) {
    const auto& value=entity.properties.at("baseline");
    return {{value.at("start")[0].get<double>(),value.at("start")[1].get<double>()},
        {value.at("end")[0].get<double>(),value.at("end")[1].get<double>()},value.at("sweep_radians").get<double>()};
}
Entity area(std::string id="area",double sweep=0,Vec2 origin={}) {
    auto entity=encode_identified_boundary_entity({id,"measurement_boundary",{
        {id+":ab",id+":a",id+":b",{{0,0},{2,0},sweep}},
        {id+":bc",id+":b",id+":c",{{2,0},{2,2},0}},
        {id+":cd",id+":c",id+":d",{{2,2},{0,2},0}},
        {id+":da",id+":d",id+":a",{{0,2},{0,0},0}}}});
    auto translated=decode_identified_boundary_entity(entity);
    for (auto& edge:translated.segments) {
        edge.segment.start.x+=origin.x; edge.segment.start.y+=origin.y;
        edge.segment.end.x+=origin.x; edge.segment.end.y+=origin.y;
    }
    entity=encode_identified_boundary_entity(translated);
    BoundaryConstructionRecord construction; construction.boundary_id=id; construction.anchor=origin;
    construction.schema_version=boundary_receipt_schema_version_v2;
    const auto boundary=decode_identified_boundary_entity(entity);
    for (const auto& edge:boundary.segments) {
        ConstructionReceipt receipt; receipt.segment_id=edge.segment_id; receipt.start=edge.segment.start;
        receipt.chord_end=edge.segment.end;
        receipt.kind=edge.segment.sweep_radians==0 ? BoundaryConstructionKind::line_to_point : BoundaryConstructionKind::arc_chord_angle;
        if (edge.segment.sweep_radians!=0) receipt.angle=angle_from_radians(edge.segment.sweep_radians);
        construction.edges.push_back({edge.segment_id,edge.start_vertex_id,edge.end_vertex_id,receipt});
    }
    entity.properties["boundary_authoring"]=encode_boundary_receipt_envelope(construction);
    return entity;
}
Entity stroke(double sweep=0) {
    MeasurementLinework model; model.stroke_id="stroke"; model.anchor={2,0};
    ConstructionReceipt receipt; receipt.segment_id="stroke:e"; receipt.start={2,0}; receipt.chord_end={4,0};
    receipt.kind=sweep==0 ? BoundaryConstructionKind::line_to_point : BoundaryConstructionKind::arc_chord_angle;
    if (sweep!=0) receipt.angle=angle_from_radians(sweep);
    model.edges.push_back({"stroke:e","stroke:a","stroke:b",receipt});
    return {"stroke","measurement_linework",{{"model",encode_measurement_linework_model(model)}},true,Json::object()};
}
WallEndpointBinding endpoint(std::string id,WallEndpointRole role) { return {std::move(id),role}; }
WallEndpointBinding corner(std::string id,std::string edge,std::string vertex,WallEndpointRole role=WallEndpointRole::start) {
    return {std::move(id),role,std::move(edge),std::move(vertex)};
}
Entity relation(std::string id,ConstraintRelationKind kind,std::vector<WallEndpointBinding> bindings,
    std::optional<Vec2> anchor=std::nullopt,std::optional<Quantity> length=std::nullopt) {
    PersistentConstraint constraint; constraint.id=std::move(id); constraint.relation=kind;
    constraint.bindings=std::move(bindings); constraint.anchor=anchor; constraint.length=length;
    return encode_constraint_entity(constraint);
}
ConstraintAuthoringIntent intent(Vec2 offset={0,1}) {
    ConstraintAuthoringIntent result; result.joint_translation=JointTranslationIntent{offset,{"area"},{"stroke"},{"selected"},true};
    return result;
}
std::vector<Entity> fixture(double sweep=0) {
    auto owner=area("area",sweep); auto line=stroke(sweep);
    BoundaryDimension manual{"manual","area","area:ab",{.5,-2},BoundaryDimensionPlacement::manual};
    BoundaryDimension automatic{"automatic","stroke","stroke:e",{3,-.25},BoundaryDimensionPlacement::automatic};
    automatic.automatic_placement_version=2;
    return {owner,line,wall("selected",{4,0},{6,0},sweep),wall("tail",{6,0},{8,0}),
        {"opening","opening",{{"wall_id","selected"},{"offset_m",.5},{"width_m",.5},{"sill_m",0.0},{"height_m",2.0}},false,Json::object()},
        encode_boundary_dimension_entity(manual),encode_boundary_dimension_entity(automatic),
        relation("area-stroke",ConstraintRelationKind::coincident,{corner("area","area:ab","area:b",WallEndpointRole::end),corner("stroke","stroke:e","stroke:a")}),
        relation("stroke-wall",ConstraintRelationKind::coincident,{corner("stroke","stroke:e","stroke:b",WallEndpointRole::end),endpoint("selected",WallEndpointRole::start)}),
        relation("wall-tail",ConstraintRelationKind::coincident,{endpoint("selected",WallEndpointRole::end),endpoint("tail",WallEndpointRole::start)}),
        relation("tail-fixed",ConstraintRelationKind::fixed_anchor,{endpoint("tail",WallEndpointRole::end)},Vec2{8,0})};
}
void cross_lane_translation_and_curves() {
    for (const double sweep:{0.0,std::numbers::pi/2}) {
        auto document=Document::create(drawing_fixture(fixture(sweep))); const auto source=document.snapshot();
        const auto original_digest=document_snapshot_digest(source);
        const auto preview=preview_constraint_authoring(source,intent()); accepted(preview);
        const auto& candidate=preview.candidate_entities();
        const auto boundary=decode_identified_boundary_entity(candidate.at("area"));
        const auto original=decode_identified_boundary_entity(source.entities().at("area"));
        require(candidate.at("area").extensions.at("boundary_geometry_derivation").at("source_boundary_authoring")==
            source.entities().at("area").properties.at("boundary_authoring"),"joint translation lost immutable original boundary receipts");
        for (std::size_t i=0;i<boundary.segments.size();++i) {
            require(boundary.segments[i].segment_id==original.segments[i].segment_id &&
                boundary.segments[i].start_vertex_id==original.segments[i].start_vertex_id &&
                boundary.segments[i].end_vertex_id==original.segments[i].end_vertex_id,"rigid selection changed child identity");
            expect_close(boundary.segments[i].segment.start.x,original.segments[i].segment.start.x);
            expect_close(boundary.segments[i].segment.start.y,original.segments[i].segment.start.y+1);
            require(boundary.segments[i].segment.sweep_radians==sweep || i!=0,"rigid boundary changed its signed circular sweep");
        }
        const auto model=*decode_measurement_linework_model(candidate.at("stroke").properties.at("model")).model;
        const auto old_model=*decode_measurement_linework_model(source.entities().at("stroke").properties.at("model")).model;
        require(model.edges==old_model.edges,"joint translation rewrote original entered stroke receipts");
        const auto curve=replay_measurement_linework(model).edges.front().segment;
        expect_close(curve.start.x,2); expect_close(curve.start.y,1); expect_close(curve.end.x,4); expect_close(curve.end.y,1);
        require(curve.sweep_radians==sweep,"rigid stroke changed signed sweep");
        const auto selected=baseline(candidate.at("selected")),tail=baseline(candidate.at("tail"));
        expect_close(selected.start.x,4); expect_close(selected.start.y,1); expect_close(selected.end.x,6); expect_close(selected.end.y,1);
        require(selected.sweep_radians==sweep && candidate.at("opening")==source.entities().at("opening"),"selected curve or hosted opening station was changed");
        expect_close(tail.start.x,6); expect_close(tail.start.y,1); expect_close(tail.end.x,8); expect_close(tail.end.y,0);
        for (const auto* id:{"manual","automatic"}) {
            const auto before=*decode_boundary_dimension_entity(source.entities().at(id)).dimension;
            const auto after=*decode_boundary_dimension_entity(candidate.at(id)).dimension;
            expect_close(after.text_position.x,before.text_position.x); expect_close(after.text_position.y,before.text_position.y+1);
            require(after.placement==before.placement && after.automatic_placement_version==before.automatic_placement_version,
                "translation changed saved callout placement provenance");
        }
        const auto lower=reconstruct_joint_translation(source.entities(),*intent().joint_translation);
        require(!lower.joint_translation && !lower.joint_translation_completion && !lower.rigid_group_transform && lower.entity_changes.empty(),
            "map reconstruction returned joint/rigid-group authority or changed constraints");
        const auto selected_proof=std::find_if(lower.wall_edits.begin(),lower.wall_edits.end(),[](const auto& edit) { return edit.wall_id=="selected"; });
        require(selected_proof!=lower.wall_edits.end(),"joint translation omitted selected wall proof");
        if (sweep==0) require(selected_proof->version==1 && !selected_proof->rigid_transform && !lower.rigid_wall_transform_completion,
            "straight joint wall borrowed historical selected rigid curve proof authority");
        else require(selected_proof->version==4 && selected_proof->rigid_transform &&
            *selected_proof->rigid_transform==PlanarTransform{{},0,false,false,{0,1}} && lower.rigid_wall_transform_completion,
            "curved joint wall lost its exact selected rigid curve proof authority");
        const auto lower_result=Document::preview_command(source,Command{lower});
        for (const auto* id:{"area","stroke","selected","tail","opening"})
            require(lower_result.entities().at(id)==candidate.at(id),"ordinary lower replay differs from shown joint geometry");
        require(preview_constraint_authoring_snapshot(source,preview).entities()==candidate,
            "source-bound joint replay differs from shown geometry or saved callout provenance");
        require(document_snapshot_digest(document.snapshot())==original_digest,"joint preview mutated document or history");
    }
}
void orientations_and_fixed_conflict() {
    auto values=fixture();
    values.erase(std::remove_if(values.begin(),values.end(),[](const auto& value){return value.id=="tail" || value.id=="tail-fixed" || value.id=="wall-tail";}),values.end());
    values.push_back(wall("vertical",{6,0},{6,3})); values.push_back(wall("parallel",{6,0},{9,0}));
    for (const auto* id:{"vertical","parallel"}) values.push_back(relation(std::string(id)+"-joint",ConstraintRelationKind::coincident,
        {endpoint("selected",WallEndpointRole::end),endpoint(id,WallEndpointRole::start)}));
    values.push_back(relation("perpendicular",ConstraintRelationKind::perpendicular,{endpoint("selected",WallEndpointRole::start),endpoint("selected",WallEndpointRole::end),
        endpoint("vertical",WallEndpointRole::start),endpoint("vertical",WallEndpointRole::end)}));
    values.push_back(relation("parallel-relation",ConstraintRelationKind::parallel,{endpoint("selected",WallEndpointRole::start),endpoint("selected",WallEndpointRole::end),
        endpoint("parallel",WallEndpointRole::start),endpoint("parallel",WallEndpointRole::end)}));
    values.push_back(relation("vertical-fixed",ConstraintRelationKind::fixed_anchor,{endpoint("vertical",WallEndpointRole::end)},Vec2{6,3}));
    values.push_back(relation("parallel-length",ConstraintRelationKind::fixed_length,{endpoint("parallel",WallEndpointRole::start),endpoint("parallel",WallEndpointRole::end)},std::nullopt,parse_quantity("3 m")));
    auto document=Document::create(drawing_fixture(values)); const auto source=document.snapshot(); const auto preview=preview_constraint_authoring(source,intent()); accepted(preview);
    const auto vertical=baseline(preview.candidate_entities().at("vertical")),parallel=baseline(preview.candidate_entities().at("parallel"));
    expect_close(vertical.start.x,6); expect_close(vertical.start.y,1); expect_close(vertical.end.x,6); expect_close(vertical.end.y,3);
    expect_close(parallel.start.x,6); expect_close(parallel.start.y,1); expect_close(parallel.end.x,9); expect_close(parallel.end.y,1);
    values.push_back(relation("selected-fixed",ConstraintRelationKind::fixed_anchor,{endpoint("selected",WallEndpointRole::start)},Vec2{4,0}));
    auto conflict=Document::create(drawing_fixture(values)); const auto before=conflict.snapshot();
    const auto rejected=preview_constraint_authoring(before,intent());
    require(!rejected.accepted() && rejected.candidate_entities()==before.entities() && conflict.snapshot().revision()==before.revision(),
        "genuine selected fixed-anchor conflict mutated or accepted geometry");
}
void deterministic_reordering() {
    auto values=fixture(); values.push_back(wall("remote",{20,0},{22,0}));
    auto document=Document::create(drawing_fixture(values)); const auto source=document.snapshot();
    auto first=intent(); first.joint_translation->partial_wall_ids.push_back("remote");
    auto second=first; std::reverse(second.joint_translation->partial_wall_ids.begin(),second.joint_translation->partial_wall_ids.end());
    const auto a=preview_constraint_authoring(source,first),b=preview_constraint_authoring(source,second); accepted(a); accepted(b);
    require(a.candidate_entities()==b.candidate_entities(),"selected target reorder changed joint solve");
    require(command_to_json(Command{reconstruct_joint_translation(source.entities(),*first.joint_translation)})==
        command_to_json(Command{reconstruct_joint_translation(source.entities(),*second.joint_translation)}),"target reorder changed deterministic lower proof");
}
void coincident_rigid_areas_and_saved_orientations() {
    auto values=fixture();
    values.erase(std::remove_if(values.begin(),values.end(),[](const auto& value) {
        return value.id=="stroke" || value.id=="automatic" || value.id=="area-stroke" || value.id=="stroke-wall";
    }),values.end());
    values.push_back(area("other",0,{2,0}));
    values.push_back(relation("area-other",ConstraintRelationKind::coincident,
        {corner("area","area:ab","area:b",WallEndpointRole::end),corner("other","other:ab","other:a")}));
    values.push_back(relation("other-wall",ConstraintRelationKind::coincident,
        {corner("other","other:ab","other:b",WallEndpointRole::end),endpoint("selected",WallEndpointRole::start)}));
    values.push_back(relation("area-horizontal",ConstraintRelationKind::horizontal,
        {corner("area","area:ab","area:a"),corner("area","area:ab","area:b",WallEndpointRole::end)}));
    values.push_back(relation("other-perpendicular",ConstraintRelationKind::perpendicular,
        {corner("other","other:ab","other:a"),corner("other","other:ab","other:b",WallEndpointRole::end),
         corner("other","other:bc","other:b"),corner("other","other:bc","other:c",WallEndpointRole::end)}));
    auto document=Document::create(drawing_fixture(values)); const auto source=document.snapshot();
    ConstraintAuthoringIntent move; move.joint_translation=JointTranslationIntent{{0,1},{"other","area"},{},{"selected"},true};
    const auto preview=preview_constraint_authoring(source,move); accepted(preview);
    for (const auto* id:{"area","other"}) {
        const auto before=decode_identified_boundary_entity(source.entities().at(id));
        const auto after=decode_identified_boundary_entity(preview.candidate_entities().at(id));
        for (std::size_t index=0;index<before.segments.size();++index) {
            require(after.segments[index].start_vertex_id==before.segments[index].start_vertex_id &&
                after.segments[index].segment_id==before.segments[index].segment_id,"joint areas lost saved child identities");
            require(after.segments[index].segment.start.x==before.segments[index].segment.start.x &&
                after.segments[index].segment.start.y==before.segments[index].segment.start.y+1,
                "coincident rigid area vertex differs from exact original plus offset");
        }
        require(preview.candidate_entities().at(id).extensions.at("boundary_geometry_derivation").at("source_boundary_authoring")==
            source.entities().at(id).properties.at("boundary_authoring"),"coincident rigid area lost immutable entered receipts");
    }
    const auto first=decode_identified_boundary_entity(preview.candidate_entities().at("area"));
    const auto second=decode_identified_boundary_entity(preview.candidate_entities().at("other"));
    require(first.segments[0].segment.end.x==second.segments[0].segment.start.x &&
        first.segments[0].segment.end.y==second.segments[0].segment.start.y,"selected area coincidence is not exact after translation");
    expect_close(baseline(preview.candidate_entities().at("selected")).start.y,1);
    for (const auto& [id,value]:source.entities()) if (value.type=="constraint")
        require(preview.candidate_entities().at(id)==value,"joint pins changed a saved relation or fixed anchor");
    require(preview_constraint_authoring_snapshot(source,preview).entities()==preview.candidate_entities(),"coincident rigid area proof failed replay");
    require(document.snapshot().entities()==source.entities(),"coincident area solve mutated source");
    values.push_back(relation("shared-corner-fixed",ConstraintRelationKind::fixed_anchor,
        {corner("other","other:ab","other:a")},Vec2{2,0}));
    auto conflict=Document::create(drawing_fixture(values)); const auto original=conflict.snapshot();
    const auto refused=preview_constraint_authoring(original,move);
    require(!refused.accepted() && refused.candidate_entities()==original.entities() &&
        document_snapshot_digest(conflict.snapshot())==document_snapshot_digest(original),
        "saved shared-corner fixed conflict was weakened or mutated source");
}
void measured_physical_perimeter_expansion() {
    std::vector<Entity> values{{"p","property",Json::object(),false},
        {"b","building",{{"property_id","p"}},false},{"f","floor",{{"building_id","b"}},false},
        {"l","layer",{{"floor_id","f"}},false},wall("bottom",{0,0},{4,0}),wall("right",{4,0},{4,3}),
        wall("top",{4,3},{0,3}),wall("left",{0,3},{0,0}),wall("partial",{6,0},{8,0})};
    for (auto& value:values) if (value.type=="wall") value.properties["layer_id"]="l";
    auto document=Document::create(drawing_fixture(values)); const auto physical=document.snapshot();
    const auto measured=derive_exterior_wall_measurement(physical,{"bottom","right","top","left"});
    IdentifiedBoundary boundary{"measured","measurement_boundary",{}};
    for (std::size_t i=0;i<measured.boundary.size();++i) boundary.segments.push_back({"measured:e"+std::to_string(i),
        "measured:v"+std::to_string(i),"measured:v"+std::to_string((i+1)%measured.boundary.size()),measured.boundary[i]});
    auto owner=drawing_fixture({encode_identified_boundary_entity(boundary)}).front();
    owner.properties["wall_measurement_source"]=measured.source;
    document.apply(ApplyEntityChanges{document.revision(),{EntityChange::upsert(owner)}, {},"Retain physical measured outline"});
    const auto source=document.snapshot(); ConstraintAuthoringIntent move;
    move.joint_translation=JointTranslationIntent{{0,1},{"measured"},{},{"partial"},true};
    const auto preview=preview_constraint_authoring(source,move); accepted(preview);
    for (const auto* id:{"bottom","right","top","left","partial"}) {
        const auto old=baseline(source.entities().at(id)),updated=baseline(preview.candidate_entities().at(id));
        expect_close(updated.start.x,old.start.x); expect_close(updated.start.y,old.start.y+1);
        expect_close(updated.end.x,old.end.x); expect_close(updated.end.y,old.end.y+1);
    }
    require(wall_measurement_source_current(preview.candidate_entities(),preview.candidate_entities().at("measured")),
        "selected measured outline was not independently rederived from its translated physical perimeter");
    auto lower=reconstruct_joint_translation(source.entities(),*move.joint_translation);
    // Entity-map reconstruction has no document revision authority. Bind the
    // ordinary lower proof to this retained snapshot before replaying it.
    lower.expected_revision=source.revision();
    require(lower.wall_edits.size()==5 && lower.boundary_edits.empty() && lower.exterior_source_edits.size()==1,
        "joint physical outline translation did not expand exactly to its perimeter sources and typed redraw");
    require(Document::preview_command(source,Command{lower}).entities()==preview.candidate_entities(),
        "expanded physical perimeter lower replay differs from the expected candidate");

    // Re-deriving a translated physical offset can differ by a few floating-
    // point bits from translating its retained analytical outline. The selected
    // outline and its physical source must still admit the exact requested move.
    const auto original=decode_identified_boundary_entity(source.entities().at("measured"));
    for (const Vec2 offset:{Vec2{.8,.4},Vec2{-.8,-.4},Vec2{.4,.8},Vec2{-.4,-.8}}) {
        move.joint_translation->offset=offset;
        const auto fractional=preview_constraint_authoring(source,move); accepted(fractional);
        const auto translated=decode_identified_boundary_entity(fractional.candidate_entities().at("measured"));
        for (std::size_t i=0;i<original.segments.size();++i) {
            const auto& before=original.segments[i]; const auto& after=translated.segments[i];
            require(before.segment_id==after.segment_id && before.start_vertex_id==after.start_vertex_id &&
                before.end_vertex_id==after.end_vertex_id && before.segment.sweep_radians==after.segment.sweep_radians,
                "fractional physical translation changed immutable analytical lineage");
            require(after.segment.start.x==before.segment.start.x+offset.x &&
                after.segment.start.y==before.segment.start.y+offset.y &&
                after.segment.end.x==before.segment.end.x+offset.x &&
                after.segment.end.y==before.segment.end.y+offset.y,
                "fractional physical outline differs from exact selected coordinates");
        }
        require(wall_measurement_source_current(fractional.candidate_entities(),fractional.candidate_entities().at("measured")),
            "exact fractional physical translation became a stale measured source");
        require(preview_constraint_authoring_snapshot(source,fractional).entities()==fractional.candidate_entities(),
            "fractional physical translation differs from independent typed replay");
        auto tampered=std::get<ApplyBoundaryConstraintChanges>(constraint_authoring_verified_command(source,fractional,nullptr));
        const auto translated_edit=std::find_if(tampered.exterior_source_edits.begin(),tampered.exterior_source_edits.end(),
            [](const auto& edit){return edit.wall_source_translation.has_value();});
        require(translated_edit!=tampered.exterior_source_edits.end(),"physical translation omitted its typed source proof");
        (*translated_edit->wall_source_translation)["offset"][0]=offset.x+.01;
        bool refused=false; try { (void)Document::preview_command(source,Command{tampered}); }
        catch (const DocumentError&) { refused=true; }
        require(refused && document.snapshot().entities()==source.entities(),"altered typed physical translation offset was admitted or mutated source");
    }
    std::vector<Entity> locked_values;
    for (const auto& [id,value]:source.entities()) locked_values.push_back(value);
    locked_values.push_back(relation("physical-selected-fixed",ConstraintRelationKind::fixed_anchor,
        {endpoint("bottom",WallEndpointRole::start)},Vec2{0,0}));
    auto locked=Document::create(std::move(locked_values)); const auto locked_source=locked.snapshot();
    const auto refused=preview_constraint_authoring(locked_source,move);
    require(!refused.accepted() && refused.candidate_entities()==locked_source.entities() &&
        document_snapshot_digest(locked.snapshot())==document_snapshot_digest(locked_source),
        "fractional exterior translation weakened a genuine physical fixed conflict");
    for (const Vec2 offset:{Vec2{.8,.4},Vec2{-.4,.3}}) {
        const auto before=document.snapshot(); move.joint_translation->offset=offset;
        const auto next=preview_constraint_authoring(before,move); accepted(next);
        const auto old_boundary=decode_identified_boundary_entity(before.entities().at("measured"));
        const auto new_boundary=decode_identified_boundary_entity(next.candidate_entities().at("measured"));
        for (std::size_t i=0;i<old_boundary.segments.size();++i) require(
            new_boundary.segments[i].segment.start.x==old_boundary.segments[i].segment.start.x+offset.x &&
            new_boundary.segments[i].segment.start.y==old_boundary.segments[i].segment.start.y+offset.y,
            "repeated physical translation changed an exact selected target");
        apply_constraint_authoring(document,next);
        const auto applied=document.snapshot();
        require(applied.entities()==next.candidate_entities() &&
            wall_measurement_source_current(applied,applied.entities().at("measured")),
            "repeated fractional translation lost independent source authority");
    }
    const auto translated=document.snapshot(); const auto& translated_owner=translated.entities().at("measured");
    require(translated_owner.properties.at("wall_measurement_source").at("version")==2,
        "fractional exterior translation did not retain versioned physical lineage");
    const auto invalid_owner=[&](Entity owner) {
        require(!wall_measurement_source_current(translated,owner),"tampered translation lineage was current");
    };
    auto forged=translated_owner;
    forged.properties["wall_measurement_source"]["origin_outline"][0]["start"][0]=100.0; invalid_owner(forged);
    forged=translated_owner; forged.properties["wall_measurement_source"]["walls"][0]["thickness_m"]=.24; invalid_owner(forged);
    forged=translated_owner; forged.properties["wall_measurement_source"]["walls"][0]["context"]["layer_id"]="other"; invalid_owner(forged);
    forged=translated_owner; forged.properties["wall_measurement_source"]["translations"][0][0]=.81; invalid_owner(forged);
    forged=translated_owner; forged.properties["wall_measurement_source"]["kernel"]="future"; invalid_owner(forged);
    forged=translated_owner; forged.properties["wall_measurement_source"]["walls"][0]["id"]="missing"; invalid_owner(forged);
    forged=translated_owner; std::swap(forged.properties["wall_measurement_source"]["walls"][0],
        forged.properties["wall_measurement_source"]["walls"][1]); invalid_owner(forged);
    forged=translated_owner; forged.properties["wall_measurement_source"]["previous"]=forged.properties["wall_measurement_source"]; invalid_owner(forged);
    forged=translated_owner;
    forged.properties["wall_measurement_source"]["translations"]=Json::array();
    for (std::size_t i=0;i<4097;++i) forged.properties["wall_measurement_source"]["translations"].push_back({.01,0});
    invalid_owner(forged);
    auto missing=translated.entities(); missing.erase("bottom");
    require(!wall_measurement_source_current(missing,translated_owner) &&
        !materialize_exterior_wall_measurement(translated_owner).boundary.empty(),
        "missing current walls invalidated intrinsic translation history or retained current authority");
    auto thickened=translated.entities(); thickened.at("bottom").properties["thickness_m"]=.24;
    require(!wall_measurement_source_current(thickened,translated_owner),"changed physical thickness retained current translation authority");
    const auto redraws=exterior_wall_measurement_source_updates(translated.entities(),thickened);
    const auto redrawn=edited_boundary_entities_batch(thickened,redraws);
    require(redrawn.at("measured").properties.at("wall_measurement_source").at("version")==1 &&
        wall_measurement_source_current(redrawn,redrawn.at("measured")),
        "ordinary thickness redraw did not reset translated authority to independent physical derivation");
    auto capped_entities=translated.entities(); auto& capped_owner=capped_entities.at("measured");
    auto& capped_source=capped_owner.properties["wall_measurement_source"];
    while (capped_source["translations"].size()<4096) capped_source["translations"].push_back({.01,0});
    for (const auto& record:capped_source.at("walls")) {
        auto& value=capped_entities.at(record.at("id").get<std::string>()).properties["baseline"];
        value=record.at("baseline");
        for (const auto& offset:capped_source.at("translations")) for (const auto* role:{"start","end"}) {
            value[role][0]=value[role][0].get<double>()+offset[0].get<double>();
            value[role][1]=value[role][1].get<double>()+offset[1].get<double>();
        }
    }
    auto capped_boundary=decode_identified_boundary_entity(capped_owner);
    const auto capped_outline=materialize_exterior_wall_measurement(capped_owner).boundary;
    for (std::size_t i=0;i<capped_boundary.segments.size();++i) capped_boundary.segments[i].segment=capped_outline[i];
    capped_owner.properties["segments"]=encode_identified_boundary_entity(capped_boundary).properties.at("segments");
    capped_owner.extensions.erase("boundary_geometry_derivation");
    std::vector<Entity> capped_values; for (const auto& [id,value]:capped_entities) capped_values.push_back(value);
    auto capped_document=Document::create(std::move(capped_values)); const auto capped_snapshot=capped_document.snapshot();
    require(wall_measurement_source_current(capped_snapshot,capped_snapshot.entities().at("measured")),
        "lineage-cap fixture must begin with exact current physical authority");
    move.joint_translation->offset={.01,0}; const auto cap_refused=preview_constraint_authoring(capped_snapshot,move);
    require(!cap_refused.accepted() && cap_refused.candidate_entities()==capped_snapshot.entities() &&
        document_snapshot_digest(capped_document.snapshot())==document_snapshot_digest(capped_snapshot),
        "lineage-cap refusal compacted history or mutated source");
}
void physical_lineage_order_and_split() {
    for (const bool reversed:{false,true}) for (const std::size_t rotation:{0U,1U,3U}) {
        auto values=drawing_fixture({wall("bottom",{0,0},{4,0}),wall("right",{4,0},{4,3}),
            wall("top",{4,3},{0,3}),wall("left",{0,3},{0,0})});
        if (rotation==3) {
            auto& stored=values.front().properties["baseline"];
            std::swap(stored["start"],stored["end"]);
        }
        const auto physical=Document::create(values);
        const auto measured=derive_exterior_wall_measurement(physical.snapshot(),{"bottom","right","top","left"});
        auto outline=measured.boundary;
        if (reversed) {
            std::reverse(outline.begin(),outline.end());
            for (auto& edge:outline) { std::swap(edge.start,edge.end); edge.sweep_radians=-edge.sweep_radians; }
        }
        std::rotate(outline.begin(),outline.begin()+static_cast<std::ptrdiff_t>(rotation),outline.end());
        IdentifiedBoundary boundary{"measured","measurement_boundary",{}};
        for (std::size_t i=0;i<outline.size();++i) boundary.segments.push_back({"measured:e"+std::to_string(i),
            "measured:v"+std::to_string(i),"measured:v"+std::to_string((i+1)%outline.size()),outline[i]});
        auto owner=drawing_fixture({encode_identified_boundary_entity(boundary)}).front();
        owner.properties["wall_measurement_source"]=measured.source; values.push_back(owner);
        // Automatic dimensions exercise insertion identity/provenance as well
        // as the physical source reset in the same typed split transaction.
        for (std::size_t i=0;i<boundary.segments.size();++i) {
            BoundaryDimension dimension; dimension.id="dimension"+std::to_string(i);
            dimension.boundary_id="measured"; dimension.segment_id=boundary.segments[i].segment_id;
            dimension.placement=BoundaryDimensionPlacement::automatic;
            dimension.automatic_placement_version=2;
            dimension.text_position=boundary.segments[i].segment.start;
            values.push_back(drawing_fixture({encode_boundary_dimension_entity(dimension)}).front());
        }
        auto document=Document::create(values);
        Json genesis;
        for (const Vec2 offset:{Vec2{.8,.4},Vec2{-.3,.7},Vec2{.11,-.23}}) {
            const auto before=document.snapshot(); ConstraintAuthoringIntent move;
            move.joint_translation=JointTranslationIntent{offset,{"measured"},{},{"bottom"},true};
            const auto preview=preview_constraint_authoring(before,move); accepted(preview);
            const auto previous=decode_identified_boundary_entity(before.entities().at("measured"));
            const auto next=decode_identified_boundary_entity(preview.candidate_entities().at("measured"));
            for (std::size_t i=0;i<next.segments.size();++i) {
                const auto& a=previous.segments[i]; const auto& b=next.segments[i];
                require(a.segment_id==b.segment_id && a.start_vertex_id==b.start_vertex_id && a.end_vertex_id==b.end_vertex_id &&
                    b.segment.start.x==a.segment.start.x+offset.x && b.segment.start.y==a.segment.start.y+offset.y &&
                    b.segment.end.x==a.segment.end.x+offset.x && b.segment.end.y==a.segment.end.y+offset.y,
                    "cyclic/reversed physical translation changed exact owner coordinates or identities");
            }
            require(preview_constraint_authoring_snapshot(before,preview).entities()==preview.candidate_entities(),
                "cyclic/reversed physical translation failed independent replay");
            apply_constraint_authoring(document,preview);
            auto source=document.snapshot().entities().at("measured").properties.at("wall_measurement_source");
            source.erase("translations");
            if (genesis.is_null()) genesis=source;
            else require(source==genesis,"repeated oriented translation rewrote immutable genesis");
        }
        const auto translated=document.snapshot(); const auto& translated_owner=translated.entities().at("measured");
        auto thickened=translated.entities(); thickened.at("bottom").properties["thickness_m"]=.24;
        const auto redrawn=edited_boundary_entities_batch(thickened,
            exterior_wall_measurement_source_updates(translated.entities(),thickened));
        require(redrawn.at("measured").properties.at("wall_measurement_source").at("version")==1 &&
            wall_measurement_source_current(redrawn,redrawn.at("measured")),
            "oriented v2 thickness redraw did not derive current v1 geometry");
        const auto translated_boundary=decode_identified_boundary_entity(translated_owner);
        const auto& selected_edge=translated_boundary.segments.front();
        ConstraintAuthoringIntent corner_move;
        corner_move.exterior_corner_move=ExteriorCornerMoveIntent{"measured",selected_edge.start_vertex_id,
            {selected_edge.segment.start.x+.1,selected_edge.segment.start.y+.1},true};
        const auto corner_preview=preview_constraint_authoring(translated,corner_move); accepted(corner_preview);
        require(corner_preview.candidate_entities().at("measured").properties.at("wall_measurement_source").at("version")==1 &&
            wall_measurement_source_current(corner_preview.candidate_entities(),corner_preview.candidate_entities().at("measured")) &&
            preview_constraint_authoring_snapshot(translated,corner_preview).entities()==corner_preview.candidate_entities(),
            "oriented v2 corner inverse lost independent v1 redraw authority");
        std::ostringstream length; length<<std::setprecision(17)<<segment_length(selected_edge.segment)+.2<<" m";
        ConstraintAuthoringIntent resize;
        resize.exterior_segment_resize=ExteriorSegmentResizeIntent{"measured",selected_edge.segment_id,
            parse_quantity(length.str()),BoundaryFixedEndpoint::start,false,true};
        const auto resize_preview=preview_constraint_authoring(translated,resize); accepted(resize_preview);
        require(resize_preview.candidate_entities().at("measured").properties.at("wall_measurement_source").at("version")==1 &&
            wall_measurement_source_current(resize_preview.candidate_entities(),resize_preview.candidate_entities().at("measured")) &&
            preview_constraint_authoring_snapshot(translated,resize_preview).entities()==resize_preview.candidate_entities(),
            "oriented v2 resize inverse lost independent v1 redraw authority");

        auto alternate=translated.entities(); std::vector<std::string> alternate_ids;
        for (const auto* id:{"bottom","right","top","left"}) {
            auto copy=alternate.at(id); copy.id=std::string("alternate-")+id;
            for (const auto* role:{"start","end"})
                copy.properties["baseline"][role][0]=copy.properties["baseline"][role][0].get<double>()+20.0;
            alternate_ids.push_back(copy.id); alternate.emplace(copy.id,std::move(copy));
        }
        const auto replacement=derive_replacement_exterior_wall_measurement(alternate,translated_owner,alternate_ids);
        require(replacement.source.at("version")==1 && replacement.source==derive_exterior_wall_measurement(alternate,alternate_ids).source,
            "new physical source IDs borrowed retained v2 authority");
        BoundaryGeometryEdit forgery; forgery.boundary_id=forgery.target_id="measured";
        forgery.kind=BoundaryGeometryEditKind::redefine_boundary;
        forgery.replacement_segments=translated_owner.properties.at("segments"); forgery.replacement_wall_source_ids=alternate_ids;
        bool refused=false; try { (void)edited_boundary_entities(alternate,forgery); } catch (const std::invalid_argument&) { refused=true; }
        require(refused,"old translated outline accepted unrelated valid new physical source IDs");

        const auto split_command=make_wall_split_command(translated,{"bottom","bottom-second",.4,"split-seam",
            {{"measured","split-vertex","split-edge","split-dimension"}}});
        document.apply(split_command); const auto split=document.snapshot();
        require(split.entities().at("measured").properties.at("wall_measurement_source").at("version")==1 &&
            wall_measurement_source_current(split,split.entities().at("measured")) && split.entities().contains("split-dimension"),
            "translated wall split lost final physical source or automatic dimension");
        const auto split_boundary=decode_identified_boundary_entity(split.entities().at("measured"));
        require(split_boundary.segments.size()==boundary.segments.size()+1 &&
            std::any_of(split_boundary.segments.begin(),split_boundary.segments.end(),[](const auto& edge) {
                return edge.segment_id=="split-edge" && (edge.start_vertex_id=="split-vertex" || edge.end_vertex_id=="split-vertex"); }),
            "translated wall split lost explicit analytical insertion identities");
        document.undo(document.revision()); require(document.snapshot().entities()==translated.entities(),"translated wall split Undo changed source lineage");
        document.redo(document.revision()); require(document.snapshot().entities()==split.entities(),"translated wall split Redo changed final geometry");
        const auto directory=std::filesystem::temp_directory_path()/
            ("vertex-lineage-split-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        struct Cleanup { std::filesystem::path path; ~Cleanup(){std::error_code error;std::filesystem::remove_all(path,error);} } cleanup{directory};
        require(std::filesystem::create_directory(directory),"could not create lineage split storage fixture");
        const auto path=directory/"split.vertex"; (void)ProjectStore::save(path,split);
        auto reopened=ProjectStore::load(path);
        require(reopened.document.snapshot().entities()==split.entities(),"translated split reopen changed final source/provenance");
        reopened.document.undo(reopened.document.revision());
        require(reopened.document.snapshot().entities()==translated.entities(),"translated split reopened Undo changed genesis");
        reopened.document.redo(reopened.document.revision());
        require(reopened.document.snapshot().entities()==split.entities(),"translated split reopened Redo changed final source");
    }
}
void physical_lineage_replay_work_budget() {
    constexpr std::size_t count=2048;
    std::vector<Entity> values; std::vector<std::string> ids; std::vector<Vec2> vertices;
    for (std::size_t i=0;i<count;++i) {
        const auto angle=2*std::numbers::pi*static_cast<double>(i)/static_cast<double>(count);
        vertices.push_back({100*std::cos(angle),100*std::sin(angle)});
    }
    for (std::size_t i=0;i<count;++i) {
        ids.push_back("ring-"+std::to_string(i)); values.push_back(wall(ids.back(),vertices[i],vertices[(i+1)%count]));
    }
    auto document=Document::create(drawing_fixture(values));
    const auto source=document.snapshot();
    const auto derived=derive_exterior_wall_measurement(source,ids);
    IdentifiedBoundary boundary{"measured","measurement_boundary",{}};
    for (std::size_t i=0;i<count;++i) boundary.segments.push_back({"e"+std::to_string(i),"v"+std::to_string(i),
        "v"+std::to_string((i+1)%count),derived.boundary[i]});
    auto owner=drawing_fixture({encode_identified_boundary_entity(boundary)}).front();
    auto records=Json::array(),outline=Json::array();
    for (const auto& record:derived.source.at("walls")) {
        const auto& value=source.entities().at(record.at("id").get<std::string>());
        records.push_back({{"id",value.id},{"context",record.at("context")},{"baseline",value.properties.at("baseline")},
            {"thickness_m",value.properties.at("thickness_m")}});
    }
    std::sort(records.begin(),records.end(),[](const auto& a,const auto& b){return a.at("id").template get<std::string>()<b.at("id").template get<std::string>();});
    for (const auto& edge:derived.boundary) outline.push_back({{"start",{edge.start.x,edge.start.y}},
        {"end",{edge.end.x,edge.end.y}},{"sweep_radians",edge.sweep_radians}});
    owner.properties["wall_measurement_source"]={{"version",2},{"basis","exterior"},{"kernel","stable"},
        {"walls",records},{"origin_outline",outline},{"translations",Json::array()}};
    for (std::size_t i=0;i<3;++i) owner.properties["wall_measurement_source"]["translations"].push_back({.01,0});
    bool refused=false;
    try { (void)materialize_exterior_wall_measurement(owner); }
    catch (const std::invalid_argument& error) { refused=std::string_view(error.what()).find("combined replay-work budget")!=std::string_view::npos; }
    require(refused,"source-derived large physical lineage did not refuse combined replay work before materialization");
}
void mixed_physical_exteriors_fractional_translation() {
    auto values=drawing_fixture({wall("whole-bottom",{20,0},{24,0}),wall("whole-right",{24,0},{24,3}),
        wall("whole-top",{24,3},{20,3}),wall("whole-left",{20,3},{20,0}),
        wall("partial-bottom",{28,0},{32,0}),wall("partial-right",{32,0},{32,3}),
        wall("partial-top",{32,3},{28,3}),wall("partial-left",{28,3},{28,0})});
    for (const auto* neighbor:{"right","left"}) {
        const bool right=std::string_view(neighbor)=="right";
        values.push_back(relation(std::string("partial-joint-")+neighbor,ConstraintRelationKind::coincident,
            {endpoint("partial-bottom",right ? WallEndpointRole::end : WallEndpointRole::start),
             endpoint(std::string("partial-")+neighbor,right ? WallEndpointRole::start : WallEndpointRole::end)}));
        values.push_back(relation(std::string("partial-fixed-")+neighbor,ConstraintRelationKind::fixed_anchor,
            {endpoint(std::string("partial-")+neighbor,right ? WallEndpointRole::end : WallEndpointRole::start)},
            right ? Vec2{32,3} : Vec2{28,3}));
    }
    auto document=Document::create(values);
    for (const auto* prefix:{"whole","partial"}) {
        std::vector<std::string> ids;
        for (const auto* suffix:{"bottom","right","top","left"}) ids.push_back(std::string(prefix)+"-"+suffix);
        const auto measured=derive_exterior_wall_measurement(document.snapshot(),ids);
        IdentifiedBoundary boundary{prefix,"measurement_boundary",{}};
        for (std::size_t i=0;i<measured.boundary.size();++i) boundary.segments.push_back({
            std::string(prefix)+":e"+std::to_string(i),std::string(prefix)+":v"+std::to_string(i),
            std::string(prefix)+":v"+std::to_string((i+1)%measured.boundary.size()),measured.boundary[i]});
        auto owner=drawing_fixture({encode_identified_boundary_entity(boundary)}).front();
        owner.properties["wall_measurement_source"]=measured.source;
        document.apply(ApplyEntityChanges{document.revision(),{EntityChange::upsert(owner)}, {},"Capture measured exterior"});
    }
    const auto source=document.snapshot(); const auto original_digest=document_snapshot_digest(source);
    ConstraintAuthoringIntent move; move.joint_translation=JointTranslationIntent{{.8,.4},{"whole"},{},{"partial-bottom"},true};
    const auto preview=preview_constraint_authoring(source,move); accepted(preview);
    const auto& candidate=preview.candidate_entities();
    for (const auto* id:{"whole","partial"}) require(wall_measurement_source_current(candidate,candidate.at(id)),
        "mixed fractional move left an exterior stale");
    require(candidate.at("partial-top")==source.entities().at("partial-top"),
        "mixed fractional move changed the unconnected far wall");
    const auto bottom=baseline(candidate.at("partial-bottom")),right=baseline(candidate.at("partial-right")),left=baseline(candidate.at("partial-left"));
    require(bottom.start.x==28+.8 && bottom.start.y==.4 && bottom.end.x==32+.8 && bottom.end.y==.4 &&
        right.start.x==bottom.end.x && right.start.y==bottom.end.y && left.end.x==bottom.start.x && left.end.y==bottom.start.y,
        "mixed fractional move lost exact selected wall or contact coordinates");
    expect_close(right.end.x,32); expect_close(right.end.y,3); expect_close(left.start.x,28); expect_close(left.start.y,3);
    const auto before=decode_identified_boundary_entity(source.entities().at("whole"));
    const auto after=decode_identified_boundary_entity(candidate.at("whole"));
    for (std::size_t i=0;i<before.segments.size();++i) require(
        after.segments[i].segment.start.x==before.segments[i].segment.start.x+.8 &&
        after.segments[i].segment.start.y==before.segments[i].segment.start.y+.4,
        "mixed fractional move lost exact rigid exterior coordinates");
    require(preview_constraint_authoring_snapshot(source,preview).entities()==candidate,
        "mixed fractional move does not reproduce its shown candidate");
    require(document_snapshot_digest(document.snapshot())==original_digest,"mixed fractional preview mutated source or history");
    const auto verified=constraint_authoring_verified_command(source,preview,nullptr);
    const auto encoded=command_to_json(verified);
    const auto decoded=command_from_json(encoded);
    require(command_to_json(decoded)==encoded && Document::preview_command(source,decoded).entities()==candidate,
        "mixed physical translation JSON must retain exact version-eight source redraws and independently replay the shown candidate");
    const auto& source_edits=encoded.at("proof").at("exterior_source_edits");
    const auto translation=std::find_if(source_edits.begin(),source_edits.end(),[](const auto& edit) {
        return edit.at("version")==8;
    });
    require(translation!=source_edits.end(),"serialized mixed translation must contain its version-eight redraw");
    const auto translation_index=static_cast<std::size_t>(translation-source_edits.begin());
    const auto rejected_json=[&](const Json& altered) {
        bool refused=false;
        try { (void)Document::preview_command(source,command_from_json(altered)); }
        catch (const std::exception&) { refused=true; }
        require(refused && document_snapshot_digest(document.snapshot())==original_digest,
            "unsupported or altered serialized physical translation was admitted or mutated source");
    };
    for (const int unsupported_version:{4,5,7,9}) {
        auto altered=encoded;
        altered["proof"]["exterior_source_edits"][translation_index]["version"]=unsupported_version;
        rejected_json(altered);
    }
    auto altered=encoded;
    altered["proof"]["exterior_source_edits"][translation_index]["wall_source_translation"]["offset"][0]=.81;
    rejected_json(altered);
    altered=encoded;
    altered["proof"]["exterior_source_edits"][translation_index]["wall_source_translation"]["future"]=true;
    rejected_json(altered);
    document.apply(verified);
    require(document.snapshot().history().size()==source.history().size()+1 &&
        command_to_json(*document.snapshot().history().back().boundary_constraint_changes).at("version")==17,
        "mixed fractional move was not one version-17 command");
    document.undo(document.revision()); require(document.snapshot().entities()==source.entities(),"mixed move Undo lost source");
    document.redo(document.revision()); require(document.snapshot().entities()==candidate,"mixed move Redo lost candidate");
}
void independent_partial_callout_and_tamper_refusal() {
    auto values=fixture(); auto partial=decode_identified_boundary_entity(area("partial-area"));
    for (auto& edge:partial.segments) { edge.segment.start.x+=4; edge.segment.end.x+=4; }
    values.push_back(encode_identified_boundary_entity(partial));
    values.push_back(relation("partial-corner",ConstraintRelationKind::coincident,
        {corner("partial-area","partial-area:ab","partial-area:a"),endpoint("selected",WallEndpointRole::start)}));
    for (std::size_t i=1;i<partial.segments.size();++i) values.push_back(relation("partial-fixed-"+std::to_string(i),
        ConstraintRelationKind::fixed_anchor,{corner("partial-area",partial.segments[i].segment_id,partial.segments[i].start_vertex_id)},
        partial.segments[i].segment.start));
    BoundaryDimension dimension{"partial-callout","partial-area","partial-area:ab",{5,-.25},BoundaryDimensionPlacement::automatic};
    dimension.automatic_placement_version=2;
    auto callout=encode_boundary_dimension_entity(dimension); callout.extensions["future_callout"]={{"retain",true}};
    values.push_back(callout);
    auto document=Document::create(drawing_fixture(values)); const auto source=document.snapshot(); auto move=intent();
    move.joint_translation->dimension_ids={"partial-callout","manual"};
    const auto preview=preview_constraint_authoring(source,move); accepted(preview);
    const auto after=*decode_boundary_dimension_entity(preview.candidate_entities().at("partial-callout")).dimension;
    expect_close(after.text_position.x,5); expect_close(after.text_position.y,.75);
    require(after.placement==BoundaryDimensionPlacement::manual && !after.automatic_placement_version &&
        preview.candidate_entities().at("partial-callout").extensions==source.entities().at("partial-callout").extensions,
        "independently selected partial callout lost source-relative position, manual placement or metadata");
    const auto manual=*decode_boundary_dimension_entity(preview.candidate_entities().at("manual")).dimension;
    expect_close(manual.text_position.y,-1);
    const auto command=std::get<ApplyBoundaryConstraintChanges>(constraint_authoring_verified_command(source,preview,nullptr));
    require(std::count_if(command.dimension_placement_moves.begin(),command.dimension_placement_moves.end(),[](const auto& item) {
        return item.dimension_id=="manual";
    })==1,"explicit selection repeated a callout already owned by a rigid target");
    require(Document::preview_command(source,Command{command}).entities()==preview.candidate_entities(),
        "independent selected callout was not admitted by the bound joint intent");
    const auto unchanged=document_snapshot_digest(source);
    const auto rejected=[&](ApplyBoundaryConstraintChanges tampered) {
        bool refused=false; try { (void)Document::preview_command(source,Command{std::move(tampered)}); }
        catch (const std::exception&) { refused=true; }
        require(refused && document_snapshot_digest(document.snapshot())==unchanged,"tampered joint callout proof was admitted or mutated source");
    };
    auto offset=command;
    const auto placement=std::find_if(offset.dimension_placement_moves.begin(),offset.dimension_placement_moves.end(),[](const auto& item) {
        return item.dimension_id=="manual";
    });
    require(placement!=offset.dimension_placement_moves.end(),"rigid-owner callout proof is missing");
    placement->offset.y+=.25; rejected(std::move(offset));
    auto unbound=command; unbound.joint_translation->dimension_ids.clear(); rejected(std::move(unbound));
    auto wrong_intent=command; wrong_intent.joint_translation->offset.y=2; rejected(std::move(wrong_intent));
    auto missing=move; missing.joint_translation->dimension_ids={"missing-callout"};
    require(!preview_constraint_authoring(source,missing).accepted(),"missing selected callout was accepted");
    auto overlap=move; overlap.joint_translation->dimension_ids={"selected"};
    require(!preview_constraint_authoring(source,overlap).accepted(),"geometry/callout selection overlap was accepted");
    auto unsupported=source.entities(); unsupported.at("partial-callout").properties["dimension_version"]=99;
    bool refused=false; try { (void)reconstruct_joint_translation(unsupported,*move.joint_translation); }
    catch (const std::exception&) { refused=true; }
    require(refused,"map-only reconstruction accepted an unsupported selected callout");
}
void tangent_connected_translation() {
    auto values=fixture(std::numbers::pi/2);
    values.erase(std::remove_if(values.begin(),values.end(),[](const auto& value){return value.id=="tail" || value.id=="tail-fixed" || value.id=="wall-tail";}),values.end());
    values.push_back(wall("tail",{6,0},{8,2}));
    values.push_back(relation("smooth-joint",ConstraintRelationKind::tangent,{endpoint("selected",WallEndpointRole::end),
        endpoint("selected",WallEndpointRole::start),endpoint("tail",WallEndpointRole::start),endpoint("tail",WallEndpointRole::end)}));
    values.push_back(relation("tail-length",ConstraintRelationKind::fixed_length,{endpoint("tail",WallEndpointRole::start),
        endpoint("tail",WallEndpointRole::end)},std::nullopt,parse_quantity("2.8284271247461903 m")));
    auto document=Document::create(drawing_fixture(values)); const auto source=document.snapshot();
    const auto preview=preview_constraint_authoring(source,intent()); accepted(preview);
    const auto selected=baseline(preview.candidate_entities().at("selected")),tail=baseline(preview.candidate_entities().at("tail"));
    expect_close(selected.end.x,6); expect_close(selected.end.y,1);
    require(selected.end.x==tail.start.x && selected.end.y==tail.start.y,"tangent contact endpoints were not canonicalized to the exact selected joint");
    expect_close(tail.end.x,8); expect_close(tail.end.y,3);
    require(selected.sweep_radians==std::numbers::pi/2,"tangent-connected translation changed captured circular sweep");
    require(preview_constraint_authoring_snapshot(source,preview).entities()==preview.candidate_entities(),
        "tangent-connected joint proof failed independent replay");
}
void related_physical_outline_rederivation() {
    std::vector<Entity> values{{"p","property",Json::object(),false},
        {"b","building",{{"property_id","p"}},false},{"f","floor",{{"building_id","b"}},false},
        {"l","layer",{{"floor_id","f"}},false},wall("bottom",{0,0},{4,0}),wall("right",{4,0},{4,3}),
        wall("top",{4,3},{0,3}),wall("left",{0,3},{0,0})};
    for (auto& value:values) if (value.type=="wall") value.properties["layer_id"]="l";
    values=drawing_fixture(std::move(values));
    auto physical=Document::create(values); const auto measurement=derive_exterior_wall_measurement(physical.snapshot(),{"bottom","right","top","left"});
    IdentifiedBoundary boundary{"measured","measurement_boundary",{}};
    for (std::size_t i=0;i<measurement.boundary.size();++i) boundary.segments.push_back({"measured:e"+std::to_string(i),
        "measured:v"+std::to_string(i),"measured:v"+std::to_string((i+1)%measurement.boundary.size()),measurement.boundary[i]});
    const auto contact=std::find_if(boundary.segments.begin(),boundary.segments.end(),[](const auto& edge) {
        return std::abs(edge.segment.start.x+.07)<1e-7 && std::abs(edge.segment.start.y+.07)<1e-7;
    });
    require(contact!=boundary.segments.end(),"physical outline fixture did not derive the expected lower-left corner");
    auto owner=drawing_fixture({encode_identified_boundary_entity(boundary)}).front();
    owner.properties["wall_measurement_source"]=measurement.source; values.push_back(owner);
    auto line=stroke(); auto model=*decode_measurement_linework_model(line.properties.at("model")).model;
    model.anchor={-.07,-.07}; model.edges[0].receipt.start=model.anchor; model.edges[0].receipt.chord_end=Vec2{-2.07,-.07};
    line.properties["model"]=encode_measurement_linework_model(model); line.properties["layer_id"]="l"; values.push_back(line);
    values.push_back(relation("outline-contact",ConstraintRelationKind::coincident,
        {corner("measured",contact->segment_id,contact->start_vertex_id),corner("stroke","stroke:e","stroke:a")}));
    for (const auto& pair:std::vector<std::pair<std::string,std::string>>{{"bottom","right"},{"right","top"},{"top","left"},{"left","bottom"}})
        values.push_back(relation(pair.first+"-"+pair.second,ConstraintRelationKind::coincident,
            {endpoint(pair.first,WallEndpointRole::end),endpoint(pair.second,WallEndpointRole::start)}));
    values.push_back(relation("upper-right-fixed",ConstraintRelationKind::fixed_anchor,{endpoint("top",WallEndpointRole::start)},Vec2{4,3}));
    values.push_back(relation("upper-left-fixed",ConstraintRelationKind::fixed_anchor,{endpoint("top",WallEndpointRole::end)},Vec2{0,3}));
    auto document=Document::create(drawing_fixture(values)); const auto source=document.snapshot(); ConstraintAuthoringIntent move;
    move.joint_translation=JointTranslationIntent{{0,1},{},{"stroke"},{"bottom"},true};
    const auto preview=preview_constraint_authoring(source,move); accepted(preview);
    const auto bottom=baseline(preview.candidate_entities().at("bottom")),right=baseline(preview.candidate_entities().at("right"));
    expect_close(bottom.start.y,1); expect_close(bottom.end.y,1); expect_close(right.start.y,1); expect_close(right.end.y,3);
    const auto updated=decode_identified_boundary_entity(preview.candidate_entities().at("measured"));
    const auto corresponding=std::find_if(updated.segments.begin(),updated.segments.end(),[&](const auto& edge) {
        return edge.start_vertex_id==contact->start_vertex_id;
    });
    require(corresponding!=updated.segments.end(),"connected source rederivation changed corner identity");
    expect_close(corresponding->segment.start.x,-.07); expect_close(corresponding->segment.start.y,.93);
    require(wall_measurement_source_current(preview.candidate_entities(),preview.candidate_entities().at("measured")),
        "hard-related physical measured owner was not rederived from final solved perimeter");
    require(preview.boundary_edits().empty() && preview.exterior_source_edits().size()==1,
        "hard-related source owner received an independent authored edit rather than physical redraw");
    require(preview_constraint_authoring_snapshot(source,preview).entities()==preview.candidate_entities(),
        "hard-related physical outline joint proof failed independent replay");
    require(document.snapshot().entities()==source.entities(),"connected physical outline preview mutated the source");
}
void bound_presentation_supplements() {
    auto values=fixture(); AnnotationState annotations;
    auto label=instantiate_label(default_label_templates().front(),"selected-label");
    label.placement.position={10,20}; label.model_plan=true; annotations.labels.push_back(label);
    auto retained=label; retained.id="retained-label"; retained.placement.position={30,40}; annotations.labels.push_back(retained);
    SymbolInstance symbol; symbol.id="selected-symbol"; symbol.definition=default_symbol_catalog().front();
    symbol.symbol_id=symbol.definition->id; symbol.placement.position={50,60}; annotations.symbols.push_back(symbol);
    auto owner=make_annotation_entity("annotations",annotations,AnnotationEntityContext{"p","b","f","l"}); owner.extensions["retained_vendor"]={{"value",17}};
    values.push_back(owner);
    Entity reference{"reference","reference_asset",{{"asset_id","reference-image"},{"position_m",{70,80}},
        {"metres_per_source_unit",.01},{"scale",1.0},{"rotation_degrees",0.0},{"intensity",.7},{"visible",true}},false,Json::object()};
    reference.extensions["retained_vendor"]={{"value",23}}; values.push_back(reference);
    auto document=Document::create(drawing_fixture(values),{Asset::create("reference-image","image/png",{std::byte{0x01}})});
    const auto source=document.snapshot(); const auto original=document_snapshot_digest(source);
    const auto preview=preview_constraint_authoring(source,intent()); accepted(preview);
    auto command=std::get<ApplyBoundaryConstraintChanges>(constraint_authoring_verified_command(source,preview,nullptr));
    auto changed_annotations=source.entities().at("annotations");
    changed_annotations.properties["state"]["labels"][0]["placement"]["y"]=21;
    changed_annotations.properties["state"]["symbols"][0]["placement"]["y"]=61;
    auto changed_reference=source.entities().at("reference"); changed_reference.properties["position_m"]={70,81};
    command.supplemental_source_completion=true;
    command.supplemental_entity_changes={EntityChange::upsert(changed_annotations),EntityChange::upsert(changed_reference)};
    const auto candidate=Document::preview_command(source,Command{command});
    auto expected=preview.candidate_entities(); expected.at("annotations")=changed_annotations; expected.at("reference")=changed_reference;
    require(candidate.entities()==expected,"bound annotation/reference supplements differ from independently specified translated placements");
    const auto rejected=[&](ApplyBoundaryConstraintChanges tampered) {
        bool refused=false; try { (void)Document::preview_command(source,Command{std::move(tampered)}); }
        catch (const std::exception&) { refused=true; }
        require(refused && document_snapshot_digest(document.snapshot())==original,
            "tampered presentation supplement was admitted or mutated source");
    };
    auto position=command; position.supplemental_entity_changes[0].entity.properties["state"]["labels"][0]["placement"]["x"]=10.25;
    rejected(std::move(position));
    auto metadata=command; metadata.supplemental_entity_changes[0].entity.properties["state"]["labels"][0]["content"]="replacement";
    rejected(std::move(metadata));
    auto reference_position=command; reference_position.supplemental_entity_changes[1].entity.properties["position_m"]={70,81.25};
    rejected(std::move(reference_position));
    auto reference_metadata=command; reference_metadata.supplemental_entity_changes[1].entity.extensions["retained_vendor"]["value"]=24;
    rejected(std::move(reference_metadata));
    auto view_move=intent(); view_move.joint_translation->presentation_offset=Vec2{2,-1};
    const auto view_preview=preview_constraint_authoring(source,view_move); accepted(view_preview);
    auto view_command=std::get<ApplyBoundaryConstraintChanges>(constraint_authoring_verified_command(source,view_preview,nullptr));
    auto view_annotations=changed_annotations;
    view_annotations.properties["state"]["symbols"][0]["placement"]["x"]=52;
    view_annotations.properties["state"]["symbols"][0]["placement"]["y"]=59;
    auto view_reference=source.entities().at("reference"); view_reference.properties["position_m"]={72,79};
    view_command.supplemental_source_completion=true;
    view_command.supplemental_entity_changes={EntityChange::upsert(view_annotations),EntityChange::upsert(view_reference)};
    auto view_expected=view_preview.candidate_entities(); view_expected.at("annotations")=view_annotations; view_expected.at("reference")=view_reference;
    require(Document::preview_command(source,Command{view_command}).entities()==view_expected,
        "presentation offset did not retain model-plan label coordinates and independent view-XY symbol/reference coordinates");
    auto wrong_view=view_command; wrong_view.joint_translation->presentation_offset.reset(); rejected(std::move(wrong_view));
    auto nonfinite=view_move; nonfinite.joint_translation->presentation_offset->x=std::numeric_limits<double>::infinity();
    require(!preview_constraint_authoring(source,nonfinite).accepted(),"nonfinite presentation offset was admitted");
    require(document_snapshot_digest(document.snapshot())==original,"presentation supplement preview mutated source");
    document.apply(Command{command});
    require(document.snapshot().entities()==expected,"presentation supplement apply differs from verified preview");
    require(document.snapshot().assets()==source.assets(),"presentation supplement changed retained reference bytes");
}
}
int main() {
    sketch::testing::noninteractive_errors();
    try {
        const std::pair<const char*,void(*)()> cases[]{
            {"cross_lane_translation_and_curves",cross_lane_translation_and_curves},
            {"orientations_and_fixed_conflict",orientations_and_fixed_conflict},
            {"deterministic_reordering",deterministic_reordering},
            {"coincident_rigid_areas_and_saved_orientations",coincident_rigid_areas_and_saved_orientations},
            {"measured_physical_perimeter_expansion",measured_physical_perimeter_expansion},
            {"physical_lineage_order_and_split",physical_lineage_order_and_split},
            {"physical_lineage_replay_work_budget",physical_lineage_replay_work_budget},
            {"mixed_physical_exteriors_fractional_translation",mixed_physical_exteriors_fractional_translation},
            {"independent_partial_callout_and_tamper_refusal",independent_partial_callout_and_tamper_refusal},
            {"tangent_connected_translation",tangent_connected_translation},
            {"related_physical_outline_rederivation",related_physical_outline_rederivation},
            {"bound_presentation_supplements",bound_presentation_supplements}};
        for (const auto& [name,run]:cases) {
            try { run(); } catch (const std::exception& error) { throw std::runtime_error(std::string(name)+": "+error.what()); }
        }
        std::cout<<"joint translation authoring tests passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
