#include "sketch/desktop/main_window.hpp"
#include "sketch/desktop/constraint_dialog.hpp"
#include "sketch/measurement_linework.hpp"
#include "sketch/boundary_entity.hpp"
#include "../src/desktop/plan_canvas.hpp"
#include "support/noninteractive_errors.hpp"
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFontDatabase>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPushButton>
#include <QStandardPaths>
#include <QTableWidget>
#include <QTimer>
#include <QUuid>
#include <cmath>
#include <functional>
#include <iostream>
#include <stdexcept>
namespace {
using namespace sketch;using namespace sketch::desktop;
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void require_near(double value,double expected){require(std::abs(value-expected)<1e-7,"unexpected measured endpoint or length");}
void events(){QApplication::processEvents();}
void capture(QWidget& widget,const char* name){const auto directory=qEnvironmentVariable("VERTEX_TEST_CAPTURE_DIR");if(directory.isEmpty())return;
    require(QDir().mkpath(directory),"capture directory exists");events();require(widget.grab().save(QDir(directory).filePath(QString::fromLatin1(name))),"measured constraint screenshot saved");}
Entity stroke(const std::string& id,const std::vector<Vec2>& points,bool arc=false){
    MeasurementLinework model;model.stroke_id=id;model.anchor=points.front();
    for(std::size_t index=1;index<points.size();++index){
        ConstructionReceipt receipt;receipt.segment_id=id+":e"+std::to_string(index-1);receipt.kind=arc?BoundaryConstructionKind::arc_chord_height:BoundaryConstructionKind::line_to_point;
        receipt.start=points[index-1];receipt.chord_end=points[index];if(arc)receipt.height=parse_quantity("1 m");
        model.edges.push_back({receipt.segment_id,id+":v"+std::to_string(index-1),id+":v"+std::to_string(index),receipt});
    }
    return {id,"measurement_linework",{{"name",id},{"property_id","p"},{"building_id","b"},{"floor_id","f"},{"layer_id","l"},{"model",encode_measurement_linework_model(model)}},true};
}
std::shared_ptr<Document> fixture(){return std::make_shared<Document>(Document::create({
    {"p","property",nlohmann::json::object(),false},{"b","building",{{"property_id","p"}},false},{"f","floor",{{"building_id","b"}},false},
    {"l","layer",{{"floor_id","f"}},false},stroke("a",{{0,0},{3.048,0},{3.048,2}}),stroke("b-stroke",{{3.048,2},{5,2}})}));}
QComboBox& combo(ConstraintDialog& dialog,const char* id){auto* box=dialog.findChild<QComboBox*>(id);require(box,"constraint combo exists");return *box;}
void operation(ConstraintDialog& dialog,int value){auto& box=combo(dialog,"constraintOperation");const auto index=box.findData(value);require(index>=0,"constraint operation available");box.setCurrentIndex(index);}
void relation(ConstraintDialog& dialog,ConstraintRelationKind kind){operation(dialog,1);auto& box=combo(dialog,"constraintRelation");box.setCurrentIndex(box.findData(static_cast<int>(kind)));}
void endpoint(ConstraintDialog& dialog,int slot,const QString& label){auto& box=combo(dialog,qPrintable(QStringLiteral("constraintBinding%1").arg(slot)));const auto index=box.findText(label);require(index>=0,"stable measured endpoint choice exists including terminal end");box.setCurrentIndex(index);}
using Use=std::function<void(ConstraintDialog&)>;
void edit(MainWindow& window,const Use& use){
    auto* button=window.findChild<QPushButton*>("editWallConstraints");require(button && button->isEnabled(),"measured stroke exposes actual constraints button");
    std::exception_ptr failure;bool opened=false;
    QTimer::singleShot(0,[&]{auto* dialog=dynamic_cast<ConstraintDialog*>(QApplication::activeModalWidget());
        if(!dialog){failure=std::make_exception_ptr(std::runtime_error("actual measured constraints dialog opens"));if(auto* modal=QApplication::activeModalWidget())modal->close();return;}
        opened=true;try{use(*dialog);}catch(...){failure=std::current_exception();dialog->reject();}});
    button->click();if(failure)std::rethrow_exception(failure);require(opened,"actual constraints callback ran");events();
}
MeasurementLineworkReplay replay(const DocumentSnapshot& snapshot,const std::string& id){const auto decoded=decode_measurement_linework_model(snapshot.entities().at(id).properties.at("model"));require(decoded.supported(),"saved measured model remains supported");return replay_measurement_linework(*decoded.model);}
void actual_constraint_workflow(){
    MainWindow window(fixture());window.setAttribute(Qt::WA_DontShowOnScreen,true);window.resize(1200,800);window.show();window.setMetricUnits(true);require(window.selectEntity("a"),"select open measured stroke");events();
    const auto original=window.document().snapshot();
    edit(window,[&](ConstraintDialog& dialog){dialog.setLengthExpression("11 ft");require(dialog.previewEdit(),"measured resize previews");require(dialog.findChild<QTableWidget*>("constraintChanges")->rowCount()==2,"measured replay segment changes appear in preview table");require(window.document().snapshot().entities()==original.entities(),"preview does not mutate measured sources");dialog.reject();});
    require(window.document().snapshot().entities()==original.entities() && window.document().revision()==original.revision(),"Cancel retains exact source and history");
    edit(window,[&](ConstraintDialog& dialog){dialog.setLengthExpression("10 ft");require(dialog.previewEdit(),"unchanged measured length can preview");
        dialog.setLengthExpression("invalid");require(!dialog.submit(),"input changes invalidate unchanged resize acceptance");require(!dialog.previewEdit(),"invalid measured length is refused");
        dialog.setLengthExpression("10 ft");require(dialog.previewEdit() && dialog.submit() && !dialog.acceptedPreview(),"unchanged measured length can Apply without a fabricated service receipt");});
    require(window.document().snapshot().entities()==original.entities() && window.document().revision()==original.revision(),"unchanged measured Apply records no command");
    edit(window,[&](ConstraintDialog& dialog){relation(dialog,ConstraintRelationKind::fixed_length);dialog.setLengthExpression("10 ft");require(dialog.previewEdit(),"open measured segment exact fixed length previews");require(dialog.findChild<QLabel*>("constraintPersistentFreedom")->text().contains("Stored"),"measured component freedom is displayed");require(dialog.submit(),"measured fixed length applies");});
    const auto fixed=window.document().snapshot();require(fixed.revision()==original.revision()+1,"actual fixed length records one command");
    bool exact=false;for(const auto& [id,entity]:fixed.entities())if(entity.type=="constraint"){const auto decoded=decode_constraint_entity(entity);if(decoded.constraint && decoded.constraint->length){exact=decoded.constraint->length->original_expression=="10 ft";require_near(decoded.constraint->length->metres,3.048);}}
    require(exact,"stored relation retains authored exact quantity expression");
    edit(window,[&](ConstraintDialog& dialog){relation(dialog,ConstraintRelationKind::coincident);endpoint(dialog,0,"Measured stroke · a · Edge 2 · end");endpoint(dialog,1,"Measured stroke · b-stroke · Edge 1 · start");require(dialog.previewEdit() && dialog.submit(),"terminal vertex cross-owner coincidence applies");});
    const auto linked=window.document().snapshot();require(linked.revision()==fixed.revision()+1,"terminal coincidence is one command");
    require(window.selectEntity("a") && window.moveSelectedBoundaryVertex("a:v2",{4,3},linked.revision()),"compatible terminal movement propagates through stored stroke coincidence");
    capture(window,"connected-measured-strokes.png");
    const auto moved=window.document().snapshot();require_near(replay(moved,"a").edges.back().segment.end.x,4);require_near(replay(moved,"b-stroke").edges.front().segment.start.x,4);require_near(replay(moved,"b-stroke").edges.front().segment.start.y,3);
    require_near(segment_length(replay(moved,"a").edges.front().segment),3.048);
    require(!window.moveSelectedBoundaryVertex("a:v1",{4,0},moved.revision()) && window.document().snapshot().entities()==moved.entities(),"incompatible authored edit refuses without changing sources or constraints");
    require(window.undoCommand() && window.document().snapshot().entities()==linked.entities() && window.redoCommand() && window.document().snapshot().entities()==moved.entities(),"connected measured movement undo redo is atomic");
    require(window.selectEntity("a"),"select measured stroke for actual context menu action");
    auto* canvas=dynamic_cast<PlanCanvas*>(window.findChild<QWidget*>("measurementPlanCanvas"));require(canvas,"actual plan canvas exists");std::exception_ptr failure;bool triggered=false;
    QTimer::singleShot(0,[&]{auto* menu=qobject_cast<QMenu*>(QApplication::activePopupWidget());if(!menu){failure=std::make_exception_ptr(std::runtime_error("actual canvas context menu opens"));return;}
        QAction* action=nullptr;for(auto* candidate:menu->actions())if(candidate->text()=="Dimensions and constraints…")action=candidate;
        if(!action){failure=std::make_exception_ptr(std::runtime_error("actual measured context QAction is gated on supported entity"));menu->close();return;}
        menu->close();QTimer::singleShot(0,[&]{auto* dialog=dynamic_cast<ConstraintDialog*>(QApplication::activeModalWidget());if(dialog)dialog->reject();else failure=std::make_exception_ptr(std::runtime_error("context QAction opens actual measured editor"));});triggered=true;action->trigger();});
    const QPointF position(10,10);QMouseEvent press(QEvent::MouseButtonPress,position,canvas->mapToGlobal(position.toPoint()),Qt::RightButton,Qt::RightButton,Qt::NoModifier);QApplication::sendEvent(canvas,&press);
    QMouseEvent release(QEvent::MouseButtonRelease,position,canvas->mapToGlobal(position.toPoint()),Qt::RightButton,Qt::NoButton,Qt::NoModifier);QApplication::sendEvent(canvas,&release);
    if(failure)std::rethrow_exception(failure);require(triggered,"actual measured context QAction triggered");
}
void curve_and_partner_choices(){
    auto document=fixture();auto arc=stroke("arc",{{0,5},{2,5}},true);document->apply(ApplyEntityChanges{document->revision(),{EntityChange::upsert(arc)},{},"Arc fixture"});
    MainWindow window(document);window.setAttribute(Qt::WA_DontShowOnScreen,true);window.resize(1200,800);window.show();window.setMetricUnits(true);
    require(window.selectEntity("arc"),"select measured arc in actual workspace");events();
    const auto snapshot=document->snapshot();require(ConstraintDialog::supportsEntity(arc),"supported receipt arc is available to constraints");
    edit(window,[&](ConstraintDialog& dialog){
    relation(dialog,ConstraintRelationKind::fixed_arc_length);auto* length=dialog.findChild<QLineEdit*>("constraintLength");require_near(parse_quantity(length->text().toStdString()).metres,segment_length(replay(snapshot,"arc").edges.front().segment));
    relation(dialog,ConstraintRelationKind::fixed_length);require_near(parse_quantity(length->text().toStdString()).metres,2);
    relation(dialog,ConstraintRelationKind::fixed_arc_length);dialog.setLengthExpression("4 m");require(dialog.previewEdit(),"measured physical curve length previews with preserved sweep");
    dialog.setAttribute(Qt::WA_DontShowOnScreen,true);dialog.show();capture(dialog,"measured-arc-constraint-preview.png");
    require(dialog.submit() && dialog.acceptedPreview() && !dialog.acceptedPreview()->changed_measured_strokes().empty(),"curve preview exposes actual measured replay changes");
    const auto accepted=dialog.acceptedPreview();const auto& change=accepted->changed_measured_strokes().front();require_near(segment_length(change.after.edges.front().segment),4);require_near(change.before.edges.front().segment.sweep_radians,change.after.edges.front().segment.sweep_radians);
    require(document->snapshot().entities()==snapshot.entities(),"curve dialog preview leaves immutable receipt source unchanged");
    });
    require(document->revision()==snapshot.revision()+1,"actual curve dialog commits one measured constraint transaction");
    const auto locked=document->snapshot();const auto original=locked.entities().at("arc");
    require(original.properties.at("model").at("version")==5,"solved measured curve persists simultaneous vertex batch dialect");
    require(window.selectEntity("arc") && window.copySelection() && window.pasteSelection(),"copy and paste constrained batch-derived measured curve");
    const auto pasted=document->snapshot();const auto copied_id=window.selectedEntityId().toStdString();
    require(copied_id!="arc" && pasted.entities().at("arc")==original,"constrained pasted curve has independent owner and leaves original unchanged");
    const auto copied_model=decode_measurement_linework_model(pasted.entities().at(copied_id).properties.at("model"));
    require(copied_model.supported() && copied_model.model->stroke_id==copied_id,"copied v5 operation targets remap to new owner");
    require_near(segment_length(replay_measurement_linework(*copied_model.model).edges.front().segment),4);
    bool copied_lock=false;for(const auto& [id,entity]:pasted.entities())if(entity.type=="constraint"){
        const auto decoded=decode_constraint_entity(entity);if(decoded.constraint && decoded.constraint->relation==ConstraintRelationKind::fixed_arc_length)
            for(const auto& binding:decoded.constraint->bindings)if(binding.owner_id==copied_id)copied_lock=true;
    }
    require(copied_lock,"pasted measured curve retains independent physical-length constraint bindings");
    require(window.undoCommand() && document->snapshot().entities()==locked.entities() && window.redoCommand() && document->snapshot().entities()==pasted.entities(),"constrained measured paste is one reversible graph edit");
    auto invalid=arc;invalid.properties["model"]["version"]=999;require(!ConstraintDialog::supportsEntity(invalid),"unsupported measured dialect is not offered for constraints");
}
void all_relations_and_owner_adapters(){
    auto document=fixture();
    Entity wall{"wall","wall",{{"name","partner"},{"baseline",{{"start",{0,0}},{"end",{2,0}},{"sweep_radians",0}}},
        {"thickness_m",0.2},{"height_m",3},{"elevation_m",0}},false};
    IdentifiedBoundary shape{"boundary","boundary",{}};const Vec2 points[]{{0,0},{2,0},{2,2},{0,2}};
    for(std::size_t index=0;index<4;++index)shape.segments.push_back({"c:e"+std::to_string(index),"c:v"+std::to_string(index),"c:v"+std::to_string((index+1)%4),{points[index],points[(index+1)%4],0}});
    auto boundary=encode_identified_boundary_entity(shape);boundary.properties["name"]="partner";
    document->apply(ApplyEntityChanges{document->revision(),{EntityChange::upsert(wall),EntityChange::upsert(boundary),EntityChange::upsert(stroke("partner",{{0,0},{2,0}}))},{},"Partner adapters"});
    const auto snapshot=document->snapshot();
    for(const auto& partner:QStringList{"Measured stroke · partner · Edge 1 · ","Wall · partner · ","Boundary · partner · Edge 1 · "})
        for(const auto kind:{ConstraintRelationKind::horizontal,ConstraintRelationKind::vertical,ConstraintRelationKind::coincident,
            ConstraintRelationKind::fixed_length,ConstraintRelationKind::parallel,ConstraintRelationKind::perpendicular}){
            ConstraintDialog dialog(snapshot,"a",true);relation(dialog,kind);
            const bool vertical=kind==ConstraintRelationKind::vertical || kind==ConstraintRelationKind::perpendicular;
            const auto selected=vertical?QStringLiteral("Measured stroke · a · Edge 2 · "):QStringLiteral("Measured stroke · a · Edge 1 · ");
            endpoint(dialog,0,selected+"start");endpoint(dialog,1,kind==ConstraintRelationKind::coincident?partner+"start":selected+"end");
            if(kind==ConstraintRelationKind::parallel || kind==ConstraintRelationKind::perpendicular){endpoint(dialog,2,partner+"start");endpoint(dialog,3,partner+"end");}
            if(kind==ConstraintRelationKind::fixed_length)dialog.setLengthExpression("10 ft");
            require(dialog.previewEdit() && dialog.submit(),"six measured endpoint relations preview through stroke wall and identified-boundary adapters");
            require(dialog.acceptedPreview()->candidate_entities().size()==snapshot.entities().size()+1,"relation preview retains owners and adds one persistent relation");
        }
    require(document->snapshot().entities()==snapshot.entities(),"independent adapter previews never mutate source owners");
}
}
int main(int argc,char** argv){sketch::testing::noninteractive_errors();QStandardPaths::setTestModeEnabled(true);QApplication app(argc,argv);QCoreApplication::setApplicationName(QStringLiteral("Vertex-measured-constraints-")+QUuid::createUuid().toString(QUuid::WithoutBraces));
    require(QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/Inter.ttf"))>=0,"bundled Inter font loads");app.setFont(QFont(QStringLiteral("Inter"),10));
    try{actual_constraint_workflow();curve_and_partner_choices();all_relations_and_owner_adapters();}catch(const std::exception& error){std::cerr<<"measurement_linework_constraints_desktop_tests: "<<error.what()<<'\n';return 1;}
    std::cout<<"Measured linework constraint desktop tests passed\n";return 0;
}
