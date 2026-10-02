#include "sketch/document.hpp"
#include "sketch/document_digest.hpp"
#include "sketch/boundary_entity.hpp"
#include "sketch/boundary_receipt.hpp"
#include "sketch/boundary_dimension.hpp"
#include "sketch/constraint_entity.hpp"
#include "sketch/project_store.hpp"
#include "sketch/project_exchange.hpp"
#include "support/noninteractive_errors.hpp"

#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numbers>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace {
using namespace sketch;
void require(bool condition,std::string_view message) {
    if(!condition) throw std::runtime_error(std::string(message));
}
void require_near(double actual,double expected,std::string_view message) {
    require(std::isfinite(actual) && std::abs(actual-expected)<1e-9,message);
}
template<class Function> void rejected(Function&& function,DocumentErrorCode code,std::string_view message) {
    try {function();}catch(const DocumentError& error){require(error.code()==code,message);return;}
    throw std::runtime_error(std::string(message));
}
struct TempDirectory {
    const std::filesystem::path parent=std::filesystem::weakly_canonical(std::filesystem::temp_directory_path());
    const std::filesystem::path path=parent/("vertex-boundary-arc-tests-"+make_stable_id());
    TempDirectory(){require(std::filesystem::create_directory(path),"arc storage fixture needs a new task-owned directory");}
    ~TempDirectory() {
        std::error_code error;
        // Remove only the exact root created above, never a replaced link or
        // a resolved path outside its original temporary parent.
        const auto state=std::filesystem::symlink_status(path,error);
        if(error || std::filesystem::is_symlink(state))return;
        const auto resolved=std::filesystem::weakly_canonical(path,error);
        if(!error && resolved==path && resolved.parent_path()==parent)
            std::filesystem::remove_all(path,error);
    }
};
Entity rectangle() {
    BoundaryConstructionRecord record;record.boundary_id="area";record.anchor={0,0};
    record.extensions={{"vendor_receipt","retain"}};
    IdentifiedBoundary boundary{"area","measurement_boundary",{}};
    const Vec2 points[]{{0,0},{10,0},{10,4},{0,4}};
    const char* runs[]{"10 m","0 m","-10 m","0 m"};
    const char* rises[]{"0 m","4 m","0 m","-4 m"};
    for(std::size_t i=0;i<4;++i) {
        const auto segment="edge-"+std::to_string(i),start="vertex-"+std::to_string(i),end="vertex-"+std::to_string((i+1)%4);
        ConstructionReceipt receipt;receipt.segment_id=segment;receipt.kind=BoundaryConstructionKind::line_rise_run;
        receipt.start=points[i];receipt.run=parse_quantity(runs[i]);receipt.rise=parse_quantity(rises[i]);
        record.edges.push_back({segment,start,end,receipt});
        boundary.segments.push_back({segment,start,end,{points[i],points[(i+1)%4],0}});
    }
    auto result=encode_identified_boundary_entity(boundary);
    result.properties["boundary_authoring"]=encode_boundary_receipt_envelope(record);
    result.properties["name"]="Receipt-backed curved area";result.extensions["vendor_owner"]={{"retain",17}};return result;
}
std::vector<Entity> fixture() {
    BoundaryDimension automatic{"automatic-length","area","edge-0",{5,-1},BoundaryDimensionPlacement::automatic,2};
    automatic.presentation=BoundaryDimensionPresentation{3,"#345678",true,false,true,.2};
    auto automatic_entity=encode_boundary_dimension_entity(automatic);automatic_entity.extensions["vendor_dimension"]="retain";
    BoundaryDimension manual{"manual-length","area","edge-0",{12,-3}};
    BoundaryDimension area{"manual-area","area","",{20,20}};area.kind=BoundaryDimensionKind::area;
    return {rectangle(),automatic_entity,encode_boundary_dimension_entity(manual),encode_boundary_dimension_entity(area)};
}
BoundaryGeometryEdit angle_edit() {
    BoundaryGeometryEdit result;result.boundary_id="area";result.kind=BoundaryGeometryEditKind::reconstruct_arc;result.target_id="edge-0";
    ConstructionReceipt receipt;receipt.segment_id="edge-0";receipt.kind=BoundaryConstructionKind::arc_chord_angle;
    receipt.start={0,0};receipt.chord_end=Vec2{10,0};receipt.angle=parse_angle("60.0 deg");result.arc_construction=receipt;return result;
}
BoundaryDimension dimension(const DocumentSnapshot& snapshot,const char* id) {
    const auto decoded=decode_boundary_dimension_entity(snapshot.entities().at(id));require(decoded.supported(),"fixture dimension must remain supported");return *decoded.dimension;
}
void assert_curved_snapshot(const DocumentSnapshot& before,const DocumentSnapshot& after,const BoundaryGeometryEdit& edit) {
    const auto& original=before.entities().at("area");const auto& current=after.entities().at("area");
    const auto old=decode_identified_boundary_entity(original),curved=decode_identified_boundary_entity(current);
    require(old.id==curved.id && old.type==curved.type && old.segments.size()==curved.segments.size(),"arc reconstruction must preserve boundary identity and topology");
    for(std::size_t i=0;i<old.segments.size();++i) {
        const auto& a=old.segments[i];const auto& b=curved.segments[i];
        require(a.segment_id==b.segment_id && a.start_vertex_id==b.start_vertex_id && a.end_vertex_id==b.end_vertex_id &&
            a.segment.start.x==b.segment.start.x && a.segment.start.y==b.segment.start.y && a.segment.end.x==b.segment.end.x && a.segment.end.y==b.segment.end.y,
            "fixed chord arc edit must not move endpoints or replace stable children");
        if(i)require(a==b,"arc reconstruction must leave every unselected edge exactly unchanged");
    }
    require_near(curved.segments.front().segment.sweep_radians,std::numbers::pi/3,"selected boundary edge must retain declared sixty-degree curve");
    const auto& archive=current.extensions.at("boundary_geometry_derivation");
    require(!current.properties.contains("boundary_authoring") && archive.at("source_boundary_authoring")==original.properties.at("boundary_authoring") &&
        archive.at("operations").size()==1 && archive.at("operations")[0].at("kind")=="geometry_edit" &&
        decode_boundary_geometry_edit(archive.at("operations")[0].at("value"))==edit &&
        decode_boundary_geometry_edit(archive.at("operations")[0].at("value")).arc_construction->angle->original_expression=="60.0 deg" &&
        current.extensions.at("vendor_owner")==original.extensions.at("vendor_owner"),"edit must archive exact line receipts and original curvature expression without rewriting owner metadata");
    require(after.entities().at("manual-length")==before.entities().at("manual-length") && after.entities().at("manual-area")==before.entities().at("manual-area"),
        "manual measurement presentations must remain exact during curvature reconstruction");
    const auto automatic=dimension(after,"automatic-length"),prior=dimension(before,"automatic-length");
    require_near(automatic.text_position.x,5,"automatic dimension remains at analytical arc midpoint X");
    require(automatic.text_position.y<-(10-5*std::sqrt(3.0)) && automatic.text_position.y!=prior.text_position.y &&
        automatic.placement==BoundaryDimensionPlacement::automatic && automatic.automatic_placement_version==2 && automatic.presentation==prior.presentation &&
        after.entities().at("automatic-length").extensions==before.entities().at("automatic-length").extensions,
        "automatic v2 dimension must move beyond the exterior arc midpoint while retaining explicit style/metadata");
    require_near(dimension(after,"manual-length").resolve(current).segment_length(),10*std::numbers::pi/3,"length dimension must derive physical arc length after edit");
    require_near(dimension(after,"manual-area").resolve(current).area(),40+50*(std::numbers::pi/3-std::sqrt(3.0)/2),"area dimension must derive curved gross area after edit");
    require_near(dimension(before,"manual-length").resolve(original).segment_length(),10,"initial manual length derives original chord");
    require_near(dimension(before,"manual-area").resolve(original).area(),40,"initial manual area derives rectangle");
}
unsigned stored_version(const std::filesystem::path& path) {
    std::ifstream file(path,std::ios::binary);require(file.good(),"native arc project must exist");
    // SQLite user_version is the four-byte big-endian value at header offset60.
    std::array<unsigned char,4> value{};file.seekg(60);file.read(reinterpret_cast<char*>(value.data()),value.size());
    require(file.good(),"saved native project must have complete version header");
    return (unsigned(value[0])<<24)|(unsigned(value[1])<<16)|(unsigned(value[2])<<8)|unsigned(value[3]);
}
void typed_arc_edit_history_and_rejection() {
    auto document=Document::create(fixture());const auto before=document.snapshot();const auto edit=angle_edit();
    const EditBoundaryGeometry command{before.revision(),edit};const auto proof=command_to_json(command);
    require(command_to_json(command_from_json(proof))==proof,"typed arc edit codec retains exact receipt");
    const auto preview=Document::preview_command(before,command);require(document.apply(command)==1,"arc reconstruction must be one document command");
    const auto after=document.snapshot();require(after.entities()==preview.entities() && after.history().back().boundary_geometry_edit==edit,"applied/preview geometry must retain the same typed command proof");
    assert_curved_snapshot(before,after,edit);
    auto replay=Document::fork(after);require(replay.snapshot().entities()==after.entities(),"authoritative document admission must replay receipt and curvature derivation exactly");
    const auto digest=document_snapshot_digest(after);
    rejected([&]{document.apply(EditBoundaryGeometry{before.revision(),edit});},DocumentErrorCode::stale_revision,"stale curvature edit must refuse atomically");
    require(document_snapshot_digest(document.snapshot())==digest,"stale arc edit must leave geometry and history unchanged");
    auto tampered=edit;tampered.arc_construction->angle->radians+=.1;
    rejected([&]{document.apply(EditBoundaryGeometry{after.revision(),tampered});},DocumentErrorCode::invalid_entity,"internally inconsistent exact angle receipt must refuse");
    require(document_snapshot_digest(document.snapshot())==digest,"tampered receipt must leave exact geometry/history unchanged");
    tampered=edit;tampered.arc_construction->chord_end->x=11;
    rejected([&]{document.apply(EditBoundaryGeometry{after.revision(),tampered});},DocumentErrorCode::invalid_entity,"tampered captured chord must refuse instead of moving boundary vertices");
    require(document_snapshot_digest(document.snapshot())==digest,"captured endpoint tampering must be atomic");
    document.undo(document.revision());require(document.snapshot().entities()==before.entities(),"arc edit undo restores exact line geometry, receipts and dimension placements");
    document.redo(document.revision());require(document.snapshot().entities()==after.entities(),"arc edit redo restores exact curve, archived receipt and dependent dimensions");
}
void native_and_exchange_preserve_arc_proof_and_reader_floor() {
    TempDirectory directory;auto document=Document::create(fixture());const auto initial=document.snapshot();const auto edit=angle_edit();
    document.apply(EditBoundaryGeometry{0,edit});const auto curved=document.snapshot();
    require(ProjectStore::required_format_version(curved)==15,"curvature command head requires native reader v15");
    const auto head_file=directory.path/"arc-head.bldproj";(void)ProjectStore::save(head_file,curved);require(stored_version(head_file)==15,"native writer must declare v15 for curvature proof");
    auto loaded=ProjectStore::load(head_file);const auto reopened=loaded.document.snapshot();
    require(reopened.entities()==curved.entities() && reopened.history().size()==curved.history().size() && reopened.history().at(1).boundary_geometry_edit==edit,
        "native reopen reproduces full typed arc history and exact proof");assert_curved_snapshot(initial,reopened,edit);
    loaded.document.undo(loaded.document.revision());require(loaded.document.snapshot().entities()==initial.entities(),"reopened undo restores original line receipt");
    loaded.document.redo(loaded.document.revision());require(loaded.document.snapshot().entities()==curved.entities(),"reopened redo replays exact curve derivation");
    document.undo(document.revision());const auto undone=document.snapshot();require(ProjectStore::required_format_version(undone)==15,"undone curvature retains v15 because redo/history still require its proof");
    const auto undone_file=directory.path/"arc-undone.bldproj";(void)ProjectStore::save(undone_file,undone);
    auto retained=ProjectStore::load(undone_file);require(stored_version(undone_file)==15 && retained.document.can_redo() && retained.document.snapshot().entities()==initial.entities(),"v15 native save must retain exact undone head and redo");
    retained.document.redo(retained.document.revision());require(retained.document.snapshot().entities()==curved.entities(),"saved undone project must replay curvature after reopen");
    std::vector<Entity> current;for(const auto& [id,value]:curved.entities()){(void)id;current.push_back(value);}
    auto entity_only=Document::create(std::move(current));require(entity_only.snapshot().history().size()==1 && ProjectStore::required_format_version(entity_only.snapshot())==15,
        "retained entity-only reconstruction derivation requires v15 without a command-history record");
    const auto entity_file=directory.path/"arc-entity-only.bldproj";(void)ProjectStore::save(entity_file,entity_only.snapshot());
    require(stored_version(entity_file)==15 && ProjectStore::load(entity_file).document.snapshot().entities()==curved.entities(),"entity-only archived arc reconstruction must reopen exactly under v15");
    const auto exchange=directory.path/"exchange";extract_project(undone,exchange);std::ifstream stream(exchange/"project.json");
    const auto wire=nlohmann::json::parse(stream);require(wire.at("exchange_version")==13 &&
        decode_boundary_geometry_edit(wire.at("revisions")[1].at("boundary_geometry_edit"))==edit,
        "extraction JSON exchange13 must retain arc proof even when current head is undone");
}
void fixed_physical_arc_length_prevents_curvature_change() {
    auto document=Document::create(fixture());document.apply(EditBoundaryGeometry{0,angle_edit()});
    const auto source=document.snapshot();const auto segment=decode_identified_boundary_entity(source.entities().at("area")).segments.front().segment;
    std::ostringstream expression;expression<<std::setprecision(std::numeric_limits<double>::max_digits10)<<segment_length(segment)<<" m";
    PersistentConstraint lock;lock.id="arc-length-lock";lock.relation=ConstraintRelationKind::fixed_arc_length;
    lock.bindings={{"area",WallEndpointRole::start,"edge-0","vertex-0"},{"area",WallEndpointRole::end,"edge-0","vertex-1"}};
    lock.length=parse_quantity(expression.str());require_near(lock.length->metres,10*std::numbers::pi/3,"constraint target must be captured from actual sixty-degree curve");
    document.apply(ApplyEntityChanges{source.revision(),{EntityChange::upsert(encode_constraint_entity(lock))}, {},"Lock actual physical curve length"});
    const auto locked=document.snapshot();const auto digest=document_snapshot_digest(locked);
    auto change=angle_edit();change.arc_construction->kind=BoundaryConstructionKind::arc_chord_height;change.arc_construction->angle.reset();change.arc_construction->height=parse_quantity("2 m");
    rejected([&]{document.apply(EditBoundaryGeometry{locked.revision(),change});},DocumentErrorCode::constraint_violation,
        "changing sagitta must refuse when actual physical arc length is locked");
    require(document_snapshot_digest(document.snapshot())==digest && document.snapshot().entities()==locked.entities(),"physical arc lock refusal must preserve curve, dimensions, receipts and history atomically");
}
}
int main() {
    sketch::testing::noninteractive_errors();
    try {
        typed_arc_edit_history_and_rejection();native_and_exchange_preserve_arc_proof_and_reader_floor();fixed_physical_arc_length_prevents_curvature_change();
        std::cout<<"boundary_arc_edit_tests passed\n";return 0;
    }catch(const std::exception& error){std::cerr<<"boundary_arc_edit_tests: "<<error.what()<<'\n';return 1;}
}
