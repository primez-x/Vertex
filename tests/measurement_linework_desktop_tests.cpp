#include "sketch/desktop/main_window.hpp"
#include "sketch/measurement_linework.hpp"
#include "../src/desktop/plan_canvas.hpp"
#include "support/noninteractive_errors.hpp"

#include <QApplication>
#include <QFont>
#include <QFontDatabase>
#include <QAction>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QInputDialog>
#include <QLineEdit>
#include <QPushButton>
#include <QEventLoop>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>
#include <QUuid>
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {
using namespace sketch;
using namespace sketch::desktop;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
bool equal(Vec2 a, Vec2 b) { return a.x == b.x && a.y == b.y; }
void events() { QCoreApplication::processEvents(QEventLoop::AllEvents, 50); }
PlanCanvas& prepare(MainWindow& window) {
    window.setAttribute(Qt::WA_DontShowOnScreen, true);
    window.resize(1400, 900); window.show(); events();
    auto* canvas = dynamic_cast<PlanCanvas*>(window.findChild<QWidget*>(QStringLiteral("measurementPlanCanvas")));
    require(canvas, "native canvas exists"); canvas->setOverviewMapEnabled(false); canvas->setSnapEnabled(false);
    return *canvas;
}
void click(PlanCanvas& canvas, Vec2 point, Qt::MouseButton button = Qt::LeftButton) {
    const auto center = QRectF(canvas.rect()).center(); const auto view = canvas.viewCenter();
    const auto screen = center + QPointF((point.x-view.x)*canvas.viewScale(), -(point.y-view.y)*canvas.viewScale());
    QMouseEvent move(QEvent::MouseMove, screen, canvas.mapToGlobal(screen.toPoint()), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&canvas, &move);
    QMouseEvent press(QEvent::MouseButtonPress, screen, canvas.mapToGlobal(screen.toPoint()), button, button, Qt::NoModifier);
    QApplication::sendEvent(&canvas, &press);
    QMouseEvent release(QEvent::MouseButtonRelease, screen, canvas.mapToGlobal(screen.toPoint()), button, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&canvas, &release); events();
}
const Entity& stroke(const DocumentSnapshot& snapshot) {
    const Entity* result = nullptr;
    for (const auto& [id, entity] : snapshot.entities()) if (entity.type == "measurement_linework") {
        require(!result, "one entity per uninterrupted stroke"); result = &entity;
    }
    require(result, "measured lines are persisted as independent linework"); return *result;
}
void test_native_stroke_and_saved_projection() {
    MainWindow window; auto& canvas = prepare(window); window.setMetricUnits(true);
    auto* modes = window.findChild<QComboBox*>(QStringLiteral("drawingMode"));
    require(modes && modes->findData(QStringLiteral("measurement_linework")) >= 0, "explicit Measured lines choice exists");
    require(modes->currentData() == QStringLiteral("wall"), "default hybrid wall drawing is retained");
    modes->setCurrentIndex(modes->findData(QStringLiteral("measurement_linework"))); events();
    const auto before = window.document().snapshot();
    click(canvas, {0,0}); require(window.document().revision() == before.revision(), "starting point creates no empty entity");
    click(canvas, {3,0}); const auto first = window.document().snapshot();
    auto model = *decode_measurement_linework_model(stroke(first).properties.at("model")).model;
    require(first.revision() == before.revision()+1 && model.edges.size()==1 && stroke(first).required, "first edge commits once with required typed model");
    for (const auto key : {"property_id", "building_id", "floor_id", "layer_id"}) require(stroke(first).properties.contains(key), "full drawing context persists");
    require(!stroke(first).properties.contains("segments"), "no duplicate geometry authority is stored");
    click(canvas, {3,2}); const auto second = window.document().snapshot();
    model = *decode_measurement_linework_model(stroke(second).properties.at("model")).model;
    if (!(second.revision()==first.revision()+1 && model.edges.size()==2 && !model.closed))
        throw std::runtime_error("each loose edge is an atomic append to the same open stroke: first_revision="+
            std::to_string(first.revision())+" second_revision="+std::to_string(second.revision())+
            " edges="+std::to_string(model.edges.size())+" closed="+std::to_string(model.closed)+
            " selected="+window.selectedEntityId().toStdString()+" error="+window.lastError().toStdString()+
            " model="+stroke(second).properties.at("model").dump());
    QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier); QApplication::sendEvent(&canvas, &enter); events();
    require(window.document().snapshot().entities()==second.entities(), "Enter finishes without changing committed geometry");
    require(window.undoCommand() && window.document().snapshot().entities()==first.entities(), "one undo removes only last committed edge");
    require(window.redoCommand() && window.document().snapshot().entities()==second.entities(), "redo restores exact receipt geometry");
    QTemporaryDir directory; const auto path = directory.filePath(QStringLiteral("linework.bldproj"));
    require(window.saveProjectAs(path) && window.openProject(path), "saved open linework reopens"); events();
    const auto saved = window.document().snapshot(); const auto id = QString::fromStdString(stroke(saved).id);
    bool projected = false;
    for (const auto& entity : canvas.entities()) if (entity.id==id) { projected=true; require(entity.segments.size()==2 && entity.thickness_metres==0 && entity.snap_points.size()>=3, "loose analytical lines have no physical wall footprint and provide snap points"); }
    require(projected && !canvas.labels().empty(), "saved linework and measured dimensions render");
    for (const auto& label : canvas.labels()) if (label.id == id && label.selection_type == QStringLiteral("measurement_linework"))
        require(label.automatic_linear_placement.has_value(), "measured dimensions use collision-aware placement outside the stroke");
    if (const auto capture=qEnvironmentVariable("VERTEX_LINEWORK_CAPTURE"); !capture.isEmpty())
        require(canvas.grab().save(capture), "native measured-line canvas screenshot saves");
    click(canvas, {1.5,0}); require(window.selectedEntityId()==id, "native click picks saved linework");
}
void test_precise_pen_up_jump_and_stale_input() {
    MainWindow window; auto& canvas = prepare(window); window.setMetricUnits(true);
    require(window.beginMeasurementLinework() && window.appendMeasurementLineworkPoint({0.01317,-0.01931}), "public authoring starts exact loose anchor");
    const auto before = window.document().revision();
    require(window.appendMeasurementLineworkHeading(QStringLiteral("1.234567 m"), QStringLiteral("17.25 deg")), "real distance and heading inputs commit exact receipt");
    const auto first=window.document().snapshot(); auto model=*decode_measurement_linework_model(stroke(first).properties.at("model")).model;
    require(model.edges.front().receipt.distance->original_expression=="1.234567 m" && model.edges.front().receipt.heading->original_expression=="17.25 deg", "typed expressions survive exact replay");
    require(!window.appendMeasurementLineworkPoint({2,2}, before) && window.document().snapshot().entities()==first.entities(), "stale point cannot append geometry");
    require(!window.appendMeasurementLineworkHeading(QStringLiteral("nonsense"), QStringLiteral("17 deg")) && window.document().snapshot().entities()==first.entities(), "invalid typed input is atomic");
    const auto id=QString::fromStdString(stroke(first).id); const auto vertex=QString::fromStdString(model.edges.front().start_vertex_id);
    require(!window.jumpMeasurementLineworkVertex(id,vertex,before), "stale jump is refused");
    require(window.jumpMeasurementLineworkVertex(id,vertex,first.revision()), "exact saved vertex relocates real next origin");
    require(window.appendMeasurementLineworkHeading(QStringLiteral("2 m"), QStringLiteral("90 deg")), "jump creates a new independent stroke");
    const auto jumped=window.document().snapshot(); std::size_t count=0;
    for (const auto& [other_id,entity] : jumped.entities()) if (entity.type=="measurement_linework") { ++count; if(other_id!=stroke(first).id) {const auto other=*decode_measurement_linework_model(entity.properties.at("model")).model; require(equal(other.anchor,model.anchor) && other.edges.size()==1, "jump introduces no connecting stray edge");} }
    require(count==2, "pen up leaves original stroke intact");
    require(window.relocateMeasurementLinework({8,9},jumped.revision()) && window.appendMeasurementLineworkPoint({10,9}), "pen-up relocation defines the actual next edge origin");
    const auto relocated=window.document().snapshot(); bool found=false;
    for(const auto& [other_id,entity]:relocated.entities()) if(entity.type=="measurement_linework") {const auto other=*decode_measurement_linework_model(entity.properties.at("model")).model; if(equal(other.anchor,{8,9})) {found=true; require(other.edges.size()==1, "relocation stores no connector");} }
    require(found, "relocated anchor is durable");
    click(canvas,{10,9},Qt::RightButton); require(window.document().snapshot().entities()==relocated.entities(), "right click ends stroke without erasing committed linework");
}
void test_native_precise_lift_and_point_jump() {
    MainWindow window; auto& canvas=prepare(window); window.setMetricUnits(true);
    require(window.beginMeasurementLinework(), "native input fixture starts measured lines"); click(canvas,{0,0});
    bool precise=false;
    QTimer::singleShot(0, [&] {
        auto* dialog=window.findChild<QDialog*>(QStringLiteral("measuredLineInput"));
        require(dialog, "D opens native measured input dialog");
        dialog->findChild<QLineEdit*>(QStringLiteral("boundaryInputLength"))->setText(QStringLiteral("3.125 m"));
        dialog->findChild<QLineEdit*>(QStringLiteral("boundaryInputHeading"))->setText(QStringLiteral("0 deg"));
        precise=true; dialog->findChild<QPushButton*>(QStringLiteral("boundaryInputAdd"))->click();
    });
    QKeyEvent d(QEvent::KeyPress,Qt::Key_D,Qt::NoModifier,QStringLiteral("d")); QApplication::sendEvent(&canvas,&d); events();
    require(precise, "native D command reaches precise input");
    const auto first=window.document().snapshot(); const auto id=QString::fromStdString(stroke(first).id);
    auto* lift=window.findChild<QAction*>(QStringLiteral("liftMeasuredPen")); require(lift,"native Lift measured pen command exists");
    lift->trigger(); click(canvas,{6,7}); click(canvas,{8,7});
    auto snapshot=window.document().snapshot(); std::size_t count=0;
    for(const auto& [other_id,entity]:snapshot.entities()) if(entity.type=="measurement_linework") { ++count; if(other_id!=id.toStdString()) require(equal(decode_measurement_linework_model(entity.properties.at("model")).model->anchor,{6,7}),"native lift next click is actual anchor without connector"); }
    require(count==2,"native pen-up creates second stroke"); window.finishMeasurementLinework();
    require(window.selectEntity(id),"native jump selects saved measured stroke");
    auto* jump=window.findChild<QAction*>(QStringLiteral("jumpBoundaryVertex")); require(jump,"native point jump action exists");
    bool jumped=false;
    QTimer::singleShot(0,[&] { auto* dialog=qobject_cast<QInputDialog*>(QApplication::activeModalWidget()); require(dialog,"native vertex jump opens vertex picker"); jumped=true; dialog->accept(); });
    jump->trigger(); require(jumped,"native saved-linework jump reaches vertex picker");
    require(window.appendMeasurementLineworkHeading(QStringLiteral("1 m"),QStringLiteral("90 deg")),"native jump starts real measured pen");
    snapshot=window.document().snapshot(); count=0;
    for(const auto& [other_id,entity]:snapshot.entities()) if(entity.type=="measurement_linework") ++count;
    require(count==3 && snapshot.entities().at(id.toStdString())==first.entities().at(id.toStdString()),"native point jump keeps original saved geometry");
}
void test_read_only_projection_and_unknown_model() {
    MainWindow source;
    require(source.beginMeasurementLinework() && source.appendMeasurementLineworkPoint({0,0}) &&
            source.appendMeasurementLineworkPoint({2,0}),"read-only fixture creates valid linework"); source.finishMeasurementLinework();
    const auto snapshot=source.document().snapshot(); const auto id=stroke(snapshot).id;
    auto document=std::make_shared<Document>(Document::fork(snapshot)); document->mark_read_only("fixture");
    MainWindow read_only(document); auto& canvas=prepare(read_only); bool shown=false;
    for(const auto& entity:canvas.entities()) if(entity.id.toStdString()==id) shown=true;
    require(shown && !read_only.beginMeasurementLinework(),"read-only saved stroke renders while authoring fails closed");
    auto unknown=std::make_shared<Document>(Document::fork(snapshot)); auto entity=snapshot.entities().at(id);
    entity.properties["model"]["version"]=999;
    unknown->apply(ApplyEntityChanges{.expected_revision=unknown->revision(),.entity_changes={EntityChange::upsert(entity)},.message="unknown fixture"});
    MainWindow opaque(unknown); auto& opaque_canvas=prepare(opaque);
    for(const auto& projected:opaque_canvas.entities()) require(projected.id.toStdString()!=id,"unknown model is never guessed into canvas geometry");
    require(opaque.document().snapshot().entities().at(id)==entity,"unknown model remains opaque and unchanged");
}
}
int main(int argc,char** argv) {
    sketch::testing::noninteractive_errors(); QStandardPaths::setTestModeEnabled(true); QApplication application(argc,argv);
    QCoreApplication::setApplicationName(QStringLiteral("Vertex-linework-test-")+QUuid::createUuid().toString(QUuid::WithoutBraces));
    try {
        require(QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/Inter.ttf")) >= 0, "bundled Inter font loads");
        application.setFont(QFont(QStringLiteral("Inter"),10));
        test_native_stroke_and_saved_projection(); test_precise_pen_up_jump_and_stale_input(); test_native_precise_lift_and_point_jump(); test_read_only_projection_and_unknown_model();
    }
    catch(const std::exception& error) { std::cerr << "measurement_linework_desktop_tests: " << error.what() << '\n'; return 1; }
    std::cout << "Measurement linework desktop tests passed\n"; return 0;
}
