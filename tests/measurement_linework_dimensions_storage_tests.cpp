#include "sketch/boundary_dimension.hpp"
#include "sketch/boundary_integrity.hpp"
#include "sketch/boundary_transform.hpp"
#include "sketch/measurement_linework.hpp"
#include "sketch/constraint_authoring.hpp"
#include "sketch/document_digest.hpp"
#include "sketch/project_store.hpp"
#include "sketch/project_exchange.hpp"
#include "support/noninteractive_errors.hpp"
#include <sqlite3.h>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <numbers>
#include <stdexcept>

namespace {
using namespace sketch;using Json=nlohmann::json;
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void require_near(double a,double b){require(std::abs(a-b)<1e-9,"saved measured dimension placement must match full rigid intent exactly once");}
void refused(const std::function<void()>& fn,const char* message){try{fn();}catch(const std::exception&){return;}throw std::runtime_error(message);}
struct Temporary {std::filesystem::path path=std::filesystem::temp_directory_path()/("vertex-measured-dimension-storage-"+make_stable_id());
    Temporary(){std::filesystem::create_directory(path);}~Temporary(){std::error_code ignored;std::filesystem::remove_all(path,ignored);}};
Entity stroke(){MeasurementLinework model;model.stroke_id="stroke";
    const Vec2 points[]{{0,0},{2,0},{2,3}};
    for(int i=0;i<2;++i){ConstructionReceipt receipt;receipt.segment_id="s:e"+std::to_string(i);receipt.kind=BoundaryConstructionKind::line_to_point;
        receipt.start=points[i];receipt.chord_end=points[i+1];model.edges.push_back({receipt.segment_id,"s:v"+std::to_string(i),"s:v"+std::to_string(i+1),receipt});}
    model.extensions={{"vendor",{{"literal","s:v1"},{"retain",true}}}};
    return {"stroke","measurement_linework",{{"property_id","p"},{"building_id","b"},{"floor_id","f"},{"layer_id","l"},
        {"model",encode_measurement_linework_model(model)}},true};}
Entity length_dimension(){BoundaryDimension dimension;dimension.id="saved-length";dimension.boundary_id="stroke";dimension.segment_id="s:e1";dimension.text_position={4,5};
    dimension.presentation=BoundaryDimensionPresentation{4.2,"#713ba2",true,false,true,0};auto entity=encode_boundary_dimension_entity(dimension);
    entity.extensions={{"vendor",{{"literal","s:e1"},{"retain",Json::array({7,"opaque"})}}}};return entity;}
Entity angle_dimension(){BoundaryDimension dimension;dimension.id="saved-angle";dimension.boundary_id="stroke";dimension.segment_id="s:e0";
    dimension.secondary_segment_id="s:e1";dimension.vertex_id="s:v1";dimension.kind=BoundaryDimensionKind::angle;dimension.text_position={2,-1};return encode_boundary_dimension_entity(dimension);}
Document fixture(bool with_dimensions){std::vector<Entity> values{{"p","property",Json::object(),false},{"b","building",{{"property_id","p"}},false},
    {"f","floor",{{"building_id","b"}},false},{"l","layer",{{"floor_id","f"}},false},stroke()};
    if(with_dimensions){values.push_back(length_dimension());values.push_back(angle_dimension());}return Document::create(values);}
BoundaryDimension dim(const DocumentSnapshot& snapshot,const char* id){const auto decoded=decode_boundary_dimension_entity(snapshot.entities().at(id));require(decoded.supported(),"retained measured dimension must decode");return *decoded.dimension;}
void accepted(const ConstraintAuthoringPreview& preview){if(preview.accepted())return;std::string error="Measured rigid dimension preview rejected";for(const auto& diagnostic:preview.diagnostics())error+="\n"+diagnostic;throw std::runtime_error(error);}
void equal(const DocumentSnapshot& a,const DocumentSnapshot& b){require(document_authoring_source_digest_v1(a)==document_authoring_source_digest_v1(b),"native reopen must retain exact source/dimensions/style/history");}
void markers_and_downgrade(const std::filesystem::path& file){sqlite3* db=nullptr;const auto encoded=file.u8string();const std::string path(reinterpret_cast<const char*>(encoded.data()),encoded.size());
    require(sqlite3_open_v2(path.c_str(),&db,SQLITE_OPEN_READWRITE,nullptr)==SQLITE_OK,"controlled native project must open");
    sqlite3_stmt* query=nullptr;require(sqlite3_prepare_v2(db,"SELECT value FROM metadata WHERE key='format_version'",-1,&query,nullptr)==SQLITE_OK&&sqlite3_step(query)==SQLITE_ROW,"native marker query must return");
    const auto marker=std::string(reinterpret_cast<const char*>(sqlite3_column_text(query,0)));sqlite3_finalize(query);require(marker=="36","native project must advertise reader36");
    require(sqlite3_prepare_v2(db,"PRAGMA user_version",-1,&query,nullptr)==SQLITE_OK&&sqlite3_step(query)==SQLITE_ROW,"native pragma query must return");
    const auto pragma=sqlite3_column_int(query,0);sqlite3_finalize(query);require(pragma==36,"both native reader markers must agree");
    const auto status=sqlite3_exec(db,"PRAGMA user_version=35; UPDATE metadata SET value='35' WHERE key='format_version'",nullptr,nullptr,nullptr);sqlite3_close(db);
    require(status==SQLITE_OK,"controlled marker downgrade must write");const auto hash=ProjectStore::file_sha256(file);bool rejected=false;
    try{(void)ProjectStore::load(file);}catch(const StorageError& error){rejected=error.code()==StorageErrorCode::unsupported_format;}
    require(rejected&&ProjectStore::file_sha256(file)==hash,"reader downgrade must refuse before acceptance and preserve native bytes");}
void independent_dimension_reader_floors(){Temporary temporary;auto document=fixture(false);const auto baseline=document.snapshot();
    require(baseline.entities().at("stroke").properties.at("model").at("version")==1&&ProjectStore::required_format_version(baseline)==1,"reader fixture must begin with genuine v1 source and no newer authority");
    document.apply(ApplyEntityChanges{.expected_revision=document.revision(),.entity_changes={EntityChange::upsert(length_dimension()),EntityChange::upsert(angle_dimension())},.message="Save measured dimensions"});
    const auto current=document.snapshot();document.undo(document.revision());const auto undone=document.snapshot();document.redo(document.revision());
    document.apply(ApplyEntityChanges{.expected_revision=document.revision(),.entity_changes={EntityChange::erase("saved-length"),EntityChange::erase("saved-angle")},.message="Delete measured dimensions"});
    const auto entity_only=fixture(true).snapshot();const std::vector<DocumentSnapshot> snapshots{entity_only,current,undone,document.snapshot()};
    for(std::size_t i=0;i<snapshots.size();++i){const auto& snapshot=snapshots[i];require(ProjectStore::required_format_version(snapshot)==36,"entity-only/current/undone/deleted dimension bindings must retain reader36");
        const auto file=temporary.path/("floor-"+std::to_string(i)+".bldproj");(void)ProjectStore::save(file,snapshot);equal(snapshot,ProjectStore::load(file).document.snapshot());
        const auto extracted=temporary.path/("extract-"+std::to_string(i));extract_project(snapshot,extracted);std::ifstream input(extracted/"project.json");const auto manifest=Json::parse(input);
        require(manifest.at("exchange_version")==34,"retained dimensions on v1 measured strokes must advertise logical reader34");markers_and_downgrade(file);}
}
void command_eleven_rigid_placement_and_overlap(){Temporary temporary;auto document=fixture(true);const auto before=document.snapshot();
    ConstraintAuthoringIntent intent;intent.measured_stroke_transform=MeasuredStrokeTransformIntent{{{"stroke",{{1,1},std::numbers::pi/2,true,false,{3,-2}}}},true};
    const auto preview=preview_constraint_authoring(before,intent);accepted(preview);const auto candidate=preview_constraint_authoring_snapshot(before,preview);
    const auto a=dim(candidate,"saved-length"),b=dim(candidate,"saved-angle");require_near(a.text_position.x,8);require_near(a.text_position.y,2);require_near(b.text_position.x,2);require_near(b.text_position.y,0);
    require_near(a.resolve(candidate.entities().at("stroke")).segment_length_metres,3);require_near(b.resolve(candidate.entities().at("stroke")).angle_radians,std::numbers::pi/2);
    require(a.presentation==dim(before,"saved-length").presentation&&candidate.entities().at("saved-length").extensions==before.entities().at("saved-length").extensions,"rigid dimension completion must retain style and opaque fields");
    auto command=*candidate.history().back().boundary_constraint_changes;const auto wire=command_to_json(command);
    require(wire.at("version")==11&&command_to_json(command_from_json(wire))==wire,"measured rigid command11 must roundtrip without new dimension codec fields");
    auto forged=command;auto placement=before.entities().at("saved-length");auto changed=dim(before,"saved-length");changed.text_position={9,9};placement=encode_boundary_dimension_entity(changed,&placement);
    forged.supplemental_source_completion=true;forged.supplemental_entity_changes={EntityChange::upsert(placement)};
    refused([&]{document.apply(forged);},"supplement cannot override source-owned rigid dimension placement");require(document.snapshot().entities()==before.entities()&&document.revision()==before.revision(),"overlap refusal must leave exact document/history");
    apply_constraint_authoring(document,preview);const auto after=document.snapshot();require(after.entities()==candidate.entities()&&after.revision()==before.revision()+1,"one rigid source event must carry dimension positions atomically");
    document.undo(document.revision());require(document.snapshot().entities()==before.entities(),"Undo must restore source and dimension positions");document.redo(document.revision());require(document.snapshot().entities()==after.entities(),"Redo must transform saved positions once");
    const auto file=temporary.path/"rigid-dimensions.bldproj";(void)ProjectStore::save(file,document.snapshot());equal(document.snapshot(),ProjectStore::load(file).document.snapshot());
}
Entity independent_boundary(){Entity entity{"area","boundary",{{"property_id","p"},{"building_id","b"},{"floor_id","f"},{"layer_id","l"},{"classification","living"},
    {"segments",Json::array({{{"start",{10,0}},{"end",{12,0}},{"sweep_radians",0}},{{"start",{12,0}},{"end",{12,2}},{"sweep_radians",0}},
        {{"start",{12,2}},{"end",{10,2}},{"sweep_radians",0}},{{"start",{10,2}},{"end",{10,0}},{"sweep_radians",0}}})}},false};
    entity=upgrade_legacy_boundary_entity(entity);
    entity.extensions["boundary_geometry_derivation"]={{"version",2},
        {"source_boundary",{{"boundary_model_version",entity.properties.at("boundary_model_version")},{"segments",entity.properties.at("segments")}}},
        {"operations",Json::array({{{"kind","transform"},{"value",encode_boundary_transform({"area",{}})}}})}};
    return entity;}
void mixed_legacy_group_placement(){Temporary temporary;
    for(bool translation:{false,true}){auto initial=fixture(true);auto values=initial.snapshot().entities();values.emplace("area",independent_boundary());std::vector<Entity> entities;
        for(const auto& [id,entity]:values){(void)id;entities.push_back(entity);}auto document=Document::create(entities);const auto before=document.snapshot();
        const PlanarTransform transform=translation?PlanarTransform{{},0,false,false,{3,-2}}:PlanarTransform{{1,1},std::numbers::pi/2,true,false,{3,-2}};
        auto replacement=before.entities().at("stroke");const auto model=*decode_measurement_linework_model(replacement.properties.at("model")).model;
        replacement.properties["model"]=encode_measurement_linework_model(transformed_measurement_linework(model,transform));
        Command command=translation?Command{TranslateBoundaries{before.revision(),{{"area",{3,-2}}},{EntityChange::upsert(replacement)},"Mixed measured translation"}}:
            Command{TransformBoundaries{before.revision(),{{"area",transform}},{EntityChange::upsert(replacement)},"Mixed measured rigid transform"}};
        const auto candidate=Document::preview_command(before,command);const auto length=dim(candidate,"saved-length"),angle=dim(candidate,"saved-angle");
        require_near(length.text_position.x,translation?7:8);require_near(length.text_position.y,translation?3:2);require_near(angle.text_position.x,translation?5:2);require_near(angle.text_position.y,translation?-3:0);
        require(length.presentation==dim(before,"saved-length").presentation,"mixed group must retain source dimension style");
        auto bad=command;auto overlap=before.entities().at("saved-length");auto placement=dim(before,"saved-length");placement.text_position={9,9};overlap=encode_boundary_dimension_entity(placement,&overlap);
        if(auto* move=std::get_if<TranslateBoundaries>(&bad))move->entity_changes.push_back(EntityChange::upsert(overlap));else std::get<TransformBoundaries>(bad).entity_changes.push_back(EntityChange::upsert(overlap));
        refused([&]{document.apply(bad);},"mixed rigid supplement cannot replace the owned dimension position");require(document.snapshot().entities()==before.entities()&&document.revision()==before.revision(),"mixed group overlap refusal must remain atomic");
        // A source payload with a different transform cannot borrow the area
        // group's placement authority for its dimensions.
        auto mismatched=command;auto wrong=replacement;wrong.properties["model"]=encode_measurement_linework_model(transformed_measurement_linework(model,{{},0,false,false,{99,0}}));
        if(auto* move=std::get_if<TranslateBoundaries>(&mismatched))move->entity_changes[0]=EntityChange::upsert(wrong);else std::get<TransformBoundaries>(mismatched).entity_changes[0]=EntityChange::upsert(wrong);
        refused([&]{document.apply(mismatched);},"mixed dimension completion must independently verify matching measured stroke transform");
        document.apply(command);const auto after=document.snapshot();require(after.entities()==candidate.entities()&&after.revision()==before.revision()+1,"legacy mixed group placement completion must form one command");
        document.undo(document.revision());require(document.snapshot().entities()==before.entities(),"mixed group Undo must restore all owners and dimensions");document.redo(document.revision());require(document.snapshot().entities()==after.entities(),"mixed group Redo must apply dimension positions once");
        const auto file=temporary.path/(translation?"mixed-translation.bldproj":"mixed-transform.bldproj");(void)ProjectStore::save(file,document.snapshot());equal(document.snapshot(),ProjectStore::load(file).document.snapshot());}
}
}
int main(){sketch::testing::noninteractive_errors();try{independent_dimension_reader_floors();command_eleven_rigid_placement_and_overlap();mixed_legacy_group_placement();}
    catch(const std::exception& error){std::cerr<<"measurement_linework_dimensions_storage_tests: "<<error.what()<<'\n';return 1;}
    std::cout<<"Measured dimension storage tests passed\n";return 0;}
