#include "sketch/boundary_receipt.hpp"
#include "sketch/boundary_authoring_recovery.hpp"
#include "sketch/boundary_construction.hpp"
#include "sketch/boundary_entity.hpp"
#include "sketch/geometry_operations.hpp"
#include "sketch/measurement_linework.hpp"
#include "sketch/project_store.hpp"
#include "sketch/project_exchange.hpp"
#include "support/noninteractive_errors.hpp"

#include <cmath>
#include <iostream>
#include <numbers>
#include <stdexcept>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <vector>

namespace {
using namespace sketch;
void require(bool value,const char* message) { if (!value) throw std::runtime_error(message); }
void invalid(const std::function<void()>& operation,const char* message) {
    try { operation(); } catch (const std::invalid_argument&) { return; }
    throw std::runtime_error(message);
}
void rejected(const std::function<void()>& operation,const char* message) {
    try { operation(); } catch (const std::exception&) { return; }
    throw std::runtime_error(message);
}
ConstructionReceipt chord(BoundaryConstructionKind kind,bool metric=true) {
    ConstructionReceipt receipt; receipt.segment_id="arc"; receipt.kind=kind; receipt.start={0,0};
    receipt.chord_input=ChordInput{parse_quantity("2",metric ? Unit::metre : Unit::foot),parse_angle("0 deg")};
    if (kind==BoundaryConstructionKind::arc_chord_angle) receipt.angle=parse_angle("90 deg");
    else if (kind==BoundaryConstructionKind::arc_chord_height) receipt.height=parse_quantity("1",metric ? Unit::metre : Unit::foot);
    else { receipt.arc_length=parse_quantity("3.141592653589793",metric ? Unit::metre : Unit::foot); receipt.clockwise=true; }
    return receipt;
}

// The existing codec compiles this fixture. Missing typed chord support fails
// behaviorally, rather than relying on a newly declared symbol to compile.
void test_typed_chord_is_authoritative_and_replayable() {
    ConstructionReceipt legacy;
    legacy.segment_id="typed-chord-arc";
    legacy.kind=BoundaryConstructionKind::arc_chord_angle;
    legacy.start={0.125,-0.25}; legacy.chord_end=Vec2{2.125,-0.25};
    legacy.angle=parse_angle("90 deg");
    auto encoded=encode_construction_receipt(legacy);
    ConstructionReceipt line;
    line.segment_id="chord-input"; line.kind=BoundaryConstructionKind::line_heading;
    line.start=legacy.start; line.distance=parse_quantity("2000 mm"); line.heading=parse_angle("0 deg");
    const auto typed=encode_construction_receipt(line);
    encoded.erase("chord_end"); encoded["version"]=2;
    encoded["chord_input"]={{"length",typed.at("distance")},{"heading",typed.at("heading")}};
    std::optional<ConstructionReceipt> receipt;
    try { receipt=decode_construction_receipt(encoded); }
    catch (const std::invalid_argument&) {}
    require(receipt.has_value(),"typed chord length/heading receipt must decode without an authoritative endpoint");
    const auto replay=replay_construction_receipt(*receipt,{legacy.start});
    require(replay.segment.end.x==2.125 && replay.segment.end.y==-0.25 &&
        std::abs(replay.segment.sweep_radians-std::numbers::pi/2)<1e-12,
        "typed chord reconstructs analytical endpoint and sweep from original inputs");
    require(encode_construction_receipt(replay.receipt)==encoded,
        "typed chord original quantity and angle expressions round trip exactly");
}

void test_three_typed_methods_exact_units_and_rejections() {
    for (bool metric : {false,true}) for (auto kind : {BoundaryConstructionKind::arc_chord_angle,
        BoundaryConstructionKind::arc_chord_height,BoundaryConstructionKind::arc_chord_length}) {
        const auto input=chord(kind,metric); const auto encoded=encode_construction_receipt(input);
        require(encoded.at("version")==2 && !encoded.contains("chord_end"), "typed receipt has one discriminated authority");
        const auto decoded=decode_construction_receipt(encoded); const auto replay=replay_construction_receipt(decoded,{{0,0}});
        const auto unit=metric ? 1.0 : 0.3048;
        const auto sweep=kind==BoundaryConstructionKind::arc_chord_angle ? std::numbers::pi/2 :
            kind==BoundaryConstructionKind::arc_chord_height ? std::numbers::pi : -std::numbers::pi;
        const auto length=kind==BoundaryConstructionKind::arc_chord_angle ? std::numbers::pi/std::sqrt(2.0)*unit : std::numbers::pi*unit;
        require(replay.segment.end.x==2*unit && replay.segment.end.y==0 &&
            std::abs(replay.segment.sweep_radians-sweep)<1e-9 && std::abs(segment_length(replay.segment)-length)<1e-9,
            "typed chord methods independently reconstruct endpoints, signed sweep and curve length in both units");
        require(replay.receipt==input && decoded.chord_input->length.original_expression=="2" &&
            decoded.chord_input->length.entered_unit==(metric ? Unit::metre : Unit::foot), "bare typed chord preserves original expression and default unit");
        auto malformed=encoded; malformed["chord_end"]={2,0};
        invalid([&] { (void)decode_construction_receipt(malformed); }, "two chord authorities must reject");
        malformed=encoded; malformed.erase("version");
        invalid([&] { (void)decode_construction_receipt(malformed); }, "typed payload cannot masquerade as legacy receipt");
        malformed=encoded; malformed["chord_input"]["length"]["metres"]=9;
        invalid([&] { (void)decode_construction_receipt(malformed); }, "inconsistent exact chord quantity must reject");
        malformed=encoded; malformed["chord_input"]["heading"]["radians"]=1;
        invalid([&] { (void)decode_construction_receipt(malformed); }, "inconsistent chord heading must reject");
        auto degenerate=input; degenerate.chord_input->length=parse_quantity("0 m");
        invalid([&] { (void)replay_construction_receipt(degenerate,{{0,0}}); }, "zero typed chord must reject");
        degenerate=input; degenerate.chord_input->length=parse_quantity("-1 m");
        invalid([&] { (void)replay_construction_receipt(degenerate,{{0,0}}); }, "negative typed chord must reject");
        degenerate=input; degenerate.chord_end=Vec2{2,0};
        invalid([&] { (void)encode_construction_receipt(degenerate); }, "encoder cannot silently drop duplicate chord authority");
    }
    auto nonarc=chord(BoundaryConstructionKind::arc_chord_angle); nonarc.kind=BoundaryConstructionKind::line_heading;
    invalid([&] { (void)replay_construction_receipt(nonarc,{{0,0}}); }, "typed chord on a non-chord method must reject");
    auto collapsed=chord(BoundaryConstructionKind::arc_chord_angle); collapsed.start={1e100,1e100};
    invalid([&] { (void)replay_construction_receipt(collapsed,{collapsed.start}); }, "precision-losing typed chord cannot fall back to guessed endpoint");
}

void test_sessions_recovery_and_boundary_frames() {
    for (auto kind : {BoundaryConstructionKind::arc_chord_angle,BoundaryConstructionKind::arc_chord_height,BoundaryConstructionKind::arc_chord_length}) {
        BoundaryAuthoringSession session(BoundaryAuthoringMode::draw_first); (void)session.anchor({0,0});
        const auto length=parse_quantity("2000 mm"); const auto heading=parse_angle("0 deg");
        if (kind==BoundaryConstructionKind::arc_chord_angle) (void)session.add_arc_chord_angle(length,heading,parse_angle("90 deg"));
        else if (kind==BoundaryConstructionKind::arc_chord_height) (void)session.add_arc_chord_height(length,heading,parse_quantity("100 cm"));
        else (void)session.add_arc_chord_arc_length(length,heading,parse_quantity("3.141592653589793 m"),true);
        const auto source=session.view();
        require(source.active_chain->receipts.front().chord_input && !source.active_chain->receipts.front().chord_end,
            "session persists typed authority instead of converting to coordinate receipt");
        require(session.undo(), "typed action can undo"); const auto checkpoint=session.recovery_checkpoint();
        require(checkpoint.version==2 && checkpoint.actions.size()>checkpoint.history_position, "typed redo tail protects recovery dialect");
        const auto encoded=encode_boundary_authoring_recovery(checkpoint);
        const auto decoded=decode_boundary_authoring_recovery(encoded); require(decoded.supported(), "typed checkpoint decodes");
        auto restored=BoundaryAuthoringSession::from_recovery_checkpoint(*decoded.checkpoint);
        require(restored.redo() && restored.view()==source, "recovery redo restores exact typed receipt, units, identities and geometry");
        auto downgraded=encoded; downgraded["version"]=1;
        invalid([&] { (void)decode_boundary_authoring_recovery(downgraded); }, "typed recovery cannot use legacy schema");
        auto future=encoded; future["version"]=99;
        require(decode_boundary_authoring_recovery(future).original_envelope==future, "future checkpoint remains opaque exactly");
        const auto before=restored.view();
        invalid([&] { (void)restored.add_arc_chord_angle(parse_quantity("0 m"),heading,parse_angle("90 deg")); }, "invalid session chord rejects atomically");
        require(restored.view()==before, "invalid typed session input does not mutate history or geometry");
        (void)restored.add_closing_segment(); const auto accepted=restored.close_chain();
        const auto envelope=boundary_construction_envelope(accepted); require(envelope.at("version")==4 && envelope.at("transforms").empty(), "typed accepted boundary opts into version four only");
        const auto record=*decode_boundary_receipt_envelope(envelope).record;
        const auto moved=transformed_boundary_construction(record,{{},0,false,false,{4,-3}});
        require(moved.schema_version==4 && moved.edges==record.edges && moved.transforms.size()==1, "world transform preserves typed local inputs");
        require(replay_boundary_construction(moved).edges.front().segment.end.x==6 && replay_boundary_construction(moved).edges.front().segment.end.y==-3,
            "typed boundary replays original local chord before world transform");
        auto old=envelope; old["version"]=3;
        invalid([&] { (void)decode_boundary_receipt_envelope(old); }, "legacy framed schema cannot contain typed chord receipts");
        auto future_boundary=envelope; future_boundary["version"]=99;
        require(decode_boundary_receipt_envelope(future_boundary).original_envelope==future_boundary, "future boundary receipt stays opaque");
    }
    BoundaryAuthoringSession legacy(BoundaryAuthoringMode::draw_first); (void)legacy.anchor({0,0});
    (void)legacy.add_arc_chord_angle(Vec2{2,0},parse_angle("90 deg")); (void)legacy.add_closing_segment();
    require(legacy.recovery_checkpoint().version==1 && boundary_construction_envelope(legacy.close_chain()).at("version")==2,
        "legacy coordinate sessions retain original recovery and boundary dialects");
}

MeasurementLinework typed_stroke() {
    MeasurementLinework model; model.stroke_id="stroke"; model.anchor={0,0};
    model=promoted_measurement_linework_for_typed_chord(model);
    auto receipt=chord(BoundaryConstructionKind::arc_chord_angle); receipt.segment_id="stroke:arc";
    model.edges.push_back({receipt.segment_id,"stroke:v0","stroke:v1",receipt}); return model;
}
void test_measured_operations_and_compatibility() {
    const auto original=typed_stroke(); const auto encoded=encode_measurement_linework_model(original);
    require(encoded.at("version")==4 && encoded.at("replay_version")==4 && encoded.at("operations").empty(), "empty live candidate promotes to typed measured dialect before first edge");
    require(encode_measurement_linework_model(*decode_measurement_linework_model(encoded).model)==encoded, "typed measured model round trips exactly");
    const auto moved=transformed_measurement_linework(original,{{},0,false,false,{4,-3}});
    require(moved.schema_version==4 && moved.edges==original.edges && moved.operations.size()==1 &&
        replay_measurement_linework(moved).edges.front().segment.end.x==6, "typed measured transform keeps original local receipt evidence");
    BoundaryGeometryEdit edit; edit.kind=BoundaryGeometryEditKind::resize_segment; edit.boundary_id="stroke";
    edit.target_id="stroke:arc"; edit.target_length_metres=2*segment_length(replay_measurement_linework(moved).edges.front().segment);
    const auto changed=edited_measurement_linework(moved,edit);
    require(changed.schema_version==4 && changed.edges==original.edges && changed.operations.size()==2 &&
        replay_measurement_linework(changed).edges.front().segment.end.x==8, "typed measured edit remains ordered after world transform");
    auto downgraded=encoded; downgraded["version"]=3; downgraded["replay_version"]=3;
    invalid([&] { (void)decode_measurement_linework_model(downgraded); }, "typed model cannot use legacy measured dialect");
    auto future=encoded; future["version"]=99;
    require(decode_measurement_linework_model(future).original_model==future, "unknown measured dialect stays opaque exactly");
    MeasurementLinework legacy; legacy.stroke_id="legacy";
    ConstructionReceipt receipt; receipt.segment_id="legacy:e"; receipt.kind=BoundaryConstructionKind::line_to_point; receipt.start={0,0}; receipt.chord_end=Vec2{2,0};
    legacy.edges.push_back({receipt.segment_id,"legacy:v0","legacy:v1",receipt});
    const auto legacy_json=encode_measurement_linework_model(legacy);
    require(legacy_json.at("version")==1 && !legacy_json.at("segments")[0].at("receipt").contains("version"), "legacy receipt and model bytes retain unversioned input dialect");
    auto framed=transformed_measurement_linework(legacy,{{},0,false,false,{3,4}});
    const auto promoted=promoted_measurement_linework_for_typed_chord(framed);
    require(promoted.transforms.empty() && promoted.operations.size()==1 && promoted.edges==framed.edges &&
        replay_measurement_linework(promoted).edges.front().segment.end.x==5, "v2 transforms migrate in order without changing input receipts");
    auto edited=edited_measurement_linework(promoted,BoundaryGeometryEdit{.boundary_id="legacy",.kind=BoundaryGeometryEditKind::move_vertex,.target_id="legacy:v1",.target_position={6,4}});
    require(edited.operations.size()==2 && encode_measurement_linework_model(*decode_measurement_linework_model(encode_measurement_linework_model(edited)).model)==encode_measurement_linework_model(edited), "promoted operations retain exact serialization through edits");
}

Document document_with_typed_stroke() {
    const auto model=typed_stroke();
    return Document::create({{"p","property",nlohmann::json::object(),false},{"b","building",{{"property_id","p"}},false},
        {"f","floor",{{"building_id","b"}},false},{"l","layer",{{"floor_id","f"}},false},
        {"stroke","measurement_linework",{{"property_id","p"},{"building_id","b"},{"floor_id","f"},{"layer_id","l"},{"model",encode_measurement_linework_model(model)}},true}});
}

Document document_with_archived_typed_boundary() {
    BoundaryAuthoringSession session(BoundaryAuthoringMode::draw_first); (void)session.anchor({0,0});
    (void)session.add_arc_chord_angle(parse_quantity("2 m"),parse_angle("0 deg"),parse_angle("90 deg"));
    (void)session.add_closing_segment(); const auto accepted=session.close_chain();
    auto entity=encode_identified_boundary_entity(accepted.boundary);
    entity.properties["boundary_authoring"]=boundary_construction_envelope(accepted);
    auto source=Document::create({entity});
    BoundaryGeometryEdit edit; edit.boundary_id=entity.id; edit.kind=BoundaryGeometryEditKind::move_vertex;
    edit.target_id=accepted.boundary.segments.front().end_vertex_id; edit.target_position={3,0};
    source.apply(EditBoundaryGeometry{.expected_revision=0,.edit=edit});
    const auto derived=source.snapshot().entities().at(entity.id);
    require(!derived.properties.contains("boundary_authoring") &&
        derived.extensions.at("boundary_geometry_derivation").at("source_boundary_authoring").at("version")==4,
        "ordinary typed boundary edit archives its original typed receipt proof");
    return Document::create({derived});
}

Document document_with_typed_replacement_proof() {
    BoundaryAuthoringSession legacy(BoundaryAuthoringMode::draw_first); (void)legacy.anchor({0,0});
    (void)legacy.add_line_to({2,0}); (void)legacy.add_line_to({2,2}); (void)legacy.add_line_to({0,2});
    (void)legacy.add_closing_segment(); const auto original=legacy.close_chain();
    auto entity=encode_identified_boundary_entity(original.boundary);
    entity.properties["boundary_authoring"]=boundary_construction_envelope(original);
    BoundaryAuthoringSession typed(BoundaryAuthoringMode::draw_first); (void)typed.anchor({0,0});
    (void)typed.add_arc_chord_angle(parse_quantity("2 m"),parse_angle("0 deg"),parse_angle("90 deg"));
    (void)typed.add_line_to({2,2}); (void)typed.add_line_to({0,2}); (void)typed.add_closing_segment();
    const auto replacement_chain=typed.close_chain();
    auto replacement=original.boundary;
    for (std::size_t index=0;index<replacement.segments.size();++index)
        replacement.segments[index].segment=replacement_chain.boundary.segments[index].segment;
    BoundaryGeometryEdit edit; edit.boundary_id=edit.target_id=entity.id;
    edit.kind=BoundaryGeometryEditKind::redefine_boundary;
    edit.replacement_segments=encode_identified_boundary_entity(replacement).properties.at("segments");
    edit.replacement_authoring=boundary_construction_envelope(replacement_chain);
    auto history=Document::create({entity}); history.apply(EditBoundaryGeometry{.expected_revision=0,.edit=edit});
    const auto derived=history.snapshot().entities().at(entity.id);
    const auto& proof=derived.extensions.at("boundary_geometry_derivation");
    require(!derived.properties.contains("boundary_authoring") && proof.at("source_boundary_authoring").at("version")==2 &&
        proof.at("operations")[0].at("value").at("replacement_authoring").at("version")==4,
        "typed redraw proof is retained in an operation with a legacy source receipt");
    history.undo(history.revision());
    require(ProjectStore::required_format_version(history.snapshot())==31, "undone typed replacement command preserves native floor");
    history.apply(ApplyEntityChanges{.expected_revision=history.revision(),.entity_changes={EntityChange::erase(entity.id)},.message="delete redrawn boundary"});
    require(history.snapshot().entities().empty() && ProjectStore::required_format_version(history.snapshot())==31,
        "typed replacement proof retained only in deleted owner history preserves native floor");
    auto imported=Document::create({derived});
    require(imported.snapshot().history().size()==1 && ProjectStore::required_format_version(imported.snapshot())==31,
        "fresh derived owner with only typed replacement operation preserves native floor");
    return imported;
}
void test_native_storage_retained_history_and_wall_metadata() {
    auto document=document_with_typed_stroke(); const auto source=document.snapshot();
    require(ProjectStore::required_format_version(source)==31, "imported typed model requires native thirty one without originating commands");
    document.apply(ApplyEntityChanges{.expected_revision=document.revision(),.entity_changes={EntityChange::erase("stroke")},.message="remove typed fixture"});
    require(ProjectStore::required_format_version(document.snapshot())==31, "typed chord survives format qualification when retained only in history");
    const auto temporary=std::filesystem::temp_directory_path()/std::filesystem::path("vertex-typed-chord-"+make_stable_id());
    const auto cleanup=[&] {
        const auto target=std::filesystem::absolute(temporary).lexically_normal();
        const auto root=std::filesystem::absolute(std::filesystem::temp_directory_path()).lexically_normal();
        require(target==(root/target.filename()).lexically_normal() && target.filename().string().starts_with("vertex-typed-chord-"),
            "temporary cleanup stays within the test-owned temporary directory");
        std::filesystem::remove_all(target);
    };
    std::filesystem::create_directories(temporary);
    try {
        const auto path=temporary/"typed.bldproj"; (void)ProjectStore::save(path,document.snapshot());
        const auto reopened=ProjectStore::load(path);
        require(reopened.document.snapshot().entities()==document.snapshot().entities() && ProjectStore::required_format_version(reopened.document.snapshot())==31,
            "native thirty one reopens exact head and retained typed history");
        const auto extraction=temporary/"extracted"; extract_project(document.snapshot(),extraction);
        std::ifstream stream(extraction/"project.json"); nlohmann::json manifest; stream>>manifest;
        require(manifest.at("exchange_version")==29, "typed retained history selects logical extraction twenty nine");
        auto derived=document_with_archived_typed_boundary();
        require(derived.snapshot().history().size()==1 && ProjectStore::required_format_version(derived.snapshot())==31,
            "fresh imported derived boundary retains native typed chord floor without prior history");
        const auto derived_path=temporary/"archived-source.bldproj";
        (void)ProjectStore::save(derived_path,derived.snapshot());
        const auto derived_reopened=ProjectStore::load(derived_path);
        require(derived_reopened.document.snapshot().entities()==derived.snapshot().entities() &&
            ProjectStore::required_format_version(derived_reopened.document.snapshot())==31,
            "native archive source alone round trips exact typed boundary proof");
        const auto derived_extraction=temporary/"derived-extracted";
        extract_project(derived.snapshot(),derived_extraction);
        std::ifstream derived_stream(derived_extraction/"project.json"); nlohmann::json derived_manifest; derived_stream>>derived_manifest;
        require(derived_manifest.at("exchange_version")==29, "archived typed boundary source alone selects extraction twenty nine");
        auto replacement=document_with_typed_replacement_proof();
        const auto replacement_path=temporary/"typed-replacement.bldproj";
        (void)ProjectStore::save(replacement_path,replacement.snapshot());
        require(ProjectStore::load(replacement_path).document.snapshot().entities()==replacement.snapshot().entities(),
            "typed replacement operation alone reopens exact native entity");
        const auto replacement_extraction=temporary/"replacement-extracted";
        extract_project(replacement.snapshot(),replacement_extraction);
        std::ifstream replacement_stream(replacement_extraction/"project.json"); nlohmann::json replacement_manifest; replacement_stream>>replacement_manifest;
        require(replacement_manifest.at("exchange_version")==29, "typed replacement operation alone selects extraction twenty nine");
    } catch (...) { cleanup(); throw; }
    cleanup();
    auto receipt=chord(BoundaryConstructionKind::arc_chord_angle);
    const nlohmann::json context={{"version",1},{"expected_start",{0,0}},{"previous_segment",nullptr},{"closure_anchor",nullptr},{"tolerance_metres",default_geometry_tolerance_metres}};
    Entity wall{"wall","wall",{{"baseline",{{"start",{8,9}},{"end",{10,9}},{"sweep_radians",std::numbers::pi/2}}},
        {"thickness",0.14},{"height",2.4},{"original_drawing_input",encode_construction_receipt(receipt)},{"original_drawing_input_context",context}},false};
    auto historical=Document::create({wall});
    require(ProjectStore::required_format_version(historical.snapshot())==31, "historical typed wall input requires native floor and replays original local frame");
    auto malformed=wall; malformed.properties["original_drawing_input"]["chord_input"]["length"]["metres"]=10;
    rejected([&] { historical.apply(ApplyEntityChanges{.expected_revision=historical.revision(),.entity_changes={EntityChange::upsert(malformed)},.message="malformed typed provenance"}); }, "malformed known typed wall receipt must reject");
    require(historical.revision()==0 && historical.snapshot().entities().at("wall")==wall, "invalid historical typed metadata rejects atomically");
    auto future=wall; future.properties["original_drawing_input"]["version"]=99;
    require(Document::create({future}).snapshot().entities().at("wall")==future, "future historical wall receipt metadata remains opaque");
    future=wall; future.properties["original_drawing_input_context"]["version"]=std::numeric_limits<std::uint64_t>::max();
    require(Document::create({future}).snapshot().entities().at("wall")==future, "future unsigned context stays opaque without signed narrowing");
    const auto reject_wall=[&](Entity candidate) {
        rejected([&] { (void)Document::create({candidate}); }, "malformed known typed wall rejects at document creation");
        const auto before=historical.snapshot();
        rejected([&] { historical.apply(ApplyEntityChanges{.expected_revision=historical.revision(),
            .entity_changes={EntityChange::upsert(candidate)},.message="invalid future-context typed input"}); },
            "malformed known typed wall rejects despite future context");
        require(historical.revision()==before.revision() && historical.snapshot().entities()==before.entities(),
            "typed wall validation rejection leaves the document atomic");
    };
    malformed=future; malformed.properties["original_drawing_input"]["chord_input"]["length"]["metres"]=10;
    reject_wall(malformed);
    malformed=future; malformed.properties["original_drawing_input"]["chord_end"]={2,0}; reject_wall(malformed);
    malformed=future; malformed.properties["original_drawing_input"].erase("version"); reject_wall(malformed);
    malformed=future; malformed.properties["original_drawing_input"]["version"]=1; reject_wall(malformed);
    malformed=future; malformed.properties["original_drawing_input"]["version"]="2"; reject_wall(malformed);
    auto unknown=future; unknown.properties["original_drawing_input"]["version"]=99;
    unknown.properties["original_drawing_input"]["chord_input"]["length"]["metres"]=10;
    require(Document::create({unknown}).snapshot().entities().at("wall")==unknown,
        "genuinely unknown positive receipt dialect remains opaque");
}

void test_typed_arc_reconstruction_and_input_budgets() {
    IdentifiedBoundary square{"square","measurement_boundary",{
        {"e0","v0","v1",{{0,0},{2,0},0}}, {"e1","v1","v2",{{2,0},{2,2},0}},
        {"e2","v2","v3",{{2,2},{0,2},0}}, {"e3","v3","v0",{{0,2},{0,0},0}}}};
    auto receipt=chord(BoundaryConstructionKind::arc_chord_angle); receipt.segment_id="e0";
    const auto curved=reconstruct_boundary_arc(square,"e0",receipt);
    require(curved.segments.front().segment.end.x==2 && curved.segments.front().segment.end.y==0 &&
        curved.segments.front().segment.sweep_radians==parse_angle("90 deg").radians,
        "typed reconstruction proves the selected chord from exact typed inputs");
    require(std::abs(signed_area(boundary_geometry(curved))-(3+std::numbers::pi/2))<1e-12,
        "typed chord curvature changes analytical area without tessellation");
    auto mismatch=receipt; mismatch.chord_input->length=parse_quantity("3 m");
    invalid([&] { (void)reconstruct_boundary_arc(square,"e0",mismatch); }, "typed reconstruction cannot change the selected chord");
    BoundaryGeometryEdit edit; edit.boundary_id="square"; edit.kind=BoundaryGeometryEditKind::reconstruct_arc;
    edit.target_id="e0"; edit.arc_construction=receipt;
    const auto encoded=encode_boundary_geometry_edit(edit);
    require(encoded.at("version")==1 && encoded.at("construction").at("version")==2 &&
        decode_boundary_geometry_edit(encoded)==edit, "nested receipt discriminator preserves the existing edit envelope dialect");
    auto document=Document::create({encode_identified_boundary_entity(square)});
    document.apply(EditBoundaryGeometry{.expected_revision=0,.edit=edit});
    require(ProjectStore::required_format_version(document.snapshot())==31, "typed arc reconstruction command and derivation require the new native floor");
    auto limits=boundary_authoring_default_resource_policy;
    limits.max_string_bytes=5000;
    BoundaryAuthoringSession bounded(BoundaryAuthoringMode::draw_first,{},limits); (void)bounded.anchor({0,0});
    const auto before=bounded.view(); const auto usage=bounded.resource_usage();
    const auto long_length=parse_quantity(std::string(6000,' ')+"2 m");
    invalid([&] { (void)bounded.add_arc_chord_angle(long_length,parse_angle("0 deg"),parse_angle("90 deg")); },
        "typed chord expressions participate in live authoring string budgets");
    require(bounded.view()==before && bounded.resource_usage()==usage, "typed input budget rejection is atomic");
}
}
int main() {
    sketch::testing::noninteractive_errors();
    try {
        test_typed_chord_is_authoritative_and_replayable(); test_three_typed_methods_exact_units_and_rejections();
        test_sessions_recovery_and_boundary_frames(); test_measured_operations_and_compatibility();
        test_native_storage_retained_history_and_wall_metadata();
        test_typed_arc_reconstruction_and_input_budgets();
    }
    catch (const std::exception& error) { std::cerr<<"typed_chord_receipt_tests: "<<error.what()<<'\n';return 1; }
    std::cout<<"Typed chord receipt tests passed\n";return 0;
}
