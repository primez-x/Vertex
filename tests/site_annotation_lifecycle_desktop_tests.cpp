// Reuse the captured fixture helpers; this target runs the lifecycle below.
#define main captured_site_plan_fixture_main
#include "site_plan_workflow_desktop_tests.cpp"
#undef main
#include "sketch/site_frame.hpp"
#include "sketch/text_library.hpp"
#include <QCheckBox>
#include <QClipboard>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QLineEdit>
#include <QListWidget>
#include <QMimeData>
#include <QPushButton>
#include <QTimer>

namespace {
QString token(const std::string& owner,const std::string& child) {
    return QStringLiteral("site-annotation:%1:%2:%3").arg(owner.size()).arg(QString::fromStdString(owner),QString::fromStdString(child));
}
void displaySite(MainWindow& window) {
    window.setMetricUnits(true);window.setAttribute(Qt::WA_DontShowOnScreen);window.resize(1300,900);window.show();
    require(window.setActiveLayer("layer-1"),"active Site annotation layer");window.setWorkspace(Workspace::architectural);
    control<QAction>(window,"sitePlan").trigger();QApplication::processEvents();
    auto& canvas=control<PlanCanvas>(window,"architecturalPlanCanvas");canvas.setSnapEnabled(false);canvas.setGridEnabled(false);canvas.setOverviewMapEnabled(false);
}
const Json& child(const Entity& owner,const char* collection) {return owner.properties.at("state").at(collection).at(0);}
void oneEdit(MainWindow& window,const DocumentSnapshot& before) {
    require(window.document().revision()==before.revision()+1,"one atomic annotation command");
    const auto after=window.document().snapshot();require(window.undoCommand(),"annotation Undo");
    require(window.document().snapshot().entities()==before.entities(),"Undo exact owners");
    require(window.redoCommand(),"annotation Redo");require(window.document().snapshot().entities()==after.entities(),"Redo exact owners");
}
void roundtrip(MainWindow& window) {
    QTemporaryDir directory;require(directory.isValid(),"annotation storage fixture folder");
    const auto before=window.document().snapshot();const auto path=directory.filePath("site-annotations.bldproj");
    require(window.saveProjectAs(path)&&window.openProject(path),"annotation editable SaveReopen");
    require(window.document().snapshot().entities()==before.entities(),"SaveReopen retains complete authoring representation");
}
void lifecycle() {
    auto f=fixture();
    // Real desktop symbol creation supplies pinned artwork and definition, then
    // the fixture gives each owner the same local child ID in different layers.
    const auto catalog=default_symbol_catalog();
    const auto sofa=std::find_if(catalog.begin(),catalog.end(),[](const auto& d){return d.id=="svg-v2-04_living-sofa-three-seat"&&d.svg_asset.has_value();});
    require(sofa!=catalog.end(),"actual SVG sofa catalog entry");
    MainWindow seed;const auto symbol_id=seed.createAnnotationSymbol(QString::fromStdString(sofa->id),{4,6});require(!symbol_id.isEmpty(),"real pinned symbol seed");
    Json symbol;
    const auto seed_source=seed.document().snapshot();
    for(const auto& [id,e]:seed_source.entities()) {
        (void)id;if(e.type!=kAnnotationEntityType)continue;
        for(const auto& raw:e.properties.at("state").at("symbols"))if(raw.at("id")==symbol_id.toStdString())symbol=raw;
    }
    require(!symbol.is_null(),"seed symbol persisted");
    require(symbol.at("definition").contains("svg_asset")&&!symbol.at("pinned_svg").get<std::string>().empty(),"seed retains SVG-backed definition and pinned artwork before palette");
    auto source=f.document->snapshot();std::vector<EntityChange> changes;
    for(const auto* owner:{"site-annotation-a","site-annotation-b"}) {
        auto e=source.entities().at(owner);auto raw=symbol;raw["id"]="repeated-symbol";
        raw["placement"]["layer_id"]=std::string(owner)=="site-annotation-a" ? f.layer2.toStdString() : "layer-1";
        e.extensions["vendor_symbols"]["repeated-symbol"]={{"retain","repeated-symbol"}};
        raw["svg_palette"]={{"version",1},{"profile","white-outline-2"},{"outline_color","#111111"},{"surface_color","#ffffff"}};
        e.properties["state"]["version"]=9;e.properties["state"]["symbols"].push_back(raw);
        e.properties["state"]["labels"][0]["vendor_label"]={{"retain","repeated-child"}};
        e.extensions["vendor_owner"]={{"retain",owner}};validate_annotation_entity(e);changes.push_back(EntityChange::upsert(e));
    }
    f.document->apply(ApplyEntityChanges{source.revision(),changes,{},"Seed duplicate owner-local annotations"});
    MainWindow window(f.document);displaySite(window);
    const auto a=token("site-annotation-a","repeated-child"),b=token("site-annotation-b","repeated-child");
    require(window.selectEntity(a),"select typed child in second building layer");
    source=window.document().snapshot();control<QPushButton>(window,"applyAnnotation").click();
    require(document_snapshot_digest(window.document().snapshot())==document_snapshot_digest(source),"untouched Properties Apply is a full-source no-op");
    control<QLineEdit>(window,"annotationContent").setText("Edited typed owner A");control<QPushButton>(window,"applyAnnotation").click();
    const auto after=window.document().snapshot();
    require(after.entities().at("site-annotation-b")==source.entities().at("site-annotation-b"),"properties preserve other duplicate owner exactly");
    require(child(after.entities().at("site-annotation-a"),"labels").at("content")=="Edited typed owner A","properties target captured owner child");
    require(child(after.entities().at("site-annotation-a"),"labels").at("placement")==child(source.entities().at("site-annotation-a"),"labels").at("placement"),"properties retain own-layer coordinates");
    require(child(after.entities().at("site-annotation-a"),"symbols")==child(source.entities().at("site-annotation-a"),"symbols"),"properties retain sibling pinned artwork and opaque symbol data");oneEdit(window,source);
    require(window.selectEntity(a)&&window.selectEntity(b,true)&&window.selectEntity(token("site-annotation-a","repeated-symbol"),true)&&window.selectEntity(token("site-annotation-b","repeated-symbol"),true),"select equal label/symbol child IDs in two owners");source=window.document().snapshot();
    require(window.copySelection(),"actual typed Copy");require(document_snapshot_digest(window.document().snapshot())==document_snapshot_digest(source),"Copy never changes source or history");
    const auto payload=Json::parse(QGuiApplication::clipboard()->text().toStdString());require(payload.at("entities").size()==2,"clipboard retains two owner namespaces");
    require(window.pasteSelection(),"actual typed Paste");const auto pasted=window.document().snapshot();
    std::set<std::string> fresh_children;int count=0;
    for(const auto& [id,e]:pasted.entities())if(e.type==kAnnotationEntityType&&!source.entities().contains(id)) {
        ++count;const auto& raw=child(e,"labels");require(raw.at("id")!="repeated-child","paste assigns fresh child");
        require(fresh_children.insert(raw.at("id").get<std::string>()).second,"duplicate source child IDs become independent per owner");
        require(raw.at("vendor_label")==Json{{"retain","repeated-child"}},"opaque strings are never identity remapped");
        require(e.properties.at("presentation_frame")==Json{{"version",1},{"mode","building"}},"paste preserves explicit frame mode");
        require(e.properties.at("property_id")=="property-1"&&e.properties.at("building_id")=="building-1"&&e.properties.at("floor_id")=="floor-1"&&e.properties.at("layer_id")=="layer-1","paste admits complete destination carrier context");
        require(raw.at("placement").at("layer_id")=="layer-1","paste assigns child own destination layer");
        const auto& pasted_symbol=child(e,"symbols");
        require(fresh_children.insert(pasted_symbol.at("id").get<std::string>()).second,"pasted symbol identity is owner-local and fresh");
        require(pasted_symbol.at("pinned_svg")==symbol.at("pinned_svg")&&pasted_symbol.at("definition")==symbol.at("definition"),"Paste preserves exact pinned SVG and definition");
        require(pasted_symbol.at("svg_palette")==Json{{"version",1},{"profile","white-outline-2"},{"outline_color","#111111"},{"surface_color","#ffffff"}},"Paste preserves explicit palette");
        close(raw.at("placement").at("x").get<double>(),2,"paste retains authored local X representation");
        const std::array<SiteAnnotationTarget,1> targets{SiteAnnotationTarget{id,raw.at("id").get<std::string>()}};
        const auto placement=resolve_site_annotation_presentations(pasted,targets).at(targets.front());
        const auto world=site_transform_point({2,3,0},placement.forward);close(world.x,78,"pasted child uses explicit destination building frame");close(world.y,207,"pasted child frame Y");
    }
    require(count==2,"Paste creates two independent explicit carriers");oneEdit(window,source);
    require(window.selectEntity(a),"select original typed Delete");source=window.document().snapshot();
    require(window.deleteSelection(),"actual typed Delete action");const auto deleted=window.document().snapshot();
    require(deleted.entities().at("site-annotation-b")==source.entities().at("site-annotation-b"),"Delete preserves duplicate other owner exactly");
    require(deleted.entities().at("site-annotation-a").properties.at("state").at("labels").empty(),"Delete removes only selected child");
    require(child(deleted.entities().at("site-annotation-a"),"symbols")==child(source.entities().at("site-annotation-a"),"symbols"),"Delete retains owner sibling");oneEdit(window,source);
    require(window.undoCommand(),"restore typed child for Cut");require(window.selectEntity(a),"select typed Cut");source=window.document().snapshot();
    require(window.cutSelection(),"actual typed Cut action");require(window.document().snapshot().entities().at("site-annotation-b")==source.entities().at("site-annotation-b"),"Cut preserves other owner");oneEdit(window,source);
    require(window.undoCommand(),"restore symbol modal source");require(window.selectEntity(token("site-annotation-a","repeated-symbol")),"select typed symbol colors");source=window.document().snapshot();
    QTimer::singleShot(0,[] {auto* dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget());require(dialog&&dialog->objectName()=="symbolColorsDialog","actual typed palette modal");dialog->reject();});
    control<QPushButton>(window,"symbolColorsButton").click();require(document_snapshot_digest(window.document().snapshot())==document_snapshot_digest(source),"palette Cancel preserves complete source");
    QTimer::singleShot(0,[] {auto* dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget());require(dialog,"actual palette Apply dialog");control<QLineEdit>(*dialog,"symbolColorsOutline").setText("#224466");auto* buttons=dialog->findChild<QDialogButtonBox*>();require(buttons,"palette buttons");buttons->button(QDialogButtonBox::Apply)->click();});
    control<QPushButton>(window,"symbolColorsButton").click();
    require(window.document().snapshot().entities().at("site-annotation-b")==source.entities().at("site-annotation-b"),"palette Apply preserves duplicate other owner");
    require(child(window.document().snapshot().entities().at("site-annotation-a"),"symbols").at("svg_palette").at("outline_color")=="#224466","palette Apply uses typed symbol");oneEdit(window,source);
    source=window.document().snapshot();
    QTimer::singleShot(0,[] {auto* dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget());require(dialog,"actual palette no-op dialog");dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Apply)->click();});
    control<QPushButton>(window,"symbolColorsButton").click();require(document_snapshot_digest(window.document().snapshot())==document_snapshot_digest(source),"unchanged palette Apply is no-op");
    // Same identity/revision replacement while the displayed inspector remains.
    auto replacement=Document::fork_at_revision(source,source.revision()-1);std::vector<EntityChange> replacement_changes;
    for(const auto& [id,e]:source.entities()) {auto next=e;if(id=="site-annotation-b")next.extensions["new_head"]=true;replacement_changes.push_back(EntityChange::upsert(next));}
    replacement.apply(ApplyEntityChanges{replacement.revision(),replacement_changes,{},"Replace same revision annotation head"});
    window.document()=std::move(replacement);const auto stale=window.document().snapshot();
    require(stale.document_id()==source.document_id()&&stale.revision()==source.revision(),"stale fixture retains ID and revision");
    control<QLineEdit>(window,"annotationX").setText("123");control<QPushButton>(window,"applyAnnotation").click();
    require(document_snapshot_digest(window.document().snapshot())==document_snapshot_digest(stale),"Properties refuses same-ID/revision replaced source");
    const auto clipboard=QGuiApplication::clipboard()->text();require(!window.copySelection()&&!window.cutSelection()&&!window.deleteSelection()&&!window.pasteSelection(),"all clipboard/delete actions refuse stale displayed authority");
    require(QGuiApplication::clipboard()->text()==clipboard&&document_snapshot_digest(window.document().snapshot())==document_snapshot_digest(stale),"stale refusals preserve clipboard and all source history");
    // Fresh independent window adopts the replaced head through normal initial publication.
    MainWindow fresh(std::make_shared<Document>(Document::fork(stale)));displaySite(fresh);roundtrip(fresh);
}
void creationFrame() {
    MainWindow seed;seed.setMetricUnits(true);auto source=seed.document().snapshot();auto property=source.entities().at("property-1");
    property.properties["site_frame"]={{"version",1},{"origin_m",{100,200,30}},{"rotation_radians",std::numbers::pi/2},{"vertical_datum",{{"identifier","legacy-property"},{"height_at_origin_m",0}}}};
    auto doc=std::make_shared<Document>(Document::fork(source));doc->apply(ApplyEntityChanges{source.revision(),{EntityChange::upsert(property)},{},"Nonidentity property with legacy building"});
    MainWindow window(doc);displaySite(window);auto& canvas=control<PlanCanvas>(window,"architecturalPlanCanvas");canvas.setViewTransform({96,203},60);
    TextLibraryEntry entry{"frame-text","Frame text","Fixture","Exact presented click",{}};
    source=window.document().snapshot();require(window.beginTextPlacement(entry),"actual Site text placement arm");click(canvas,{97,202});wait(window,source.revision()+1);
    auto saved=window.document().snapshot();std::string label_owner,label_id;
    for(const auto& [id,e]:saved.entities())if(e.type==kAnnotationEntityType&&e.properties.contains("presentation_frame"))for(const auto& raw:e.properties.at("state").at("labels"))if(raw.at("content")==entry.content){label_owner=id;label_id=raw.at("id").get<std::string>();close(raw.at("placement").at("x").get<double>(),2,"text click inverse matches explicit V3 carrier X");close(raw.at("placement").at("y").get<double>(),3,"text click inverse matches explicit V3 carrier Y");}
    require(!label_owner.empty(),"explicit text carrier persists");
    const auto label=std::find_if(canvas.labels().begin(),canvas.labels().end(),[&](const auto& value){return value.id==token(label_owner,label_id);});require(label!=canvas.labels().end(),"created Site label rendered");close(label->position.x,97,"legacy-building text remains under click X");close(label->position.y,202,"legacy-building text remains under click Y");
    // Actual drag/drop event through the desktop canvas callback.
    QMimeData mime;
    // Resolve the actual SVG-backed library entry rather than a legacy family alias.
    const auto& catalog=default_symbol_catalog();const auto sofa=std::find_if(catalog.begin(),catalog.end(),[](const auto& d){return d.id=="svg-v2-04_living-sofa-three-seat"&&d.svg_asset.has_value();});require(sofa!=catalog.end(),"actual SVG sofa catalog entry");
    mime.setData("application/x-vertex-symbol",QByteArray::fromStdString(Json{{"id",sofa->id},{"scale",1}}.dump()));
    const auto pixel=screen(canvas,{95,204});QDragEnterEvent enter(pixel.toPoint(),Qt::CopyAction,&mime,Qt::LeftButton,Qt::NoModifier);QApplication::sendEvent(&canvas,&enter);
    source=window.document().snapshot();QDropEvent drop(pixel,Qt::CopyAction,&mime,Qt::LeftButton,Qt::NoModifier);QApplication::sendEvent(&canvas,&drop);QApplication::processEvents();wait(window,source.revision()+1);
    saved=window.document().snapshot();bool found=false;for(const auto& [id,e]:saved.entities())if(e.type==kAnnotationEntityType&&e.properties.contains("presentation_frame"))for(const auto& raw:e.properties.at("state").at("symbols")){const std::array<SiteAnnotationTarget,1> targets{SiteAnnotationTarget{id,raw.at("id").get<std::string>()}};const auto f=resolve_site_annotation_presentations(saved,targets).at(targets.front());const auto p=raw.at("placement");const auto world=site_transform_point({p.at("x").get<double>(),p.at("y").get<double>(),0},f.forward);close(world.x,95,"symbol drop carrier agrees with displayed X");close(world.y,204,"symbol drop carrier agrees with displayed Y");found=true;}
    require(found,"actual drop creates persisted Site symbol");roundtrip(window);
    // Actual library activation signal arms the production placement workflow.
    auto& list=control<QListWidget>(window,"symbolLibraryItems");QListWidgetItem chosen;chosen.setText("Sofa");chosen.setData(Qt::UserRole,QString::fromStdString(sofa->id));
    require(QMetaObject::invokeMethod(&list,"itemActivated",Qt::DirectConnection,Q_ARG(QListWidgetItem*,&chosen)),"actual library activation");
    source=window.document().snapshot();click(canvas,{93,206});wait(window,source.revision()+1);
    saved=window.document().snapshot();bool clicked=false;for(const auto& [id,e]:saved.entities())if(e.type==kAnnotationEntityType&&e.properties.contains("presentation_frame"))for(const auto& raw:e.properties.at("state").at("symbols")) {
        bool old=false;const auto prior=source.entities().find(id);if(prior!=source.entities().end())for(const auto& p:prior->second.properties.at("state").at("symbols"))if(p.at("id")==raw.at("id"))old=true;
        if(old)continue;const std::array<SiteAnnotationTarget,1> targets{SiteAnnotationTarget{id,raw.at("id").get<std::string>()}};const auto f=resolve_site_annotation_presentations(saved,targets).at(targets.front());const auto p=raw.at("placement");const auto world=site_transform_point({p.at("x").get<double>(),p.at("y").get<double>(),0},f.forward);close(world.x,93,"armed symbol click uses actual V3 inverse X");close(world.y,206,"armed symbol click uses actual V3 inverse Y");clicked=true;
    }
    require(clicked,"armed canvas click persists symbol");roundtrip(window);
}
void legacyClipboard() {
    MainWindow seed;const auto id=seed.createAnnotationLabel(QString::fromStdString(default_label_templates().front().id),"Legacy world child",{9,11});require(!id.isEmpty(),"ordinary V1/V2 label creation");
    const auto source=seed.document().snapshot();std::string owner;
    for(const auto& [key,e]:source.entities())if(e.type==kAnnotationEntityType)for(const auto& raw:e.properties.at("state").at("labels"))if(raw.at("id")==id.toStdString())owner=key;
    require(!owner.empty()&&!source.entities().at(owner).properties.contains("presentation_frame"),"ordinary creation remains legacy world");
    MainWindow window(std::make_shared<Document>(Document::fork(source)));displaySite(window);require(window.selectEntity(token(owner,id.toStdString())),"Site selects explicit owner for legacy world child");
    const auto before=window.document().snapshot();require(window.copySelection(),"copy legacy world in Site");require(document_snapshot_digest(window.document().snapshot())==document_snapshot_digest(before),"legacy Copy does not enroll source");
    require(window.pasteSelection(),"paste legacy world in Site");const auto after=window.document().snapshot();bool found=false;
    for(const auto& [key,e]:after.entities())if(e.type==kAnnotationEntityType&&!before.entities().contains(key)) {
        require(!e.properties.contains("presentation_frame")&&e.properties.at("version")==source.entities().at(owner).properties.at("version"),"legacy Paste preserves V1/V2 representation without frame enrollment");
        close(child(e,"labels").at("placement").at("x").get<double>(),9,"legacy Paste retains world X");
        require(e.extensions==source.entities().at(owner).extensions,"legacy Paste retains opaque owner metadata");found=true;
    }
    require(found&&after.entities().at(owner)==before.entities().at(owner),"legacy Paste creates new owner and preserves source");oneEdit(window,before);roundtrip(window);
}
void renderIdentityCollisions() {
    auto f=fixture();auto source=f.document->snapshot();
    auto annotation=source.entities().at("site-annotation-b");annotation.id="a";
    annotation.properties["state"]["labels"][0]["id"]="b";
    AnnotationState symbols;SymbolInstance symbol;symbol.id="c";
    const auto catalog=default_symbol_catalog();require(!catalog.empty(),"collision symbol catalog");
    symbol.symbol_id=catalog.front().id;symbol.definition=catalog.front();
    symbol.placement.position={4,6};symbol.placement.layer_id="layer-1";symbols.symbols.push_back(symbol);
    const auto encoded=encode_annotation_state(symbols,catalog);
    annotation.properties["state"]["symbols"]=encoded.at("symbols");
    annotation.properties["state"]["version"]=std::max(annotation.properties["state"]["version"].get<int>(),encoded.at("version").get<int>());
    validate_annotation_entity(annotation);
    f.document->apply(ApplyEntityChanges{source.revision(),{EntityChange::upsert(annotation)},{},"Seed collision annotation owner"});
    source=f.document->snapshot();const auto label=token("a","b"),symbol_token=token("a","c");
    auto root=source.entities().at(f.wall1.toStdString());root.id=label.toStdString();
    auto suffix_root=root;suffix_root.id=(label+":").toStdString();
    auto symbol_root=root;symbol_root.id=symbol_token.toStdString();
    f.document->apply(ApplyEntityChanges{source.revision(),{EntityChange::upsert(root),EntityChange::upsert(suffix_root),EntityChange::upsert(symbol_root)},{},"Add presentation token model collisions"});
    MainWindow window(f.document);displaySite(window);auto& canvas=control<PlanCanvas>(window,"architecturalPlanCanvas");
    const auto label_alias=label+"::",symbol_alias=symbol_token+":";
    require(std::any_of(canvas.labels().begin(),canvas.labels().end(),[&](const auto& item){return item.id==label_alias;}),"label skips canonical and suffixed persisted model IDs");
    require(item(canvas,symbol_alias).type=="symbol","symbol uses the same collision allocator");
    require(window.selectEntity(label_alias)&&window.selectedEntityId()==label_alias,"collision-resolved label survives selection refresh");
    source=window.document().snapshot();control<QLineEdit>(window,"annotationContent").setText("Edited alias child");
    control<QPushButton>(window,"applyAnnotation").click();auto edited=window.document().snapshot();
    require(child(edited.entities().at("a"),"labels").at("content")=="Edited alias child","alias Properties resolves captured annotation owner");
    require(edited.entities().at(root.id)==source.entities().at(root.id)&&edited.entities().at(suffix_root.id)==source.entities().at(suffix_root.id)&&edited.entities().at(symbol_root.id)==source.entities().at(symbol_root.id),"alias edit preserves every colliding model exactly");
    oneEdit(window,source);require(window.selectedEntityId()==label_alias,"alias label survives edit Undo and Redo");
    require(window.selectEntity(symbol_alias,true),"collision-resolved symbol joins label selection");
    source=window.document().snapshot();require(window.copySelection(),"Copy resolves both captured aliases");
    const auto payload=Json::parse(QGuiApplication::clipboard()->text().toStdString());
    require(payload.at("root_ids")==Json::array({"a"})&&payload.at("entities").size()==1,"annotation aliases copy the carrier rather than colliding models");
    require(payload.at("entities").at(0).at("properties").at("state").at("labels").size()==1&&payload.at("entities").at(0).at("properties").at("state").at("symbols").size()==1,"Copy retains selected label and symbol");
    require(document_snapshot_digest(window.document().snapshot())==document_snapshot_digest(source),"collision Copy preserves complete source and history");
    // Undo the edit before traversing the separately seeded collision command.
    require(window.undoCommand(),"Undo alias edit for collision history");
    require(window.undoCommand(),"Undo removes colliding model roots");
    require(window.selectedEntityIds()==QStringList{label,symbol_token},"typed label and symbol selection follows aliases after model collision removal");
    require(window.selectedEntityId()==symbol_token&&item(canvas,symbol_token).selected,"new alias is selected in the published canvas");
    require(window.copySelection(),"rebased alias selection keeps current publication authority");
    require(window.redoCommand(),"Redo restores colliding models");
    require(window.selectedEntityIds()==QStringList{label_alias,symbol_alias}&&window.selectedEntityId()==symbol_alias&&item(canvas,symbol_alias).selected,"typed multi-selection follows changed aliases after model collision addition");
    require(window.copySelection(),"collision addition publishes matching selection authority");
    require(window.selectEntity(label),"persisted model whose ID has the annotation prefix selects normally");
    source=window.document().snapshot();require(window.copySelection(),"prefixed persisted model Copy bypasses only stale-token rejection");
    const auto model_payload=Json::parse(QGuiApplication::clipboard()->text().toStdString());
    require(model_payload.at("root_ids")==Json::array({root.id})&&model_payload.at("entities").at(0).at("id")==root.id,"prefixed model Copy has exact persisted root authority");
    require(document_snapshot_digest(window.document().snapshot())==document_snapshot_digest(source),"prefixed model Copy leaves annotation and model history unchanged");
    require(window.selectEntity(label_alias),"return to annotation before retired-target history");
    // A separate captured history replaces a child with a model at its old alias.
    // The fixture prepares history before publication, so no stale head is adopted.
    auto retired=source.entities().at("a");retired.properties["state"]["labels"]=Json::array();
    auto takeover=root;takeover.id=label_alias.toStdString();
    auto history=std::make_shared<Document>(Document::fork(source));
    history->apply(ApplyEntityChanges{source.revision(),{EntityChange::upsert(retired),EntityChange::upsert(takeover)},{},"Retire annotation with alias model collision"});
    history->undo(history->revision());MainWindow retirement(history);displaySite(retirement);
    require(retirement.selectEntity(label_alias),"select child before its prepared retirement history");
    require(retirement.redoCommand()&&retirement.selectedEntityIds().isEmpty()&&retirement.selectedEntityId().isEmpty(),"retired typed child never retargets a model with its old render alias");
}

