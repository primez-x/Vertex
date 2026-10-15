#include "sketch/boundary_dimension.hpp"
#include "sketch/project_store.hpp"
#include "sketch/project_exchange.hpp"
#include "sketch/site_frame.hpp"
#include "sketch/phase_roof_resize.hpp"
#include "sketch/slab_geometry_edit.hpp"
#include "sketch/roof_opening_group_edit.hpp"
#include <QAction>
#include <QDialogButtonBox>
#include <QElapsedTimer>
#include <QListWidget>
#include <QPushButton>
#include <QMouseEvent>
#include <filesystem>
#include <fstream>
#include <functional>
#include <numbers>
#include <optional>
#include "sketch/desktop/main_window.hpp"
#include "sketch/architecture.hpp"
#include "sketch/document_solid.hpp"
#include <array>
#include "sketch/assembly_document_adapter.hpp"
#include "sketch/assembly_instance_property_edit.hpp"
#include "sketch/assembly_geometry.hpp"
#include "sketch/building_entity.hpp"
#include "sketch/document_digest.hpp"
#include <BRepBndLib.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include "../src/desktop/plan_canvas.hpp"
#include "support/noninteractive_errors.hpp"
#include <QApplication>
#include <QDialog>
#include <QDir>
#include <QEventLoop>
#include <QFontDatabase>
#include <QFont>
#include <QImage>
#include <QPainter>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolButton>
#include <QUuid>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <iostream>
#include <memory>
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
enum class Cohort {roof_structural,roof_slab_structural,roof_slab_child,roof_structural_child};
const char* cohort_name(Cohort cohort) {
    switch(cohort) {
    case Cohort::roof_structural:return "roof-structural";
    case Cohort::roof_slab_structural:return "roof-slab-structural";
    case Cohort::roof_slab_child:return "roof-slab-child";
    case Cohort::roof_structural_child:return "roof-structural-child";
    }
    throw std::runtime_error("unknown qualification cohort");
}
struct Fixture {
    std::shared_ptr<Document> document;
    std::string property=fresh("property"),building=fresh("building"),floor=fresh("floor"),layer=fresh("layer");
    std::string component=fresh("component"),roof=fresh("roof"),catalog=fresh("catalog"),type=fresh("type");
    std::string column=fresh("column"),slab=fresh("slab"),host=fresh("child-host"),child=fresh("skylight"),sibling=fresh("sibling"),dimension;
    Cohort cohort=Cohort::roof_structural;
    bool has_slab() const {return cohort==Cohort::roof_slab_structural || cohort==Cohort::roof_slab_child;}
    bool has_column() const {return cohort!=Cohort::roof_slab_child;}
    bool has_child() const {return cohort==Cohort::roof_slab_child || cohort==Cohort::roof_structural_child;}
    double depth() const {return has_column()?4.0:3.0;}
    bool site=false;
};
Fixture fixture(bool with_dimension=false,bool site=false,Cohort cohort=Cohort::roof_structural) {
    Fixture f;f.site=site;f.cohort=cohort;
    const Json context={{"property_id",f.property},{"building_id",f.building},{"floor_id",f.floor},{"layer_id",f.layer}};
    AssemblyProfile profile;profile.id=fresh("profile");profile.outer=rectangle(0,0,2,1);profile.elevation_m=.25;profile.height_m=1.5;
    if(with_dimension) profile.topology=AssemblyProfileTopology{
        {"source-outer",{"source-e0","source-e1","source-e2","source-e3"},
                        {"source-v0","source-v1","source-v2","source-v3"}}, {}};
    AssemblyType type;type.id=f.type;type.name="Independent roof cohort component";type.profiles={profile};
    Entity catalog{f.catalog,"assembly_model",{{"model",AssemblyModel::create({}, {type}, {}).to_json()}}};
    catalog.extensions={{"opaque_fixture",{{"retain","catalog"},{"values",Json::array({17,true,"source"})}}}};
    catalog.extensions["opaque_command"]={{"version",52},{"mixed_selection_edit_intent",{{"version",44}}}};
    catalog.extensions["opaque_future_command"]={{"version",52},{"mixed_selection_edit_intent",{{"version",47}}}};
    AssemblyInstance instance;instance.id=f.component;instance.type_id=f.type;instance.root_transform=AssemblyTransform{{5,1,.75},0,1};
    auto component=encode_document_assembly_instance({f.component,"assembly_instance",context},{f.catalog,instance});
    SlopedRoofPanel authored;authored.id=f.roof;authored.base_position={1,0,4};authored.orientation_radians=0;
    authored.run=2;authored.span=2;authored.rise=0;authored.pitch_radians=0;authored.overhang=0;authored.thickness=.25;
    auto roof=encode_building_entity(authored);

    // The native producer offsets by half-width/depth from this base center.
    auto column=encode_building_entity(RectangularColumn{f.column,{4,3.5,.5},2,1,2,0});
    auto slab_properties=context;
    auto boundary=Json::array();
    for(const auto& edge:rectangle(1,0,3,3))
        boundary.push_back({{"start",{edge.start.x,edge.start.y}},{"end",{edge.end.x,edge.end.y}},{"sweep_radians",edge.sweep_radians}});
    slab_properties["boundary"]=boundary;slab_properties["holes"]=Json::array();
    slab_properties["elevation_m"]=1.25;slab_properties["thickness_m"]=.2;
    slab_properties["layers"]=Json::array({{{"id",fresh("slab-core")},{"thickness_m",.2}}});
    Entity slab{f.slab,"slab",std::move(slab_properties),false};
    SlopedRoofPanel containing{f.host,{-5,-5,3},0,20,20,0,0,0,.2,
        {{f.child,7,6,2,1,RoofSkylight{.06,.15,.024},0},
         {f.sibling,15,15,1,1,RoofSkylight{.06,.15,.024},0}}};
    auto host=encode_building_entity(containing);host.properties["version"]=4;
    for(auto& row:host.properties.at("roof_openings")) {
        row["rotation_rad"]=0.0;row["opaque_child_metadata"]={{"retain",row.at("id")}};
    }
    const auto receipt=[](const char* expression,int numerator,int denominator) {
        return Json{{"version",1},{"original_expression",expression},{"entered_unit","m"},
            {"exact_metres",{{"numerator",numerator},{"denominator",denominator}}}};
    };
    roof.properties["quantity_entries"]={
        {"/base_position_m/0",receipt("1 m",1,1)},{"/base_position_m/1",receipt("0 m",0,1)},
        {"/run_m",receipt("2 m",2,1)},{"/span_m",receipt("2 m",2,1)},
        {"/thickness_m",receipt("0.25 m",1,4)},
        {"/future_dimension_m",{{"version",9},{"opaque",Json::array({"retain",17})}}}};
    column.properties["quantity_entries"]={
        {"/base_center_m/0",receipt("4 m",4,1)},
        {"/base_center_m/1",receipt("3.5 m",7,2)},
        {"/width_m",receipt("2 m",2,1)},{"/depth_m",receipt("1 m",1,1)},
        {"/height_m",receipt("2 m",2,1)},
        {"/future_dimension_m",{{"version",9},{"opaque",Json::array({"retain",23})}}}};
    for(auto* entity:{&component,&roof,&column,&slab,&host}) {
        for(auto it=context.begin();it!=context.end();++it) entity->properties[it.key()]=it.value();
        entity->extensions={{"opaque_fixture",{{"retain",entity->id},{"values",Json::array({19,true,"source"})}}}};
    }
    auto asset=Asset::create(fresh("asset"),"application/octet-stream",{std::byte{0x31},std::byte{0x42}},{{"opaque_fixture",{{"retain","asset"}}}});
    std::vector<Entity> entities{
        {f.property,"property",{{"name","Roof cohort"}},false},
        {f.building,"building",{{"property_id",f.property},{"name","Building"}},false},
        {f.floor,"floor",{{"building_id",f.building},{"name","Ground"},{"elevation_m",0}},false},
        {f.layer,"layer",{{"floor_id",f.floor},{"name","Plan"}},false},catalog,component,roof};
    if(f.has_column())entities.push_back(column);
    if(f.has_slab())entities.push_back(slab);
    if(f.has_child())entities.push_back(host);
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
            {"vertical_datum",{{"identifier","roof-physical-fixture-datum"},{"height_at_origin_m",1500}}}};
        entities[1].properties["site_placement"]={{"version",1},{"translation_m",{10,20,3}},
            {"rotation_radians",0}};
        for(auto& entity:entities) if(entity.id==f.component || entity.id==f.roof || entity.id==f.column || entity.id==f.slab || entity.id==f.host)
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
    if(f.has_column()) {
        const auto& column_entity=source.entities().at(f.column);
        require(column_entity.type=="column" && column_entity.properties.at("version")==1 &&
                column_entity.properties.at("form")=="rectangular_column","canonical V1 rectangular column source");
        for(const auto* key:{"base_center_m","width_m","depth_m","height_m","rotation_rad"})
            require(column_entity.properties.contains(key),"column codec retains the typed source property");
        const auto column=std::get<RectangularColumn>(decode_building_entity(column_entity));
        require(column.id==f.column,"actual typed column owner identity");
        require_near(column.base_center.x,4,"column base center X");
        require_near(column.base_center.y,3.5,"column base center Y");
        require_near(column.base_center.z,.5,"column base Z");
        require_near(column.width,2,"column width");require_near(column.depth,1,"column depth");
        require_near(column.height,2,"column height");require_near(column.rotation_radians,0,"column rotation");
        native_box(make_building_shape(column),{3,3,.5},{5,4,2.5},4);
    }

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
QImage output(PlanCanvas& drawing) {
    QImage image(drawing.size(),QImage::Format_ARGB32_Premultiplied);QPainter painter(&image);
    drawing.renderSceneAt(painter,image.rect(),drawing.viewScale(),drawing.viewCenter(),Qt::white);painter.end();return image;
}
void unchanged(MainWindow& window,const DocumentSnapshot& before) {
    const auto after=window.document().snapshot();
    require(document_snapshot_digest(after)==document_snapshot_digest(before) && after.entities()==before.entities() &&
            after.assets()==before.assets() && after.saved_revision_optional()==before.saved_revision_optional() &&
            after.revision()==before.revision() && after.history().size()==before.history().size(),
            "selection preserves complete source, opaque metadata, history, save authority and assets");
}
void no_preview(const PlanCanvas& drawing) {
    require(!drawing.entityTransformPreviewPending() && drawing.entityTransformPreview().empty() &&
            !drawing.roofOpeningPreviewPending() && !drawing.roofOpeningPreviewBoundary() && !drawing.roofOpeningPreviewEdit() &&
            !drawing.roofOpeningGroupPreviewPending() && !drawing.roofOpeningGroupPreviewBoundary() &&
            !drawing.roofOpeningGroupPreviewEdit() && drawing.roofOpeningGroupPreviewControls().empty(),
            "initial admission has no provisional preview");
}
struct ResizeCase {bool resized=false,depth=false,opposite=false;double source_depth=4;};
Vec2 hand(Vec2 p,ResizeCase c) {
    if(!c.resized) return p;
    // Fixed hand coordinates keep the oracle independent of product affine helpers.
    const auto lookup=[](double coordinate,const auto& source,const auto& result) {
        const auto found=std::find(source.begin(),source.end(),coordinate);
        require(found!=source.end(),"hand fixture coordinate has an explicit oracle entry");
        return result.at(static_cast<std::size_t>(found-source.begin()));
    };
    if(c.depth) {
        const std::array source{0.,1.,2.,3.,4.};
        const std::array top{0.,1.5,3.,4.5,6.},bottom4{-2.,-.5,1.,2.5,4.},bottom3{-1.5,0.,1.5,3.,4.5};
        p.y=lookup(p.y,source,c.opposite?(c.source_depth==4?bottom4:bottom3):top);
    } else {
        const std::array source{1.,2.,3.,4.,5.,6.,7.};
        const std::array right{1.,2.5,4.,5.5,7.,8.5,10.},left{-2.,-.5,1.,2.5,4.,5.5,7.};
        p.x=lookup(p.x,source,c.opposite?left:right);
    }
    return p;
}
Vec2 presented(Vec2 p,bool site) {return site ? Vec2{80-p.y,210+p.x} : p;}
Boundary hand_boundary(Boundary boundary,ResizeCase c,bool site=false) {
    for(auto& edge:boundary) {edge.start=presented(hand(edge.start,c),site);edge.end=presented(hand(edge.end,c),site);}
    return boundary;
}

