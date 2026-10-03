#include "sketch/desktop/main_window.hpp"
#include "sketch/measurement_linework.hpp"
#include "sketch/boundary_entity.hpp"
#include "sketch/project_store.hpp"
#include "sketch/measurement_linework_source.hpp"
#include "sketch/appraisal_document.hpp"
#include "sketch/sheet_view_entity_codec.hpp"
#include "../src/desktop/plan_canvas.hpp"
#include "support/noninteractive_errors.hpp"

#include <QApplication>
#include <QFont>
#include <QFontDatabase>
#include <QPdfDocument>
#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QElapsedTimer>
#include <QMouseEvent>
#include <QComboBox>
#include <QTabWidget>
#include <QLabel>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QUuid>
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {
using namespace sketch;
using namespace sketch::desktop;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
Entity stroke(std::string id, const std::vector<Vec2>& points, bool closed) {
    MeasurementLinework model; model.stroke_id=id; model.anchor=points.front(); model.closed=closed;
    for (std::size_t i=1; i<points.size(); ++i) {
        const auto segment=id+":e"+std::to_string(i);
        ConstructionReceipt receipt; receipt.segment_id=segment; receipt.kind=BoundaryConstructionKind::line_to_point;
        receipt.start=points[i-1]; receipt.chord_end=points[i];
        model.edges.push_back({segment,id+":v"+std::to_string(i-1),
            closed&&i==points.size()-1?id+":v0":id+":v"+std::to_string(i),receipt});
    }
    return {id,"measurement_linework",{{"property_id","p"},{"building_id","b"},{"floor_id","f"},
        {"layer_id","l"},{"model",encode_measurement_linework_model(model)}},true};
}
std::shared_ptr<Document> fixture() {
    return std::make_shared<Document>(Document::create({
        {"p","property",{{"name","Measured property"}},false},
        {"b","building",{{"property_id","p"}},false},
        {"f","floor",{{"building_id","b"}},false},
        {"l","layer",{{"floor_id","f"},{"name","Areas"}},false},
        stroke("outline",{{0,0},{4,0},{4,4},{0,4},{0,0}},true),
        stroke("separator",{{2,-1},{2,5}},false)
    }));
}
void test_define_areas_and_history() {
    MainWindow window(fixture()); window.setAttribute(Qt::WA_DontShowOnScreen,true);
    window.resize(1400,900); window.show(); QCoreApplication::processEvents(QEventLoop::AllEvents,50);
    auto* canvas=dynamic_cast<PlanCanvas*>(window.findChild<QWidget*>(QStringLiteral("measurementPlanCanvas")));
    require(canvas,"canvas exists"); require(window.selectEntity(QStringLiteral("outline")),"select measured outline");
    const auto source=window.document().snapshot();
    require(window.detectRoomBoundariesFromExistingWalls(QStringLiteral("living"),source.revision()+1).isEmpty() &&
        window.document().snapshot().entities()==source.entities(),"stale area detection leaves measured source unchanged");
    const auto ids=window.detectRoomBoundariesFromExistingWalls(QStringLiteral("living"),source.revision());
    require(ids.size()==2,"crossing separator must define two measured areas on the native canvas");
    const auto defined=window.document().snapshot();
    require(defined.revision()==source.revision()+1,"all derived areas commit in one undo step");
    require(defined.entities().at("outline")==source.entities().at("outline") &&
        defined.entities().at("separator")==source.entities().at("separator"),"area creation never rewrites source strokes");
    double total=0;
    for (const auto& id:ids) {
        const auto& area=defined.entities().at(id.toStdString());
        require(area.type=="measurement_boundary" && area.properties.at("classification")=="living",
            "derived areas are actual classified measurement boundaries");
        require(area.properties.at("property_id")=="p" && area.properties.at("layer_id")=="l",
            "derived areas retain source drawing context");
        require(area.extensions.contains("measurement_linework_sources"),"derived areas retain edge source lineage");
        total+=std::abs(signed_area(boundary_geometry(decode_identified_boundary_entity(area))));
    }
    require(std::abs(total-16)<1e-10,"separated measured areas exactly cover original sixteen square metres");
    require(window.undoCommand() && window.document().snapshot().entities()==source.entities(),"one Undo removes both areas and preserves strokes");
    require(window.redoCommand() && window.document().snapshot().entities()==defined.entities(),"Redo restores exact derived boundaries and lineage");
    require(window.selectEntity(QStringLiteral("outline")),"select source again");
    const auto before_repeat=window.document().snapshot();
    require(window.detectRoomBoundariesFromExistingWalls(QStringLiteral("living")).isEmpty() &&
        window.document().snapshot().entities()==before_repeat.entities(),"repeated definition does not silently double-count identical faces");
    QTemporaryDir temp; const auto path=temp.filePath("areas.bldproj");
    require(window.saveProjectAs(path) && window.openProject(path),"linework and defined areas save and reopen");
    require(window.document().snapshot().entities()==defined.entities(),"native storage preserves source lineage and exact boundary geometry");
    bool exterior_bottom=false;
    for (const auto& label:canvas->labels()) if (label.id==QStringLiteral("outline") && label.automatic_linear_placement) {
        const auto& placement=*label.automatic_linear_placement;
        if(placement.anchor.start.y==0 && placement.anchor.end.y==0) {
            require(placement.outward_normal.y<0 && label.position.y<0,"closed measured outline places its bottom dimension on the exterior");
            require(label.text.contains(QStringLiteral("13'")) && !label.text.contains(QStringLiteral("157.480")),
                "permanent measured dimensions use readable project units rather than unlimited decimal inches");
            exterior_bottom=true;
        }
    }
    require(exterior_bottom,"closed measured outline exposes its exterior dimension");
    const auto pdf_path=temp.filePath("measured-areas.pdf");
    require(window.exportDraftPdf(pdf_path),"defined measured areas and original strokes export through shared PDF output");
    QPdfDocument pdf;
    require(pdf.load(pdf_path)==QPdfDocument::Error::None && pdf.pageCount()>0,"measured output is a readable PDF");
    const auto rendered=pdf.render(0,pdf.pagePointSize(0).scaled(QSizeF(1200,900),Qt::KeepAspectRatio).toSize());
    require(!rendered.isNull(),"shared measured PDF output renders");
    const auto capture=qEnvironmentVariable("VERTEX_TEST_CAPTURE_DIR");
    if(!capture.isEmpty()) {
        QDir().mkpath(capture);
        require(window.grab().save(QDir(capture).filePath("measured-lines-defined-areas.png")),"save native canvas capture");
        require(rendered.save(QDir(capture).filePath("measured-lines-output.png")),"save actual measured PDF output capture");
    }
}
void test_ansi_profile_dimensions() {
    auto document=fixture();
    const auto source=document->snapshot();
    auto property=source.entities().at("p");
    property.properties["calculation_workflow"]="appraisal";
    property.properties["appraisal_policy"]={{"policy_kind","ansi_z765_2021"},{"version",1},
        {"property_kind","detached_single_family"},{"measurement_basis","exterior"}};
    document->apply(ApplyEntityChanges{source.revision(),{EntityChange::upsert(property)},{},"Set dimension presentation profile"});
    MainWindow window(document); window.setMetricUnits(true);
    auto* canvas=dynamic_cast<PlanCanvas*>(window.findChild<QWidget*>(QStringLiteral("measurementPlanCanvas")));
    require(canvas,"ANSI dimension canvas exists");
    bool canonical=false;
    for(const auto& label:canvas->labels()) if(label.id==QStringLiteral("outline"))
        canonical=canonical || label.text==QStringLiteral("13.1 ft (4.000 m)");
    require(canonical,"ANSI-profile measured dimensions remain tenths of feet with supplemental metric display");
    require(document->snapshot().entities().at("outline")==source.entities().at("outline"),
        "dimension display rounding does not change exact retained measurements");
}
void mouse(PlanCanvas& canvas,QEvent::Type type,QPointF point) {
    QMouseEvent event(type,point,canvas.mapToGlobal(point.toPoint()),
        type==QEvent::MouseMove?Qt::NoButton:Qt::LeftButton,
        type==QEvent::MouseButtonRelease?Qt::NoButton:Qt::LeftButton,Qt::NoModifier);
    QApplication::sendEvent(&canvas,&event);
}
template<class Predicate> bool wait(Predicate ready) {
    QElapsedTimer timer;timer.start();
    do {QCoreApplication::processEvents(QEventLoop::AllEvents,20);if(ready())return true;}while(timer.elapsed()<10000);
    return false;
}
void test_canvas_source_move_and_live_gla() {
    MainWindow window(fixture());window.setAttribute(Qt::WA_DontShowOnScreen,true);
    window.resize(1400,900);window.show();QCoreApplication::processEvents();
    require(window.selectEntity("outline"),"select measured source for definition");
    const auto ids=window.detectRoomBoundariesFromExistingWalls("living");require(ids.size()==2,"actual source areas exist");
    auto snapshot=window.document().snapshot();
    auto property=snapshot.entities().at("p");property.properties["calculation_workflow"]="appraisal";
    property.properties["appraisal_policy"]={{"policy_kind","residential_declared"},{"version",1},
        {"property_kind","detached_single_family"},{"measurement_basis","exterior"}};
    auto floor=snapshot.entities().at("f");floor.properties["appraisal_facts"]={{"grade","above"}};
    std::vector<EntityChange> changes{EntityChange::upsert(property),EntityChange::upsert(floor)};
    QString left;
    for(const auto& id:ids) {
        auto area=snapshot.entities().at(id.toStdString());
        const bool dwelling=boundary_bounds(boundary_geometry(decode_identified_boundary_entity(area))).minimum.x==0;
        if(dwelling)left=id;
        area.properties["appraisal_facts"]={{"finish","finished"},{"access","direct_interior"},
            {"ceiling_eligibility","standard"},{"area_use",dwelling?"dwelling":"garage"},{"boundary_role","measured_area"}};
        changes.push_back(EntityChange::upsert(std::move(area)));
    }
    window.document().apply(ApplyEntityChanges{snapshot.revision(),std::move(changes),{},"Declare actual area facts"});
    require(window.selectEntity("separator"),"select separator to move");
    auto* canvas=dynamic_cast<PlanCanvas*>(window.findChild<QWidget*>("measurementPlanCanvas"));require(canvas,"move canvas exists");
    canvas->setOverviewMapEnabled(false);canvas->setSnapEnabled(false);canvas->setWallSnapEnabled(false);
    canvas->setViewTransform({2,2},80);
    const auto before=window.document().snapshot();
    const auto original_model=before.entities().at("separator").properties.at("model");
    const auto before_report=build_appraisal_document_report(before,"p",AreaUnit::square_metre);
    require(before_report.qualified && std::abs(before_report.calculation->property.gla().total.square_metres-8)<1e-10,
        "initial declared dwelling GLA is eight square metres");
    const auto center=QRectF(canvas->rect()).center();
    const QPointF grab=center+QPointF(0,80),target=grab+QPointF(80,0);
    mouse(*canvas,QEvent::MouseButtonPress,grab);mouse(*canvas,QEvent::MouseMove,target);
    require(wait([&]{return !canvas->entitiesMovePreviewPending() && !canvas->entitiesMovePreview().empty();}),
        "actual measured-line drag prepares a source and area proposal");
    require(window.document().snapshot().entities()==before.entities(),"drag preview never mutates persisted source or GLA");
    mouse(*canvas,QEvent::MouseButtonRelease,target);
    require(wait([&]{return window.document().revision()==before.revision()+1 && !canvas->entitiesMovePreviewPending();}),
        "actual measured-line release commits once");
    const auto moved=window.document().snapshot();
    const auto decoded=decode_measurement_linework_model(moved.entities().at("separator").properties.at("model"));
    require(decoded.supported() && decoded.model->schema_version==2 && replay_measurement_linework(*decoded.model).anchor.x==3,
        "drag applies world translation through retained receipt frame");
    require(moved.entities().at("separator").properties.at("model").at("segments")==original_model.at("segments"),
        "world move preserves original entered local receipts and stable identities");
    const auto checks=measurement_linework_source_checks(moved.entities());
    for(const auto& id:ids)require(checks.at(id.toStdString()).current,"every derived area agrees with moved source graph");
    const auto report=build_appraisal_document_report(moved,"p",AreaUnit::square_metre);
    require(report.qualified && std::abs(report.calculation->property.gla().total.square_metres-12)<1e-10,
        "actual source move automatically changes dwelling GLA to twelve square metres");
    require(moved.entities().at(left.toStdString()).properties.at("appraisal_facts")==before.entities().at(left.toStdString()).properties.at("appraisal_facts"),
        "live boundary update preserves the appraiser's declared classification facts");
    require(window.undoCommand() && window.document().snapshot().entities()==before.entities(),"one Undo restores sources areas and GLA facts together");
    require(window.redoCommand() && window.document().snapshot().entities()==moved.entities(),"Redo restores exact source frame and area consequences");
    QTemporaryDir directory;require(window.saveProjectAs(directory.filePath("moved.bldproj")) && window.openProject(directory.filePath("moved.bldproj")),
        "edited source frames and live areas save and reopen");
    require(window.document().snapshot().entities()==moved.entities(),"native reopen preserves exact edited sources and derived areas");
    auto* tabs=window.findChild<QTabWidget*>("sidebarTabs");require(tabs,"Details tabs exist after reopen");tabs->setCurrentIndex(2);
    auto* gla=window.findChild<QLabel*>("appraisalDetailsGla");
    require(gla && gla->text().contains("129.17"),"visible Details GLA uses the updated twelve square metres after reopen");
    const auto capture=qEnvironmentVariable("VERTEX_TEST_CAPTURE_DIR");if(!capture.isEmpty()) {
        QDir().mkpath(capture);require(window.grab().save(QDir(capture).filePath("measured-line-live-area-move.png")),"capture actual edited native canvas");
    }
}
void test_canvas_source_rotation() {
    auto document=fixture();auto source=document->snapshot();
    document->apply(ApplyEntityChanges{source.revision(),{EntityChange::erase("separator")},{},"Remove divider"});
    MainWindow window(document);window.setAttribute(Qt::WA_DontShowOnScreen,true);window.resize(1400,900);window.show();QCoreApplication::processEvents();
    require(window.selectEntity("outline"),"select closed stroke");
    const auto areas=window.detectRoomBoundariesFromExistingWalls("living");require(areas.size()==1,"one closed source defines one area");
    require(window.selectEntity("outline"),"reselect source for rotation");
    auto* canvas=dynamic_cast<PlanCanvas*>(window.findChild<QWidget*>("measurementPlanCanvas"));require(canvas,"rotation canvas exists");
    canvas->setSnapEnabled(false);canvas->setWallSnapEnabled(false);canvas->setOverviewMapEnabled(false);canvas->setViewTransform({2,2},80);
    const auto before=window.document().snapshot();const auto pin=canvas->selectionRotationHandlePosition();require(pin.has_value(),"saved stroke exposes rotation handle");
    const auto center=QRectF(canvas->rect()).center();const auto radial=*pin-center;
    const auto target=center+QPointF(radial.y(),-radial.x());
    mouse(*canvas,QEvent::MouseButtonPress,*pin);mouse(*canvas,QEvent::MouseMove,target);
    require(wait([&]{return !canvas->entityTransformPreviewPending() && !canvas->entityTransformPreview().empty();}),"exact measured stroke rotation preview completes");
    require(window.document().snapshot().entities()==before.entities(),"rotation proposal leaves retained geometry unchanged");
    mouse(*canvas,QEvent::MouseButtonRelease,target);
    require(wait([&]{return window.document().revision()==before.revision()+1 && !canvas->entityTransformPreviewPending();}),"rotation release commits exact source proposal");
    const auto rotated=window.document().snapshot();
    require(measurement_linework_source_checks(rotated.entities()).at(areas.front().toStdString()).current,"rotated area follows its source with exact lineage");
    const auto next_pin=canvas->selectionRotationHandlePosition();require(next_pin && std::abs(next_pin->x()-pin->x())>40,
        "rotation handle retains the source orientation after release");
    require(window.undoCommand() && window.document().snapshot().entities()==before.entities(),"rotation Undo restores local receipts source frame and derived area");
}
void test_saved_plan_move_frame() {
    MainWindow window(fixture());window.setAttribute(Qt::WA_DontShowOnScreen,true);window.resize(1400,900);
    CoordinatedView view;view.id="offset-plan";view.name="Offset rotated plan";view.origin_m={100,200,0};view.up={1,0,0};
    const auto source=window.document().snapshot();
    window.document().apply(ApplyEntityChanges{source.revision(),{EntityChange::upsert(make_sheet_view_entity(
        "offset-plan-owner",SheetViewModel::create({view},{})))},{},"Create offset plan"});
    window.setWorkspace(Workspace::architectural);window.show();QCoreApplication::processEvents();
    require(window.selectEntity("separator"),"refresh saved-plan source selection");
    auto* views=window.findChild<QComboBox*>("architecturalView");require(views,"saved-plan selector exists");
    int index=-1;for(int i=0;i<views->count();++i)if(views->itemData(i,Qt::UserRole+1).toString()=="offset-plan" &&
        views->itemData(i,Qt::UserRole+2).toString()=="offset-plan-owner")index=i;
    require(index>=3,"nonidentity saved plan is selectable");views->setCurrentIndex(index);QCoreApplication::processEvents();
    require(window.selectEntity("separator"),"select saved-view measured source");
    auto* canvas=dynamic_cast<PlanCanvas*>(window.findChild<QWidget*>("architecturalPlanCanvas"));require(canvas,"saved-plan canvas exists");
    const auto find=[&](const std::vector<CanvasEntity>& items) -> const CanvasEntity* {
        const auto found=std::find_if(items.begin(),items.end(),[](const auto& item){return item.id=="separator";});
        return found==items.end()?nullptr:&*found;
    };
    const auto* retained=find(canvas->entities());require(retained && retained->resize_frame,"saved stroke has projected frame");
    const auto original_frame=*retained->resize_frame;
    canvas->setOverviewMapEnabled(false);canvas->setSnapEnabled(false);canvas->setWallSnapEnabled(false);canvas->setViewTransform(original_frame.center,80);
    const auto before=window.document().snapshot();const auto grab=QRectF(canvas->rect()).center()+QPointF(30,0),target=grab+QPointF(0,-80);
    mouse(*canvas,QEvent::MouseButtonPress,grab);mouse(*canvas,QEvent::MouseMove,target);
    require(wait([&]{return !canvas->entitiesMovePreviewPending() && !canvas->entitiesMovePreview().empty();}),"saved-plan measured source move proposal completes");
    const auto preview=canvas->entitiesMovePreview();const auto* proposed=find(preview);require(proposed && proposed->resize_frame,"saved-plan proposal has selection frame");
    require(std::abs(proposed->resize_frame->center.x-original_frame.center.x)<1e-10 &&
        std::abs(proposed->resize_frame->center.y-original_frame.center.y-1)<1e-10,
        "saved-plan proposal applies movement once in the projected frame");
    const auto expected=*proposed->resize_frame;
    mouse(*canvas,QEvent::MouseButtonRelease,target);
    require(wait([&]{return window.document().revision()==before.revision()+1 && !canvas->entitiesMovePreviewPending();}),"saved-plan source movement commits once");
    const auto* committed=find(canvas->entities());require(committed && committed->resize_frame,"committed source retains its frame");
    require(std::abs(committed->resize_frame->center.x-expected.center.x)<1e-10 &&
        std::abs(committed->resize_frame->center.y-expected.center.y)<1e-10 &&
        std::abs(std::remainder(committed->resize_frame->rotation_radians-expected.rotation_radians,2*std::acos(-1)))<1e-10,
        "saved-plan selection controls do not jump between proposal and commit");
}
void test_derived_area_move_keeps_sources() {
    MainWindow window(fixture());window.setAttribute(Qt::WA_DontShowOnScreen,true);window.resize(1400,900);window.show();QCoreApplication::processEvents();
    require(window.selectEntity("outline"),"select sources to define move fixture");
    const auto areas=window.detectRoomBoundariesFromExistingWalls("living");require(areas.size()==2,"area move fixture has two source faces");
    QString left;for(const auto& id:areas)if(boundary_bounds(boundary_geometry(decode_identified_boundary_entity(
        window.document().snapshot().entities().at(id.toStdString())))).minimum.x==0)left=id;
    require(window.selectEntity(left),"select the actual derived area");
    auto* canvas=dynamic_cast<PlanCanvas*>(window.findChild<QWidget*>("measurementPlanCanvas"));require(canvas,"derived-area canvas exists");
    canvas->setOverviewMapEnabled(false);canvas->setSnapEnabled(false);canvas->setWallSnapEnabled(false);canvas->setViewTransform({2,2},80);
    const auto before=window.document().snapshot();const auto grab=QRectF(canvas->rect()).center()+QPointF(-80,30),target=grab+QPointF(80,0);
    mouse(*canvas,QEvent::MouseButtonPress,grab);mouse(*canvas,QEvent::MouseMove,target);
    require(wait([&]{return !canvas->entitiesMovePreviewPending() && !canvas->entitiesMovePreview().empty();}),"derived area body drag prepares sources and all affected faces");
    mouse(*canvas,QEvent::MouseButtonRelease,target);
    require(wait([&]{return window.document().revision()==before.revision()+1 && !canvas->entitiesMovePreviewPending();}),"derived area body drag commits once");
    const auto moved=window.document().snapshot();
    for(const auto id:{"outline","separator"}) {
        const auto original=replay_measurement_linework(*decode_measurement_linework_model(before.entities().at(id).properties.at("model")).model);
        const auto replay=replay_measurement_linework(*decode_measurement_linework_model(moved.entities().at(id).properties.at("model")).model);
        require(replay.anchor.x==original.anchor.x+1 && replay.anchor.y==original.anchor.y,"moving a derived area moves its actual supporting strokes");
    }
    const auto checks=measurement_linework_source_checks(moved.entities());
    for(const auto& id:areas)require(checks.at(id.toStdString()).current,"shared source areas remain current after dragging the area body");
    require(window.undoCommand() && window.document().snapshot().entities()==before.entities(),"derived area and shared source movement undo together");
}
}
int main(int argc,char** argv) {
    sketch::testing::noninteractive_errors(); QStandardPaths::setTestModeEnabled(true); QApplication app(argc,argv);
    QCoreApplication::setApplicationName("Vertex-linework-area-test-"+QUuid::createUuid().toString());
    try {
        require(QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/Inter.ttf"))>=0,"bundled Inter font loads");
        app.setFont(QFont(QStringLiteral("Inter"),10));
        test_define_areas_and_history();test_ansi_profile_dimensions();test_canvas_source_move_and_live_gla();test_canvas_source_rotation();test_saved_plan_move_frame();test_derived_area_move_keeps_sources();std::cout<<"measurement linework area desktop checks passed\n";return 0;
    }
    catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
