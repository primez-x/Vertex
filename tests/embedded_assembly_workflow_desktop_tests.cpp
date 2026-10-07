#include "sketch/desktop/main_window.hpp"
#include "sketch/desktop/assembly_authoring_dialog.hpp"
#include "sketch/assembly_model.hpp"
#include "sketch/assembly_document_adapter.hpp"
#include "sketch/boundary_entity.hpp"
#include "sketch/architecture.hpp"
#include "sketch/visualization/native_geometry_preparation.hpp"
#include "sketch/architectural_schedule.hpp"
#include "support/noninteractive_errors.hpp"
#include "../src/desktop/plan_canvas.hpp"
#include <QApplication>
#include <QComboBox>
#include <QClipboard>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QLineEdit>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QListWidget>
#include <QPushButton>
#include <QSettings>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <algorithm>
#include <cmath>
#include <exception>
#include <functional>
#include <iostream>
#include <stdexcept>

namespace {
using namespace sketch;
using namespace sketch::desktop;
using Json=nlohmann::json;
void require(bool value,const char* message) { if(!value)throw std::runtime_error(message); }
void close(double value,double expected,const char* message) { require(std::abs(value-expected)<1e-6,message); }
template<class T> T& control(QObject& owner,const char* name) {
    for(auto* child:owner.findChildren<QObject*>(QString::fromLatin1(name)))
        if(auto* value=dynamic_cast<T*>(child))return *value;
    throw std::runtime_error(name);
}
Boundary rectangle(double x,double y,double w,double h) {
    return {{{x,y},{x+w,y},0},{{x+w,y},{x+w,y+h},0},{{x+w,y+h},{x,y+h},0},{{x,y+h},{x,y},0}};
}
Json boundary_json(const Boundary& edges) {
    auto result=Json::array(); for(const auto& e:edges)result.push_back({{"start",{e.start.x,e.start.y}},
        {"end",{e.end.x,e.end.y}},{"sweep_radians",e.sweep_radians}}); return result;
}
void display(MainWindow& window) {
    window.setAttribute(Qt::WA_DontShowOnScreen);window.resize(1280,900);window.setMetricUnits(true);window.show();
    QApplication::processEvents();
}
void mouse(PlanCanvas& canvas,QEvent::Type type,QPointF point) {
    QMouseEvent event(type,point,canvas.mapToGlobal(point.toPoint()),
        type==QEvent::MouseMove?Qt::NoButton:Qt::LeftButton,
        type==QEvent::MouseButtonRelease?Qt::NoButton:Qt::LeftButton,Qt::NoModifier);
    QApplication::sendEvent(&canvas,&event);
}
const CanvasEntity& shape(MainWindow& window,const QString& id,const char* canvas="measurementPlanCanvas") {
    const auto& items=control<PlanCanvas>(window,canvas).entities();
    const auto found=std::find_if(items.begin(),items.end(),[&](const auto& e){return e.id==id;});
    require(found!=items.end(),"actual canvas must retain embedded child identity");return *found;
}
void bounds(MainWindow& window,const QString& id,double x,double y,double width,double depth,const char* canvas="measurementPlanCanvas") {
    const auto box=boundary_bounds(shape(window,id,canvas).segments);
    close(box.minimum.x,x,"embedded minimum X comes from profile placement");close(box.minimum.y,y,"embedded minimum Y comes from profile placement");
    close(box.maximum.x-x,width,"embedded width comes from actual profile");close(box.maximum.y-y,depth,"embedded depth comes from actual profile");
}
double volume(MainWindow& window,const std::string& id) {
    const auto projection=window.scheduleSnapshot(); require(projection.diagnostics.empty(),"actual embedded schedules have no diagnostic");
    const auto found=std::find_if(projection.snapshot.rows.begin(),projection.snapshot.rows.end(),[&](const auto& row){return row.object_id==id;});
    require(found!=projection.snapshot.rows.end(),"schedule retains exact embedded child identity");
    return std::get<ScheduleQuantity>(found->cells.at("volume").value).value;
}
void history(MainWindow& window,const DocumentSnapshot& before,const DocumentSnapshot& after) {
    require(window.undoCommand() && window.document().snapshot().entities()==before.entities(),"Undo restores exact embedded source");
    require(window.redoCommand() && window.document().snapshot().entities()==after.entities(),"Redo restores exact embedded source");
}
AssemblyInstance embedded(MainWindow& window,const std::string& id) {
    const auto model=AssemblyModel::from_json(window.document().snapshot().entities().at("embedded-catalog").properties.at("model"));
    const auto found=std::find_if(model.instances().begin(),model.instances().end(),[&](const auto& value){return value.id==id;});
    require(found!=model.instances().end(),"embedded local instance exists");return *found;
}
sketch::visualization::PreparedNativeSolid native(MainWindow& window,const QString& id) {
    auto geometry=sketch::visualization::prepare_native_geometry(window.document().snapshot(),std::nullopt);
    require(geometry && geometry->errors.empty(),"actual edited embedded source prepares native geometry");
    return geometry->solids.at(id.toStdString());
}
void modal(const char* name,const std::function<void()>& open,const std::function<void(QDialog&)>& edit) {
    std::exception_ptr failure;bool seen=false;QTimer timer;timer.setSingleShot(true);
    QObject::connect(&timer,&QTimer::timeout,[&]{
        auto* dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget());
        try {require(dialog && dialog->objectName()==name,"expected actual assembly editor");seen=true;
            dialog->setAttribute(Qt::WA_DontShowOnScreen);edit(*dialog);if(dialog->isVisible())dialog->reject();}
        catch(...) {failure=std::current_exception();if(dialog)dialog->reject();}
    });timer.start(0);open();timer.stop();if(failure)std::rethrow_exception(failure);require(seen,"actual modal action executed");
}
void lifecycle(const QString& directory) {
    MainWindow window({},nullptr,directory+"/library.json");display(window);
    const auto host=window.createSlabFromBoundary(rectangle(0,0,100,80),"0.2 m","0 m");
    const auto wider=window.createBoundary(rectangle(0,0,6,3));
    require(!host.isEmpty()&&!wider.isEmpty(),"real host and type replacement outline created");
    const auto initial=window.document().snapshot();
    auto wide_entity=initial.entities().at(wider.toStdString());wide_entity.properties["name"]="Wider profile";
    wide_entity.properties["holes"]=Json::array({boundary_json(rectangle(1,1,1,1))});
    AssemblyType leaf;leaf.id="leaf";leaf.name="Ring profile";leaf.materials["finish"]="red";
    leaf.profiles.push_back({"ring",rectangle(0,0,4,3),{rectangle(1,1,1,1)},0,0.5,"finish"});
    AssemblyType nested;nested.id="nested";nested.name="Nested profile";nested.properties["name"]="Entrance ring";
    AssemblyPart part;part.id="ring-part";part.type_id=leaf.id;nested.parts={part};
    AssemblyType legacy;legacy.id="legacy";legacy.name="Legacy declaration";
    AssemblyInstance placed;placed.id="placed";placed.type_id=nested.id;
    placed.placement=AssemblyPlacement{host.toStdString(),{10,10},0,2};
    AssemblyInstance root;root.id="root";root.type_id=nested.id;root.root_transform=AssemblyTransform{{20,30,2},0,1};
    AssemblyInstance copy;copy.id="copy";copy.type_id=legacy.id;copy.placement=AssemblyPlacement{host.toStdString(),{-50,-50},0,1};
    Entity catalog{"embedded-catalog","assembly_model",{{"name","Embedded profile catalog"},{"model",AssemblyModel::create(
        {{"red","Red","#ff0000"}},{leaf,nested,legacy},{placed,root,copy}).to_json()}}};
    const auto& host_properties=initial.entities().at(host.toStdString()).properties;
    for(const auto* key:{"property_id","building_id","floor_id","layer_id"})
        if(host_properties.contains(key))catalog.properties[key]=host_properties.at(key);
    window.document().apply(ApplyEntityChanges{initial.revision(),{EntityChange::upsert(wide_entity),EntityChange::upsert(catalog)}, {},"Seed embedded profile lifecycle"});
    require(window.selectEntity(host),"refresh embedded fixture");
    const auto placed_id=QStringLiteral("embedded-catalog:instance:placed"),root_id=QStringLiteral("embedded-catalog:instance:root");
    const auto copy_id=QStringLiteral("embedded-catalog:instance:copy");
    bounds(window,placed_id,10,10,8,6);bounds(window,root_id,20,30,4,3);bounds(window,copy_id,-50,-50,100,80);
    bounds(window,placed_id,10,10,8,6,"architecturalPlanCanvas");bounds(window,root_id,20,30,4,3,"architecturalPlanCanvas");
    require(shape(window,placed_id).holes.size()==1 && shape(window,root_id).holes.size()==1,"holed plan profiles retain actual fill cutouts");
    close(volume(window,placed_id.toStdString()),44,"embedded schedule measures scaled ring volume");
    close(volume(window,root_id.toStdString()),5.5,"standalone embedded schedule measures XYZ root ring volume");
    require(window.selectEntity(root_id) && window.selectedEntityId()==root_id && shape(window,root_id).selected,
        "root without host is selectable using stable derived catalog child identity");
    require(!window.document().snapshot().entities().contains(root_id.toStdString()),"selection never synthesizes a document child entity");
    // Geometric legacy placement is itself selected. Every editor writes the
    // catalog-owned instance, never the differently sized host.
    require(window.selectEntity(placed_id) && window.selectedEntityId()==placed_id,"geometric placement selects actual catalog child before legacy host mapping");
    const auto original_host=window.document().snapshot().entities().at(host.toStdString());
    const auto before_edit=window.document().snapshot();
    modal("embeddedAssemblyPropertiesDialog",[&]{control<QPushButton>(window,"editBuildingObject").click();},[](QDialog& dialog){
        control<QLineEdit>(dialog,"embeddedAssemblyZ").setText("99 m");dialog.reject();
    });
    require(window.document().snapshot().entities()==before_edit.entities() && window.document().revision()==before_edit.revision(),"embedded properties Cancel preserves exact source and history");
    modal("embeddedAssemblyPropertiesDialog",[&]{control<QPushButton>(window,"editBuildingObject").click();},[](QDialog& dialog){
        auto& properties=control<QTableWidget>(dialog,"embeddedAssemblyProperties");properties.item(0,1)->setText("Edited entrance ring");
        control<QDialogButtonBox>(dialog,"embeddedAssemblyButtons").button(QDialogButtonBox::Save)->click();
        require(dialog.result()==QDialog::Accepted,"actual embedded properties editor saves");
    });
    require(embedded(window,"placed").property_overrides.at("name")=="Edited entrance ring" && embedded(window,"placed").placement,
        "property editing retains geometric legacy placement and explicit overrides");
    history(window,before_edit,window.document().snapshot());require(window.undoCommand(),"restore fixture after editor coverage");require(window.selectEntity(placed_id),"reselect embedded geometric placement");
    const auto before_transform=window.document().snapshot();const auto old_native=native(window,placed_id);
    require(window.transformSelectedArchitecturalObject("0","3 m","0 m","2 m","0.5",false),"actual embedded transform controller commits");
    require(window.selectedEntityId()==placed_id && !embedded(window,"placed").placement && embedded(window,"placed").root_transform,
        "transform converts geometric placement to equivalent editable XYZ root with stable child identity");
    require(window.document().snapshot().entities().at(host.toStdString())==original_host,"geometric embedded transform preserves exact host source");
    bounds(window,placed_id,8,5,4,3);close(volume(window,placed_id.toStdString()),5.5,"transformed plan and schedule use the same scaled profile");
    const auto edited_native=native(window,placed_id);require(edited_native.content!=old_native.content,"embedded transform invalidates native cache identity");
    close(solid_volume(edited_native.shape),5.5,"transformed native volume agrees with actual schedule");
    history(window,before_transform,window.document().snapshot());require(window.undoCommand(),"Undo returns exact old geometric placement");
    require(embedded(window,"placed").placement && !embedded(window,"placed").root_transform,"Undo restores legacy placement representation");
    require(window.selectEntity(placed_id),"reselect embedded root for gesture");
    auto& drawing=control<PlanCanvas>(window,"measurementPlanCanvas");drawing.setViewTransform({14,13},32);QApplication::processEvents();
    const auto before_gesture=window.document().snapshot();
    auto handle=drawing.selectionRotationHandlePosition();auto frame=drawing.selectionBounds();require(handle && frame,"geometric embedded child has reachable transform control");
    const auto pivot=frame->center();const QPointF rotated{pivot.x()-(handle->y()-pivot.y()),pivot.y()+(handle->x()-pivot.x())};
    mouse(drawing,QEvent::MouseButtonPress,*handle);mouse(drawing,QEvent::MouseMove,rotated);
    QKeyEvent escape(QEvent::KeyPress,Qt::Key_Escape,Qt::NoModifier);QApplication::sendEvent(&drawing,&escape);
    mouse(drawing,QEvent::MouseButtonRelease,rotated);
    require(window.document().snapshot().entities()==before_gesture.entities() && window.document().revision()==before_gesture.revision(),"cancelled actual embedded transform gesture preserves source");
    require(window.selectEntity(placed_id),"reselect embedded root after cancelled gesture");
    mouse(drawing,QEvent::MouseButtonPress,*handle);mouse(drawing,QEvent::MouseMove,rotated);mouse(drawing,QEvent::MouseButtonRelease,rotated);QApplication::processEvents();
    require(window.document().revision()==before_gesture.revision()+1 && embedded(window,"placed").root_transform &&
        window.document().snapshot().entities().at(host.toStdString())==original_host,"actual embedded rotation gesture commits one catalog operation and preserves host");
    history(window,before_gesture,window.document().snapshot());require(window.undoCommand(),"restore fixture after gesture coverage");require(window.selectEntity(placed_id),"reselect embedded root for clone");
    const auto before_clone=window.document().snapshot();
    require(window.transformSelectedArchitecturalObject("0","15 m","0 m","0 m","1",true),"actual geometric embedded clone commits");
    const auto clone_id=window.selectedEntityId();require(clone_id!=placed_id && clone_id.startsWith("embedded-catalog:instance:"),"clone retains catalog ownership and fresh stable local identity");
    bounds(window,clone_id,25,10,8,6);close(volume(window,clone_id.toStdString()),44,"cloned schedule retains actual profile volume");
    const auto after_clone=window.document().snapshot();history(window,before_clone,after_clone);
    require(window.selectEntity(clone_id) && window.deleteSelection(),"Delete removes actual embedded clone through typed catalog command");
    const auto after_delete=window.document().snapshot();require(after_delete.entities().at(host.toStdString())==original_host && embedded(window,"placed").placement,"Delete preserves host and unrelated original instance");
    history(window,after_clone,after_delete);require(window.undoCommand() && window.undoCommand(),"restore fixture after clone/delete coverage");
    require(window.selectEntity(placed_id),"select embedded geometric root for Copy");
    const auto before_copy=window.document().snapshot();require(window.copySelection(),"Copy flattens a detached geometric catalog child into typed independent clipboard source");
    require(window.document().snapshot().entities()==before_copy.entities() && window.document().revision()==before_copy.revision(),"Copy preserves exact active catalog, host placement and source IDs");
    const auto payload=Json::parse(QApplication::clipboard()->text().toStdString());
    const auto copied_root=payload.at("root_id").get<std::string>();bool typed=false;
    for(const auto& item:payload.at("entities"))if(item.at("id")==copied_root)typed=item.at("type")=="assembly_instance";
    require(typed,"clipboard root is the existing typed external assembly envelope");
    require(window.pasteSelection(),"actual Paste admits detached assembly with catalog closure");
    const auto pasted=window.selectedEntityId();require(window.document().snapshot().entities().at(pasted.toStdString()).type=="assembly_instance","Paste publishes actual independent root");
    require(window.document().snapshot().entities().at("embedded-catalog")==before_copy.entities().at("embedded-catalog"),"Paste preserves original source catalog exactly");
    history(window,before_copy,window.document().snapshot());require(window.undoCommand(),"restore fixture after clipboard lifecycle coverage");
    require(window.selectEntity(placed_id) && window.cutSelection(),"Cut uses detached clipboard copy and typed embedded catalog deletion");
    const auto after_cut=window.document().snapshot();require(after_cut.entities().at(host.toStdString())==original_host,"embedded Cut preserves host source");
    history(window,before_copy,after_cut);require(window.undoCommand(),"restore exact fixture after Cut");
    require(window.selectEntity(root_id),"restore root selection for type lifecycle");
    const auto before=window.document().snapshot();
    // Exercise the production type editor against a type referenced only by
    // catalog-owned instances, including a nested root and legacy placement.
    modal("assemblyWorkspaceDialog",[&]{window.showAssemblies();},[&](QDialog& hub){
        auto& list=control<QListWidget>(hub,"assemblyAuthoredTypeList");bool selected=false;
        for(int i=0;i<list.count();++i) {
            const auto key=Json::parse(list.item(i)->data(Qt::UserRole).toString().toStdString());
            if(key.at(0)==catalog.id && key.at(1)==leaf.id){list.setCurrentRow(i);selected=true;break;}
        }
        require(selected,"actual reusable type hub exposes embedded referenced leaf");
        modal("assemblyTypeAuthoringDialog",[&]{control<QPushButton>(hub,"editGeometricAssemblyType").click();},[&](QDialog& dialog){
            auto& profiles=control<QTableWidget>(dialog,"assemblyProfiles");
            auto* outline=qobject_cast<QComboBox*>(profiles.cellWidget(0,0));require(outline,"actual outline choice");
            const auto index=outline->findData(wider);require(index>=0,"replacement captured boundary offered");outline->setCurrentIndex(index);
            auto* height=qobject_cast<QLineEdit*>(profiles.cellWidget(0,1));require(height,"actual profile height field");height->setText("0.75 m");
            control<QPushButton>(dialog,"assemblyPreviewTypeUpdate").click();
            require(control<QTableWidget>(dialog,"assemblyUpdateImpacts").rowCount()==2,"both embedded transitive roots appear in type impact preview");
            control<QDialogButtonBox>(dialog,"assemblyAuthoringButtons").button(QDialogButtonBox::Save)->click();
            require(dialog.result()==QDialog::Accepted,"actual type update saves embedded profile change");
        });
    });
    const auto after=window.document().snapshot();require(after.revision()==before.revision()+1,"embedded type update uses one atomic revision");
    history(window,before,after);
    bounds(window,placed_id,10,10,12,6);bounds(window,root_id,20,30,6,3);bounds(window,copy_id,-50,-50,100,80);
    close(volume(window,placed_id.toStdString()),102,"embedded type edit updates actual scaled schedule volume");
    close(volume(window,root_id.toStdString()),12.75,"embedded type edit updates actual root schedule volume");
    const auto before_recolor=window.document().snapshot();
    auto colored=before_recolor.entities().at(catalog.id);auto model=AssemblyModel::from_json(colored.properties.at("model"));
    auto materials=model.materials();materials.front().color_srgb="#00ff00";
    colored.properties["model"]=AssemblyModel::create(materials,model.types(),model.instances()).to_json();
    window.document().apply(ApplyEntityChanges{before_recolor.revision(),{EntityChange::upsert(colored)}, {},"Recolor embedded profile"});
    require(window.selectEntity(root_id),"refresh embedded profile recolor");
    require(shape(window,placed_id).fill_color==QColor("#00ff00") && shape(window,root_id).fill_color==QColor("#00ff00"),
        "material edits refresh actual per-profile canvas appearance");
    history(window,before_recolor,window.document().snapshot());
    const auto path=directory+"/embedded-assembly.sketch";require(window.saveProjectAs(path),"embedded lifecycle saves");
    const auto saved=window.document().snapshot();MainWindow reopened({},nullptr,directory+"/reopened-library.json");display(reopened);
    require(reopened.openProject(path),"embedded lifecycle reopens");
    require(reopened.document().snapshot().entities()==saved.entities() && reopened.scheduleSnapshot().snapshot.rows==window.scheduleSnapshot().snapshot.rows,
        "Save/Reopen retains exact embedded graph and derived schedules");
    bounds(reopened,placed_id,10,10,12,6);bounds(reopened,root_id,20,30,6,3);
    require(reopened.selectEntity(root_id) && reopened.selectedEntityId()==root_id,"reopened embedded root retains selectable stable identity");
}

