#include "sketch/desktop/main_window.hpp"
#include "sketch/annotation_entity_codec.hpp"
#include "sketch/sheet_view_entity_codec.hpp"
#include "sketch/site_frame.hpp"
#include "../src/desktop/plan_canvas.hpp"
#include "support/noninteractive_errors.hpp"

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QLineEdit>
#include <QMouseEvent>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QUuid>
#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>

namespace {
using namespace sketch;
using namespace sketch::desktop;
constexpr double pi=3.14159265358979323846, wall_angle=pi/12, frame_angle=pi/6;
const Vec2 source_tangent{std::cos(wall_angle),std::sin(wall_angle)};
const Vec2 source_normal{-source_tangent.y,source_tangent.x};
Vec2 operator+(Vec2 a,Vec2 b){return {a.x+b.x,a.y+b.y};}
Vec2 operator-(Vec2 a,Vec2 b){return {a.x-b.x,a.y-b.y};}
Vec2 operator*(Vec2 a,double b){return {a.x*b,a.y*b};}
double dot(Vec2 a,Vec2 b){return a.x*b.x+a.y*b.y;}
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
bool close_enough(double a,double b){return std::abs(a-b)<1e-6;}
bool same(Vec2 a,Vec2 b){return close_enough(a.x,b.x)&&close_enough(a.y,b.y);}
bool same_axis(double a,double b){return close_enough(std::remainder(a-b,pi),0);}
template<class Predicate> void await(Predicate ready,const char* message){
    QElapsedTimer timer;timer.start();
    do{QApplication::processEvents(QEventLoop::AllEvents,20);if(ready())return;}
    while(timer.elapsed()<5000);
    require(false,message);
}
template<class T> T& control(MainWindow& window,const char* name){
    auto* result=dynamic_cast<T*>(window.findChild<QObject*>(name));
    require(result,"frame fixture control missing");return *result;
}
const CanvasEntity& item(const PlanCanvas& canvas,const QString& id){
    const auto& values=canvas.entities();
    const auto it=std::find_if(values.begin(),values.end(),[&](const auto& e){return e.id==id;});
    require(it!=values.end(),"frame entity absent from published canvas");return *it;
}
SymbolInstance persisted(MainWindow& window,const SiteAnnotationTarget& target){
    const auto snapshot=window.document().snapshot();
    for(const auto& s:decode_annotation_entity(snapshot.entities().at(target.owner_entity_id)).symbols)
        if(s.id==target.child_id)return s;
    throw std::runtime_error("typed persisted sofa missing");
}
QPointF screen(const PlanCanvas& canvas,Vec2 p){
    const auto center=canvas.viewCenter();
    return QRectF(canvas.rect()).center()+QPointF((p.x-center.x)*canvas.viewScale(),
                                                -(p.y-center.y)*canvas.viewScale());
}
void mouse(PlanCanvas& canvas,QEvent::Type type,Vec2 p){
    const auto pixel=screen(canvas,p);
    QMouseEvent event(type,pixel,canvas.mapToGlobal(pixel.toPoint()),
        type==QEvent::MouseMove?Qt::NoButton:Qt::LeftButton,
        type==QEvent::MouseButtonRelease?Qt::NoButton:Qt::LeftButton,Qt::NoModifier);
    QApplication::sendEvent(&canvas,&event);
}
void capture(MainWindow& window,const QString& name){
    const auto dir=qEnvironmentVariable("VERTEX_TEST_CAPTURE_DIR");
    if(!dir.isEmpty())require(QDir().mkpath(dir)&&window.grab().save(QDir(dir).filePath(name)),
                            "frame preview capture failed");
}

// Two actual named-plan bases (including reflection), and one building-bound
// Site carrier. Angles are source +15; displayed axes are -15, -165, and +45.
void frameCase(bool site,double direction_z=-1){
    MainWindow seed;seed.setMetricUnits(true);seed.setWorkspace(Workspace::architectural);
    control<QLineEdit>(seed,"wallDrawThickness").setText("200 mm");
    const auto wall=seed.createStraightWall(source_tangent*(-4),source_tangent*4);
    const Vec2 original{1.2,2.5};
    const auto child=seed.createAnnotationSymbol("svg-v2-04_living-sofa-three-seat",original);
    require(!wall.isEmpty()&&!child.isEmpty(),"genuine wall/sofa fixture failed");
    const auto source=seed.document().snapshot();
    require(close_enough(source.entities().at(wall.toStdString()).properties.at("thickness_m").get<double>(),.2),
            "physical source wall must be 0.2 metres thick");
    SiteAnnotationTarget target;
    for(const auto& [id,e]:source.entities())if(e.type==kAnnotationEntityType)
        for(const auto& s:decode_annotation_entity(e).symbols)if(s.id==child.toStdString())
            target={id,s.id};
    require(!target.owner_entity_id.empty(),"persisted sofa owner not found");
    auto owner=source.entities().at(target.owner_entity_id);
    // Physical library placements opt in to model-plan projection. Keep the
    // genuine pinned SVG and source pose supplied by the public creation API.
    for(auto& raw:owner.properties["state"]["symbols"])
        if(raw.at("id")==target.child_id)raw["model_plan"]=true;
    owner.properties["state"]["version"]=std::max(10,owner.properties["state"]["version"].get<int>());
    std::vector<EntityChange> changes;
    if(site){
        owner.properties["version"]=3;
        owner.properties["presentation_frame"]={{"version",1},{"mode","building"}};
        auto building=source.entities().at("building-1");
        building.properties["site_placement"]={{"version",1},{"translation_m",{20,-7,0}},
                                               {"rotation_radians",frame_angle}};
        changes.push_back(EntityChange::upsert(building));
    }else{
        auto sheet=source.entities().at("sheet-view-1");
        const auto model=decode_sheet_view_entity(sheet);auto views=model.views();
        auto plan=std::find_if(views.begin(),views.end(),[](const auto& v){return v.id=="view-plan";});
        require(plan!=views.end(),"named plan fixture missing");
        plan->origin_m={10,-4,direction_z<0?10.0:-10.0};plan->direction={0,0,direction_z};
        plan->up={-std::sin(frame_angle),std::cos(frame_angle),0};
        sheet.properties["model"]=SheetViewModel::create(std::move(views),model.sheets(),
            model.schedule_ids(),model.sheet_order()).to_json();
        changes.push_back(EntityChange::upsert(sheet));
    }
    validate_annotation_entity(owner);changes.push_back(EntityChange::upsert(owner));
    auto document=std::make_shared<Document>(Document::fork(source));
    document->apply(ApplyEntityChanges{source.revision(),std::move(changes),{},"Wall frame fixture"});
    MainWindow window(document);window.setMetricUnits(true);window.setWorkspace(Workspace::architectural);
    window.resize(1500,1000);window.show();QApplication::processEvents();
    QTemporaryDir directory;require(directory.isValid(),"frame fixture directory missing");
    const auto path=directory.filePath("wall-frame.bldproj");
    require(window.saveProjectAs(path)&&window.openProject(path),"frame fixture save/reopen failed");
    auto& canvas=control<PlanCanvas>(window,"architecturalPlanCanvas");
    const auto selectView=[&]{
        require(window.setActiveLayer("layer-1"),"frame fixture layer unavailable");
        if(site){
            auto& action=control<QAction>(window,"sitePlan");
            if(!action.isChecked())action.trigger();
            require(action.isChecked(),"frame fixture must retain active Site Plan after reopening");
        }
        else{
            auto& views=control<QComboBox>(window,"architecturalView");
            const auto index=views.findData(QStringLiteral("view-plan"),Qt::UserRole+1);
            require(index>=3,"rotated named plan selector missing");views.setCurrentIndex(index);
        }
        QApplication::processEvents();
    };
    selectView();
    const auto before=window.document().snapshot();
    const auto saved=persisted(window,target);
    require(saved.model_plan&&close_enough(saved.placement.rotation_radians,0)&&
        same(saved.placement.position,original)&&!saved.pinned_svg.empty(),"source sofa contract changed");
    SitePresentationPlacement site_frame;
    if(site){
        const std::array<SiteAnnotationTarget,1> targets{target};
        site_frame=resolve_site_annotation_presentations(before,targets).at(target);
        require(close_enough(site_frame.forward.rotation_radians,frame_angle),"Site carrier must rotate +30 degrees");
    }
    const auto project=[&](Vec2 p){
        if(site){const auto q=site_transform_point({p.x,p.y,0},site_frame.forward);return Vec2{q.x,q.y};}
        p=p-Vec2{10,-4};const auto c=std::cos(frame_angle),s=std::sin(frame_angle);
        return Vec2{-direction_z*(c*p.x+s*p.y),-s*p.x+c*p.y};
    };
    const auto inverse=[&](Vec2 p){
        if(site){const auto q=site_transform_point({p.x,p.y,0},site_frame.inverse);return Vec2{q.x,q.y};}
        const auto c=std::cos(frame_angle),s=std::sin(frame_angle),h=-direction_z;
        return Vec2{10+c*h*p.x-s*p.y,-4+s*h*p.x+c*p.y};
    };
    // Discover the published render alias from the exact pinned artwork and
    // typed target's resolved pose, rather than guessing a Site token/owner ID.
    QString alias;
    for(const auto& e:canvas.entities())if(e.svg_symbol&&
        e.svg_symbol->catalog_id==QStringLiteral("svg-v2-04_living-sofa-three-seat")&&
        e.svg_symbol->document==QByteArray::fromStdString(saved.pinned_svg)&&
        same(e.svg_symbol->position,project(original))){
        require(alias.isEmpty(),"ambiguous published sofa alias");alias=e.id;
    }
    require(!alias.isEmpty()&&window.selectEntity(alias),"typed sofa render alias cannot select");
    canvas.setTool(CanvasTool::select);canvas.setGridEnabled(false);canvas.setOverviewMapEnabled(false);
    canvas.setSnapEnabled(true);canvas.setWallSnapEnabled(true);canvas.fitView();QApplication::processEvents();
    const auto retained=*item(canvas,alias).svg_symbol;
    require(close_enough(retained.width_metres,2.25)&&close_enough(retained.depth_metres,.95)&&
        retained.document.contains("sofa-three-seat--artwork"),"real sofa dimensions/artwork missing");
    require(same_axis(retained.rotation_radians,site?frame_angle:direction_z<0?-frame_angle:-5*pi/6),
            "unrotated source sofa did not follow the actual view basis");
    const auto& published_wall=item(canvas,wall);
    require(!published_wall.drawing_alignment_segments.empty(),"published analytical wall baseline missing");
    const auto baseline=published_wall.drawing_alignment_segments.front();
    auto tangent=baseline.end-baseline.start;tangent=tangent*(1/std::hypot(tangent.x,tangent.y));
    const Vec2 normal{-tangent.y,tangent.x},middle=(baseline.start+baseline.end)*.5;
    const auto displayed_angle=std::atan2(tangent.y,tangent.x);
    const auto expected_angle=site?pi/4:direction_z<0?-pi/12:-11*pi/12;
    require(same_axis(displayed_angle,expected_angle),"wall is not in the requested displayed frame");
    const Vec2 width_axis{std::cos(retained.rotation_radians),std::sin(retained.rotation_radians)};
    const Vec2 depth_axis{-width_axis.y,width_axis.x};
    const auto support=std::abs(dot(normal,width_axis))*retained.width_metres/2+
                       std::abs(dot(normal,depth_axis))*retained.depth_metres/2;
    const auto raw=middle+tangent*.83+normal*(.1+support+6/canvas.viewScale());
    require(close_enough((dot(raw-middle,normal)-support-.1)*canvas.viewScale(),6),"raw footprint must be six pixels from wall face");
    const auto expected=middle+tangent*.83+normal*(.1+retained.depth_metres/2);
    for(const auto p:{retained.position,raw})require(QRectF(canvas.rect()).adjusted(15,15,-15,-15)
        .contains(screen(canvas,p)),"frame drag fixture clipped");
    mouse(canvas,QEvent::MouseButtonPress,retained.position);mouse(canvas,QEvent::MouseMove,raw);
    await([&]{return !canvas.entitiesMovePreviewPending()&&!canvas.entitiesMovePreview().empty();},"frame move preview missing");
    const auto previews=canvas.entitiesMovePreview();
    const auto preview=std::find_if(previews.begin(),previews.end(),[&](const auto& e){return e.id==alias;});
    require(preview!=previews.end()&&preview->svg_symbol,"frame SVG preview missing");
    const auto live=*preview->svg_symbol;
    const auto nearest=retained.rotation_radians+std::remainder(displayed_angle-retained.rotation_radians,pi);
    require(close_enough(live.rotation_radians,nearest)&&same(live.position,expected),"preview must align nearest displayed width axis and wall face");
    require(live.document==retained.document&&close_enough(live.width_metres,retained.width_metres)&&
        close_enough(live.depth_metres,retained.depth_metres)&&live.flip_horizontal==retained.flip_horizontal&&
        live.flip_vertical==retained.flip_vertical,"preview changed pinned artwork or physical footprint");
    const auto local=inverse(live.position);
    require(close_enough(std::abs(dot(local,source_normal))-.95/2,.1),"inverse-projected preview misses physical source wall face");
    require(window.document().snapshot().entities()==before.entities(),"frame preview mutated persisted source");
    const auto still=*item(canvas,alias).svg_symbol;
    require(same(still.position,retained.position)&&close_enough(still.rotation_radians,retained.rotation_radians),"preview mutated retained source");
    const auto name=QString(site?"site-rotated":direction_z<0?"named-rotated":"named-reflected");
    capture(window,name+"-wall-preview.png");
    mouse(canvas,QEvent::MouseButtonRelease,raw);
    await([&]{return !canvas.entitiesMovePreviewPending()&&window.document().revision()==before.revision()+1;},"frame pose did not commit once");
    const auto committed=persisted(window,target);
    require(same(committed.placement.position,local)&&same_axis(committed.placement.rotation_radians,wall_angle),"commit did not inverse-project source pose to +15 degrees modulo pi");
    require(committed.pinned_svg==saved.pinned_svg&&close_enough(committed.width_scale,saved.width_scale)&&
        close_enough(committed.depth_scale,saved.depth_scale)&&close_enough(committed.placement.scale,saved.placement.scale)&&
        committed.flip_horizontal==saved.flip_horizontal&&committed.flip_vertical==saved.flip_vertical,"commit changed artwork/proportions/flips");
    const auto checkRetained=[&]{const auto s=*item(canvas,alias).svg_symbol;
        require(same(s.position,live.position)&&close_enough(s.rotation_radians,live.rotation_radians)&&
                s.document==live.document,"committed/reopened presentation differs from preview");};
    checkRetained();const auto after=window.document().snapshot();
    require(window.undoCommand()&&window.document().snapshot().entities()==before.entities(),"one Undo must restore exact source owners");
    require(!control<QAction>(window,"undoCommand").isEnabled(),"frame position/rotation created multiple undo entries");
    require(window.redoCommand()&&window.document().snapshot().entities()==after.entities(),"one Redo must restore complete aligned source pose");
    checkRetained();
    require(window.saveProject()&&window.openProject(path),"aligned frame save/open failed");selectView();
    require(window.document().snapshot().entities()==after.entities(),"frame save/open changed persisted source");
    checkRetained();
}
} // namespace

int main(int argc,char** argv){
    sketch::testing::noninteractive_errors();QStandardPaths::setTestModeEnabled(true);QApplication app(argc,argv);
    QCoreApplication::setApplicationName("Vertex-wall-frame-test-"+QUuid::createUuid().toString(QUuid::WithoutBraces));
    try{frameCase(false,-1);frameCase(false,1);frameCase(true);
        std::cout<<"wall alignment rotation frame checks passed\n";return 0;
    }catch(const std::exception& e){std::cerr<<"wall_alignment_rotation_frame_tests: "<<e.what()<<'\n';return 1;}
}