QStringList ordinary_ids(const Fixture& f) {
    QStringList ids{QString::fromStdString(f.component),QString::fromStdString(f.roof)};
    if(f.has_column())ids.push_back(QString::fromStdString(f.column));
    if(f.has_slab())ids.push_back(QString::fromStdString(f.slab));
    ids.sort();return ids;
}
std::vector<std::string> members(const Fixture& f) {
    std::vector<std::string> ids{f.component,f.roof};
    if(f.has_slab())ids.push_back(f.slab);
    if(f.has_column())ids.push_back(f.column);
    if(f.has_child())ids.push_back(f.child);
    return ids;
}
const CanvasRoofOpeningControls& controls(const PlanCanvas& drawing,const Fixture& f,const std::string& child,std::optional<Revision> revision=std::nullopt) {
    const auto& all=drawing.roofOpeningControls();
    const auto found=std::find_if(all.begin(),all.end(),[&](const auto& item){return item.target.roof_id==QString::fromStdString(f.host)&&item.target.opening_id==QString::fromStdString(child);});
    require(found!=all.end(),"actual qualified mouth controls exist");
    require((!revision || found->target.source_revision==*revision)&&found->presentation_owner_id==QString::fromStdString(f.host),"controls qualify current revision and actual host");
    require(std::count_if(all.begin(),all.end(),[&](const auto& item){return item.target==found->target;})==1,"unambiguous qualified mouth");return *found;
}
void capture(PlanCanvas& drawing,const QString& name) {
    const auto directory=qEnvironmentVariable("VERTEX_TEST_CAPTURE_DIR");
    const auto painted=drawing.grab();require(!painted.isNull(),"actual selected canvas paints");
    static const auto run=QStringLiteral("properties47-")+QUuid::createUuid().toString(QUuid::WithoutBraces);
    const auto unique=QDir(directory).filePath(run);
    if(!directory.isEmpty()) require(QDir().mkpath(unique) && painted.save(QDir(unique).filePath(name.startsWith("roof-physical-") ? name : "roof-physical-"+name)),"optional native roof capture saves");
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
    drawing.setViewTransform(presented({4,f.depth()/2},f.site),45);settle(guard);
}

