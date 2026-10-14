#include "sketch/desktop/main_window.hpp"
#include "sketch/architecture.hpp"
#include "sketch/assembly_document_adapter.hpp"
#include "sketch/assembly_geometry.hpp"
#include "sketch/building_entity.hpp"
#include "sketch/boundary_dimension.hpp"
#include "sketch/document_digest.hpp"
#include "sketch/project_store.hpp"
#include "../src/desktop/plan_canvas.hpp"
#include "support/noninteractive_errors.hpp"
#include <QAction>
#include <QApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFontDatabase>
#include <QFont>
#include <QImage>
#include <QListWidget>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolButton>
#include <QUuid>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <functional>
#include <iostream>
#include <memory>
#include <numbers>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace sketch;
using namespace sketch::desktop;
using Json=nlohmann::json;
void require(bool value,const char* message) {if(!value) throw std::runtime_error(message);}
void require_near(double actual,double expected,const char* message) {
    if(!std::isfinite(actual)||std::abs(actual-expected)>1e-7)
        throw std::runtime_error(std::string(message)+": expected "+std::to_string(expected)+", observed "+std::to_string(actual));
}
std::string fresh(const char* role) {return std::string(role)+"-"+QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();}
Boundary rectangle(double x,double y,double w,double d) {
    return {{{x,y},{x+w,y},0},{{x+w,y},{x+w,y+d},0},{{x+w,y+d},{x,y+d},0},{{x,y+d},{x,y},0}};
}
void point(Vec3 actual,Vec3 expected) {
    require_near(actual.x,expected.x,"typed stair source X");require_near(actual.y,expected.y,"typed stair source Y");require_near(actual.z,expected.z,"typed stair source Z");
}
void polygon(const StairPolygon& actual,const StairPolygon& expected) {
    for(std::size_t i=0;i<4;++i) point(actual[i],expected[i]);
}
struct Fixture {
    std::shared_ptr<Document> document;
    std::string property=fresh("property"),building=fresh("building"),floor=fresh("floor"),layer=fresh("layer");
    std::string component=fresh("component"),stair=fresh("stair"),catalog=fresh("catalog"),type=fresh("type");
    std::string dimension;
    bool site=false;
};
Fixture fixture(bool with_dimension=false,bool site=false) {
    Fixture f;f.site=site;
    const Json context={{"property_id",f.property},{"building_id",f.building},{"floor_id",f.floor},{"layer_id",f.layer}};
    AssemblyProfile profile;profile.id=fresh("profile");profile.outer=rectangle(0,0,2,1);profile.elevation_m=.25;profile.height_m=1.5;
    if(with_dimension) profile.topology=AssemblyProfileTopology{
        {"source-outer",{"source-e0","source-e1","source-e2","source-e3"},
                        {"source-v0","source-v1","source-v2","source-v3"}}, {}};
    AssemblyType type;type.id=f.type;type.name="Independent stair cohort component";type.profiles={profile};
    Entity catalog{f.catalog,"assembly_model",{{"model",AssemblyModel::create({}, {type}, {}).to_json()}}};
    catalog.extensions={{"opaque_fixture",{{"retain","catalog"},{"values",Json::array({17,true,"source"})}}}};
    catalog.extensions["opaque_command"]={{"version",52},{"mixed_selection_edit_intent",{{"version",43}}}};
    AssemblyInstance instance;instance.id=f.component;instance.type_id=f.type;instance.root_transform=AssemblyTransform{{5,1,.75},0,1};
    auto component=encode_document_assembly_instance({f.component,"assembly_instance",context},{f.catalog,instance});
    StairFlight authored;authored.id=f.stair;authored.base_position={1,0,.4};authored.orientation_radians=0;
    authored.riser_count=4;authored.total_rise=2;authored.going=.5;authored.width=2;authored.top_landing=StairLanding{.5,.15};
    auto stair=encode_building_entity(authored);
    // Deliberately mixed numeric representations must survive unchanged axes.
    stair.properties["base_position_m"][0]=1;stair.properties["base_position_m"][1]=0;
    stair.properties["width_m"]=2;stair.properties["total_rise_m"]=2;
    stair.properties["top_landing"]["vendor"]={{"retain","landing envelope"}};
    const auto receipt=[](const char* expression,int numerator,int denominator) {
        return Json{{"version",1},{"original_expression",expression},{"entered_unit","m"},
                    {"exact_metres",{{"numerator",numerator},{"denominator",denominator}}}};
    };
    stair.properties["quantity_entries"]={
        {"/base_position_m/0",receipt("1 m",1,1)},
        {"/base_position_m/1",receipt("0 m",0,1)},
        {"/base_position_m/2",receipt("0.4 m",2,5)},
        {"/going_m",receipt("0.5 m",1,2)},
        {"/width_m",receipt("2 m",2,1)},
        {"/total_rise_m",receipt("2 m",2,1)},
        {"/top_landing/depth_m",receipt("0.5 m",1,2)},
        {"/top_landing/thickness_m",receipt("0.15 m",3,20)},
        {"/future_dimension_m",{{"version",9},{"opaque",Json::array({"retain",17})}}}};
    stair.properties["quantity_entries"]["/total_rise_m"]["vendor"]={{"opaque",Json::array({23,"retained receipt envelope"})}};
    for(auto* entity:{&component,&stair}) {
        for(auto it=context.begin();it!=context.end();++it) entity->properties[it.key()]=it.value();
        entity->extensions={{"opaque_fixture",{{"retain",entity->id},{"values",Json::array({19,true,"source"})}}}};
    }
    auto asset=Asset::create(fresh("asset"),"application/octet-stream",{std::byte{0x31},std::byte{0x42}},{{"opaque_fixture",{{"retain","asset"}}}});
    std::vector<Entity> entities{
        {f.property,"property",{{"name","Stair cohort"}},false},
        {f.building,"building",{{"property_id",f.property},{"name","Building"}},false},
        {f.floor,"floor",{{"building_id",f.building},{"name","Ground"},{"elevation_m",0}},false},
        {f.layer,"layer",{{"floor_id",f.floor},{"name","Plan"}},false},catalog,component,stair};
    if(with_dimension) {
        f.dimension=fresh("profile-dimension");
        BoundaryDimension dimension;dimension.id=f.dimension;dimension.boundary_id=f.component;
        dimension.kind=BoundaryDimensionKind::assembly_profile;dimension.text_position={6,.5};
        AssemblyDimensionTarget target;target.component={f.catalog,f.component,f.component,{}};
        target.profile=AssemblyProfileDimensionTarget{profile.id,"source-outer",
            AssemblyProfileDimensionMeasurement::edge_length,"source-e0",std::nullopt,std::nullopt};
        dimension.assembly_target=target;auto carrier=encode_boundary_dimension_entity(dimension);
        for(auto it=context.begin();it!=context.end();++it) carrier.properties[it.key()]=it.value();
        entities.push_back(std::move(carrier));
    }
    if(site) {
        entities[0].properties["site_frame"]={{"version",1},{"origin_m",{100,200,30}},
            {"rotation_radians",std::numbers::pi/2},
            {"vertical_datum",{{"identifier","stair-fixture-datum"},{"height_at_origin_m",1500}}}};
        entities[1].properties["site_placement"]={{"version",1},{"translation_m",{10,20,3}},
            {"rotation_radians",0}};
        for(auto& entity:entities) if(entity.id==f.component || entity.id==f.stair)
            entity.properties["presentation_frame"]={{"version",1},{"mode","building"}};
    }
    f.document=std::make_shared<Document>(Document::create(std::move(entities),{std::move(asset)}));
    return f;
}
void admit_source(const Fixture& f) {
    const auto source=f.document->snapshot();const auto& entity=source.entities().at(f.stair);
    require(entity.properties.at("version")==1 && entity.properties.at("form")=="straight_stair_flight","source stair is canonical V1 ordinary straight flight");
    const auto stair=std::get<StairFlight>(decode_building_entity(entity));
    require(!stair.level_connection && stair.flights.empty() && stair.landings.empty(),"source stair has no level connection or explicit topology");
    point(stair.base_position,{1,0,.4});require_near(stair.orientation_radians,0,"source yaw");
    require(stair.riser_count==4 && stair.top_landing.has_value(),"source riser count and genuine top landing");
    require_near(stair.total_rise,2,"source rise");require_near(stair.going,.5,"source going");require_near(stair.width,2,"source width");
    require_near(stair.top_landing->depth,.5,"source landing depth");require_near(stair.top_landing->thickness,.15,"source landing thickness");
    const auto layout=derive_stair_layout(stair);
    require(layout.flights.size()==1 && layout.landings.size()==1,"typed source layout has one flight and one genuine landing");
    const auto& flight=layout.flights.front();require_near(flight.run,2,"run is riser_count times going");
    point(flight.base_position,{1,0,.4});point(flight.end_position,{3,0,2.4});
    // stair_semantics.cpp rectangle is CCW; footprint stays at base Z.
    polygon(flight.footprint,StairPolygon{{{1,0,.4},{3,0,.4},{3,2,.4},{1,2,.4}}});
    require(flight.treads.size()==4,"typed source has four actual treads");
    for(std::size_t i=0;i<4;++i) {
        const double x=1+.5*i,z=.9+.5*i;
        polygon(flight.treads[i].footprint,StairPolygon{{{x,0,z},{x+.5,0,z},{x+.5,2,z},{x,2,z}}});
        point(flight.treads[i].nosing_start,{x,0,z});point(flight.treads[i].nosing_end,{x,2,z});require_near(flight.treads[i].elevation,z,"actual tread elevation");
    }
    polygon(layout.landings.front().footprint,StairPolygon{{{3,0,2.4},{3.5,0,2.4},{3.5,2,2.4},{3,2,2.4}}});
    require_near(layout.landings.front().elevation,2.4,"landing elevation");require_near(layout.landings.front().thickness,.15,"landing thickness");
    require(!make_building_shape(stair).IsNull(),"admitted source stair produces real native solid");
    AssemblyExpansionBudget budget;const auto expansion=expand_document_assembly_instance(source.entities().at(f.component),source.entities(),budget);
    require(!expansion.source_instance.placement,"component source is independent of any host");
    require(expansion.profiles.size()==1,"component source expands exactly one independent profile");
    const auto& expanded=expansion.profiles.front();const auto& pose=expanded.transform;
    require(expanded.part_path.empty() && expanded.profile.outer.size()==4,"component source retains direct root rectangular profile");
    require(expanded.profile.holes.empty() && (f.dimension.empty()==!expanded.profile.topology.has_value()),"source topology exists only for the real dimension fixture");
    const auto local=rectangle(0,0,2,1);
    for(std::size_t i=0;i<4;++i) {
        require_near(expanded.profile.outer[i].start.x,local[i].start.x,"component local source start X");require_near(expanded.profile.outer[i].start.y,local[i].start.y,"component local source start Y");
        require_near(expanded.profile.outer[i].end.x,local[i].end.x,"component local source end X");require_near(expanded.profile.outer[i].end.y,local[i].end.y,"component local source end Y");require_near(expanded.profile.outer[i].sweep_radians,0,"component straight profile edge");
    }
    require_near(expanded.profile.elevation_m,.25,"component source elevation");require_near(expanded.profile.height_m,1.5,"component source height");
    require_near(pose.translation_m.x,5,"component source translation X");require_near(pose.translation_m.y,1,"component source translation Y");require_near(pose.translation_m.z,.75,"component source translation Z");
    require_near(pose.rotation_radians,0,"component source pose yaw");require_near(pose.scale,1,"component source scale");require_near(pose.vertical_scale,1,"component source vertical scale");require(!pose.mirrored_y,"component source has ordinary unmirrored pose");
    const auto geometry=make_assembly_geometry(expansion);
    require(geometry.solids.size()==1 && !geometry.shape.IsNull() && !geometry.solids.front().shape.IsNull(),"independent component produces one real native profile solid");
}
class ModalGuard {
    QTimer timer;QString title;
public:
    ModalGuard() {timer.setInterval(10);QObject::connect(&timer,&QTimer::timeout,[this] {
        if(auto* dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget())) {title=dialog->windowTitle();if(title.isEmpty())title="unnamed dialog";dialog->setAttribute(Qt::WA_DontShowOnScreen);dialog->reject();}
    });timer.start();}
    void verify() const {if(!title.isEmpty())throw std::runtime_error("Unexpected stair regression modal: "+title.toStdString());}
    void pause() {timer.stop();}
    void resume() {timer.start();}
};
void settle(const ModalGuard& guard) {for(int i=0;i<3;++i) {QApplication::sendPostedEvents(nullptr,QEvent::LayoutRequest);QApplication::processEvents(QEventLoop::AllEvents,20);guard.verify();}}
PlanCanvas& canvas(MainWindow& window) {
    const auto name=window.workspace()==Workspace::architectural ? "architecturalPlanCanvas" : "measurementPlanCanvas";
    auto* result=dynamic_cast<PlanCanvas*>(window.findChild<QWidget*>(name));require(result,"actual active PlanCanvas exists");return *result;
}
// Projected native ink may split a boundary or merge collinear tread edges.
// Authenticate coverage of each hand tread/landing edge, rather than counting
// four owner edges or fabricating a rectangular stair presentation.
void covered(const Boundary& actual,const Boundary& expected) {
    for(const auto& edge:expected) {
        const auto dx=edge.end.x-edge.start.x,dy=edge.end.y-edge.start.y,length=std::hypot(dx,dy);
        std::vector<std::pair<double,double>> intervals;
        for(const auto& candidate:actual) {
            if(std::abs(candidate.sweep_radians)>1e-7)continue;
            const auto cross=[&](Vec2 p){return ((p.x-edge.start.x)*dy-(p.y-edge.start.y)*dx)/length;};
            if(std::abs(cross(candidate.start))>1e-7||std::abs(cross(candidate.end))>1e-7)continue;
            const auto along=[&](Vec2 p){return ((p.x-edge.start.x)*dx+(p.y-edge.start.y)*dy)/length;};
            double a=along(candidate.start),b=along(candidate.end);if(a>b)std::swap(a,b);
            intervals.emplace_back(a,b);
        }
        std::sort(intervals.begin(),intervals.end());double reach=0;
        for(const auto& [a,b]:intervals)if(a<=reach+1e-7)reach=std::max(reach,b);
        require(reach>=length-1e-7,"published native source geometry must cover each independent hand boundary/tread/landing edge");
    }
}
void published(const PlanCanvas& drawing,const Fixture& f) {
    for(const auto& id:{f.component,f.stair}) {
        Boundary ink;bool found=false;
        for(const auto& item:drawing.entities())if(item.id==QString::fromStdString(id)) {
            found=true;require(item.selected && item.holes.empty(),"actual canvas publishes selected ordinary owner without fabricated holes");
            ink.insert(ink.end(),item.segments.begin(),item.segments.end());
        }
        require(found && !ink.empty(),"actual canvas publishes nonempty native component/stair ink");
        if(id==f.component) {require(ink.size()==4,"component publishes four actual profile edges");covered(ink,rectangle(5,1,2,1));}
        else {
            covered(ink,rectangle(1,0,2,2));
            for(int i=0;i<4;++i)covered(ink,rectangle(1+.5*i,0,.5,2));
            covered(ink,rectangle(3,0,.5,2));
        }
    }
}
std::string scenario(const QString& directory,bool reverse) {
    auto f=fixture();admit_source(f);const auto before=f.document->snapshot();
    const QString stem=reverse ? "stair-component" : "component-stair";
    ModalGuard guard;MainWindow window(f.document,nullptr,QDir(directory).filePath(stem+"-library.json"));
    window.setAttribute(Qt::WA_DontShowOnScreen);window.resize(1400,900);window.setWorkspace(Workspace::measurement);window.setMetricUnits(true);window.show();settle(guard);
    auto* snap=window.findChild<QToolButton*>("snapTool");require(snap,"actual Snap toggle exists");snap->setChecked(false);
    auto& drawing=canvas(window);drawing.setOverviewMapEnabled(false);drawing.setGridEnabled(false);drawing.setTool(CanvasTool::select);
    drawing.setSnapEnabled(false);drawing.setWallSnapEnabled(false);drawing.setViewTransform({4,1},60);settle(guard);
    require(window.selectEntity({}),"clear public selection");
    QStringList requested{QString::fromStdString(f.component),QString::fromStdString(f.stair)};if(reverse)std::reverse(requested.begin(),requested.end());
    for(const auto& id:requested)require(window.selectEntity(id,true),"public selection admits ordinary component/stair owner");settle(guard);
    auto selected=drawing.selectedIds();auto expected=requested;selected.sort();expected.sort();
    require(selected==expected && drawing.selectedRoofOpenings().empty(),"actual published roster has exactly component and stair regardless of request order");
    published(drawing,f);
    const auto after=window.document().snapshot();
    require(document_snapshot_digest(after)==document_snapshot_digest(before) && after.entities()==before.entities() && after.assets()==before.assets(),"selection preserves exact source/context/opaque metadata/assets/revision/history");
    const auto capture=qEnvironmentVariable("VERTEX_TEST_CAPTURE_DIR");
    const auto painted=drawing.grab();require(!painted.isNull(),"actual selected canvas paints before frame/grip qualification");
    if(!capture.isEmpty())require(QDir().mkpath(capture) && painted.save(QDir(capture).filePath(stem+"-selected.png")),"optional actual native canvas capture saves");
    guard.verify();const auto frame=drawing.selectionAxisResizeCohortFrame();
    if(!frame)return stem.toStdString()+": MissingFrame: independent component + ordinary V1 stair must expose one common four-side resize frame";
    // Hand union: component [5,7]x[1,2], flight [1,3]x[0,2], landing [3,3.5]x[0,2].
    require_near(frame->center.x,4,"hand cohort centre X");require_near(frame->center.y,1,"hand cohort centre Y");require_near(frame->width_metres,6,"hand cohort width");require_near(frame->depth_metres,2,"hand cohort depth");require_near(frame->rotation_radians,0,"hand cohort yaw");
    const auto grips=drawing.selectionAxisHandlePositions();require(grips.size()==4,"actual common frame publishes four painted side grips");
    for(std::size_t i=0;i<grips.size();++i) {require(std::isfinite(grips[i].x())&&std::isfinite(grips[i].y()),"painted side grips are finite");for(std::size_t j=0;j<i;++j)require(std::hypot(grips[i].x()-grips[j].x(),grips[i].y()-grips[j].y())>1,"painted side grips are distinct");}
    return {};
}

