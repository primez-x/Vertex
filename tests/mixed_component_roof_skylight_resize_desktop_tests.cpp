#include "sketch/desktop/main_window.hpp"
#include "sketch/architecture.hpp"
#include "sketch/assembly_document_adapter.hpp"
#include "sketch/assembly_geometry.hpp"
#include "sketch/building_entity.hpp"
#include "sketch/document_digest.hpp"
#include "sketch/boundary_dimension.hpp"
#include "sketch/project_store.hpp"
#include "sketch/project_exchange.hpp"
#include "sketch/site_frame.hpp"
#include "sketch/phase_roof_resize.hpp"
#include "sketch/roof_entity_codec.hpp"
#include "sketch/roof_opening_group_edit.hpp"
#include "../src/desktop/plan_canvas.hpp"
#include "support/noninteractive_errors.hpp"
#include <BRepBndLib.hxx>
#include <Bnd_Box.hxx>
#include <BRep_Tool.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <QAction>
#include <QApplication>
#include <QDialogButtonBox>
#include <QElapsedTimer>
#include <QListWidget>
#include <QPushButton>
#include <QDialog>
#include <QDir>
#include <QEventLoop>
#include <QFont>
#include <QFontDatabase>
#include <QImage>
#include <QMouseEvent>
#include <QPainter>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolButton>
#include <QUuid>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <fstream>
#include <functional>
#include <numbers>
#include <optional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace sketch;
using namespace sketch::desktop;
using Json=nlohmann::json;
void require(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
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
    std::string component=fresh("component"),catalog=fresh("catalog"),type=fresh("type"),roof=fresh("whole-roof");
    std::string host=fresh("child-host"),child=fresh("skylight"),sibling=fresh("unselected-skylight");
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
    AssemblyType type;type.id=f.type;type.name="Independent component";type.profiles={profile};
    Entity catalog{f.catalog,"assembly_model",{{"model",AssemblyModel::create({}, {type}, {}).to_json()}}};
    // An opaque lookalike must not create native or semantic command authority.
    catalog.extensions["opaque_command"]={{"version",52},{"mixed_selection_edit_intent",{{"version",45}}}};
    AssemblyInstance instance;instance.id=f.component;instance.type_id=f.type;instance.root_transform=AssemblyTransform{{5,1,.75},0,1};
    auto component=encode_document_assembly_instance({f.component,"assembly_instance",context},{f.catalog,instance});
    SlopedRoofPanel panel;panel.id=f.roof;panel.base_position={1,0,4};panel.orientation_radians=0;
    panel.run=2;panel.span=2;panel.rise=0;panel.pitch_radians=0;panel.overhang=0;panel.thickness=.25;
    auto roof=encode_building_entity(panel);
    // Real host margin admits the bottom gesture's world Y=-.25 mouth.
    // Child coordinates are local to (-10,-10), preserving the original world ink.
    SlopedRoofPanel containing{f.host,{-10,-10,3},0,40,40,0,0,0,.2,
        {{f.child,11,10.5,2,1,RoofSkylight{.06,.15,.024},0},
         {f.sibling,22,18,1.5,1.25,RoofSkylight{.08,.2,.03},0}}};
    auto host=encode_building_entity(containing);host.properties["version"]=4;
    for(auto& row:host.properties.at("roof_openings")) {
        row["rotation_rad"]=0.0;row["opaque_child_metadata"]={{"retain",row.at("id")}};
    }
    for(auto* entity:{&catalog,&component,&roof,&host}) {
        if(entity!=&catalog)for(auto it=context.begin();it!=context.end();++it)entity->properties[it.key()]=it.value();
        entity->extensions["opaque_fixture"]={{"retain",entity->id}};
    }
    auto asset=Asset::create(fresh("asset"),"application/octet-stream",{std::byte{0x31},std::byte{0x42}},{{"opaque_fixture",{{"retain","asset"}}}});
    std::vector<Entity> entities{
        {f.property,"property",{{"name","Roof cohort"}},false},
        {f.building,"building",{{"property_id",f.property},{"name","Building"}},false},
        {f.floor,"floor",{{"building_id",f.building},{"name","Ground"},{"elevation_m",0}},false},
        {f.layer,"layer",{{"floor_id",f.floor},{"name","Plan"}},false},catalog,component,roof,host};
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
        for(auto& entity:entities) if(entity.id==f.component || entity.id==f.roof || entity.id==f.host)
            entity.properties["presentation_frame"]={{"version",1},{"mode","building"}};
    }
    f.document=std::make_shared<Document>(Document::create(std::move(entities),{std::move(asset)}));
    const auto source=f.document->snapshot();auto retained=source.entities().at(f.component);
    retained.extensions["retained_history_fixture"]=true;
    f.document->apply(ApplyEntityChanges{source.revision(),{EntityChange::upsert(std::move(retained))},{},"Original three-roster source"});
    return f;
}
void native_box(const TopoDS_Shape& shape,Vec3 minimum,Vec3 opposite_corner,double volume) {
    require(!shape.IsNull(),"source manufactures a genuine native solid");require_near(solid_volume(shape),volume,"native hand volume");
    Bnd_Box box;BRepBndLib::Add(shape,box);require(!box.IsVoid(),"native source has bounds");
    double x0,y0,z0,x1,y1,z1;box.Get(x0,y0,z0,x1,y1,z1);
    const std::array actual{x0,y0,z0,x1,y1,z1},expected{minimum.x,minimum.y,minimum.z,opposite_corner.x,opposite_corner.y,opposite_corner.z};
    for(std::size_t i=0;i<actual.size();++i)
        require(std::isfinite(actual[i])&&std::abs(actual[i]-expected[i])<1e-6,"native bounds match hand prism including OCC tolerance");
    std::vector<Vec3> vertices;
    for(TopExp_Explorer it(shape,TopAbs_VERTEX);it.More();it.Next()) {
        const auto point=BRep_Tool::Pnt(TopoDS::Vertex(it.Current()));vertices.push_back({point.X(),point.Y(),point.Z()});
        require((std::abs(point.X()-minimum.x)<1e-7||std::abs(point.X()-opposite_corner.x)<1e-7)&&
            (std::abs(point.Y()-minimum.y)<1e-7||std::abs(point.Y()-opposite_corner.y)<1e-7)&&
            (std::abs(point.Z()-minimum.z)<1e-7||std::abs(point.Z()-opposite_corner.z)<1e-7),"native vertices are hand prism corners");
    }
    for(double x:{minimum.x,opposite_corner.x})for(double y:{minimum.y,opposite_corner.y})for(double z:{minimum.z,opposite_corner.z})
        require(std::any_of(vertices.begin(),vertices.end(),[&](Vec3 p){return std::abs(p.x-x)<1e-7&&std::abs(p.y-y)<1e-7&&std::abs(p.z-z)<1e-7;}),"every hand corner exists in actual native solid");
}
void admit_source(const Fixture& f) {
    const auto source=f.document->snapshot();const auto& entity=source.entities().at(f.roof);
    require(entity.properties.at("version")==1&&entity.properties.at("form")=="sloped_roof_panel","ordinary canonical schema1 panel");
    const auto roof=std::get<SlopedRoofPanel>(decode_building_entity(entity));
    require(roof.id==f.roof&&roof.openings.empty(),"whole roof has no openings");
    require_near(roof.base_position.x,1,"roof base X");require_near(roof.base_position.y,0,"roof base Y");require_near(roof.base_position.z,4,"roof base Z");
    require_near(roof.run,2,"roof run");require_near(roof.span,2,"roof span");require_near(roof.thickness,.25,"roof thickness");
    require_near(roof.orientation_radians,0,"roof yaw");require_near(roof.rise,0,"roof rise");require_near(roof.pitch_radians,0,"roof pitch");require_near(roof.overhang,0,"roof overhang");
    native_box(make_building_shape(roof),{1,0,3.75},{3,2,4},1);
    AssemblyExpansionBudget budget;const auto expansion=expand_document_assembly_instance(source.entities().at(f.component),source.entities(),budget);
    require(!expansion.source_instance.placement&&expansion.profiles.size()==1,"independent component expands one profile");
    const auto& p=expansion.profiles.front();
    require(p.part_path.empty()&&p.profile.outer.size()==4&&p.profile.holes.empty()&&(f.dimension.empty()==!p.profile.topology.has_value())&&!p.profile.planar_affine,"actual ordinary rectangular profile");
    const auto local=rectangle(0,0,2,1);
    for(std::size_t i=0;i<local.size();++i) {
        require_near(p.profile.outer[i].start.x,local[i].start.x,"component local start X");
        require_near(p.profile.outer[i].start.y,local[i].start.y,"component local start Y");
        require_near(p.profile.outer[i].end.x,local[i].end.x,"component local end X");
        require_near(p.profile.outer[i].end.y,local[i].end.y,"component local end Y");
        require_near(p.profile.outer[i].sweep_radians,0,"component straight edge");
    }
    require_near(p.transform.rotation_radians,0,"component yaw");require_near(p.transform.scale,1,"component scale");
    require_near(p.transform.vertical_scale,1,"component vertical scale");require(!p.transform.mirrored_y,"component unmirrored");
    require_near(p.profile.elevation_m,.25,"component elevation");require_near(p.profile.height_m,1.5,"component height");
    require_near(p.transform.translation_m.x,5,"component X");require_near(p.transform.translation_m.y,1,"component Y");require_near(p.transform.translation_m.z,.75,"component Z");
    const auto geometry=make_assembly_geometry(expansion);require(geometry.solids.size()==1,"one actual component solid");
    require_near(geometry.volume_m3,3,"component volume");native_box(geometry.solids.front().shape,{5,1,1},{7,2,2.5},3);
    const auto& host=source.entities().at(f.host);require(host.properties.at("version")==4,"different child host retains schema4");
    const auto decoded=std::get<SlopedRoofPanel>(decode_roof_entity(host));
    require(decoded.id!=roof.id&&decoded.openings.size()==2,"different actual host has selected child and genuine sibling");
    require_near(decoded.base_position.x,-10,"child host X margin");require_near(decoded.base_position.y,-10,"child host Y margin");
    require_near(decoded.base_position.z,3,"child host Z");require_near(decoded.run,40,"child host run");require_near(decoded.span,40,"child host span");require_near(decoded.thickness,.2,"child host thickness");
    for(const auto& child:decoded.openings) {
        require(child.skylight.has_value(),"both real children retain construction profiles");
        const bool selected=child.id==f.child;require(selected||child.id==f.sibling,"only fixture child identities");
        require_near(child.x,selected?11:22,"child X");require_near(child.y,selected?10.5:18,"child Y");
        require_near(child.width,selected?2:1.5,"child width");require_near(child.depth,selected?1:1.25,"child depth");
        require_near(child.rotation_radians,0,"child authored rotation");
        require_near(child.skylight->frame_width,selected?.06:.08,"actual frame profile");
        require_near(child.skylight->curb_height,selected?.15:.2,"actual curb profile");
        require_near(child.skylight->glazing_thickness,selected?.024:.03,"actual glazing profile");
    }
}
class ModalGuard {
    QTimer timer;QString title;
public:
    ModalGuard() {timer.setInterval(10);QObject::connect(&timer,&QTimer::timeout,[this] {
        if(auto* dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget())) {title=dialog->windowTitle();if(title.isEmpty())title="unnamed dialog";dialog->setAttribute(Qt::WA_DontShowOnScreen);dialog->reject();}
    });timer.start();}
    void verify() const {if(!title.isEmpty())throw std::runtime_error("Unexpected modal: "+title.toStdString());}
    void pause() {timer.stop();}
    void resume() {timer.start();}
};
void settle(const ModalGuard& guard) {for(int i=0;i<3;++i){QApplication::sendPostedEvents(nullptr,QEvent::LayoutRequest);QApplication::processEvents(QEventLoop::AllEvents,20);guard.verify();}}
PlanCanvas& canvas(MainWindow& window) {
    const auto name=window.workspace()==Workspace::architectural ? "architecturalPlanCanvas" : "measurementPlanCanvas";
    auto* result=dynamic_cast<PlanCanvas*>(window.findChild<QWidget*>(name));require(result,"actual active PlanCanvas exists");return *result;
}
// Coverage authenticates published native projection, not a generic fixture glyph.
void covered(const Boundary& actual,const Boundary& expected) {
    for(const auto& edge:expected) {
        const auto dx=edge.end.x-edge.start.x,dy=edge.end.y-edge.start.y,length=std::hypot(dx,dy);
        std::vector<std::pair<double,double>> intervals;
        for(const auto& candidate:actual) {
            if(std::abs(candidate.sweep_radians)>1e-7)continue;
            const auto cross=[&](Vec2 p){return ((p.x-edge.start.x)*dy-(p.y-edge.start.y)*dx)/length;};
            if(std::abs(cross(candidate.start))>1e-7||std::abs(cross(candidate.end))>1e-7)continue;
            const auto along=[&](Vec2 p){return ((p.x-edge.start.x)*dx+(p.y-edge.start.y)*dy)/length;};
            double a=along(candidate.start),b=along(candidate.end);if(a>b)std::swap(a,b);intervals.emplace_back(a,b);
        }
        std::sort(intervals.begin(),intervals.end());double reach=0;
        for(const auto& [a,b]:intervals)if(a<=reach+1e-7)reach=std::max(reach,b);
        require(reach>=length-1e-7,"published native ink covers every hand edge");
    }
}
QImage output(PlanCanvas& drawing) {
    QImage image(drawing.size(),QImage::Format_ARGB32_Premultiplied);QPainter painter(&image);
    drawing.renderSceneAt(painter,image.rect(),drawing.viewScale(),drawing.viewCenter(),Qt::white);painter.end();return image;
}
void unchanged(MainWindow& window,const DocumentSnapshot& before) {
    const auto after=window.document().snapshot();
    require(document_snapshot_digest(after)==document_snapshot_digest(before)&&after.entities()==before.entities()&&after.assets()==before.assets()&&
        after.revision()==before.revision()&&after.saved_revision_optional()==before.saved_revision_optional()&&after.history().size()==before.history().size(),
        "selection preserves complete source, archives, history, revision, save and digest");
}
const CanvasRoofOpeningControls& controls(const PlanCanvas& drawing,const Fixture& f,const std::string& child,std::optional<Revision> revision=std::nullopt) {
    const auto& all=drawing.roofOpeningControls();
    const auto found=std::find_if(all.begin(),all.end(),[&](const auto& item){return item.target.roof_id==QString::fromStdString(f.host)&&item.target.opening_id==QString::fromStdString(child);});
    require(found!=all.end(),"actual qualified mouth controls exist");
    require((!revision || found->target.source_revision==*revision)&&found->presentation_owner_id==QString::fromStdString(f.host),"controls qualify current revision and actual host");
    require(std::count_if(all.begin(),all.end(),[&](const auto& item){return item.target==found->target;})==1,"unambiguous qualified mouth");return *found;
}
std::string scenario(const QString& directory,const std::array<int,3>& order) {
    auto f=fixture();admit_source(f);const auto before=f.document->snapshot();ModalGuard guard;
    MainWindow window(f.document,nullptr,QDir(directory).filePath(QString::fromStdString(fresh("library"))+".json"));
    window.setAttribute(Qt::WA_DontShowOnScreen);window.resize(1400,900);window.setWorkspace(Workspace::measurement);window.setMetricUnits(true);window.show();settle(guard);
    auto* drawing=dynamic_cast<PlanCanvas*>(window.findChild<QWidget*>("measurementPlanCanvas"));require(drawing,"actual measurement canvas exists");
    auto& canvas=*drawing;auto* snap=window.findChild<QToolButton*>("snapTool");require(snap,"actual Snap toggle exists");snap->setChecked(false);
    canvas.setOverviewMapEnabled(false);canvas.setGridEnabled(false);canvas.setTool(CanvasTool::select);canvas.setSnapEnabled(false);canvas.setWallSnapEnabled(false);canvas.setViewTransform({4,1},45);settle(guard);
    const auto committed=output(canvas);require(!committed.isNull(),"actual committed scene renders");
    const auto target=controls(canvas,f,f.child,before.revision()).target;const auto mouth=controls(canvas,f,f.child,before.revision()).frame;
    require_near(mouth.center.x,2,"mouth centre X");require_near(mouth.center.y,1,"mouth centre Y");
    require_near(mouth.width_metres,2,"mouth width");require_near(mouth.depth_metres,1,"mouth depth");require_near(mouth.rotation_radians,0,"mouth rotation");
    require_near(mouth.reference_along.x,1,"flat mouth along X");require_near(mouth.reference_along.y,0,"flat mouth along Y");
    require_near(mouth.reference_across.x,0,"flat mouth across X");require_near(mouth.reference_across.y,1,"flat mouth across Y");
    require_near(mouth.width_surface_scale,1,"flat mouth width surface scale");require_near(mouth.depth_surface_scale,1,"flat mouth depth surface scale");
    const auto sibling=controls(canvas,f,f.sibling,before.revision()).frame;require_near(sibling.center.x,12.75,"sibling centre X");require_near(sibling.center.y,8.625,"sibling centre Y");
    for(const auto& id:{f.component,f.roof,f.host}) {
        Boundary ink;for(const auto& item:canvas.entities())if(item.id==QString::fromStdString(id))ink.insert(ink.end(),item.segments.begin(),item.segments.end());
        require(!ink.empty(),"actual component and both roofs publish native ink");
        covered(ink,id==f.component?rectangle(5,1,2,1):id==f.roof?rectangle(1,0,2,2):rectangle(-10,-10,40,40));
        if(id==f.host) {covered(ink,rectangle(1,.5,2,1));covered(ink,rectangle(12,8,1.5,1.25));}
    }
    require(window.selectEntity({}),"clear actual selection");QStringList expected;bool child_selected=false;
    for(const int member:order) {
        if(member<2) {
            const auto id=QString::fromStdString(member==0?f.component:f.roof);
            require(window.selectEntity(id,true),"ordinary owner is selected through actual window API");expected.push_back(id);
        } else {
            // Ctrl mouth selection has native child precedence even over the overlapping selected whole roof.
            const auto center=canvas.viewCenter();const auto point=QRectF(canvas.rect()).center()+QPointF((mouth.center.x-center.x)*canvas.viewScale(),-(mouth.center.y-center.y)*canvas.viewScale());
            require(canvas.rect().contains(point.toPoint()),"mouth click lies inside canvas");
            for(const auto kind:{QEvent::MouseButtonPress,QEvent::MouseButtonRelease}) {
                QMouseEvent event(kind,point,canvas.mapToGlobal(point.toPoint()),Qt::LeftButton,kind==QEvent::MouseButtonRelease?Qt::NoButton:Qt::LeftButton,Qt::ControlModifier);
                QApplication::sendEvent(&canvas,&event);
            }
            child_selected=true;
        }
        settle(guard);auto actual=canvas.selectedIds();auto sorted=expected;actual.sort();sorted.sort();
        require(actual==sorted,"each permutation prefix preserves precisely its ordinary semantic roster");
        require(canvas.selectedRoofOpenings()==(child_selected?std::vector{target}:std::vector<CanvasRoofOpeningTarget>{}),"each prefix preserves precisely its qualified child roster");
        if(child_selected)require(canvas.selectedRoofOpening()&&*canvas.selectedRoofOpening()==target,"observed single-child compatibility view is qualified");
        unchanged(window,before);
    }
    require(expected.size()==2&&canvas.selectedRoofOpenings().size()==1,"all three semantic members admitted");
    const auto hit=canvas.selectedRoofOpeningAtModelPoint(mouth.center);require(hit&&*hit==target,"selected mouth hit authenticates qualified child");
    QStringList published;for(const auto& item:canvas.entities())if(item.selected&&!published.contains(item.id))published.push_back(item.id);
    published.sort();expected.sort();require(published==expected,"published ordinary membership excludes child host and sibling");
    const auto painted=canvas.grab();require(!painted.isNull(),"actual selected canvas paints");
    const auto capture_directory=qEnvironmentVariable("VERTEX_TEST_CAPTURE_DIR");
    const auto order_label=QString::number(order[0])+QString::number(order[1])+QString::number(order[2]);
    if(!capture_directory.isEmpty())require(QDir().mkpath(capture_directory)&&painted.save(QDir(capture_directory).filePath(
        "roof-skylight-frame-"+order_label+"-selected.png")),"optional uniquely prefixed six-order source capture saves");
    const auto frame=canvas.selectionAxisResizeCohortFrame();const auto grips=canvas.selectionAxisHandlePositions();
    require(!canvas.entityTransformPreviewPending()&&canvas.entityTransformPreview().empty()&&
        !canvas.roofOpeningPreviewPending()&&!canvas.roofOpeningPreviewBoundary()&&!canvas.roofOpeningPreviewEdit()&&
        !canvas.roofOpeningGroupPreviewPending()&&!canvas.roofOpeningGroupPreviewBoundary()&&!canvas.roofOpeningGroupPreviewEdit()&&canvas.roofOpeningGroupPreviewControls().empty(),
        "selection and frame query create no provisional native preview");
    unchanged(window,before);require(output(canvas)==committed,"selection and frame preserve committed native ink");guard.verify();
    const auto label=std::to_string(order[0])+std::to_string(order[1])+std::to_string(order[2]);
    if(!frame)return label+": MissingFrame: independent component + whole schema1 roof + different-host typed skylight";
    require_near(frame->center.x,4,"hand frame centre X");require_near(frame->center.y,1,"hand frame centre Y");
    require_near(frame->width_metres,6,"hand frame width");require_near(frame->depth_metres,2,"hand frame depth");require_near(frame->rotation_radians,0,"hand frame rotation");
    require_near(frame->center.x-frame->width_metres/2,1,"hand frame minimum X");require_near(frame->center.y-frame->depth_metres/2,0,"hand frame minimum Y");
    require(grips.size()==4,"common analytical frame has four actual side grips");
    // Unit release and release without a grip must leave the entire original source intact.
    auto unit_points=grips;unit_points.push_back(QPointF(20,20));
    for(const auto point:unit_points) {
        for(const auto kind:{QEvent::MouseButtonPress,QEvent::MouseButtonRelease}) {
            QMouseEvent event(kind,point,canvas.mapToGlobal(point.toPoint()),Qt::LeftButton,
                kind==QEvent::MouseButtonRelease?Qt::NoButton:Qt::LeftButton,Qt::ShiftModifier);
            QApplication::sendEvent(&canvas,&event);
        }
        settle(guard);unchanged(window,before);
        require(canvas.entityTransformPreview().empty()&&!canvas.entityTransformPreviewPending()&&
            !canvas.roofOpeningPreviewPending()&&!canvas.roofOpeningPreviewBoundary()&&!canvas.roofOpeningPreviewEdit()&&
            !canvas.roofOpeningGroupPreviewPending()&&!canvas.roofOpeningGroupPreviewBoundary()&&
            !canvas.roofOpeningGroupPreviewEdit()&&canvas.roofOpeningGroupPreviewControls().empty(),
            "unit/no-grip release creates no provisional geometry or child controls");
        require(output(canvas)==committed,"unit/no-grip release preserves committed output");
    }
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
    if(!directory.isEmpty()) require(QDir().mkpath(directory) && painted.save(QDir(directory).filePath(name.startsWith("roof-skylight-") ? name : "roof-skylight-"+name)),"optional native roof capture saves");
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
    actual.sort();expected.sort();require(actual==expected,"ordinary roster contains precisely component and whole roof");
    const auto target=controls(drawing,f,f.child).target;
    require(drawing.selectedRoofOpenings()==std::vector{target} &&
        drawing.selectedRoofOpening() && *drawing.selectedRoofOpening()==target,"typed roster contains precisely qualified different-host child");
    QStringList published;
    for(const auto& item:drawing.entities()) if(item.selected&&!published.contains(item.id))published.push_back(item.id);
    published.sort();require(published==expected,"unselected containing host and sibling never become ordinary owners");
}
void select(MainWindow& window,const Fixture& f,int first,const ModalGuard& guard) {
    require(window.selectEntity({}),"clear actual public selection");
    auto& drawing=canvas(window);const auto target=controls(drawing,f,f.child,window.document().revision()).target;
    const auto mouth=controls(drawing,f,f.child).frame.center;
    const std::array<std::array<int,3>,3> orders{{{{0,1,2}},{{1,2,0}},{{2,0,1}}}};
    QStringList owners;bool child=false;
    for(const int member:orders.at(static_cast<std::size_t>(first))) {
        if(member<2) {
            const auto id=QString::fromStdString(member==0?f.component:f.roof);
            require(window.selectEntity(id,true),"actual window admits ordered ordinary member");owners.push_back(id);
        } else {
            const auto center=drawing.viewCenter();
            const auto point=QRectF(drawing.rect()).center()+QPointF((mouth.x-center.x)*drawing.viewScale(),-(mouth.y-center.y)*drawing.viewScale());
            require(drawing.rect().contains(point.toPoint()),"actual qualified mouth click lies inside canvas");
            for(const auto kind:{QEvent::MouseButtonPress,QEvent::MouseButtonRelease}) {
                QMouseEvent event(kind,point,drawing.mapToGlobal(point.toPoint()),Qt::LeftButton,
                    kind==QEvent::MouseButtonRelease?Qt::NoButton:Qt::LeftButton,Qt::ControlModifier);
                QApplication::sendEvent(&drawing,&event);
            }
            child=true;
        }
        settle(guard);auto actual=drawing.selectedIds(),expected=owners;actual.sort();expected.sort();
        require(actual==expected,"each lifecycle prefix has exact ordinary roster");
        require(drawing.selectedRoofOpenings()==(child?std::vector{target}:std::vector<CanvasRoofOpeningTarget>{}),
            "each lifecycle prefix has exact qualified child roster");
    }
    roster(drawing,f);
}
void mouth_controls(const PlanCanvas& drawing,const Fixture& f,Revision revision,ResizeCase c={}) {
    const auto& mouth=controls(drawing,f,f.child,revision).frame;
    const auto low=hand({1,.5},c),high=hand({3,1.5},c);
    const auto center=presented({(low.x+high.x)/2,(low.y+high.y)/2},f.site);
    require_near(mouth.center.x,center.x,"authenticated mouth center X");
    require_near(mouth.center.y,center.y,"authenticated mouth center Y");
    require_near(mouth.width_metres,high.x-low.x,"authenticated mouth width");
    require_near(mouth.depth_metres,high.y-low.y,"authenticated mouth depth");
    require_near(mouth.rotation_radians,0,"authenticated authored mouth surface angle");
    require_near(mouth.reference_along.x,f.site?0:1,"actual mouth along X");
    require_near(mouth.reference_along.y,f.site?1:0,"actual mouth along Y");
    require_near(mouth.reference_across.x,f.site?-1:0,"actual mouth across X");
    require_near(mouth.reference_across.y,f.site?0:1,"actual mouth across Y");
    require_near(mouth.width_surface_scale,1,"flat width scale");
    require_near(mouth.depth_surface_scale,1,"flat depth scale");
    const auto& sibling=controls(drawing,f,f.sibling,revision).frame;
    const auto sibling_center=presented({12.75,8.625},f.site);
    require_near(sibling.center.x,sibling_center.x,"unselected sibling center X");
    require_near(sibling.center.y,sibling_center.y,"unselected sibling center Y");
    require_near(sibling.width_metres,1.5,"unselected sibling width");
    require_near(sibling.depth_metres,1.25,"unselected sibling depth");
}