void roster(const PlanCanvas& drawing,const Fixture& f) {
    auto actual=drawing.selectedIds();actual.sort();const auto expected=ordinary_ids(f);
    require(actual==expected,"exact complete ordinary component/whole-roof/slab/structural roster");
    const auto children=f.has_child()?std::vector{controls(drawing,f,f.child).target}:std::vector<CanvasRoofOpeningTarget>{};
    require(drawing.selectedRoofOpenings()==children,"exact qualified different-host child roster");
    require(f.has_child()?drawing.selectedRoofOpening() && *drawing.selectedRoofOpening()==children.front():!drawing.selectedRoofOpening(),
        "single qualified child authority agrees with full child roster");
    QStringList published;
    for(const auto& item:drawing.entities())if(item.selected&&!published.contains(item.id))published.push_back(item.id);
    published.sort();require(published==expected,"published ordinary selection never promotes child host or sibling");
}
void select_order(MainWindow& window,const Fixture& f,const std::vector<int>& order,const ModalGuard& guard) {
    require(window.selectEntity({}),"clear public ordinary and child selection");
    const auto ids=members(f);auto& drawing=canvas(window);QStringList owners;bool child=false;
    for(const int index:order) {
        const auto& id=ids.at(static_cast<std::size_t>(index));
        if(id!=f.child) {
            require(window.selectEntity(QString::fromStdString(id),true),"public selection admits ordered semantic owner");
            owners.push_back(QString::fromStdString(id));
        } else {
            const auto mouth=controls(drawing,f,f.child,window.document().revision()).frame.center,center=drawing.viewCenter();
            const auto pixel=QRectF(drawing.rect()).center()+QPointF((mouth.x-center.x)*drawing.viewScale(),-(mouth.y-center.y)*drawing.viewScale());
            require(drawing.rect().contains(pixel.toPoint()),"actual typed skylight click lies inside canvas");
            for(const auto kind:{QEvent::MouseButtonPress,QEvent::MouseButtonRelease}) {
                QMouseEvent event(kind,pixel,drawing.mapToGlobal(pixel.toPoint()),Qt::LeftButton,
                    kind==QEvent::MouseButtonRelease?Qt::NoButton:Qt::LeftButton,Qt::ControlModifier);
                QApplication::sendEvent(&drawing,&event);
            }
            child=true;
        }
        settle(guard);auto actual=drawing.selectedIds();actual.sort();owners.sort();
        require(actual==owners,"every selection prefix retains exact ordinary owner roster");
        require(drawing.selectedRoofOpenings()==(child?std::vector{controls(drawing,f,f.child).target}:std::vector<CanvasRoofOpeningTarget>{}),
            "every selection prefix retains exact qualified child roster");
    }
    roster(drawing,f);
}
void select(MainWindow& window,const Fixture& f,int first,const ModalGuard& guard) {
    const auto count=static_cast<int>(members(f).size());std::vector<int> order;
    for(int i=0;i<count;++i)order.push_back((first+i)%count);
    select_order(window,f,order,guard);
}
void mouth_controls(const PlanCanvas& drawing,const Fixture& f,Revision revision,ResizeCase c={}) {
    if(!f.has_child())return;
    const auto& mouth=controls(drawing,f,f.child,revision).frame;
    const auto low=hand({2,1},c),high=hand({4,2},c);
    const auto center=presented({(low.x+high.x)/2,(low.y+high.y)/2},f.site);
    require_near(mouth.center.x,center.x,"actual qualified mouth center X");
    require_near(mouth.center.y,center.y,"actual qualified mouth center Y");
    require_near(mouth.width_metres,high.x-low.x,"actual qualified mouth width");
    require_near(mouth.depth_metres,high.y-low.y,"actual qualified mouth depth");
    require_near(mouth.rotation_radians,0,"flat authored mouth angle");
    require_near(mouth.reference_along.x,f.site?0:1,"mouth actual source along X");
    require_near(mouth.reference_along.y,f.site?1:0,"mouth actual source along Y");
    require_near(mouth.reference_across.x,f.site?-1:0,"mouth actual source across X");
    require_near(mouth.reference_across.y,f.site?0:1,"mouth actual source across Y");
    const auto& sibling=controls(drawing,f,f.sibling,revision).frame;const auto sibling_center=presented({10.5,10.5},f.site);
    require_near(sibling.center.x,sibling_center.x,"untouched sibling world center X");
    require_near(sibling.center.y,sibling_center.y,"untouched sibling world center Y");
    require_near(sibling.width_metres,1,"untouched sibling width");require_near(sibling.depth_metres,1,"untouched sibling depth");
}
void plans(const std::vector<CanvasEntity>& scene,const Fixture& f,ResizeCase c={}) {
    auto ids=ordinary_ids(f);if(f.has_child())ids.push_back(QString::fromStdString(f.host));
    for(const auto& id:ids) {
        Boundary ink;
        for(const auto& item:scene)if(item.id==id)ink.insert(ink.end(),item.segments.begin(),item.segments.end());
        require(!ink.empty(),"all actual physical contributions publish native ink");
        if(id==QString::fromStdString(f.component))covered(ink,hand_boundary(rectangle(5,1,2,1),c,f.site));
        else if(id==QString::fromStdString(f.roof))covered(ink,hand_boundary(rectangle(1,0,2,2),c,f.site));
        else if(id==QString::fromStdString(f.column))covered(ink,hand_boundary(rectangle(3,3,2,1),c,f.site));
        else if(id==QString::fromStdString(f.slab))covered(ink,hand_boundary(rectangle(1,0,3,3),c,f.site));
        else {
            covered(ink,hand_boundary(rectangle(-5,-5,20,20),{},f.site));
            covered(ink,hand_boundary(rectangle(10,10,1,1),{},f.site));
            covered(ink,hand_boundary(rectangle(2,1,2,1),c,f.site));
        }
    }
}
void frame(const PlanCanvas& drawing,const Fixture& f,ResizeCase c={}) {
    const auto actual=drawing.selectionAxisResizeCohortFrame();require(actual.has_value(),"all captured semantic owners expose a common physical frame");
    const auto minimum=hand({1,0},c),maximum=hand({7,f.depth()},c);
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
    Json result={{"component",vertices},{"roof",{base.x,base.y,4,roof.run,roof.span,0,.25}}};
    if(f.has_slab()) {
        Slab slab;std::string error;
        require(read_document_slab(source.entities().at(f.slab),slab,error),"resized slab retains actual editable typed authoring");
        require(slab.boundary.size()==4 && slab.holes.empty() && slab.layers.size()==1,"slab retains complete straight single-layer boundary");
        auto physical=slab.boundary;const auto slab_expected=hand_boundary(rectangle(1,0,3,3),c);
        if(slab.planar_affine) {
            const auto& a=*slab.planar_affine;
            const auto mapped=[&](Vec2 p) {const double x=p.x-a.source_origin.x,y=p.y-a.source_origin.y;
                return Vec2{a.target_origin.x+a.xx*x+a.xy*y,a.target_origin.y+a.yx*x+a.yy*y};};
            for(auto& edge:physical){edge.start=mapped(edge.start);edge.end=mapped(edge.end);}
        }
        auto slab_vertices=Json::array();
        for(std::size_t i=0;i<4;++i) {
            require_near(physical[i].start.x,slab_expected[i].start.x,"slab hand corner X");
            require_near(physical[i].start.y,slab_expected[i].start.y,"slab hand corner Y");
            require_near(physical[i].end.x,slab_expected[i].end.x,"slab hand end X");
            require_near(physical[i].end.y,slab_expected[i].end.y,"slab hand end Y");
            require_near(slab.boundary[i].sweep_radians,0,"slab source edges stay straight");
            slab_vertices.push_back({physical[i].start.x,physical[i].start.y});
        }
        require_near(slab.elevation,1.25,"slab elevation retained");require_near(slab.thickness,.2,"slab thickness retained");
        const auto slab_low=hand({1,0},c),slab_high=hand({4,3},c);
        native_box(make_slab(slab),{slab_low.x,slab_low.y,1.25},{slab_high.x,slab_high.y,1.45},c.resized?2.7:1.8);
        result["slab"]=slab_vertices;
    }
    if(f.has_column()) {
        const auto& entity=source.entities().at(f.column);
        require(entity.type=="column" && entity.properties.at("version")==1 &&
            entity.properties.at("form")=="rectangular_column","actual canonical column remains editable");
        const auto column=std::get<RectangularColumn>(decode_building_entity(entity));
        const auto low=hand({3,3},c),high=hand({5,4},c);
        require_near(column.base_center.x,(low.x+high.x)/2,"hand column center X");
        require_near(column.base_center.y,(low.y+high.y)/2,"hand column center Y");
        require_near(column.base_center.z,.5,"column base Z invariant");
        require_near(column.width,high.x-low.x,"hand column width");require_near(column.depth,high.y-low.y,"hand column depth");
        require_near(column.height,2,"column height invariant");require_near(column.rotation_radians,0,"column yaw invariant");
        native_box(make_building_shape(column),{low.x,low.y,.5},{high.x,high.y,2.5},c.resized?6:4);
        result["column"]={low.x,low.y,high.x,high.y,.5,2.5};
    }
    if(f.has_child()) {
        const auto& host=source.entities().at(f.host);
        require(host.properties.at("version")==4 && host.properties.at("roof_openings").size()==2 &&
            !host.properties.contains("phase_id"),"actual different unphased host retains schema4 and both typed children");
        const auto decoded=std::get<SlopedRoofPanel>(decode_building_entity(host));
        require_near(decoded.base_position.x,-5,"unselected host root X");
        require_near(decoded.base_position.y,-5,"unselected host root Y");
        require_near(decoded.base_position.z,3,"unselected host root Z");
        require_near(decoded.run,20,"unselected host run");require_near(decoded.span,20,"unselected host span");
        require_near(decoded.thickness,.2,"unselected host thickness");
        const auto child=std::find_if(decoded.openings.begin(),decoded.openings.end(),[&](const auto& item){return item.id==f.child;});
        require(child!=decoded.openings.end() && child->skylight.has_value(),"selected child retains actual typed skylight construction");
        const auto low=hand({2,1},c),high=hand({4,2},c);
        require_near(child->x-5,low.x,"child local X expresses hand world mouth");
        require_near(child->y-5,low.y,"child local Y expresses hand world mouth");
        require_near(child->width,high.x-low.x,"child actual width");require_near(child->depth,high.y-low.y,"child actual depth");
        require_near(child->rotation_radians,0,"child surface angle invariant");
        require_near(child->skylight->frame_width,.06,"child frame construction invariant");
        require_near(child->skylight->curb_height,.15,"child curb construction invariant");
        require_near(child->skylight->glazing_thickness,.024,"child glazing construction invariant");
        // Authenticate the actual typed producer, including all nine frame/curb/glazing solids.
        // Hand volumes: 2x1 -> .16896; 3x1 -> .23496; 2x1.5 -> .21396.
        // The producer's sub-micrometre mouth clearance is admitted within 2 micrometres.
        const auto native=make_roof_skylight(decoded,*child);
        require(!native.IsNull(),"typed skylight producer returns a genuine native assembly");
        const double expected_volume=!c.resized?.16896:c.depth?.21396:.23496;
        require(std::abs(solid_volume(native)-expected_volume)<2e-6,"actual typed skylight solid volume matches independent hand construction");
        Bnd_Box bounds;BRepBndLib::Add(native,bounds);require(!bounds.IsVoid(),"typed skylight has real native bounds");
        double x0,y0,z0,x1,y1,z1;bounds.Get(x0,y0,z0,x1,y1,z1);
        const auto close=[](double a,double b){require(std::isfinite(a)&&std::abs(a-b)<2e-6,"typed native skylight hand bounds/corners");};
        close(x0,low.x);close(y0,low.y);close(z0,2.8);close(x1,high.x);close(y1,high.y);close(z1,3.174);
        int solids=0;for(TopExp_Explorer it(native,TopAbs_SOLID);it.More();it.Next())++solids;
        require(solids==9,"actual skylight has four curb, four upper frame and one glazing solids");
        const std::array xs{low.x,low.x+.06,high.x-.06,high.x};
        const std::array ys{low.y,low.y+.06,high.y-.06,high.y};
        const std::array zs{2.8,3.15,3.174};std::vector<Vec3> corners;
        for(TopExp_Explorer it(native,TopAbs_VERTEX);it.More();it.Next()) {
            const auto p=BRep_Tool::Pnt(TopoDS::Vertex(it.Current()));corners.push_back({p.X(),p.Y(),p.Z()});
            require(std::any_of(xs.begin(),xs.end(),[&](double x){return std::abs(p.X()-x)<2e-6;}) &&
                std::any_of(ys.begin(),ys.end(),[&](double y){return std::abs(p.Y()-y)<2e-6;}) &&
                std::any_of(zs.begin(),zs.end(),[&](double z){return std::abs(p.Z()-z)<2e-6;}),"each typed native vertex belongs to hand frame/curb/glazing corners");
        }
        for(double x:{low.x,high.x})for(double y:{low.y,high.y})for(double z:{2.8,3.174})
            require(std::any_of(corners.begin(),corners.end(),[&](Vec3 p){return std::abs(p.x-x)<2e-6&&std::abs(p.y-y)<2e-6&&std::abs(p.z-z)<2e-6;}),
                "every hand outer skylight corner exists in actual native assembly");
        result["child"]={low.x,low.y,high.x,high.y,expected_volume};
    }
    return result;
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
    require(before.saved_revision_optional()==after.saved_revision_optional(),"resize/refusal preserves original save authority");
    require(before.entities().size()==after.entities().size()+(retired ? 1 : 0),"resize adds no owner identity or unrelated entity");
    for(const auto& [id,entity]:before.entities()) {
        if(retired && id==f.dimension) {require(!after.entities().contains(id),"only explicitly reviewed dimension is retired");continue;}
        require(after.entities().contains(id),"complete source inventory remains present");
        const auto& actual=after.entities().at(id);
        require(actual.id==entity.id && actual.type==entity.type && actual.required==entity.required,"complete source identity/type/required envelope stays exact");
        if(id!=f.component && id!=f.catalog && id!=f.roof && id!=f.slab && id!=f.column && id!=f.host) require(actual==entity,"unselected context and unrelated carriers remain exact");
        else if(id!=f.roof && id!=f.slab && id!=f.column && id!=f.host) require(actual.extensions.dump()==entity.extensions.dump(),"owner/catalog opaque extension envelopes remain exact");
        if(id==f.component || id==f.roof || id==f.slab || id==f.column || id==f.host) {
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
        auto extensions=result.extensions;extensions.erase(std::string(roof_plan_resize_derivations_key));
        require(extensions==original.extensions,"only owned roof derivation archive changes opaque extension envelope");
    }
    require(after.history().size()>=before.history().size(),"all original source history remains retained");
    for(std::size_t i=0;i<before.history().size();++i) {
        const auto& a=before.history().at(i);const auto& b=after.history().at(i);
        require(a.revision==b.revision&&a.parent_revision==b.parent_revision&&a.source_revision==b.source_revision&&
            a.action==b.action&&a.name==b.name&&a.entities==b.entities&&a.assets==b.assets&&
            a.undo_stack==b.undo_stack&&a.redo_stack==b.redo_stack,
            "complete original history records retain exact source envelopes, assets and navigation");
        require(a.boundary_constraint_changes.has_value()==b.boundary_constraint_changes.has_value(),"source command authority presence remains exact");
        if(a.boundary_constraint_changes)require(command_to_json(*a.boundary_constraint_changes)==command_to_json(*b.boundary_constraint_changes),
            "complete original command wire remains exact");
    }
    if(f.has_slab()) {
        const auto& old_slab=before.entities().at(f.slab);const auto& slab=after.entities().at(f.slab);
        if(slab!=old_slab) {
            const auto& wire=after.history().back().boundary_constraint_changes->mixed_selection_edit_intent.at("slab_geometry").at(0);
            const auto operation=decode_slab_geometry_edit_intent(wire);
            require(slab==replay_slab_geometry_entity(old_slab,operation),"complete slab equals exact original-source codec replay");
            validate_slab_geometry_derivation(slab);
            const auto key=std::string(slab_geometry_derivations_key);
            const auto& archive=slab.extensions.at(key);
            require(archive.at("version")==4 && archive.at("operations").size()==1 &&
                archive.at("operations").at(0).at("operation")==wire,"ordinary slab retains one exact coordinated leaf4 derivation");
            auto extensions=slab.extensions;extensions.erase(key);
            require(extensions.dump()==old_slab.extensions.dump(),"slab preserves every unrelated opaque extension");
            require(slab.properties.at("layers").dump()==old_slab.properties.at("layers").dump(),"slab preserves exact layer identity and thickness");
        }
    }
    if(f.has_column()) {
        const auto& old=before.entities().at(f.column);const auto& current=after.entities().at(f.column);
        require(old.extensions==current.extensions,"structural opaque extension envelope remains exact");
        const std::vector<std::string> geometry{"base_center_m","width_m","depth_m","quantity_entries"};
        auto old_properties=old.properties,new_properties=current.properties;
        for(const auto& key:geometry){old_properties.erase(key);new_properties.erase(key);}
        require(old_properties==new_properties,"only canonical column plan dimensions/receipts change; identity, height and vertical source stay exact");
        require(current.properties.at("quantity_entries").at("/future_dimension_m")==old.properties.at("quantity_entries").at("/future_dimension_m"),
            "structural opaque receipt sibling survives actual typed resize");
    }
    if(f.has_child()) {
        const auto& old_host=before.entities().at(f.host);const auto& host=after.entities().at(f.host);
        if(host!=old_host) {
            const auto& wire=after.history().back().boundary_constraint_changes->mixed_selection_edit_intent.at("roof_opening_axis_requests").at(0);
            const std::array requests{RoofOpeningDirectionalResizeRequest{{f.host,f.child},wire.at("sx").get<double>(),wire.at("sy").get<double>(),
                {wire.at("anchor_m").at(0).get<double>(),wire.at("anchor_m").at(1).get<double>()},wire.at("frame_rotation_radians").get<double>()}};
            const auto replay=prepare_roof_opening_directional_resizes(before.entities(),requests);
            require(host==replay.candidate_entities.at(f.host),
                "complete child host equals exact ordinary producer replay from same original source and retained child operator");
        }
        auto old_body=old_host,new_body=host;
        old_body.properties.erase("roof_openings");new_body.properties.erase("roof_openings");
        old_body.extensions.erase("roof_opening_input");new_body.extensions.erase("roof_opening_input");
        require(old_body==new_body && old_body.properties.dump()==new_body.properties.dump() &&
            old_body.extensions.dump()==new_body.extensions.dump(),"unselected host body/root/context/opaque envelope remain byte-exact");
        const auto find_opening=[](const Entity& owner,const std::string& id)->const Json& {
            const auto& rows=owner.properties.at("roof_openings");
            const auto found=std::find_if(rows.begin(),rows.end(),[&](const auto& row){return row.at("id")==id;});
            require(found!=rows.end(),"retained child exists");return *found;
        };
        require(find_opening(old_host,f.sibling).dump()==find_opening(host,f.sibling).dump(),"real unselected sibling raw construction remains exact");
        auto old_child=find_opening(old_host,f.child),new_child=find_opening(host,f.child);
        for(const auto* key:{"x_m","y_m","width_m","depth_m","rotation_rad"}) {old_child.erase(key);new_child.erase(key);}
        require(old_child.dump()==new_child.dump(),"selected child retains exact identity/construction/opaque fields");
        if(host.extensions.contains("roof_opening_input")) {
            const auto& entries=host.extensions.at("roof_opening_input").at("entries");
            require(entries.size()==1&&entries.contains(f.child),"child receipt consequences belong only to actual selected child");
        }
    }
    const auto old_model=AssemblyModel::from_json(before.entities().at(f.catalog).properties.at("model"));
    const auto new_model=AssemblyModel::from_json(after.entities().at(f.catalog).properties.at("model"));
    for(const auto& type:old_model.types()) {
        const auto found=std::find_if(new_model.types().begin(),new_model.types().end(),[&](const auto& candidate){return candidate.id==type.id;});
        require(found!=new_model.types().end() && *found==type,"component original shared catalog type remains exact");
    }
}

void wait(MainWindow& window,const ModalGuard& guard,const std::function<bool()>& predicate,const char* message,int timeout_ms=10000) {
    const auto initial_revision=window.document().revision();
    QElapsedTimer deadline;deadline.start();
    while(!predicate() && deadline.elapsed()<timeout_ms) {QApplication::processEvents(QEventLoop::AllEvents,20);guard.verify();}
    guard.verify();if(!predicate()) throw std::runtime_error(std::string(message)+": "+window.lastError().toStdString()+
        " (elapsed="+std::to_string(deadline.elapsed())+"ms, pending="+
        (canvas(window).entityTransformPreviewPending()?"true":"false")+", preview="+
        std::to_string(canvas(window).entityTransformPreview().size())+", initial_revision="+
        std::to_string(initial_revision)+", revision="+std::to_string(window.document().revision())+")");
    if(deadline.elapsed()>=1000) std::cout<<"[native wait] "<<message<<": elapsed="<<deadline.elapsed()
        <<"ms, initial_revision="<<initial_revision<<", revision="<<window.document().revision()
        <<", pending="<<(canvas(window).entityTransformPreviewPending()?"true":"false")
        <<", preview="<<canvas(window).entityTransformPreview().size()<<'\n'<<std::flush;
}

void mouse(PlanCanvas& drawing,QEvent::Type kind,QPointF pixel) {
    QMouseEvent event(kind,pixel,drawing.mapToGlobal(pixel.toPoint()),kind==QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
        kind==QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton,Qt::ShiftModifier);
    QApplication::sendEvent(&drawing,&event);
}
QPointF completed_preview(MainWindow& window,const Fixture& f,const DocumentSnapshot& before,const ModalGuard& guard,ResizeCase c,bool await_native=true) {
    auto& drawing=canvas(window);frame(drawing,f);roster(drawing,f);const auto committed=output(drawing);
    const auto presentations=drawing.selectedEntityPresentations();
    const auto controls_generation=drawing.roofOpeningControlsGeneration();
    const auto grips=drawing.selectionAxisHandlePositions();
    const Vec2 side=c.depth ? Vec2{4,c.opposite ? 0.0 : f.depth()} : Vec2{c.opposite ? 1.0 : 7.0,f.depth()/2};
    const auto side_presented=presented(side,f.site),center=drawing.viewCenter();
    const QPointF expected(drawing.width()/2.0+(side_presented.x-center.x)*drawing.viewScale(),
                           drawing.height()/2.0-(side_presented.y-center.y)*drawing.viewScale());
    const auto press=*std::min_element(grips.begin(),grips.end(),[&](QPointF a,QPointF b) {
        return std::hypot(a.x()-expected.x(),a.y()-expected.y())<std::hypot(b.x()-expected.x(),b.y()-expected.y());
    });
    const Vec2 physical_delta=c.depth ? Vec2{0,(c.opposite ? -.5 : .5)*f.depth()} : Vec2{c.opposite ? -3.0 : 3.0,0};
    const Vec2 delta=f.site ? Vec2{-physical_delta.y,physical_delta.x} : physical_delta;
    const auto release=press+QPointF(delta.x*drawing.viewScale(),-delta.y*drawing.viewScale());
    require(drawing.rect().contains(press.toPoint()) && drawing.rect().contains(release.toPoint()),"real anchored side gesture remains inside native canvas");
    mouse(drawing,QEvent::MouseButtonPress,press);unchanged(window,before);frame(drawing,f);
    mouse(drawing,QEvent::MouseMove,release);require(drawing.entityTransformPreviewPending(),"actual side gesture starts asynchronous native manufacture");
    unchanged(window,before);require(output(drawing)==committed,"pending preview remains outside committed Print output");
    if(!await_native)return release;
    wait(window,guard,[&]{return !drawing.entityTransformPreviewPending() && !drawing.entityTransformPreview().empty();},
        "detached combined component/roof-bearing native preview finishes",30000);
    c.resized=true;mouth_controls(drawing,f,before.revision());plans(drawing.entityTransformPreview(),f,c);plans(drawing.entities(),f);
    QStringList ids;for(const auto& item:drawing.entityTransformPreview()) if(!ids.contains(item.id)) ids.push_back(item.id);
    auto expected_ids=ordinary_ids(f);if(f.has_child())expected_ids.push_back(QString::fromStdString(f.host));ids.sort();expected_ids.sort();
    require(ids==expected_ids,"detached preview contains exact ordinary owners and only the actual qualified child host");
    unchanged(window,before);frame(drawing,f);roster(drawing,f);
    require(drawing.selectedEntityPresentations()==presentations && drawing.roofOpeningControlsGeneration()==controls_generation,
            "detached manufacture preserves captured ordinary and qualified presentation publication");
    require(output(drawing)==committed,"complete detached preview remains outside committed Print output");return release;
}

void floor270(const DocumentSnapshot& saved) {
    require(ProjectStore::required_format_version(saved)==270 && ProjectStore::required_semantic_format_version(saved)==270,
        "applied/undone/redone canonical Properties47 requires native and semantic reader270");
}
void mixed47_command(const DocumentSnapshot& after,const Fixture& f,const DocumentSnapshot& before,ResizeCase c={true,false,false}) {
    const auto& command=after.history().back().boundary_constraint_changes;
    require(command && command->mixed_selection_edit_completion && command->mixed_selection_edit_intent.at("version")==47,
        "actual release publishes sealed Properties47 completion");
    require(command_to_json(*command).at("version")==52,"canonical command52 envelope");
    const auto& intent=command->mixed_selection_edit_intent;
    require(intent.size()==17,"Properties47 has exactly seventeen closed mandatory fields");
    for(const auto* key:{"version","expected_revision","source_snapshot_digest","source_authoring_digest","source_entities_digest",
        "source_saved_revision","component_request","component_identity_reservations","slab_geometry","slab_allocation",
        "structural_axis_requests","structural_allocation","roof_opening_axis_requests","roof_plan_resizes","room_reviews","phase_room_reviews","message"})
        require(intent.contains(key),"Properties47 contains each mandatory source-bound field");
    for(const auto* key:{"room_reviews","phase_room_reviews"})
        require(intent.at(key).is_array()&&intent.at(key).empty(),"ordinary unphased cohort has empty room reviews");
    const auto& roofs=intent.at("roof_plan_resizes");const auto& slabs=intent.at("slab_geometry");
    const auto& structures=intent.at("structural_axis_requests");const auto& children=intent.at("roof_opening_axis_requests");
    require(roofs.is_array()&&roofs.size()==1&&slabs.is_array()&&slabs.size()==(f.has_slab()?1:0)&&
        structures.is_array()&&structures.size()==(f.has_column()?1:0)&&children.is_array()&&children.size()==(f.has_child()?1:0),
        "all four operator rosters exactly match captured semantic family inventory");
    require(intent.at("slab_allocation").is_null()&&intent.at("structural_allocation").is_null(),
        "both actual ordinary changed-baseline inventories are empty and both allocation fields are null");
    const auto family_count=1+!slabs.empty()+!structures.empty()+!children.empty();
    require(family_count>=2&&family_count<=3,"new47 retains two or three nonempty physical families before unit elimination");
    const auto& roof=roofs.at(0);
    require(roof.size()==7&&roof.at("version")==2&&roof.at("anchor_mode")=="shared_frame"&&roof.at("roof_id")==f.roof&&
        roof==encode_roof_plan_resize_intent(decode_roof_plan_resize_intent(roof)),"exact singleton canonical roofv2 shared-frame roster");
    const bool canvas_x=f.site?c.depth:!c.depth;
    const double sx=canvas_x?1.5:1,sy=canvas_x?1:1.5;
    const Vec2 source_anchor=c.depth?Vec2{4,c.opposite?f.depth():0}:Vec2{c.opposite?7.0:1.0,f.depth()/2};
    const auto canvas_anchor=presented(source_anchor,f.site);
    const auto check_operator=[&](const std::string& owner,const Json& row,const char* xkey,const char* ykey) {
        const auto placement=resolve_site_presentation(before,owner);
        const auto actual=site_transform_point({canvas_anchor.x,canvas_anchor.y,0},placement.inverse);
        require_near(actual.x,source_anchor.x,"actual owner inverse anchor X");require_near(actual.y,source_anchor.y,"actual owner inverse anchor Y");
        require_near(row.at("anchor_m").at(0).get<double>(),actual.x,"retained original-source anchor X");
        require_near(row.at("anchor_m").at(1).get<double>(),actual.y,"retained original-source anchor Y");
        require_near(row.at("frame_rotation_radians").get<double>(),-placement.forward.rotation_radians,"actual owner inverse source axis");
        require_near(row.at(xkey).get<double>(),sx,"captured canvas X factor");require_near(row.at(ykey).get<double>(),sy,"captured canvas Y factor");
    };
    check_operator(f.roof,roof,"scale_x","scale_y");
    if(f.has_slab()) {
        const auto& leaf=slabs.at(0);const auto operation=decode_slab_geometry_edit_intent(leaf);
        require(leaf==encode_slab_geometry_edit_intent(operation)&&operation.slab_id==f.slab&&
            operation.kind==SlabGeometryEditKind::resize_plan&&operation.resize&&operation.coordinate_world_hosted_geometry&&
            leaf.at("version")==4&&!operation.preserve_planar_source_geometry&&
            !slab_geometry_edit_requires_planar_source(before.entities().at(f.slab),operation),
            "exact singleton ordinary coordinated slab leaf4 roster");
        const auto& resize=*operation.resize;
        check_operator(f.slab,Json{{"anchor_m",{resize.anchor_m.x,resize.anchor_m.y}},{"frame_rotation_radians",resize.frame_rotation_radians},
            {"scale_x",resize.scale_x},{"scale_y",resize.scale_y}},"scale_x","scale_y");
    }
    if(f.has_column()) {
        const auto& row=structures.at(0);
        require(row.size()==5&&row.at("object_id")==f.column,"exact closed singleton structural axis roster");
        for(const auto* key:{"object_id","sx","sy","anchor_m","frame_rotation_radians"})require(row.contains(key),"mandatory structural axis row field");
        check_operator(f.column,row,"sx","sy");
    }
    if(f.has_child()) {
        const auto& row=children.at(0);
        require(row.size()==6&&row.at("roof_id")==f.host&&row.at("opening_id")==f.child&&f.host!=f.roof,
            "exact closed singleton qualified child roster excludes whole-roof and sibling authority");
        for(const auto* key:{"roof_id","opening_id","sx","sy","anchor_m","frame_rotation_radians"})require(row.contains(key),"mandatory child axis row field");
        check_operator(f.host,row,"sx","sy");
    }
    const auto& component=intent.at("component_request");
    require(component.at("version")==7||component.at("version")==8||component.at("version")==9,"mandatory geometry-only component request7/8/9");
    const auto& component_rows=component.at("authoring").at("intents");
    require(component_rows.is_array() && component_rows.size()==1,"exact one-member original component roster");
    require(decode_assembly_instance_property_edit_intent(component_rows.at(0)).target==
        AssemblyInstancePropertyEditTarget{f.catalog,f.component,f.component,{}},
        "component operator retains exact original qualified catalog/root/entity/path correspondence");
    const auto component_placement=resolve_site_presentation(before,f.component);
    const auto component_anchor=site_transform_point({canvas_anchor.x,canvas_anchor.y,0},component_placement.inverse);
    require_near(component_anchor.x,source_anchor.x,"component actual inverse source anchor X");
    require_near(component_anchor.y,source_anchor.y,"component actual inverse source anchor Y");
    require(intent.at("expected_revision")==before.revision()&&intent.at("source_snapshot_digest")==document_snapshot_digest(before)&&
        intent.at("source_authoring_digest")==document_authoring_source_digest_v2(before)&&intent.at("source_entities_digest")==entity_map_digest(before.entities())&&
        intent.at("source_saved_revision")==(before.saved_revision_optional()?Json(*before.saved_revision_optional()):Json(nullptr)),
        "all five mixed fences bind complete actual original source");
    for(const auto* key:{"expected_revision","source_snapshot_digest","source_authoring_digest","source_entities_digest","source_saved_revision"})
        require(component.at(key)==intent.at(key),"component retains same five original-source fences");
    floor270(after);
}

void scenario(const QString& directory,Cohort cohort,const std::vector<int>& order) {
    auto f=fixture(false,false,cohort);admit_source(f);const auto before=f.document->snapshot();structure(before,f);
    QString order_name;for(const int index:order)order_name+=QString::number(index);
    const auto stem=QStringLiteral("roof-physical-frame-")+QString::fromLatin1(cohort_name(cohort))+"-"+order_name;
    ModalGuard guard;MainWindow window(f.document,nullptr,QDir(directory).filePath(stem+"-library.json"));display(window,guard,f);
    auto& drawing=canvas(window);const auto committed=output(drawing);require(!committed.isNull(),"real committed Print output renders");
    select_order(window,f,order,guard);plans(drawing.entities(),f);roster(drawing,f);mouth_controls(drawing,f,before.revision());
    unchanged(window,before);no_preview(drawing);require(output(drawing)==committed,"exact ordinary and qualified selection preserves committed output");
    capture(drawing,stem+"-selected.png");frame(drawing,f);unchanged(window,before);no_preview(drawing);
    require(output(drawing)==committed,"frame admission preserves complete original output");guard.verify();
}
int fresh_open_count=0;
void fresh_open(const QString& directory,const QString& stem,const DocumentSnapshot& saved,const Fixture& f,
                const ModalGuard& guard,int first,ResizeCase c) {
    MainWindow reopened({},nullptr,QDir(directory).filePath(stem+"-reopened-library.json"));
    require(reopened.openProject(QDir(directory).filePath(stem+".bldproj")) && reopened.document().is_editable(),"ordinary fresh Open acquires actual editable writer ownership");
    display(reopened,guard,f);const auto loaded=reopened.document().snapshot();
    require(document_snapshot_digest(loaded)==document_snapshot_digest(saved) && loaded.entities()==saved.entities() && loaded.assets()==saved.assets(),
            "fresh editable Open retains exact saved source, assets, revision, save and complete retained history");floor270(loaded);
    const auto extracted=std::filesystem::path(QDir(directory).filePath(stem+"-extracted").toStdWString());
    extract_project(loaded,extracted);std::ifstream input(extracted/"project.json");const auto manifest=Json::parse(input);
    require(manifest.at("exchange_version")==268,"actual native-persisted extraction requires exchange268");
    bool retained_command=false;
    for(const auto& record:saved.history()) if(record.boundary_constraint_changes &&
        record.boundary_constraint_changes->mixed_selection_edit_intent.value("version",0)==47) {
        const auto wire=command_to_json(*record.boundary_constraint_changes);
        require(command_to_json(*loaded.history().at(record.revision).boundary_constraint_changes)==wire&&
            manifest.at("revisions").at(record.revision).at("boundary_constraint_changes")==wire,
            "fresh native Open and extraction retain complete original-source command wire, all four rosters and both allocations");
        retained_command=true;
    }
    require(retained_command,"applied/undone/redone extraction authenticates a real retained Properties47 command");
    auto* undo=reopened.findChild<QAction*>("undoCommand");
    require(undo && !undo->isEnabled() && !reopened.undoCommand() && !reopened.redoCommand(),"fresh editable Open has no Undo/Redo boundary despite retained history");
    unchanged(reopened,loaded);structure(loaded,f,c);select(reopened,f,first,guard);
    auto& drawing=canvas(reopened);plans(drawing.entities(),f,c);frame(drawing,f,c);roster(drawing,f);mouth_controls(drawing,f,loaded.revision(),c);no_preview(drawing);
    capture(drawing,stem+"-reopened.png");guard.verify();++fresh_open_count;
}
Json gesture_scenario(const QString& directory,int first,bool depth,bool site,bool opposite=false,Cohort cohort=Cohort::roof_structural) {
    const auto f=fixture(false,site,cohort);admit_source(f);ResizeCase c{true,depth,opposite,f.depth()};
    const auto stem=QString::fromLatin1(cohort_name(cohort))+"-"+QString(site ? "roof-physical-site-" : "roof-physical-plan-")+(depth ? "depth-" : "width-")+
        (opposite ? "opposite-" : "")+(first==0 ? "component-first" : first==1 ? "whole-first" : "structural-first");
    std::cout<<"[roof physical desktop] "<<stem.toStdString()<<'\n'<<std::flush;
    ModalGuard guard;MainWindow window(f.document,nullptr,QDir(directory).filePath(stem+"-library.json"));display(window,guard,f);select(window,f,first,guard);
    auto& drawing=canvas(window);const auto before=window.document().snapshot();
    require(ProjectStore::required_format_version(before)<270 && ProjectStore::required_semantic_format_version(before)<270,"opaque command-shaped metadata does not create reader270 authority");
    structure(before,f);plans(drawing.entities(),f);mouth_controls(drawing,f,before.revision());const auto committed=output(drawing);capture(drawing,stem+"-selected.png");
    const auto release=completed_preview(window,f,before,guard,c);capture(drawing,stem+"-preview.png");mouse(drawing,QEvent::MouseButtonRelease,release);
    wait(window,guard,[&]{return window.document().revision()==before.revision()+1 && !drawing.entityTransformPreviewPending();},"one side release publishes every captured member exactly once",30000);
    const auto after=window.document().snapshot();mixed47_command(after,f,before,c);
    require(after.history().size()==before.history().size()+1,"one release creates one retained mixed history event");
    no_preview(drawing);const auto result=structure(after,f,c);preserved(before,after,f);plans(drawing.entities(),f,c);frame(drawing,f,c);roster(drawing,f);mouth_controls(drawing,f,after.revision(),c);
    require(output(drawing)!=committed,"release publishes changed native geometry into committed Print output");capture(drawing,stem+"-committed.png");const auto output_after=output(drawing);
    const auto save=[&](const QString& state) {
        const auto current=window.document().snapshot();require(window.saveProjectAs(QDir(directory).filePath(stem+state+".bldproj")),"actual native lifecycle state saves");
        const auto saved=window.document().snapshot();require(saved.entities()==current.entities() && saved.assets()==current.assets(),"Save retains exact authoring graph and assets");floor270(saved);return saved;
    };
    const auto applied=save("-applied");
    require(window.undoCommand() && window.document().snapshot().entities()==before.entities() && window.document().snapshot().assets()==before.assets(),"one Undo restores exact original owners, all receipts and assets");
    const auto undo_state=window.document().snapshot();
    no_preview(drawing);
    require(undo_state.revision()==after.revision()+1 && undo_state.history().size()==after.history().size()+1 && undo_state.history().back().action=="undo" &&
            undo_state.history().back().source_revision==before.revision(),"one Undo appends exactly one navigation event targeting the actual source");
    structure(undo_state,f);plans(drawing.entities(),f);frame(drawing,f);roster(drawing,f);mouth_controls(drawing,f,undo_state.revision());require(output(drawing)==committed,"Undo restores exact original committed native output");
    const auto undone=save("-undone");
    require(window.redoCommand() && window.document().snapshot().entities()==after.entities() && window.document().snapshot().assets()==after.assets(),"one Redo restores exact resized owners, all retained envelopes and assets");
    const auto redo_state=window.document().snapshot();
    no_preview(drawing);
    require(redo_state.revision()==after.revision()+2 && redo_state.history().size()==after.history().size()+2 && redo_state.history().back().action=="redo" &&
            redo_state.history().back().source_revision==after.revision(),"one Redo appends exactly one navigation event targeting the actual result");
    structure(redo_state,f,c);plans(drawing.entities(),f,c);frame(drawing,f,c);roster(drawing,f);mouth_controls(drawing,f,redo_state.revision(),c);
    require(output(drawing)==output_after,"Redo restores exact committed resized output");
    const auto redone=save("-redone");require(window.createNewProject(),"release native writer leases before fresh ordinary Open");
    fresh_open(directory,stem+"-applied",applied,f,guard,first,c);
    fresh_open(directory,stem+"-undone",undone,f,guard,first,{});
    fresh_open(directory,stem+"-redone",redone,f,guard,first,c);guard.verify();return result;
}

DocumentSnapshot replace_history(MainWindow& window,const DocumentSnapshot& before,bool geometry=false) {
    require(before.revision()>0,"history-only stale fixture has one actual retained source event");
    auto replacement=Document::fork_at_revision(before,before.revision()-1);std::vector<EntityChange> changes;
    for(const auto& [id,entity]:before.entities()) {
        auto copy=entity;if(geometry && copy.properties.value("form",std::string{})=="sloped_roof_panel") {
            copy.properties["base_position_m"][0]=1.25;if(copy.properties.contains("quantity_entries"))copy.properties["quantity_entries"].erase("/base_position_m/0");
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
    const auto stem=QStringLiteral("roof-physical-stale-")+(authority==StaleAuthority::selection ? "selection" : authority==StaleAuthority::viewport ? "viewport" :
        authority==StaleAuthority::presentation ? "presentation" : authority==StaleAuthority::geometry ? "same-id-revision-geometry" : "history");
    std::cout<<"[roof physical desktop] "<<stem.toStdString()<<'\n'<<std::flush;
    ModalGuard guard;MainWindow window(f.document,nullptr,QDir(directory).filePath(stem+"-library.json"));display(window,guard,f);select(window,f,0,guard);
    auto& drawing=canvas(window);const auto before=window.document().snapshot();const auto committed=output(drawing);
    const auto center=drawing.viewCenter();const auto scale=drawing.viewScale();const auto source_scene=drawing.entities();
    const auto release=completed_preview(window,f,before,guard,{true,false,false},false);
    capture(drawing,stem+"-preview.png");auto expected=before;
    if(authority==StaleAuthority::selection) {
        require(window.selectEntity(QString::fromStdString(f.component)),"stale fixture changes real public selection");
        require(drawing.selectedIds()==QStringList{QString::fromStdString(f.component)},"stale selection removes roof and column from captured roster");
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
    if(authority!=StaleAuthority::geometry) {structure(window.document().snapshot(),f);preserved(expected,window.document().snapshot(),f);}
    if(authority==StaleAuthority::presentation) drawing.setEntities(source_scene);
    drawing.setViewTransform(center,scale);select(window,f,0,guard);plans(drawing.entities(),f);frame(drawing,f);
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
                    require(dialog->result()==QDialog::Rejected && !dialog->isVisible(),"real Cancel rejects the complete component/roof/column cohort");return;
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
    const auto stem=QStringLiteral("roof-physical-review-")+(action==ReviewAction::cancel ? "cancel" : action==ReviewAction::retire ? "remove" :
        action==ReviewAction::stale_presentation ? "stale-presentation" : "stale-history");
    std::cout<<"[roof physical desktop] "<<stem.toStdString()<<'\n'<<std::flush;
    ModalGuard guard;MainWindow window(f.document,nullptr,QDir(directory).filePath(stem+"-library.json"));display(window,guard,f);select(window,f,0,guard);
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
        settle_without_publication(window,expected,guard);structure(window.document().snapshot(),f);preserved(expected,window.document().snapshot(),f);
        require(output(drawing)==committed,"Cancel/stale review retains exact committed output");
        require_near(dimension.dimension->resolve(window.document().snapshot()).segment_length(),2,"Cancel/stale review retains exact original live dimension");
        if(action!=ReviewAction::cancel) require(!window.lastError().isEmpty(),"accepted stale review reports refused publication");
        capture(drawing,stem+"-refused.png");
    } else {
        wait(window,guard,[&]{return window.document().revision()==before.revision()+1 && !drawing.entityTransformPreviewPending();},"reviewed retirement publishes all three members in one revision",30000);
        const auto after=window.document().snapshot();mixed47_command(after,f,before);no_preview(drawing);
        require(after.history().size()==before.history().size()+1 && !after.entities().contains(f.dimension),"one reviewed event combines all three owners and explicit dimension retirement");
        structure(after,f,{true,false,false});preserved(before,after,f,true);plans(drawing.entities(),f,{true,false,false});frame(drawing,f,{true,false,false});roster(drawing,f);
        require(output(drawing)!=committed,"actual reviewed Apply publishes changed committed output");capture(drawing,stem+"-committed.png");
        const auto reviewed_output=output(drawing);
        require(window.undoCommand() && window.document().snapshot().entities()==before.entities() && window.document().snapshot().assets()==before.assets(),"one reviewed Undo restores all three original owners, exact dimension and assets");
        const auto undone=window.document().snapshot();
        require(undone.revision()==after.revision()+1 && undone.history().size()==after.history().size()+1 &&
            undone.history().back().action=="undo" && undone.history().back().source_revision==before.revision(),
            "one reviewed Undo appends exactly one navigation event targeting the original source");
        no_preview(drawing);structure(undone,f);plans(drawing.entities(),f);frame(drawing,f);roster(drawing,f);
        require(output(drawing)==committed,"reviewed Undo restores exact committed source output");
        require(window.document().snapshot().entities().at(f.dimension)==before.entities().at(f.dimension),"reviewed Undo restores exact original dimension carrier");
        require_near(decode_boundary_dimension_entity(window.document().snapshot().entities().at(f.dimension)).dimension->resolve(window.document().snapshot()).segment_length(),2,
            "reviewed Undo restores actual resolved original dimension length");
        require(window.redoCommand() && window.document().snapshot().entities()==after.entities() && window.document().snapshot().assets()==after.assets(),"one reviewed Redo restores all three resized owners and explicit dimension retirement");
        const auto redone=window.document().snapshot();
        require(redone.revision()==after.revision()+2 && redone.history().size()==after.history().size()+2 &&
            redone.history().back().action=="redo" && redone.history().back().source_revision==after.revision(),
            "one reviewed Redo appends exactly one navigation event targeting the actual result");
        no_preview(drawing);structure(redone,f,{true,false,false});frame(drawing,f,{true,false,false});roster(drawing,f);
        plans(drawing.entities(),f,{true,false,false});
        require(output(drawing)==reviewed_output,"reviewed Redo restores exact committed resized output");
        require(window.saveProjectAs(QDir(directory).filePath(stem+".bldproj")),"reviewed cohort retirement saves");
        const auto saved=window.document().snapshot();floor270(saved);require(window.createNewProject(),"release reviewed writer lease before editable Open");
        fresh_open(directory,stem,saved,f,guard,0,{true,false,false});
    }
    review.verify();guard.verify();
}

} // namespace
int main(int argc,char** argv) {
    sketch::testing::noninteractive_errors();qputenv("QT_QPA_PLATFORM","offscreen");QStandardPaths::setTestModeEnabled(true);
    QApplication application(argc,argv);QTemporaryDir settings;
    QCoreApplication::setOrganizationName("VertexTests");
    QCoreApplication::setApplicationName("MixedComponentRoofPhysicalResize-"+QUuid::createUuid().toString(QUuid::WithoutBraces));
    QSettings::setDefaultFormat(QSettings::IniFormat);QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,settings.path());
    QSettings::setPath(QSettings::IniFormat,QSettings::SystemScope,settings.path());
    try {
        require(settings.isValid(),"private settings directory exists");
        const auto font=QFontDatabase::addApplicationFont(":/fonts/Inter.ttf");require(font>=0,"bundled Inter loads");
        const auto families=QFontDatabase::applicationFontFamilies(font);require(!families.isEmpty(),"Inter exposes family");
        application.setFont(QFont(families.front(),10));QTemporaryDir directory;require(directory.isValid(),"private fixture directory exists");
        const auto flag=[&](const char* short_name,const char* historical_name) {
            return application.arguments().contains(QString::fromLatin1(short_name))||application.arguments().contains(QString::fromLatin1(historical_name));
        };
        const bool frame_only=flag("--frame","--frame-only"),site_only=flag("--site","--site-only");
        const bool review_only=flag("--review","--review-only"),stale_only=flag("--stale","--stale-only");
        const bool left_only=flag("--left","--left-only");
        const std::array cohorts{Cohort::roof_structural,Cohort::roof_slab_structural,Cohort::roof_slab_child,Cohort::roof_structural_child};
        int admitted=0,lifecycles=0,reviews=0,stale=0;std::vector<std::string> failures;
        // Preserve the primary six genuine native gates first; each triple has all 24 selection orders.
        for(const auto cohort:cohorts) {
            std::vector<int> order=cohort==Cohort::roof_structural?std::vector<int>{0,1,2}:std::vector<int>{0,1,2,3};
            int count=0;
            do {
                try {scenario(directory.path(),cohort,order);++admitted;++count;}
                catch(const std::exception& error) {
                    std::string label=cohort_name(cohort);for(const int member:order)label+="-"+std::to_string(member);
                    failures.push_back(label+": "+error.what());
                }
            }while(std::next_permutation(order.begin(),order.end()));
            std::cout<<"[frame admissions] "<<cohort_name(cohort)<<": "<<count<<"/"<<(cohort==Cohort::roof_structural?6:24)<<'\n'<<std::flush;
        }
        for(const auto& failure:failures)std::cerr<<failure<<'\n';
        require(failures.empty()&&admitted==78,"all six primary and all 72 triple native frame/selection/source gates pass");
        if(left_only) {
            for(const auto cohort:cohorts){gesture_scenario(directory.path(),0,false,false,true,cohort);++lifecycles;}
        } else if(!frame_only) {
            if(!review_only&&!stale_only)for(const auto cohort:cohorts)for(const bool site:{false,true})
                for(const bool depth:{false,true})for(const bool opposite:{false,true}) {
                    if(site_only&&!site)continue;
                    std::optional<Json> first;
                    const int first_roles=cohort==Cohort::roof_structural?3:1;
                    for(int role=0;role<first_roles;++role) {
                        const auto result=gesture_scenario(directory.path(),role,depth,site,opposite,cohort);++lifecycles;
                        if(first)same_structure(*first,result);else first=result;
                    }
                }
            if(!site_only&&!stale_only)for(const auto action:{ReviewAction::cancel,ReviewAction::retire,ReviewAction::stale_presentation,ReviewAction::stale_history}) {
                dimension_review(directory.path(),action);++reviews;
            }
            if(!site_only&&!review_only)for(const auto authority:{StaleAuthority::selection,StaleAuthority::viewport,StaleAuthority::presentation,StaleAuthority::history,StaleAuthority::geometry}) {
                stale_release(directory.path(),authority);++stale;
            }
        }
        std::cout<<"mixed_component_roof_physical_resize_desktop_tests passed: "<<admitted<<" frame permutations, "
            <<lifecycles<<" lifecycle cases, "<<reviews<<" dimension review cases, "<<stale<<" stale-authority cases, "
            <<fresh_open_count<<" fresh editable Opens\n";return 0;
    }catch(const std::exception& error){std::cerr<<"mixed_component_roof_physical_resize_desktop_tests: "<<error.what()<<'\n';return 1;}
}