void historySelectionLiveness() {
    auto f=fixture();MainWindow window(f.document);displaySite(window);
    const auto before=window.document().snapshot();
    const auto created=window.createAnnotationLabel(QString::fromStdString(default_label_templates().front().id),
        "Temporary group member",{2,3});
    require(!created.isEmpty(),"actual Site label creates a typed group member");
    const auto survivor=token("site-annotation-b","repeated-child");
    require(window.selectEntity(created)&&window.selectEntity(survivor,true)&&
        window.selectedEntityIds()==QStringList{created,survivor},"new child is non-primary in a real multi-selection");
    require(window.undoCommand()&&window.document().snapshot().entities()==before.entities(),
        "Undo removes created child while preserving the primary owner's exact source");
    require(window.selectedEntityIds()==QStringList{survivor}&&window.selectedEntityId()==survivor,
        "history prunes every missing child without discarding the surviving primary");
    const auto undone=window.document().snapshot();require(window.copySelection(),"surviving typed group remains usable for Copy");
    require(document_snapshot_digest(window.document().snapshot())==document_snapshot_digest(undone),
        "Copy after group pruning preserves full source and history");
    require(window.redoCommand()&&window.selectedEntityIds()==QStringList{survivor},
        "Redo never reintroduces a retired token into the retained selection");
}
}
int main(int argc,char** argv) {
    sketch::testing::noninteractive_errors();QStandardPaths::setTestModeEnabled(true);QApplication app(argc,argv);
    QCoreApplication::setApplicationName("Vertex-site-annotation-test-"+QUuid::createUuid().toString(QUuid::WithoutBraces));
    try{lifecycle();creationFrame();legacyClipboard();renderIdentityCollisions();historySelectionLiveness();std::cout<<"Site annotation lifecycle passed\n";return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
