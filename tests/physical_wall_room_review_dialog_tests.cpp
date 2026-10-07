#include "support/detached_document_snapshot.hpp"
#include "sketch/desktop/physical_wall_room_review_dialog.hpp"
#include "sketch/physical_wall_room_review.hpp"
#include "sketch/document_digest.hpp"
#include "sketch/project_store.hpp"
#include "sketch/boundary_dimension.hpp"
#include "sketch/constraint_entity.hpp"
#include "../src/desktop/plan_canvas.hpp"
#include "support/noninteractive_errors.hpp"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QEvent>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPixmap>
#include <QPointer>
#include <QPushButton>
#include <QStandardPaths>
#include <QTableWidget>
#include <QTemporaryDir>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace {
using namespace sketch;using namespace sketch::desktop;using Json=nlohmann::json;
void require(bool ok,const char* why) { if (!ok) throw std::runtime_error(why); }
template<class T> T* control(PhysicalWallRoomReviewDialog& dialog,const QString& name) {
    auto* value=dialog.findChild<T*>(name);require(value,"room review control is present");return value;
}
void choose(PhysicalWallRoomReviewDialog& dialog,const QString& name,const QString& data) {
    auto* combo=control<QComboBox>(dialog,name);const auto index=combo->findData(data);
    require(index>=0,"explicit room review choice is present");combo->setCurrentIndex(index);QCoreApplication::processEvents();
    // A table rebuild retires its cell widgets with deleteLater(). Direct
    // processEvents calls do not drain that queue as the native event loop does;
    // stale widgets otherwise retain the same objectNames as their replacements.
    QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
}
Entity entity(std::string id,std::string type,Json p=Json::object()) { return {std::move(id),std::move(type),std::move(p),false,Json::object()}; }
Entity wall(std::string id,Vec2 a,Vec2 b) {
    return entity(std::move(id),"wall",{{"baseline",{{"start",{a.x,a.y}},{"end",{b.x,b.y}},{"sweep_radians",0}}},
        {"thickness_m",0.2},{"height_m",3},{"elevation_m",0},{"layer_id","layer"}});
}
std::vector<Entity> fixture(bool divided=false) {
    std::vector<Entity> values{entity("property","property"),entity("building","building",{{"property_id","property"}}),
        entity("floor","floor",{{"building_id","building"}}),entity("layer","layer",{{"floor_id","floor"}}),
        wall("bottom",{0,0},{4,0}),wall("right",{4,0},{4,3}),wall("top",{4,3},{0,3}),wall("left",{0,3},{0,0})};
    if (divided) values.push_back(wall("divider",{2,0},{2,3}));return values;
}
std::vector<Entity> copied(const DocumentSnapshot& s) {
    std::vector<Entity> result;for (const auto& [id,e]:s.entities()) { (void)id;result.push_back(e); }return result;
}
void show(PhysicalWallRoomReviewDialog& dialog) {
    dialog.setAttribute(Qt::WA_DontShowOnScreen);dialog.resize(1100,900);dialog.show();QCoreApplication::processEvents();
}
void capture(PhysicalWallRoomReviewDialog& dialog,const char* name) {
    const auto directory=qEnvironmentVariable("VERTEX_TEST_CAPTURE_DIR");if (directory.isEmpty()) return;
    QCoreApplication::processEvents();require(QDir().mkpath(directory) && dialog.grab().save(QDir(directory).filePath(QString::fromLatin1(name))),
        "actual complete room review preview capture saves");
}
void pick(PhysicalWallRoomReviewDialog& dialog,std::size_t index,Vec2 point) {
    control<QPushButton>(dialog,QStringLiteral("freshPick:%1").arg(index))->click();
    auto* canvas=dynamic_cast<PlanCanvas*>(control<QWidget>(dialog,"physicalRoomReviewCanvas"));require(canvas,"real PlanCanvas preview exists");
    const auto center=canvas->viewCenter();const auto scale=canvas->viewScale();
    const QPointF pixel(canvas->rect().center().x()+(point.x-center.x)*scale,canvas->rect().center().y()-(point.y-center.y)*scale);
    QMouseEvent press(QEvent::MouseButtonPress,pixel,canvas->mapToGlobal(pixel.toPoint()),Qt::LeftButton,Qt::LeftButton,Qt::NoModifier);
    QApplication::sendEvent(canvas,&press);
    QMouseEvent release(QEvent::MouseButtonRelease,pixel,canvas->mapToGlobal(pixel.toPoint()),Qt::LeftButton,Qt::NoButton,Qt::NoModifier);
    QApplication::sendEvent(canvas,&release);QCoreApplication::processEvents();
}
void split_apply_undo_reopen() {
    auto original=Document::create(fixture());const auto initial=prepare_physical_wall_rooms(original.snapshot(),"bottom",{0},"office");
    const auto old=initial.entity_changes.at(0).entity.id;original.apply(initial);auto values=copied(original.snapshot());
    values.push_back(wall("divider",{2,0},{2,3}));auto document=Document::create(values);const auto source=document.snapshot();
    const auto report=physical_wall_room_correspondence(source,"bottom");std::optional<std::size_t> left,right;
    for (const auto& space:report.fresh) {
        if (PlanCanvas::containsAreaPoint(space.boundary,{1,1})) left=space.index;
        if (PlanCanvas::containsAreaPoint(space.boundary,{3,1})) right=space.index;
    }
    require(left && right && *left!=*right,"independent left/right interior points identify distinct split pieces");
    PhysicalWallRoomReviewDialog dialog(source,"bottom",true,[&]{return document.snapshot();});show(dialog);
    require(control<QTableWidget>(dialog,"physicalRoomReviewRetained")->rowCount()==1 &&
        control<QTableWidget>(dialog,"physicalRoomReviewFresh")->rowCount()==2,"real split dialog shows complete old and fresh rows");
    auto* apply=control<QPushButton>(dialog,"physicalRoomReviewApply");require(!apply->isEnabled(),"all decisions start unresolved");
    choose(dialog,QStringLiteral("retainedDecision:")+QString::fromStdString(old),"retain");
    choose(dialog,QStringLiteral("freshAssignment:%1").arg(*left),QStringLiteral("retained:")+QString::fromStdString(old));
    pick(dialog,*left,{0,0});require(!apply->isEnabled(),"boundary/outside click cannot certify an interior witness");
    pick(dialog,*left,{1,1});require(!apply->isEnabled(),"unresolved second split piece cannot be silently abandoned");
    choose(dialog,QStringLiteral("freshAssignment:%1").arg(*right),"create");control<QLineEdit>(dialog,QStringLiteral("freshName:%1").arg(*right))->setText("New bedroom");
    control<QLineEdit>(dialog,QStringLiteral("freshClassification:%1").arg(*right))->setText("bedroom");pick(dialog,*right,{3,1});
    if (!apply->isEnabled()) throw std::runtime_error("complete split preview: "+dialog.lastError().toStdString());
    require(document_snapshot_digest(document.snapshot())==document_snapshot_digest(source),"preview never mutates complete source state");
    auto* canvas=dynamic_cast<PlanCanvas*>(control<QWidget>(dialog,"physicalRoomReviewCanvas"));
    require(canvas->entities().size()>=3,"real preview retains old outline and both fresh spaces");
    capture(dialog,"physical-room-review-split-preview.png");
    apply->click();require(dialog.result()==QDialog::Accepted && dialog.acceptedCommand().has_value(),"explicit complete batch is accepted");
    const auto command=*dialog.acceptedCommand();const auto intent=decode_physical_wall_room_review_intent(command.room_review_intent);
    const auto new_id=intent.fresh.at(*right).room_id;document.apply(command);const auto after=document.snapshot();
    require(after.revision()==source.revision()+1 && after.entities().at(old).properties.at("classification")=="office" &&
        after.entities().at(new_id).properties.at("classification")=="bedroom","one atomic command preserves old facts and explicit new facts");
    require(document.undo(document.revision())>source.revision() && document.snapshot().entities()==source.entities(),"one Undo restores entire split source");
    document.redo(document.revision());require(document.snapshot().entities()==after.entities(),"one Redo restores entire reviewed result");
    QTemporaryDir files;require(files.isValid(),"native save fixture directory exists");
    const auto path=std::filesystem::path(files.filePath("review.bldproj").toStdWString());(void)ProjectStore::save(path,document.snapshot());
    auto reopened=ProjectStore::load(path);require(reopened.document.snapshot().entities()==after.entities(),"save/reopen replays whole reviewed command");
    reopened.document.undo(reopened.document.revision());require(reopened.document.snapshot().entities()==source.entities(),"reopened one Undo restores all dispositions");
}
void merge_reference_decisions() {
    auto original=Document::create(fixture(true));const auto initial=prepare_physical_wall_rooms(original.snapshot(),"bottom",{0,1},"office");
    original.apply(initial);auto values=copied(original.snapshot());
    values.erase(std::remove_if(values.begin(),values.end(),[](const auto& e){return e.id=="divider";}),values.end());
    const auto kept=initial.entity_changes.at(0).entity.id,retired=initial.entity_changes.at(1).entity.id;
    BoundaryDimension dimension;dimension.id="retired-area";dimension.boundary_id=retired;dimension.kind=BoundaryDimensionKind::area;dimension.text_position={3,1};
    values.push_back(encode_boundary_dimension_entity(dimension));
    const auto graph=RoomRelationshipSnapshot::create({{kept,RoomReferenceKind::room_boundary},{retired,RoomReferenceKind::room_boundary}},
        {{kept,retired,RoomRelationKind::independent}});
    values.push_back(entity("relationships","room_relationships",{{"model",graph.to_json()}}));
    auto document=Document::create(values);const auto source=document.snapshot();
    PhysicalWallRoomReviewDialog dialog(source,"bottom",true,[&]{return document.snapshot();});show(dialog);
    choose(dialog,QStringLiteral("retainedDecision:")+QString::fromStdString(kept),"retain");
    choose(dialog,QStringLiteral("retainedDecision:")+QString::fromStdString(retired),"retire");
    choose(dialog,"freshAssignment:0",QStringLiteral("retained:")+QString::fromStdString(kept));pick(dialog,0,{2,1});
    auto* apply=control<QPushButton>(dialog,"physicalRoomReviewApply");require(!apply->isEnabled(),"retirement cannot silently remove a dimension or graph reference");
    choose(dialog,"referenceDecision:retired-area","remove");require(!apply->isEnabled(),"graph membership/rows require separate acknowledgement");
    for (auto* check:dialog.findChildren<QCheckBox*>()) if (check->property("required").toBool()) check->setChecked(true);
    if (!apply->isEnabled()) throw std::runtime_error("complete merge preview: "+dialog.lastError().toStdString());
    capture(dialog,"physical-room-review-merge-preview.png");
    apply->click();require(dialog.acceptedCommand().has_value(),"complete merge accepted");document.apply(*dialog.acceptedCommand());
    require(!document.snapshot().entities().contains(retired) && !document.snapshot().entities().contains(dimension.id),"explicit retirement and dimension deletion published together");
    const auto remaining=RoomRelationshipSnapshot::from_json(document.snapshot().entities().at("relationships").properties.at("model"));
    require(remaining.references().size()==1 && remaining.references().at(0).id==kept && remaining.relations().empty(),"precisely acknowledged graph rows removed");
    document.undo(document.revision());require(document.snapshot().entities()==source.entities(),"one merge Undo restores rooms and all references");
}
void cancel_reset_and_same_head_fence() {
    const auto asset=Asset::create("review-asset","application/octet-stream",{std::byte{1}});
    auto document=Document::create(fixture(),{asset});const auto source=document.snapshot();auto current=source;
    PhysicalWallRoomReviewDialog dialog(source,"bottom",true,[&]{return current;});show(dialog);
    choose(dialog,"freshAssignment:0","create");control<QLineEdit>(dialog,"freshName:0")->setText("Explicit room");
    control<QLineEdit>(dialog,"freshClassification:0")->setText("office");pick(dialog,0,{2,1});
    require(control<QPushButton>(dialog,"physicalRoomReviewApply")->isEnabled(),"new-only room review is supported");
    QPointer<QComboBox> previous_assignment=control<QComboBox>(dialog,"freshAssignment:0");
    choose(dialog,"physicalRoomReviewSource","right");
    auto* fresh=control<QTableWidget>(dialog,"physicalRoomReviewFresh");
    auto* current_assignment=qobject_cast<QComboBox*>(fresh->cellWidget(0,1));
    require(previous_assignment.isNull() && current_assignment &&
        current_assignment==control<QComboBox>(dialog,"freshAssignment:0"),
        "source wall change replaces retired cell controls with the current visible row");
    require(current_assignment->currentIndex()==0 &&
        !control<QPushButton>(dialog,"physicalRoomReviewApply")->isEnabled(),"changing source wall resets all explicit decisions");
    require(control<QLineEdit>(dialog,"freshName:0")->text().isEmpty() &&
        control<QLineEdit>(dialog,"freshClassification:0")->text().isEmpty() &&
        !control<QPushButton>(dialog,"freshPick:0")->isEnabled() &&
        control<QPushButton>(dialog,"freshPick:0")->text()=="Pick inside" && !dialog.acceptedCommand(),
        "source wall change also clears names, classifications, interior witnesses and prepared command");
    dialog.reject();require(!dialog.acceptedCommand() && document_snapshot_digest(document.snapshot())==document_snapshot_digest(source),"Cancel changes no history, assets or saved state");
    for (const bool change_asset:{false,true}) {
        PhysicalWallRoomReviewDialog stale(source,"bottom",true,[&]{return current;});show(stale);
        choose(stale,"freshAssignment:0","unclassified");require(control<QPushButton>(stale,"physicalRoomReviewApply")->isEnabled(),"explicit leave-unclassified completes new-only review");
        sketch::test::DetachedDocumentSnapshotFixture altered(source);auto& record=altered.history().front();
        if (change_asset) record.assets.at(asset.id)=Asset::create(asset.id,asset.media_type,{std::byte{2}});
        else record.entities.at("property").extensions["same_head_replacement"]=true;
        current=altered;stale.accept();require(!stale.acceptedCommand() && stale.result()!=QDialog::Accepted && !stale.lastError().isEmpty(),"same-head entities/assets replacement cannot accept stale preview");
        current=source;
    }
}
void explicit_constraint_mapping() {
    auto original=Document::create(fixture());const auto initial=prepare_physical_wall_rooms(original.snapshot(),"bottom",{0},"office");
    original.apply(initial);const auto room_id=initial.entity_changes.at(0).entity.id;auto values=copied(original.snapshot());
    const auto old=decode_identified_boundary_entity(original.snapshot().entities().at(room_id)).segments.at(0);
    PersistentConstraint anchor;anchor.id="kept-anchor";anchor.relation=ConstraintRelationKind::fixed_anchor;anchor.anchor=old.segment.start;
    anchor.bindings={{room_id,WallEndpointRole::start,old.segment_id,old.start_vertex_id}};
    values.push_back(encode_constraint_entity(anchor));values.push_back(wall("divider",{2,0},{2,3}));
    auto document=Document::create(values);const auto source=document.snapshot();const auto report=physical_wall_room_correspondence(source,"bottom");
    std::optional<std::pair<std::size_t,std::size_t>> destination;
    for (const auto& space:report.fresh) for (std::size_t edge=0;edge<space.boundary.size();++edge) {
        const auto p=space.boundary[edge].start;if (std::hypot(p.x-old.segment.start.x,p.y-old.segment.start.y)<1e-9) destination=std::pair{space.index,edge};
    }
    require(destination.has_value(),"saved anchor corner survives in an independently identified split component");
    PhysicalWallRoomReviewDialog dialog(source,"bottom",true,[&]{return document.snapshot();});show(dialog);
    choose(dialog,QStringLiteral("retainedDecision:")+QString::fromStdString(room_id),"retain");
    for (std::size_t i=0;i<report.fresh.size();++i) choose(dialog,QStringLiteral("freshAssignment:%1").arg(i),
        i==destination->first?QStringLiteral("retained:")+QString::fromStdString(room_id):QStringLiteral("unclassified"));
    const auto bounds=boundary_bounds(report.fresh.at(destination->first).boundary);
    pick(dialog,destination->first,{(bounds.minimum.x+bounds.maximum.x)/2,(bounds.minimum.y+bounds.maximum.y)/2});
    choose(dialog,"referenceDecision:kept-anchor","keep");auto* apply=control<QPushButton>(dialog,"physicalRoomReviewApply");
    require(!apply->isEnabled(),"Keep cannot infer its original edge and corner mappings");
    const auto prefix=QStringLiteral("mapping:")+QString::fromStdString(room_id);
    control<QComboBox>(dialog,prefix+":segment:"+QString::fromStdString(old.segment_id))->setCurrentIndex(static_cast<int>(destination->second+1));
    control<QComboBox>(dialog,prefix+":vertex:"+QString::fromStdString(old.start_vertex_id))->setCurrentIndex(static_cast<int>(destination->second+1));
    if (!apply->isEnabled()) throw std::runtime_error("complete explicit constraint mapping: "+dialog.lastError().toStdString());
    apply->click();require(dialog.acceptedCommand().has_value(),"explicit child map accepted");
    const auto intent=decode_physical_wall_room_review_intent(dialog.acceptedCommand()->room_review_intent);
    require(intent.kept_reference_ids==std::vector<std::string>{anchor.id} && intent.removed_reference_ids.empty(),"explicit Keep persisted without silent reference deletion");
    document.apply(*dialog.acceptedCommand());const auto mapped=decode_constraint_entity(document.snapshot().entities().at(anchor.id)).constraint->bindings.at(0);
    require(mapped.owner_id==room_id && mapped.segment_id!=old.segment_id && mapped.vertex_id!=old.start_vertex_id && mapped.role==WallEndpointRole::start,
        "actual mapped constraint retains identity/role and uses freshly reviewed children");
    document.undo(document.revision());require(document.snapshot().entities()==source.entities(),"one Undo restores exact mapped-reference source");
}
void kept_dimensions_show_current_clear_values() {
    auto original=Document::create(fixture());const auto initial=prepare_physical_wall_rooms(original.snapshot(),"bottom",{0},"office");
    original.apply(initial);const auto room_id=initial.entity_changes.at(0).entity.id;auto values=copied(original.snapshot());
    const auto edge=decode_identified_boundary_entity(original.snapshot().entities().at(room_id)).segments.at(0);
    BoundaryDimension area_dimension;area_dimension.id="kept-area";area_dimension.boundary_id=room_id;
    area_dimension.kind=BoundaryDimensionKind::area;area_dimension.text_position={2,2};values.push_back(encode_boundary_dimension_entity(area_dimension));
    BoundaryDimension automatic;automatic.id="automatic-length";automatic.boundary_id=room_id;automatic.segment_id=edge.segment_id;
    automatic.placement=BoundaryDimensionPlacement::automatic;automatic.automatic_placement_version=1;automatic.text_position={2,-.5};
    values.push_back(encode_boundary_dimension_entity(automatic));values.push_back(wall("island",{1,1},{3,1}));
    auto document=Document::create(values);const auto source=document.snapshot();
    PhysicalWallRoomReviewDialog dialog(source,"bottom",true,[&]{return document.snapshot();});show(dialog);
    choose(dialog,QStringLiteral("retainedDecision:")+QString::fromStdString(room_id),"retain");
    choose(dialog,"freshAssignment:0",QStringLiteral("retained:")+QString::fromStdString(room_id));
    pick(dialog,0,{2,1});require(!control<QPushButton>(dialog,"physicalRoomReviewApply")->isEnabled(),"wall-island material cannot provide a dimension-repair witness");
    pick(dialog,0,{2,2});choose(dialog,"referenceDecision:kept-area","keep");choose(dialog,"referenceDecision:automatic-length","keep");
    auto* apply=control<QPushButton>(dialog,"physicalRoomReviewApply");
    if (!apply->isEnabled()) throw std::runtime_error("kept physical-room dimension preview: "+dialog.lastError().toStdString());
    auto* preview=dynamic_cast<PlanCanvas*>(control<QWidget>(dialog,"physicalRoomReviewCanvas"));
    require(std::any_of(preview->labels().begin(),preview->labels().end(),[](const auto& label){return label.id=="kept-area" && label.text==QStringLiteral("10.24 m²");}),
        "actual preview displays net clear area with island hole instead of retained outer area");
    capture(dialog,"physical-room-review-kept-dimensions.png");apply->click();require(dialog.acceptedCommand().has_value(),"explicit kept dimensions accepted");
    const auto intent=decode_physical_wall_room_review_intent(dialog.acceptedCommand()->room_review_intent);
    require(intent.kept_reference_ids.size()==2 && intent.retained.at(0).replacement_dimension_ids.size()==4,
        "kept area and regenerated automatic dimensions have explicit persisted decisions");
    document.apply(*dialog.acceptedCommand());const auto after=document.snapshot();
    require(after.entities().contains(area_dimension.id) && !after.entities().contains(automatic.id),"area identity retained while old automatic edge dimension retires");
    const auto resolved=resolve_boundary_dimension(*decode_boundary_dimension_entity(after.entities().at(area_dimension.id)).dimension,after);
    require(std::abs(resolved.area_square_metres-10.24)<1e-9,"kept physical-room dimension resolves the independently expected current net area");
    for (const auto& id:intent.retained.at(0).replacement_dimension_ids) require(after.entities().contains(id),"every regenerated automatic edge dimension exists");
    document.undo(document.revision());require(document.snapshot().entities()==source.entities(),"one Undo restores dimension identities and original stale source evidence");
}
}
int main(int argc,char** argv) {
    sketch::testing::noninteractive_errors();QStandardPaths::setTestModeEnabled(true);QApplication application(argc,argv);
    try { split_apply_undo_reopen();merge_reference_decisions();cancel_reset_and_same_head_fence();explicit_constraint_mapping();kept_dimensions_show_current_clear_values(); }
    catch (const std::exception& e) { std::cerr<<"physical_wall_room_review_dialog_tests: "<<e.what()<<'\n';return 1; }
    std::cout<<"physical wall room review dialog checks passed\n";
}
