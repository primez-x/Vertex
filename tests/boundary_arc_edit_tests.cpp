#include "sketch/document.hpp"
#include "sketch/document_digest.hpp"
#include "sketch/boundary_entity.hpp"
#include "sketch/boundary_receipt.hpp"
#include "sketch/boundary_dimension.hpp"
#include "sketch/constraint_entity.hpp"
#include "sketch/project_store.hpp"
#include "sketch/project_exchange.hpp"
#include "sketch/wall_measurement.hpp"
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

void same_count_fresh_exterior_topology_is_reviewed_and_atomic() {
    for (const bool curved : {false,true}) for (const bool authored : {false,true}) {
        const auto entity=[](std::string id,std::string type,nlohmann::json properties=nlohmann::json::object()) {
            return Entity{std::move(id),std::move(type),std::move(properties),false,nlohmann::json::object()};
        };
        std::vector<Entity> values{entity("property","property"),entity("building","building",{{"property_id","property"}}),
            entity("floor","floor",{{"building_id","building"}}),entity("layer","layer",{{"floor_id","floor"}})};
        const Vec2 points[]{{0,0},{4,0},{4,3},{0,3}};
        std::vector<std::string> old_ids,new_ids;
        for(std::size_t i=0;i<4;++i) {
            old_ids.push_back("old-wall-"+std::to_string(i));new_ids.push_back("replacement-wall-"+std::to_string(i));
            const auto a=points[i],b=points[(i+1)%4];
            values.push_back(entity(old_ids.back(),"wall",{{"baseline",{{"start",{a.x,a.y}},{"end",{b.x,b.y}},
                {"sweep_radians",curved && (i==1 || i==3) ? std::numbers::pi : 0.0}}},{"thickness_m",.2},
                {"height_m",3.0},{"elevation_m",0.0},{"floor_id","floor"},{"layer_id","layer"}}));
        }
        auto wall_document=Document::create(values);
        const auto old_outline=derive_exterior_wall_measurement(wall_document.snapshot(),old_ids);
        IdentifiedBoundary old_boundary{"area","measurement_boundary",{}};
        BoundaryConstructionRecord record;record.schema_version=2;record.boundary_id="area";
        record.anchor=old_outline.boundary.front().start;record.extensions={{"original_input","retain"}};
        for(std::size_t i=0;i<4;++i) {
            const auto edge="old-edge-"+std::to_string(i),start="old-corner-"+std::to_string(i),end="old-corner-"+std::to_string((i+1)%4);
            old_boundary.segments.push_back({edge,start,end,old_outline.boundary[i]});
            ConstructionReceipt receipt;receipt.segment_id=edge;receipt.start=old_outline.boundary[i].start;receipt.chord_end=old_outline.boundary[i].end;
            receipt.kind=old_outline.boundary[i].sweep_radians==0 ? BoundaryConstructionKind::line_to_point : BoundaryConstructionKind::arc_chord_angle;
            if(receipt.kind==BoundaryConstructionKind::arc_chord_angle)receipt.angle=angle_from_radians(old_outline.boundary[i].sweep_radians);
            record.edges.push_back({edge,start,end,receipt});
        }
        auto owner=encode_identified_boundary_entity(old_boundary);
        owner.properties["floor_id"]="floor";owner.properties["layer_id"]="layer";
        owner.properties["wall_measurement_source"]=old_outline.source;
        owner.properties["name"]="Retained measured area";owner.properties["factor_expression"]="2/2";
        owner.properties["factor_numerator"]=2;owner.properties["factor_denominator"]=2;
        owner.properties["deduction_ids"]={"deduction"};
        owner.properties["appraisal_facts"]={{"boundary_role","measured_area"}};
        owner.extensions["vendor_style"]={{"color","#123456"}};
        if(authored)owner.properties["boundary_authoring"]=encode_boundary_receipt_envelope(record);
        values.push_back(owner);
        values.push_back(entity("deduction","measurement_boundary",{{"floor_id","floor"},{"layer_id","layer"},
            {"boundary",nlohmann::json::array({{{"start",{1,1}},{"end",{1.5,1}},{"sweep_radians",0}},
                {{"start",{1.5,1}},{"end",{1.5,1.5}},{"sweep_radians",0}},
                {{"start",{1.5,1.5}},{"end",{1,1.5}},{"sweep_radians",0}},
                {{"start",{1,1.5}},{"end",{1,1}},{"sweep_radians",0}}})}}));
        BoundaryDimension manual{"manual","area",old_boundary.segments[0].segment_id,{12,-3}};
        auto manual_entity=encode_boundary_dimension_entity(manual);manual_entity.extensions["presentation_note"]="retain";values.push_back(manual_entity);
        BoundaryDimension automatic{"automatic","area",old_boundary.segments[0].segment_id,{2,-1},BoundaryDimensionPlacement::automatic,2};
        values.push_back(encode_boundary_dimension_entity(automatic));
        PersistentConstraint coincident;coincident.id="shared-corner";coincident.relation=ConstraintRelationKind::coincident;
        coincident.bindings={{"area",WallEndpointRole::end,old_boundary.segments[0].segment_id,old_boundary.segments[0].end_vertex_id},
            {"area",WallEndpointRole::start,old_boundary.segments[1].segment_id,old_boundary.segments[1].start_vertex_id}};
        values.push_back(encode_constraint_entity(coincident));
        PersistentConstraint length;length.id="old-length-lock";length.relation=ConstraintRelationKind::fixed_length;
        length.bindings={{"area",WallEndpointRole::start,old_boundary.segments[0].segment_id,old_boundary.segments[0].start_vertex_id},
            {"area",WallEndpointRole::end,old_boundary.segments[0].segment_id,old_boundary.segments[0].end_vertex_id}};
        std::ostringstream metres;metres<<std::setprecision(17)<<segment_length(old_boundary.segments[0].segment)<<" m";length.length=parse_quantity(metres.str());
        values.push_back(encode_constraint_entity(length));
        auto document=Document::create(values);
        std::vector<EntityChange> changed;
        for(std::size_t i=0;i<4;++i) {
            auto wall=document.snapshot().entities().at(old_ids[i]);wall.id=new_ids[i];wall.properties["thickness_m"]=.4;
            for(const auto* endpoint:{"start","end"})
                if(wall.properties["baseline"][endpoint][0]==4.0)wall.properties["baseline"][endpoint][0]=5.0;
            changed.push_back(EntityChange::erase(old_ids[i]));changed.push_back(EntityChange::upsert(wall));
        }
        document.apply(ApplyEntityChanges{document.revision(),changed,{},"Redraw exterior source walls"});
        const auto before=document.snapshot();const auto new_outline=derive_exterior_wall_measurement(before,new_ids);
        IdentifiedBoundary replacement{"area","measurement_boundary",{}};
        for(std::size_t i=0;i<4;++i)replacement.segments.push_back({"fresh-edge-"+std::to_string(i),"fresh-corner-"+std::to_string(i),
            "fresh-corner-"+std::to_string((i+1)%4),new_outline.boundary[i]});
        BoundaryGeometryEdit intent;intent.boundary_id="area";intent.target_id="area";intent.kind=BoundaryGeometryEditKind::redefine_boundary;
        intent.replacement_segments=encode_identified_boundary_entity(replacement).properties.at("segments");
        intent.replacement_wall_source_ids=new_ids;intent.replacement_dimension_ids={"fresh-dimension-0","fresh-dimension-1","fresh-dimension-2","fresh-dimension-3"};
        intent.replacement_child_mapping={{"segments",{{old_boundary.segments[0].segment_id,"fresh-edge-0"},{old_boundary.segments[1].segment_id,"fresh-edge-1"}}},
            {"vertices",{{old_boundary.segments[0].end_vertex_id,"fresh-corner-1"}}}};
        intent.replacement_removed_reference_ids={"old-length-lock"};
        auto wire=encode_boundary_geometry_edit(intent);
        require(wire.at("version")==3 && encode_boundary_geometry_edit(decode_boundary_geometry_edit(wire))==wire,
            "default source repair retains its preexisting version three bytes");
        wire["version"]=4;wire["fresh_topology"]=true;
        const auto edit=decode_boundary_geometry_edit(wire);
        require(encode_boundary_geometry_edit(edit)==wire,"explicit fresh topology must retain strict version four intent");
        const auto refuses_wire=[](const nlohmann::json& malformed) {
            try {(void)decode_boundary_geometry_edit(malformed);}catch(const std::exception&){return;}
            throw std::runtime_error("invalid version four redraw envelope must reject");
        };
        auto malformed=wire;malformed["fresh_topology"]=false;refuses_wire(malformed);
        malformed=wire;malformed["fresh_topology"]="true";refuses_wire(malformed);
        malformed=wire;malformed["unknown"]=true;refuses_wire(malformed);
        malformed=wire;malformed["kind"]="move_vertex";refuses_wire(malformed);
        malformed=wire;malformed["replacement_wall_source_ids"]={"bad wall id","replacement-wall-1","replacement-wall-2"};refuses_wire(malformed);
        malformed=wire;malformed["replacement_wall_source_ids"]={"replacement-wall-0","replacement-wall-0","replacement-wall-2"};refuses_wire(malformed);
        malformed=wire;malformed.erase("replacement_wall_source_ids");refuses_wire(malformed);
        malformed=wire;malformed.erase("replacement_child_mapping");refuses_wire(malformed);
        malformed=wire;malformed["replacement_segments"][0]["oversized"]=std::string(1024*1024,'x');refuses_wire(malformed);
        const EditBoundaryGeometry command{before.revision(),edit};const auto digest=document_snapshot_digest(before);
        const auto preview=Document::preview_command(before,command);
        require(document_snapshot_digest(document.snapshot())==digest,"fresh-topology preview must remain detached from caller/source state");
        document.apply(command);const auto after=document.snapshot();
        require(after.revision()==before.revision()+1 && after.entities()==preview.entities() && after.entities().contains("area"),
            "same-count source redraw must commit one atomic command on the same owner");
        require(decode_identified_boundary_entity(after.entities().at("area"))==replacement && wall_measurement_source_current(after,after.entities().at("area")),
            "explicit fresh topology must adopt the exact derived analytical exterior and complete new child IDs");
        require(!after.entities().contains("automatic") && !after.entities().contains("old-length-lock"),"reviewed redraw must regenerate automatic dimensions and apply permitted lock removal");
        require(dimension(after,"manual").segment_id=="fresh-edge-0" && dimension(after,"manual").text_position.x==manual.text_position.x && dimension(after,"manual").text_position.y==manual.text_position.y &&
            after.entities().at("manual").extensions==manual_entity.extensions,"manual dimensions must retain exact placement and opaque presentation after reviewed mapping");
        const auto mapped=decode_constraint_entity(after.entities().at("shared-corner"));
        require(mapped.constraint->bindings[0].segment_id=="fresh-edge-0" && mapped.constraint->bindings[1].segment_id=="fresh-edge-1" &&
            mapped.constraint->bindings[0].vertex_id=="fresh-corner-1","shared-corner constraint mapping must follow explicitly reviewed children");
        const auto& actual=after.entities().at("area");
        for(const auto* key:{"name","factor_expression","factor_numerator","factor_denominator","appraisal_facts","deduction_ids"})require(actual.properties.at(key)==owner.properties.at(key),"fresh topology must preserve measured-area facts, factors and deduction references");
        require(after.entities().at("deduction")==before.entities().at("deduction"),"fresh topology must retain exact contained deduction geometry");
        require(actual.extensions.at("vendor_style")==owner.extensions.at("vendor_style"),"fresh topology must preserve owner style");
        if(authored)require(actual.extensions.at("boundary_geometry_derivation").at("source_boundary_authoring")==owner.properties.at("boundary_authoring"),"fresh source redraw must archive original authored receipts exactly");
        else require(actual.extensions.at("boundary_geometry_derivation").at("version")==2,"plain identified owners must retain their explicit topology origin");
        auto missing=wire;missing["replacement_child_mapping"]["vertices"]=nlohmann::json::object();
        const auto missing_edit=decode_boundary_geometry_edit(missing);
        rejected([&]{(void)Document::preview_command(before,EditBoundaryGeometry{before.revision(),missing_edit});},DocumentErrorCode::invalid_entity,"missing reviewed reference decisions must refuse same-count fresh redraw");
        const auto refuses_review=[&](const nlohmann::json& changed_wire) {
            rejected([&]{(void)Document::preview_command(before,EditBoundaryGeometry{before.revision(),decode_boundary_geometry_edit(changed_wire)});},
                DocumentErrorCode::invalid_entity,"invalid fresh-topology identities or reference decisions must refuse atomically");
            require(document.snapshot().entities()==after.entities(),"refused detached review must preserve the accepted document");
        };
        malformed=wire;malformed["replacement_segments"][0]["segment_id"]=old_boundary.segments[0].start_vertex_id;refuses_review(malformed);
        malformed=wire;malformed["replacement_child_mapping"]["segments"][old_boundary.segments[2].segment_id]="fresh-edge-2";refuses_review(malformed);
        malformed=wire;malformed["replacement_child_mapping"]["segments"][old_boundary.segments[0].start_vertex_id]="fresh-edge-0";refuses_review(malformed);
        malformed=wire;malformed["replacement_child_mapping"]["segments"][old_boundary.segments[1].segment_id]="fresh-edge-0";refuses_review(malformed);
        malformed=wire;malformed["replacement_child_mapping"]["segments"][old_boundary.segments[0].segment_id]="missing-new-edge";refuses_review(malformed);
        malformed=wire;malformed["replacement_removed_reference_ids"].push_back("unaffected-reference");refuses_review(malformed);
        auto retained_lock=wire;retained_lock["replacement_removed_reference_ids"]=nlohmann::json::array();
        retained_lock["replacement_child_mapping"]["vertices"][old_boundary.segments[0].start_vertex_id]="fresh-corner-0";
        require(std::abs(segment_length(replacement.segments[0].segment)-length.length->metres)>1e-3,
            "retained length-lock refusal fixture must actually change the locked endpoint distance");
        const auto accepted_digest=document_snapshot_digest(document.snapshot());
        rejected([&]{(void)Document::preview_command(before,EditBoundaryGeometry{before.revision(),decode_boundary_geometry_edit(retained_lock)});},DocumentErrorCode::constraint_violation,"retained incompatible length lock must refuse rather than silently move geometry");
        require(document_snapshot_digest(document.snapshot())==accepted_digest && document_snapshot_digest(before)==digest,
            "retained hard-lock refusal must preserve accepted state and detached review source exactly");
        auto fork=Document::fork(after);require(fork.snapshot().entities()==after.entities(),"fresh-topology history must fork exactly");
        document.undo(document.revision());require(document.snapshot().entities()==before.entities() && ProjectStore::required_format_version(document.snapshot())==17,"undo retains fresh-topology redo intent and reader seventeen");
        document.redo(document.revision());require(document.snapshot().entities()==after.entities(),"redo restores exact fresh topology and annotations");
        TempDirectory temp;(void)ProjectStore::save(temp.path/"fresh.bldproj",after);
        auto reopened=ProjectStore::load(temp.path/"fresh.bldproj");require(reopened.document.snapshot().entities()==after.entities() && stored_version(temp.path/"fresh.bldproj")==17,"fresh topology saves/reopens under native seventeen");
        extract_project(after,temp.path/"exchange");std::ifstream exported(temp.path/"exchange"/"project.json");
        require(nlohmann::json::parse(exported).at("exchange_version")==15,"fresh topology requires exchange fifteen");
    }
}

