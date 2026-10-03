#include "sketch/desktop/main_window.hpp"
#include "sketch/desktop/boundary_input_dialog.hpp"
#include "sketch/measurement_linework.hpp"
#include "sketch/boundary_entity.hpp"
#include "sketch/project_store.hpp"
#include "../src/desktop/plan_canvas.hpp"
#include "support/noninteractive_errors.hpp"
#include <QApplication>
#include <QComboBox>
#include <QCheckBox>
#include <QDir>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QMouseEvent>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QDialog>
#include <QFont>
#include <QFontDatabase>
#include <QKeyEvent>
#include <QLineEdit>
#include <QPushButton>
#include <QStandardPaths>
#include <QTimer>
#include <QUuid>
#include <iostream>
#include <stdexcept>
#include <cmath>
#include <exception>
#include <functional>
#include <numbers>

namespace {
void require(bool value,const char* message) { if(!value) throw std::runtime_error(message); }
using namespace sketch;
using namespace sketch::desktop;
PlanCanvas& prepare(MainWindow& window,bool metric=true) {
    window.setAttribute(Qt::WA_DontShowOnScreen,true); window.resize(1400,900); window.show();
    QApplication::processEvents(); window.setMetricUnits(metric);
    auto* canvas=dynamic_cast<PlanCanvas*>(window.findChild<QWidget*>(QStringLiteral("measurementPlanCanvas")));
    require(canvas,"native canvas exists"); canvas->setOverviewMapEnabled(false); return *canvas;
}
void field(QDialog& dialog,const char* name,const QString& text) {
    auto* edit=dialog.findChild<QLineEdit*>(QString::fromLatin1(name)); require(edit,"precision field exists"); edit->setText(text);
}
void accept(QDialog& dialog) {
    auto* add=dialog.findChild<QPushButton*>(QStringLiteral("boundaryInputAdd"));
    require(add && add->isEnabled(),"valid typed chord enables admission"); add->click();
}
void input(MainWindow& window,PlanCanvas& canvas,const std::function<void(BoundaryInputDialog&)>& operation) {
    bool recognized=false; std::exception_ptr failure;
    QTimer::singleShot(0,&window,[&] {
        auto* dialog=dynamic_cast<BoundaryInputDialog*>(QApplication::activeModalWidget()); recognized=dialog && dialog->isVisible();
        if(!dialog) { if(auto* active=qobject_cast<QDialog*>(QApplication::activeModalWidget())) active->reject(); return; }
        try { operation(*dialog); } catch(...) { failure=std::current_exception(); dialog->reject(); }
    });
    QKeyEvent key(QEvent::KeyPress,Qt::Key_D,Qt::NoModifier); QApplication::sendEvent(&canvas,&key); QApplication::processEvents();
    if(failure) std::rethrow_exception(failure); require(recognized,"actual D opens shared precision input");
}
void typed(QDialog& dialog,int index,const QString& length="2",const QString& heading="90 deg") {
    auto* methods=dialog.findChild<QComboBox*>(QStringLiteral("boundaryInputMethod")); require(methods && methods->count()==8,"eight analytical methods retained"); methods->setCurrentIndex(index);
    auto* definition=dialog.findChild<QComboBox*>(QStringLiteral("boundaryInputChordDefinition"));
    require(definition && definition->count()==2 && definition->currentIndex()==1,"new chord forms default to length and heading");
    require(dialog.findChild<QLineEdit*>(QStringLiteral("boundaryInputEndX"))->isHidden(),"typed chord hides endpoint coordinates");
    field(dialog,"boundaryInputChordLength",length); field(dialog,"boundaryInputChordHeading",heading);
    field(dialog,"boundaryInputSweep","-90 deg"); field(dialog,"boundaryInputHeight","1"); field(dialog,"boundaryInputArcLength","3.141592653589793");
    if(index==6) dialog.findChild<QCheckBox*>(QStringLiteral("boundaryInputClockwise"))->setChecked(true);
}
MeasurementLinework model(const DocumentSnapshot& snapshot) {
    std::optional<MeasurementLinework> result;
    for(const auto& [id,entity]:snapshot.entities()) if(entity.type=="measurement_linework") {
        require(!result,"one continuous measured stroke"); result=decode_measurement_linework_model(entity.properties.at("model")).model;
    }
    require(result.has_value(),"measured model decodes"); return *result;
}
void capture(QWidget& widget,const QString& filename) {
    const auto directory=qEnvironmentVariable("VERTEX_TEST_CAPTURE_DIR"); if(directory.isEmpty()) return;
    require(QDir().mkpath(directory) && widget.grab().save(QDir(directory).filePath(filename)),"native capture saves");
}
void rotate_canvas(MainWindow& window,PlanCanvas& canvas) {
    canvas.fitView(); QApplication::processEvents();
    const auto pin=canvas.selectionRotationHandlePosition(); const auto frame=canvas.selectionBounds();
    require(pin && frame,"saved typed chord exposes actual rotation control");
    const auto center=frame->center(); const auto radial=*pin-center; const auto target=center+QPointF(radial.y(),-radial.x());
    const auto mouse=[&](QEvent::Type type,QPointF point) {
        QMouseEvent event(type,point,canvas.mapToGlobal(point.toPoint()),type==QEvent::MouseMove?Qt::NoButton:Qt::LeftButton,type==QEvent::MouseButtonRelease?Qt::NoButton:Qt::LeftButton,Qt::NoModifier);
        QApplication::sendEvent(&canvas,&event);
    };
    const auto before=window.document().snapshot(); mouse(QEvent::MouseButtonPress,*pin); mouse(QEvent::MouseMove,target);
    QElapsedTimer timer; timer.start();
    while((canvas.entityTransformPreviewPending() || canvas.entityTransformPreview().empty()) && timer.elapsed()<10000) QCoreApplication::processEvents(QEventLoop::AllEvents,50);
    if(canvas.entityTransformPreview().empty()) throw std::runtime_error("typed chord exact rotation preview: "+window.lastError().toStdString());
    require(window.document().snapshot().entities()==before.entities(),"rotation preview is detached"); mouse(QEvent::MouseButtonRelease,target);
    timer.restart(); while(canvas.entityTransformPreviewPending() && timer.elapsed()<10000) QCoreApplication::processEvents(QEventLoop::AllEvents,50);
    require(window.document().revision()==before.revision()+1,"actual canvas rotation commits once");
}
void test_three_methods_both_units() {
    for(bool metric:{false,true}) for(int index:{4,5,6}) {
        MainWindow window; auto& canvas=prepare(window,metric); const double unit=metric?1:0.3048;
        window.findChild<QComboBox*>(QStringLiteral("drawingMode"))->setCurrentIndex(2);
        input(window,canvas,[&](BoundaryInputDialog& dialog) { field(dialog,"boundaryInputEndX","3"); field(dialog,"boundaryInputEndY","-2"); accept(dialog); });
        const auto before=window.document().snapshot();
        input(window,canvas,[&](BoundaryInputDialog& dialog) { typed(dialog,index); if(index==4) capture(dialog,metric?"typed-chord-metric.png":"typed-chord-imperial.png"); accept(dialog); });
        const auto after=window.document().snapshot(); const auto saved=model(after); const auto& receipt=saved.edges.front().receipt;
        require(after.revision()==before.revision()+1 && saved.edges.size()==1 && saved.schema_version==4 && saved.replay_version==4,"one typed chord admits one v4 edge");
        require(receipt.chord_input && !receipt.chord_end && receipt.chord_input->length.original_expression=="2" && receipt.chord_input->length.entered_unit==(metric?Unit::metre:Unit::foot) && receipt.chord_input->heading.original_expression=="90 deg","original typed measurements retain exact authority");
        const auto segment=replay_measurement_linework(saved).edges.front().segment;
        const double sweep=index==4?-std::numbers::pi/2:index==5?std::numbers::pi:-std::numbers::pi;
        const double length=(index==4?std::numbers::pi/std::sqrt(2.0):std::numbers::pi)*unit;
        require(std::abs(segment.end.x-3*unit)<1e-9 && std::abs(segment.end.y)<1e-9 && std::abs(segment.sweep_radians-sweep)<1e-9 && std::abs(segment_length(segment)-length)<1e-9,"independent endpoint sweep and physical length match both unit systems");
        require(window.undoCommand() && window.document().snapshot().entities()==before.entities() && window.redoCommand() && window.document().snapshot().entities()==after.entities(),"native Undo/Redo preserves exact chord input");
        window.finishMeasurementLinework();
        require(window.selectEntity(QString::fromStdString(saved.stroke_id)),"native typed chord selects");
        rotate_canvas(window,canvas);
        const auto transformed=model(window.document().snapshot());
        require(transformed.schema_version==4 && transformed.edges.front().receipt==receipt && transformed.operations.size()==1 && std::abs(segment_length(replay_measurement_linework(transformed).edges.front().segment)-length)<1e-9,"transform preserves local original inputs and length");
        if(index==4) {
            require(window.editSelectedBoundaryEdgeLength(QString::fromStdString(saved.edges.front().segment_id),metric?"3 m":"3 ft",BoundaryFixedEndpoint::start,false,window.document().revision()),"native physical curve length edit commits");
            const auto edited=model(window.document().snapshot()); require(edited.edges.front().receipt==receipt && edited.operations.size()==2 && std::abs(segment_length(replay_measurement_linework(edited).edges.front().segment)-3*unit)<1e-9,"ordered edit retains typed chord and exact requested length");
        }
    }
}
void test_preferences_invalid_and_stale() {
    MainWindow window; auto& canvas=prepare(window); require(window.beginMeasurementLinework() && window.appendMeasurementLineworkPoint({0,0}),"preferences fixture starts");
    const auto before=window.document().snapshot();
    input(window,canvas,[&](BoundaryInputDialog& dialog) {
        typed(dialog,4,"bad quantity"); require(!dialog.submit() && !dialog.lastError().isEmpty(),"invalid chord remains editable");
        field(dialog,"boundaryInputChordLength","2000 mm"); field(dialog,"boundaryInputChordHeading","pi/2"); field(dialog,"boundaryInputSweep","90 deg"); accept(dialog);
    });
    const auto first=window.document().snapshot();
    input(window,canvas,[&](BoundaryInputDialog& dialog) {
        require(dialog.findChild<QLineEdit*>(QStringLiteral("boundaryInputChordLength"))->text()=="2000 mm","accepted expression remembered");
        dialog.findChild<QComboBox*>(QStringLiteral("boundaryInputChordDefinition"))->setCurrentIndex(0);
        require(!dialog.findChild<QLineEdit*>(QStringLiteral("boundaryInputEndX"))->isHidden(),"endpoint controls still available"); dialog.reject();
    });
    require(window.document().snapshot().entities()==first.entities(),"Cancel changes no geometry");
    input(window,canvas,[&](BoundaryInputDialog& dialog) {
        require(dialog.findChild<QComboBox*>(QStringLiteral("boundaryInputChordDefinition"))->currentIndex()==1,"cancelled choice not remembered");
        window.setMetricUnits(false); window.setMetricUnits(true); accept(dialog);
    });
    require(window.document().revision()==first.revision(),"stale unit generation rejects typed receipt");
    input(window,canvas,[&](BoundaryInputDialog& dialog) {
        typed(dialog,4); dialog.findChild<QComboBox*>(QStringLiteral("boundaryInputChordDefinition"))->setCurrentIndex(0);
        field(dialog,"boundaryInputEndX","3 m"); field(dialog,"boundaryInputEndY","2 m"); accept(dialog);
    });
    const auto endpoint=model(window.document().snapshot()); require(endpoint.edges.back().receipt.chord_end && !endpoint.edges.back().receipt.chord_input,"explicit endpoint admits legacy receipt following typed edge");
    input(window,canvas,[&](BoundaryInputDialog& dialog) { require(dialog.findChild<QComboBox*>(QStringLiteral("boundaryInputChordDefinition"))->currentIndex()==0,"accepted definition remembered"); dialog.reject(); });
    require(first.revision()==before.revision()+1,"correction after invalid input creates one command");
}
void test_native_save_area_asset_and_pdf() {
    MainWindow window; auto& canvas=prepare(window); require(window.beginMeasurementLinework() && window.appendMeasurementLineworkPoint({0,0}),"semicircle starts");
    input(window,canvas,[&](BoundaryInputDialog& dialog) { typed(dialog,4,"2 m","0 deg"); field(dialog,"boundaryInputSweep","180 deg"); accept(dialog); });
    input(window,canvas,[&](BoundaryInputDialog& dialog) { dialog.findChild<QComboBox*>(QStringLiteral("boundaryInputMethod"))->setCurrentIndex(3); field(dialog,"boundaryInputEndX","0 m"); field(dialog,"boundaryInputEndY","0 m"); accept(dialog); });
    const auto stroke=model(window.document().snapshot()); require(stroke.closed,"typed arc and chord close");
    require(window.selectEntity(QString::fromStdString(stroke.stroke_id)),"select curved outline");
    const auto areas=window.detectRoomBoundariesFromExistingWalls("living",window.document().revision()); require(areas.size()==1,"analytical curved face defines area");
    const auto area=decode_identified_boundary_entity(window.document().snapshot().entities().at(areas.front().toStdString()));
    require(std::abs(std::abs(signed_area(boundary_geometry(area)))-std::numbers::pi/2)<1e-9,"area uses true semicircle");
    window.document().apply(ApplyEntityChanges{window.document().revision(),{}, {AssetChange::upsert(Asset::create("typed-chord-package-fixture","application/octet-stream",{std::byte{0},std::byte{13},std::byte{255},std::byte{10}}))},"typed chord raw asset fixture"});
    const auto saved=window.document().snapshot(); require(ProjectStore::required_format_version(saved)==31,"native format floor31");
    QTemporaryDir temporary; const auto directory=qEnvironmentVariable("VERTEX_TEST_CAPTURE_DIR"); const auto root=directory.isEmpty()?temporary.path():directory; require(QDir().mkpath(root),"artifact directory exists");
    const auto path=QDir(root).filePath("typed-chord-native-"+QUuid::createUuid().toString(QUuid::WithoutBraces)+".bldproj");
    require(window.saveProjectAs(path) && window.openProject(path) && window.document().snapshot().entities()==saved.entities() && window.document().snapshot().assets()==saved.assets(),"native save/reopen retains receipt area history and raw bytes");
    const auto pdf=QDir(root).filePath("typed-chord-area.pdf"); require(window.exportDraftPdf(pdf) && QFileInfo(pdf).size()>0,"analytical curved area exports native PDF");
    capture(window,"typed-chord-saved-area.png");
}
void test_default_wall_typed_chord() {
    for(bool metric:{false,true}) {
        MainWindow window; auto& canvas=prepare(window,metric); const double unit=metric?1:0.3048;
        require(window.findChild<QComboBox*>(QStringLiteral("drawingMode"))->currentIndex()==0,"Wall remains default Draw choice");
        input(window,canvas,[&](BoundaryInputDialog& dialog) { field(dialog,"boundaryInputEndX","0"); field(dialog,"boundaryInputEndY","0"); accept(dialog); });
        input(window,canvas,[&](BoundaryInputDialog& dialog) { typed(dialog,4,"2","0 deg"); field(dialog,"boundaryInputSweep","90 deg"); accept(dialog); });
        std::optional<Entity> wall;
        const auto admitted=window.document().snapshot();
        for(const auto& [id,entity]:admitted.entities()) if(entity.type=="wall") { require(!wall,"one exact wall"); wall=entity; }
        if(!wall) throw std::runtime_error("actual default wall D creates no physical wall: "+window.lastError().toStdString()+"; entities="+std::to_string(window.document().snapshot().entities().size()));
        const auto receipt=decode_construction_receipt(wall->properties.at("original_drawing_input"));
        const auto segment=replay_construction_receipt(receipt,{receipt.start}).segment;
        require(receipt.chord_input && !receipt.chord_end && std::abs(segment.end.x-2*unit)<1e-9 && std::abs(segment.sweep_radians-std::numbers::pi/2)<1e-9 && ProjectStore::required_format_version(window.document().snapshot())==31,"default wall retains exact typed chord original frame and format floor");
        QKeyEvent cancel(QEvent::KeyPress,Qt::Key_Escape,Qt::NoModifier); QApplication::sendEvent(&canvas,&cancel);
        const auto saved=window.document().snapshot();
        require(window.undoCommand() && window.redoCommand() && window.document().snapshot().entities()==saved.entities(),"physical typed wall Undo/Redo exact");
        QTemporaryDir temporary; const auto path=temporary.filePath("typed-wall.bldproj");
        require(window.saveProjectAs(path) && window.openProject(path) && window.document().snapshot().entities()==saved.entities(),"physical typed wall save/reopen exact");
    }
}
void test_actual_d_offers_typed_chord() {
    sketch::desktop::MainWindow window;
    window.setAttribute(Qt::WA_DontShowOnScreen,true);
    window.resize(1400,900); window.show(); QApplication::processEvents();
    auto* draw=window.findChild<QComboBox*>(QStringLiteral("drawingMode"));
    auto* canvas=dynamic_cast<sketch::desktop::PlanCanvas*>(window.findChild<QWidget*>(QStringLiteral("measurementPlanCanvas")));
    require(draw && canvas,"native Draw choice and canvas exist"); draw->setCurrentIndex(2);
    bool anchor=false;
    QTimer::singleShot(0,&window,[&] {
        auto* dialog=window.findChild<QDialog*>(QStringLiteral("measuredLineInput"));
        if(!dialog) { if(auto* active=qobject_cast<QDialog*>(QApplication::activeModalWidget())) active->reject(); return; }
        auto* x=dialog->findChild<QLineEdit*>(QStringLiteral("boundaryInputEndX"));
        auto* y=dialog->findChild<QLineEdit*>(QStringLiteral("boundaryInputEndY"));
        auto* add=dialog->findChild<QPushButton*>(QStringLiteral("boundaryInputAdd"));
        if(x && y && add) { x->setText(QStringLiteral("0 m")); y->setText(QStringLiteral("0 m")); anchor=true; add->click(); }
        else dialog->reject();
    });
    QKeyEvent start(QEvent::KeyPress,Qt::Key_D,Qt::NoModifier); QApplication::sendEvent(canvas,&start);
    require(anchor,"Draw choice and D place an exact start before any canvas click");
    bool typed_chord=false;
    QTimer::singleShot(0,&window,[&] {
        auto* dialog=window.findChild<QDialog*>(QStringLiteral("measuredLineInput"));
        if(!dialog) { if(auto* active=qobject_cast<QDialog*>(QApplication::activeModalWidget())) active->reject(); return; }
        if(auto* method=dialog->findChild<QComboBox*>(QStringLiteral("boundaryInputMethod"))) method->setCurrentIndex(4);
        auto* definition=dialog->findChild<QComboBox*>(QStringLiteral("boundaryInputChordDefinition"));
        auto* length=dialog->findChild<QLineEdit*>(QStringLiteral("boundaryInputChordLength"));
        auto* heading=dialog->findChild<QLineEdit*>(QStringLiteral("boundaryInputChordHeading"));
        typed_chord=definition && definition->count()==2 && definition->currentIndex()==1 && length && heading && !length->isHidden() && !heading->isHidden();
        dialog->reject();
    });
    QKeyEvent edge(QEvent::KeyPress,Qt::Key_D,Qt::NoModifier); QApplication::sendEvent(canvas,&edge);
    require(typed_chord,"Chord arcs must allow direct length and heading input");
}
}
int main(int argc,char** argv) {
    sketch::testing::noninteractive_errors(); QStandardPaths::setTestModeEnabled(true);
    QApplication application(argc,argv);
    QCoreApplication::setApplicationName(QStringLiteral("Vertex-typed-chord-test-")+QUuid::createUuid().toString(QUuid::WithoutBraces));
    try {
        require(QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/Inter.ttf"))>=0,"bundled font loads");
        application.setFont(QFont(QStringLiteral("Inter"),10));
        test_actual_d_offers_typed_chord();
        test_three_methods_both_units();
        test_preferences_invalid_and_stale();
        test_native_save_area_asset_and_pdf();
        test_default_wall_typed_chord();
    } catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
    std::cout<<"Typed chord desktop tests passed\n"; return 0;
}
