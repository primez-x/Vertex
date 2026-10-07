// Install beside site_plan_workflow_desktop_tests.cpp; reuse its actual window
// and native input helpers without changing the independently owned fixture.
#define main site_plan_workflow_unused_main
#include "site_plan_workflow_desktop_tests.cpp"
#undef main
#include "sketch/site_frame.hpp"
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QLineEdit>
#include <QListWidget>
#include <QMimeData>
#include <QPushButton>
#include <array>

namespace {
struct OpeningFrameFixture {
    std::shared_ptr<Document> document;
    QString wall;
    double translation;
};
OpeningFrameFixture openingFixture(const std::string& mode) {
    MainWindow seed;seed.setMetricUnits(true);
    const auto wall=seed.createStraightWall({20,.04},{24,.04});
    require(!wall.isEmpty(),"real thin host fixture created");
    const auto source=seed.document().snapshot();
    auto document=std::make_shared<Document>(Document::fork(source));
    auto property=source.entities().at("property-1");
    property.properties["site_frame"]={{"version",1},{"origin_m",{100,0,0}},{"rotation_radians",0},
        {"vertical_datum",{{"identifier","opening-fixture"},{"height_at_origin_m",0}}}};
    auto building=source.entities().at("building-1");
    building.properties["site_placement"]={{"version",1},{"translation_m",{10,0,0}},{"rotation_radians",0}};
    auto host=source.entities().at(wall.toStdString());
    if(mode!="inherited")host.properties["presentation_frame"]={{"version",1},{"mode",mode}};
    document->apply(ApplyEntityChanges{source.revision(),{EntityChange::upsert(property),
        EntityChange::upsert(building),EntityChange::upsert(host)}, {},"Explicit independent host placement fixture"});
    return {document,wall,mode=="world" ? 0.0 : mode=="site" ? 100.0 : 110.0};
}
PlanCanvas& showSite(MainWindow& window,const OpeningFrameFixture& f) {
    window.setMetricUnits(true);window.setAttribute(Qt::WA_DontShowOnScreen);
    window.resize(1300,900);window.show();require(window.setActiveLayer("layer-1"),"opening active layer");
    window.setWorkspace(Workspace::architectural);control<QAction>(window,"sitePlan").trigger();
    QApplication::processEvents();auto& canvas=control<PlanCanvas>(window,"architecturalPlanCanvas");
    canvas.setOverviewMapEnabled(false);canvas.setGridEnabled(true);canvas.setSnapEnabled(true);
    canvas.setViewTransform({27+f.translation,.04},40);
    const auto& host=item(canvas,f.wall);
    require(host.snap_points.size()==2,"displayed physical host baseline exists");
    close(host.snap_points.front().x,20+f.translation,"independent displayed host X");
    close(host.snap_points.front().y,.04,"independent displayed host Y");
    return canvas;
}
void armOpening(MainWindow& window,const char* kind) {
    control<QPushButton>(window,(std::string("library")+kind).c_str()).click();
    QApplication::processEvents();
    require(control<QAction>(window,"sitePlan").isChecked()&&
        control<PlanCanvas>(window,"architecturalPlanCanvas").isVisible(),"palette keeps the actual Site workspace active");
    control<QLineEdit>(window,"openingDrawWidth").setText("0.8 m");
}
std::string newOpening(const DocumentSnapshot& before,const DocumentSnapshot& after) {
    std::string result;
    for(const auto& [id,entity]:after.entities())if(entity.type=="opening"&&!before.entities().contains(id)) {
        require(result.empty(),"one placement creates exactly one hosted opening");result=id;
    }
    require(!result.empty(),"actual pointer placement creates a semantic opening");return result;
}
void checkOpening(const DocumentSnapshot& before,const DocumentSnapshot& after,
    const OpeningFrameFixture& f,double expected_offset,const char* expected_kind) {
    const auto id=newOpening(before,after);const auto& opening=after.entities().at(id);
    require(opening.properties.at("wall_id")==f.wall.toStdString(),"opening binds the exact physical host");
    close(opening.properties.at("offset_m").get<double>(),expected_offset,"placement offset is host-local metres");
    require(opening.properties.at("opening_kind")==expected_kind,"opening retains requested semantic kind");
    const std::array<std::string,2> ids{f.wall.toStdString(),id};const auto frames=resolve_site_presentations(after,ids);
    require(frames.at(id).source_frame==frames.at(f.wall.toStdString()).source_frame,
        "opening retains captured physical-host source frame");
    require(frames.at(id).drawing_context==frames.at(f.wall.toStdString()).drawing_context,
        "opening retains physical host ownership context");
    require(after.entities().at(f.wall.toStdString())==before.entities().at(f.wall.toStdString()),
        "opening leaves host geometry, source frame and opaque properties exact");
    for(const auto& [owner,entity]:before.entities())require(after.entities().at(owner)==entity,
        "opening command preserves all pre-existing source owners");
}
void previewOnHost(const PlanCanvas& canvas,double translation) {
    require(canvas.boundaryDraftPreview()&&!canvas.boundaryDraftPreview()->segments.empty(),"native opening hover supplies actual assembly preview");
    for(const auto& edge:canvas.boundaryDraftPreview()->segments)for(const auto p:{edge.start,edge.end})
        require(p.x>20+translation&&p.x<24+translation,"opening preview uses exact host presentation rather than active building");
}
void paletteFrames() {
    for(const std::string mode:{"world","site","inherited"})for(const char* kind:{"Doorway","Door","Window"}) {
        const auto f=openingFixture(mode);MainWindow window(f.document);auto& canvas=showSite(window,f);
        armOpening(window,kind);const auto before=window.document().snapshot();
        const Vec2 target{22.13+f.translation,.04};
        mouse(canvas,QEvent::MouseMove,target);previewOnHost(canvas,f.translation);
        require(document_snapshot_digest(window.document().snapshot())==document_snapshot_digest(before),"hover never changes full source or history");
        escape(canvas);require(document_snapshot_digest(window.document().snapshot())==document_snapshot_digest(before),"Cancel preserves exact hosted-placement source");
        armOpening(window,kind);
        // This is the old falsely accepted pointer: active-building inverse
        // turns it into the raw explicit-world/site host baseline.
        if(mode!="inherited") {
            click(canvas,{target.x+10,target.y});
            require(document_snapshot_digest(window.document().snapshot())==document_snapshot_digest(before),"empty displaced point refuses without geometry/history mutation");
        }
        click(canvas,target);wait(window,before.revision()+1);
        const auto placed=window.document().snapshot();
        const auto semantic=std::string(kind)=="Doorway" ? "opening" : std::string(kind)=="Door" ? "door" : "window";
        checkOpening(before,placed,f,1.73,semantic);
        require(window.undoCommand()&&window.document().snapshot().entities()==before.entities(),"one-command Undo restores exact host and owners");
        require(window.redoCommand()&&window.document().snapshot().entities()==placed.entities(),"Redo retains exact offset and host frame");
        QTemporaryDir directory;require(directory.isValid(),"opening roundtrip folder");
        const auto path=directory.filePath("site-opening.bldproj");
        require(window.saveProjectAs(path)&&window.openProject(path),"actual semantic Site opening saves and reopens editable");
        require(window.document().snapshot().entities()==placed.entities(),"save/reopen preserves exact hosted-opening properties and host frame");
    }
}
void libraryOpening(MainWindow& window,const QString& id) {
    auto& category=control<QComboBox>(window,"annotationSymbolCategory");
    for(int i=0;i<category.count();++i)if(category.itemData(i).toString()=="09_doors")category.setCurrentIndex(i);
    control<QLineEdit>(window,"annotationSymbolSearch").clear();QApplication::processEvents();
    auto& list=control<QListWidget>(window,"symbolLibraryItems");
    for(int i=0;i<list.count();++i)if(list.item(i)->data(Qt::UserRole).toString()==id) {
        list.setCurrentItem(list.item(i));
        require(QMetaObject::invokeMethod(&list,"itemActivated",Qt::DirectConnection,Q_ARG(QListWidgetItem*,list.item(i))),"activate real hosted library item");
        QApplication::processEvents();return;
    }
    throw std::runtime_error("real hosted catalog item available");
}
void dropOpening(PlanCanvas& canvas,const QString& id,Vec2 target) {
    QMimeData data;data.setData("application/x-vertex-symbol",QByteArray::fromStdString(Json{{"id",id.toStdString()},{"scale",1.0}}.dump()));
    const auto pixel=screen(canvas,target);
    QDragEnterEvent enter(pixel.toPoint(),Qt::CopyAction,&data,Qt::LeftButton,Qt::NoModifier);
    QApplication::sendEvent(&canvas,&enter);require(enter.isAccepted(),"real host catalog drag enters Site canvas");
    QDropEvent drop(pixel,Qt::CopyAction,&data,Qt::LeftButton,Qt::NoModifier);
    QApplication::sendEvent(&canvas,&drop);QApplication::processEvents();require(drop.isAccepted(),"real Site catalog drop is dispatched");
}
void libraryFrames() {
    const QString id="svg-v2-09_doors-door-hinged-910-left";
    for(const std::string mode:{"world","site","inherited"})for(const bool drop:{false,true}) {
        const auto f=openingFixture(mode);MainWindow window(f.document);auto& canvas=showSite(window,f);
        const auto before=window.document().snapshot();const Vec2 target{22.13+f.translation,.04};
        if(drop)dropOpening(canvas,id,target);
        else {libraryOpening(window,id);mouse(canvas,QEvent::MouseMove,target);previewOnHost(canvas,f.translation);click(canvas,target);}
        wait(window,before.revision()+1);const auto placed=window.document().snapshot();
        const auto opening=newOpening(before,placed);const auto width=placed.entities().at(opening).properties.at("width_m").get<double>();
        checkOpening(before,placed,f,2.13-width*.5,"door");
        require(placed.entities().at(opening).properties.at("catalog_symbol_id")==id.toStdString(),"library retains exact catalog semantic profile");
        require(window.undoCommand()&&window.document().snapshot().entities()==before.entities(),"catalog click/drop is one Undo");
        if(drop&&mode!="inherited") {
            const auto empty=window.document().snapshot();dropOpening(canvas,id,{target.x+10,target.y});
            require(document_snapshot_digest(window.document().snapshot())==document_snapshot_digest(empty),"catalog drop at displaced empty point refuses exact source");
        }
    }
}
void staleOpening() {
    for(const bool history_only:{false,true}) {
        const auto f=openingFixture("world");MainWindow window(f.document);auto& canvas=showSite(window,f);
        armOpening(window,"Doorway");mouse(canvas,QEvent::MouseMove,{22.13,.04});previewOnHost(canvas,0);
        const auto replacement=replaceDisplayedHead(window,f.wall,history_only);
        click(canvas,{22.13,.04});
        require(document_snapshot_digest(window.document().snapshot())==document_snapshot_digest(replacement),"same-ID/revision replacement refuses pending hosted placement");
        require(!window.lastError().isEmpty(),"stale hosted source refuses visibly");
    }
    {
        const auto f=openingFixture("world");MainWindow window(f.document);auto& canvas=showSite(window,f);
        const auto replacement=replaceDisplayedHead(window,f.wall,true);
        control<QPushButton>(window,"libraryDoor").click();
        require(!window.lastError().isEmpty(),"palette refuses stale publication before selection clearing can regenerate it");
        require(document_snapshot_digest(window.document().snapshot())==document_snapshot_digest(replacement),"stale palette arming preserves exact replacement source");
        (void)canvas;
    }
    {
        const auto f=openingFixture("world");MainWindow window(f.document);auto& canvas=showSite(window,f);
        armOpening(window,"Doorway");const auto before=window.document().snapshot();
        require(window.selectEntity(f.wall),"change selection after pending host source capture");
        click(canvas,{22.13,.04});
        require(document_snapshot_digest(window.document().snapshot())==document_snapshot_digest(before)&&!window.lastError().isEmpty(),
            "refreshed selection cannot reauthorize captured opening source context");
    }
    for(const bool units:{false,true}) {
        const auto f=openingFixture("world");MainWindow window(f.document);auto& canvas=showSite(window,f);
        const auto layer=window.createLayer("floor-1","Alternative placement layer");require(!layer.isEmpty(),"stale context fixture layer");
        armOpening(window,"Doorway");const auto before=window.document().snapshot();
        if(units)window.setMetricUnits(false);else require(window.setActiveLayer(layer),"change pending opening context");
        click(canvas,{22.13,.04});
        require(document_snapshot_digest(window.document().snapshot())==document_snapshot_digest(before),"pending host placement refuses changed units or owner layer");
    }
}
void rotatedHosts() {
    for(const std::string mode:{"world","site","inherited"}) {
        auto f=openingFixture(mode);const auto seeded=f.document->snapshot();
        auto property=seeded.entities().at("property-1");property.properties["site_frame"]["rotation_radians"]=std::numbers::pi/2;
        auto building=seeded.entities().at("building-1");building.properties["site_placement"]["rotation_radians"]=std::numbers::pi/2;
        f.document->apply(ApplyEntityChanges{seeded.revision(),{EntityChange::upsert(property),EntityChange::upsert(building)}, {},"Independent quarter-turn host frames"});
        // Independent F: world identity, site R90+(100,0), building
        // R180+(100,10). No production frame resolver computes these targets.
        const auto presented=[&](Vec2 p)->Vec2 {
            if(mode=="world")return p;
            if(mode=="site")return {100-p.y,p.x};
            return {100-p.x,10-p.y};
        };
        MainWindow window(f.document);window.setMetricUnits(true);window.setAttribute(Qt::WA_DontShowOnScreen);
        window.resize(1300,900);window.show();window.setWorkspace(Workspace::architectural);
        control<QAction>(window,"sitePlan").trigger();QApplication::processEvents();
        auto& canvas=control<PlanCanvas>(window,"architecturalPlanCanvas");canvas.setOverviewMapEnabled(false);canvas.setSnapEnabled(true);
        const auto target=presented({22.13,.04});canvas.setViewTransform(target,40);
        armOpening(window,"Doorway");const auto before=window.document().snapshot();
        mouse(canvas,QEvent::MouseMove,target);
        require(canvas.boundaryDraftPreview()&&canvas.boundaryDraftPreview()->segments.size()==4,"rotated bare doorway has four physical jamb edges");
        const std::array<Vec2,4> expected{presented({21.73,.11}),presented({22.53,.11}),presented({22.53,-.03}),presented({21.73,-.03})};
        for(const auto p:expected)require(std::any_of(canvas.boundaryDraftPreview()->segments.begin(),canvas.boundaryDraftPreview()->segments.end(),[&](const auto& edge) {
            return std::hypot(p.x-edge.start.x,p.y-edge.start.y)<1e-6;
        }),"native preview rotates through exact host frame once");
        click(canvas,target);wait(window,before.revision()+1);checkOpening(before,window.document().snapshot(),f,1.73,"opening");
        require(window.undoCommand()&&window.document().snapshot().entities()==before.entities(),"rotated source placement retains one-command Undo");
    }
}
}
int main(int argc,char** argv) {
    sketch::testing::noninteractive_errors();QStandardPaths::setTestModeEnabled(true);QApplication app(argc,argv);
    QCoreApplication::setApplicationName("Vertex-site-opening-test-"+QUuid::createUuid().toString(QUuid::WithoutBraces));
    try {paletteFrames();libraryFrames();rotatedHosts();staleOpening();std::cout<<"Site opening frame desktop workflow passed\n";return 0;}
    catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