// Independent hand operators. The opposite-side cases move the stair's base:
// left: x'=7+1.5*(x-7); bottom: y'=2+1.5*(y-2).
// Ordinary right/top cases anchor at x=1/y=0 respectively.
struct ResizeCase {bool resized=false,depth=false,opposite=false;};
Vec2 hand(Vec2 p,ResizeCase c) {
    if(!c.resized) return p;
    if(c.depth) p.y=(c.opposite ? 2 : 0)+1.5*(p.y-(c.opposite ? 2 : 0));
    else p.x=(c.opposite ? 7 : 1)+1.5*(p.x-(c.opposite ? 7 : 1));
    return p;
}
Vec2 presented(Vec2 p,bool site) {return site ? Vec2{80-p.y,210+p.x} : p;}
Boundary hand_boundary(Boundary boundary,ResizeCase c,bool site=false) {
    for(auto& edge:boundary) {edge.start=presented(hand(edge.start,c),site);edge.end=presented(hand(edge.end,c),site);}
    return boundary;
}
void capture(PlanCanvas& drawing,const QString& name) {
    const auto directory=qEnvironmentVariable("VERTEX_TEST_CAPTURE_DIR");
    const auto painted=drawing.grab();require(!painted.isNull(),"actual selected canvas paints");
    if(!directory.isEmpty()) require(QDir().mkpath(directory) && painted.save(QDir(directory).filePath(name)),"optional native stair capture saves");
}
void display(MainWindow& window,const ModalGuard& guard,const Fixture& f) {
    window.setAttribute(Qt::WA_DontShowOnScreen);window.resize(1400,900);
    window.setWorkspace(Workspace::measurement);window.setMetricUnits(true);window.show();settle(guard);
    if(f.site) {
        require(window.setActiveLayer(QString::fromStdString(f.layer)),"Site activates actual source layer");
        window.setWorkspace(Workspace::architectural);
        auto* action=window.findChild<QAction*>("sitePlan");require(action,"actual Site Plan action exists");
        if(!action->isChecked()) action->trigger();settle(guard);
        require(action->isChecked() && window.workspace()==Workspace::architectural &&
                window.activeLayerId()==QString::fromStdString(f.layer),"Site publishes the actual checked view and source layer");
    }
    auto* snap=window.findChild<QToolButton*>("snapTool");require(snap,"actual Snap toggle exists");snap->setChecked(false);
    auto& drawing=canvas(window);drawing.setOverviewMapEnabled(false);drawing.setGridEnabled(false);
    drawing.setTool(CanvasTool::select);drawing.setSnapEnabled(false);drawing.setWallSnapEnabled(false);
    drawing.setViewTransform(presented({4,1},f.site),45);settle(guard);
}
void roster(const PlanCanvas& drawing,const Fixture& f) {
    auto actual=drawing.selectedIds();QStringList expected{QString::fromStdString(f.component),QString::fromStdString(f.stair)};
    actual.sort();expected.sort();require(actual==expected && drawing.selectedRoofOpenings().empty(),
        "actual selection has exactly the independent component and ordinary stair");
    QStringList published_ids;
    for(const auto& item:drawing.entities()) if(item.selected && !published_ids.contains(item.id)) published_ids.push_back(item.id);
    published_ids.sort();require(published_ids==expected,"selected published presentations have exactly the two source owners");
}
void select(MainWindow& window,const Fixture& f,bool reverse,const ModalGuard& guard) {
    require(window.selectEntity({}),"clear actual public selection");
    QStringList requested{QString::fromStdString(f.component),QString::fromStdString(f.stair)};
    if(reverse) std::reverse(requested.begin(),requested.end());
    for(const auto& id:requested) require(window.selectEntity(id,true),"actual public selection admits each ordinary owner");
    settle(guard);roster(canvas(window),f);
}
void plans(const std::vector<CanvasEntity>& scene,const Fixture& f,ResizeCase c={}) {
    for(const auto& id:{f.component,f.stair}) {
        Boundary ink;
        for(const auto& item:scene) if(item.id==QString::fromStdString(id)) {
            require(item.holes.empty(),"native ordinary owner retains no fabricated holes");
            ink.insert(ink.end(),item.segments.begin(),item.segments.end());
        }
        require(!ink.empty(),"actual native scene contains both owners");
        if(id==f.component) {
            require(ink.size()==4,"component publishes exactly four actual profile edges");
            covered(ink,hand_boundary(rectangle(5,1,2,1),c,f.site));
        } else {
            covered(ink,hand_boundary(rectangle(1,0,2,2),c,f.site));
            for(int i=0;i<4;++i) covered(ink,hand_boundary(rectangle(1+.5*i,0,.5,2),c,f.site));
            covered(ink,hand_boundary(rectangle(3,0,.5,2),c,f.site));
        }
    }
}
void frame(const PlanCanvas& drawing,const Fixture& f,ResizeCase c={}) {
    const auto actual=drawing.selectionAxisResizeCohortFrame();require(actual.has_value(),"both ordinary owners expose a common physical frame");
    const auto minimum=hand({1,0},c),maximum=hand({7,2},c);
    const auto center=presented({(minimum.x+maximum.x)/2,(minimum.y+maximum.y)/2},f.site);
    const double width=maximum.x-minimum.x,depth=maximum.y-minimum.y;
    require_near(actual->center.x,center.x,"hand frame center X");require_near(actual->center.y,center.y,"hand frame center Y");
    require_near(actual->width_metres,f.site ? depth : width,"hand displayed frame width");
    require_near(actual->depth_metres,f.site ? width : depth,"hand displayed frame depth");
    require_near(actual->rotation_radians,0,"common frame follows published Plan/Site axes");
    const auto grips=drawing.selectionAxisHandlePositions();require(grips.size()==4,"actual common frame paints four side grips");
    for(std::size_t i=0;i<grips.size();++i) {
        require(std::isfinite(grips[i].x()) && std::isfinite(grips[i].y()),"painted side grips are finite");
        for(std::size_t j=0;j<i;++j) require(std::hypot(grips[i].x()-grips[j].x(),grips[i].y()-grips[j].y())>1,"four painted side grips are distinct");
    }
}
Json structure(const DocumentSnapshot& source,const Fixture& f,ResizeCase c={}) {
    const auto& entity=source.entities().at(f.stair);
    require(entity.properties.at("version")==1 && entity.properties.at("form")=="straight_stair_flight","resized stair retains actual V1 authoring");
    const auto stair=std::get<StairFlight>(decode_building_entity(entity));
    require(!stair.level_connection && stair.flights.empty() && stair.landings.empty() && stair.riser_count==4 && stair.top_landing,
            "stair retains four risers, one ordinary flight and its genuine top landing");
    const auto base=hand({1,0},c);point(stair.base_position,{base.x,base.y,.4});
    require_near(stair.orientation_radians,0,"stair authored yaw remains unchanged");require_near(stair.total_rise,2,"stair total rise remains unchanged");
    const double going=c.resized && !c.depth ? .75 : .5,width=c.resized && c.depth ? 3 : 2;
    const double landing_depth=c.resized && !c.depth ? .75 : .5;
    require_near(stair.going,going,"hand going");require_near(stair.width,width,"hand stair width");
    require_near(stair.top_landing->depth,landing_depth,"hand top landing depth");require_near(stair.top_landing->thickness,.15,"landing thickness unchanged");
    const auto layout=derive_stair_layout(stair);require(layout.flights.size()==1 && layout.landings.size()==1,"actual typed layout has one flight and one landing");
    const auto transformed=[&](Vec3 p) {const auto xy=hand({p.x,p.y},c);return Vec3{xy.x,xy.y,p.z};};
    const auto transformed_polygon=[&](StairPolygon p) {for(auto& corner:p) corner=transformed(corner);return p;};
    const auto& flight=layout.flights.front();point(flight.base_position,transformed({1,0,.4}));point(flight.end_position,transformed({3,0,2.4}));
    require_near(flight.run,4*going,"actual flight run uses four resized goings");
    polygon(flight.footprint,transformed_polygon(StairPolygon{{{1,0,.4},{3,0,.4},{3,2,.4},{1,2,.4}}}));
    require(flight.treads.size()==4,"actual resized native layout retains all four treads");
    for(std::size_t i=0;i<4;++i) {
        const double x=1+.5*i,z=.9+.5*i;
        polygon(flight.treads[i].footprint,transformed_polygon(StairPolygon{{{x,0,z},{x+.5,0,z},{x+.5,2,z},{x,2,z}}}));
        point(flight.treads[i].nosing_start,transformed({x,0,z}));point(flight.treads[i].nosing_end,transformed({x,2,z}));
        require_near(flight.treads[i].elevation,z,"every tread elevation remains unchanged");
    }
    polygon(layout.landings.front().footprint,transformed_polygon(StairPolygon{{{3,0,2.4},{3.5,0,2.4},{3.5,2,2.4},{3,2,2.4}}}));
    require_near(layout.landings.front().elevation,2.4,"landing elevation unchanged");require_near(layout.landings.front().thickness,.15,"native landing thickness unchanged");
    const auto native_stair=make_building_shape(stair);require(!native_stair.IsNull(),"complete typed result manufactures a real native stair solid");
    // Four actual step prisms: .5*2*(.5+1+1.5+2), plus .5*2*.15 landing.
    require_near(solid_volume(native_stair),c.resized ? 7.725 : 5.15,"native stair volume matches all four hand treads and genuine landing");
    AssemblyExpansionBudget budget;const auto expanded=expand_document_assembly_instance(source.entities().at(f.component),source.entities(),budget);
    require(expanded.profiles.size()==1,"component retains one independent native profile");
    const auto& profile=expanded.profiles.front();const auto& pose=profile.transform;
    require(profile.profile.outer.size()==4 && profile.profile.holes.empty(),"component retains complete rectangular profile");
    const auto expected=hand_boundary(rectangle(5,1,2,1),c);auto vertices=Json::array();
    for(std::size_t i=0;i<4;++i) {
        auto p=profile.profile.outer[i].start;
        if(profile.profile.planar_affine) {
            const auto& a=*profile.profile.planar_affine;const double x=p.x-a.source_origin.x,y=p.y-a.source_origin.y;
            p={a.target_origin.x+a.xx*x+a.xy*y,a.target_origin.y+a.yx*x+a.yy*y};
        }
        const double py=pose.mirrored_y ? -p.y : p.y;
        const double x=pose.translation_m.x+pose.scale*(p.x*std::cos(pose.rotation_radians)-py*std::sin(pose.rotation_radians));
        const double y=pose.translation_m.y+pose.scale*(p.x*std::sin(pose.rotation_radians)+py*std::cos(pose.rotation_radians));
        require_near(x,expected[i].start.x,"component hand world corner X");require_near(y,expected[i].start.y,"component hand world corner Y");
        require_near(profile.profile.outer[i].sweep_radians,0,"component profile edges remain straight");vertices.push_back({x,y});
    }
    require_near(pose.translation_m.z+pose.scale*pose.vertical_scale*profile.profile.elevation_m,1,"component physical base Z unchanged");
    require_near(pose.scale*pose.vertical_scale*profile.profile.height_m,1.5,"component physical height unchanged");
    const auto instance=decode_document_assembly_instance(source.entities().at(f.component));
    require(instance.instance.root_transform && !instance.instance.placement,"component remains an independent root");
    require_near(instance.instance.root_transform->translation_m.z,.75,"component root Z unchanged");
    const auto geometry=make_assembly_geometry(expanded);
    require(geometry.solids.size()==1 && !geometry.shape.IsNull() && !geometry.solids.front().shape.IsNull(),"component manufactures one actual native solid");
    require_near(geometry.volume_m3,c.resized ? 4.5 : 3,"native component volume follows hand horizontal scaling");
    return {{"component",vertices},{"stair",{base.x,base.y,.4,going,width,landing_depth,2,.15}}};
}
void same_structure(const Json& a,const Json& b) {
    if(a.is_number() && b.is_number()) {require_near(a.get<double>(),b.get<double>(),"selection orders retain the same hand geometry");return;}
    require(a.type()==b.type() && a.size()==b.size(),"selection orders retain the same complete structure");
    if(a.is_object()) for(auto it=a.begin();it!=a.end();++it) same_structure(it.value(),b.at(it.key()));
    else if(a.is_array()) for(std::size_t i=0;i<a.size();++i) same_structure(a.at(i),b.at(i));
    else require(a==b,"selection orders retain exact nonnumeric values");
}
void preserved(const DocumentSnapshot& before,const DocumentSnapshot& after,const Fixture& f,bool retired=false) {
    require(before.assets()==after.assets() && !after.assets().empty(),"all source asset bytes and opaque envelopes remain exact");
    require(before.entities().size()==after.entities().size()+(retired ? 1 : 0),"stair adds no owner/child identity or unrelated entity");
    for(const auto& [id,entity]:before.entities()) {
        if(retired && id==f.dimension) {require(!after.entities().contains(id),"only explicitly reviewed dimension is retired");continue;}
        require(after.entities().contains(id),"complete source inventory remains present");
        const auto& actual=after.entities().at(id);
        if(id!=f.component && id!=f.catalog && id!=f.stair) require(actual==entity,"unselected context and unrelated carriers remain exact");
        else require(actual.extensions.dump()==entity.extensions.dump(),"owner/catalog opaque extension envelopes remain exact");
        if(id==f.component || id==f.stair) {
            for(const auto* key:{"property_id","building_id","floor_id","layer_id"}) require(actual.properties.at(key)==entity.properties.at(key),"owner drawing contexts remain exact");
            if(f.site) require(actual.properties.at("presentation_frame").dump()==entity.properties.at("presentation_frame").dump(),"Site owner retains exact building presentation frame");
        }
    }
    auto original=before.entities().at(f.stair),result=after.entities().at(f.stair);
    const auto& receipts=original.properties.at("quantity_entries");const auto& actual_receipts=result.properties.at("quantity_entries");
    auto expected_receipts=receipts;
    // Typed known receipts are invalidated only for changed owned scalar paths.
    // Opaque future receipts stay exact; Cancel has no geometry archive contract.
    for(const auto* path:{"/base_position_m/0","/base_position_m/1","/going_m","/width_m","/top_landing/depth_m"}) {
        const Json::json_pointer pointer(path);
        if(original.properties.at(pointer)!=result.properties.at(pointer)) expected_receipts.erase(path);
        else require(original.properties.at(pointer).dump()==result.properties.at(pointer).dump(),"unchanged owned scalar retains exact numeric representation");
    }
    require(actual_receipts.dump()==expected_receipts.dump(),"only changed known owned scalar receipts are invalidated; unaffected/opaque receipts remain exact");
    for(const auto* path:{"/base_position_m/0","/base_position_m/1","/going_m","/width_m","/top_landing/depth_m"}) {
        const Json::json_pointer pointer(path);result.properties[pointer]=original.properties.at(pointer);
    }
    result.properties["quantity_entries"]=receipts;
    require(result==original && result.properties.dump()==original.properties.dump() && result.extensions.dump()==original.extensions.dump(),
            "stair changes only five owned geometric scalars and their affected known receipts, preserving all other raw authoring");
    const auto old_model=AssemblyModel::from_json(before.entities().at(f.catalog).properties.at("model"));
    const auto new_model=AssemblyModel::from_json(after.entities().at(f.catalog).properties.at("model"));
    for(const auto& type:old_model.types()) {
        const auto found=std::find_if(new_model.types().begin(),new_model.types().end(),[&](const auto& candidate){return candidate.id==type.id;});
        require(found!=new_model.types().end() && *found==type,"component original shared catalog type remains exact");
    }
}
void unchanged(MainWindow& window,const DocumentSnapshot& before) {
    const auto after=window.document().snapshot();
    require(document_snapshot_digest(after)==document_snapshot_digest(before) && after.entities()==before.entities() && after.assets()==before.assets(),
            "detached preview/refusal preserves complete source, revision, save, retained history and assets");
}
void wait(MainWindow& window,const ModalGuard& guard,const std::function<bool()>& predicate,const char* message) {
    QElapsedTimer deadline;deadline.start();
    while(!predicate() && deadline.elapsed()<10000) {QApplication::processEvents(QEventLoop::AllEvents,20);guard.verify();}
    guard.verify();if(!predicate()) throw std::runtime_error(std::string(message)+": "+window.lastError().toStdString());
}
QImage output(PlanCanvas& drawing) {
    QImage image(drawing.size(),QImage::Format_ARGB32_Premultiplied);QPainter painter(&image);
    drawing.renderSceneAt(painter,image.rect(),drawing.viewScale(),drawing.viewCenter(),Qt::white);painter.end();return image;
}
void mouse(PlanCanvas& drawing,QEvent::Type kind,QPointF pixel) {
    QMouseEvent event(kind,pixel,drawing.mapToGlobal(pixel.toPoint()),kind==QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
        kind==QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton,Qt::ShiftModifier);
    QApplication::sendEvent(&drawing,&event);
}
void no_preview(const PlanCanvas& drawing) {
    require(!drawing.entityTransformPreviewPending() && drawing.entityTransformPreview().empty() &&
            !drawing.roofOpeningPreviewPending() && !drawing.roofOpeningPreviewBoundary() && !drawing.roofOpeningPreviewEdit() &&
            !drawing.roofOpeningGroupPreviewPending() && !drawing.roofOpeningGroupPreviewBoundary() &&
            !drawing.roofOpeningGroupPreviewEdit() && drawing.roofOpeningGroupPreviewControls().empty(),
            "release/review/refusal consumes all provisional preview tickets");
}
QPointF completed_preview(MainWindow& window,const Fixture& f,const DocumentSnapshot& before,const ModalGuard& guard,ResizeCase c) {
    auto& drawing=canvas(window);frame(drawing,f);roster(drawing,f);const auto committed=output(drawing);
    const auto presentations=drawing.selectedEntityPresentations();const auto generation=drawing.roofOpeningControlsGeneration();
    const auto grips=drawing.selectionAxisHandlePositions();
    const Vec2 side=c.depth ? Vec2{4,c.opposite ? 0.0 : 2.0} : Vec2{c.opposite ? 1.0 : 7.0,1};
    const auto side_presented=presented(side,f.site),center=drawing.viewCenter();
    const QPointF expected(drawing.width()/2.0+(side_presented.x-center.x)*drawing.viewScale(),
                           drawing.height()/2.0-(side_presented.y-center.y)*drawing.viewScale());
    const auto press=*std::min_element(grips.begin(),grips.end(),[&](QPointF a,QPointF b) {
        return std::hypot(a.x()-expected.x(),a.y()-expected.y())<std::hypot(b.x()-expected.x(),b.y()-expected.y());
    });
    const Vec2 physical_delta=c.depth ? Vec2{0,c.opposite ? -1.0 : 1.0} : Vec2{c.opposite ? -3.0 : 3.0,0};
    const Vec2 delta=f.site ? Vec2{-physical_delta.y,physical_delta.x} : physical_delta;
    const auto release=press+QPointF(delta.x*drawing.viewScale(),-delta.y*drawing.viewScale());
    require(drawing.rect().contains(press.toPoint()) && drawing.rect().contains(release.toPoint()),"real anchored side gesture remains inside native canvas");
    mouse(drawing,QEvent::MouseButtonPress,press);unchanged(window,before);frame(drawing,f);
    mouse(drawing,QEvent::MouseMove,release);require(drawing.entityTransformPreviewPending(),"actual side gesture starts asynchronous native manufacture");
    unchanged(window,before);require(output(drawing)==committed,"pending preview remains outside committed Print output");
    wait(window,guard,[&]{return !drawing.entityTransformPreviewPending() && !drawing.entityTransformPreview().empty();},"detached component/stair native preview finishes");
    c.resized=true;plans(drawing.entityTransformPreview(),f,c);plans(drawing.entities(),f);
    QStringList ids;for(const auto& item:drawing.entityTransformPreview()) if(!ids.contains(item.id)) ids.push_back(item.id);
    QStringList expected_ids{QString::fromStdString(f.component),QString::fromStdString(f.stair)};ids.sort();expected_ids.sort();
    require(ids==expected_ids,"detached native preview has precisely the original two-owner roster");
    unchanged(window,before);frame(drawing,f);roster(drawing,f);
    require(drawing.selectedEntityPresentations()==presentations && drawing.roofOpeningControlsGeneration()==generation,
            "detached manufacture preserves captured presentation identities and publication generation");
    require(output(drawing)==committed,"complete detached preview remains outside committed Print output");return release;
}
void mixed43_command(const DocumentSnapshot& after,const Fixture& f) {
    const auto& command=after.history().back().boundary_constraint_changes;
    require(command && command->mixed_selection_edit_completion && command->mixed_selection_edit_intent.at("version")==43,
            "actual release publishes sealed Properties43 completion");
    require(command_to_json(*command).at("version")==52,"retained outer command remains canonical command52");
    const auto& intent=command->mixed_selection_edit_intent;
    require(intent.size()==12 && intent.at("room_reviews").empty() && intent.at("phase_room_reviews").empty() &&
            intent.at("stair_axis_requests").size()==1,"canonical Properties43 retains one stair row and empty room reviews");
    for(const auto* key:{"version","expected_revision","source_snapshot_digest","source_authoring_digest","source_entities_digest",
        "source_saved_revision","component_request","component_identity_reservations","stair_axis_requests","room_reviews","phase_room_reviews","message"})
        require(intent.contains(key),"retained Properties43 has exactly its twelve mandatory source-bound fields");
    const auto& row=intent.at("stair_axis_requests").at(0);
    require(row.size()==5 && row.at("stair_id")==f.stair && row.contains("sx") && row.contains("sy") && row.contains("anchor_m") &&
            row.contains("frame_rotation_radians"),"canonical retained stair roster contains precisely the actual requested source stair");
    require(ProjectStore::required_format_version(after)==266 && ProjectStore::required_semantic_format_version(after)==266,
            "canonical retained command requires native and semantic reader266");
}
void floor266(const DocumentSnapshot& saved) {
    require(ProjectStore::required_format_version(saved)==266 && ProjectStore::required_semantic_format_version(saved)==266,
            "applied/undone/redone retained canonical command requires reader266");
}
void fresh_open(const QString& directory,const QString& stem,const DocumentSnapshot& saved,const Fixture& f,
                const ModalGuard& guard,bool reverse,ResizeCase c) {
    MainWindow reopened({},nullptr,QDir(directory).filePath(stem+"-reopened-library.json"));
    require(reopened.openProject(QDir(directory).filePath(stem+".bldproj")) && reopened.document().is_editable(),"ordinary fresh Open acquires actual editable writer ownership");
    display(reopened,guard,f);const auto loaded=reopened.document().snapshot();
    require(document_snapshot_digest(loaded)==document_snapshot_digest(saved) && loaded.entities()==saved.entities() && loaded.assets()==saved.assets(),
            "fresh editable Open retains exact saved source, assets, revision, save and complete retained history");floor266(loaded);
    auto* undo=reopened.findChild<QAction*>("undoCommand");
    require(undo && !undo->isEnabled() && !reopened.undoCommand() && !reopened.redoCommand(),"fresh editable Open has no Undo/Redo boundary despite retained history");
    unchanged(reopened,loaded);structure(loaded,f,c);select(reopened,f,reverse,guard);
    auto& drawing=canvas(reopened);plans(drawing.entities(),f,c);frame(drawing,f,c);roster(drawing,f);no_preview(drawing);
    capture(drawing,stem+"-reopened.png");guard.verify();
}
Json gesture_scenario(const QString& directory,bool reverse,bool depth,bool site,bool opposite=false) {
    const auto f=fixture(false,site);admit_source(f);ResizeCase c{true,depth,opposite};
    const auto stem=QString(site ? "site-" : "plan-")+(depth ? "depth-" : "width-")+
        (opposite ? "opposite-" : "")+(reverse ? "stair-first" : "component-first");
    std::cout<<"[stair desktop] "<<stem.toStdString()<<'\n'<<std::flush;
    ModalGuard guard;MainWindow window(f.document,nullptr,QDir(directory).filePath(stem+"-library.json"));display(window,guard,f);select(window,f,reverse,guard);
    auto& drawing=canvas(window);const auto before=window.document().snapshot();
    require(ProjectStore::required_format_version(before)<266 && ProjectStore::required_semantic_format_version(before)<266,"opaque command-shaped metadata does not create reader266 authority");
    structure(before,f);plans(drawing.entities(),f);const auto committed=output(drawing);capture(drawing,stem+"-selected.png");
    const auto release=completed_preview(window,f,before,guard,c);capture(drawing,stem+"-preview.png");mouse(drawing,QEvent::MouseButtonRelease,release);
    wait(window,guard,[&]{return window.document().revision()==before.revision()+1 && !drawing.entityTransformPreviewPending();},"one side release publishes both members exactly once");
    const auto after=window.document().snapshot();mixed43_command(after,f);
    require(after.history().size()==before.history().size()+1,"one release creates one retained mixed history event");
    no_preview(drawing);const auto result=structure(after,f,c);preserved(before,after,f);plans(drawing.entities(),f,c);frame(drawing,f,c);roster(drawing,f);
    require(output(drawing)!=committed,"release publishes changed native geometry into committed Print output");capture(drawing,stem+"-committed.png");
    const auto save=[&](const QString& state) {
        const auto current=window.document().snapshot();require(window.saveProjectAs(QDir(directory).filePath(stem+state+".bldproj")),"actual native lifecycle state saves");
        const auto saved=window.document().snapshot();require(saved.entities()==current.entities() && saved.assets()==current.assets(),"Save retains exact authoring graph and assets");floor266(saved);return saved;
    };
    const auto applied=save("-applied");
    require(window.undoCommand() && window.document().snapshot().entities()==before.entities() && window.document().snapshot().assets()==before.assets(),"one Undo restores exact original owners, all receipts and assets");
    const auto undo_state=window.document().snapshot();
    require(undo_state.revision()==after.revision()+1 && undo_state.history().size()==after.history().size()+1 && undo_state.history().back().action=="undo" &&
            undo_state.history().back().source_revision==before.revision(),"one Undo appends exactly one navigation event targeting the actual source");
    structure(undo_state,f);plans(drawing.entities(),f);frame(drawing,f);roster(drawing,f);require(output(drawing)==committed,"Undo restores exact original committed native output");
    const auto undone=save("-undone");
    require(window.redoCommand() && window.document().snapshot().entities()==after.entities() && window.document().snapshot().assets()==after.assets(),"one Redo restores exact resized owners, all retained envelopes and assets");
    const auto redo_state=window.document().snapshot();
    require(redo_state.revision()==after.revision()+2 && redo_state.history().size()==after.history().size()+2 && redo_state.history().back().action=="redo" &&
            redo_state.history().back().source_revision==after.revision(),"one Redo appends exactly one navigation event targeting the actual result");
    structure(redo_state,f,c);plans(drawing.entities(),f,c);frame(drawing,f,c);roster(drawing,f);
    const auto redone=save("-redone");require(window.createNewProject(),"release native writer leases before fresh ordinary Open");
    fresh_open(directory,stem+"-applied",applied,f,guard,reverse,c);
    fresh_open(directory,stem+"-undone",undone,f,guard,reverse,{});
    fresh_open(directory,stem+"-redone",redone,f,guard,reverse,c);guard.verify();return result;
}