void plans(const std::vector<CanvasEntity>& scene,const Fixture& f,ResizeCase c={}) {
    for(const auto& id:{f.component,f.roof,f.host}) {
        Boundary ink;
        for(const auto& item:scene)if(item.id==QString::fromStdString(id))
            ink.insert(ink.end(),item.segments.begin(),item.segments.end());
        require(!ink.empty(),"manufactured scene contains all three physical contributions");
        if(id==f.component)covered(ink,hand_boundary(rectangle(5,1,2,1),c,f.site));
        else if(id==f.roof)covered(ink,hand_boundary(rectangle(1,0,2,2),c,f.site));
        else {
            covered(ink,hand_boundary(rectangle(-10,-10,40,40),{},f.site));
            covered(ink,hand_boundary(rectangle(12,8,1.5,1.25),{},f.site));
            // Inspect native cavity edges in actual manufactured host geometry.
            covered(ink,hand_boundary(rectangle(1,.5,2,1),c,f.site));
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
    const auto& host=source.entities().at(f.host);
    require(host.properties.at("version")==4&&host.properties.at("roof_openings").size()==2,"different host retains schema4 and both real children");
    const auto decoded=std::get<SlopedRoofPanel>(decode_roof_entity(host));
    require_near(decoded.base_position.x,-10,"host root X invariant");require_near(decoded.base_position.y,-10,"host root Y invariant");
    require_near(decoded.base_position.z,3,"host root Z invariant");require_near(decoded.run,40,"host run invariant");
    require_near(decoded.span,40,"host span invariant");require_near(decoded.thickness,.2,"host thickness invariant");
    const auto child=std::find_if(decoded.openings.begin(),decoded.openings.end(),[&](const auto& item){return item.id==f.child;});
    require(child!=decoded.openings.end()&&child->skylight.has_value(),"actual typed skylight remains editable");
    const auto mouth_low=hand({1,.5},c),mouth_high=hand({3,1.5},c);
    require_near(child->x-10,mouth_low.x,"child local X reexpresses hand world mouth");
    require_near(child->y-10,mouth_low.y,"child local Y reexpresses hand world mouth");
    require_near(child->width,mouth_high.x-mouth_low.x,"hand child width");
    require_near(child->depth,mouth_high.y-mouth_low.y,"hand child depth");
    require_near(child->rotation_radians,0,"authored child surface angle");
    require_near(child->skylight->frame_width,.06,"child frame construction invariant");
    require_near(child->skylight->curb_height,.15,"child curb construction invariant");
    require_near(child->skylight->glazing_thickness,.024,"child glazing construction invariant");
    return {{"component",vertices},{"roof",{base.x,base.y,4,roof.run,roof.span,0,.25}},
        {"mouth",{mouth_low.x,mouth_low.y,child->width,child->depth}}};
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
        if(id!=f.component && id!=f.catalog && id!=f.roof && id!=f.host) require(actual==entity,"unselected context and unrelated carriers remain exact");
        else if(id!=f.roof && id!=f.host) require(actual.extensions.dump()==entity.extensions.dump(),"owner/catalog opaque extension envelopes remain exact");
        if(id==f.component || id==f.roof || id==f.host) {
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
    if(deadline.elapsed()>=1000) std::cout<<"[native wait] "<<message<<": "<<deadline.elapsed()<<"ms\n"<<std::flush;
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
    wait(window,guard,[&]{return !drawing.entityTransformPreviewPending() && !drawing.entityTransformPreview().empty();},
        "detached component/roof native preview finishes",30000);
    mouth_controls(drawing,f,before.revision());
    c.resized=true;plans(drawing.entityTransformPreview(),f,c);plans(drawing.entities(),f);
    QStringList ids;for(const auto& item:drawing.entityTransformPreview()) if(!ids.contains(item.id)) ids.push_back(item.id);
    QStringList expected_ids{QString::fromStdString(f.component),QString::fromStdString(f.roof),QString::fromStdString(f.host)};ids.sort();expected_ids.sort();
    require(ids==expected_ids,"detached native preview has precisely component, whole roof and manufactured child host");
    unchanged(window,before);frame(drawing,f);roster(drawing,f);
    require(drawing.selectedEntityPresentations()==presentations && drawing.roofOpeningControlsGeneration()==generation,
            "detached manufacture preserves captured presentation identities and publication generation");
    require(output(drawing)==committed,"complete detached preview remains outside committed Print output");return release;
}
void mixed45_command(const DocumentSnapshot& after,const Fixture& f,const DocumentSnapshot& before,ResizeCase c={true,false,false}) {
    const auto& command=after.history().back().boundary_constraint_changes;
    require(command && command->mixed_selection_edit_completion && command->mixed_selection_edit_intent.at("version")==45,
            "actual release publishes sealed Properties45 completion");
    require(command_to_json(*command).at("version")==52,"retained outer command remains canonical command52");
    const auto& intent=command->mixed_selection_edit_intent;
    require(intent.size()==13 && intent.at("room_reviews").empty() && intent.at("phase_room_reviews").empty() &&
            intent.at("roof_plan_resizes").size()==1&&intent.at("roof_opening_axis_requests").size()==1,"canonical Properties45 retains one roof row and empty room reviews");
    for(const auto* key:{"version","expected_revision","source_snapshot_digest","source_authoring_digest","source_entities_digest",
        "source_saved_revision","component_request","component_identity_reservations","roof_plan_resizes","roof_opening_axis_requests","room_reviews","phase_room_reviews","message"})
        require(intent.contains(key),"retained Properties45 has exactly its thirteen mandatory source-bound fields");
    const auto& row=intent.at("roof_plan_resizes").at(0);
    require(row.size()==7 && row.at("version")==2 && row.at("anchor_mode")=="shared_frame" &&
        row==encode_roof_plan_resize_intent(decode_roof_plan_resize_intent(row)) && row.at("roof_id")==f.roof,
        "one closed RoofPlanResizeIntent v2 row identifies actual roof and shared anchor");
    const auto& child=intent.at("roof_opening_axis_requests").at(0);
    require(child.size()==6,"one closed six-field child operator");
    for(const auto* key:{"roof_id","opening_id","sx","sy","anchor_m","frame_rotation_radians"})
        require(child.contains(key),"qualified child row contains every mandatory field");
    require(child.at("roof_id")==f.host&&child.at("opening_id")==f.child&&f.host!=f.roof,
        "command retains disjoint actual whole-roof and child-host identities");
    const Vec2 source_anchor=c.depth?Vec2{4,c.opposite?2.0:0.0}:Vec2{c.opposite?7.0:1.0,1};
    const auto canvas_anchor=presented(source_anchor,f.site);
    for(const auto& owner:{f.component,f.roof,f.host}) {
        const auto placement=resolve_site_presentation(before,owner);
        const auto actual_source=site_transform_point({canvas_anchor.x,canvas_anchor.y,0},placement.inverse);
        require_near(actual_source.x,source_anchor.x,"actual per-owner Site inverse anchor X");
        require_near(actual_source.y,source_anchor.y,"actual per-owner Site inverse anchor Y");
        if(owner!=f.component) {
            const auto& operator_row=owner==f.roof?row:child;
            require_near(operator_row.at("anchor_m").at(0).get<double>(),actual_source.x,"retained operator uses actual inverse anchor X");
            require_near(operator_row.at("anchor_m").at(1).get<double>(),actual_source.y,"retained operator uses actual inverse anchor Y");
            require_near(operator_row.at("frame_rotation_radians").get<double>(),-placement.forward.rotation_radians,
                "retained operator axis uses actual source Site rotation");
        }
    }
    const bool canvas_x=f.site?c.depth:!c.depth;
    require_near(row.at("scale_x").get<double>(),canvas_x?1.5:1,"roof canvas X factor");
    require_near(row.at("scale_y").get<double>(),canvas_x?1:1.5,"roof canvas Y factor");
    require_near(child.at("sx").get<double>(),row.at("scale_x").get<double>(),"child preserves identical captured canvas X factor");
    require_near(child.at("sy").get<double>(),row.at("scale_y").get<double>(),"child preserves identical captured canvas Y factor");
    const auto& component=intent.at("component_request");
    require(component.at("version")==7||component.at("version")==8||component.at("version")==9,
        "component request retains existing geometry-only version7/8/9");
    require(!intent.contains("physical_identity_allocation")&&!intent.contains("physical_id_allocation"),
        "three existing semantic members allocate no physical identities");
    require(intent.at("expected_revision")==before.revision() &&
        intent.at("source_snapshot_digest")==document_snapshot_digest(before) &&
        intent.at("source_authoring_digest")==document_authoring_source_digest_v2(before) &&
        intent.at("source_entities_digest")==entity_map_digest(before.entities()) &&
        intent.at("source_saved_revision")==(before.saved_revision_optional() ? Json(*before.saved_revision_optional()) : Json(nullptr)),
        "all five source fences bind the complete actual original snapshot");
    for(const auto* key:{"expected_revision","source_snapshot_digest","source_authoring_digest","source_entities_digest","source_saved_revision"})
        require(component.at(key)==intent.at(key),"component and mixed command retain all five identical original-source fences");
    require(ProjectStore::required_format_version(after)==268 && ProjectStore::required_semantic_format_version(after)==268,
            "canonical retained command requires native and semantic reader268");
}
void floor268(const DocumentSnapshot& saved) {
    require(ProjectStore::required_format_version(saved)==268 && ProjectStore::required_semantic_format_version(saved)==268,
            "applied/undone/redone retained canonical command requires reader268");
}
void fresh_open(const QString& directory,const QString& stem,const DocumentSnapshot& saved,const Fixture& f,
                const ModalGuard& guard,int first,ResizeCase c) {
    MainWindow reopened({},nullptr,QDir(directory).filePath(stem+"-reopened-library.json"));
    require(reopened.openProject(QDir(directory).filePath(stem+".bldproj")) && reopened.document().is_editable(),"ordinary fresh Open acquires actual editable writer ownership");
    display(reopened,guard,f);const auto loaded=reopened.document().snapshot();
    require(document_snapshot_digest(loaded)==document_snapshot_digest(saved) && loaded.entities()==saved.entities() && loaded.assets()==saved.assets(),
            "fresh editable Open retains exact saved source, assets, revision, save and complete retained history");floor268(loaded);
    const auto extracted=std::filesystem::path(QDir(directory).filePath(stem+"-extracted").toStdWString());
    extract_project(loaded,extracted);std::ifstream input(extracted/"project.json");const auto manifest=Json::parse(input);
    require(manifest.at("exchange_version")==266,"actual native-persisted extraction requires exchange266");
    bool retained_command=false;
    for(const auto& record:saved.history()) if(record.boundary_constraint_changes &&
        record.boundary_constraint_changes->mixed_selection_edit_intent.value("version",0)==45) {
        const auto wire=command_to_json(*record.boundary_constraint_changes);
        require(command_to_json(*loaded.history().at(record.revision).boundary_constraint_changes)==wire&&
            manifest.at("revisions").at(record.revision).at("boundary_constraint_changes")==wire,
            "fresh native Open and extraction retain same complete original-source command wire and both operator rosters");
        retained_command=true;
    }
    require(retained_command,"applied/undone/redone extraction authenticates a real retained Properties45 command");
    auto* undo=reopened.findChild<QAction*>("undoCommand");
    require(undo && !undo->isEnabled() && !reopened.undoCommand() && !reopened.redoCommand(),"fresh editable Open has no Undo/Redo boundary despite retained history");
    unchanged(reopened,loaded);structure(loaded,f,c);select(reopened,f,first,guard);mouth_controls(canvas(reopened),f,loaded.revision(),c);
    auto& drawing=canvas(reopened);plans(drawing.entities(),f,c);frame(drawing,f,c);roster(drawing,f);no_preview(drawing);
    capture(drawing,stem+"-reopened.png");guard.verify();
}
Json gesture_scenario(const QString& directory,int first,bool depth,bool site,bool opposite=false) {
    const auto f=fixture(false,site);admit_source(f);ResizeCase c{true,depth,opposite};
    const auto stem=QString(site ? "roof-skylight-site-" : "roof-skylight-plan-")+(depth ? "depth-" : "width-")+
        (opposite ? "opposite-" : "")+(first==0 ? "component-first" : first==1 ? "whole-first" : "child-first");
    std::cout<<"[roof/skylight desktop] "<<stem.toStdString()<<'\n'<<std::flush;
    ModalGuard guard;MainWindow window(f.document,nullptr,QDir(directory).filePath(stem+"-library.json"));display(window,guard,f);select(window,f,first,guard);
    auto& drawing=canvas(window);const auto before=window.document().snapshot();
    require(ProjectStore::required_format_version(before)<268 && ProjectStore::required_semantic_format_version(before)<268,"opaque command-shaped metadata does not create reader268 authority");
    structure(before,f);plans(drawing.entities(),f);const auto committed=output(drawing);capture(drawing,stem+"-selected.png");
    const auto release=completed_preview(window,f,before,guard,c);capture(drawing,stem+"-preview.png");mouse(drawing,QEvent::MouseButtonRelease,release);
    wait(window,guard,[&]{return window.document().revision()==before.revision()+1 && !drawing.entityTransformPreviewPending();},"one side release publishes all three members exactly once",30000);
    const auto after=window.document().snapshot();mixed45_command(after,f,before,c);
    require(after.history().size()==before.history().size()+1,"one release creates one retained mixed history event");
    no_preview(drawing);mouth_controls(drawing,f,after.revision(),c);const auto result=structure(after,f,c);preserved(before,after,f);plans(drawing.entities(),f,c);frame(drawing,f,c);roster(drawing,f);
    require(output(drawing)!=committed,"release publishes changed native geometry into committed Print output");capture(drawing,stem+"-committed.png");const auto output_after=output(drawing);
    const auto save=[&](const QString& state) {
        const auto current=window.document().snapshot();require(window.saveProjectAs(QDir(directory).filePath(stem+state+".bldproj")),"actual native lifecycle state saves");
        const auto saved=window.document().snapshot();require(saved.entities()==current.entities() && saved.assets()==current.assets(),"Save retains exact authoring graph and assets");floor268(saved);return saved;
    };
    const auto applied=save("-applied");
    require(window.undoCommand() && window.document().snapshot().entities()==before.entities() && window.document().snapshot().assets()==before.assets(),"one Undo restores exact original owners, all receipts and assets");
    const auto undo_state=window.document().snapshot();
    require(undo_state.revision()==after.revision()+1 && undo_state.history().size()==after.history().size()+1 && undo_state.history().back().action=="undo" &&
            undo_state.history().back().source_revision==before.revision(),"one Undo appends exactly one navigation event targeting the actual source");
    mouth_controls(drawing,f,undo_state.revision());structure(undo_state,f);plans(drawing.entities(),f);frame(drawing,f);roster(drawing,f);require(output(drawing)==committed,"Undo restores exact original committed native output");
    const auto undone=save("-undone");
    require(window.redoCommand() && window.document().snapshot().entities()==after.entities() && window.document().snapshot().assets()==after.assets(),"one Redo restores exact resized owners, all retained envelopes and assets");
    const auto redo_state=window.document().snapshot();
    require(redo_state.revision()==after.revision()+2 && redo_state.history().size()==after.history().size()+2 && redo_state.history().back().action=="redo" &&
            redo_state.history().back().source_revision==after.revision(),"one Redo appends exactly one navigation event targeting the actual result");
    mouth_controls(drawing,f,redo_state.revision(),c);structure(redo_state,f,c);plans(drawing.entities(),f,c);frame(drawing,f,c);roster(drawing,f);
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
    const auto stem=QStringLiteral("roof-skylight-stale-")+(authority==StaleAuthority::selection ? "selection" : authority==StaleAuthority::viewport ? "viewport" :
        authority==StaleAuthority::presentation ? "presentation" : authority==StaleAuthority::geometry ? "same-id-revision-geometry" : "history");
    std::cout<<"[roof/skylight desktop] "<<stem.toStdString()<<'\n'<<std::flush;
    ModalGuard guard;MainWindow window(f.document,nullptr,QDir(directory).filePath(stem+"-library.json"));display(window,guard,f);select(window,f,0,guard);
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
                mouth_controls(drawing,m_fixture,m_before.revision());
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
    const auto stem=QStringLiteral("roof-skylight-review-")+(action==ReviewAction::cancel ? "cancel" : action==ReviewAction::retire ? "remove" :
        action==ReviewAction::stale_presentation ? "stale-presentation" : "stale-history");
    std::cout<<"[roof/skylight desktop] "<<stem.toStdString()<<'\n'<<std::flush;
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
        const auto after=window.document().snapshot();mixed45_command(after,f,before);no_preview(drawing);mouth_controls(drawing,f,after.revision(),{true,false,false});
        require(after.history().size()==before.history().size()+1 && !after.entities().contains(f.dimension),"one reviewed event combines both owners and explicit dimension retirement");
        structure(after,f,{true,false,false});preserved(before,after,f,true);plans(drawing.entities(),f,{true,false,false});frame(drawing,f,{true,false,false});roster(drawing,f);
        require(output(drawing)!=committed,"actual reviewed Apply publishes changed committed output");capture(drawing,stem+"-committed.png");
        const auto reviewed_output=output(drawing);
        require(window.undoCommand() && window.document().snapshot().entities()==before.entities() && window.document().snapshot().assets()==before.assets(),"one reviewed Undo restores both original owners, exact dimension and assets");
        require(output(drawing)==committed,"reviewed Undo restores exact committed source output");
        require(window.document().snapshot().entities().at(f.dimension)==before.entities().at(f.dimension),"reviewed Undo restores exact original dimension carrier");
        require_near(decode_boundary_dimension_entity(window.document().snapshot().entities().at(f.dimension)).dimension->resolve(window.document().snapshot()).segment_length(),2,
            "reviewed Undo restores actual resolved original dimension length");
        mouth_controls(drawing,f,window.document().revision());
        require(window.redoCommand() && window.document().snapshot().entities()==after.entities() && window.document().snapshot().assets()==after.assets(),"one reviewed Redo restores both resized owners and explicit dimension retirement");
        mouth_controls(drawing,f,window.document().revision(),{true,false,false});
        plans(drawing.entities(),f,{true,false,false});
        require(output(drawing)==reviewed_output,"reviewed Redo restores exact committed resized output");
        require(window.saveProjectAs(QDir(directory).filePath(stem+".bldproj")),"reviewed cohort retirement saves");
        const auto saved=window.document().snapshot();floor268(saved);require(window.createNewProject(),"release reviewed writer lease before editable Open");
        fresh_open(directory,stem,saved,f,guard,0,{true,false,false});
    }
    review.verify();guard.verify();
}
} // namespace
int main(int argc,char** argv) {
    sketch::testing::noninteractive_errors();qputenv("QT_QPA_PLATFORM","offscreen");QStandardPaths::setTestModeEnabled(true);QApplication application(argc,argv);
    QTemporaryDir settings;QCoreApplication::setOrganizationName("VertexTests");QCoreApplication::setApplicationName("MixedComponentRoofSkylightResize-"+QUuid::createUuid().toString(QUuid::WithoutBraces));
    QSettings::setDefaultFormat(QSettings::IniFormat);QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,settings.path());QSettings::setPath(QSettings::IniFormat,QSettings::SystemScope,settings.path());
    try {
        require(settings.isValid(),"private settings directory exists");const auto font=QFontDatabase::addApplicationFont(":/fonts/Inter.ttf");require(font>=0,"bundled Inter loads");
        const auto families=QFontDatabase::applicationFontFamilies(font);require(!families.isEmpty(),"Inter exposes family");application.setFont(QFont(families.front(),10));
        QTemporaryDir directory;require(directory.isValid(),"private fixture directory exists");
        const bool frame_only=application.arguments().contains("--frame-only"),site_only=application.arguments().contains("--site-only");
        const bool review_only=application.arguments().contains("--review-only"),stale_only=application.arguments().contains("--stale-only");
        const bool left_only=application.arguments().contains("--left-only");
        std::array order{0,1,2};std::vector<std::string> missing,unexpected;int admitted=0,lifecycles=0,reviews=0,stale=0;
        // Keep all six original native source/ink/control/prefix gates ahead of lifecycle work.
        do {
            try {const auto diagnostic=scenario(directory.path(),order);++admitted;if(!diagnostic.empty())missing.push_back(diagnostic);}
            catch(const std::exception& error){unexpected.push_back(std::to_string(order[0])+std::to_string(order[1])+std::to_string(order[2])+": UnexpectedError: "+error.what());}
        } while(std::next_permutation(order.begin(),order.end()));
        for(const auto& message:unexpected)std::cerr<<message<<'\n';
        for(const auto& message:missing)std::cerr<<message<<'\n';
        std::cerr<<"source/selection admitted permutations: "<<admitted<<"/6\n";
        if(!unexpected.empty()||admitted!=6||!missing.empty())return 1;
        if(left_only) {gesture_scenario(directory.path(),0,false,false,true);++lifecycles;}
        else if(!frame_only) {
            if(!review_only&&!stale_only)for(const bool site:{false,true})for(const bool depth:{false,true})for(const bool opposite:{false,true}) {
                if(site_only&&!site)continue;
                std::optional<Json> first;
                for(int order_first=0;order_first<3;++order_first) {
                    const auto result=gesture_scenario(directory.path(),order_first,depth,site,opposite);++lifecycles;
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
        std::cout<<"mixed_component_roof_skylight_resize_desktop_tests passed: "<<admitted<<" frame permutations, "
            <<lifecycles<<" lifecycle cases, "<<reviews<<" dimension review cases, "<<stale<<" stale-authority cases\n";return 0;
    }catch(const std::exception& error){std::cerr<<"mixed_component_roof_skylight_resize_desktop_tests: "<<error.what()<<'\n';return 1;}
}
