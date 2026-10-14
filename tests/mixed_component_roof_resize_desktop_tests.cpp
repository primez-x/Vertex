#include "sketch/desktop/main_window.hpp"
#include "sketch/architecture.hpp"
#include "sketch/assembly_document_adapter.hpp"
#include "sketch/assembly_geometry.hpp"
#include "sketch/building_entity.hpp"
#include "sketch/boundary_dimension.hpp"
#include "sketch/document_digest.hpp"
#include "sketch/project_store.hpp"
#include "sketch/phase_roof_resize.hpp"
#include <BRepBndLib.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
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
struct Fixture {
    std::shared_ptr<Document> document;
    std::string property=fresh("property"),building=fresh("building"),floor=fresh("floor"),layer=fresh("layer");
    std::string component=fresh("component"),roof=fresh("roof"),catalog=fresh("catalog"),type=fresh("type");
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
    AssemblyType type;type.id=f.type;type.name="Independent roof cohort component";type.profiles={profile};
    Entity catalog{f.catalog,"assembly_model",{{"model",AssemblyModel::create({}, {type}, {}).to_json()}}};
    catalog.extensions={{"opaque_fixture",{{"retain","catalog"},{"values",Json::array({17,true,"source"})}}}};
    catalog.extensions["opaque_command"]={{"version",52},{"mixed_selection_edit_intent",{{"version",44}}}};
    AssemblyInstance instance;instance.id=f.component;instance.type_id=f.type;instance.root_transform=AssemblyTransform{{5,1,.75},0,1};
    auto component=encode_document_assembly_instance({f.component,"assembly_instance",context},{f.catalog,instance});
    SlopedRoofPanel authored;authored.id=f.roof;authored.base_position={1,0,4};authored.orientation_radians=0;
    authored.run=2;authored.span=2;authored.rise=0;authored.pitch_radians=0;authored.overhang=0;authored.thickness=.25;
    auto roof=encode_building_entity(authored);
    const auto receipt=[](const char* expression,int numerator,int denominator) {
        return Json{{"version",1},{"original_expression",expression},{"entered_unit","m"},
            {"exact_metres",{{"numerator",numerator},{"denominator",denominator}}}};
    };
    roof.properties["quantity_entries"]={
        {"/base_position_m/0",receipt("1 m",1,1)},{"/base_position_m/1",receipt("0 m",0,1)},
        {"/run_m",receipt("2 m",2,1)},{"/span_m",receipt("2 m",2,1)},
        {"/thickness_m",receipt("0.25 m",1,4)},
        {"/future_dimension_m",{{"version",9},{"opaque",Json::array({"retain",17})}}}};
    for(auto* entity:{&component,&roof}) {
        for(auto it=context.begin();it!=context.end();++it) entity->properties[it.key()]=it.value();
        entity->extensions={{"opaque_fixture",{{"retain",entity->id},{"values",Json::array({19,true,"source"})}}}};
    }
    auto asset=Asset::create(fresh("asset"),"application/octet-stream",{std::byte{0x31},std::byte{0x42}},{{"opaque_fixture",{{"retain","asset"}}}});
    std::vector<Entity> entities{
        {f.property,"property",{{"name","Roof cohort"}},false},
        {f.building,"building",{{"property_id",f.property},{"name","Building"}},false},
        {f.floor,"floor",{{"building_id",f.building},{"name","Ground"},{"elevation_m",0}},false},
        {f.layer,"layer",{{"floor_id",f.floor},{"name","Plan"}},false},catalog,component,roof};
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
            {"vertical_datum",{{"identifier","roof-fixture-datum"},{"height_at_origin_m",1500}}}};
        entities[1].properties["site_placement"]={{"version",1},{"translation_m",{10,20,3}},
            {"rotation_radians",0}};
        for(auto& entity:entities) if(entity.id==f.component || entity.id==f.roof)
            entity.properties["presentation_frame"]={{"version",1},{"mode","building"}};
    }
    f.document=std::make_shared<Document>(Document::create(std::move(entities),{std::move(asset)}));
    const auto source=f.document->snapshot();auto retained=source.entities().at(f.component);
    retained.extensions["retained_history_fixture"]=true;
    f.document->apply(ApplyEntityChanges{source.revision(),{EntityChange::upsert(std::move(retained))},{},"Original roof cohort history authority"});
    return f;
}
void native_box(const TopoDS_Shape& shape,Vec3 minimum,Vec3 maximum,double volume) {
    require(!shape.IsNull(),"source manufactures a genuine native solid");require_near(solid_volume(shape),volume,"native hand volume");
    Bnd_Box box;BRepBndLib::Add(shape,box);require(!box.IsVoid(),"native shape has actual bounds");
    double x0,y0,z0,x1,y1,z1;box.Get(x0,y0,z0,x1,y1,z1);
    // OCC bounds include the shape tolerance; vertices below use the stricter hand tolerance.
    const auto bound=[](double actual,double expected) {require(std::isfinite(actual)&&std::abs(actual-expected)<1e-6,"actual native bounding box matches hand bounds");};
    bound(x0,minimum.x);bound(y0,minimum.y);bound(z0,minimum.z);bound(x1,maximum.x);bound(y1,maximum.y);bound(z1,maximum.z);
    std::vector<Vec3> vertices;
    for(TopExp_Explorer it(shape,TopAbs_VERTEX);it.More();it.Next()) {
        const auto p=BRep_Tool::Pnt(TopoDS::Vertex(it.Current()));vertices.push_back({p.X(),p.Y(),p.Z()});
        require((std::abs(p.X()-minimum.x)<1e-7||std::abs(p.X()-maximum.x)<1e-7)&&
                (std::abs(p.Y()-minimum.y)<1e-7||std::abs(p.Y()-maximum.y)<1e-7)&&
                (std::abs(p.Z()-minimum.z)<1e-7||std::abs(p.Z()-maximum.z)<1e-7),"native vertices are actual hand prism corners");
    }
    for(double x:{minimum.x,maximum.x})for(double y:{minimum.y,maximum.y})for(double z:{minimum.z,maximum.z})
        require(std::any_of(vertices.begin(),vertices.end(),[&](Vec3 p){return std::abs(p.x-x)<1e-7&&std::abs(p.y-y)<1e-7&&std::abs(p.z-z)<1e-7;}),"each hand corner exists in the native solid");
}
void admit_source(const Fixture& f) {
    const auto source=f.document->snapshot();const auto& entity=source.entities().at(f.roof);
    require(entity.properties.at("version")==1&&entity.properties.at("form")=="sloped_roof_panel","ordinary canonical V1 roof source");
    for(const auto* key:{"base_position_m","orientation_rad","run_m","span_m","rise_m","pitch_rad","overhang_m","thickness_m"})
        require(entity.properties.contains(key),"roof codec retains the typed source property");
    const auto roof=std::get<SlopedRoofPanel>(decode_building_entity(entity));require(roof.id==f.roof&&roof.openings.empty(),"typed roof owner has no openings");
    require_near(roof.base_position.x,1,"roof base X");require_near(roof.base_position.y,0,"roof base Y");require_near(roof.base_position.z,4,"roof base Z");
    require_near(roof.orientation_radians,0,"roof orientation");require_near(roof.run,2,"roof run");require_near(roof.span,2,"roof span");
    require_near(roof.rise,0,"roof rise");require_near(roof.pitch_radians,0,"roof pitch");require_near(roof.overhang,0,"roof overhang");require_near(roof.thickness,.25,"roof thickness");
    native_box(make_building_shape(roof),{1,0,3.75},{3,2,4},1);
    AssemblyExpansionBudget budget;const auto expansion=expand_document_assembly_instance(source.entities().at(f.component),source.entities(),budget);
    require(!expansion.source_instance.placement&&expansion.profiles.size()==1,"independent component expands one profile without a host");
    const auto& expanded=expansion.profiles.front();const auto& profile=expanded.profile;const auto& pose=expanded.transform;
    require(expanded.part_path.empty()&&profile.outer.size()==4&&profile.holes.empty()&&(f.dimension.empty()==!profile.topology.has_value())&&!profile.planar_affine,"direct ordinary rectangular component profile");
    require_near(profile.elevation_m,.25,"component elevation");require_near(profile.height_m,1.5,"component height");
    require_near(pose.translation_m.x,5,"component root X");require_near(pose.translation_m.y,1,"component root Y");require_near(pose.translation_m.z,.75,"component root Z");
    require_near(pose.rotation_radians,0,"component yaw");require_near(pose.scale,1,"component scale");require_near(pose.vertical_scale,1,"component vertical scale");require(!pose.mirrored_y,"component is unmirrored");
    const auto local=rectangle(0,0,2,1),world=rectangle(5,1,2,1);
    for(std::size_t i=0;i<4;++i) {
        const auto& edge=profile.outer[i];require_near(edge.sweep_radians,0,"component straight edge");
        require_near(edge.start.x,local[i].start.x,"component local corner X");require_near(edge.start.y,local[i].start.y,"component local corner Y");
        require_near(edge.end.x,local[i].end.x,"component local end X");require_near(edge.end.y,local[i].end.y,"component local end Y");
        require_near(pose.translation_m.x+pose.scale*edge.start.x,world[i].start.x,"expanded world corner X");
        require_near(pose.translation_m.y+pose.scale*edge.start.y,world[i].start.y,"expanded world corner Y");
    }
    require_near(pose.translation_m.z+pose.scale*pose.vertical_scale*profile.elevation_m,1,"component world base Z");
    require_near(pose.scale*pose.vertical_scale*profile.height_m,1.5,"component world height");
    const auto geometry=make_assembly_geometry(expansion);require(geometry.solids.size()==1,"one genuine component native solid");
    require_near(geometry.volume_m3,3,"component geometry volume");native_box(geometry.solids.front().shape,{5,1,1},{7,2,2.5},3);
}
class ModalGuard {
    QTimer timer;QString title;
public:
    ModalGuard() {timer.setInterval(10);QObject::connect(&timer,&QTimer::timeout,[this] {
        if(auto* dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget())) {title=dialog->windowTitle();if(title.isEmpty())title="unnamed dialog";dialog->setAttribute(Qt::WA_DontShowOnScreen);dialog->reject();}
    });timer.start();}
    void verify() const {if(!title.isEmpty())throw std::runtime_error("Unexpected roof regression modal: "+title.toStdString());}
    void pause() {timer.stop();}
    void resume() {timer.start();}
};
void settle(const ModalGuard& guard) {for(int i=0;i<3;++i) {QApplication::sendPostedEvents(nullptr,QEvent::LayoutRequest);QApplication::processEvents(QEventLoop::AllEvents,20);guard.verify();}}
PlanCanvas& canvas(MainWindow& window) {
    const auto name=window.workspace()==Workspace::architectural ? "architecturalPlanCanvas" : "measurementPlanCanvas";
    auto* result=dynamic_cast<PlanCanvas*>(window.findChild<QWidget*>(name));require(result,"actual active PlanCanvas exists");return *result;
}
// Authenticate each hand outline against actual native projected ink.
// This flat-panel fixture makes no claim about nonzero-slope native extents.
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
        require(reach>=length-1e-7,"published native source geometry covers each independent hand outline edge");
    }
}
void published(const PlanCanvas& drawing,const Fixture& f) {
    QStringList expected{QString::fromStdString(f.component),QString::fromStdString(f.roof)},actual=drawing.selectedIds(),owners;
    actual.sort();expected.sort();require(actual==expected&&drawing.selectedRoofOpenings().empty(),"exact sorted public owner selection without openings");
    for(const auto& item:drawing.entities())if(item.selected&&!owners.contains(item.id))owners.push_back(item.id);
    owners.sort();require(owners==expected,"exact selected canvas membership independent of publication order");
    for(const auto& id:{f.component,f.roof}) {
        Boundary ink;
        for(const auto& item:drawing.entities())if(item.id==QString::fromStdString(id)) {
            require(item.selected&&item.holes.empty(),"selected ordinary source publication without holes");ink.insert(ink.end(),item.segments.begin(),item.segments.end());
        }
        require(!ink.empty(),"both owners publish genuine native ink");
        if(id==f.component){require(ink.size()==4,"component publishes four profile edges");covered(ink,rectangle(5,1,2,1));}
        else covered(ink,rectangle(1,0,2,2)); // Native roof projection may publish more than four edges.
    }
}
QImage output(PlanCanvas& drawing);
void unchanged(MainWindow& window,const DocumentSnapshot& before);
void no_preview(const PlanCanvas& drawing);
void mouse(PlanCanvas& drawing,QEvent::Type kind,QPointF pixel);
std::string scenario(const QString& directory,bool reverse) {
    auto f=fixture();admit_source(f);const auto before=f.document->snapshot();
    const QString stem=reverse ? "roof-frame-roof-component" : "roof-frame-component-roof";
    ModalGuard guard;MainWindow window(f.document,nullptr,QDir(directory).filePath(stem+"-library.json"));
    window.setAttribute(Qt::WA_DontShowOnScreen);window.resize(1400,900);window.setWorkspace(Workspace::measurement);window.setMetricUnits(true);window.show();settle(guard);
    auto* snap=window.findChild<QToolButton*>("snapTool");require(snap,"actual Snap toggle exists");snap->setChecked(false);
    auto& drawing=canvas(window);drawing.setOverviewMapEnabled(false);drawing.setGridEnabled(false);drawing.setTool(CanvasTool::select);
    drawing.setSnapEnabled(false);drawing.setWallSnapEnabled(false);drawing.setViewTransform({4,1},60);settle(guard);
    const auto committed=output(drawing);require(!committed.isNull(),"actual Print committed scene renders");
    require(window.selectEntity({}),"clear public selection");
    QStringList requested{QString::fromStdString(f.component),QString::fromStdString(f.roof)};if(reverse)std::reverse(requested.begin(),requested.end());
    for(const auto& id:requested)require(window.selectEntity(id,true),"public selection admits each ordinary owner");settle(guard);
    published(drawing,f);unchanged(window,before);no_preview(drawing);
    const auto painted=drawing.grab();require(!painted.isNull(),"actual selected canvas paints");
    const auto captures=qEnvironmentVariable("VERTEX_TEST_CAPTURE_DIR");
    if(!captures.isEmpty())require(QDir().mkpath(captures)&&painted.save(QDir(captures).filePath(stem+"-selected.png")),"optional role-prefixed native capture saves");
    const auto frame=drawing.selectionAxisResizeCohortFrame();const auto grips=drawing.selectionAxisHandlePositions();
    unchanged(window,before);no_preview(drawing);require(output(drawing)==committed,"selection/render/frame leave Print committed output unchanged");guard.verify();
    if(!frame)return stem.toStdString()+": MissingFrame: independent component + ordinary V1 sloped roof panel must expose one common four-side resize frame";
    require_near(frame->center.x,4,"hand union centre X");require_near(frame->center.y,1,"hand union centre Y");
    require_near(frame->width_metres,6,"hand union width");require_near(frame->depth_metres,2,"hand union depth");require_near(frame->rotation_radians,0,"hand union rotation");
    require(grips.size()==4,"common frame publishes four side grips");
    for(std::size_t i=0;i<grips.size();++i) {
        require(std::isfinite(grips[i].x())&&std::isfinite(grips[i].y()),"finite actual grips");
        for(std::size_t j=0;j<i;++j)require(std::hypot(grips[i].x()-grips[j].x(),grips[i].y()-grips[j].y())>1,"four distinct actual grips");
    }
    mouse(drawing,QEvent::MouseButtonPress,grips.front());
    mouse(drawing,QEvent::MouseButtonRelease,grips.front());settle(guard);
    unchanged(window,before);no_preview(drawing);require(output(drawing)==committed,"no-op side release creates no archive, output or history change");
    return {};
}
// Independent hand operators. The opposite-side cases move the roof's base:
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
    if(!directory.isEmpty()) require(QDir().mkpath(directory) && painted.save(QDir(directory).filePath(name.startsWith("roof-") ? name : "roof-"+name)),"optional native roof capture saves");
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
    auto actual=drawing.selectedIds();QStringList expected{QString::fromStdString(f.component),QString::fromStdString(f.roof)};
    actual.sort();expected.sort();require(actual==expected && drawing.selectedRoofOpenings().empty(),
        "actual selection has exactly the independent component and ordinary roof");
    QStringList published_ids;
    for(const auto& item:drawing.entities()) if(item.selected && !published_ids.contains(item.id)) published_ids.push_back(item.id);
    published_ids.sort();require(published_ids==expected,"selected published presentations have exactly the two source owners");
}
void select(MainWindow& window,const Fixture& f,bool reverse,const ModalGuard& guard) {
    require(window.selectEntity({}),"clear actual public selection");
    QStringList requested{QString::fromStdString(f.component),QString::fromStdString(f.roof)};
    if(reverse) std::reverse(requested.begin(),requested.end());
    for(const auto& id:requested) require(window.selectEntity(id,true),"actual public selection admits each ordinary owner");
    settle(guard);roster(canvas(window),f);
}
void plans(const std::vector<CanvasEntity>& scene,const Fixture& f,ResizeCase c={}) {
    for(const auto& id:{f.component,f.roof}) {
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
    const auto& entity=source.entities().at(f.roof);
    require(entity.properties.at("version")==1 && entity.properties.at("form")=="sloped_roof_panel","canonical flat panel authoring");
    const auto roof=std::get<SlopedRoofPanel>(decode_building_entity(entity));
    const auto base=hand({1,0},c),opposite_corner=hand({3,2},c);
    require_near(roof.base_position.x,base.x,"hand roof base X");require_near(roof.base_position.y,base.y,"hand roof base Y");
    require_near(roof.base_position.z,4,"authored roof Z");require_near(roof.run,opposite_corner.x-base.x,"hand roof run");
    require_near(roof.span,opposite_corner.y-base.y,"hand roof span");
    require_near(roof.orientation_radians,0,"roof yaw");require_near(roof.rise,0,"flat rise");require_near(roof.pitch_radians,0,"flat pitch");
    require_near(roof.overhang,0,"roof overhang");require_near(roof.thickness,.25,"roof thickness");require(roof.openings.empty(),"no roof openings");
    native_box(make_building_shape(roof),{base.x,base.y,3.75},{opposite_corner.x,opposite_corner.y,4},c.resized ? 1.5 : 1);
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
    const auto low=hand({5,1},c),high=hand({7,2},c);
    native_box(geometry.solids.front().shape,{low.x,low.y,1},{high.x,high.y,2.5},c.resized ? 4.5 : 3);
    return {{"component",vertices},{"roof",{base.x,base.y,4,roof.run,roof.span,0,.25}}};
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
    require(before.entities().size()==after.entities().size()+(retired ? 1 : 0),"roof adds no owner/child identity or unrelated entity");
    for(const auto& [id,entity]:before.entities()) {
        if(retired && id==f.dimension) {require(!after.entities().contains(id),"only explicitly reviewed dimension is retired");continue;}
        require(after.entities().contains(id),"complete source inventory remains present");
        const auto& actual=after.entities().at(id);
        if(id!=f.component && id!=f.catalog && id!=f.roof) require(actual==entity,"unselected context and unrelated carriers remain exact");
        else if(id!=f.roof) require(actual.extensions.dump()==entity.extensions.dump(),"owner/catalog opaque extension envelopes remain exact");
        if(id==f.component || id==f.roof) {
            for(const auto* key:{"property_id","building_id","floor_id","layer_id"}) require(actual.properties.at(key)==entity.properties.at(key),"owner drawing contexts remain exact");
            if(f.site) require(actual.properties.at("presentation_frame").dump()==entity.properties.at("presentation_frame").dump(),"Site owner retains exact building presentation frame");
        }
    }
    const auto& original=before.entities().at(f.roof);const auto& result=after.entities().at(f.roof);
    if(result!=original) {
        const auto& wire=after.history().back().boundary_constraint_changes->mixed_selection_edit_intent.at("roof_plan_resizes").at(0);
        const auto operation=decode_roof_plan_resize_intent(wire);
        require(result==replay_roof_plan_resize_entity(original,operation),"complete roof equals exact replay of original source and actual command operator");
        validate_roof_plan_resize_derivations(result);
        const auto& archive=result.extensions.at(std::string(roof_plan_resize_derivations_key));
        require(archive.at("version")==4 && wire.at("version")==2 && wire.at("anchor_mode")=="shared_frame",
            "new roof proof retains archive4 and the exact shared-frame operator");
        require(archive.at("operations").size()==1 && archive.at("operations").at(0).at("operation")==wire,"one canonical archive records actual operator");
        auto retained=original.properties.at("quantity_entries"),archived=Json::object();
        for(const auto* path:{"/base_position_m/0","/base_position_m/1","/run_m","/span_m"}) {
            const Json::json_pointer pointer(path);
            if(original.properties.at(pointer)!=result.properties.at(pointer)) {archived[path]=retained.at(path);retained.erase(path);}
        }
        require(result.properties.at("quantity_entries")==retained &&
            archive.at("operations").at(0).at("receipts").at("quantity_entries")==archived,"changed known receipts archive verbatim; unaffected opaque receipts remain current");
        auto extensions=result.extensions;extensions.erase(std::string(roof_plan_resize_derivations_key));
        require(extensions==original.extensions,"only owned roof derivation archive changes opaque extension envelope");
    }
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
QPointF completed_preview(MainWindow& window,const Fixture& f,const DocumentSnapshot& before,const ModalGuard& guard,ResizeCase c,bool await_native=true) {
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
    if(!await_native)return release;
    wait(window,guard,[&]{return !drawing.entityTransformPreviewPending() && !drawing.entityTransformPreview().empty();},"detached component/roof native preview finishes");
    c.resized=true;plans(drawing.entityTransformPreview(),f,c);plans(drawing.entities(),f);
    QStringList ids;for(const auto& item:drawing.entityTransformPreview()) if(!ids.contains(item.id)) ids.push_back(item.id);
    QStringList expected_ids{QString::fromStdString(f.component),QString::fromStdString(f.roof)};ids.sort();expected_ids.sort();
    require(ids==expected_ids,"detached native preview has precisely the original two-owner roster");
    unchanged(window,before);frame(drawing,f);roster(drawing,f);
    require(drawing.selectedEntityPresentations()==presentations && drawing.roofOpeningControlsGeneration()==generation,
            "detached manufacture preserves captured presentation identities and publication generation");
    require(output(drawing)==committed,"complete detached preview remains outside committed Print output");return release;
}
void mixed44_command(const DocumentSnapshot& after,const Fixture& f,const DocumentSnapshot& before) {
    const auto& command=after.history().back().boundary_constraint_changes;
    require(command && command->mixed_selection_edit_completion && command->mixed_selection_edit_intent.at("version")==44,
            "actual release publishes sealed Properties44 completion");
    require(command_to_json(*command).at("version")==52,"retained outer command remains canonical command52");
    const auto& intent=command->mixed_selection_edit_intent;
    require(intent.size()==12 && intent.at("room_reviews").empty() && intent.at("phase_room_reviews").empty() &&
            intent.at("roof_plan_resizes").size()==1,"canonical Properties44 retains one roof row and empty room reviews");
    for(const auto* key:{"version","expected_revision","source_snapshot_digest","source_authoring_digest","source_entities_digest",
        "source_saved_revision","component_request","component_identity_reservations","roof_plan_resizes","room_reviews","phase_room_reviews","message"})
        require(intent.contains(key),"retained Properties44 has exactly its twelve mandatory source-bound fields");
    const auto& row=intent.at("roof_plan_resizes").at(0);
    require(row.size()==7 && row.at("version")==2 && row.at("anchor_mode")=="shared_frame" &&
        row==encode_roof_plan_resize_intent(decode_roof_plan_resize_intent(row)) && row.at("roof_id")==f.roof,
        "one closed RoofPlanResizeIntent v2 row identifies actual roof and shared anchor");
    const auto& component=intent.at("component_request");
    require(intent.at("expected_revision")==before.revision() &&
        intent.at("source_snapshot_digest")==document_snapshot_digest(before) &&
        intent.at("source_authoring_digest")==document_authoring_source_digest_v2(before) &&
        intent.at("source_entities_digest")==entity_map_digest(before.entities()) &&
        intent.at("source_saved_revision")==(before.saved_revision_optional() ? Json(*before.saved_revision_optional()) : Json(nullptr)),
        "all five source fences bind the complete actual original snapshot");
    for(const auto* key:{"expected_revision","source_snapshot_digest","source_authoring_digest","source_entities_digest","source_saved_revision"})
        require(component.at(key)==intent.at(key),"component and mixed command retain all five identical original-source fences");
    require(ProjectStore::required_format_version(after)==267 && ProjectStore::required_semantic_format_version(after)==267,
            "canonical retained command requires native and semantic reader267");
}
void floor267(const DocumentSnapshot& saved) {
    require(ProjectStore::required_format_version(saved)==267 && ProjectStore::required_semantic_format_version(saved)==267,
            "applied/undone/redone retained canonical command requires reader267");
}
void fresh_open(const QString& directory,const QString& stem,const DocumentSnapshot& saved,const Fixture& f,
                const ModalGuard& guard,bool reverse,ResizeCase c) {
    MainWindow reopened({},nullptr,QDir(directory).filePath(stem+"-reopened-library.json"));
    require(reopened.openProject(QDir(directory).filePath(stem+".bldproj")) && reopened.document().is_editable(),"ordinary fresh Open acquires actual editable writer ownership");
    display(reopened,guard,f);const auto loaded=reopened.document().snapshot();
    require(document_snapshot_digest(loaded)==document_snapshot_digest(saved) && loaded.entities()==saved.entities() && loaded.assets()==saved.assets(),
            "fresh editable Open retains exact saved source, assets, revision, save and complete retained history");floor267(loaded);
    auto* undo=reopened.findChild<QAction*>("undoCommand");
    require(undo && !undo->isEnabled() && !reopened.undoCommand() && !reopened.redoCommand(),"fresh editable Open has no Undo/Redo boundary despite retained history");
    unchanged(reopened,loaded);structure(loaded,f,c);select(reopened,f,reverse,guard);
    auto& drawing=canvas(reopened);plans(drawing.entities(),f,c);frame(drawing,f,c);roster(drawing,f);no_preview(drawing);
    capture(drawing,stem+"-reopened.png");guard.verify();
}
Json gesture_scenario(const QString& directory,bool reverse,bool depth,bool site,bool opposite=false) {
    const auto f=fixture(false,site);admit_source(f);ResizeCase c{true,depth,opposite};
    const auto stem=QString(site ? "roof-site-" : "roof-plan-")+(depth ? "depth-" : "width-")+
        (opposite ? "opposite-" : "")+(reverse ? "roof-first" : "component-first");
    std::cout<<"[roof desktop] "<<stem.toStdString()<<'\n'<<std::flush;
    ModalGuard guard;MainWindow window(f.document,nullptr,QDir(directory).filePath(stem+"-library.json"));display(window,guard,f);select(window,f,reverse,guard);
    auto& drawing=canvas(window);const auto before=window.document().snapshot();
    require(ProjectStore::required_format_version(before)<267 && ProjectStore::required_semantic_format_version(before)<267,"opaque command-shaped metadata does not create reader267 authority");
    structure(before,f);plans(drawing.entities(),f);const auto committed=output(drawing);capture(drawing,stem+"-selected.png");
    const auto release=completed_preview(window,f,before,guard,c);capture(drawing,stem+"-preview.png");mouse(drawing,QEvent::MouseButtonRelease,release);
    wait(window,guard,[&]{return window.document().revision()==before.revision()+1 && !drawing.entityTransformPreviewPending();},"one side release publishes both members exactly once");
    const auto after=window.document().snapshot();mixed44_command(after,f,before);
    require(after.history().size()==before.history().size()+1,"one release creates one retained mixed history event");
    no_preview(drawing);const auto result=structure(after,f,c);preserved(before,after,f);plans(drawing.entities(),f,c);frame(drawing,f,c);roster(drawing,f);
    require(output(drawing)!=committed,"release publishes changed native geometry into committed Print output");capture(drawing,stem+"-committed.png");
    const auto save=[&](const QString& state) {
        const auto current=window.document().snapshot();require(window.saveProjectAs(QDir(directory).filePath(stem+state+".bldproj")),"actual native lifecycle state saves");
        const auto saved=window.document().snapshot();require(saved.entities()==current.entities() && saved.assets()==current.assets(),"Save retains exact authoring graph and assets");floor267(saved);return saved;
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

DocumentSnapshot replace_history(MainWindow& window,const DocumentSnapshot& before,bool geometry=false) {
    require(before.revision()>0,"history-only stale fixture has one actual retained source event");
    auto replacement=Document::fork_at_revision(before,before.revision()-1);std::vector<EntityChange> changes;
    for(const auto& [id,entity]:before.entities()) {
        auto copy=entity;if(geometry && copy.properties.value("form",std::string{})=="sloped_roof_panel") {
            copy.properties["base_position_m"][0]=1.25;copy.properties["quantity_entries"].erase("/base_position_m/0");
        }
        (void)id;changes.push_back(EntityChange::upsert(std::move(copy)));
    }
    replacement.apply(ApplyEntityChanges{replacement.revision(),std::move(changes),{},"Independently replaced roof cohort history authority"});
    const auto expected=replacement.snapshot();
    require(expected.document_id()==before.document_id() && expected.revision()==before.revision() && (geometry || expected.entities()==before.entities()) &&
            expected.assets()==before.assets() && expected.saved_revision_optional()==before.saved_revision_optional(),
            "history replacement retains exact identity, revision, save, geometry and assets");
    require(document_snapshot_digest(expected)!=document_snapshot_digest(before),"replacement changes complete retained history source digest");
    if(geometry)require(expected.entities()!=before.entities(),"same-ID/revision replacement actually changes source geometry");
    window.document()=std::move(replacement);return expected;
}
void settle_without_publication(MainWindow& window,const DocumentSnapshot& expected,const ModalGuard& guard) {
    QElapsedTimer deadline;deadline.start();
    do {QApplication::processEvents(QEventLoop::AllEvents,20);guard.verify();unchanged(window,expected);no_preview(canvas(window));} while(deadline.elapsed()<350);
}
void replace_presentation(PlanCanvas& drawing,const Fixture& f) {
    const auto original=drawing.selectedEntityPresentations();const auto committed=output(drawing);auto scene=drawing.entities();
    bool changed=false;
    for(auto& item:scene) if(item.id==QString::fromStdString(f.roof) && item.selected) {
        item.presentation_key+=QStringLiteral("-stale-identity");changed=true;
    }
    require(changed,"presentation fixture replaces an actual selected roof presentation");drawing.setEntities(std::move(scene));
    require(drawing.selectedEntityPresentations()!=original,"stale presentation changes the actual selected presentation identity");
    require(output(drawing)==committed,"stale presentation retains identical native ink and committed output");
}
enum class StaleAuthority {selection,viewport,presentation,history,geometry};
void stale_release(const QString& directory,StaleAuthority authority) {
    auto f=fixture();
    const auto stem=QStringLiteral("roof-stale-")+(authority==StaleAuthority::selection ? "selection" : authority==StaleAuthority::viewport ? "viewport" :
        authority==StaleAuthority::presentation ? "presentation" : authority==StaleAuthority::geometry ? "same-id-revision-geometry" : "history");
    std::cout<<"[roof desktop] "<<stem.toStdString()<<'\n'<<std::flush;
    ModalGuard guard;MainWindow window(f.document,nullptr,QDir(directory).filePath(stem+"-library.json"));display(window,guard,f);select(window,f,false,guard);
    auto& drawing=canvas(window);const auto before=window.document().snapshot();const auto committed=output(drawing);
    const auto center=drawing.viewCenter();const auto scale=drawing.viewScale();const auto source_scene=drawing.entities();
    const auto release=completed_preview(window,f,before,guard,{true,false,false},false);
    capture(drawing,stem+"-preview.png");auto expected=before;
    if(authority==StaleAuthority::selection) {
        require(window.selectEntity(QString::fromStdString(f.component)),"stale fixture changes real public selection");
        require(drawing.selectedIds()==QStringList{QString::fromStdString(f.component)},"stale selection removes roof from captured roster");
    } else if(authority==StaleAuthority::viewport) {
        drawing.zoomBy(1.2);require(drawing.viewScale()!=scale,"stale viewport changes actual scale");
    } else if(authority==StaleAuthority::presentation) {
        replace_presentation(drawing,f);
    } else expected=replace_history(window,before,authority==StaleAuthority::geometry);
    QApplication::processEvents();guard.verify();unchanged(window,expected);mouse(drawing,QEvent::MouseButtonRelease,release);
    wait(window,guard,[&]{return !drawing.entityTransformPreviewPending() && drawing.entityTransformPreview().empty();},"stale release consumes complete detached candidate");
    settle_without_publication(window,expected,guard);
    if(authority==StaleAuthority::geometry) {
        require(output(drawing)==committed,"late callback cannot publish replaced same-ID/revision source geometry");
        capture(drawing,stem+"-refused.png");guard.verify();return;
    }
    if(authority!=StaleAuthority::geometry) {structure(window.document().snapshot(),f);preserved(before,window.document().snapshot(),f);}
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
                        "roof cohort release opens actual component dimension review");
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
                    require(dialog->result()==QDialog::Rejected && !dialog->isVisible(),"real Cancel rejects the complete component/roof cohort");return;
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
              catch(...) {if(m_failure.empty()) m_failure="unknown native roof review callback failure";dialog->reject();}
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
    auto f=fixture(true);admit_source(f);
    const auto stem=QStringLiteral("roof-review-")+(action==ReviewAction::cancel ? "cancel" : action==ReviewAction::retire ? "remove" :
        action==ReviewAction::stale_presentation ? "stale-presentation" : "stale-history");
    std::cout<<"[roof desktop] "<<stem.toStdString()<<'\n'<<std::flush;
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
        const auto after=window.document().snapshot();mixed44_command(after,f,before);no_preview(drawing);
        require(after.history().size()==before.history().size()+1 && !after.entities().contains(f.dimension),"one reviewed event combines both owners and explicit dimension retirement");
        structure(after,f,{true,false,false});preserved(before,after,f,true);plans(drawing.entities(),f,{true,false,false});frame(drawing,f,{true,false,false});roster(drawing,f);
        require(output(drawing)!=committed,"actual reviewed Apply publishes changed committed output");capture(drawing,stem+"-committed.png");
        require(window.undoCommand() && window.document().snapshot().entities()==before.entities() && window.document().snapshot().assets()==before.assets(),"one reviewed Undo restores both original owners, exact dimension and assets");
        require(output(drawing)==committed,"reviewed Undo restores exact committed source output");
        require(window.redoCommand() && window.document().snapshot().entities()==after.entities() && window.document().snapshot().assets()==after.assets(),"one reviewed Redo restores both resized owners and explicit dimension retirement");
        require(window.saveProjectAs(QDir(directory).filePath(stem+".bldproj")),"reviewed cohort retirement saves");
        const auto saved=window.document().snapshot();floor267(saved);require(window.createNewProject(),"release reviewed writer lease before editable Open");
        fresh_open(directory,stem,saved,f,guard,false,{true,false,false});
    }
    review.verify();guard.verify();
}
} // namespace
int main(int argc,char** argv) {
    sketch::testing::noninteractive_errors();qputenv("QT_QPA_PLATFORM","offscreen");QStandardPaths::setTestModeEnabled(true);
    QApplication application(argc,argv);QTemporaryDir settings;
    QCoreApplication::setOrganizationName("VertexTests");QCoreApplication::setApplicationName("MixedComponentRoofResize-"+QUuid::createUuid().toString(QUuid::WithoutBraces));
    QSettings::setDefaultFormat(QSettings::IniFormat);QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,settings.path());QSettings::setPath(QSettings::IniFormat,QSettings::SystemScope,settings.path());
    try {
        require(settings.isValid(),"private temporary settings exist");const auto font=QFontDatabase::addApplicationFont(":/fonts/Inter.ttf");require(font>=0,"bundled Inter loads");
        const auto families=QFontDatabase::applicationFontFamilies(font);require(!families.isEmpty(),"Inter family exists");application.setFont(QFont(families.front(),10));
        QTemporaryDir directory;require(directory.isValid(),"private fixture directory exists");
        const bool frame_only=application.arguments().contains("--frame-only");
        const bool site_only=application.arguments().contains("--site-only");
        const bool review_only=application.arguments().contains("--review-only");
        const bool stale_only=application.arguments().contains("--stale-only");
        const bool left_only=application.arguments().contains("--left-only");
        const auto forward=scenario(directory.path(),false);const auto reverse=scenario(directory.path(),true);
        if(!forward.empty()||!reverse.empty())throw std::runtime_error(forward+(forward.empty()||reverse.empty()?"":"\n")+reverse);
        if(left_only) gesture_scenario(directory.path(),false,false,false,true);
        else if(!frame_only) {
            if(!review_only && !stale_only) for(const bool site:{false,true}) for(const bool depth:{false,true}) {
                if(site_only && !site) continue;
                const auto first=gesture_scenario(directory.path(),false,depth,site);
                const auto second=gesture_scenario(directory.path(),true,depth,site);same_structure(first,second);
                const auto opposite_first=gesture_scenario(directory.path(),false,depth,site,true);
                const auto opposite_second=gesture_scenario(directory.path(),true,depth,site,true);same_structure(opposite_first,opposite_second);
            }
            if(!site_only && !stale_only) for(const auto action:{ReviewAction::cancel,ReviewAction::retire,ReviewAction::stale_presentation,ReviewAction::stale_history})
                dimension_review(directory.path(),action);
            if(!site_only && !review_only) for(const auto authority:{StaleAuthority::selection,StaleAuthority::viewport,StaleAuthority::presentation,StaleAuthority::history,StaleAuthority::geometry})
                stale_release(directory.path(),authority);
        }
        std::cout<<"mixed_component_roof_resize_desktop_tests"<<(frame_only ? " frame admission" : " native lifecycle")<<" passed\n";return 0;
    }catch(const std::exception& error) {std::cerr<<"mixed_component_roof_resize_desktop_tests: "<<error.what()<<'\n';return 1;}
}