void seed_history(Fixture& f) {
    const auto source=f.document->snapshot();auto retained=source.entities().at(f.component);retained.extensions["retained_history_fixture"]=true;
    f.document->apply(ApplyEntityChanges{source.revision(),{EntityChange::upsert(std::move(retained))},{},"Original stair cohort history authority"});
}
DocumentSnapshot replace_history(MainWindow& window,const DocumentSnapshot& before) {
    require(before.revision()>0,"history-only stale fixture has one actual retained source event");
    auto replacement=Document::fork_at_revision(before,before.revision()-1);std::vector<EntityChange> changes;
    for(const auto& [id,entity]:before.entities()) {(void)id;changes.push_back(EntityChange::upsert(entity));}
    replacement.apply(ApplyEntityChanges{replacement.revision(),std::move(changes),{},"Independently replaced stair cohort history authority"});
    const auto expected=replacement.snapshot();
    require(expected.document_id()==before.document_id() && expected.revision()==before.revision() && expected.entities()==before.entities() &&
            expected.assets()==before.assets() && expected.saved_revision_optional()==before.saved_revision_optional(),
            "history replacement retains exact identity, revision, save, geometry and assets");
    require(document_snapshot_digest(expected)!=document_snapshot_digest(before),"replacement changes complete retained history source digest");
    window.document()=std::move(replacement);return expected;
}
void settle_without_publication(MainWindow& window,const DocumentSnapshot& expected,const ModalGuard& guard) {
    QElapsedTimer deadline;deadline.start();
    do {QApplication::processEvents(QEventLoop::AllEvents,20);guard.verify();unchanged(window,expected);no_preview(canvas(window));} while(deadline.elapsed()<350);
}
void replace_presentation(PlanCanvas& drawing,const Fixture& f) {
    const auto original=drawing.selectedEntityPresentations();const auto committed=output(drawing);auto scene=drawing.entities();
    bool changed=false;
    for(auto& item:scene) if(item.id==QString::fromStdString(f.stair) && item.selected) {
        item.presentation_key+=QStringLiteral("-stale-identity");changed=true;
    }
    require(changed,"presentation fixture replaces an actual selected stair presentation");drawing.setEntities(std::move(scene));
    require(drawing.selectedEntityPresentations()!=original,"stale presentation changes the actual selected presentation identity");
    require(output(drawing)==committed,"stale presentation retains identical native ink and committed output");
}
enum class StaleAuthority {selection,viewport,presentation,history};
void stale_release(const QString& directory,StaleAuthority authority) {
    auto f=fixture();if(authority==StaleAuthority::history) seed_history(f);
    const auto stem=QStringLiteral("stale-")+(authority==StaleAuthority::selection ? "selection" : authority==StaleAuthority::viewport ? "viewport" :
        authority==StaleAuthority::presentation ? "presentation" : "history");
    std::cout<<"[stair desktop] "<<stem.toStdString()<<'\n'<<std::flush;
    ModalGuard guard;MainWindow window(f.document,nullptr,QDir(directory).filePath(stem+"-library.json"));display(window,guard,f);select(window,f,false,guard);
    auto& drawing=canvas(window);const auto before=window.document().snapshot();const auto committed=output(drawing);
    const auto center=drawing.viewCenter();const auto scale=drawing.viewScale();const auto source_scene=drawing.entities();
    const auto release=completed_preview(window,f,before,guard,{true,false,false});
    capture(drawing,stem+"-preview.png");auto expected=before;
    if(authority==StaleAuthority::selection) {
        require(window.selectEntity(QString::fromStdString(f.component)),"stale fixture changes real public selection");
        require(drawing.selectedIds()==QStringList{QString::fromStdString(f.component)},"stale selection removes stair from captured roster");
    } else if(authority==StaleAuthority::viewport) {
        drawing.zoomBy(1.2);require(drawing.viewScale()!=scale,"stale viewport changes actual scale");
    } else if(authority==StaleAuthority::presentation) {
        replace_presentation(drawing,f);
    } else expected=replace_history(window,before);
    QApplication::processEvents();guard.verify();unchanged(window,expected);mouse(drawing,QEvent::MouseButtonRelease,release);
    wait(window,guard,[&]{return !drawing.entityTransformPreviewPending() && drawing.entityTransformPreview().empty();},"stale release consumes complete detached candidate");
    settle_without_publication(window,expected,guard);structure(window.document().snapshot(),f);preserved(before,window.document().snapshot(),f);
    if(authority==StaleAuthority::presentation) drawing.setEntities(source_scene);
    drawing.setViewTransform(center,scale);select(window,f,false,guard);plans(drawing.entities(),f);frame(drawing,f);
    require(output(drawing)==committed,"stale refusal retains original committed output at restored view");capture(drawing,stem+"-refused.png");guard.verify();
}

