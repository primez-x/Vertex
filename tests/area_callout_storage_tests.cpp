#include "sketch/annotation_entity_codec.hpp"
#include "sketch/boundary_entity.hpp"
#include "sketch/constraint_authoring.hpp"
#include "sketch/measurement_linework.hpp"
#include "sketch/project_exchange.hpp"
#include "sketch/project_store.hpp"
#include "support/noninteractive_errors.hpp"

#include <sqlite3.h>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numbers>
#include <stdexcept>

namespace {
using namespace sketch;
using Json=nlohmann::json;
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
struct Temporary {
    std::filesystem::path path=std::filesystem::temp_directory_path()/("vertex-callouts-"+make_stable_id());
    Temporary(){std::filesystem::create_directory(path);}
    ~Temporary(){std::error_code ignored;std::filesystem::remove_all(path,ignored);}
};
Entity area() {
    return encode_identified_boundary_entity({"area","measurement_boundary",{
        {"a","v0","v1",{{0,0},{4,0},0}}, {"b","v1","v2",{{4,0},{4,3},0}},
        {"c","v2","v3",{{4,3},{0,3},0}}, {"d","v3","v0",{{0,3},{0,0},0}}}});
}
AnnotationState roles(std::string target="area") {
    AnnotationState state;
    PresentationOverride name;name.target_kind="area_name";name.target_id=target;
    name.style.text_alignment="left";name.style.stroke_color="#224466";
    name.plan_label_offset=Vec2{1,2};name.paper_text_height_mm=3;
    name.plan_label_rotation_radians=.25;state.overrides.push_back(name);
    auto calculation=name;calculation.target_kind="area_calculation";
    calculation.style.text_alignment="right";calculation.style.stroke_color="#993311";
    calculation.visible=false;calculation.plan_label_offset=Vec2{-3,4};
    calculation.paper_text_height_mm=2;state.overrides.push_back(calculation);
    return state;
}
void downgrade(const std::filesystem::path& path) {
    sqlite3* database=nullptr;
    const auto opened=sqlite3_open(path.string().c_str(),&database);
    if(opened!=SQLITE_OK){if(database)sqlite3_close(database);throw std::runtime_error("fixture database open failed");}
    const auto code=sqlite3_exec(database,"PRAGMA user_version=44; UPDATE metadata SET value='44' WHERE key='format_version'",nullptr,nullptr,nullptr);
    sqlite3_close(database);require(code==SQLITE_OK,"fixture downgrade failed");
}
void persistence_and_retained_reader_floor() {
    Temporary temporary;
    AnnotationState legacy;legacy.overrides.push_back({"area","area"});
    auto document=Document::create({area(),make_annotation_entity("annotations",legacy)});
    require(ProjectStore::required_format_version(document.snapshot())<45,"legacy centered callout was unnecessarily promoted");
    const auto original_area=document.snapshot().entities().at("area");
    const auto annotation=make_annotation_entity("annotations",roles());
    require(annotation.properties.at("state").at("version")==8,"split callouts lost their annotation reader version");
    document.apply(ApplyEntityChanges{document.revision(),{EntityChange::upsert(annotation)}, {},"Separate live callout presentation"});
    const auto head=document.snapshot();auto deleted=Document::fork(head);
    deleted.apply(ApplyEntityChanges{deleted.revision(),{EntityChange::erase("annotations")},{},"Delete callout presentation"});
    document.undo(document.revision());
    for(const auto& snapshot:{head,document.snapshot(),deleted.snapshot()}) {
        require(ProjectStore::required_format_version(snapshot)==45,"active, undone or deleted callouts lost required reader45");
        require(snapshot.entities().at("area")==original_area,"presentation changed analytical geometry");
        const auto file=temporary.path/(make_stable_id()+".bldproj");
        (void)ProjectStore::save(file,snapshot);const auto restored=ProjectStore::load(file).document.snapshot();
        require(restored.entities()==snapshot.entities() && restored.history().size()==snapshot.history().size(),
            "native reopen changed independent callout properties or retained history");
        const auto extraction=temporary.path/make_stable_id();extract_project(snapshot,extraction);
        std::ifstream input(extraction/"project.json");const auto manifest=Json::parse(input);
        require(manifest.at("exchange_version")==43,"extraction failed to require alignment-aware readers");
        downgrade(file);const auto unchanged_hash=ProjectStore::file_sha256(file);bool refused=false;
        try{(void)ProjectStore::load(file);}catch(const StorageError& error){refused=error.code()==StorageErrorCode::unsupported_format;}
        require(refused && ProjectStore::file_sha256(file)==unchanged_hash,"downgraded reader marker was accepted or changed source bytes");
    }
    document.redo(document.revision());
    require(document.snapshot().entities()==head.entities(),"Redo changed independent name/value settings");
    AnnotationState aligned;auto label=instantiate_label(default_label_templates().front(),"aligned-label");
    label.style.text_alignment="right";aligned.labels.push_back(label);
    require(ProjectStore::required_format_version(Document::create({make_annotation_entity("text",aligned)}).snapshot())==45,
        "alignment without area roles lost its native reader floor");
}
void measured_owner_rotation_completes_both_offsets() {
    // The completion seam is keyed by measured-owner ID; exercise both new
    // presentation roles without borrowing a desktop-provided output offset.
    MeasurementLinework model;model.stroke_id="stroke";
    ConstructionReceipt receipt;receipt.segment_id="edge";receipt.kind=BoundaryConstructionKind::line_to_point;
    receipt.start={0,0};receipt.chord_end={2,0};model.edges.push_back({"edge","start","end",receipt});
    auto annotation=make_annotation_entity("annotations",roles("stroke"));
    auto document=Document::create({{"p","property",Json::object()},{"b","building",{{"property_id","p"}}},
        {"f","floor",{{"building_id","b"}}},{"l","layer",{{"floor_id","f"}}},
        {"stroke","measurement_linework",{{"property_id","p"},{"building_id","b"},{"floor_id","f"},
            {"layer_id","l"},{"model",encode_measurement_linework_model(model)}},true},annotation});
    const auto before=document.snapshot();ConstraintAuthoringIntent intent;MeasuredStrokeTransformIntent transform;
    transform.targets={{"stroke",{{},std::numbers::pi/2,true,false,{7,9}}}};intent.measured_stroke_transform=transform;
    const auto preview=preview_constraint_authoring(before,intent);require(preview.accepted(),"measured role transform preview refused");
    apply_constraint_authoring(document,preview);
    const auto after=document.snapshot();const auto state=decode_annotation_entity(after.entities().at("annotations"));
    require(state.overrides.size()==2,"measured completion lost a callout role");
    const auto points_equal=[](Vec2 a,Vec2 b){return std::hypot(a.x-b.x,a.y-b.y)<1e-12;};
    require(points_equal(*state.overrides[0].plan_label_offset,{2,1}) && points_equal(*state.overrides[1].plan_label_offset,{4,-3}),
        "role offsets were not rotated and reflected exactly once");
    require(!state.overrides[1].visible && state.overrides[0].style.text_alignment=="left" &&
        state.overrides[1].style.text_alignment=="right","completion changed independent style or visibility");
    document.undo(document.revision());require(document.snapshot().entities()==before.entities(),"role transform Undo changed source");
    document.redo(document.revision());require(document.snapshot().entities()==after.entities(),"role transform Redo changed result");
    require(Document::fork(document.snapshot()).snapshot().entities()==after.entities(),"retained role completion did not revalidate");
}
}
int main(){sketch::testing::noninteractive_errors();try{
    persistence_and_retained_reader_floor();measured_owner_rotation_completes_both_offsets();
    std::cout<<"Area callout storage tests passed\n";return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