void generic_plain_fresh_topology_retains_explicit_origin() {
    auto owner=rectangle();owner.properties.erase("boundary_authoring");
    auto document=Document::create({owner});const auto before=document.snapshot();
    auto replacement=decode_identified_boundary_entity(owner);
    for(std::size_t i=0;i<replacement.segments.size();++i) {
        auto& edge=replacement.segments[i];edge.segment_id="generic-new-edge-"+std::to_string(i);
        edge.start_vertex_id="generic-new-corner-"+std::to_string(i);edge.end_vertex_id="generic-new-corner-"+std::to_string((i+1)%4);
        if(edge.segment.start.x==10)edge.segment.start.x=12;
        if(edge.segment.end.x==10)edge.segment.end.x=12;
    }
    BoundaryGeometryEdit edit;edit.boundary_id=owner.id;edit.target_id=owner.id;edit.kind=BoundaryGeometryEditKind::redefine_boundary;
    edit.replacement_segments=encode_identified_boundary_entity(replacement).properties.at("segments");edit.fresh_topology=true;
    const auto wire=encode_boundary_geometry_edit(edit);
    require(wire.at("version")==4 && wire.at("replacement_wall_source_ids").empty() && decode_boundary_geometry_edit(wire)==edit,
        "generic fresh topology must round-trip strict v4 with explicitly empty wall sources");
    const auto preview=Document::preview_command(before,EditBoundaryGeometry{before.revision(),edit});
    require(document.snapshot().entities()==before.entities(),"generic redraw preview is detached");
    document.apply(EditBoundaryGeometry{before.revision(),edit});const auto after=document.snapshot();
    require(after.entities()==preview.entities() && !after.entities().at(owner.id).properties.contains("wall_measurement_source"),
        "generic fresh redraw creates exact geometry without inventing wall provenance");
    const auto& proof=after.entities().at(owner.id).extensions.at("boundary_geometry_derivation");
    require(proof.at("version")==2 && proof.at("source_boundary").at("segments")==owner.properties.at("segments") &&
        proof.at("operations").size()==1,"plain fresh redraw archives its exact original identified topology");
    std::vector<Entity> imported_values;for(const auto& [id,value]:after.entities()){(void)id;imported_values.push_back(value);}
    auto imported=Document::create(imported_values);
    require(imported.snapshot().history().size()==1 && ProjectStore::required_format_version(imported.snapshot())==17 &&
        Document::fork(imported.snapshot()).snapshot().entities()==after.entities(),"generic imported fresh topology replays exactly and retains reader seventeen");
    document.undo(document.revision());require(document.snapshot().entities()==before.entities(),"generic fresh topology undo restores original children");
    document.redo(document.revision());require(document.snapshot().entities()==after.entities(),"generic fresh topology redo restores new children");
    TempDirectory temp;(void)ProjectStore::save(temp.path/"generic.bldproj",imported.snapshot());
    require(stored_version(temp.path/"generic.bldproj")==17 && ProjectStore::load(temp.path/"generic.bldproj").document.snapshot().entities()==after.entities(),
        "generic entity-only fresh redraw persists under native seventeen");
    extract_project(imported.snapshot(),temp.path/"generic-exchange");std::ifstream exported(temp.path/"generic-exchange"/"project.json");
    require(nlohmann::json::parse(exported).at("exchange_version")==15,"generic imported fresh redraw requires exchange fifteen");
}
}
int main(int argc,char** argv) {
    sketch::testing::noninteractive_errors();
    try {
        if(argc>1 && std::string_view(argv[1])=="--fresh-topology-only") {same_count_fresh_exterior_topology_is_reviewed_and_atomic();generic_plain_fresh_topology_retains_explicit_origin();std::cout<<"fresh topology workflows passed\n";return 0;}
        same_count_fresh_exterior_topology_is_reviewed_and_atomic();
        generic_plain_fresh_topology_retains_explicit_origin();
        typed_arc_edit_history_and_rejection();native_and_exchange_preserve_arc_proof_and_reader_floor();fixed_physical_arc_length_prevents_curvature_change();
        std::cout<<"boundary_arc_edit_tests passed\n";return 0;
    }catch(const std::exception& error){std::cerr<<"boundary_arc_edit_tests: "<<error.what()<<'\n';return 1;}
}