enum class ReviewAction {cancel,retire,stale_presentation,stale_history};
class DimensionReviewDriver {
public:
    DimensionReviewDriver(MainWindow& window,const Fixture& f,const DocumentSnapshot& before,const QImage& committed,ReviewAction action)
        : m_window(window),m_fixture(f),m_before(before),m_output(committed),m_action(action) {
        m_timer.setInterval(10);QObject::connect(&m_timer,&QTimer::timeout,[this] {
            auto* dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget());if(!dialog) return;
            try {
                require(dialog->objectName()==QStringLiteral("AssemblyProfileDimensionReviewDialog") || dialog->windowTitle()==QStringLiteral("Review component dimensions"),
                        "stair cohort release opens actual component dimension review");
                require(++m_reviews==1,"one affected source dimension enters exactly one actual review");
                auto& drawing=canvas(m_window);no_preview(drawing);unchanged(m_window,m_before);
                require(output(drawing)==m_output,"nested review has unchanged committed output and consumes provisional preview");
                roster(drawing,m_fixture);plans(drawing.entities(),m_fixture);structure(m_window.document().snapshot(),m_fixture);
                auto* list=dialog->findChild<QListWidget*>();
                require(list && list->count()==1 && list->item(0)->toolTip()==QString::fromStdString(m_fixture.dimension),"actual review identifies original source profile carrier");
                auto* buttons=dialog->findChild<QDialogButtonBox*>();
                require(buttons && buttons->button(QDialogButtonBox::Cancel) && buttons->button(QDialogButtonBox::Ok),"real review exposes actual Cancel and Apply");
                require(!buttons->button(QDialogButtonBox::Ok)->isEnabled(),"actual Apply requires explicit dimension decision");
                if(m_action==ReviewAction::cancel) {
                    buttons->button(QDialogButtonBox::Cancel)->click();
                    require(dialog->result()==QDialog::Rejected && !dialog->isVisible(),"real Cancel rejects the complete component/stair cohort");return;
                }
                auto* remove=dialog->findChild<QPushButton*>("dimensionRemove");
                if(!remove) for(auto* button:dialog->findChildren<QPushButton*>()) if(button->text()==QStringLiteral("Remove dimension")) {remove=button;break;}
                require(remove && remove->isEnabled(),"real review offers enabled Remove dimension decision");
                remove->click();unchanged(m_window,m_before);no_preview(drawing);
                require(output(drawing)==m_output && buttons->button(QDialogButtonBox::Ok)->isEnabled(),"explicit Remove enables Apply while source and output remain exact");
                if(m_action==ReviewAction::stale_presentation || m_action==ReviewAction::stale_history) {
                    QObject::connect(dialog,&QDialog::accepted,dialog,[this] {
                        try {
                            if(m_action==ReviewAction::stale_history) m_expected=replace_history(m_window,m_before);
                            else {replace_presentation(canvas(m_window),m_fixture);m_expected=m_window.document().snapshot();}
                        } catch(const std::exception& error) {m_failure=error.what();}
                    });
                }
                buttons->button(QDialogButtonBox::Ok)->click();
                require(dialog->result()==QDialog::Accepted && !dialog->isVisible(),"explicit retirement accepts through actual Apply");
            } catch(const std::exception& error) {if(m_failure.empty()) m_failure=error.what();dialog->reject();}
              catch(...) {if(m_failure.empty()) m_failure="unknown native stair review callback failure";dialog->reject();}
        });m_timer.start();
    }
    void verify() const {
        if(!m_failure.empty()) throw std::runtime_error(m_failure);
        require(m_reviews==1,"release executes exactly one real native dimension review");
        if(m_action==ReviewAction::stale_presentation || m_action==ReviewAction::stale_history)
            require(m_expected.has_value(),"accepted stale callback actually replaces captured source authority");
    }
    DocumentSnapshot expected() const {return m_expected.value_or(m_before);}
