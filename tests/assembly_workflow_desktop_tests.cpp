#include "sketch/desktop/main_window.hpp"
#include "sketch/desktop/assembly_authoring_dialog.hpp"
#include "sketch/assembly_document_adapter.hpp"
#include "sketch/architectural_schedule.hpp"
#include "sketch/boundary_entity.hpp"
#include "sketch/building_entity.hpp"
#include "sketch/building_view_projection.hpp"
#include "sketch/roof_join_semantics.hpp"
#include "sketch/architecture.hpp"
#include "support/noninteractive_errors.hpp"
#include "../src/desktop/plan_canvas.hpp"

#include <BRepAlgoAPI_Common.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFontDatabase>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSettings>
#include <QTableWidget>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <algorithm>
#include <cmath>
#include <exception>
#include <functional>
#include <iostream>
#include <numbers>
#include <stdexcept>

namespace {
using namespace sketch;
using namespace sketch::desktop;
using Json = nlohmann::json;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void check_close(double value, double expected, const char* message) { require(std::abs(value-expected)<1e-7,message); }
template<class T> T& control(QObject& owner, const char* name) {
    // PlanCanvas intentionally has no Q_OBJECT. Search QObject descendants
    // and use RTTI; this also finds the unnamed button box past layout objects.
    for(auto* child:owner.findChildren<QObject*>(QString::fromLatin1(name)))
        if(auto* value=dynamic_cast<T*>(child))return *value;
    throw std::runtime_error(name);
}
void click(QObject& owner,const char* name) { control<QPushButton>(owner,name).click(); }
void text(QObject& owner,const char* name,const char* value) { control<QLineEdit>(owner,name).setText(QString::fromUtf8(value)); }
void choice(QComboBox& box,const QString& value) {
    const auto index=box.findData(value); require(index>=0,"typed choice must exist"); box.setCurrentIndex(index);
}
void capture(QWidget& owner,const QString& name) {
    const auto dir=qEnvironmentVariable("VERTEX_TEST_CAPTURE_DIR"); if(dir.isEmpty())return;
    QApplication::processEvents();require(QDir().mkpath(dir) && owner.grab().save(QDir(dir).filePath(name)),"capture must save");
}
// Run the actual modal action, including nested child editors. Exceptions close
// the current modal so a failed assertion cannot leave a blocking native loop.
void modal(const char* name,const std::function<void()>& open,const std::function<void(QDialog&)>& edit) {
    std::exception_ptr failure; bool seen=false; QTimer timer; timer.setSingleShot(true);
    QObject::connect(&timer,&QTimer::timeout,[&] {
        auto* dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget());
        try {
            require(dialog && dialog->objectName()==name,"expected actual modal editor");seen=true;
            dialog->setAttribute(Qt::WA_DontShowOnScreen);edit(*dialog);
            if(dialog->isVisible())dialog->reject();
        } catch(...) {failure=std::current_exception();if(dialog)dialog->reject();}
    });
    timer.start(0);open();timer.stop();if(failure)std::rethrow_exception(failure);require(seen,"modal widget action must execute");
}
void hub(MainWindow& window,const std::function<void(QDialog&)>& edit) {
    modal("assemblyWorkspaceDialog",[&]{window.showAssemblies();},edit);
}
void save(QDialog& dialog) {
    control<QDialogButtonBox>(dialog,"assemblyAuthoringButtons").button(QDialogButtonBox::Save)->click();
    require(dialog.result()==QDialog::Accepted,"typed assembly editor must accept Save");
}
PlanCanvas& canvas(MainWindow& window,const char* name="measurementPlanCanvas") { return control<PlanCanvas>(window,name); }
void display(MainWindow& window) {
    window.setAttribute(Qt::WA_DontShowOnScreen);window.resize(1280,900);window.setMetricUnits(true);window.show();
    QApplication::processEvents();canvas(window).setOverviewMapEnabled(false);canvas(window,"architecturalPlanCanvas").setOverviewMapEnabled(false);
}
Boundary rectangle(double x,double y,double width,double depth) {
    return {{{x,y},{x+width,y},0},{{x+width,y},{x+width,y+depth},0},
        {{x+width,y+depth},{x,y+depth},0},{{x,y+depth},{x,y},0}};
}
Json boundary_json(const Boundary& value) {
    Json result=Json::array();for(const auto& edge:value)result.push_back({{"start",{edge.start.x,edge.start.y}},
        {"end",{edge.end.x,edge.end.y}},{"sweep_radians",edge.sweep_radians}});return result;
}
AssemblyModel model(MainWindow& window,const std::string& catalog="fixture-catalog") {
    return AssemblyModel::from_json(window.document().snapshot().entities().at(catalog).properties.at("model"));
}
AssemblyType type(MainWindow& window,const std::string& name) {
    const auto catalog=model(window);const auto found=std::find_if(catalog.types().begin(),catalog.types().end(),[&](const auto& t){return t.name==name;});
    require(found!=catalog.types().end(),"authored type must exist");return *found;
}
void select_type(QDialog& hub,const std::string& id) {
    auto& list=control<QListWidget>(hub,"assemblyAuthoredTypeList");
    for(int i=0;i<list.count();++i) {
        const auto key=Json::parse(list.item(i)->data(Qt::UserRole).toString().toStdString());
        if(key.at(0)=="fixture-catalog" && key.at(1)==id){list.setCurrentRow(i);return;}
    }
    throw std::runtime_error("typed hub row must retain stable type identity");
}
void select_catalog_type(QDialog& hub,const std::string& catalog,const std::string& id) {
    auto& list=control<QListWidget>(hub,"assemblyAuthoredTypeList");
    for(int i=0;i<list.count();++i) {
        const auto key=Json::parse(list.item(i)->data(Qt::UserRole).toString().toStdString());
        if(key.at(0)==catalog && key.at(1)==id){list.setCurrentRow(i);return;}
    }
    throw std::runtime_error("hub must distinguish equal type IDs in independent catalogs");
}
void named(QObject& dialog,const char* prefix,const char* kind,const char* key,const char* value) {
    const auto table_name=std::string(prefix)+kind;click(dialog,(table_name+"Add").c_str());
    auto& rows=control<QTableWidget>(dialog,table_name.c_str());const auto row=rows.rowCount()-1;
    rows.item(row,0)->setText(QString::fromUtf8(key));
    if(std::string(kind)=="Materials")choice(*qobject_cast<QComboBox*>(rows.cellWidget(row,1)),QString::fromUtf8(value));
    else rows.item(row,1)->setText(QString::fromUtf8(value));
}
void cell_text(QTableWidget& rows,int row,int column,const char* value) {
    auto* field=qobject_cast<QLineEdit*>(rows.cellWidget(row,column));require(field,"typed numeric profile/part field");field->setText(QString::fromUtf8(value));
}
const CanvasEntity& shape(PlanCanvas& drawing,const QString& id) {
    const auto found=std::find_if(drawing.entities().begin(),drawing.entities().end(),[&](const auto& e){return e.id==id;});
    require(found!=drawing.entities().end(),"actual canvas must publish semantic root");return *found;
}
const ScheduleRow& row(const DocumentScheduleProjection& projection,const std::string& id) {
    const auto found=std::find_if(projection.snapshot.rows.begin(),projection.snapshot.rows.end(),[&](const auto& r){return r.object_id==id;});
    require(found!=projection.snapshot.rows.end(),"actual schedule must contain source row");return *found;
}
double quantity(const ScheduleRow& source,const char* key) { return std::get<ScheduleQuantity>(source.cells.at(key).value).value; }
double summary_total(const DocumentScheduleProjection& projection) {
    double result=0;for(const auto& source:projection.snapshot.rows)if(source.kind==ScheduleRowKind::material_summary)result+=quantity(source,"volume");return result;
}
AssemblyDocumentInstance instance(MainWindow& window,const QString& id) {
    return decode_document_assembly_instance(window.document().snapshot().entities().at(id.toStdString()));
}
void history(MainWindow& window,const DocumentSnapshot& before,const DocumentSnapshot& after) {
    require(window.undoCommand() && window.document().snapshot().entities()==before.entities(),"Undo restores exact lifecycle source");
    require(window.redoCommand() && window.document().snapshot().entities()==after.entities(),"Redo restores exact lifecycle result");
}
struct Authored { QString root; std::string leaf_type,root_type,part; };
void empty_hub_cancellation(MainWindow& window) {
    const auto before=window.document().snapshot();
    hub(window,[](QDialog& dialog){
        require(control<QListWidget>(dialog,"assemblyAuthoredTypeList").count()==0,"new project assembly hub starts empty");
    });
    require(window.document().snapshot().entities()==before.entities() && window.document().snapshot().assets()==before.assets() && window.document().revision()==before.revision(),"pure hub browsing and Close preserve empty catalog/history");
    hub(window,[&](QDialog& workspace){
        modal("assemblyCatalogDialog",[&]{click(workspace,"assemblyCatalogMetadata");},[](QDialog& dialog){
            require(control<QListWidget>(dialog,"assemblyMaterialList").count()==0,"detached empty material catalog starts empty");
            text(dialog,"assemblyMaterialId","cancelled");text(dialog,"assemblyMaterialName","Unapplied material");
            click(dialog,"closeAssemblyCatalog");
        });
    });
    require(window.document().snapshot().entities()==before.entities() && window.document().snapshot().assets()==before.assets() && window.document().revision()==before.revision(),"empty materials browsing and Close leave source and history exact");
    hub(window,[&](QDialog& workspace){
        modal("assemblyTypeAuthoringDialog",[&]{click(workspace,"createGeometricAssemblyType");},[](QDialog& dialog){
            text(dialog,"assemblyTypeName","Cancelled first type");dialog.reject();
        });
    });
    require(window.document().snapshot().entities()==before.entities() && window.document().snapshot().assets()==before.assets() && window.document().revision()==before.revision(),"first type-editor Cancel leaves no empty catalog or history revision");
}
void first_material_apply(const QString& directory) {
    MainWindow window({},nullptr,QDir(directory).filePath("first-material-library.json"));display(window);
    const auto before=window.document().snapshot();
    hub(window,[&](QDialog& workspace){
        modal("assemblyCatalogDialog",[&]{click(workspace,"assemblyCatalogMetadata");},[&](QDialog& dialog){
            text(dialog,"assemblyMaterialId","first");click(dialog,"saveAssemblyMaterial");
            require(window.document().revision()==before.revision() && window.document().snapshot().entities()==before.entities(),"invalid first material cannot commit an empty catalog");
            text(dialog,"assemblyMaterialName","First material");text(dialog,"assemblyMaterialColor","#123456");
            click(dialog,"saveAssemblyMaterial");
            require(window.document().revision()==before.revision()+1,"first material and catalog are one atomic Apply");
            click(dialog,"saveAssemblyMaterial");
            require(window.document().revision()==before.revision()+1,"unchanged material Apply adds no history revision");
            click(dialog,"closeAssemblyCatalog");
        });
    });
    const auto after=window.document().snapshot();std::size_t catalogs=0;
    for(const auto& [id,entity]:after.entities())if(entity.type=="assembly_model") {
        (void)id;++catalogs;const auto catalog=AssemblyModel::from_json(entity.properties.at("model"));
        require(catalog.materials().size()==1 && catalog.materials().front().id=="first" && catalog.materials().front().color_srgb==std::optional<std::string>{"#123456"},"Close retains first applied material and its color");
    }
    require(catalogs==1 && after.revision()==before.revision()+1,"first Apply creates exactly one populated catalog");history(window,before,after);
}
void untouched_assembly_saves(MainWindow& window,const Authored& authored) {
    const auto before=window.document().snapshot();
    hub(window,[&](QDialog& workspace){
        select_type(workspace,authored.leaf_type);
        modal("assemblyTypeAuthoringDialog",[&]{click(workspace,"editGeometricAssemblyType");},[](QDialog& dialog){save(dialog);});
    });
    require(window.document().snapshot().entities()==before.entities() && window.document().snapshot().assets()==before.assets() && window.document().revision()==before.revision(),"untouched type accepts first Save without a catalog or history change");
    hub(window,[&](QDialog& workspace){
        auto& roots=control<QListWidget>(workspace,"independentAssemblyList");
        for(int i=0;i<roots.count();++i)if(roots.item(i)->data(Qt::UserRole).toString()==authored.root)roots.setCurrentRow(i);
        modal("assemblyInstanceAuthoringDialog",[&]{click(workspace,"editIndependentAssembly");},[](QDialog& dialog){save(dialog);});
    });
    require(window.document().snapshot().entities()==before.entities() && window.document().snapshot().assets()==before.assets() && window.document().revision()==before.revision(),"untouched placed instance Save preserves the exact graph and history");
}
Authored author(MainWindow& window) {
    const auto outer=window.createBoundary(rectangle(0,0,4,3));const auto cap=window.createBoundary(rectangle(0,0,1,1));
    const auto cutout=window.createBoundary(rectangle(1,1,1,1));require(!outer.isEmpty()&&!cap.isEmpty()&&!cutout.isEmpty(),"real outline geometry created");
    auto source=window.document().snapshot();auto ring=source.entities().at(outer.toStdString());ring.properties["holes"]=Json::array({boundary_json(rectangle(1,1,1,1))});ring.properties["name"]="Ring with opening";
    AssemblyType unused{"unused","Unrelated unused type"};
    Entity catalog{"fixture-catalog","assembly_model",{{"name","Lifecycle materials"},{"model",AssemblyModel::create(
        {{"wood","Timber","#A67842"},{"steel","Steel","#557799"},{"red","Red roof","#C54444"},{"blue","Blue roof","#4466BB"}}, {unused},{}).to_json()}}};
    window.document().apply(ApplyEntityChanges{source.revision(),{EntityChange::upsert(ring),EntityChange::upsert(catalog)}, {},"Seed real cutout and material choices"});
    require(window.selectEntity(outer),"refresh authored outlines");
    std::string first_profile;
    hub(window,[&](QDialog& workspace) {
        modal("assemblyTypeAuthoringDialog",[&]{click(workspace,"createGeometricAssemblyType");},[&](QDialog& dialog) {
            text(dialog,"assemblyTypeName","Ring module");named(dialog,"assemblyType","Properties","finish","natural");
            named(dialog,"assemblyType","Materials","core","wood");named(dialog,"assemblyType","Quantities","volume","99");
            qobject_cast<QComboBox*>(control<QTableWidget>(dialog,"assemblyTypeQuantities").cellWidget(0,2))->setCurrentIndex(static_cast<int>(AssemblyQuantityUnit::cubic_metre));
            named(dialog,"assemblyType","Quantities","pieces","2");
            click(dialog,"assemblyAddProfile");auto& profiles=control<QTableWidget>(dialog,"assemblyProfiles");
            choice(*qobject_cast<QComboBox*>(profiles.cellWidget(0,0)),outer);cell_text(profiles,0,1,"1 m");cell_text(profiles,0,3,"core");
            // The profile imports the actual boundary's authored hole. Inspect
            // its cutout editor rather than injecting an assembly-only void.
            profiles.setCurrentCell(0,0);modal("assemblyProfileCutoutsDialog",[&]{click(dialog,"assemblyProfileCutouts");},[](QDialog& child){
                require(control<QTableWidget>(child,"assemblyProfileCutoutRows").rowCount()==1,"imported real opening is visible in cutout editor");save(child);
            });
            click(dialog,"assemblyAddProfile");choice(*qobject_cast<QComboBox*>(profiles.cellWidget(1,0)),cap);
            cell_text(profiles,1,1,"0.5 m");cell_text(profiles,1,2,"2 m");cell_text(profiles,1,3,"core");
            first_profile=qobject_cast<QComboBox*>(profiles.cellWidget(1,0))->property("stableId").toString().toStdString();
            profiles.setCurrentCell(1,0);click(dialog,"assemblyProfileUp");save(dialog);
        });
    });
    const auto leaf=type(window,"Ring module");require(leaf.profiles.front().id==first_profile && leaf.profiles.back().holes.size()==1,"profile reorder retains stable IDs and opening");
    std::string first_part;
    hub(window,[&](QDialog& workspace) {
        modal("assemblyTypeAuthoringDialog",[&]{click(workspace,"createGeometricAssemblyType");},[&](QDialog& dialog) {
            text(dialog,"assemblyTypeName","Nested canopy");named(dialog,"assemblyType","Properties","label","Canopy");
            click(dialog,"assemblyAddPart");click(dialog,"assemblyAddPart");auto& parts=control<QTableWidget>(dialog,"assemblyParts");
            for(int i=0;i<2;++i)choice(*qobject_cast<QComboBox*>(parts.cellWidget(i,0)),QString::fromStdString(leaf.id));
            cell_text(parts,0,1,"6 m");cell_text(parts,0,5,"2");cell_text(parts,1,3,"0.1 m");cell_text(parts,1,4,"15");
            first_part=qobject_cast<QComboBox*>(parts.cellWidget(1,0))->property("stableId").toString().toStdString();parts.setCurrentCell(1,0);click(dialog,"assemblyPartUp");save(dialog);
        });
    });
    const auto root=type(window,"Nested canopy");require(root.parts.front().id==first_part && root.parts.size()==2,"nested reorder preserves part identities");
    const auto before=window.document().snapshot();
    hub(window,[&](QDialog& workspace) {
        modal("assemblyInstanceAuthoringDialog",[&]{click(workspace,"placeIndependentAssembly");},[&](QDialog& dialog) {
            choice(control<QComboBox>(dialog,"assemblyInstanceType"),QString::fromStdString(root.id));text(dialog,"assemblyInstanceName","Entrance canopy");
            text(dialog,"assemblyRootX","10 m");text(dialog,"assemblyRootY","20 m");text(dialog,"assemblyRootZ","0.2 m");text(dialog,"assemblyRootYaw","30");text(dialog,"assemblyRootScale","1.25");
            named(dialog,"assemblyRoot","Properties","label","Canopy");
            auto& nested=control<QComboBox>(dialog,"assemblyNestedPart");require(nested.count()==2,"both typed nested choices exposed");nested.setCurrentIndex(0);
            named(dialog,"assemblyNested","Properties","finish","natural");named(dialog,"assemblyNested","Materials","core","steel");named(dialog,"assemblyNested","Quantities","pieces","2");
            capture(dialog,"assembly-placement-editor.png");save(dialog);
        });
    });
    const auto selected=window.selectedEntityId();require(!selected.isEmpty(),"placed root becomes actual selection");
    const auto after=window.document().snapshot();require(after.revision()==before.revision()+1,"placement is one atomic revision");history(window,before,after);
    const auto placed=instance(window,selected);require(placed.instance.root_transform && placed.instance.nested_overrides.size()==1,"independent placement and explicit nested values persist");
    check_close(placed.instance.root_transform->translation_m.z,.2,"authored Z placement");check_close(placed.instance.root_transform->rotation_radians,std::numbers::pi/6,"authored yaw");
    require(placed.instance.property_overrides.at("label")=="Canopy" && placed.instance.nested_overrides.front().property_overrides.at("finish")=="natural","equal overrides retain explicit provenance");
    const auto schedules=window.scheduleSnapshot();require(schedules.diagnostics.empty(),"actual assembly schedules resolve completely");
    check_close(quantity(row(schedules,selected.toStdString()),"volume"),103.5*std::pow(1.25,3),"measured cutout volume follows transforms");
    check_close(quantity(row(schedules,selected.toStdString()),"quantity:volume"),198,"declared volume remains independent of scaled geometry");
    check_close(summary_total(schedules),103.5*std::pow(1.25,3),"actual material takeoff conserves all nested profile volumes");
    return {selected,leaf.id,root.id,first_part};
}
void instance_editor(MainWindow& window,const Authored& authored) {
    const auto before=window.document().snapshot();const auto original=instance(window,authored.root);
    hub(window,[&](QDialog& workspace){
        auto& roots=control<QListWidget>(workspace,"independentAssemblyList");
        require(roots.count()==1,"placed hub exposes one independent root");roots.setCurrentRow(0);
        modal("assemblyInstanceAuthoringDialog",[&]{click(workspace,"editIndependentAssembly");},[&](QDialog& dialog){
            text(dialog,"assemblyInstanceName","Edited entrance canopy");text(dialog,"assemblyRootScale","0");
            auto* typed=dynamic_cast<AssemblyInstanceDialog*>(&dialog);require(typed && !typed->submit() && !typed->lastError().isEmpty(),"invalid instance scale refuses inline");
            require(window.document().snapshot().entities()==before.entities(),"invalid instance edit never writes partial source");
            text(dialog,"assemblyRootScale","1.25");save(dialog);
        });
    });
    const auto after=window.document().snapshot();require(after.revision()==before.revision()+1 && instance(window,authored.root)==original && after.entities().at(authored.root.toStdString()).properties.at("name")=="Edited entrance canopy","actual instance edit changes name atomically and retains exact independent placement and equal overrides");
    history(window,before,after);
}
void selected_frame(MainWindow& window,const QString& root,double yaw) {
    require(window.selectEntity(root),"select independent root");auto& drawing=canvas(window);drawing.fitView();QApplication::processEvents();
    const auto& projected=shape(drawing,root);require(projected.selected && !projected.segments.empty() && projected.resize_frame,"selected root has actual outline and persistent frame");
    check_close(projected.resize_frame->rotation_radians,yaw,"frame follows retained root yaw");
    require(drawing.selectionBounds() && drawing.selectionRotationHandlePosition(),"visible frame and rotation control are reachable");
    const auto selected=drawing.grab().toImage();require(window.selectEntity(root,true),"toggle selected root off");
    QApplication::processEvents();const auto unselected=drawing.grab().toImage();int changed=0;
    require(selected.size()==unselected.size(),"selection render comparison uses one stable viewport");
    for(int y=0;y<selected.height();++y)for(int x=0;x<selected.width();++x)if(selected.pixel(x,y)!=unselected.pixel(x,y))++changed;
    require(changed>50,"selection outline and controls actually paint visible pixels");require(window.selectEntity(root),"reselect persistent root frame");
    check_close(shape(drawing,root).resize_frame->rotation_radians,yaw,"reselection preserves frame rotation");
    capture(window,"assembly-lifecycle-window.png");
}
void catalog_metadata_lifecycle(MainWindow& window,const Authored& authored,const QString& pasted) {
    const auto pasted_catalog=instance(window,pasted).assembly_catalog_id;
    for(const auto& target:std::vector<std::string>{"fixture-catalog",pasted_catalog}) {
        const auto before=window.document().snapshot();
        const auto other=target=="fixture-catalog" ? pasted_catalog : std::string{"fixture-catalog"};
        hub(window,[&](QDialog& workspace){
            select_catalog_type(workspace,target,authored.leaf_type);
            modal("assemblyCatalogDialog",[&]{click(workspace,"assemblyCatalogMetadata");},[&](QDialog& dialog){
                require(control<QListWidget>(dialog,"assemblyMaterialList").count()==static_cast<int>(model(window,target).materials().size()),"materials editor opens the selected catalog");
                text(dialog,"assemblyMaterialId","wood");text(dialog,"assemblyMaterialName","Reviewed timber");text(dialog,"assemblyMaterialColor","#2468AC");
                click(dialog,"saveAssemblyMaterial");
                require(window.document().revision()==before.revision()+1,"material name and color update is one Apply");
                const auto materials=model(window,target).materials();const auto timber=std::find_if(materials.begin(),materials.end(),[](const auto& m){return m.id=="wood";});
                require(timber!=materials.end() && timber->name=="Reviewed timber" && timber->color_srgb==std::optional<std::string>{"#2468AC"},"selected catalog retains authored material color and name");
                auto& types=control<QListWidget>(dialog,"assemblyTypeList");
                for(int i=0;i<types.count();++i)if(types.item(i)->data(Qt::UserRole).toString().toStdString()==authored.leaf_type)types.setCurrentRow(i);
                text(dialog,"assemblyTypeName","Reviewed leaf name");click(dialog,"renameAssemblyType");
                require(window.document().revision()==before.revision()+2,"selected catalog type rename is one Apply");
                const auto renamed=model(window,target);const auto leaf=std::find_if(renamed.types().begin(),renamed.types().end(),[&](const auto& t){return t.id==authored.leaf_type;});
                require(leaf!=renamed.types().end() && leaf->name=="Reviewed leaf name","rename targets the selected equal type ID");
                text(dialog,"assemblyTypeId","metadata-added");text(dialog,"assemblyTypeName","Metadata added type");click(dialog,"addAssemblyType");
                require(window.document().revision()==before.revision()+3 && model(window,target).types().size()==renamed.types().size()+1,"new metadata type belongs to selected catalog");
                text(dialog,"assemblyMaterialId","metadata-added-material");text(dialog,"assemblyMaterialName","Added catalog material");text(dialog,"assemblyMaterialColor","#ABCDEF");click(dialog,"saveAssemblyMaterial");
                const auto added=model(window,target).materials();const auto material=std::find_if(added.begin(),added.end(),[](const auto& m){return m.id=="metadata-added-material";});
                require(window.document().revision()==before.revision()+4 && material!=added.end() && material->color_srgb==std::optional<std::string>{"#ABCDEF"},"new material and color belong to the selected catalog");
                require(window.document().snapshot().entities().at(other)==before.entities().at(other),"material and type edits preserve the other independent catalog exactly");
                click(dialog,"closeAssemblyCatalog");
            });
        });
        require(window.document().revision()==before.revision()+4,"Close retains all four explicit catalog Applies");
        for(int i=0;i<4;++i)require(window.undoCommand(),"catalog operations remain individually undoable");
        require(window.document().snapshot().entities()==before.entities(),"Undo restores both independent catalogs and placed roots exactly");
        hub(window,[&](QDialog& workspace){
            select_catalog_type(workspace,target,authored.leaf_type);
            modal("assemblyInstanceAuthoringDialog",[&]{click(workspace,"placeIndependentAssembly");},[&](QDialog& dialog){
                require(control<QComboBox>(dialog,"assemblyInstanceCatalog").currentData().toString().toStdString()==target,"Place starts with the selected type's catalog");dialog.reject();
            });
            modal("assemblyTypeAuthoringDialog",[&]{click(workspace,"createGeometricAssemblyType");},[](QDialog& dialog){
                text(dialog,"assemblyTypeName","Selected catalog new type");click(dialog,"assemblyAddProfile");save(dialog);
            });
        });
        require(window.document().snapshot().entities().at(other)==before.entities().at(other) && model(window,target).types().size()==AssemblyModel::from_json(before.entities().at(target).properties.at("model")).types().size()+1,"Create type publishes only into selected catalog");
        require(window.undoCommand() && window.document().snapshot().entities()==before.entities(),"selected catalog creation remains one reversible command");
    }
    // With the Placed tab active, the chosen root takes precedence over a type
    // row that happens to retain selection in the other tab.
    hub(window,[&](QDialog& workspace){
        select_type(workspace,authored.leaf_type);auto& roots=control<QListWidget>(workspace,"independentAssemblyList");
        for(int i=0;i<roots.count();++i)if(roots.item(i)->data(Qt::UserRole).toString()==pasted)roots.setCurrentRow(i);
        control<QTabWidget>(workspace,"").setCurrentIndex(1);
        modal("assemblyCatalogDialog",[&]{click(workspace,"assemblyCatalogMetadata");},[&](QDialog& dialog){
            require(control<QListWidget>(dialog,"assemblyMaterialList").count()==static_cast<int>(model(window,pasted_catalog).materials().size()),"selected pasted root opens its detached materials");click(dialog,"closeAssemblyCatalog");
        });
    });
}
void stale_catalog_metadata(MainWindow& window,const Authored& authored) {
    DocumentSnapshot replacement_source=window.document().snapshot();
    hub(window,[&](QDialog& workspace){
        select_type(workspace,authored.leaf_type);
        modal("assemblyCatalogDialog",[&]{click(workspace,"assemblyCatalogMetadata");},[&](QDialog& dialog){
            text(dialog,"assemblyMaterialId","wood");text(dialog,"assemblyMaterialName","Must not replace newer work");
            const auto captured=window.document().snapshot();auto replacement=Document::fork_at_revision(captured,captured.revision()-1);
            const auto prefix=replacement.snapshot();std::vector<EntityChange> changes;
            for(const auto& [id,original]:captured.entities()) {auto next=original;if(id=="fixture-catalog")next.extensions["metadata_concurrent_work"]=true;changes.push_back(EntityChange::upsert(next));}
            for(const auto& [id,unused]:prefix.entities()) {(void)unused;if(!captured.entities().contains(id))changes.push_back(EntityChange::erase(id));}
            replacement.apply(ApplyEntityChanges{prefix.revision(),std::move(changes),{},"Concurrent metadata source replacement"});
            require(replacement.revision()==captured.revision() && replacement.snapshot().document_id()==captured.document_id(),"metadata fixture retains same document ID and revision");
            window.document()=std::move(replacement);replacement_source=window.document().snapshot();
            click(dialog,"saveAssemblyMaterial");click(dialog,"saveAssemblyMaterial");
            require(control<QLabel>(dialog,"assemblyCatalogStatus").text().contains("changed"),"metadata source fence reports same-head replacement without silently rebasing the draft");
            click(dialog,"closeAssemblyCatalog");
        });
    });
    require(window.document().snapshot().entities()==replacement_source.entities() && window.document().snapshot().assets()==replacement_source.assets() && window.document().revision()==replacement_source.revision(),"stale material Apply preserves the newer complete source and history");
}
void catalog_context_refusal(MainWindow& window,const Authored& authored,const QString& pasted) {
    for(const bool change_workspace:{false,true}) {
        window.setWorkspace(Workspace::measurement);require(window.selectEntity(authored.root),"material guard captures selected source root");
        const auto before=window.document().snapshot();
        hub(window,[&](QDialog& workspace){
            select_type(workspace,authored.leaf_type);
            modal("assemblyCatalogDialog",[&]{click(workspace,"assemblyCatalogMetadata");},[&](QDialog& dialog){
                text(dialog,"assemblyMaterialId","wood");text(dialog,"assemblyMaterialName","Must retain captured authority");
                if(change_workspace)window.setWorkspace(Workspace::architectural);
                else require(window.selectEntity(pasted),"selection change exercises catalog context fence");
                click(dialog,"saveAssemblyMaterial");
                require(control<QLabel>(dialog,"assemblyCatalogStatus").text().contains("changed"),"material Apply refuses changed workspace or selection");click(dialog,"closeAssemblyCatalog");
            });
        });
        require(window.document().snapshot().entities()==before.entities() && window.document().snapshot().assets()==before.assets() && window.document().revision()==before.revision(),"changed material authority preserves the exact source and history");
    }
    window.setWorkspace(Workspace::measurement);
}
void transforms_clipboard_output(MainWindow& window,const Authored& authored,const QString& directory) {
    selected_frame(window,authored.root,std::numbers::pi/6);
    for(const auto& edit:std::vector<std::vector<QString>>{{"0","2 m","-1 m",".1 m","1"},{"45","0 m","0 m","0 m","1"},{"0","0 m","0 m","0 m",".5"}}) {
        const auto before=window.document().snapshot();const auto prior=instance(window,authored.root);
        require(window.transformSelectedArchitecturalObject(edit[0],edit[1],edit[2],edit[3],edit[4],false),"actual move/rotate/uniform resize commits");
        const auto after=window.document().snapshot();require(after.revision()==before.revision()+1 && instance(window,authored.root).instance.nested_overrides==prior.instance.nested_overrides,"transform is atomic and preserves nested overrides");
        history(window,before,after);require(window.selectEntity(authored.root),"restore selected root after history");
    }
    auto transformed=instance(window,authored.root);check_close(transformed.instance.root_transform->translation_m.z,.15,"world transform composition scales moved Z");
    check_close(transformed.instance.root_transform->scale,.625,"uniform resize retains world scale");selected_frame(window,authored.root,transformed.instance.root_transform->rotation_radians);
    const auto before_clone=window.document().snapshot();require(window.transformSelectedArchitecturalObject("0","15 m","0 m","0 m","1",true),"duplicate uses actual architectural transform controller");
    const auto duplicate=window.selectedEntityId();require(duplicate!=authored.root && before_clone.entities().at(authored.root.toStdString())==window.document().snapshot().entities().at(authored.root.toStdString()),"duplicate has independent identity and leaves source exact");
    require(instance(window,duplicate).instance.nested_overrides==transformed.instance.nested_overrides,"duplicate retains nested identities and overrides");
    require(window.selectEntity(authored.root) && window.copySelection(),"actual Copy publishes local graph");
    const auto payload=Json::parse(QApplication::clipboard()->text().toStdString());bool copied_catalog=false;
    for(const auto& e:payload.at("entities"))if(e.at("type")=="assembly_model") {
        copied_catalog=true;const auto catalog=AssemblyModel::from_json(e.at("properties").at("model"));
        require(catalog.types().size()==2 && catalog.instances().empty() && catalog.materials().size()==2,"clipboard catalog is transitive and minimal, excluding unused types and roof materials");
    }
    require(copied_catalog,"Copy includes detached supporting catalog");const auto before_paste=window.document().snapshot();
    require(window.pasteSelection(),"actual Paste admits independent graph");const auto pasted=window.selectedEntityId();
    require(pasted!=duplicate && pasted!=authored.root,"Paste allocates independent root ID");const auto pasted_value=instance(window,pasted);
    require(pasted_value.assembly_catalog_id!=transformed.assembly_catalog_id && pasted_value.instance.type_id==transformed.instance.type_id && pasted_value.instance.nested_overrides==transformed.instance.nested_overrides,"Paste remaps catalog/root identities and preserves local part IDs");
    history(window,before_paste,window.document().snapshot());
    catalog_metadata_lifecycle(window,authored,pasted);
    catalog_context_refusal(window,authored,pasted);
    stale_catalog_metadata(window,authored);
    window.setWorkspace(Workspace::architectural);auto& views=control<QComboBox>(window,"architecturalView");
    for(int view=0;view<3;++view) {
        views.setCurrentIndex(view);QApplication::processEvents();auto& drawing=canvas(window,"architecturalPlanCanvas");
        const auto& projection=shape(drawing,authored.root);require(!projection.segments.empty(),"plan/elevation/section publish actual assembly linework");
        drawing.fitView();const auto output=QDir(directory).filePath(QString("assembly-view-%1.png").arg(view));
        require(window.exportDraftImage(output),"actual view PNG exports");const QImage image(output);require(!image.isNull() && image.width()>100 && image.height()>100,"exported coordinated PNG is readable");
        capture(drawing,QString("assembly-view-%1-canvas.png").arg(view));
    }
    views.setCurrentIndex(0);const auto svg=QDir(directory).filePath("assembly-plan.svg");require(window.exportDraftSvg(svg),"actual assembly SVG exports");
    QFile output(svg);require(output.open(QIODevice::ReadOnly) && output.readAll().contains("<svg"),"SVG contains vector document");
    const auto path=QDir(directory).filePath("assembly-lifecycle.sketch");require(window.saveProjectAs(path),"assembly project saves");const auto saved=window.document().snapshot();
    MainWindow reopened({},nullptr,QDir(directory).filePath("reopened-library.json"));display(reopened);require(reopened.openProject(path),"actual assembly project reopens");
    require(reopened.document().snapshot().entities()==saved.entities() && reopened.document().snapshot().assets()==saved.assets(),"Save/Reopen retains exact semantic graph and assets");
    require(instance(reopened,authored.root)==instance(window,authored.root) && reopened.scheduleSnapshot().snapshot.rows==window.scheduleSnapshot().snapshot.rows,"reopened placements, overrides and schedules agree");
}
void type_update_delete_and_stale(MainWindow& window,const Authored& authored) {
    window.setWorkspace(Workspace::measurement);require(window.selectEntity(authored.root),"type lifecycle keeps selected root");
    const auto before=window.document().snapshot();const auto retained=instance(window,authored.root);
    hub(window,[&](QDialog& workspace) {
        select_type(workspace,authored.leaf_type);
        modal("assemblyTypeAuthoringDialog",[&]{click(workspace,"editGeometricAssemblyType");},[&](QDialog& dialog) {
            auto& properties=control<QTableWidget>(dialog,"assemblyTypeProperties");properties.item(0,1)->setText("paint");
            auto& profiles=control<QTableWidget>(dialog,"assemblyProfiles");cell_text(profiles,1,1,"1.5 m");
            auto* typed=dynamic_cast<AssemblyTypeDialog*>(&dialog);require(typed && !typed->submit(),"type editing requires exact impact review");
            click(dialog,"assemblyPreviewTypeUpdate");auto& impacts=control<QTableWidget>(dialog,"assemblyUpdateImpacts");
            require(impacts.rowCount()==2,"update preview includes original and duplicate transitive roots, excluding detached pasted catalog");
            require(impacts.item(0,2) && !impacts.item(0,2)->text().isEmpty(),"typed preview displays retained equal override provenance");
            capture(dialog,"assembly-type-impact-editor.png");save(dialog);
        });
    });
    const auto after=window.document().snapshot();require(after.revision()==before.revision()+1 && instance(window,authored.root)==retained,"type update is one atomic catalog revision and retains every explicit instance override");
    const auto refreshed=window.scheduleSnapshot();require(refreshed.diagnostics.empty(),"type update refreshes actual schedules without stale quantities");
    check_close(quantity(row(refreshed,authored.root.toStdString()),"volume"),153*std::pow(.625,3),"type profile height edit refreshes transitive measured volume");
    check_close(quantity(row(refreshed,authored.root.toStdString()),"quantity:volume"),198,"type geometry edit preserves independent declared quantities");
    history(window,before,after);const auto catalog_before_remove=window.document().snapshot();
    hub(window,[&](QDialog& workspace){select_type(workspace,authored.leaf_type);click(workspace,"removeGeometricAssemblyType");
        require(!control<QLabel>(workspace,"").text().isEmpty(),"in-use type removal presents an actual diagnostic");});
    require(window.document().snapshot().entities()==catalog_before_remove.entities() && window.document().revision()==catalog_before_remove.revision(),"in-use nested type removal refuses without a revision");
    require(window.selectEntity(authored.root),"select independent root for deletion");const auto before_delete=window.document().snapshot();
    require(window.deleteSelection() && !window.document().snapshot().entities().contains(authored.root.toStdString()),"root Delete removes independent instance");
    require(window.document().snapshot().entities().at("fixture-catalog")==before_delete.entities().at("fixture-catalog"),"root Delete preserves reusable catalog");history(window,before_delete,window.document().snapshot());
    // Replace the entire same-ID, same-revision head while an editor retains
    // its source. A revision-only guard would incorrectly accept this draft.
    DocumentSnapshot stale_result=window.document().snapshot();
    hub(window,[&](QDialog& workspace) {
        select_type(workspace,authored.leaf_type);
        modal("assemblyTypeAuthoringDialog",[&]{click(workspace,"editGeometricAssemblyType");},[&](QDialog& dialog) {
            text(dialog,"assemblyTypeName","Must not overwrite newer source");click(dialog,"assemblyPreviewTypeUpdate");
            const auto captured=window.document().snapshot();auto replacement=Document::fork_at_revision(captured,captured.revision()-1);
            const auto prefix=replacement.snapshot();std::vector<EntityChange> changes;
            for(const auto& [id,original]:captured.entities()) {auto next=original;if(id=="fixture-catalog")next.extensions["newer_unrelated_work"]=true;changes.push_back(EntityChange::upsert(next));}
            for(const auto& [id,unused]:prefix.entities()) { (void)unused;if(!captured.entities().contains(id))changes.push_back(EntityChange::erase(id)); }
            replacement.apply(ApplyEntityChanges{prefix.revision(),std::move(changes),{},"Concurrent same-head replacement"});
            require(replacement.revision()==captured.revision() && replacement.snapshot().document_id()==captured.document_id(),"stale fixture retains exact document ID and revision");
            window.document()=std::move(replacement);stale_result=window.document().snapshot();save(dialog);
        });
    });
    require(window.document().snapshot().entities()==stale_result.entities() && window.document().revision()==stale_result.revision() && window.lastError().contains("changed"),"full-snapshot fence refuses stale type draft and preserves newer unrelated work");
}
void joined_roof_lifecycle(const QString& directory) {
    MainWindow window({},nullptr,QDir(directory).filePath("roof-library.json"));display(window);
    const auto boundary=window.createBoundary(rectangle(0,0,1,1));require(!boundary.isEmpty(),"roof context created");
    const auto source=window.document().snapshot();const auto context=source.entities().at(boundary.toStdString()).properties;
    SlopedRoofPanel first{"roof-a",{0,0,0},0,2,4,1,std::atan(.5),0,.1,{{"opening-a",.4,1.5,.3,.4}}};
    auto second=first;second.id="roof-b";second.base_position={1.9,0,.95};second.openings={{"opening-b",.4,.5,.3,.4}};
    auto a=encode_building_entity(first),b=encode_building_entity(second);
    for(auto* e:{&a,&b})for(const char* key:{"property_id","building_id","floor_id","layer_id"})if(context.contains(key))e->properties[key]=context.at(key);
    a.properties["name"]="Roof A with opening";b.properties["name"]="Roof B with opening";
    a.properties["mark"]="R-A";b.properties["mark"]="R-B";
    a.properties["material_assignment"]={{"version",1},{"catalog_id","roof-catalog"},{"material_id","red"}};
    b.properties["material_assignment"]={{"version",1},{"catalog_id","roof-catalog"},{"material_id","blue"}};
    Entity catalog{"roof-catalog","assembly_model",{{"model",AssemblyModel::create({{"red","Red roof","#C54444"},{"blue","Blue roof","#4466BB"}}, {}, {}).to_json()}}};
    window.document().apply(ApplyEntityChanges{source.revision(),{EntityChange::upsert(a),EntityChange::upsert(b),EntityChange::upsert(catalog)}, {},"Seed overlapping roofs with actual openings"});
    require(window.selectEntity("roof-a") && window.selectEntity("roof-b",true),"actual ordered multi-roof selection");
    const auto before_join=window.document().snapshot();const auto join=window.joinSelectedRoofs();require(!join.isEmpty(),"actual Join creates fused roof root");
    const auto after_join=window.document().snapshot();require(after_join.revision()==before_join.revision()+1 && after_join.entities().at(a.id)==a && after_join.entities().at(b.id)==b,"Join is atomic and preserves source roof/opening identities");history(window,before_join,after_join);
    const auto sa=make_sloped_roof_panel(first),sb=make_sloped_roof_panel(second);BRepAlgoAPI_Common common(sa,sb);common.Build();BRepAlgoAPI_Fuse fused(sa,sb);fused.Build();
    require(common.IsDone()&&!common.HasErrors()&&fused.IsDone()&&!fused.HasErrors(),"independent roof union/common fixture succeeds");
    const auto va=solid_volume(sa),vb=solid_volume(sb),overlap=solid_volume(common.Shape()),total=va+vb-overlap;require(overlap>1e-5,"true overlap exercises material priority");
    require(window.selectEntity(join),"select actual fused canvas root");
    const auto expected=project_shape_view(fused.Shape(),BuildingViewKind::plan);const auto& displayed=shape(canvas(window),join);
    require(displayed.type=="roof_join" && displayed.segments.size()==expected.size() && !displayed.segments.empty(),"actual joined canvas publishes fused outline rather than separate source rectangles");
    double displayed_length=0,expected_length=0;for(const auto& edge:displayed.segments)displayed_length+=segment_length(edge);for(const auto& edge:expected)expected_length+=segment_length(edge);
    check_close(displayed_length,expected_length,"fused canvas linework agrees with independently projected union including opening edges");
    for(const auto& entity:canvas(window).entities())require(entity.id!="roof-a" && entity.id!="roof-b","joined canvas suppresses member seams");
    auto schedules=window.scheduleSnapshot();
    if(!schedules.diagnostics.empty())throw std::runtime_error("joined roof schedule: "+schedules.diagnostics.front());
    check_close(quantity(row(schedules,join.toStdString()),"volume"),total,"joined volume matches independent union");check_close(summary_total(schedules),total,"net material summaries exclude gross sources");
    check_close(quantity(row(schedules,roof_join_material_schedule_row_id(join.toStdString(),"roof-b")),"volume"),vb-overlap,"second priority owns only residual material");
    auto& materials=control<QComboBox>(window,"materialAssignment");int blue=-1;
    for(int i=0;i<materials.count();++i)if(!materials.itemData(i).toString().isEmpty()) {const auto value=Json::parse(materials.itemData(i).toString().toStdString());if(value.at("material_id")=="blue")blue=i;}
    require(blue>=0,"joined object exposes real catalog material choice");const auto before_assign=window.document().snapshot();materials.setCurrentIndex(blue);click(window,"assignMaterial");
    const auto assigned=window.document().snapshot();const auto semantic=parse_roof_join(assigned.entities().at(join.toStdString()).properties,join.toStdString());
    require(assigned.revision()==before_assign.revision()+1 && semantic.material_assignment==RoofJoinMaterialAssignment{"roof-catalog","blue"} && assigned.entities().at(join.toStdString()).properties.at("version")==2,"material combo publishes one V2 joined assignment revision");
    schedules=window.scheduleSnapshot();check_close(summary_total(schedules),total,"join assignment conserves net takeoff");
    for(const auto& id:{"roof-a","roof-b"})require(std::get<std::string>(row(schedules,roof_join_material_schedule_row_id(join.toStdString(),id)).cells.at("material_id").value)=="blue","joined assignment overrides every disjoint region binding");history(window,before_assign,assigned);
    require(window.selectEntity(join),"refresh joined material context");choice(control<QComboBox>(window,"materialAssignment"),QString{});click(window,"assignMaterial");
    require(!parse_roof_join(window.document().snapshot().entities().at(join.toStdString()).properties,join.toStdString()).material_assignment,"material combo clears V2 override");check_close(summary_total(window.scheduleSnapshot()),total,"clearing restores source bindings without double count");
    const auto before_priority=window.document().snapshot();
    modal("roofJoinPropertiesDialog",[&]{click(window,"editBuildingObject");},[&](QDialog& dialog) {
        auto& members=control<QListWidget>(dialog,"roofJoinMembers");require(members.count()==2,"priority editor lists real source roofs");
        members.setCurrentRow(1);click(dialog,"roofJoinMemberUp");require(members.item(0)->data(Qt::UserRole).toString()=="roof-b","Move up changes ordered priority");
        click(dialog,"roofJoinMemberDown");require(members.item(0)->data(Qt::UserRole).toString()=="roof-a","Move down restores ordered priority");
        click(dialog,"roofJoinMemberUp");capture(dialog,"joined-roof-priority-editor.png");control<QDialogButtonBox>(dialog,"").button(QDialogButtonBox::Ok)->click();require(dialog.result()==QDialog::Accepted,"priority modal saves");
    });
    const auto reordered=window.document().snapshot();require(reordered.revision()==before_priority.revision()+1 && parse_roof_join(reordered.entities().at(join.toStdString()).properties,join.toStdString()).roof_ids==std::vector<std::string>{"roof-b","roof-a"},"priority edit is one ordered atomic revision");
    schedules=window.scheduleSnapshot();check_close(summary_total(schedules),total,"priority reorder conserves exact total");check_close(quantity(row(schedules,roof_join_material_schedule_row_id(join.toStdString(),"roof-a")),"volume"),va-overlap,"reordered net material ownership follows first source");history(window,before_priority,reordered);
    require(window.selectEntity(join),"retain joined roof selection");canvas(window).fitView();capture(window,"joined-roof-lifecycle-window.png");
    const auto path=QDir(directory).filePath("joined-roof.sketch");require(window.saveProjectAs(path),"joined roof project saves");const auto saved=window.document().snapshot();
    MainWindow reopened({},nullptr,QDir(directory).filePath("roof-reopened-library.json"));display(reopened);require(reopened.openProject(path),"joined roof project reopens");
    require(reopened.document().snapshot().entities()==saved.entities() && reopened.scheduleSnapshot().snapshot.rows==window.scheduleSnapshot().snapshot.rows,"joined priority, source openings, V2 clear and net schedules survive Save/Reopen");
}
}
int main(int argc,char** argv) {
    sketch::testing::noninteractive_errors();QApplication application(argc,argv);QTemporaryDir settings;
    QCoreApplication::setOrganizationName("VertexTests");QCoreApplication::setApplicationName("AssemblyWorkflowDesktop");
    QSettings::setDefaultFormat(QSettings::IniFormat);QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,settings.path());
    try {
        require(QFontDatabase::addApplicationFont(":/fonts/Inter.ttf")>=0,"bundled readable test font loads");
        QTemporaryDir directory;require(directory.isValid(),"isolated fixture output directory");
        if(application.arguments().contains(QStringLiteral("--roof-only"))) {
            joined_roof_lifecycle(directory.path());std::cout<<"joined roof desktop lifecycle passed\n";return 0;
        }
        first_material_apply(directory.path());
        MainWindow window({},nullptr,directory.filePath("library.json"));display(window);empty_hub_cancellation(window);const auto authored=author(window);instance_editor(window,authored);untouched_assembly_saves(window,authored);
        transforms_clipboard_output(window,authored,directory.path());type_update_delete_and_stale(window,authored);
        joined_roof_lifecycle(directory.path());std::cout<<"assembly_workflow_desktop_tests passed\n";return 0;
    } catch(const std::exception& error){std::cerr<<"assembly_workflow_desktop_tests: "<<error.what()<<'\n';return 1;}
}
