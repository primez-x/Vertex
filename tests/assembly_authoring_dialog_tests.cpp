#include "sketch/desktop/assembly_authoring_dialog.hpp"
#include "sketch/boundary_entity.hpp"
#include "support/noninteractive_errors.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QTimer>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <exception>
#include <functional>
#include <stdexcept>

namespace {
using namespace sketch;
using namespace sketch::desktop;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class W> W& widget(QDialog& dialog, const char* name) {
    auto* result = dialog.findChild<W*>(name);
    require(result != nullptr, name); return *result;
}
void click(QDialog& dialog, const char* name) { widget<QPushButton>(dialog,name).click(); }
void text(QDialog& dialog, const char* name, const char* value) { widget<QLineEdit>(dialog,name).setText(value); }
void modal_action(QDialog& owner,const char* action,const char* modal_name,const std::function<void(QDialog&)>& edit) {
    std::exception_ptr error;bool executed=false;QTimer timer;timer.setSingleShot(true);
    QObject::connect(&timer,&QTimer::timeout,[&]{
        executed=true;
        auto* modal=qobject_cast<QDialog*>(QApplication::activeModalWidget());
        try {require(modal && modal->objectName()==modal_name,"expected typed child editor");edit(*modal);}
        catch(...) {error=std::current_exception();if(modal)modal->reject();}
    });
    timer.start(0);click(owner,action);if(error)std::rethrow_exception(error);require(executed,"child editor must execute the widget action");
}
void save(QDialog& dialog) {widget<QDialogButtonBox>(dialog,"assemblyAuthoringButtons").button(QDialogButtonBox::Save)->click();}
Entity catalog() {
    AssemblyType leaf; leaf.id="leaf"; leaf.name="Timber post";
    leaf.properties={{"finish","natural"}}; leaf.materials={{"surface","wood"}};
    leaf.quantities={{"pieces",{1,AssemblyQuantityUnit::count}}};
    leaf.profiles={{"solid", {{{0,0},{1,0},0},{{1,0},{1,1},0},{{1,1},{0,1},0},{{0,1},{0,0},0}}, {},0,2,"surface"}};
    AssemblyType frame; frame.id="frame"; frame.name="Post frame";
    frame.parts={{"post","leaf",{{2,0,0},0,1},{},{},{}}};
    return {"catalog","assembly_model",{{"name","Timber catalog"},{"model",AssemblyModel::create({{"wood","Wood"}}, {leaf,frame},{}).to_json()}}};
}
Entity instance() {
    Entity source{"placed","assembly_instance",{{"name","Entrance frame"},{"opaque","retain"}}};
    AssemblyInstance item; item.id=source.id; item.type_id="frame"; item.root_transform=AssemblyTransform{{10,20,3},0,1};
    item.nested_overrides={{{"post"},std::nullopt,{{"finish","natural"}},{{"surface","wood"}},{{"pieces",{1,AssemblyQuantityUnit::count}}}}};
    return encode_document_assembly_instance(source,{"catalog",item});
}
Entity outline() {
    IdentifiedBoundary b; b.id="outline"; b.type="boundary";
    const Boundary square{{{0,0},{2,0},0},{{2,0},{2,1},0},{{2,1},{0,1},0},{{0,1},{0,0},0}};
    for (std::size_t i=0;i<square.size();++i) b.segments.push_back({"edge"+std::to_string(i),"v"+std::to_string(i),"v"+std::to_string((i+1)%4),square[i]});
    auto e=encode_identified_boundary_entity(b); e.properties["name"]="Base outline"; return e;
}
void type_create_geometry_and_cycle_refusal() {
    auto document=Document::create({catalog(),outline()}); const auto source=document.snapshot();
    AssemblyTypeDialog dialog(source,"catalog",std::nullopt,true);
    text(dialog,"assemblyTypeName","Canopy"); click(dialog,"assemblyAddProfile");
    auto& profiles=widget<QTableWidget>(dialog,"assemblyProfiles");
    qobject_cast<QLineEdit*>(profiles.cellWidget(0,1))->setText("3 ft");
    qobject_cast<QLineEdit*>(profiles.cellWidget(0,2))->setText("0.25 m");
    click(dialog,"assemblyAddPart");
    auto& parts=widget<QTableWidget>(dialog,"assemblyParts");
    auto* child=qobject_cast<QComboBox*>(parts.cellWidget(0,0)); child->setCurrentIndex(child->findData("leaf"));
    qobject_cast<QLineEdit*>(parts.cellWidget(0,1))->setText("2 m");
    qobject_cast<QLineEdit*>(parts.cellWidget(0,4))->setText("90");
    require(dialog.submit(),"typed profile and child draft must submit");
    auto draft=*dialog.candidate(); require(draft.replacement.profiles.size()==1 && draft.replacement.parts.size()==1,"real typed geometry");
    require(std::abs(draft.replacement.profiles[0].height_m-0.9144)<1e-12,"shared units parser");
    require(draft.replacement.parts[0].transform.translation_m.x==2,"child placement");
    require(std::abs(draft.replacement.parts[0].transform.rotation_radians-std::acos(-1.0)/2)<1e-12,"bare yaw uses degrees");
    require(document.snapshot().entities()==source.entities() && document.revision()==source.revision(),"dialog must not mutate Document");

    auto model=AssemblyModel::from_json(catalog().properties.at("model"));
    auto leaf=*std::find_if(model.types().begin(),model.types().end(),[](const auto& t){return t.id=="leaf";});
    AssemblyTypeDialog cyclic(source,"catalog",leaf,true); click(cyclic,"assemblyAddPart");
    auto* type=qobject_cast<QComboBox*>(widget<QTableWidget>(cyclic,"assemblyParts").cellWidget(0,0));
    type->setCurrentIndex(type->findData("frame"));
    require(!cyclic.previewUpdate() && !cyclic.candidate(),"indirect cycle refused");
    require(!cyclic.lastError().isEmpty(),"cycle is inline diagnostic");
}
void instance_overrides_and_cancel() {
    auto original=instance(); auto document=Document::create({catalog(),original}); auto snapshot=document.snapshot();
    AssemblyInstanceDialog dialog(snapshot,original,{},true);
    text(dialog,"assemblyInstanceName","Moved entrance"); text(dialog,"assemblyRootX","12 m");
    widget<QCheckBox>(dialog,"assemblyNestedTransformEnabled").setChecked(true);
    text(dialog,"assemblyNestedZ","4 m");
    require(dialog.submit(),"independent instance edit"); auto draft=*dialog.candidate();
    require(draft.value.instance.root_transform->translation_m.x==12,"world placement");
    require(draft.value.instance.nested_overrides.size()==1,"stable nested override");
    const auto& nested=draft.value.instance.nested_overrides.front();
    require(nested.part_path==std::vector<std::string>{"post"} && nested.transform->translation_m.z==4,"part transform");
    require(nested.property_overrides.at("finish")=="natural" && nested.quantity_overrides.at("pieces").value==1,"equal explicit overrides retained");
    require(draft.expected_entity==original && draft.name=="Moved entrance","source and name returned");
    require(document.snapshot().entities()==snapshot.entities(),"source preserved");
    AssemblyInstanceDialog invalid(snapshot,original,{},true); text(invalid,"assemblyRootScale","0");
    require(!invalid.submit() && !invalid.candidate(),"invalid scale refused atomically");
    AssemblyInstanceDialog cancel(snapshot,original,{},true); text(cancel,"assemblyRootX","55 m"); cancel.reject();
    require(!cancel.candidate() && document.snapshot().entities()==snapshot.entities(),"Cancel preserves full source");
}
void edit_preview_and_stable_identity() {
    auto document=Document::create({catalog(),instance(),outline()}); auto source=document.snapshot();
    auto model=AssemblyModel::from_json(catalog().properties.at("model"));
    const auto leaf=*std::find_if(model.types().begin(),model.types().end(),[](const auto& t){return t.id=="leaf";});
    AssemblyTypeDialog dialog(source,"catalog",leaf,true); text(dialog,"assemblyTypeName","Edited post");
    require(!dialog.submit(),"edit needs displayed impact review");
    require(dialog.previewUpdate(),"transitive update preview");
    require(widget<QTableWidget>(dialog,"assemblyUpdateImpacts").rowCount()==1,"external transitive instance shown");
    text(dialog,"assemblyTypeName","Changed after preview"); require(!dialog.submit(),"changed preview cannot accept");
    require(dialog.previewUpdate() && dialog.submit(),"reviewed exact edit accepts");
    auto draft=*dialog.candidate(); require(draft.replacement.profiles[0].id=="solid","profile identity retained");
    require(draft.document_impacts.size()==1 && draft.document_impacts[0].retained_overrides.instance==decode_document_assembly_instance(instance()).instance,"explicit override preview provenance retained");
    bool refused=false;
    try {AssemblyTypeDialog stale(source,"catalog",AssemblyType{"leaf","wrong"},true);}
    catch(const std::invalid_argument&) {refused=true;}
    require(refused,"caller/source mismatch refused");
}
void reorder_remove_and_precision() {
    auto document=Document::create({catalog(),outline()}); auto snapshot=document.snapshot();
    AssemblyTypeDialog dialog(snapshot,"catalog",std::nullopt,false);text(dialog,"assemblyTypeName","Ordered solids");
    click(dialog,"assemblyAddProfile");click(dialog,"assemblyAddProfile");
    auto& profiles=widget<QTableWidget>(dialog,"assemblyProfiles");
    auto profile_id=qobject_cast<QComboBox*>(profiles.cellWidget(1,0))->property("stableId").toString().toStdString();
    profiles.setCurrentCell(1,0);click(dialog,"assemblyProfileUp");
    click(dialog,"assemblyAddPart");click(dialog,"assemblyAddPart");click(dialog,"assemblyAddPart");
    auto& parts=widget<QTableWidget>(dialog,"assemblyParts");
    for(int i=0;i<parts.rowCount();++i){auto* choice=qobject_cast<QComboBox*>(parts.cellWidget(i,0));choice->setCurrentIndex(choice->findData("leaf"));}
    auto part_id=qobject_cast<QComboBox*>(parts.cellWidget(1,0))->property("stableId").toString().toStdString();
    parts.setCurrentCell(1,0);click(dialog,"assemblyPartUp");parts.setCurrentCell(2,0);click(dialog,"assemblyPartRemove");
    require(dialog.submit(),"reordered profile and part draft accepts");auto draft=*dialog.candidate();
    require(draft.replacement.profiles.front().id==profile_id && draft.replacement.parts.front().id==part_id,"source order changes while stable IDs remain");
    require(draft.replacement.parts.size()==2,"remove affects only selected part");

    auto original=instance();auto decoded=decode_document_assembly_instance(original);
    decoded.instance.root_transform->rotation_radians=0.12345678901234567;
    decoded.instance.root_transform->translation_m.x=1.2345678901234567;
    original=encode_document_assembly_instance(original,decoded);
    auto precise_document=Document::create({catalog(),original});
    AssemblyInstanceDialog untouched(precise_document.snapshot(),original,{},false);
    require(untouched.submit() && untouched.candidate()->value==decoded,"untouched semantic doubles and explicit overrides are exact");
    auto& names=widget<QComboBox>(untouched,"assemblyNestedPart");
    require(names.currentText().contains("Timber post") && !names.currentText().contains("post /"),"nested choices use readable type names");
}
void named_override_validation() {
    auto document=Document::create({catalog()});auto snapshot=document.snapshot();
    AssemblyInstanceDialog dialog(snapshot,std::nullopt,"catalog",true);
    auto& types=widget<QComboBox>(dialog,"assemblyInstanceType");types.setCurrentIndex(types.findData("leaf"));
    text(dialog,"assemblyInstanceName","Explicit post");
    click(dialog,"assemblyRootPropertiesAdd");auto& properties=widget<QTableWidget>(dialog,"assemblyRootProperties");
    properties.item(0,0)->setText("finish");properties.item(0,1)->setText("paint");
    click(dialog,"assemblyRootMaterialsAdd");widget<QTableWidget>(dialog,"assemblyRootMaterials").item(0,0)->setText("surface");
    click(dialog,"assemblyRootQuantitiesAdd");auto& quantities=widget<QTableWidget>(dialog,"assemblyRootQuantities");
    quantities.item(0,0)->setText("pieces");quantities.item(0,1)->setText("3");
    require(dialog.submit(),"explicit named property material and quantity widgets produce a valid instance");
    const auto authored=*dialog.candidate();const auto& item=authored.value.instance;
    require(item.property_overrides.at("finish")=="paint" && item.material_overrides.at("surface")=="wood" && item.quantity_overrides.at("pieces").value==3,"typed override values retained");
    auto existing=instance();auto with_instance=Document::create({catalog(),existing});
    AssemblyInstanceDialog invalid(with_instance.snapshot(),existing,{},true);
    auto& nested_quantities=widget<QTableWidget>(invalid,"assemblyNestedQuantities");
    qobject_cast<QComboBox*>(nested_quantities.cellWidget(0,2))->setCurrentIndex(static_cast<int>(AssemblyQuantityUnit::metre));
    require(!invalid.submit() && !invalid.candidate(),"changed override quantity dimension refuses");
    qobject_cast<QComboBox*>(nested_quantities.cellWidget(0,2))->setCurrentIndex(static_cast<int>(AssemblyQuantityUnit::count));
    widget<QTableWidget>(invalid,"assemblyNestedProperties").item(0,0)->setText("undeclared");
    require(!invalid.submit() && !invalid.candidate(),"undeclared nested property refuses");
    AssemblyTypeDialog cancel(snapshot,"catalog",std::nullopt,true);text(cancel,"assemblyTypeName","Uncommitted type");cancel.reject();
    require(!cancel.candidate() && document.snapshot().entities()==snapshot.entities(),"type Cancel preserves source");
}
void authored_part_overrides_modal() {
    auto document=Document::create({catalog()});const auto snapshot=document.snapshot();
    auto model=AssemblyModel::from_json(catalog().properties.at("model"));auto frame=*std::find_if(model.types().begin(),model.types().end(),[](const auto& value){return value.id=="frame";});
    AssemblyTypeDialog dialog(snapshot,"catalog",frame,true);
    auto& parts=widget<QTableWidget>(dialog,"assemblyParts");parts.setCurrentCell(0,0);
    modal_action(dialog,"assemblyPartOverrides","assemblyPartOverridesDialog",[](auto& modal){
        click(modal,"assemblyPartOverridePropertiesAdd");auto& p=widget<QTableWidget>(modal,"assemblyPartOverrideProperties");p.item(0,0)->setText("finish");p.item(0,1)->setText("natural");
        click(modal,"assemblyPartOverrideMaterialsAdd");widget<QTableWidget>(modal,"assemblyPartOverrideMaterials").item(0,0)->setText("surface");
        click(modal,"assemblyPartOverrideQuantitiesAdd");auto& quantities=widget<QTableWidget>(modal,"assemblyPartOverrideQuantities");quantities.item(0,0)->setText("pieces");quantities.item(0,1)->setText("1");save(modal);
    });
    modal_action(dialog,"assemblyPartOverrides","assemblyPartOverridesDialog",[](auto& modal){
        widget<QTableWidget>(modal,"assemblyPartOverrideProperties").item(0,1)->setText("paint");modal.reject();
    });
    require(dialog.previewUpdate() && dialog.submit(),"typed authored child overrides accept");const auto draft=*dialog.candidate();const auto& part=draft.replacement.parts.front();
    require(part.id=="post" && part.property_overrides.at("finish")=="natural" && part.material_overrides.at("surface")=="wood" && part.quantity_overrides.at("pieces").value==1,"explicit equal-value part overrides survive child editor Cancel");
    require(document.snapshot().entities()==snapshot.entities(),"part editor preserves source");
    auto changed=model.with_type(draft.replacement);auto next_catalog=catalog();next_catalog.properties["model"]=changed.to_json();auto next_document=Document::create({next_catalog});
    AssemblyTypeDialog replace(next_document.snapshot(),"catalog",draft.replacement,true);widget<QTableWidget>(replace,"assemblyParts").setCurrentCell(0,0);
    modal_action(replace,"assemblyPartOverrides","assemblyPartOverridesDialog",[](auto& modal){
        auto& quantities=widget<QTableWidget>(modal,"assemblyPartOverrideQuantities");qobject_cast<QComboBox*>(quantities.cellWidget(0,2))->setCurrentIndex(static_cast<int>(AssemblyQuantityUnit::metre));save(modal);
        require(!widget<QLabel>(modal,"assemblyAuthoringError").text().isEmpty() && modal.result()!=QDialog::Accepted,"invalid authored part override unit refuses inline");
        qobject_cast<QComboBox*>(quantities.cellWidget(0,2))->setCurrentIndex(static_cast<int>(AssemblyQuantityUnit::count));widget<QTableWidget>(modal,"assemblyPartOverrideProperties").item(0,1)->setText("paint");save(modal);
    });
    require(replace.previewUpdate() && replace.submit() && replace.candidate()->replacement.parts[0].id=="post" && replace.candidate()->replacement.parts[0].property_overrides.at("finish")=="paint","part override replacement retains stable identity");
    AssemblyTypeDialog clear(next_document.snapshot(),"catalog",draft.replacement,true);widget<QTableWidget>(clear,"assemblyParts").setCurrentCell(0,0);
    modal_action(clear,"assemblyPartOverrides","assemblyPartOverridesDialog",[](auto& modal){click(modal,"assemblyClearPartOverrides");save(modal);});
    require(clear.previewUpdate() && clear.submit() && clear.candidate()->replacement.parts[0].property_overrides.empty() && clear.candidate()->replacement.parts[0].quantity_overrides.empty(),"all authored child overrides clear explicitly");
}
Entity cutout_boundary(const std::string& id,double x) {
    IdentifiedBoundary b; b.id=id;b.type="boundary";
    const Boundary edges{{{x,0.25},{x+0.25,0.25},0},{{x+0.25,0.25},{x+0.25,0.75},0},{{x+0.25,0.75},{x,0.75},0},{{x,0.75},{x,0.25},0}};
    for(std::size_t i=0;i<edges.size();++i)b.segments.push_back({"e"+std::to_string(i),"v"+std::to_string(i),"v"+std::to_string((i+1)%4),edges[i]});
    auto value=encode_identified_boundary_entity(b);value.properties["name"]="Cutout "+id;return value;
}
void profile_holes_reimport_and_cutouts() {
    auto first=cutout_boundary("cutout-one",0.25);auto second=cutout_boundary("cutout-two",1.25);
    auto with_holes=outline();with_holes.id="outline-with-holes";with_holes.properties["name"]="Authored outline with cutout";
    const auto hole=boundary_geometry(decode_identified_boundary_entity(first));nlohmann::json encoded=nlohmann::json::array();
    for(const auto& edge:hole)encoded.push_back({{"start",{edge.start.x,edge.start.y}},{"end",{edge.end.x,edge.end.y}},{"sweep_radians",edge.sweep_radians}});
    with_holes.properties["holes"]=nlohmann::json::array({encoded});
    auto document=Document::create({catalog(),outline(),with_holes,first,second});auto snapshot=document.snapshot();
    AssemblyTypeDialog dialog(snapshot,"catalog",std::nullopt,true);text(dialog,"assemblyTypeName","Cutout canopy");click(dialog,"assemblyAddProfile");auto& profiles=widget<QTableWidget>(dialog,"assemblyProfiles");
    auto* choice=qobject_cast<QComboBox*>(profiles.cellWidget(0,0));choice->setCurrentIndex(choice->findData("outline-with-holes"));profiles.setCurrentCell(0,0);
    modal_action(dialog,"assemblyProfileCutouts","assemblyProfileCutoutsDialog",[](auto& modal){
        auto& cutouts=widget<QTableWidget>(modal,"assemblyProfileCutoutRows");require(cutouts.rowCount()==1,"authored boundary holes imported");click(modal,"assemblyProfileCutoutRowsAdd");auto* selected=qobject_cast<QComboBox*>(cutouts.cellWidget(1,0));selected->setCurrentIndex(selected->findData("cutout-two"));save(modal);
    });
    modal_action(dialog,"assemblyProfileCutouts","assemblyProfileCutoutsDialog",[](auto& modal){auto& cutouts=widget<QTableWidget>(modal,"assemblyProfileCutoutRows");cutouts.setCurrentCell(0,0);click(modal,"assemblyProfileCutoutRowsRemove");modal.reject();});
    modal_action(dialog,"assemblyProfileCutouts","assemblyProfileCutoutsDialog",[](auto& modal){
        auto& cutouts=widget<QTableWidget>(modal,"assemblyProfileCutoutRows");click(modal,"assemblyProfileCutoutRowsAdd");auto* selected=qobject_cast<QComboBox*>(cutouts.cellWidget(2,0));selected->setCurrentIndex(selected->findData("outline"));save(modal);
        require(!widget<QLabel>(modal,"assemblyAuthoringError").text().isEmpty() && modal.result()!=QDialog::Accepted,"contact or escaping cutout refuses inline");modal.reject();
    });
    require(dialog.submit() && dialog.candidate()->replacement.profiles[0].holes.size()==2,"cutout add and Cancel preserve local typed geometry");
    AssemblyTypeDialog reimport(snapshot,"catalog",std::nullopt,true);text(reimport,"assemblyTypeName","Reimported canopy");click(reimport,"assemblyAddProfile");auto& rows=widget<QTableWidget>(reimport,"assemblyProfiles");auto* selected=qobject_cast<QComboBox*>(rows.cellWidget(0,0));selected->setCurrentIndex(selected->findData("outline-with-holes"));rows.setCurrentCell(0,0);
    modal_action(reimport,"assemblyProfileCutouts","assemblyProfileCutoutsDialog",[](auto& modal){auto& rows=widget<QTableWidget>(modal,"assemblyProfileCutoutRows");rows.setCurrentCell(0,0);click(modal,"assemblyProfileCutoutRowsRemove");save(modal);});
    click(reimport,"assemblyProfileReimport");require(reimport.submit() && reimport.candidate()->replacement.profiles[0].holes.size()==1,"reimport restores authored outer and holes");
    AssemblyTypeDialog replace(snapshot,"catalog",std::nullopt,true);text(replace,"assemblyTypeName","Solid canopy");click(replace,"assemblyAddProfile");auto* boundary=qobject_cast<QComboBox*>(widget<QTableWidget>(replace,"assemblyProfiles").cellWidget(0,0));boundary->setCurrentIndex(boundary->findData("outline-with-holes"));boundary->setCurrentIndex(boundary->findData("outline"));
    require(replace.submit() && replace.candidate()->replacement.profiles[0].holes.empty(),"outer replacement replaces previous authored holes");
    require(document.snapshot().entities()==snapshot.entities(),"cutout dialogs preserve all source entities");
}
}
int main(int argc, char** argv) {
    sketch::testing::noninteractive_errors();
    QApplication application(argc,argv);
    try {
        type_create_geometry_and_cycle_refusal(); instance_overrides_and_cancel();
        edit_preview_and_stable_identity();
        reorder_remove_and_precision();
        named_override_validation();
        authored_part_overrides_modal();profile_holes_reimport_and_cutouts();
        return 0;
    } catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
