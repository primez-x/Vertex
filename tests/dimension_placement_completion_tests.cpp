#include "support/detached_document_snapshot.hpp"
#include "sketch/boundary_dimension.hpp"
#include "sketch/constraint_authoring.hpp"
#include "sketch/project_exchange.hpp"
#include "sketch/project_store.hpp"
#include "sketch/wall_measurement.hpp"
#include "support/noninteractive_errors.hpp"
#include <sqlite3.h>
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>

namespace sketch {
std::map<std::string, Entity, std::less<>> boundary_constraint_entities(
    const std::map<std::string, Entity, std::less<>>& source,
    const ApplyBoundaryConstraintChanges& command, bool retained_replay = false);
}

namespace {
using namespace sketch;
using Json = nlohmann::json;
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
template<class F> void rejects(F function, const char* message) {
    try { function(); } catch (const std::exception&) { return; }
    throw std::runtime_error(message);
}
Entity wall(const char* id, Vec2 start, Vec2 end) {
    return {id, "wall", {{"baseline", {{"start", {start.x,start.y}}, {"end", {end.x,end.y}}, {"sweep_radians",0}}},
        {"thickness_m",0.2}, {"height_m",3}, {"elevation_m",0}, {"property_id","property"},
        {"building_id","building"}, {"floor_id","floor"}, {"layer_id","layer"}}};
}
Document fixture(bool exterior = true) {
    std::vector<Entity> entities{{"property","property",{{"calculation_workflow","measurement"}}},
        {"building","building",{{"property_id","property"}}}, {"floor","floor",{{"building_id","building"}}},
        {"layer","layer",{{"floor_id","floor"}}}, wall("bottom",{0,0},{4,0}), wall("right",{4,0},{4,3}),
        wall("top",{4,3},{0,3}), wall("left",{0,3},{0,0}),
        {"note","label",{{"text","Keep this note"},{"position",{2,1}}},false}};
    auto initial = Document::create(entities);
    const auto measured = derive_exterior_wall_measurement(initial.snapshot(), {"bottom","right","top","left"});
    IdentifiedBoundary boundary{"area","measurement_boundary",{}};
    for (std::size_t i=0;i<measured.boundary.size();++i)
        boundary.segments.push_back({"edge-"+std::to_string(i), "vertex-"+std::to_string(i),
            "vertex-"+std::to_string((i+1)%measured.boundary.size()), measured.boundary[i]});
    auto owner = encode_identified_boundary_entity(boundary);
    for (const auto* key : {"property_id","building_id","floor_id","layer_id"}) owner.properties[key] = entities[4].properties.at(key);
    if (exterior) owner.properties["wall_measurement_source"] = measured.source;
    entities.push_back(owner);
    for (const auto* id : {"manual","automatic","unselected"}) {
        BoundaryDimension dimension{id,"area",boundary.segments.front().segment_id,{2,-0.5}};
        if (std::string(id)!="manual") { dimension.placement=BoundaryDimensionPlacement::automatic; dimension.automatic_placement_version=2; }
        dimension.presentation=BoundaryDimensionPresentation{3.2,"#123456",true,true,true,0.25};
        auto entity=encode_boundary_dimension_entity(dimension);
        entity.extensions["vendor"]={{"retain",17}}; entity.properties["vendor"]="source metadata";
        entities.push_back(entity);
    }
    const std::vector<std::string> walls{"bottom","right","top","left"};
    for (std::size_t i=0;i<walls.size();++i)
        entities.push_back(encode_constraint_entity({"join-"+std::to_string(i),ConstraintRelationKind::coincident,
            {{walls[i],WallEndpointRole::end},{walls[(i+1)%walls.size()],WallEndpointRole::start}}, {}, {}}));
    return Document::create(entities);
}
ApplyBoundaryConstraintChanges command_for(const DocumentSnapshot& source, bool two = false) {
    ConstraintAuthoringIntent intent; intent.message="Move partial perimeter and callouts";
    WallGeometryMoveIntent move{{{"bottom",{0.4,0.2},{4.4,0.2}}},true};
    if (two) move.targets.push_back({"right",{4.4,0.2},{4.4,3.2}});
    intent.wall_geometry_move=move;
    const auto preview=preview_constraint_authoring(source,intent);
    if(!preview.accepted()) {
        std::string reason="Connected partial-wall fixture refused:";
        for(const auto& diagnostic:preview.diagnostics())reason+="\n"+diagnostic;
        throw std::runtime_error(reason);
    }
    auto candidate=Document::fork(source); (void)apply_constraint_authoring(candidate,preview);
    auto command=*candidate.snapshot().history().back().boundary_constraint_changes;
    command.dimension_placement_completion=true;
    command.dimension_placement_moves={{"manual",{0.4,0.2}},{"automatic",{0.4,0.2}}};
    return command;
}
void exact_placement(const DocumentSnapshot& before, const DocumentSnapshot& after) {
    for (const auto* id : {"manual","automatic"}) {
        const auto old=*decode_boundary_dimension_entity(before.entities().at(id)).dimension;
        const auto current=*decode_boundary_dimension_entity(after.entities().at(id)).dimension;
        require(current.text_position.x==old.text_position.x+0.4 && current.text_position.y==old.text_position.y+0.2,
            "selected text position must use original source plus explicit delta after automatic reflow");
        require(current.placement==BoundaryDimensionPlacement::manual && !current.automatic_placement_version,
            "explicit placement must clear automatic placement authority");
        require(current.presentation==old.presentation && after.entities().at(id).extensions==before.entities().at(id).extensions &&
            after.entities().at(id).properties.at("vendor")=="source metadata", "saved callout style and opaque metadata must survive");
    }
}
std::string digest(const DocumentSnapshot& snapshot, unsigned format, const Json* altered=nullptr) {
    Json manifest{{"format_version",format},{"document_id",snapshot.document_id()},{"head_revision",snapshot.revision()},
        {"saved_revision",snapshot.revision()},{"named_revisions",snapshot.named_revisions()},{"history",Json::array()}};
    for (const auto& revision : snapshot.history()) {
        Json row{{"revision",revision.revision},{"parent_revision",revision.parent_revision},{"source_revision",revision.source_revision},
            {"action",revision.action},{"name",revision.name},{"undo_stack",revision.undo_stack},{"redo_stack",revision.redo_stack},
            {"entities",Json::array()},{"assets",Json::array()}};
        require(revision.assets.empty() && !revision.boundary_geometry_edit && !revision.boundary_translation &&
            !revision.boundary_transform && !revision.boundary_translations && !revision.boundary_transforms,"digest fixture only carries placement proof");
        if (revision.boundary_constraint_changes) row["boundary_constraint_changes"]=altered ? *altered : command_to_json(Command{*revision.boundary_constraint_changes});
        for (const auto& [id,entity] : revision.entities)
            row["entities"].push_back({{"id",id},{"type",entity.type},{"required",entity.required},{"properties",entity.properties},{"extensions",entity.extensions}});
        manifest["history"].push_back(std::move(row));
    }
    const auto text=manifest.dump(); return sha256_hex(std::as_bytes(std::span<const char>(text.data(),text.size())));
}
void forge(const std::filesystem::path& path, const DocumentSnapshot& snapshot, unsigned format, const Json* proof=nullptr) {
    sqlite3* database{}; require(sqlite3_open(path.string().c_str(),&database)==SQLITE_OK,"open test-owned placement database");
    if (proof) {
        sqlite3_stmt* statement{};
        require(sqlite3_prepare_v2(database,"UPDATE revisions SET boundary_constraint_changes_json=? WHERE boundary_constraint_changes_json IS NOT NULL",-1,&statement,nullptr)==SQLITE_OK,"prepare test-owned proof rewrite");
        const auto text=proof->dump(); sqlite3_bind_text(statement,1,text.c_str(),-1,SQLITE_TRANSIENT);
        const auto result=sqlite3_step(statement); sqlite3_finalize(statement); require(result==SQLITE_DONE,"replace test-owned proof");
    }
    const auto sql="PRAGMA user_version="+std::to_string(format)+"; UPDATE metadata SET value='"+std::to_string(format)+
        "' WHERE key='format_version'; UPDATE metadata SET value='"+digest(snapshot,format,proof)+"' WHERE key='logical_digest';";
    const auto result=sqlite3_exec(database,sql.c_str(),nullptr,nullptr,nullptr); sqlite3_close(database);
    require(result==SQLITE_OK,"forge independently valid digest and floor");
}
void stored_refuses(const std::filesystem::path& path, StorageErrorCode code) {
    const auto fingerprint=ProjectStore::file_sha256(path); bool refused{};
    try { (void)ProjectStore::load(path); } catch (const StorageError& error) { refused=error.code()==code; }
    require(refused && ProjectStore::file_sha256(path)==fingerprint,"forged stored proof must refuse without modifying bytes");
}
void workflow(const std::filesystem::path& root, bool two, bool exterior, bool presentation = false) {
    auto document=fixture(exterior); const auto before=document.snapshot(); auto command=command_for(before,two);
    if(presentation) {
        auto note=before.entities().at("note");note.properties["position"]={2.4,1.2};
        command.supplemental_source_completion=true;
        command.supplemental_entity_changes.push_back(EntityChange::upsert(note));
    }
    auto geometry=command; geometry.dimension_placement_moves.clear(); geometry.dimension_placement_completion=false;
    geometry.supplemental_entity_changes.clear();geometry.supplemental_source_completion=false;
    const auto ordinary=Document::preview_command(before,geometry);
    const auto encoded=command_to_json(Command{command});
    require(encoded.at("version")==15 && encoded.at("dimension_placement_completion")==true,"typed placement requires envelope15");
    require(encoded.at("source_completion")==exterior,"placement supplements must not infer exterior authority");
    require(command_to_json(command_from_json(encoded))==encoded,"envelope15 must roundtrip exactly");
    const auto preview=Document::preview_command(before,command);
    require(document.snapshot().entities()==before.entities(),"preview must preserve the source");
    exact_placement(before,preview);
    if(!two)require(preview.entities().at("top")==before.entities().at("top"),"Unchanged joined wall retains its exact original geometry and metadata");
    if(presentation)require(preview.entities().at("note").properties.at("position")==Json::array({2.4,1.2}) &&
        preview.entities().at("note").properties.at("text")=="Keep this note","selected ordinary presentation must move with plain wall and callout");
    require(preview.entities().at("unselected")==ordinary.entities().at("unselected"),"unselected automatic callout must retain ordinary source reflow");
    document.apply(command); const auto committed=document.snapshot(); exact_placement(before,committed);
    require(committed.revision()==before.revision()+1 && preview.entities()==committed.entities(),"geometry and callouts must commit one exact preview revision");
    rejects([&]{document.apply(command);},"stale placement command must refuse");
    document.undo(document.revision()); require(document.snapshot().entities()==before.entities(),"Undo must restore geometry and placement together");
    require(ProjectStore::required_format_version(document.snapshot())==41,"Undo must retain native41 placement history");
    document.redo(document.revision()); require(document.snapshot().entities()==committed.entities(),"Redo must restore geometry and placement together");
    const auto path=root/(make_stable_id()+".sketch"); (void)ProjectStore::save(path,document.snapshot());
    const auto loaded=ProjectStore::load(path).document.snapshot();
    require(loaded.is_editable() && loaded.entities()==document.snapshot().entities() && loaded.history().size()==document.snapshot().history().size(),"native41 must independently replay and reopen complete history");
    if(const auto* capture_root=std::getenv("VERTEX_TEST_CAPTURE_DIR"); capture_root && *capture_root) {
        const auto directory=std::filesystem::path(capture_root);
        std::filesystem::create_directories(directory);
        const auto name=std::string("placement-")+(two?"two":"one")+(exterior?"-exterior":"-plain")+(presentation?"-note":"")+".bldproj";
        std::filesystem::copy_file(path,directory/name);
    }
    const auto extraction=root/make_stable_id(); extract_project(loaded,extraction);
    std::ifstream input(extraction/"project.json"); require(Json::parse(input).at("exchange_version")==39,"native41 history maps to extraction39");
    const auto snapshot=document.snapshot();
    forge(path,snapshot,40); stored_refuses(path,StorageErrorCode::unsupported_format);
    auto tampered=encoded; tampered["dimension_placement_moves"][0]["offset"][0]=0.6;
    const auto proof_path=root/(make_stable_id()+".sketch");
    (void)ProjectStore::save(proof_path,snapshot); forge(proof_path,snapshot,41,&tampered); stored_refuses(proof_path,StorageErrorCode::integrity_failure);
    sketch::test::DetachedDocumentSnapshotFixture stripped(snapshot);
    auto& retained=stripped.history()[1].boundary_constraint_changes;
    retained->dimension_placement_moves.clear();
    require(ProjectStore::required_format_version(stripped)==41,"stripped retained moves cannot lower the marker's storage floor");
    auto deletion=Document::fork(snapshot);
    deletion.apply(ApplyEntityChanges{deletion.revision(),{EntityChange::erase("manual"),EntityChange::erase("automatic")},{},"Delete selected callouts"});
    require(ProjectStore::required_format_version(deletion.snapshot())==41,"deleting selected callouts cannot lower retained proof floor");
}
void refusal() {
    auto document=fixture(); const auto before=document.snapshot(); const auto valid=command_for(before);
    auto check=[&](const ApplyBoundaryConstraintChanges& command) {
        rejects([&]{(void)Document::preview_command(before,command);},"invalid completion must refuse preview");
        rejects([&]{document.apply(command);},"invalid completion must refuse commit");
        require(document.snapshot().entities()==before.entities() && document.revision()==before.revision(),"refusal must be atomic");
    };
    auto changed=valid; changed.dimension_placement_moves.push_back(changed.dimension_placement_moves.front()); check(changed);
    changed=valid; changed.dimension_placement_moves.front().dimension_id="missing"; check(changed);
    changed=valid; changed.dimension_placement_moves.front().dimension_id="bottom"; check(changed);
    changed=valid; changed.dimension_placement_moves.front().offset.x=std::numeric_limits<double>::infinity(); check(changed);
    changed=valid; changed.dimension_placement_moves.front().offset.y=std::numeric_limits<double>::quiet_NaN(); check(changed);
    changed=valid; changed.dimension_placement_completion=false; check(changed);
    changed=valid; changed.dimension_placement_moves.clear();
    require(command_to_json(Command{changed}).at("version")==15,"stripped vector must retain envelope15 discriminator"); check(changed);
    changed=valid; changed.entity_changes.push_back(EntityChange::upsert(before.entities().at("manual"))); check(changed);
    changed=valid; changed.supplemental_source_completion=true;
    changed.supplemental_entity_changes.push_back(EntityChange::upsert(before.entities().at("manual"))); check(changed);
    auto encoded=command_to_json(Command{valid}); encoded["dimension_placement_moves"][0]["surprise"]=1;
    rejects([&]{(void)command_from_json(encoded);},"placement lane must reject unknown fields");
    encoded=command_to_json(Command{valid}); encoded["version"]=11;
    rejects([&]{(void)command_from_json(encoded);},"older dialect cannot borrow placement authority");
    encoded=command_to_json(Command{valid}); encoded["dimension_placement_completion"]=false;
    rejects([&]{(void)command_from_json(encoded);},"placement mode must agree with its explicit lane");
}
void overflow_and_locks() {
    auto document=fixture(false); auto entity=document.snapshot().entities().at("manual");
    auto dimension=*decode_boundary_dimension_entity(entity).dimension;
    dimension.text_position.x=std::numeric_limits<double>::max(); entity=encode_boundary_dimension_entity(dimension,&entity);
    document.apply(ApplyEntityChanges{document.revision(),{EntityChange::upsert(entity)},{},"Place finite far-away callout"});
    const auto before=document.snapshot(); auto command=command_for(before);
    command.dimension_placement_moves.front().offset.x=std::numeric_limits<double>::max();
    rejects([&]{document.apply(command);},"finite offsets must refuse an overflowing final position");
    require(document.snapshot().entities()==before.entities(),"overflow must refuse atomically");
    auto locked=fixture();
    PersistentConstraint pin{"pin",ConstraintRelationKind::fixed_anchor,{{"bottom",WallEndpointRole::start}}, {}, Vec2{0,0}};
    locked.apply(ApplyEntityChanges{locked.revision(),{EntityChange::upsert(encode_constraint_entity(pin))},{},"Pin selected wall"});
    ConstraintAuthoringIntent move; move.wall_geometry_move=WallGeometryMoveIntent{{{"bottom",{0.4,0.2},{4.4,0.2}}},true};
    require(!preview_constraint_authoring(locked.snapshot(),move).accepted(),"selected callout cannot make contradictory wall locks movable");
    auto borrowed=command_for(fixture().snapshot()); borrowed.expected_revision=locked.revision();
    const auto locked_before=locked.snapshot(); rejects([&]{locked.apply(borrowed);},"placement cannot authorize a wall proof contradicting a retained lock");
    require(locked.snapshot().entities()==locked_before.entities(),"contradictory lock must refuse atomically");
    auto near_pins=fixture(false);const auto pin_source=near_pins.snapshot();
    ConstraintAuthoringIntent pinned_move;
    pinned_move.wall_geometry_move=WallGeometryMoveIntent{{{"top",{4.4,3.2},{0.4,3.2}}},true};
    pinned_move.relation_mutations.push_back(ConstraintRelationMutation::upsert(
        PersistentConstraint{"first-exact-pin",ConstraintRelationKind::fixed_anchor,{{"bottom",WallEndpointRole::start}},{},Vec2{0,0}}));
    pinned_move.relation_mutations.push_back(ConstraintRelationMutation::upsert(
        PersistentConstraint{"second-exact-pin",ConstraintRelationKind::fixed_anchor,{{"left",WallEndpointRole::end}},{},Vec2{0,1e-12}}));
    const auto pin_preview=preview_constraint_authoring(pin_source,pinned_move);
    require(!pin_preview.accepted() && std::any_of(pin_preview.diagnostics().begin(),pin_preview.diagnostics().end(),
        [](const auto& diagnostic){return diagnostic.find("incompatible exact fixed positions")!=std::string::npos;}),
        "Different exact pins on a declared joint refuse rather than moving an anchor within tolerance");
    require(near_pins.snapshot().entities()==pin_source.entities(),"Near-pin conflict cannot alter source geometry");
}
void analytical_targets() {
    const auto before=fixture(false).snapshot(); auto command=command_for(before);
    auto invalid=before.entities(); invalid.at("manual").properties.at("target")["segment_id"]="missing-edge";
    rejects([&]{(void)boundary_constraint_entities(invalid,command);},"invalid original analytical target must refuse typed placement");
    invalid=before.entities(); invalid.at("manual").properties["dimension_version"]=99;
    rejects([&]{(void)boundary_constraint_entities(invalid,command);},"unsupported dimension source must refuse typed placement");
    invalid=before.entities(); invalid.erase("area");
    rejects([&]{(void)boundary_constraint_entities(invalid,command);},"missing analytical owner must refuse typed placement");
    const auto old=decode_identified_boundary_entity(before.entities().at("area")); auto fresh=old;
    for (std::size_t i=0;i<fresh.segments.size();++i) {
        fresh.segments[i].segment_id="fresh-edge-"+std::to_string(i);
        fresh.segments[i].start_vertex_id="fresh-vertex-"+std::to_string(i);
        fresh.segments[i].end_vertex_id="fresh-vertex-"+std::to_string((i+1)%fresh.segments.size());
    }
    BoundaryGeometryEdit edit; edit.boundary_id="area"; edit.target_id="area";
    edit.kind=BoundaryGeometryEditKind::redefine_boundary; edit.fresh_topology=true;
    edit.replacement_segments=encode_identified_boundary_entity(fresh).properties.at("segments");
    edit.replacement_child_mapping={{"segments",{{old.segments.front().segment_id,fresh.segments.front().segment_id}}},{"vertices",Json::object()}};
    for (std::size_t i=0;i<fresh.segments.size();++i) edit.replacement_dimension_ids.push_back("fresh-dimension-"+std::to_string(i));
    auto geometry=command; geometry.dimension_placement_moves.clear(); geometry.dimension_placement_completion=false;
    geometry.boundary_edits.push_back(edit);
    const auto candidate=Document::preview_command(before,geometry);
    require(candidate.entities().contains("manual") && !candidate.entities().contains("automatic"),"fresh topology fixture must remap manual targets and retire automatic labels");
    command.boundary_edits.push_back(edit); command.dimension_placement_moves={{"manual",{0.4,0.2}}};
    rejects([&]{(void)Document::preview_command(before,command);},"placement must reject a surviving callout whose stable target was remapped");
    command.dimension_placement_moves={{"automatic",{0.4,0.2}}};
    rejects([&]{(void)Document::preview_command(before,command);},"placement must reject a callout retired by candidate geometry");
    command.boundary_edits.clear(); command.dimension_placement_moves={{"manual",{0.4,0.2}}};
    command.wall_split=WallSplitIntent{"bottom","second",0.5,"seam",{}};
    rejects([&]{(void)command_to_json(Command{command});},"placement cannot borrow split intent authority");
    command.wall_split.reset(); command.exterior_corner_move=ExteriorCornerMoveIntent{};
    rejects([&]{(void)command_to_json(Command{command});},"placement cannot borrow corner intent authority");
    command.exterior_corner_move.reset(); command.exterior_segment_resize=ExteriorSegmentResizeIntent{};
    rejects([&]{(void)command_to_json(Command{command});},"placement cannot borrow resize intent authority");
    command.exterior_segment_resize.reset(); command.exterior_segment_arc=ExteriorSegmentArcIntent{};
    rejects([&]{(void)command_to_json(Command{command});},"placement cannot borrow arc intent authority");
}
}
int main() {
    testing::noninteractive_errors();
    const auto root=std::filesystem::temp_directory_path()/("vertex-placement-completion-"+make_stable_id());
    std::filesystem::create_directory(root);
    try {
        workflow(root,false,true); workflow(root,true,true); workflow(root,false,false);
        workflow(root,false,false,true); refusal(); overflow_and_locks(); analytical_targets();
        std::filesystem::remove_all(root); std::cout<<"Dimension placement completion tests passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr<<"dimension_placement_completion_tests: "<<error.what()<<"\nEvidence: "<<root<<'\n'; return 1; }
}
