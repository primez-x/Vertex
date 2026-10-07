#include "sketch/desktop/main_window.hpp"
#include "sketch/annotation_entity_codec.hpp"
#include "sketch/assembly_document_adapter.hpp"
#include "sketch/document_digest.hpp"
#include "sketch/reference_grid.hpp"
#include "sketch/terrain_surface.hpp"
#include "../src/desktop/plan_canvas.hpp"
#include "support/noninteractive_errors.hpp"

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QElapsedTimer>
#include <QKeyEvent>
#include <QLineEdit>
#include <QImage>
#include <QMouseEvent>
#include <QPainter>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QToolButton>
#include <QUuid>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <iomanip>
#include <numbers>
#include <sstream>
#include <source_location>
#include <stdexcept>

namespace {
using namespace sketch;
using namespace sketch::desktop;
using Json=nlohmann::json;
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void close(double value,double expected,const char* message){
    if(std::abs(value-expected)<1e-6)return;
    std::ostringstream detail;detail<<message<<": expected "<<std::setprecision(17)<<expected<<", observed "<<value;
    throw std::runtime_error(detail.str());
}
template<class T>T& control(QObject& window,const char* name){
    for(auto* object:window.findChildren<QObject*>(QString::fromLatin1(name)))
        if(auto* result=dynamic_cast<T*>(object))return *result;
    throw std::runtime_error(name);
}
QPointF screen(const PlanCanvas& canvas,Vec2 point){
    const auto center=QRectF(canvas.rect()).center();const auto origin=canvas.viewCenter();
    return {center.x()+(point.x-origin.x)*canvas.viewScale(),center.y()-(point.y-origin.y)*canvas.viewScale()};
}
void mouse(PlanCanvas& canvas,QEvent::Type kind,Vec2 point,Qt::MouseButton button=Qt::NoButton,Qt::MouseButtons buttons=Qt::NoButton,Qt::KeyboardModifiers modifiers=Qt::NoModifier){
    const auto pixel=screen(canvas,point);
    QMouseEvent event(kind,pixel,canvas.mapToGlobal(pixel.toPoint()),button,buttons,modifiers);
    QApplication::sendEvent(&canvas,&event);QApplication::processEvents();
}
void click(PlanCanvas& canvas,Vec2 point){
    mouse(canvas,QEvent::MouseMove,point);mouse(canvas,QEvent::MouseButtonPress,point,Qt::LeftButton,Qt::LeftButton);
    mouse(canvas,QEvent::MouseButtonRelease,point,Qt::LeftButton);
}
void escape(PlanCanvas& canvas){QKeyEvent event(QEvent::KeyPress,Qt::Key_Escape,Qt::NoModifier);QApplication::sendEvent(&canvas,&event);QApplication::processEvents();}
void wait(MainWindow& window,Revision revision,
          const std::source_location location=std::source_location::current()){
    QElapsedTimer timer;timer.start();
    while(window.document().revision()!=revision&&timer.elapsed()<5000){QApplication::processEvents();QThread::msleep(1);}
    if(window.document().revision()!=revision)
        throw std::runtime_error("actual canvas commit at line "+std::to_string(location.line())+
            " expected revision "+std::to_string(revision)+" but observed "+
            std::to_string(window.document().revision())+": "+window.lastError().toStdString());
}
const CanvasEntity& item(const PlanCanvas& canvas,const QString& id){
    const auto it=std::find_if(canvas.entities().begin(),canvas.entities().end(),[&](const auto& value){return value.id==id;});
    require(it!=canvas.entities().end(),"Site Plan retains visible source owner");return *it;
}
Segment baseline(const Entity& entity){const auto& b=entity.properties.at("baseline");return {{b.at("start").at(0).get<double>(),b.at("start").at(1).get<double>()},{b.at("end").at(0).get<double>(),b.at("end").at(1).get<double>()},0};}
Boundary rectangle(){return {{{0,0},{1,0},0},{{1,0},{1,1},0},{{1,1},{0,1},0},{{0,1},{0,0},0}};}
struct Fixture{std::shared_ptr<Document> document;QString wall1,wall2,upper,building2,layer2,reference,opening,boundary;};
Fixture fixture(bool posed_reference=false){
    MainWindow seed;seed.setMetricUnits(true);
    Fixture f;f.wall1=seed.createStraightWall({0,0},{4,0});require(!f.wall1.isEmpty(),"first physical wall created");
    require(seed.selectEntity(f.wall1),"select opening host");f.opening=seed.createHostedOpening("door","1 m","1 m","0 m","2 m");
    require(!f.opening.isEmpty(),"semantic hosted opening fixture");
    f.building2=seed.createBuilding("property-1","Second building");
    const auto floor2=seed.createFloor(f.building2,"Second ground");f.layer2=seed.createLayer(floor2,"Second plan");
    require(seed.setActiveLayer(f.layer2),"activate second source frame");f.wall2=seed.createStraightWall({0,0},{0,3});
    const auto upperfloor=seed.createFloor("building-1","Upper level");const auto upperlayer=seed.createLayer(upperfloor,"Upper plan");
    require(seed.setActiveLayer(upperlayer),"activate upper visible floor");f.upper=seed.createStraightWall({0,5},{4,5});
    QTemporaryDir images;require(images.isValid(),"reference fixture folder");
    QImage image(posed_reference?160:8,posed_reference?90:6,QImage::Format_ARGB32);image.fill(posed_reference?QColor(30,110,210):QColor(Qt::white));
    if(posed_reference){QPainter ink(&image);ink.fillRect(3,7,37,21,Qt::yellow);ink.fillRect(112,53,41,29,QColor(220,40,90));}
    const auto path=images.filePath("reference.png");require(image.save(path),"fixture raster encodes");f.reference=seed.importReferenceImage(path);
    if(f.reference.isEmpty())throw std::runtime_error("real reference import produces source asset: "+seed.lastError().toStdString());
    if(posed_reference){
        require(seed.calibrateReference(f.reference,"0","0","100","0","1.3 m"),"real reference calibration command");
        require(seed.editReferenceTransform(f.reference,"12.3","-9.7","0.013","1.4","17","0.63",true,true,true),"real flipped reference pose command");
    }
    require(seed.setActiveLayer("layer-1"),"boundary source layer");
    f.boundary=seed.createBoundary({{{5,3},{7,3},0},{{7,3},{7,5},0},{{7,5},{5,5},0},{{5,5},{5,3},0}});
    require(!f.boundary.isEmpty(),"identified editable boundary fixture");
    auto source=seed.document().snapshot();f.document=std::make_shared<Document>(Document::fork(source));
    auto property=source.entities().at("property-1");
    property.properties["site_frame"]={{"version",1},{"origin_m",{100,200,30}},{"rotation_radians",std::numbers::pi/2},
        {"vertical_datum",{{"identifier","fixture-datum"},{"height_at_origin_m",1500}}}};
    auto building=source.entities().at("building-1");building.properties["site_placement"]={{"version",1},{"translation_m",{10,20,3}},{"rotation_radians",std::numbers::pi/2}};
    auto second=source.entities().at(f.building2.toStdString());second.properties["site_placement"]={{"version",1},{"translation_m",{-5,7,11}},{"rotation_radians",0}};
    auto reference=source.entities().at(f.reference.toStdString());reference.properties["property_id"]="property-1";reference.properties["building_id"]="building-1";
    reference.properties["floor_id"]="floor-1";reference.properties["layer_id"]="layer-1";reference.properties["presentation_frame"]={{"version",1},{"mode","site"}};
    if(posed_reference){
        property.properties["site_frame"]["origin_m"]={101.7,-42.3,30.2};property.properties["site_frame"]["rotation_radians"]=.41;
        second.properties["site_placement"]["rotation_radians"]=-.23;
        reference.properties["building_id"]=f.building2.toStdString();reference.properties["floor_id"]=floor2.toStdString();
        reference.properties["layer_id"]=f.layer2.toStdString();reference.properties["presentation_frame"]["mode"]="building";
    }
    const AnnotationEntityContext context{"property-1","building-1","floor-1","layer-1",{}};
    AnnotationState a;auto label=instantiate_label(default_label_templates().front(),"repeated-child");
    label.content="Own child frame";label.placement.position={2,3};label.placement.layer_id=f.layer2.toStdString();a.labels.push_back(label);
    auto annotation=make_annotation_entity("site-annotation-a",a,context);annotation.properties["version"]=3;
    annotation.properties["presentation_frame"]={{"version",1},{"mode","building"}};
    auto other=annotation;other.id="site-annotation-b";other.properties["state"]["labels"][0]["placement"]["layer_id"]="layer-1";
    ReferenceGridModel grid;grid.count_x=1;grid.count_y=1;
    Entity grid_entity{"site-grid","reference_grid",{{"model",grid.to_json()},{"property_id","property-1"},{"building_id","building-1"},{"floor_id","floor-1"},{"layer_id","layer-1"},
        {"presentation_frame",{{"version",1},{"mode","site"}}}}};
    TerrainSurface terrain("fixture",{{"a",0,0,1502},{"b",2,0,1503},{"c",0,2,1502}},{{{0,1,2}}},1,true);
    Entity terrain_entity{"site-terrain","terrain_surface",{{"model",terrain.to_json()},{"property_id","property-1"},{"building_id","building-1"},{"floor_id","floor-1"},{"layer_id","layer-1"},
        {"terrain_elevation_binding",{{"version",1},{"mode","declared_absolute"},{"datum_identifier","fixture-datum"}}}}};
    AssemblyType leaf{"leaf","Unit profile"};leaf.profiles.push_back({"profile",rectangle(),{},1,1,{}});
    AssemblyType nested{"nested","Nested module"};nested.parts.push_back({"member","leaf",{{2,0,1},.2,1}});
    Entity catalog{"site-catalog","assembly_model",{{"model",AssemblyModel::create({}, {leaf,nested},{}).to_json()}}};
    AssemblyInstance instance;instance.id="site-independent";instance.type_id="nested";instance.root_transform=AssemblyTransform{{8,0,3},.3,1};
    Entity assembly{"site-independent","assembly_instance",{{"property_id","property-1"},{"building_id","building-1"},{"floor_id","floor-1"},{"layer_id","layer-1"}}};
    assembly=encode_document_assembly_instance(assembly,{catalog.id,instance});
    auto enrolled=assembly;enrolled.id="site-enrolled";instance.id=enrolled.id;instance.root_transform=AssemblyTransform{{8,4,3},.3,1};
    enrolled=encode_document_assembly_instance(enrolled,{catalog.id,instance});enrolled.properties["presentation_frame"]={{"version",1},{"mode","building"}};
    f.document->apply(ApplyEntityChanges{source.revision(),{EntityChange::upsert(property),EntityChange::upsert(building),EntityChange::upsert(second),
        EntityChange::upsert(annotation),EntityChange::upsert(other),EntityChange::upsert(grid_entity),EntityChange::upsert(terrain_entity),EntityChange::upsert(catalog),
        EntityChange::upsert(assembly),EntityChange::upsert(enrolled),EntityChange::upsert(reference)}, {},"Seed independent source-frame fixtures"});
    return f;
}
DocumentSnapshot replaceDisplayedHead(MainWindow& window,const QString& wall,bool history_only){
    const auto captured=window.document().snapshot();const auto digest=document_snapshot_digest(captured);
    auto replacement=Document::fork_at_revision(captured,captured.revision()-1);
    std::vector<EntityChange> changes;
    for(const auto& [id,original]:captured.entities()){
        auto next=original;
        if(id==wall.toStdString()&&!history_only)next.extensions["concurrent_site_source"]=true;
        changes.push_back(EntityChange::upsert(std::move(next)));
    }
    replacement.apply(ApplyEntityChanges{replacement.revision(),std::move(changes),{},"Independent replacement of displayed Site history"});
    const auto result=replacement.snapshot();
    require(result.document_id()==captured.document_id()&&result.revision()==captured.revision(),"replacement retains displayed ID and revision");
    if(history_only)require(result.entities()==captured.entities(),"history-only replacement retains every head entity");
    require(document_snapshot_digest(result)!=digest,"replacement changes the complete retained source");
    require(document_snapshot_digest(captured)==digest,"independent replacement preserves immutable displayed snapshot");
    window.document()=std::move(replacement);return result;
}
void idleSiteCursor(){
    auto f=fixture();MainWindow window(f.document);window.setMetricUnits(true);window.setAttribute(Qt::WA_DontShowOnScreen);
    window.resize(1300,900);window.show();window.setWorkspace(Workspace::architectural);
    control<QAction>(window,"sitePlan").trigger();QApplication::processEvents();
    require(window.selectEntity("property-1")&&window.activeLayerId().isEmpty(),"property with several layers has no implicit authoring layer");
    auto& canvas=control<PlanCanvas>(window,"architecturalPlanCanvas");canvas.setViewTransform({65,195},60);
    const auto source=document_snapshot_digest(window.document().snapshot());
    mouse(canvas,QEvent::MouseMove,{60,190});
    require(window.lastError().isEmpty()&&document_snapshot_digest(window.document().snapshot())==source,
        "idle Site cursor without a drawing layer changes no source and raises no authoring error");
    click(canvas,{60,190});click(canvas,{60,190});
    require(!window.lastError().isEmpty()&&document_snapshot_digest(window.document().snapshot())==source,
        "actual Site drawing still refuses an incomplete source layer visibly without mutation");
}
void displayedAuthority(){
    // Capturing the newest request at press would silently authorize this
    // replacement against old hit geometry. Both full-source differences must
    // refuse before selection can refresh and conceal the obsolete publication.
    for(const bool history_only:{false,true})for(const bool before_press:{true,false}){
        auto f=fixture();MainWindow window(f.document);window.setMetricUnits(true);window.setAttribute(Qt::WA_DontShowOnScreen);
        window.resize(1300,900);window.show();require(window.setActiveLayer("layer-1"),"authority fixture active layer");
        window.setWorkspace(Workspace::architectural);control<QAction>(window,"sitePlan").trigger();QApplication::processEvents();
        auto& canvas=control<PlanCanvas>(window,"architecturalPlanCanvas");canvas.setOverviewMapEnabled(false);canvas.setSnapEnabled(false);canvas.setGridEnabled(false);
        require(window.selectEntity(f.wall1),"publish selected editable source");canvas.setViewTransform({78,210},60);
        const auto published=window.document().snapshot();const auto published_digest=document_snapshot_digest(published);
        if(!before_press)mouse(canvas,QEvent::MouseButtonPress,{79.5,210},Qt::LeftButton,Qt::LeftButton);
        const auto replacement=replaceDisplayedHead(window,f.wall1,history_only);
        const auto replacement_digest=document_snapshot_digest(replacement);
        if(before_press)mouse(canvas,QEvent::MouseButtonPress,{79.5,210},Qt::LeftButton,Qt::LeftButton);
        mouse(canvas,QEvent::MouseMove,{81.5,211},Qt::NoButton,Qt::LeftButton);
        mouse(canvas,QEvent::MouseButtonRelease,{81.5,211},Qt::LeftButton);
        require(document_snapshot_digest(window.document().snapshot())==replacement_digest,"stale publication gesture preserves exact replacement source and retained history");
        require(!window.lastError().isEmpty(),"stale publication refuses visibly");
        require(document_snapshot_digest(published)==published_digest,"stale gesture preserves captured displayed source");
        // A newly opened window explicitly publishes the replacement. Its real
        // gesture is legitimate and keeps physical coordinates in local metres.
        MainWindow fresh(std::make_shared<Document>(Document::fork(replacement)));fresh.setMetricUnits(true);fresh.setAttribute(Qt::WA_DontShowOnScreen);
        fresh.resize(1300,900);fresh.show();require(fresh.setActiveLayer("layer-1"),"fresh publication layer");fresh.setWorkspace(Workspace::architectural);
        control<QAction>(fresh,"sitePlan").trigger();QApplication::processEvents();require(fresh.selectEntity(f.wall1),"fresh publication selected root");
        auto& current=control<PlanCanvas>(fresh,"architecturalPlanCanvas");current.setOverviewMapEnabled(false);current.setSnapEnabled(false);current.setGridEnabled(false);current.setViewTransform({78,210},60);
        const auto before=fresh.document().snapshot();
        mouse(current,QEvent::MouseButtonPress,{79.5,210},Qt::LeftButton,Qt::LeftButton);
        mouse(current,QEvent::MouseMove,{81.5,211},Qt::NoButton,Qt::LeftButton);
        mouse(current,QEvent::MouseButtonRelease,{81.5,211},Qt::LeftButton);wait(fresh,before.revision()+1);
        const auto moved=baseline(fresh.document().snapshot().entities().at(f.wall1.toStdString()));
        close(moved.start.x,-2,"fresh publication drag maps to authored local X");close(moved.start.y,-1,"fresh publication drag maps to authored local Y");
        require(fresh.undoCommand()&&fresh.document().snapshot().entities()==before.entities(),"fresh publication edit has one-command Undo");
    }
    // A plain stale hit must not select then regenerate a new authority.
    auto f=fixture();MainWindow window(f.document);window.setMetricUnits(true);window.setAttribute(Qt::WA_DontShowOnScreen);window.resize(1300,900);window.show();
    require(window.setActiveLayer("layer-1"),"stale hit layer");window.setWorkspace(Workspace::architectural);control<QAction>(window,"sitePlan").trigger();QApplication::processEvents();
    auto& canvas=control<PlanCanvas>(window,"architecturalPlanCanvas");canvas.setOverviewMapEnabled(false);canvas.setViewTransform({78,210},60);
    require(window.selectEntity({}),"publish unselected stale hit fixture");const auto replacement=replaceDisplayedHead(window,f.wall1,true);
    click(canvas,{79.5,210});
    require(window.selectedEntityId().isEmpty(),"stale hit cannot select and refresh its replacement source");
    require(document_snapshot_digest(window.document().snapshot())==document_snapshot_digest(replacement)&&!window.lastError().isEmpty(),"stale unselected click refuses before selection refresh");
}
void readOnlyInspection(){
    auto f=fixture();const auto editable=f.document->snapshot();
    const auto editable_digest=document_snapshot_digest(editable);
    f.document->mark_read_only("Inspection regression");
    MainWindow window(f.document);window.setMetricUnits(true);
    window.setAttribute(Qt::WA_DontShowOnScreen);window.setAttribute(Qt::WA_ShowWithoutActivating);
    window.resize(1300,900);window.show();require(window.setActiveLayer("layer-1"),"read-only active layer");
    window.setWorkspace(Workspace::architectural);control<QAction>(window,"sitePlan").trigger();QApplication::processEvents();
    auto& canvas=control<PlanCanvas>(window,"architecturalPlanCanvas");canvas.setOverviewMapEnabled(false);
    canvas.setSnapEnabled(false);canvas.setGridEnabled(false);canvas.setTool(CanvasTool::select);canvas.setViewTransform({78,210},60);
    const auto digest=document_snapshot_digest(window.document().snapshot());
    click(canvas,{79.5,210});require(window.selectedEntityId()==f.wall1,"read-only Site click selects displayed wall");
    require(item(canvas,f.wall1).selected,"read-only selection is presented for inspection");
    mouse(canvas,QEvent::MouseButtonPress,{79.5,210},Qt::LeftButton,Qt::LeftButton,Qt::ControlModifier);
    mouse(canvas,QEvent::MouseButtonRelease,{79.5,210},Qt::LeftButton,Qt::NoButton,Qt::ControlModifier);
    require(window.selectedEntityIds().isEmpty(),"read-only Ctrl click toggles displayed selection");
    click(canvas,{79.5,210});require(window.copySelection(),"read-only displayed selection copies");
    mouse(canvas,QEvent::MouseButtonDblClick,{79.5,210},Qt::LeftButton,Qt::LeftButton);
    mouse(canvas,QEvent::MouseButtonRelease,{79.5,210},Qt::LeftButton);
    require(window.selectedEntityId()==f.wall1,"read-only double click retains inspection target");
    require(!control<QLineEdit>(window,"inspectorLength").isEnabled() &&
        !control<QLineEdit>(window,"inspectorHeight").isEnabled(),"read-only contextual properties disable mutation controls");
    require(!canvas.selectionRotationHandlePosition(),"read-only inspection exposes no rotation control");
    mouse(canvas,QEvent::MouseButtonPress,{79.5,210},Qt::LeftButton,Qt::LeftButton);
    mouse(canvas,QEvent::MouseMove,{81.5,211},Qt::NoButton,Qt::LeftButton);
    mouse(canvas,QEvent::MouseButtonRelease,{81.5,211},Qt::LeftButton);
    canvas.setTool(CanvasTool::wall);click(canvas,{70,205});click(canvas,{68,205});escape(canvas);
    const auto center=canvas.viewCenter();
    mouse(canvas,QEvent::MouseButtonPress,{79,209},Qt::MiddleButton,Qt::MiddleButton);
    mouse(canvas,QEvent::MouseMove,{80,210},Qt::NoButton,Qt::MiddleButton);
    mouse(canvas,QEvent::MouseButtonRelease,{80,210},Qt::MiddleButton);
    require(canvas.viewCenter().x!=center.x || canvas.viewCenter().y!=center.y,"read-only Site permits camera pan");
    require(document_snapshot_digest(window.document().snapshot())==digest,"read-only inspection refuses drag and drawing without history mutation");
    require(document_snapshot_digest(editable)==editable_digest,"inspection preserves captured editable source");
}
const CanvasReference& referenceItem(const PlanCanvas& canvas,const QString& id){
    const auto it=std::find_if(canvas.references().begin(),canvas.references().end(),[&](const auto& value){return value.id==id;});
    require(it!=canvas.references().end(),"real Site publication retains reference underlay");return *it;
}
Vec2 modelPoint(const PlanCanvas& canvas,QPointF pixel){
    const auto center=QRectF(canvas.rect()).center();const auto origin=canvas.viewCenter();
    return {origin.x+(pixel.x()-center.x())/canvas.viewScale(),origin.y-(pixel.y()-center.y())/canvas.viewScale()};
}
void referenceMouse(PlanCanvas& canvas,QEvent::Type type,QPointF pixel){
    QMouseEvent event(type,pixel,canvas.mapToGlobal(pixel.toPoint()),type==QEvent::MouseMove?Qt::NoButton:Qt::LeftButton,
        type==QEvent::MouseButtonRelease?Qt::NoButton:Qt::LeftButton,Qt::NoModifier);
    QApplication::sendEvent(&canvas,&event); // Keep queued MainWindow preview work deferred until explicitly drained.
}
enum class ReferenceGesture{move,rotate,scale};
std::optional<QRectF> relativeSelectionFrame(const PlanCanvas& canvas){
    const auto frame=canvas.selectionBounds();if(!frame)return std::nullopt;
    return frame->translated(-QRectF(canvas.rect()).center());
}
void referencePreviewReady(PlanCanvas& canvas,ReferenceGesture gesture){
    QElapsedTimer timer;timer.start();
    const auto pending=[&]{return gesture==ReferenceGesture::move?canvas.entitiesMovePreviewPending():canvas.entityTransformPreviewPending();};
    while(pending()&&timer.elapsed()<5000){QApplication::processEvents();QThread::msleep(1);}
    require(!pending(),"real MainWindow deferred reference preview finishes");QApplication::processEvents();
}
QImage referenceArtwork(PlanCanvas& canvas){
    // Capture paintEvent around the isolated underlay. Selection decoration and
    // bottom cursor/status text are UI feedback, not the reference artwork.
    canvas.setSelectionControlsVisible(false);
    QImage image(canvas.size(),QImage::Format_ARGB32_Premultiplied);image.fill(Qt::transparent);
    const auto exact_center=QRectF(canvas.rect()).center();const auto center=exact_center.toPoint();
    {
        // Showing the status bar after Save changes the viewport's height and
        // may change its half-pixel center. Compare the real paintEvent at the
        // same pixel phase without changing the camera or authored geometry.
        QPainter painter(&image);painter.translate(QPointF(center)-exact_center);canvas.render(&painter);
    }
    canvas.setSelectionControlsVisible(true);
    const QRect crop(center-QPoint(210,170),QSize(420,340));
    require(canvas.rect().contains(crop),"reference artwork crop fits actual desktop canvas");return image.copy(crop);
}
void referencePose(const CanvasReference& actual,const CanvasReference& source,const Entity& local){
    const auto& p=local.properties;const auto x=p.at("position_m").at(0).get<double>(),y=p.at("position_m").at(1).get<double>();
    // Independent property/building composition: Rp(.41) * Tb(-5,7,11)
    // followed by the authored reference point in Rp(.41)*Rb(-.23).
    const auto tx=101.7-5*std::cos(.41)-7*std::sin(.41),ty=-42.3-5*std::sin(.41)+7*std::cos(.41);
    close(actual.position.x,tx+x*std::cos(.18)-y*std::sin(.18),"reference publication applies own building X exactly once");
    close(actual.position.y,ty+x*std::sin(.18)+y*std::cos(.18),"reference publication applies own building Y exactly once");
    close(actual.rotation_degrees,p.at("rotation_degrees").get<double>()+.18*180/std::numbers::pi,"reference publication composes own asymmetric yaw");
    close(actual.scale,p.at("scale").get<double>(),"reference publication uses authored scale");
    require(actual.image==source.image&&actual.id==source.id,"reference publication retains exact decoded artwork and owner");
    close(actual.metres_per_source_unit,.013,"reference publication retains measured calibration");
    require(actual.flip_horizontal&&actual.flip_vertical&&actual.visible,"reference publication retains both flips and visibility");
    close(actual.intensity,.63,"reference publication retains intensity");
}
void referenceSourceChange(const DocumentSnapshot& before,const DocumentSnapshot& after,const QString& id,const Entity& expected){
    auto entities=before.entities();entities.at(id.toStdString())=expected;
    require(after.entities()==entities,"reference gesture changes only the exact permitted local pose; full source and frame contract retained");
    require(after.assets()==before.assets(),"reference gesture preserves every original and render Asset byte and hash");
    const auto& original=before.entities().at(id.toStdString());
    for(const auto* key:{"asset_id","render_asset_id"}){
        const auto& asset=after.assets().at(original.properties.at(key).get<std::string>());
        require(asset.sha256==sha256_hex(asset.bytes),"reference retained Asset hash matches exact bytes");
    }
}
std::pair<QPointF,QPointF> referenceGesturePoints(const PlanCanvas& canvas,const CanvasReference& source,ReferenceGesture gesture){
    const auto frame=canvas.selectionBounds();require(frame.has_value(),"real selected reference has public controls");
    const auto center=frame->center();
    close(center.x(),screen(canvas,source.position).x(),"selected reference frame has presented X center");
    close(center.y(),screen(canvas,source.position).y(),"selected reference frame has presented Y center");
    if(gesture==ReferenceGesture::move)return {center,center+QPointF(60,30)};
    if(gesture==ReferenceGesture::rotate){
        const auto handle=canvas.selectionRotationHandlePosition();require(handle.has_value(),"MainWindow enables actual reference rotation control");
        const auto v=*handle-center;const auto c=std::cos(-std::numbers::pi/4),s=std::sin(-std::numbers::pi/4);
        return {*handle,center+QPointF(c*v.x()-s*v.y(),s*v.x()+c*v.y())};
    }
    const auto angle=-source.rotation_degrees*std::numbers::pi/180;
    const QPointF corner(source.image.width()*source.metres_per_source_unit*source.scale*canvas.viewScale()/2+7.5,
        source.image.height()*source.metres_per_source_unit*source.scale*canvas.viewScale()/2+7.5);
    const auto start=center+QPointF(std::cos(angle)*corner.x()-std::sin(angle)*corner.y(),std::sin(angle)*corner.x()+std::cos(angle)*corner.y());
    return {start,center+(start-center)*1.25};
}
void configureReferenceWindow(MainWindow& window,const Fixture& f){
    window.setMetricUnits(true);window.setAttribute(Qt::WA_DontShowOnScreen);window.resize(1300,900);window.show();
    require(window.setActiveLayer("layer-1"),"reference lifecycle starts from another layer");
    window.setWorkspace(Workspace::architectural);control<QAction>(window,"sitePlan").trigger();QApplication::processEvents();
    require(window.selectEntity(f.reference),"real MainWindow selects the own-layer Site reference");
    require(window.activeLayerId()==f.layer2,"reference selection respects its actual own layer");
    control<QToolButton>(window,"gridTool").setChecked(false);control<QToolButton>(window,"snapTool").setChecked(false);
    control<QToolButton>(window,"overviewMapTool").setChecked(false);
    auto& canvas=control<PlanCanvas>(window,"architecturalPlanCanvas");
    const auto presented=referenceItem(canvas,f.reference);canvas.setViewTransform(presented.position,70);QApplication::processEvents();
    require(presented.selected&&canvas.selectionBounds().has_value()&&canvas.selectionRotationHandlePosition().has_value(),
        "MainWindow publication enables actual selected reference frame and rotation control");
}
void referenceDocumentLifecycle(){
    auto f=fixture(true);const auto seed=f.document->snapshot();
    MainWindow window(std::make_shared<Document>(Document::fork(seed)));configureReferenceWindow(window,f);
    auto& canvas=control<PlanCanvas>(window,"architecturalPlanCanvas");const auto pinned=referenceItem(canvas,f.reference);
    require(pinned.image.width()==160&&pinned.image.height()==90&&pinned.image.pixelColor(4,8)==QColor(Qt::yellow)&&
        pinned.image.pixelColor(120,60)==QColor(220,40,90),"actual imported asymmetric artwork remains recognizable");
    referencePose(pinned,pinned,seed.entities().at(f.reference.toStdString()));
    for(const auto gesture:{ReferenceGesture::move,ReferenceGesture::rotate,ReferenceGesture::scale}){
        require(window.selectEntity(f.reference),"publish reference before independent lifecycle gesture");
        const auto before=window.document().snapshot();const auto digest=document_snapshot_digest(before);
        const auto retained=referenceItem(canvas,f.reference);const auto baseline_art=referenceArtwork(canvas);
        const auto [start,end]=referenceGesturePoints(canvas,retained,gesture);
        const auto repeat_state=[&](const char* stage){
            std::ostringstream detail;const auto view=canvas.viewCenter();const auto frame=canvas.selectionBounds();const auto handle=canvas.selectionRotationHandlePosition();
            detail<<std::setprecision(17)<<stage<<" canvas="<<canvas.width()<<'x'<<canvas.height()
                <<" view="<<view.x<<','<<view.y<<" scale="<<canvas.viewScale()
                <<" start="<<start.x()<<','<<start.y()<<" end="<<end.x()<<','<<end.y()
                <<" frame=";if(frame)detail<<frame->x()<<','<<frame->y()<<','<<frame->width()<<','<<frame->height();else detail<<"none";
            detail<<" rotation_handle=";if(handle)detail<<handle->x()<<','<<handle->y()<<" start_distance="<<std::hypot(start.x()-handle->x(),start.y()-handle->y());else detail<<"none";
            detail<<" selected="<<window.selectedEntityId().toStdString()<<" reference_selected="<<referenceItem(canvas,f.reference).selected
                <<" layer="<<window.activeLayerId().toStdString()<<" workspace="<<static_cast<int>(window.workspace())
                <<" site="<<control<QAction>(window,"sitePlan").isChecked()<<" revision="<<window.document().revision()
                <<" move_pending="<<canvas.entitiesMovePreviewPending()<<" move_serial="<<canvas.entitiesMovePreviewSerial()
                <<" transform_pending="<<canvas.entityTransformPreviewPending()<<" transform_serial="<<canvas.entityTransformPreviewSerial()
                <<" source_unchanged="<<(document_snapshot_digest(window.document().snapshot())==digest)
                <<" error="<<window.lastError().toStdString();return detail.str();
        };
        const auto initial_gesture_state=repeat_state("initial gesture geometry");
        referenceMouse(canvas,QEvent::MouseButtonPress,start);
        referenceMouse(canvas,QEvent::MouseMove,start+(end-start)*.6);
        const auto first_serial=gesture==ReferenceGesture::move?canvas.entitiesMovePreviewSerial():canvas.entityTransformPreviewSerial();
        referenceMouse(canvas,QEvent::MouseMove,end);
        require((gesture==ReferenceGesture::move?canvas.entitiesMovePreviewSerial():canvas.entityTransformPreviewSerial())>first_serial,"real mouse motion supersedes queued preview serial");
        referencePreviewReady(canvas,gesture);
        require(document_snapshot_digest(window.document().snapshot())==digest,"reference preview leaves complete source snapshot and retained history unchanged");
        referencePose(referenceItem(canvas,f.reference),pinned,before.entities().at(f.reference.toStdString()));
        const auto preview_art=referenceArtwork(canvas);const auto preview_frame=relativeSelectionFrame(canvas);
        require(preview_art!=baseline_art,"real reference preview changes asymmetric artwork");
        escape(canvas);referenceMouse(canvas,QEvent::MouseButtonRelease,end);QApplication::processEvents();
        require(document_snapshot_digest(window.document().snapshot())==digest,"reference Escape and release add no command or history");
        require(referenceArtwork(canvas)==baseline_art,"reference cancellation restores exact posed artwork");
        // Cancellation may change the viewport layout. Repeat the gesture at
        // its current visible controls, preserving the same relative motion.
        const auto [repeat_start,repeat_end]=referenceGesturePoints(canvas,referenceItem(canvas,f.reference),gesture);
        const auto before_repeat_state=repeat_state("before repeated press");
        referenceMouse(canvas,QEvent::MouseButtonPress,repeat_start);const auto after_repeat_press_state=repeat_state("after repeated press");
        referenceMouse(canvas,QEvent::MouseMove,repeat_end);
        const auto repeat_pending=gesture==ReferenceGesture::move?canvas.entitiesMovePreviewPending():canvas.entityTransformPreviewPending();
        if(!repeat_pending){
            const auto name=gesture==ReferenceGesture::move?"move":gesture==ReferenceGesture::rotate?"rotate":"scale";
            std::cerr<<"Site repeated reference preview gesture="<<name<<'\n'<<initial_gesture_state<<'\n'
                <<before_repeat_state<<'\n'<<after_repeat_press_state<<'\n'<<repeat_state("after repeated move")<<'\n';
        }
        require(repeat_pending,"MainWindow owns a deferred reference candidate before cancellation");
        escape(canvas);referenceMouse(canvas,QEvent::MouseButtonRelease,repeat_end);referencePreviewReady(canvas,gesture);
        require(document_snapshot_digest(window.document().snapshot())==digest&&referenceArtwork(canvas)==baseline_art,
            "Escape invalidates queued MainWindow reference candidate without source/history/artwork mutation");
        // Repeat the same actual gesture, then release while its final preview
        // is queued. MainWindow must finish its captured Document candidate.
        const auto [commit_start,commit_end]=referenceGesturePoints(canvas,referenceItem(canvas,f.reference),gesture);
        referenceMouse(canvas,QEvent::MouseButtonPress,commit_start);referenceMouse(canvas,QEvent::MouseMove,commit_end);referencePreviewReady(canvas,gesture);
        require(referenceArtwork(canvas)==preview_art,"fresh legitimate reference preview is deterministic after cancellation");
        referenceMouse(canvas,QEvent::MouseButtonRelease,commit_end);wait(window,before.revision()+1);
        const auto committed=window.document().snapshot();auto expected=before.entities().at(f.reference.toStdString());
        if(gesture==ReferenceGesture::move){
            const auto from=modelPoint(canvas,commit_start),to=modelPoint(canvas,commit_end);
            const Vec2 delta{to.x-from.x,to.y-from.y};
            expected.properties["position_m"]={expected.properties.at("position_m").at(0).get<double>()+delta.x*std::cos(.18)+delta.y*std::sin(.18),
                expected.properties.at("position_m").at(1).get<double>()-delta.x*std::sin(.18)+delta.y*std::cos(.18)};
        }else if(gesture==ReferenceGesture::rotate){
            const auto initial=retained.rotation_degrees*std::numbers::pi/180;
            const auto final=std::round((initial+std::numbers::pi/4)/(std::numbers::pi/4))*(std::numbers::pi/4);
            expected.properties["rotation_degrees"]=expected.properties.at("rotation_degrees").get<double>()+(final-initial)*180/std::numbers::pi;
        }else expected.properties["scale"]=expected.properties.at("scale").get<double>()*1.25;
        // JSON double arithmetic may differ in its final ULP; check numerical
        // pose independently, then compare every other source field exactly.
        const auto& actual=committed.entities().at(f.reference.toStdString());
        for(const auto* key:{"scale","rotation_degrees"})close(actual.properties.at(key).get<double>(),expected.properties.at(key).get<double>(),"reference committed scalar matches real handle gesture");
        for(int i=0;i<2;++i)close(actual.properties.at("position_m").at(i).get<double>(),expected.properties.at("position_m").at(i).get<double>(),"reference committed local position matches captured inverse frame");
        expected.properties["position_m"]=actual.properties.at("position_m");expected.properties["scale"]=actual.properties.at("scale");expected.properties["rotation_degrees"]=actual.properties.at("rotation_degrees");
        referenceSourceChange(before,committed,f.reference,expected);referencePose(referenceItem(canvas,f.reference),pinned,actual);
        require(referenceArtwork(canvas)==preview_art,"real MainWindow reference preview and committed artwork agree exactly");
        const auto committed_frame=relativeSelectionFrame(canvas);
        require(preview_frame&&committed_frame,"reference preview and commit retain public selection controls");
        close(committed_frame->x(),preview_frame->x(),"reference preview and committed frame relative X agree");
        close(committed_frame->y(),preview_frame->y(),"reference preview and committed frame relative Y agree");
        close(committed_frame->width(),preview_frame->width(),"reference preview and committed frame width agree");
        close(committed_frame->height(),preview_frame->height(),"reference preview and committed frame height agree");
        require(document_snapshot_digest(before)==digest,"reference commit never mutates immutable captured source");
        require(window.undoCommand(),"one-command reference Undo");
        require(window.document().snapshot().entities()==before.entities()&&window.document().snapshot().assets()==before.assets(),"reference Undo restores exact source entities and Asset hashes");
        require(referenceArtwork(canvas)==baseline_art,"reference Undo restores exact presented artwork");
        require(window.redoCommand(),"one-command reference Redo");
        require(window.document().snapshot().entities()==committed.entities()&&window.document().snapshot().assets()==committed.assets(),"reference Redo restores exact committed source and assets");
        require(referenceArtwork(canvas)==preview_art,"reference Redo restores exact committed artwork");
        const auto saved_center=canvas.viewCenter();const auto saved_scale=canvas.viewScale();
        const auto render_state=[&](const char* stage){
            std::ostringstream detail;const auto rect=canvas.rect();const auto center=QRectF(rect).center();const auto rounded=center.toPoint();const auto view=canvas.viewCenter();
            detail<<std::setprecision(17)<<stage<<" canvas="<<rect.width()<<'x'<<rect.height()
                <<" center="<<center.x()<<','<<center.y()<<" crop_phase="<<center.x()-rounded.x()<<','<<center.y()-rounded.y()
                <<" view="<<view.x<<','<<view.y<<" scale="<<canvas.viewScale()
                <<" workspace="<<static_cast<int>(window.workspace())<<" site="<<control<QAction>(window,"sitePlan").isChecked()
                <<" layer="<<window.activeLayerId().toStdString()<<" grid="<<control<QToolButton>(window,"gridTool").isChecked()
                <<" snap="<<control<QToolButton>(window,"snapTool").isChecked()<<" map="<<control<QToolButton>(window,"overviewMapTool").isChecked();
            return detail.str();
        };
        const auto saved_render_state=render_state("before save/reopen");
        QTemporaryDir directory;require(directory.isValid(),"reference lifecycle save folder");const auto path=directory.filePath("posed-site-reference.bldproj");
        require(window.saveProjectAs(path)&&window.openProject(path),"real Site reference saves and reopens");
        require(window.document().snapshot().entities()==committed.entities()&&window.document().snapshot().assets()==committed.assets(),"reopened reference preserves exact local source/frame/calibration/flips/intensity/layer/assets");
        require(window.selectEntity(f.reference),"reopened reference remains selectable");
        const auto selected_render_state=render_state("after reopen/reselect");
        canvas.setViewTransform(saved_center,saved_scale);referencePose(referenceItem(canvas,f.reference),pinned,actual);
        const auto before_capture_state=render_state("before reopened paint capture");
        const auto reopened_art=referenceArtwork(canvas);
        if(reopened_art!=preview_art){
            const auto name=gesture==ReferenceGesture::move?"move":gesture==ReferenceGesture::rotate?"rotate":"scale";
            std::size_t changed=0;QRect changed_bounds;
            for(int y=0;y<std::min(reopened_art.height(),preview_art.height());++y)
                for(int x=0;x<std::min(reopened_art.width(),preview_art.width());++x)
                    if(reopened_art.pixel(x,y)!=preview_art.pixel(x,y)){++changed;changed_bounds=changed_bounds.united(QRect(x,y,1,1));}
            std::cerr<<"Site reference roundtrip gesture="<<name<<'\n'<<saved_render_state<<'\n'<<selected_render_state<<'\n'
                <<before_capture_state<<'\n'<<render_state("after reopened paint capture")
                <<"\ncrop before="<<preview_art.width()<<'x'<<preview_art.height()<<" reopened="<<reopened_art.width()<<'x'<<reopened_art.height()
                <<" changed_pixels="<<changed<<" diff_bbox="<<changed_bounds.x()<<','<<changed_bounds.y()<<','<<changed_bounds.width()<<','<<changed_bounds.height()<<'\n';
            if(const auto captures=qEnvironmentVariable("VERTEX_TEST_CAPTURE_DIR");!captures.isEmpty()){
                QDir output(captures);if(output.mkpath(".")){
                    const auto stem=QStringLiteral("site-reference-roundtrip-%1").arg(QString::fromLatin1(name));
                    const auto saved_png=preview_art.save(output.filePath(stem+QStringLiteral("-before.png")));
                    const auto reopened_png=reopened_art.save(output.filePath(stem+QStringLiteral("-reopened.png")));
                    std::cerr<<"roundtrip PNG captures saved="<<saved_png<<','<<reopened_png<<" directory="<<captures.toStdString()<<'\n';
                }
            }
        }
        require(reopened_art==preview_art,"save/reopen retains exact Site reference artwork");
    }
    for(const auto gesture:{ReferenceGesture::move,ReferenceGesture::rotate,ReferenceGesture::scale})
        for(const bool history_only:{false,true})for(const bool before_press:{true,false}){
            MainWindow stale(std::make_shared<Document>(Document::fork(seed)));configureReferenceWindow(stale,f);
            auto& view=control<PlanCanvas>(stale,"architecturalPlanCanvas");const auto retained=referenceItem(view,f.reference);
            const auto [start,end]=referenceGesturePoints(view,retained,gesture);const auto published=stale.document().snapshot();const auto published_digest=document_snapshot_digest(published);
            if(!before_press){referenceMouse(view,QEvent::MouseButtonPress,start);referenceMouse(view,QEvent::MouseMove,start+(end-start)*.6);}
            const auto replacement=replaceDisplayedHead(stale,f.reference,history_only);const auto replacement_digest=document_snapshot_digest(replacement);
            if(before_press)referenceMouse(view,QEvent::MouseButtonPress,start);
            referenceMouse(view,QEvent::MouseMove,end);referenceMouse(view,QEvent::MouseButtonRelease,end);referencePreviewReady(view,gesture);
            require(document_snapshot_digest(stale.document().snapshot())==replacement_digest,"stale reference gesture preserves exact same-ID/revision replacement including history");
            require(!stale.lastError().isEmpty(),"stale reference publication or captured gesture refuses visibly");
            require(document_snapshot_digest(published)==published_digest,"stale reference gesture preserves immutable published source");
            MainWindow fresh(std::make_shared<Document>(Document::fork(replacement)));configureReferenceWindow(fresh,f);
            auto& current=control<PlanCanvas>(fresh,"architecturalPlanCanvas");const auto [fresh_start,fresh_end]=referenceGesturePoints(current,referenceItem(current,f.reference),gesture);
            const auto before=fresh.document().snapshot();referenceMouse(current,QEvent::MouseButtonPress,fresh_start);referenceMouse(current,QEvent::MouseMove,fresh_end);
            referencePreviewReady(current,gesture);require(document_snapshot_digest(fresh.document().snapshot())==document_snapshot_digest(before),"fresh replacement preview also preserves complete source");
            const auto artwork=referenceArtwork(current);referenceMouse(current,QEvent::MouseButtonRelease,fresh_end);wait(fresh,before.revision()+1);
            require(referenceArtwork(current)==artwork,"fresh reference gesture commits the same presented replacement preview");
            require(fresh.undoCommand()&&fresh.document().snapshot().entities()==before.entities()&&fresh.document().snapshot().assets()==before.assets(),"fresh gesture after stale refusal remains usable and has exact one-command Undo");
        }
}
void workflow(){
    auto f=fixture();MainWindow window(f.document);window.setMetricUnits(true);window.setAttribute(Qt::WA_DontShowOnScreen);window.resize(1300,900);window.show();
    require(window.setActiveLayer("layer-1"),"active property source scope");window.setWorkspace(Workspace::architectural);QApplication::processEvents();
    control<QToolButton>(window,"overviewMapTool").setChecked(false);control<QToolButton>(window,"gridTool").setChecked(false);
    auto& snap=control<QToolButton>(window,"snapTool");snap.setChecked(false);
    auto& canvas=control<PlanCanvas>(window,"architecturalPlanCanvas");
    const auto ordinary_before=item(control<PlanCanvas>(window,"measurementPlanCanvas"),f.wall1).segments;
    auto& combo=control<QComboBox>(window,"architecturalView");const auto old_index=combo.currentIndex();const auto source=window.document().snapshot();
    auto& site=control<QAction>(window,"sitePlan");site.trigger();QApplication::processEvents();
    require(site.isChecked()&&combo.currentIndex()==old_index,"Site Plan has separate state and preserves named/canonical indexing");
    require(!snap.isChecked(),"Site publication retains the app's free mouse drawing preference");
    const auto& first=item(canvas,f.wall1);require(first.snap_points.size()==2,"presented physical baseline snap points exist");
    close(first.snap_points[0].x,80,"site composition translates first building X");close(first.snap_points[0].y,210,"site composition translates first building Y");
    close(first.snap_points[1].x,76,"property plus building yaw applied exactly once");
    item(canvas,f.wall2);item(canvas,f.upper);item(canvas,"site-terrain");item(canvas,"site-independent");item(canvas,"site-enrolled");
    const auto reference=std::find_if(canvas.references().begin(),canvas.references().end(),[&](const auto& value){return value.id==f.reference;});
    require(reference!=canvas.references().end(),"property Site Plan includes explicitly framed reference");close(reference->position.x,100,"reference explicit site frame X");close(reference->position.y,200,"reference explicit site frame Y");
    require(!canvas.referenceGrids().empty(),"Site Plan contains framed reference grid");
    const auto a=std::find_if(canvas.labels().begin(),canvas.labels().end(),[](const auto& value){return value.id.contains("site-annotation-a");});
    const auto b=std::find_if(canvas.labels().begin(),canvas.labels().end(),[](const auto& value){return value.id.contains("site-annotation-b");});
    require(a!=canvas.labels().end()&&b!=canvas.labels().end()&&a->id!=b->id,"equal annotation child IDs retain typed render targets");
    close(a->position.x,90,"annotation uses child own second-building layer X");close(a->position.y,197,"annotation uses child own second-building layer Y");
    close(b->position.x,78,"other owner child uses first building inverse namespace");
    require(document_snapshot_digest(window.document().snapshot())==document_snapshot_digest(source),"presentation never mutates source or measurement facts");
    require(window.selectEntity(f.opening),"select opening in presented building");canvas.setViewTransform({78,210},60);
    const auto timer_source=window.document().snapshot();
    require(QMetaObject::invokeMethod(&control<QTimer>(window,"workspaceSavePoll"),"timeout",Qt::DirectConnection),
        "actual autosave polling callback runs before Site editing");
    require(document_snapshot_digest(window.document().snapshot())==document_snapshot_digest(timer_source) &&
        window.selectedEntityId()==f.opening,"autosave recovery mirroring preserves complete displayed source and selection");
    const auto opening_source=window.document().snapshot();const auto controls=*item(canvas,f.opening).opening_width_controls;
    mouse(canvas,QEvent::MouseButtonPress,controls.end_jamb,Qt::LeftButton,Qt::LeftButton);
    mouse(canvas,QEvent::MouseMove,{controls.end_jamb.x-1,controls.end_jamb.y},Qt::NoButton,Qt::LeftButton);
    mouse(canvas,QEvent::MouseButtonRelease,{controls.end_jamb.x-1,controls.end_jamb.y},Qt::LeftButton);wait(window,opening_source.revision()+1);
    close(window.document().snapshot().entities().at(f.opening.toStdString()).properties.at("width_m").get<double>(),2,"opening width uses local scalar without frame scale");
    require(window.undoCommand(),"opening resize Undo restores source width");
    require(window.selectEntity(f.boundary),"select identified site boundary");canvas.setViewTransform({75,207},60);
    const auto corner_source=window.document().snapshot();const auto handle=item(canvas,f.boundary).vertex_handles.front();
    mouse(canvas,QEvent::MouseButtonPress,handle.position,Qt::LeftButton,Qt::LeftButton);
    mouse(canvas,QEvent::MouseMove,{handle.position.x+.5,handle.position.y+.5},Qt::NoButton,Qt::LeftButton);
    mouse(canvas,QEvent::MouseButtonRelease,{handle.position.x+.5,handle.position.y+.5},Qt::LeftButton);wait(window,corner_source.revision()+1);
    const auto& corner=item(canvas,f.boundary).vertex_handles.front();close(corner.position.x,handle.position.x+.5,"vertex edit projects exact inverse absolute target X");close(corner.position.y,handle.position.y+.5,"vertex edit projects exact inverse absolute target Y");
    require(window.undoCommand(),"corner edit Undo restores identified source topology");
    require(window.selectEntity(f.wall1),"select actual physical root");canvas.setViewTransform({78,210},60);QApplication::processEvents();
    const auto before=window.document().snapshot();
    mouse(canvas,QEvent::MouseButtonPress,{79.5,210},Qt::LeftButton,Qt::LeftButton);
    mouse(canvas,QEvent::MouseMove,{81.5,211},Qt::NoButton,Qt::LeftButton);
    mouse(canvas,QEvent::MouseButtonRelease,{81.5,211},Qt::LeftButton);wait(window,before.revision()+1);
    const auto moved=baseline(window.document().snapshot().entities().at(f.wall1.toStdString()));
    close(moved.start.x,-2,"world drag delta inversely rotates to local X");close(moved.start.y,-1,"world drag delta inversely rotates to local Y");
    require(window.undoCommand()&&window.document().snapshot().entities()==before.entities(),"Undo restores source geometry and frame contracts");
    require(window.redoCommand(),"Redo reuses one authoritative source command");
    require(window.undoCommand(),"return to source for cancellation");const auto cancelled=window.document().snapshot();
    require(window.selectEntity(f.wall1),"select root before cancel");mouse(canvas,QEvent::MouseButtonPress,{79.5,210},Qt::LeftButton,Qt::LeftButton);
    mouse(canvas,QEvent::MouseMove,{80.5,213},Qt::NoButton,Qt::LeftButton);escape(canvas);
    require(document_snapshot_digest(window.document().snapshot())==document_snapshot_digest(cancelled),"Escape abandons exact Site Plan preview without history");
    require(window.selectEntity(f.wall1)&&window.selectEntity(f.wall2,true),"select roots in different source frames");
    const auto mixed=window.document().snapshot();mouse(canvas,QEvent::MouseButtonPress,{79.5,210},Qt::LeftButton,Qt::LeftButton);
    mouse(canvas,QEvent::MouseMove,{81.5,211},Qt::NoButton,Qt::LeftButton);mouse(canvas,QEvent::MouseButtonRelease,{81.5,211},Qt::LeftButton);
    require(document_snapshot_digest(window.document().snapshot())==document_snapshot_digest(mixed),"mixed source frames refuse without distorted geometry or history");
    require(window.activeLayerId()==f.layer2,"mixed source selection follows the second wall's actual layer");
    require(window.selectEntity({}),"clear selection before new wall drawing");
    require(window.activeLayerId()==f.layer2,"clearing selection preserves the active drawing layer");
    require(window.setActiveLayer("layer-1")&&window.activeLayerId()=="layer-1","choose the first building's source layer before drawing");
    require(!snap.isChecked(),"Site edits retain the app's free mouse drawing preference");
    canvas.setViewTransform({72,205},60);const auto draw_source=window.document().snapshot();
    click(canvas,{70,205});click(canvas,{68,205});escape(canvas);wait(window,draw_source.revision()+1);
    const auto created_source=window.document().snapshot();
    std::string created;for(const auto& [id,e]:created_source.entities())if(e.type=="wall"&&!draw_source.entities().contains(id))created=id;
    require(!created.empty(),"actual empty canvas clicks create a physical wall in Site mode");
    const auto& created_wall=created_source.entities().at(created);
    require(created_wall.properties.at("layer_id")=="layer-1"&&created_wall.properties.at("floor_id")=="floor-1","new Site wall owns the explicitly chosen source layer and floor");
    const auto local=baseline(created_wall);close(local.start.x,10,"new wall inverse absolute X");close(local.start.y,5,"new wall inverse absolute Y");
    close(local.end.x,12,"new wall end inverse absolute X");close(local.end.y,5,"new wall end inverse absolute Y");
    close(segment_length(local),2,"site placement preserves authored physical length");
    require(!snap.isChecked(),"Site wall commit retains the app's free mouse drawing preference");
    QTemporaryDir directory;require(directory.isValid(),"roundtrip directory");const auto file=directory.filePath("site.bldproj");
    const auto saved=window.document().snapshot();require(window.saveProjectAs(file)&&window.openProject(file),"editable site source saves and reopens");
    require(window.document().snapshot().entities()==saved.entities(),"roundtrip retains local model geometry and explicit frames");
    if(site.isChecked())site.trigger();QApplication::processEvents();
    const auto& ordinary=control<PlanCanvas>(window,"measurementPlanCanvas");
    const auto& local_first=item(ordinary,f.wall1);require(local_first.segments.size()==ordinary_before.size(),"ordinary floor path retains source edge count");
    for(std::size_t i=0;i<ordinary_before.size();++i){close(local_first.segments[i].start.x,ordinary_before[i].start.x,"ordinary floor remains in authored X frame");close(local_first.segments[i].start.y,ordinary_before[i].start.y,"ordinary floor remains in authored Y frame");}
}
}
int main(int argc,char** argv){
    sketch::testing::noninteractive_errors();QStandardPaths::setTestModeEnabled(true);QApplication app(argc,argv);
    QCoreApplication::setApplicationName("Vertex-site-plan-test-"+QUuid::createUuid().toString(QUuid::WithoutBraces));
    try {
        const auto scenario = [](const char* name, auto&& run) {
            std::cout << "[scenario start] " << name << std::endl;
            run();
            std::cout << "[scenario end] " << name << std::endl;
        };
        scenario("idleSiteCursor", idleSiteCursor);
        scenario("displayedAuthority", displayedAuthority);
        scenario("readOnlyInspection", readOnlyInspection);
        scenario("workflow", workflow);
        scenario("referenceDocumentLifecycle", referenceDocumentLifecycle);
        std::cout << "Site Plan desktop workflow passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
