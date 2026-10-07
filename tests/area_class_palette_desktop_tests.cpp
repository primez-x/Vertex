#include "support/detached_document_snapshot.hpp"
#include "sketch/desktop/main_window.hpp"
#include "sketch/desktop/area_class_palette.hpp"
#include "sketch/boundary_entity.hpp"
#include "sketch/measurement_linework.hpp"
#include "sketch/area_type_presets.hpp"
#include "sketch/appraisal_document.hpp"
#include "../src/desktop/plan_canvas.hpp"
#include "support/noninteractive_errors.hpp"
#include <QApplication>
#include <QAbstractItemModel>
#include <QComboBox>
#include <QDragEnterEvent>
#include <QDrag>
#include <QDropEvent>
#include <QDir>
#include <QFontDatabase>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMimeData>
#include <QMouseEvent>
#include <QPushButton>
#include <QSettings>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QWindow>
#include <QtTest/qtestmouse.h>
#include <cmath>
#include <memory>
#include <iostream>
#include <stdexcept>

namespace {
using namespace sketch;
using namespace sketch::desktop;
void require(bool value,const char* message) { if(!value) throw std::runtime_error(message); }
template<class T> T& child(QObject& owner,const char* name) {
    auto* value=owner.findChild<T*>(QString::fromLatin1(name));require(value,"control exists");return *value;
}
QPointF pixel(PlanCanvas& canvas,Vec2 point) {
    const auto center=canvas.viewCenter();const auto scale=canvas.viewScale();
    return {canvas.rect().center().x()+(point.x-center.x)*scale,
        canvas.rect().center().y()-(point.y-center.y)*scale};
}
void click(QWidget& target,QPointF point) {
    QMouseEvent press(QEvent::MouseButtonPress,point,target.mapToGlobal(point.toPoint()),Qt::LeftButton,Qt::LeftButton,Qt::NoModifier);
    QApplication::sendEvent(&target,&press);
    QMouseEvent release(QEvent::MouseButtonRelease,point,target.mapToGlobal(point.toPoint()),Qt::LeftButton,Qt::NoButton,Qt::NoModifier);
    QApplication::sendEvent(&target,&release);QApplication::processEvents();
}
bool drop(QWidget& target,QPointF point,const QString& classification) {
    QMimeData mime;mime.setData(area_class_mime_type,encode_area_class_drag(classification));
    QDragEnterEvent enter(point.toPoint(),Qt::CopyAction,&mime,Qt::LeftButton,Qt::NoModifier);
    QApplication::sendEvent(&target,&enter);
    QDropEvent event(point,Qt::CopyAction,&mime,Qt::LeftButton,Qt::NoModifier);
    QApplication::sendEvent(&target,&event);QApplication::processEvents();return event.isAccepted();
}
void platformPointer(QWidget& widget,QEvent::Type type,QPoint point,
    Qt::MouseButton button,Qt::MouseButtons buttons) {
    const auto global=widget.mapToGlobal(point);auto* window=widget.window()->windowHandle();
    static int timestamp=1000;
    qt_handleMouseEvent(window,QPointF(window->mapFromGlobal(global)),QPointF(global),
        buttons,button,type,Qt::NoModifier,timestamp+=100);
    QApplication::processEvents();
}
QListWidgetItem* classItem(QListWidget& classes, const QString& key) {
    for (int i = 0; i < classes.count(); ++i)
        if (classes.item(i)->data(Qt::UserRole).toString() == key) return classes.item(i);
    return nullptr;
}
void dragVisibleClass(QListWidget& classes,PlanCanvas& canvas,QPoint target) {
    auto* intended = classItem(classes, QStringLiteral("garage"));
    require(classes.isVisible()&&canvas.isVisible()&&intended,"class drag uses visible filtered palette and canvas");
    const auto start=classes.visualItemRect(intended).center();
    require(classes.itemAt(start)==intended,"class pointer begins on the intended item");
    QTimer movement;movement.setSingleShot(true);
    QObject::connect(&movement,&QTimer::timeout,&canvas,[&] {
        platformPointer(canvas,QEvent::MouseMove,target,Qt::NoButton,Qt::LeftButton);
        platformPointer(canvas,QEvent::MouseButtonRelease,target,Qt::LeftButton,Qt::NoButton);
    });
    QTimer watchdog;watchdog.setSingleShot(true);
    QObject::connect(&watchdog,&QTimer::timeout,[]{QDrag::cancel();});
    platformPointer(*classes.viewport(),QEvent::MouseButtonPress,start,Qt::LeftButton,Qt::LeftButton);
    require(intended->isSelected(),"platform pointer selects the intended class before dragging");
    platformPointer(*classes.viewport(),QEvent::MouseMove,start+QPoint(1,0),Qt::NoButton,Qt::LeftButton);
    movement.start(100);watchdog.start(3000);
    // This enters the actual ClassList::startDrag / Qt platform drag loop.
    // The helper does not manufacture MIME or a drop event.
    platformPointer(*classes.viewport(),QEvent::MouseMove,start+QPoint(QApplication::startDragDistance()+8,0),Qt::NoButton,Qt::LeftButton);
    QApplication::processEvents();watchdog.stop();
}
void capture(QWidget& widget,const char* name) {
    const auto directory=qEnvironmentVariable("VERTEX_TEST_CAPTURE_DIR");if(directory.isEmpty())return;
    QApplication::processEvents();require(QDir().mkpath(directory)&&widget.grab().save(QDir(directory).filePath(name)),"actual area palette capture saves");
}
QListWidgetItem* areaRow(QListWidget& rows,const QString& id) {
    for(int i=0;i<rows.count();++i)if(rows.item(i)->data(Qt::UserRole).toString()==id)return rows.item(i);
    return nullptr;
}
void workflow() {
    QTemporaryDir files;MainWindow window({},nullptr,files.filePath("text.json"));
    // The offscreen platform remains invisible to the desktop, but QDrag needs
    // an exposed platform window for hit testing; DontShowOnScreen bypasses it.
    window.resize(1200,900);window.show();QApplication::processEvents();
    auto& canvas=*dynamic_cast<PlanCanvas*>(&child<QWidget>(window,"measurementPlanCanvas"));
    canvas.setOverviewMapEnabled(false);
    auto profile_source=window.document().snapshot();
    for(const auto& [id,entity]:profile_source.entities())if(entity.type=="property") {
        auto property=entity;
        property.properties["calculation_profile"]["classifications"]["garage"]={{"building_total",true},{"living_total",false}};
        window.document().apply(ApplyEntityChanges{profile_source.revision(),{EntityChange::upsert(property)}, {},"fixture profile"});break;
    }
    const Boundary square{{{0,0},{2,0}},{{2,0},{2,2}},{{2,2},{0,2}},{{0,2},{0,0}}};
    const auto first=window.createBoundary(square,"living");
    Boundary next=square;for(auto& edge:next) {edge.start.x+=3;edge.end.x+=3;}
    const auto second=window.createBoundary(next,"living");require(!first.isEmpty()&&!second.isEmpty(),"closed areas created");
    canvas.fitView();auto source=window.document().snapshot();auto retained=source.entities().at(first.toStdString());
    retained.properties["appraisal_facts"]={{"finish","unfinished"}};retained.extensions["opaque"]={{"keep",17}};
    window.document().apply(ApplyEntityChanges{source.revision(),{EntityChange::upsert(retained)}, {},"fixture facts"});
    require(window.selectEntity(first),"refresh retained facts fixture");source=window.document().snapshot();
    auto& tabs=child<QTabWidget>(window,"sidebarTabs");tabs.setCurrentIndex(1);
    child<QTabWidget>(window,"libraryPages").setCurrentIndex(1);QApplication::processEvents();
    auto& search=child<QLineEdit>(window,"areaClassSearch");auto& list=child<QListWidget>(window,"areaClassItems");
    search.setText("gar");require(list.count()>0,"search finds garage");
    for(int i=0;i<list.count();++i)require(list.item(i)->text().contains("gar",Qt::CaseInsensitive),"search removes unrelated classes");
    search.clear();auto& filter=child<QComboBox>(window,"areaClassCategory");
    filter.setCurrentIndex(filter.findData("living"));require(list.count()>0,"living category has current profile entries");
    require(list.findItems("Garage",Qt::MatchFixedString).isEmpty(),"category excludes garage");filter.setCurrentIndex(0);
    const auto before_profile_change=window.document().snapshot();
    for(const auto& [id,entity]:before_profile_change.entities())if(entity.type=="property") {
        auto property=entity;property.properties["calculation_profile"]["classifications"].erase("garage");
        window.document().apply(ApplyEntityChanges{before_profile_change.revision(),{EntityChange::upsert(property)}, {},"fixture profile change"});break;
    }
    require(window.selectEntity(first),"refresh changed profile");search.setText("garage");
    require(!classItem(list,"garage"),"palette removes class deleted from current profile without removing detached garage");
    require(window.undoCommand(),"restore configured fixture profile");require(classItem(list,"garage"),"profile Undo refreshes classes");search.clear();
    search.setText("garage");
    dragVisibleClass(list,canvas,pixel(canvas,{1,1}).toPoint());search.clear();
    auto changed=window.document().snapshot();auto expected=source.entities().at(first.toStdString());
    expected.properties["classification"]="garage";expected.properties["measurement_classification"]="garage";
    if(changed.entities().at(first.toStdString())!=expected) {
        for(const auto& record:changed.history())if(record.revision>source.revision())std::cerr<<"Pointer history "<<record.revision<<": "<<record.action<<'\n';
        std::cerr<<"Pointer property difference: "<<nlohmann::json::diff(expected.properties,changed.entities().at(first.toStdString()).properties).dump()<<'\n';
        throw std::runtime_error("Pointer class drag differs: "+window.lastError().toStdString()+
            "; class="+changed.entities().at(first.toStdString()).properties.value("classification",std::string("absent"))+
            "; revision="+std::to_string(changed.revision())+"; expected source="+std::to_string(source.revision()));
    }
    require(changed.entities().at(second.toStdString())==source.entities().at(second.toStdString()),"drop scoped to one owner");
    require(window.undoCommand()&&window.document().snapshot().entities()==source.entities(),"one Undo restores exact class");
    auto& cached_rows=child<QListWidget>(window,"areaClassTargets");
    cached_rows.setCurrentItem(areaRow(cached_rows,first));
    require(window.selectEntity(second)&&cached_rows.currentItem()&&cached_rows.currentItem()->data(Qt::UserRole).toString()==first,
        "selection-only inspector refresh preserves the unchanged area list and its selection");
    search.setText("garage");auto* garage_item=classItem(list,"garage");require(garage_item,"garage entry exists among garage types");
    click(*list.viewport(),list.visualItemRect(garage_item).center());
    capture(window,"area-class-palette-armed.png");
    const auto source_mime=std::unique_ptr<QMimeData>(list.model()->mimeData({list.model()->index(list.row(garage_item),0)}));
    require(source_mime && source_mime->hasFormat("application/x-vertex-area-class") &&
        source_mime->data("application/x-vertex-area-class").contains("garage"),"actual palette model produces external class drag");
    const auto before_pan=window.document().snapshot();const auto before_center=canvas.viewCenter();
    const auto pan_start=pixel(canvas,{1,1});const auto pan_end=pan_start+QPointF(35,15);
    QMouseEvent pan_press(QEvent::MouseButtonPress,pan_start,canvas.mapToGlobal(pan_start.toPoint()),Qt::LeftButton,Qt::LeftButton,Qt::NoModifier);
    QMouseEvent pan_move(QEvent::MouseMove,pan_end,canvas.mapToGlobal(pan_end.toPoint()),Qt::NoButton,Qt::LeftButton,Qt::NoModifier);
    QMouseEvent pan_release(QEvent::MouseButtonRelease,pan_end,canvas.mapToGlobal(pan_end.toPoint()),Qt::LeftButton,Qt::NoButton,Qt::NoModifier);
    QApplication::sendEvent(&canvas,&pan_press);QApplication::sendEvent(&canvas,&pan_move);QApplication::sendEvent(&canvas,&pan_release);
    require(window.document().snapshot().entities()==before_pan.entities()&&
        (canvas.viewCenter().x!=before_center.x||canvas.viewCenter().y!=before_center.y),"armed drag pans and never classifies");
    const auto roundtrip_start=pixel(canvas,{1,1});const auto roundtrip_far=roundtrip_start+QPointF(45,20);
    const auto roundtrip_end=roundtrip_start+QPointF(1,1);const auto roundtrip_source=window.document().snapshot();
    QMouseEvent roundtrip_press(QEvent::MouseButtonPress,roundtrip_start,canvas.mapToGlobal(roundtrip_start.toPoint()),Qt::LeftButton,Qt::LeftButton,Qt::NoModifier);
    QMouseEvent roundtrip_move(QEvent::MouseMove,roundtrip_far,canvas.mapToGlobal(roundtrip_far.toPoint()),Qt::NoButton,Qt::LeftButton,Qt::NoModifier);
    QMouseEvent roundtrip_return(QEvent::MouseMove,roundtrip_end,canvas.mapToGlobal(roundtrip_end.toPoint()),Qt::NoButton,Qt::LeftButton,Qt::NoModifier);
    QMouseEvent roundtrip_release(QEvent::MouseButtonRelease,roundtrip_end,canvas.mapToGlobal(roundtrip_end.toPoint()),Qt::LeftButton,Qt::NoButton,Qt::NoModifier);
    QApplication::sendEvent(&canvas,&roundtrip_press);QApplication::sendEvent(&canvas,&roundtrip_move);
    QApplication::sendEvent(&canvas,&roundtrip_return);QApplication::sendEvent(&canvas,&roundtrip_release);
    require(window.document().snapshot().entities()==roundtrip_source.entities()&&child<QLabel>(window,"areaClassStatus").text().contains("armed"),
        "armed drag that returns near its press remains a pan and preserves the pending class");
    click(canvas,pixel(canvas,{1,1}));click(canvas,pixel(canvas,{4,1}));
    require(window.document().snapshot().entities().at(first.toStdString()).properties.at("classification")=="garage"&&
        window.document().snapshot().entities().at(second.toStdString()).properties.at("classification")=="garage","armed class applies repeatedly");
    QKeyEvent escape(QEvent::KeyPress,Qt::Key_Escape,Qt::NoModifier);QApplication::sendEvent(&canvas,&escape);
    const auto cancelled=window.document().snapshot();click(canvas,pixel(canvas,{1,1}));
    require(window.document().snapshot().entities()==cancelled.entities(),"Escape disarms without mutation");
    require(!drop(canvas,pixel(canvas,{20,20}),"garage"),"empty canvas drop rejects");
    require(!drop(canvas,pixel(canvas,{1,1}),"invented class"),"unconfigured class rejects");
    const auto before_clear=window.document().snapshot();
    require(drop(canvas,pixel(canvas,{1,1}),{}),"clear classification drop accepted");
    require(!window.document().snapshot().entities().at(first.toStdString()).properties.contains("classification"),"clear removes class without fabricating facts");
    auto cleared=before_clear.entities().at(first.toStdString());cleared.properties.erase("classification");cleared.properties.erase("measurement_classification");
    require(window.document().snapshot().entities().at(first.toStdString())==cleared,"clear preserves all geometry, source and facts");
    require(window.undoCommand(),"clear Undo works");
    auto& rows=child<QListWidget>(window,"areaClassTargets");QListWidgetItem* row=nullptr;
    for(int i=0;i<rows.count();++i)if(rows.item(i)->data(Qt::UserRole).toString()==first)row=rows.item(i);
    require(row,"existing area row is present");const auto row_point=rows.visualItemRect(row).center();
    const auto before_stale=window.document().snapshot();
    for(const auto& [id,entity]:before_stale.entities())if(entity.type=="property") {
        auto property=entity;property.extensions["fixture_stale_row"]=true;
        window.document().apply(ApplyEntityChanges{before_stale.revision(),{EntityChange::upsert(property)}, {},"fixture stale area row"});break;
    }
    const auto stale=window.document().snapshot();require(!drop(*rows.viewport(),row_point,"living")&&
        window.document().snapshot().entities()==stale.entities()&&window.lastError().contains("area list changed"),"stale list refuses without partial reclassification");
    require(window.undoCommand(),"restore stale fixture source");row=nullptr;
    for(int i=0;i<rows.count();++i)if(rows.item(i)->data(Qt::UserRole).toString()==first)row=rows.item(i);
    require(row&&drop(*rows.viewport(),rows.visualItemRect(row).center(),"living"),"actual area row drop reclassifies");
    auto overlapping=window.createBoundary(square,"living");require(!overlapping.isEmpty(),"ambiguous-owner fixture exists");
    const auto ambiguity=window.document().snapshot();require(!drop(canvas,pixel(canvas,{1,1}),"garage")&&
        window.document().snapshot().entities()==ambiguity.entities(),"overlapping owners reject atomically");
    require(window.undoCommand(),"remove ambiguous fixture");
    const auto saved=window.document().snapshot();const auto path=files.filePath("classes.bldproj");
    require(window.saveProjectAs(path)&&window.createNewProject(),"save releases project");
    MainWindow reopened({},nullptr,files.filePath("reopen-text.json"));reopened.setAttribute(Qt::WA_DontShowOnScreen);
    require(reopened.openProject(path)&&reopened.document().snapshot().entities()==saved.entities(),"classes and exact facts reopen");
    reopened.resize(1200,900);reopened.show();QApplication::processEvents();
    auto& reopened_canvas=*dynamic_cast<PlanCanvas*>(&child<QWidget>(reopened,"measurementPlanCanvas"));reopened_canvas.setOverviewMapEnabled(false);reopened_canvas.fitView();
    reopened.document().mark_read_only("fixture");const auto readonly=reopened.document().snapshot();
    require(!drop(reopened_canvas,pixel(reopened_canvas,{1,1}),"garage")&&reopened.document().snapshot().entities()==readonly.entities()&&
        reopened.lastError().contains("read-only"),"read-only drops refuse without mutation");
}

Entity stroke(std::string id,const std::vector<Vec2>& points,bool closed) {
    MeasurementLinework model;model.stroke_id=id;model.anchor=points.front();model.closed=closed;
    for(std::size_t i=1;i<points.size();++i) {
        const auto segment=id+":e"+std::to_string(i);ConstructionReceipt receipt;receipt.segment_id=segment;
        receipt.kind=BoundaryConstructionKind::line_to_point;receipt.start=points[i-1];receipt.chord_end=points[i];
        model.edges.push_back({segment,id+":v"+std::to_string(i-1),closed&&i==points.size()-1?id+":v0":id+":v"+std::to_string(i),receipt});
    }
    return {id,"measurement_linework",{{"property_id","p"},{"building_id","b"},{"floor_id","f"},
        {"layer_id","l"},{"model",encode_measurement_linework_model(model)}},true};
}
void detected_spaces() {
    auto document=std::make_shared<Document>(Document::create({
        {"p","property",{{"name","Measured spaces"}},false},{"b","building",{{"property_id","p"}},false},
        {"f","floor",{{"building_id","b"}},false},{"l","layer",{{"floor_id","f"}},false},
        stroke("outline",{{0,0},{4,0},{4,4},{0,4},{0,0}},true),stroke("separator",{{2,-1},{2,5}},false)}));
    QTemporaryDir files;MainWindow window(document,nullptr,files.filePath("text.json"));
    window.setAttribute(Qt::WA_DontShowOnScreen);window.resize(1200,900);window.show();QApplication::processEvents();
    require(window.setActiveLayer("l"),"detected fixture layer activates");
    auto& canvas=*dynamic_cast<PlanCanvas*>(&child<QWidget>(window,"measurementPlanCanvas"));canvas.setOverviewMapEnabled(false);canvas.fitView();
    const auto source=window.document().snapshot();
    child<QTabWidget>(window,"sidebarTabs").setCurrentIndex(1);child<QTabWidget>(window,"libraryPages").setCurrentIndex(1);QApplication::processEvents();
    auto& stale_rows=child<QListWidget>(window,"areaClassTargets");QListWidgetItem* stale_target=nullptr;
    for(int i=0;i<stale_rows.count();++i)if(stale_rows.item(i)->data(Qt::UserRole).toString().startsWith("detected-area:"))stale_target=stale_rows.item(i);
    require(stale_target,"retained detected row exists");const auto stale_point=stale_rows.visualItemRect(stale_target).center();
    sketch::test::DetachedDocumentSnapshotFixture replaced(source);
    replaced.history().front().entities.at("separator")=stroke("separator",{{1,-1},{1,5}},false);
    window.document()=Document::fork(replaced);const auto changed=window.document().snapshot();
    require(changed.revision()==source.revision()&&changed.document_id()==source.document_id()&&changed.entities()!=source.entities(),"detected replacement changes geometry at the same immutable identity and revision");
    require(!drop(*stale_rows.viewport(),stale_point,"living")&&window.document().snapshot().entities()==changed.entities()&&window.lastError().contains("area list changed"),
        "detected row cannot reuse a face index from same-revision replaced geometry");
    window.document()=Document::fork(source);require(window.selectEntity("outline"),"refresh original detection source");
    capture(window,"area-class-palette-detected-outlines.png");
    require(drop(canvas,pixel(canvas,{1,2}),"living"),"drop creates first measured face");
    auto first=window.document().snapshot();int count=0;QString first_id;
    for(const auto& [id,entity]:first.entities())if(entity.type=="measurement_boundary") {
        ++count;first_id=QString::fromStdString(id);require(entity.properties.at("classification")=="living"&&
            entity.extensions.contains("measurement_linework_sources"),"detected class retains analytical lineage");
        require(std::abs(signed_area(boundary_geometry(decode_identified_boundary_entity(entity)))-8)<1e-9,"one face has exactly eight square metres");
        require(!entity.properties.contains("appraisal_facts"),"detected label creates no eligibility facts");
    }
    require(count==1&&first.entities().at("outline")==source.entities().at("outline")&&
        first.entities().at("separator")==source.entities().at("separator"),"drop creates only its face without rewriting sources");
    child<QTabWidget>(window,"sidebarTabs").setCurrentIndex(1);child<QTabWidget>(window,"libraryPages").setCurrentIndex(1);QApplication::processEvents();
    auto& rows=child<QListWidget>(window,"areaClassTargets");QListWidgetItem* target=nullptr;
    for(int i=0;i<rows.count();++i)if(rows.item(i)->data(Qt::UserRole).toString().startsWith("detected-area:"))target=rows.item(i);
    require(target&&drop(*rows.viewport(),rows.visualItemRect(target).center(),"room"),"actual detected row drop creates remaining face");
    const auto both=window.document().snapshot();count=0;for(const auto& [id,entity]:both.entities())if(entity.type=="measurement_boundary")++count;
    require(count==2&&both.entities().at(first_id.toStdString())==first.entities().at(first_id.toStdString()),"row drop retains existing area identity and facts");
    require(window.undoCommand()&&window.document().snapshot().entities()==first.entities(),"detected row is one Undo");
    require(window.undoCommand()&&window.document().snapshot().entities()==source.entities(),"first detected drop is one Undo");
    require(window.redoCommand()&&window.redoCommand(),"detected definitions redo");
    capture(window,"area-class-palette-detected-spaces.png");
    const auto saved=window.document().snapshot();const auto path=files.filePath("detected.bldproj");
    require(window.saveProjectAs(path)&&window.createNewProject(),"detected fixture saves and releases");
    MainWindow reopened({},nullptr,files.filePath("reopened.json"));reopened.setAttribute(Qt::WA_DontShowOnScreen);
    require(reopened.openProject(path)&&reopened.document().snapshot().entities()==saved.entities(),"detected identities, geometry and source lineage reopen");
}
void appraisal_authority() {
    QTemporaryDir files;MainWindow window({},nullptr,files.filePath("appraisal-text.json"));
    window.setAttribute(Qt::WA_DontShowOnScreen);window.resize(1200,900);window.show();QApplication::processEvents();
    const Boundary square{{{0,0},{2,0}},{{2,0},{2,2}},{{2,2},{0,2}},{{0,2},{0,0}}};
    const auto area=window.createBoundary(square,"living");require(!area.isEmpty(),"appraisal fixture boundary exists");
    const auto source=window.document().snapshot();std::vector<EntityChange> changes;
    for(const auto& [id,entity]:source.entities())if(entity.type=="property") {
        auto property=entity;property.properties["calculation_workflow"]="appraisal";
        property.properties["appraisal_policy"]={{"policy_kind","residential_declared"},{"version",1},
            {"property_kind","detached_single_family"},{"measurement_basis","exterior"}};
        changes.push_back(EntityChange::upsert(property));
    }
    auto boundary=source.entities().at(area.toStdString());boundary.properties["appraisal_facts"]={{"finish","finished"},
        {"access","direct_interior"},{"ceiling_eligibility","standard"},{"area_use","living"},{"boundary_role","measured_area"}};
    changes.push_back(EntityChange::upsert(boundary));
    window.document().apply(ApplyEntityChanges{source.revision(),changes, {},"fixture declared appraisal authority"});
    require(window.selectEntity(area),"refresh declared appraisal fixture");
    auto& canvas=*dynamic_cast<PlanCanvas*>(&child<QWidget>(window,"measurementPlanCanvas"));canvas.setOverviewMapEnabled(false);canvas.fitView();
    const auto declared=window.document().snapshot();
    require(!drop(canvas,pixel(canvas,{1,1}),"garage")&&window.document().snapshot().entities()==declared.entities()&&
        window.lastError().contains("facts"),"palette cannot replace derived category or eligibility facts");
    require(!drop(canvas,pixel(canvas,{1,1}),{})&&window.document().snapshot().entities()==declared.entities(),"clear cannot erase declared appraisal facts");
    child<QTabWidget>(window,"sidebarTabs").setCurrentIndex(1);child<QTabWidget>(window,"libraryPages").setCurrentIndex(1);QApplication::processEvents();
    auto& rows=child<QListWidget>(window,"areaClassTargets");
    auto* row=areaRow(rows,area);require(row&&row->text().contains("Unqualified")&&row->text().contains("facts"),
        "incomplete facts own an unqualified row instead of its stale living class");
    auto qualified=declared.entities().at(area.toStdString());qualified.properties["appraisal_category"]="above_grade_finished";
    qualified.properties["appraisal_facts"]["area_use"]="garage";
    changes={EntityChange::upsert(qualified)};
    for(const auto& [id,entity]:declared.entities())if(entity.type=="floor") {
        auto floor=entity;floor.properties["appraisal_facts"]={{"grade","above"}};changes.push_back(EntityChange::upsert(floor));
    }
    window.document().apply(ApplyEntityChanges{declared.revision(),changes, {},"fixture derived garage conflicts with stored class"});
    require(window.selectEntity(area),"refresh derived garage fixture");row=areaRow(rows,area);
    require(row&&row->text().contains("Garage")&&!row->text().contains("Above-grade finished")&&row->text().contains("facts"),
        "derived garage category overrides conflicting stored appraisal category");
    capture(window,"area-class-palette-facts-derived.png");
    const auto garage=window.document().snapshot();auto excluded=garage.entities().at(area.toStdString());
    excluded.properties["appraisal_facts"]={{"boundary_role","other_void"}};
    window.document().apply(ApplyEntityChanges{garage.revision(),{EntityChange::upsert(excluded)}, {},"fixture qualified exclusion"});
    require(window.selectEntity(area),"refresh excluded fixture");row=areaRow(rows,area);
    require(row&&row->text().contains("Excluded")&&!row->text().contains("above_grade_finished")&&row->text().contains("facts"),
        "qualified exclusion replaces conflicting stored category in palette row");
}
void named_area_types() {
    QTemporaryDir files; MainWindow window({}, nullptr, files.filePath("types-text.json"));
    window.setAttribute(Qt::WA_DontShowOnScreen); window.resize(1200,900); window.show(); QApplication::processEvents();
    const Boundary square{{{0,0},{2,0}},{{2,0},{2,2}},{{2,2},{0,2}},{{0,2},{0,0}}};
    const auto area = window.createBoundary(square,"living"); require(!area.isEmpty(),"type fixture created");
    auto& classes = child<QListWidget>(window,"areaClassItems");
    auto& rows = child<QListWidget>(window,"areaClassTargets");
    auto& canvas = *dynamic_cast<PlanCanvas*>(&child<QWidget>(window,"measurementPlanCanvas"));
    canvas.setOverviewMapEnabled(false); canvas.fitView();
    const QStringList expected_labels{"First Floor","Second Floor","Third Floor","Fourth Floor","Gross Building Area",
        "Finished Basement","Unfinished Basement","Garage","Detached Garage","Accessory Dwelling / ADU",
        "Shed / Outbuilding","Carport","Porch","Patio","Wood Deck","Balcony","Storage",
        "Low Ceiling / Non-GLA","Open to Below","Non-Calculated Area","Subject Site"};
    for (const auto& label : expected_labels) require(classes.findItems(label,Qt::MatchFixedString).size()==1,"named type appears once in a new measurement project");
    require(classItem(classes,{}),"measurement clear choice is available");
    const auto initial = window.document().snapshot();
    for (const auto& preset : area_type_presets) {
        const auto key = QString::fromUtf8(preset.classification.data(),static_cast<qsizetype>(preset.classification.size()));
        require(drop(canvas,pixel(canvas,{1,1}),key),"each named measurement type applies");
        const auto changed = window.document().snapshot(); auto expected = initial.entities().at(area.toStdString());
        if (key.isEmpty()) { expected.properties.erase("classification"); expected.properties.erase("measurement_classification"); }
        else { expected.properties["classification"]=key.toStdString(); expected.properties["measurement_classification"]=key.toStdString(); }
        require(changed.entities().at(area.toStdString())==expected,"type changes only the chosen classification; geometry and context remain exact");
        require(window.undoCommand() && window.document().snapshot().entities()==initial.entities(),"each measurement type has one-step Undo");
    }
    const auto before_custom=window.document().snapshot(); std::string property_id;
    for (const auto& [id,entity] : before_custom.entities()) if (entity.type=="property") {
        property_id=id; auto property=entity; auto& profile=property.properties["calculation_profile"];
        profile["classifications"].erase("first_floor");
        profile["classifications"]["garage"]={{"building_total",false},{"living_total",false},{"appraisal_category","none"}};
        profile["retained_note"]="custom user rule";
        window.document().apply(ApplyEntityChanges{before_custom.revision(),{EntityChange::upsert(property)}, {},"type fixture custom profile"}); break;
    }
    require(window.selectEntity(area),"refresh old customized profile");
    const auto custom=window.document().snapshot(); auto& add_types=child<QPushButton>(window,"areaClassAddTypes");
    require(!classItem(classes,"first_floor") && !add_types.isHidden(),"old profile offers explicit addition instead of silently inserting presets");
    add_types.click(); const auto enabled=window.document().snapshot();
    auto expected_property=custom.entities().at(property_id);
    expected_property.properties["calculation_profile"]["classifications"]["first_floor"]={{"building_total",true},{"living_total",true},{"appraisal_category","none"}};
    require(enabled.entities().at(property_id)==expected_property && add_types.isHidden(),"add types fills only the missing rule and preserves custom flags and unknown profile metadata");
    require(window.undoCommand() && window.document().snapshot().entities()==custom.entities(),"add types has one-step Undo");
    require(window.undoCommand() && window.document().snapshot().entities()==before_custom.entities(),"custom fixture returns to initial profile");
    std::vector<EntityChange> changes;
    const auto measurement_source=window.document().snapshot();
    for (const auto& [id,entity] : measurement_source.entities()) if (entity.type=="property") {
        property_id=id; auto property=entity; property.properties["calculation_workflow"]="appraisal";
        property.properties["appraisal_policy"]={{"policy_kind","residential_declared"},{"version",1},
            {"property_kind","detached_single_family"},{"measurement_basis","exterior"}};
        changes.push_back(EntityChange::upsert(property));
    }
    window.document().apply(ApplyEntityChanges{window.document().snapshot().revision(),changes,{},"type fixture appraisal"});
    require(window.selectEntity(area),"refresh appraisal types");
    const auto appraisal_source=window.document().snapshot();
    const auto original_report=build_appraisal_document_report(appraisal_source,property_id);
    require(!original_report.qualified && !original_report.calculation,"incomplete fixture cannot produce GLA");
    for (const auto& preset : area_type_presets) {
        const auto token = QStringLiteral("area-type:")+QString::fromUtf8(preset.code.data(),static_cast<qsizetype>(preset.code.size()));
        require(classItem(classes,token),"all 22 drawing types are accessible in appraisal mode");
        if (preset.classification.empty()) continue; // Clear is checked against a retained type below.
        require(drop(canvas,pixel(canvas,{1,1}),token),"appraisal drawing type applies");
        const auto typed=window.document().snapshot(); auto expected=appraisal_source.entities().at(area.toStdString());
        expected.extensions["area_type"]={{"version",1},{"code",std::string(preset.code)}};
        require(typed.entities().at(area.toStdString())==expected,"appraisal type is descriptive and preserves facts/classification/geometry/floor");
        const auto report=build_appraisal_document_report(typed,property_id);
        require(!report.qualified && !report.calculation && report.issues==original_report.issues,"floor or GLA preset cannot manufacture eligibility or totals");
        require(drop(canvas,pixel(canvas,{1,1}),QStringLiteral("area-type:UND")),"drawing type clear applies");
        require(window.document().snapshot().entities()==appraisal_source.entities(),"type clear preserves every appraisal input");
        require(window.undoCommand() && window.document().snapshot().entities()==typed.entities(),"type clear Undo restores only the drawing type");
        require(window.undoCommand() && window.document().snapshot().entities()==appraisal_source.entities(),"type assignment Undo restores exact source");
    }
    require(!drop(canvas,pixel(canvas,{1,1}),QStringLiteral("area-type:UNKNOWN")),"unknown preset refuses");
    require(drop(canvas,pixel(canvas,{1,1}),QStringLiteral("area-type:BSMT-F")),"retained type fixture applies");
    auto* row=areaRow(rows,area); require(row && row->text().contains("Finished Basement") && row->text().contains("Unqualified"),"type label and qualification both visible");
    child<QTabWidget>(window,"sidebarTabs").setCurrentIndex(1); child<QTabWidget>(window,"libraryPages").setCurrentIndex(1);
    capture(window,"area-types-appraisal.png");
    const auto retained=window.document().snapshot(); const auto path=files.filePath("types.bldproj");
    require(window.saveProjectAs(path) && window.createNewProject(),"type project saves");
    require(window.openProject(path) && window.document().snapshot().entities()==retained.entities(),"type identity and all appraisal inputs reopen exactly");
}
void stale_sources() {
    QTemporaryDir files;MainWindow seed({},nullptr,files.filePath("seed.json"));
    const Boundary square{{{0,0},{2,0}},{{2,0},{2,2}},{{2,2},{0,2}},{{0,2},{0,0}}};
    const auto area=seed.createBoundary(square,"living");require(!area.isEmpty(),"stale source area exists");
    const auto seeded=seed.document().snapshot();std::vector<Entity> entities;std::vector<Asset> assets;
    for(const auto& [id,entity]:seeded.entities())entities.push_back(entity);
    for(const auto& [id,asset]:seeded.assets())assets.push_back(asset);
    MainWindow window(std::make_shared<Document>(Document::create(entities,assets)),nullptr,files.filePath("stale.json"));
    window.setAttribute(Qt::WA_DontShowOnScreen);window.resize(1200,900);window.show();QApplication::processEvents();
    require(window.selectEntity(area),"stale source area selected");
    child<QTabWidget>(window,"sidebarTabs").setCurrentIndex(1);child<QTabWidget>(window,"libraryPages").setCurrentIndex(1);
    auto& canvas=*dynamic_cast<PlanCanvas*>(&child<QWidget>(window,"measurementPlanCanvas"));canvas.setOverviewMapEnabled(false);canvas.fitView();
    auto source=window.document().snapshot();auto changed=source.entities().at(area.toStdString());changed.extensions["fixture_stale_canvas"]=true;
    window.document().apply(ApplyEntityChanges{source.revision(),{EntityChange::upsert(changed)}, {},"fixture retained canvas"});
    const auto current=window.document().snapshot();
    require(!drop(canvas,pixel(canvas,{1,1}),"room")&&window.document().snapshot().entities()==current.entities()&&window.lastError().contains("drawing changed"),
        "retained canvas refuses a changed revision without classifying its old hit");
    window.document()=Document::create(entities,assets);require(window.selectEntity(area),"refresh replacement create head");
    auto& rows=child<QListWidget>(window,"areaClassTargets");auto* row=areaRow(rows,area);require(row,"replacement area row exists");
    const auto row_point=rows.visualItemRect(row).center();source=window.document().snapshot();sketch::test::DetachedDocumentSnapshotFixture altered(source);
    altered.history().front().entities.at(area.toStdString()).extensions["fixture_same_revision"]=true;
    window.document()=Document::fork(altered);const auto replacement=window.document().snapshot();
    require(replacement.revision()==source.revision()&&replacement.document_id()==source.document_id()&&replacement.entities()!=source.entities(),
        "replacement changes authority while preserving identity and revision");
    require(!drop(*rows.viewport(),row_point,"room")&&window.document().snapshot().entities()==replacement.entities()&&window.lastError().contains("area list changed"),
        "same-identity same-revision replacement refuses retained row input");
    require(!drop(canvas,pixel(canvas,{1,1}),"room")&&window.document().snapshot().entities()==replacement.entities(),
        "same-identity same-revision replacement refuses retained canvas input");
    require(window.selectEntity(area),"refresh same-revision replacement");row=areaRow(rows,area);
    require(row&&drop(*rows.viewport(),rows.visualItemRect(row).center(),"room"),"fresh projection accepts replaced head after refresh");
}
}
int main(int argc,char** argv) {
    testing::noninteractive_errors();
    // Match the established symbol-drag fixture: offscreen's drag backend
    // ignores every drag; minimal is also headless and runs Qt's real drag loop.
    if(qEnvironmentVariable("QT_QPA_PLATFORM")==QStringLiteral("offscreen"))
        qputenv("QT_QPA_PLATFORM","minimal:enable_fonts");
    QApplication app(argc,argv);QTemporaryDir settings;
    QCoreApplication::setOrganizationName("VertexTests");QCoreApplication::setApplicationName("AreaClassPalette");
    QSettings::setDefaultFormat(QSettings::IniFormat);QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,settings.path());
    try {
        require(QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/Inter.ttf"))>=0,"bundled Inter loads for actual palette captures");
        app.setFont(QFont(QStringLiteral("Inter"),10));
        workflow();detected_spaces();appraisal_authority();named_area_types();stale_sources();std::cout<<"area_class_palette_desktop_tests passed\n";return 0;}
    catch(const std::exception& error) {std::cerr<<"area_class_palette_desktop_tests: "<<error.what()<<'\n';return 1;}
}
