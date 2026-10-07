#include "support/detached_document_snapshot.hpp"
#include "sketch/boundary_dimension.hpp"
#include "sketch/constraint_authoring.hpp"
#include "sketch/project_exchange.hpp"
#include "sketch/project_store.hpp"
#include "sketch/wall_measurement.hpp"
#include "support/noninteractive_errors.hpp"
#include <sqlite3.h>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numbers>
#include <span>
#include <stdexcept>

namespace {
using namespace sketch;
using Json = nlohmann::json;
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
template<class F> void rejects(F function, const char* message) {
    try { function(); } catch (const std::exception&) { return; }
    throw std::runtime_error(message);
}
Entity wall(std::string id, Vec2 start, Vec2 end) {
    return {std::move(id), "wall", {{"baseline", {{"start", {start.x,start.y}}, {"end", {end.x,end.y}}, {"sweep_radians",0}}},
        {"thickness_m",0.2}, {"height_m",3}, {"elevation_m",0}, {"property_id","property"},
        {"building_id","building"}, {"floor_id","floor"}, {"layer_id","layer"}}};
}
Document fixture(bool exterior=true,bool curved=false) {
    std::vector<Entity> entities{{"property","property",{{"calculation_workflow","measurement"}}},
        {"building","building",{{"property_id","property"}}}, {"floor","floor",{{"building_id","building"}}},
        {"layer","layer",{{"floor_id","floor"}}}};
    for (const auto* prefix : {"rigid", "partial"}) {
        const double x = std::string(prefix)=="rigid" ? 0 : 12;
        entities.push_back(wall(std::string(prefix)+"-bottom",{x,0},{x+4,0}));
        entities.push_back(wall(std::string(prefix)+"-right",{x+4,0},{x+4,3}));
        entities.push_back(wall(std::string(prefix)+"-top",{x+4,3},{x,3}));
        entities.push_back(wall(std::string(prefix)+"-left",{x,3},{x,0}));
        if(curved && std::string(prefix)=="rigid")entities[entities.size()-4].properties["baseline"]["sweep_radians"]=0.35;
    }
    const auto physical=Document::create(entities).snapshot();
    for (const auto* prefix : {"rigid", "partial"}) {
        const std::string p=prefix;
        const std::vector<std::string> ids{p+"-bottom",p+"-right",p+"-top",p+"-left"};
        const auto measured=derive_exterior_wall_measurement(physical,ids);
        IdentifiedBoundary boundary{p+"-area","measurement_boundary",{}};
        for (std::size_t i=0;i<measured.boundary.size();++i)
            boundary.segments.push_back({p+"-edge-"+std::to_string(i),p+"-vertex-"+std::to_string(i),
                p+"-vertex-"+std::to_string((i+1)%measured.boundary.size()),measured.boundary[i]});
        auto owner=encode_identified_boundary_entity(boundary);
        if(exterior)owner.properties["wall_measurement_source"]=measured.source;
        for (const auto* key : {"property_id","building_id","floor_id","layer_id"})
            owner.properties[key]=physical.entities().at(ids.front()).properties.at(key);
        entities.push_back(owner);
        for (const auto* mode : {"automatic","manual"}) {
            BoundaryDimension dimension{p+"-"+mode,p+"-area",boundary.segments.front().segment_id,{2, -0.5}};
            if(std::string(mode)=="automatic") {
                dimension.placement=BoundaryDimensionPlacement::automatic;
                dimension.automatic_placement_version=2;
            }
            auto entity=encode_boundary_dimension_entity(dimension);
            entity.extensions["vendor"]={{"retain",17}};
            entities.push_back(entity);
        }
        for(std::size_t i=0;i<ids.size();++i)
            entities.push_back(encode_constraint_entity({p+"-join-"+std::to_string(i),ConstraintRelationKind::coincident,
                {{ids[i],WallEndpointRole::end},{ids[(i+1)%ids.size()],WallEndpointRole::start}},{},{}}));
    }
    return Document::create(entities);
}
TransformBoundaries rigid_command(const DocumentSnapshot& before) {
    PlanarTransform transform; transform.offset={0.4,0.2};
    TransformBoundaries command{before.revision(),{{"rigid-area",transform}},{},"Rigid perimeter"};
    for(const auto* suffix : {"bottom","right","top","left"}) {
        auto entity=before.entities().at(std::string("rigid-")+suffix);
        auto& baseline=entity.properties.at("baseline");
        for(const auto* endpoint : {"start","end"}) {
            baseline[endpoint][0]=baseline.at(endpoint)[0].get<double>()+transform.offset.x;
            baseline[endpoint][1]=baseline.at(endpoint)[1].get<double>()+transform.offset.y;
        }
        command.entity_changes.push_back(EntityChange::upsert(entity));
    }
    return command;
}
ApplyBoundaryConstraintChanges mixed_command(const DocumentSnapshot& before) {
    ConstraintAuthoringIntent intent; intent.message="Move mixed perimeters";
    intent.wall_geometry_move=WallGeometryMoveIntent{{{"partial-bottom",{12.4,0.2},{16.4,0.2}}},true};
    const auto preview=preview_constraint_authoring(before,intent);
    if(!preview.accepted()) {
        std::string message="Partial fixture refused";
        for(const auto& diagnostic:preview.diagnostics())message+="\n"+diagnostic;
        throw std::runtime_error(message);
    }
    auto candidate=Document::fork(before); (void)apply_constraint_authoring(candidate,preview);
    auto command=*candidate.snapshot().history().back().boundary_constraint_changes;
    command.dimension_placement_completion=true;
    command.dimension_placement_moves={{"partial-automatic",{0.4,0.2}}};
    command.rigid_group_completion=true;
    command.rigid_group_transform=rigid_command(before);
    return command;
}
void forged_storage_refusals(const std::filesystem::path& directory,const DocumentSnapshot& snapshot) {
    for(const unsigned format : {41u,42u}) {
        const auto path=directory/("forged-"+std::to_string(format)+".sketch");ProjectStore::save(path,snapshot);
        auto proof=command_to_json(Command{*snapshot.history().back().boundary_constraint_changes});
        if(format==42)proof["rigid_group_transform"]["transformations"][0]["transform"]["offset"]["x"]=0.5;
        Json manifest{{"format_version",format},{"document_id",snapshot.document_id()},{"head_revision",snapshot.revision()},
            {"saved_revision",snapshot.revision()},{"named_revisions",snapshot.named_revisions()},{"history",Json::array()}};
        for(const auto& revision:snapshot.history()) {
            Json row{{"revision",revision.revision},{"parent_revision",revision.parent_revision},{"source_revision",revision.source_revision},
                {"action",revision.action},{"name",revision.name},{"undo_stack",revision.undo_stack},{"redo_stack",revision.redo_stack},
                {"entities",Json::array()},{"assets",Json::array()}};
            if(revision.boundary_constraint_changes)row["boundary_constraint_changes"]=proof;
            for(const auto& [id,entity]:revision.entities)
                row["entities"].push_back({{"id",id},{"type",entity.type},{"required",entity.required},{"properties",entity.properties},{"extensions",entity.extensions}});
            manifest["history"].push_back(std::move(row));
        }
        const auto bytes=manifest.dump();const auto hash=sha256_hex(std::as_bytes(std::span<const char>(bytes.data(),bytes.size())));
        sqlite3* db{};require(sqlite3_open(path.string().c_str(),&db)==SQLITE_OK,"Open test-owned native42 forgery");
        if(format==42) {
            sqlite3_stmt* statement{};const auto encoded=proof.dump();
            require(sqlite3_prepare_v2(db,"UPDATE revisions SET boundary_constraint_changes_json=? WHERE boundary_constraint_changes_json IS NOT NULL",-1,&statement,nullptr)==SQLITE_OK,"Prepare mixed proof forgery");
            sqlite3_bind_text(statement,1,encoded.c_str(),-1,SQLITE_TRANSIENT);
            const auto result=sqlite3_step(statement);sqlite3_finalize(statement);require(result==SQLITE_DONE,"Write forged mixed proof");
        }
        const auto sql="PRAGMA user_version="+std::to_string(format)+"; UPDATE metadata SET value='"+std::to_string(format)+
            "' WHERE key='format_version'; UPDATE metadata SET value='"+hash+"' WHERE key='logical_digest';";
        const auto result=sqlite3_exec(db,sql.c_str(),nullptr,nullptr,nullptr);sqlite3_close(db);require(result==SQLITE_OK,"Write independently recomputed forged digest");
        const auto fingerprint=ProjectStore::file_sha256(path);bool refused{};
        try{(void)ProjectStore::load(path);}catch(const StorageError&){refused=true;}
        require(refused && ProjectStore::file_sha256(path)==fingerprint,"Native42 proof/floor forgery must refuse without modifying bytes");
    }
}
void workflow(const std::filesystem::path& directory,bool curved=false) {
    auto document=fixture(true,curved);const auto before=document.snapshot();const auto command=mixed_command(before);
    const auto rigid=Document::preview_command(before,*command.rigid_group_transform);
    auto partial=command;partial.rigid_group_transform.reset();partial.rigid_group_completion=false;
    const auto connected=Document::preview_command(before,partial);
    const auto encoded=command_to_json(Command{command});
    require(encoded.at("version")==16 && encoded.at("rigid_group_completion")==true,"Mixed translation requires envelope16");
    require(command_to_json(command_from_json(encoded))==encoded,"Typed mixed proof must roundtrip exactly");
    const auto preview=Document::preview_command(before,command);
    for(const auto& [id,entity]:rigid.entities())
        if(entity!=before.entities().at(id) && !can_recognize_boundary_dimension_entity_type(entity.type))
            require(preview.entities().at(id)==entity,"Rigid geometry delta must equal independent original-source replay");
    for(const auto& [id,entity]:connected.entities())
        if(entity!=before.entities().at(id))require(preview.entities().at(id)==entity,"Partial delta must equal independent original-source replay");
    for(const auto* mode : {"automatic","manual"}) {
        const auto old=*decode_boundary_dimension_entity(before.entities().at(std::string("rigid-")+mode)).dimension;
        const auto after=*decode_boundary_dimension_entity(preview.entities().at(std::string("rigid-")+mode)).dimension;
        require(after.placement==old.placement && after.automatic_placement_version==old.automatic_placement_version,
            "Rigid callout translation must retain placement provenance");
        require(after.text_position.x==old.text_position.x+0.4 && after.text_position.y==old.text_position.y+0.2,
            "All rigid callouts must follow exactly once");
    }
    require(decode_boundary_dimension_entity(preview.entities().at("partial-automatic")).dimension->placement==BoundaryDimensionPlacement::manual,
        "Explicit partial placement must become manual after source reflow");
    require(preview.entities().at("partial-top")==before.entities().at("partial-top"),"Unchanged partial perimeter wall must retain exact source");
    document.apply(command);const auto after=document.snapshot();
    require(after.entities()==preview.entities() && after.revision()==before.revision()+1,"Mixed selection must apply as one exact preview revision");
    document.undo(document.revision());require(document.snapshot().entities()==before.entities(),"Undo restores both independent lanes");
    document.redo(document.revision());require(document.snapshot().entities()==after.entities(),"Redo restores both lanes and callouts");
    const auto path=directory/"mixed.sketch";(void)ProjectStore::save(path,document.snapshot());
    const auto loaded=ProjectStore::load(path).document.snapshot();
    require(loaded.is_editable() && loaded.entities()==after.entities(),"Native mixed proof must reopen editable with exact entities");
    require(ProjectStore::required_format_version(loaded)==42,"Mixed history must retain native42 through undo and redo");
    extract_project(loaded,directory/"extracted");
    std::ifstream extracted(directory/"extracted"/"project.json");
    require(Json::parse(extracted).at("exchange_version")==40,"Mixed native history must extract as version40");
    if(!curved)forged_storage_refusals(directory,after);
    if(const auto* destination=std::getenv("VERTEX_MIXED_FIXTURE_DIRECTORY")) {
        const std::filesystem::path output(destination);std::filesystem::create_directories(output);
        ProjectStore::save(output/(curved ? "mixed-curved.bldproj" : "mixed-straight.bldproj"),after);
    }
    sketch::test::DetachedDocumentSnapshotFixture tampered(after);
    auto& proof=tampered.history().back().boundary_constraint_changes;
    proof->rigid_group_transform->transformations.front().transform.offset.x+=0.1;
    rejects([&]{(void)Document::fork(tampered);},"Tampered child must fail deterministic retained reconstruction");
    auto stripped=command;stripped.rigid_group_transform.reset();
    require(command_to_json(Command{stripped}).at("version")==16,"Emptied rigid child retains mixed dialect");
    rejects([&]{(void)Document::preview_command(before,stripped);},"Marker cannot lend authority without a rigid child");
    sketch::test::DetachedDocumentSnapshotFixture stripped_history(after);
    stripped_history.history().back().boundary_constraint_changes->rigid_group_transform.reset();
    require(ProjectStore::required_format_version(stripped_history)==42,"Stripped retained child cannot lower explicit mixed storage floor");
    auto geometry=command;geometry.dimension_placement_completion=false;geometry.dimension_placement_moves.clear();
    require(command_to_json(command_from_json(command_to_json(Command{geometry})))==command_to_json(Command{geometry}),
        "Mixed dialect must not require an explicit partial placement lane");
    (void)Document::preview_command(before,geometry);
}
void no_source_half_turn_reopens(const std::filesystem::path& directory) {
    auto document=fixture(false);const auto before=document.snapshot();auto command=mixed_command(before);
    require(!command.exterior_source_completion,"No-source fixture must exercise mixed authority alone");
    PlanarTransform transform{{2,0},std::numbers::pi,false,false,{}};
    auto& rigid=*command.rigid_group_transform;rigid.transformations.front().transform=transform;
    for(auto& change:rigid.entity_changes) {
        const auto& old=before.entities().at(change.entity.id).properties.at("baseline");
        const Segment segment{{old.at("start")[0].get<double>(),old.at("start")[1].get<double>()},
            {old.at("end")[0].get<double>(),old.at("end")[1].get<double>()},old.at("sweep_radians").get<double>()};
        const auto moved=transform_segment(segment,transform);
        change.entity.properties["baseline"]={{"start",{moved.start.x,moved.start.y}},
            {"end",{moved.end.x,moved.end.y}},{"sweep_radians",moved.sweep_radians}};
    }
    const auto preview=Document::preview_command(before,command);document.apply(command);
    require(document.snapshot().entities()==preview.entities(),"No-source half-turn applies its exact admitted mixed proposal");
    require(Document::fork(document.snapshot()).snapshot().entities()==preview.entities(),"No-source mixed half-turn reconstructs retained authority");
    const auto path=directory/"half-turn.sketch";ProjectStore::save(path,document.snapshot());
    require(ProjectStore::load(path).document.snapshot().entities()==preview.entities(),"No-source rigid endpoint exchange reopens exactly");
}

void refusals() {
    auto document=fixture();const auto before=document.snapshot();const auto valid=mixed_command(before);
    const auto check=[&](const ApplyBoundaryConstraintChanges& command) {
        rejects([&]{(void)Document::preview_command(before,command);},"Invalid mixed proof must refuse preview");
        rejects([&]{document.apply(command);},"Invalid mixed proof must refuse apply");
        require(document.snapshot().entities()==before.entities() && document.revision()==before.revision(),"Mixed refusal must preserve source atomically");
    };
    auto command=valid;command.rigid_group_completion=false;check(command);
    command=valid;++command.rigid_group_transform->expected_revision;check(command);
    command=valid;command.dimension_placement_moves.push_back({"rigid-automatic",{0.4,0.2}});check(command);
    command=valid;command.wall_edits.push_back({"rigid-bottom",{{0.4,0.2},{4.4,0.2},0},{}});check(command);
    command=valid;command.supplemental_source_completion=true;
    command.supplemental_entity_changes.push_back(EntityChange::upsert(before.entities().at("rigid-join-0")));check(command);
    command=valid;
    command.entity_changes.push_back(EntityChange::upsert(encode_constraint_entity({"cross-lane-parallel",ConstraintRelationKind::parallel,
        {{"rigid-bottom",WallEndpointRole::start},{"rigid-bottom",WallEndpointRole::end},
         {"partial-bottom",WallEndpointRole::start},{"partial-bottom",WallEndpointRole::end}},{},{}})));
    check(command);
    command=valid;
    auto unchanged=before.entities().at("partial-top");
    command.rigid_group_transform->entity_changes.push_back(EntityChange::upsert(unchanged));check(command);
    auto encoded=command_to_json(Command{valid});encoded["rigid_group_transform"]["kind"]="apply_entity_changes";
    rejects([&]{(void)command_from_json(encoded);},"Mixed child must reject arbitrary command identity");
    encoded=command_to_json(Command{valid});encoded["rigid_group_transform"]["surprise"]=1;
    rejects([&]{(void)command_from_json(encoded);},"Rigid child must reject unknown fields");
    encoded=command_to_json(Command{valid});encoded["surprise"]=1;
    rejects([&]{(void)command_from_json(encoded);},"Mixed envelope must reject unknown fields");
    auto locked=fixture();
    locked.apply(ApplyEntityChanges{locked.revision(),{EntityChange::upsert(encode_constraint_entity(
        {"partial-pin",ConstraintRelationKind::fixed_anchor,{{"partial-bottom",WallEndpointRole::start}},{},Vec2{12,0}}))},{},"Pin partial selection"});
    command=valid;command.expected_revision=locked.revision();command.rigid_group_transform->expected_revision=locked.revision();
    rejects([&]{locked.apply(command);},"Mixed rigid authority cannot move a partial fixed anchor");
    require(locked.snapshot().entities().at("partial-bottom")==before.entities().at("partial-bottom"),"Refused partial anchor cannot move");
    auto rigid_locked=fixture();
    rigid_locked.apply(ApplyEntityChanges{rigid_locked.revision(),{EntityChange::upsert(encode_constraint_entity(
        {"rigid-pin",ConstraintRelationKind::fixed_anchor,{{"rigid-bottom",WallEndpointRole::start}},{},Vec2{0,0}}))},{},"Pin rigid selection"});
    command=valid;command.expected_revision=rigid_locked.revision();command.rigid_group_transform->expected_revision=rigid_locked.revision();
    auto pin=rigid_locked.snapshot().entities().at("rigid-pin");
    auto moved_pin=*decode_constraint_entity(pin).constraint;moved_pin.anchor=Vec2{0.4,0.2};
    command.rigid_group_transform->entity_changes.push_back(EntityChange::upsert(encode_constraint_entity(moved_pin,&pin)));
    rejects([&]{rigid_locked.apply(command);},"Mixed group cannot translate fixed anchor payload along with its owner");
    require(rigid_locked.snapshot().entities().at("rigid-pin")==pin,"Refused rigid anchor preserves exact pin payload");
}
}
int main() {
    testing::noninteractive_errors();
    const auto directory=std::filesystem::temp_directory_path()/("vertex-mixed-translation-"+make_stable_id());
    std::filesystem::create_directory(directory);
    try {
        workflow(directory);const auto curved=directory/"curved";std::filesystem::create_directory(curved);workflow(curved,true);
        no_source_half_turn_reopens(directory);refusals();std::filesystem::remove_all(directory);
        std::cout<<"Mixed translation completion tests passed\n";return 0;
    } catch(const std::exception& error) {
        std::cerr<<"mixed_translation_completion_tests: "<<error.what()<<"\nEvidence: "<<directory<<'\n';return 1;
    }
}