private:
    MainWindow& m_window;const Fixture& m_fixture;const DocumentSnapshot& m_before;const QImage& m_output;
    ReviewAction m_action;QTimer m_timer;int m_reviews=0;std::string m_failure;std::optional<DocumentSnapshot> m_expected;
};
void dimension_review(const QString& directory,ReviewAction action) {
    auto f=fixture(true);if(action==ReviewAction::stale_history) seed_history(f);admit_source(f);
    const auto stem=QStringLiteral("review-")+(action==ReviewAction::cancel ? "cancel" : action==ReviewAction::retire ? "remove" :
        action==ReviewAction::stale_presentation ? "stale-presentation" : "stale-history");
    std::cout<<"[stair desktop] "<<stem.toStdString()<<'\n'<<std::flush;
    ModalGuard guard;MainWindow window(f.document,nullptr,QDir(directory).filePath(stem+"-library.json"));display(window,guard,f);select(window,f,false,guard);
    auto& drawing=canvas(window);const auto before=window.document().snapshot();
    const auto dimension=decode_boundary_dimension_entity(before.entities().at(f.dimension));
    require(dimension.supported() && dimension.dimension->kind==BoundaryDimensionKind::assembly_profile,"review fixture has a real supported original-topology profile dimension");
    require_near(dimension.dimension->resolve(before).segment_length(),2,"review source edge measures two metres");
    const auto committed=output(drawing);const auto release=completed_preview(window,f,before,guard,{true,false,false});capture(drawing,stem+"-preview.png");
    guard.pause();DimensionReviewDriver review(window,f,before,committed,action);mouse(drawing,QEvent::MouseButtonRelease,release);review.verify();
    QApplication::setActiveWindow(&window);guard.resume();
    if(action!=ReviewAction::retire) {
        const auto expected=review.expected();
        wait(window,guard,[&]{return !drawing.entityTransformPreviewPending() && drawing.entityTransformPreview().empty();},"Cancel/stale review consumes entire detached cohort");
        settle_without_publication(window,expected,guard);structure(window.document().snapshot(),f);preserved(before,window.document().snapshot(),f);
        require(output(drawing)==committed,"Cancel/stale review retains exact committed output");
        require_near(dimension.dimension->resolve(window.document().snapshot()).segment_length(),2,"Cancel/stale review retains exact original live dimension");
        if(action!=ReviewAction::cancel) require(!window.lastError().isEmpty(),"accepted stale review reports refused publication");
        capture(drawing,stem+"-refused.png");
    } else {
        wait(window,guard,[&]{return window.document().revision()==before.revision()+1 && !drawing.entityTransformPreviewPending();},"reviewed retirement publishes both owners in one revision");
        const auto after=window.document().snapshot();mixed43_command(after,f);no_preview(drawing);
        require(after.history().size()==before.history().size()+1 && !after.entities().contains(f.dimension),"one reviewed event combines both owners and explicit dimension retirement");
        structure(after,f,{true,false,false});preserved(before,after,f,true);plans(drawing.entities(),f,{true,false,false});frame(drawing,f,{true,false,false});roster(drawing,f);
        require(output(drawing)!=committed,"actual reviewed Apply publishes changed committed output");capture(drawing,stem+"-committed.png");
        require(window.undoCommand() && window.document().snapshot().entities()==before.entities() && window.document().snapshot().assets()==before.assets(),"one reviewed Undo restores both original owners, exact dimension and assets");
        require(output(drawing)==committed,"reviewed Undo restores exact committed source output");
        require(window.redoCommand() && window.document().snapshot().entities()==after.entities() && window.document().snapshot().assets()==after.assets(),"one reviewed Redo restores both resized owners and explicit dimension retirement");
        require(window.saveProjectAs(QDir(directory).filePath(stem+".bldproj")),"reviewed cohort retirement saves");
        const auto saved=window.document().snapshot();floor266(saved);require(window.createNewProject(),"release reviewed writer lease before editable Open");
        fresh_open(directory,stem,saved,f,guard,false,{true,false,false});
    }
    review.verify();guard.verify();
}
} // namespace
int main(int argc,char** argv) {
    sketch::testing::noninteractive_errors();qputenv("QT_QPA_PLATFORM","offscreen");QStandardPaths::setTestModeEnabled(true);
    QApplication application(argc,argv);QTemporaryDir settings;
    QCoreApplication::setOrganizationName("VertexTests");QCoreApplication::setApplicationName("MixedComponentStairResize-"+QUuid::createUuid().toString(QUuid::WithoutBraces));
    QSettings::setDefaultFormat(QSettings::IniFormat);QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,settings.path());QSettings::setPath(QSettings::IniFormat,QSettings::SystemScope,settings.path());
    try {
        require(settings.isValid(),"private temporary settings exist");const auto font=QFontDatabase::addApplicationFont(":/fonts/Inter.ttf");require(font>=0,"bundled Inter loads");
        const auto families=QFontDatabase::applicationFontFamilies(font);require(!families.isEmpty(),"Inter family exists");application.setFont(QFont(families.front(),10));
        QTemporaryDir directory;require(directory.isValid(),"private fixture directory exists");
        const bool frame_only=application.arguments().contains("--frame-only");
        const bool site_only=application.arguments().contains("--site-only");
        const bool review_only=application.arguments().contains("--review-only");
        const bool stale_only=application.arguments().contains("--stale-only");
        const auto forward=scenario(directory.path(),false);const auto reverse=scenario(directory.path(),true);
        if(!forward.empty()||!reverse.empty())throw std::runtime_error(forward+(forward.empty()||reverse.empty()?"":"\n")+reverse);
        if(!frame_only) {
            if(!review_only && !stale_only) for(const bool site:{false,true}) for(const bool depth:{false,true}) {
                if(site_only && !site) continue;
                const auto first=gesture_scenario(directory.path(),false,depth,site);
                const auto second=gesture_scenario(directory.path(),true,depth,site);same_structure(first,second);
                // Outward left/bottom gestures prove actual base XY movement in
                // both presentations; ordinary gestures already qualify orders.
                gesture_scenario(directory.path(),false,depth,site,true);
            }
            if(!site_only && !stale_only) for(const auto action:{ReviewAction::cancel,ReviewAction::retire,ReviewAction::stale_presentation,ReviewAction::stale_history})
                dimension_review(directory.path(),action);
            if(!site_only && !review_only) for(const auto authority:{StaleAuthority::selection,StaleAuthority::viewport,StaleAuthority::presentation,StaleAuthority::history})
                stale_release(directory.path(),authority);
        }
        std::cout<<"mixed_component_stair_resize_desktop_tests"<<(frame_only ? " frame admission" : " native lifecycle")<<" passed\n";return 0;
    }catch(const std::exception& error) {std::cerr<<"mixed_component_stair_resize_desktop_tests: "<<error.what()<<'\n';return 1;}
}