void frame_owner_collision(const QString& directory) {
    MainWindow window({},nullptr,directory+"/frame-library.json");display(window);
    const auto ordinary=window.createBoundary(rectangle(-20,-10,7,2));
    require(!ordinary.isEmpty(),"ordinary frame fixture created");
    const std::string child="frame-catalog:instance:root";
    IdentifiedBoundary shadow{child,"measurement_boundary",{}};
    const auto edges=rectangle(-50,-50,17,9);
    for(std::size_t i=0;i<edges.size();++i)
        shadow.segments.push_back({child+":s"+std::to_string(i),child+":v"+std::to_string(i),
            child+":v"+std::to_string((i+1)%edges.size()),edges[i]});
    AssemblyType type;type.id="profile";type.name="Frame profile";
    type.profiles.push_back({"outer",rectangle(0,0,4,3),{},0,0.5,{}});
    AssemblyInstance root;root.id="root";root.type_id=type.id;
    root.root_transform=AssemblyTransform{{20,30,0},0.5,1};
    Entity catalog{"frame-catalog","assembly_model",{{"model",AssemblyModel::create({}, {type},{root}).to_json()}}};
    const auto source=window.document().snapshot();
    window.document().apply(ApplyEntityChanges{source.revision(),
        {EntityChange::upsert(catalog),EntityChange::upsert(encode_identified_boundary_entity(shadow))},
        {},"Frame owner collision fixture"});
    require(window.selectEntity(ordinary),"refresh frame owner collision fixture");
    const auto captured=window.document().snapshot();
    int collision_entries=0;
    for(const auto& entry:control<PlanCanvas>(window,"measurementPlanCanvas").entities()) {
        if(entry.id!=QString::fromStdString(child))continue;
        ++collision_entries;
        require(entry.resize_frame.has_value(),"colliding geometric owner retains frame");
        close(entry.resize_frame->rotation_radians,0.5,"embedded frame retains priority over persisted same-ID boundary");
        close(entry.resize_frame->width_metres,4,"embedded frame width uses captured profile");
        close(entry.resize_frame->depth_metres,3,"embedded frame depth uses captured profile");
    }
    require(collision_entries>=1,"colliding owner remains present on canvas");
    require(shape(window,ordinary).resize_frame.has_value(),"ordinary measured boundary retains frame");
    close(shape(window,ordinary).resize_frame->width_metres,7,"ordinary frame uses its own geometry");
    require(window.document().snapshot().entities()==captured.entities() && window.document().revision()==captured.revision(),
        "frame preparation does not edit source or history");
}
}
int main(int argc,char** argv) {
    sketch::testing::noninteractive_errors();QApplication application(argc,argv);QTemporaryDir settings;
    QCoreApplication::setOrganizationName("VertexTests");QCoreApplication::setApplicationName("EmbeddedAssemblyWorkflowDesktop");
    QSettings::setDefaultFormat(QSettings::IniFormat);QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,settings.path());
    try {
        require(QFontDatabase::addApplicationFont(":/fonts/Inter.ttf")>=0,"bundled test font loads");
        QTemporaryDir directory;require(directory.isValid(),"isolated fixture output");
        frame_owner_collision(directory.path());lifecycle(directory.path());
        std::cout<<"embedded_assembly_workflow_desktop_tests passed\n";return 0;
    } catch(const std::exception& error){std::cerr<<"embedded_assembly_workflow_desktop_tests: "<<error.what()<<'\n';return 1;}
}
