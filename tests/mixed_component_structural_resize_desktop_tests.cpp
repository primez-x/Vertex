#include "sketch/desktop/main_window.hpp"
#include "sketch/assembly_document_adapter.hpp"
#include "sketch/building_entity.hpp"
#include "sketch/boundary_dimension.hpp"
#include "sketch/document_digest.hpp"
#include "sketch/model_phases.hpp"
#include "../src/desktop/plan_canvas.hpp"
#include "support/noninteractive_errors.hpp"

#include <QAction>
#include <QApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFont>
#include <QFontDatabase>
#include <QMouseEvent>
#include <QListWidget>
#include <QPainter>
#include <QPushButton>
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
#include <functional>
#include <iostream>
#include <numbers>
#include <stdexcept>
#include <utility>

namespace {
using namespace sketch;
using namespace sketch::desktop;
using Json = nlohmann::json;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void require_near(double actual, double expected, const char* message) {
    if (!std::isfinite(actual) || std::abs(actual-expected)>1e-7)
        throw std::runtime_error(std::string(message)+": expected "+std::to_string(expected)+
                                 ", observed "+std::to_string(actual));
}
std::string fresh(const char* role) {
    return std::string(role)+"-"+QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
}
Boundary rectangle(double x, double y, double width, double depth) {
    return {{{x,y},{x+width,y},0},{{x+width,y},{x+width,y+depth},0},
            {{x+width,y+depth},{x,y+depth},0},{{x,y+depth},{x,y},0}};
}
struct Fixture {
    std::shared_ptr<Document> document;
    std::string component=fresh("component"), column=fresh("column"), beam=fresh("beam");
    std::string catalog=fresh("catalog"), type=fresh("type");
    std::string dimension, hosted_catalog, hosted_instance, hosted_dimension;
};
Fixture fixture(bool with_dimension=false) {
    Fixture f;
    const Json context={{"property_id","property-1"},{"building_id","building-1"},
                        {"floor_id","floor-1"},{"layer_id","layer-1"}};
    AssemblyProfile profile;profile.id="profile";profile.outer=rectangle(0,0,2,1);
    profile.elevation_m=.25;profile.height_m=1.5;
    if (with_dimension) profile.topology=AssemblyProfileTopology{
        {"source-outer",{"source-e0","source-e1","source-e2","source-e3"},
                        {"source-v0","source-v1","source-v2","source-v3"}}, {}};
    AssemblyType type;type.id=f.type;type.name="Independent structural cohort component";type.profiles={profile};
    Entity catalog{f.catalog,"assembly_model",{{"model",AssemblyModel::create({}, {type}, {}).to_json()}}};
    AssemblyInstance instance;instance.id=f.component;instance.type_id=f.type;
    instance.root_transform=AssemblyTransform{{5,1,.75},0,1};
    auto component=encode_document_assembly_instance({f.component,"assembly_instance",context}, {f.catalog,instance});
    auto column=encode_building_entity(RectangularColumn{f.column,{2,1,.4},2,2,3,0});
    auto beam=encode_building_entity(Beam{.id=f.beam,.start={2,3,2.2},.end={6,3,2.2},
                                       .up={0,0,1},.width=2,.depth=.4});
    for (auto* entity:{&column,&beam}) {
        for (auto it=context.begin();it!=context.end();++it) entity->properties[it.key()]=it.value();
        entity->properties["mark"]=entity->type=="column" ? "C-1" : "B-1";
        entity->extensions={{"structural_resize_test",{{"preserve",true}}}};
    }
    // Physical footprints: column [1,3] x [0,2], beam [2,6] x [2,4],
    // independent component [5,7] x [1,2]. Common frame [1,7] x [0,4].
    std::vector<Entity> entities{
        {"property-1","property",{{"name","Component and structural resize"}},false},
        {"building-1","building",{{"property_id","property-1"},{"name","Building"}},false},
        {"floor-1","floor",{{"building_id","building-1"},{"name","Ground"},{"elevation_m",0}},false},
        {"layer-1","layer",{{"floor_id","floor-1"},{"name","Plan"}},false},
        catalog,component,column,beam};
    if (with_dimension) {
        f.dimension=fresh("profile-dimension");
        BoundaryDimension dimension;dimension.id=f.dimension;dimension.boundary_id=f.component;
        dimension.kind=BoundaryDimensionKind::assembly_profile;dimension.text_position={6,.5};
        AssemblyDimensionTarget target;target.component={f.catalog,f.component,f.component,{}};
        target.profile=AssemblyProfileDimensionTarget{"profile","source-outer",
            AssemblyProfileDimensionMeasurement::edge_length,"source-e0",std::nullopt,std::nullopt};
        dimension.assembly_target=target;auto carrier=encode_boundary_dimension_entity(dimension);
        for (auto it=context.begin();it!=context.end();++it) carrier.properties[it.key()]=it.value();
        entities.push_back(std::move(carrier));
    }
    auto asset=Asset::create(fresh("retained-asset"),"application/octet-stream",
                            {std::byte{0x31},std::byte{0x42},std::byte{0x53}});
    f.document=std::make_shared<Document>(Document::create(std::move(entities),{std::move(asset)}));
    return f;
}
PlanCanvas& canvas(MainWindow& window) {
    auto* result=dynamic_cast<PlanCanvas*>(window.findChild<QWidget*>("measurementPlanCanvas"));
    require(result,"actual measurement PlanCanvas must exist");return *result;
}
void display(MainWindow& window) {
    window.setAttribute(Qt::WA_DontShowOnScreen);window.resize(1400,900);
    window.setWorkspace(Workspace::measurement);window.setMetricUnits(true);window.show();
    QApplication::processEvents();
    auto* snap=window.findChild<QToolButton*>("snapTool");
    require(snap,"actual window Snap toggle must exist");snap->setChecked(false);
    auto& drawing=canvas(window);drawing.setOverviewMapEnabled(false);drawing.setGridEnabled(false);
    drawing.setSnapEnabled(false);drawing.setWallSnapEnabled(false);drawing.setViewTransform({5,2},45);
}
class ModalGuard {
public:
    ModalGuard() {
        m_timer.setInterval(10);QObject::connect(&m_timer,&QTimer::timeout,[this] {
            if (auto* dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget())) {
                m_title=dialog->windowTitle();if (m_title.isEmpty()) m_title="unnamed dialog";
                dialog->reject();
            }
        });m_timer.start();
    }
    void verify() const {
        if (!m_title.isEmpty()) throw std::runtime_error("Unexpected structural resize modal: "+m_title.toStdString());
    }
private:
    QTimer m_timer;QString m_title;
};
void select(MainWindow& window, const Fixture& f, bool reverse) {
    QStringList ids{QString::fromStdString(f.component),QString::fromStdString(f.column),QString::fromStdString(f.beam)};
    if (reverse) std::reverse(ids.begin(),ids.end());
    for (int i=0;i<ids.size();++i)
        require(window.selectEntity(ids[i],i!=0),"each independent structural cohort owner must be selectable");
    require(window.selectedEntityIds()==ids,"native selection retains all three owners in the requested order");
    QApplication::processEvents();
}
void frame(const CanvasSelectionFrame& actual, bool resized, bool depth) {
    require_near(actual.center.x,resized && !depth ? 5.5 : 4,"common structural frame centre X");
    require_near(actual.center.y,resized && depth ? 3 : 2,"common structural frame centre Y");
    require_near(actual.width_metres,resized && !depth ? 9 : 6,"common structural frame width");
    require_near(actual.depth_metres,resized && depth ? 6 : 4,"common structural frame depth");
    require_near(actual.rotation_radians,0,"common structural frame uses horizontal plan axes");
    require_near(actual.center.x-actual.width_metres/2,1,"common opposite physical X edge stays anchored");
    require_near(actual.center.y-actual.depth_metres/2,0,"common opposite physical Y edge stays anchored");
}
void require_mixed_frame(MainWindow& window, const Fixture& f, bool resized=false, bool depth=false) {
    auto& drawing=canvas(window);
    for (const auto& id:{f.component,f.column,f.beam}) {
        const auto found=std::find_if(drawing.entities().begin(),drawing.entities().end(),[&](const auto& item) {
            return item.id==QString::fromStdString(id) && item.selected && !item.segments.empty();
        });
        require(found!=drawing.entities().end(),"fixture publishes every selected physical structural owner before frame qualification");
    }
    const auto analytical=drawing.selectionAxisResizeCohortFrame();
    require(analytical.has_value(),
        "MISSING STRUCTURAL MIXED SIDE FRAME: independent component + canonical rectangular column + horizontal straight beam must expose a common four-side resize frame");
    frame(*analytical,resized,depth);
    require(drawing.selectionAxisHandlePositions().size()==4,
        "MISSING STRUCTURAL MIXED SIDE GRIPS: component + column + beam must offer four actual painted side grips");
}
void mouse(PlanCanvas& drawing, QEvent::Type kind, QPointF pixel) {
    QMouseEvent event(kind,pixel,drawing.mapToGlobal(pixel.toPoint()),
        kind==QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
        kind==QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton,Qt::ShiftModifier);
    QApplication::sendEvent(&drawing,&event);
}
void wait(MainWindow& window, const ModalGuard& guard, const std::function<bool()>& predicate, const char* message) {
    QElapsedTimer deadline;deadline.start();
    while (!predicate() && deadline.elapsed()<10000) {
        QApplication::processEvents(QEventLoop::AllEvents,20);guard.verify();
    }
    guard.verify();
    if (!predicate()) throw std::runtime_error(std::string(message)+": "+window.lastError().toStdString());
}
QImage output(PlanCanvas& drawing) {
    QImage image(drawing.size(),QImage::Format_ARGB32_Premultiplied);QPainter painter(&image);
    drawing.renderSceneAt(painter,image.rect(),drawing.viewScale(),drawing.viewCenter(),Qt::white);
    painter.end();return image;
}
void capture(PlanCanvas& drawing, const QString& name) {
    const auto directory=qEnvironmentVariable("VERTEX_TEST_CAPTURE_DIR");if (directory.isEmpty()) return;
    require(QDir().mkpath(directory) && drawing.grab().save(QDir(directory).filePath(name)),
            "optional structural resize native capture must save");
}
void unchanged(MainWindow& window, const DocumentSnapshot& before) {
    const auto after=window.document().snapshot();
    require(document_snapshot_digest(after)==document_snapshot_digest(before) &&
            after.entities()==before.entities() && after.assets()==before.assets(),
            "detached structural preview preserves exact source, revision, retained history and assets before release");
}
// These are independent handwritten physical expectations for Gx(x,y) =
// (1 + 1.5*(x-1), y) and Gy(x,y) = (x, 1.5*y). No resize builder supplies them.
std::array<Boundary,3> expected_plans(bool resized, bool depth) {
    if (!resized) return {rectangle(5,1,2,1),rectangle(1,0,2,2),rectangle(2,2,4,2)};
    if (depth) return {rectangle(5,1.5,2,1.5),rectangle(1,0,2,3),rectangle(2,3,4,3)};
    return {rectangle(7,1,3,1),rectangle(1,0,3,2),rectangle(2.5,2,6,2)};
}
// Verify the complete exact straight boundary, accepting reversed edge order
// or orientation from the solid projector. Bounds alone miss internal errors.
void exact_plan(const std::vector<CanvasEntity>& scene, const std::string& id, const Boundary& expected) {
    Boundary actual;
    for (const auto& item:scene) if (item.id==QString::fromStdString(id)) {
        require(item.holes.empty(),"solid structural fixture has no fabricated preview holes");
        actual.insert(actual.end(),item.segments.begin(),item.segments.end());
    }
    require(actual.size()==expected.size(),"exact structural candidate retains precisely four physical boundary edges");
    const auto same=[](Vec2 a,Vec2 b) {return std::hypot(a.x-b.x,a.y-b.y)<1e-7;};
    for (const auto& edge:expected) {
        const auto found=std::find_if(actual.begin(),actual.end(),[&](const auto& candidate) {
            return std::abs(candidate.sweep_radians)<1e-7 &&
                ((same(candidate.start,edge.start) && same(candidate.end,edge.end)) ||
                 (same(candidate.start,edge.end) && same(candidate.end,edge.start)));
        });
        require(found!=actual.end(),"exact structural candidate contains each independently expected physical edge");
        actual.erase(found);
    }
}
void plans(const std::vector<CanvasEntity>& scene, const Fixture& f, bool resized, bool depth) {
    const auto expected=expected_plans(resized,depth);
    exact_plan(scene,f.component,expected[0]);exact_plan(scene,f.column,expected[1]);exact_plan(scene,f.beam,expected[2]);
}
Json physical_structure(const DocumentSnapshot& snapshot, const Fixture& f, bool resized, bool depth) {
    AssemblyExpansionBudget budget;
    const auto expanded=expand_document_assembly_instance(snapshot.entities().at(f.component),snapshot.entities(),budget);
    require(expanded.profiles.size()==1,"independent component retains its one complete profile");
    const auto& profile=expanded.profiles.front();const auto& pose=profile.transform;
    require(profile.profile.outer.size()==4 && profile.profile.holes.empty(),"component retains four-edge rectangular topology");
    const auto expected=expected_plans(resized,depth);auto vertices=Json::array();
    for (std::size_t i=0;i<4;++i) {
        auto p=profile.profile.outer[i].start;
        if (profile.profile.planar_affine) {
            const auto& a=*profile.profile.planar_affine;const double x=p.x-a.source_origin.x,y=p.y-a.source_origin.y;
            p={a.target_origin.x+a.xx*x+a.xy*y,a.target_origin.y+a.yx*x+a.yy*y};
        }
        const double py=pose.mirrored_y ? -p.y : p.y;
        const double x=pose.translation_m.x+pose.scale*(p.x*std::cos(pose.rotation_radians)-py*std::sin(pose.rotation_radians));
        const double y=pose.translation_m.y+pose.scale*(p.x*std::sin(pose.rotation_radians)+py*std::cos(pose.rotation_radians));
        require_near(x,expected[0][i].start.x,"component physical X follows common anchored unequal factors");
        require_near(y,expected[0][i].start.y,"component physical Y follows common anchored unequal factors");
        require_near(profile.profile.outer[i].sweep_radians,0,"straight component edges stay straight");vertices.push_back({x,y});
    }
    require_near(pose.translation_m.z+pose.scale*pose.vertical_scale*profile.profile.elevation_m,1,"component physical base Z preserved");
    require_near(pose.scale*pose.vertical_scale*profile.profile.height_m,1.5,"component physical height preserved");
    const auto instance=decode_document_assembly_instance(snapshot.entities().at(f.component));
    require(instance.instance.root_transform && !instance.instance.placement,"component remains an independent root");
    require_near(instance.instance.root_transform->translation_m.z,.75,"component root Z preserved");
    const auto& ce=snapshot.entities().at(f.column);const auto& be=snapshot.entities().at(f.beam);
    require(ce.properties.at("version")==1 && ce.properties.at("form")=="rectangular_column" &&
            be.properties.at("version")==1 && be.properties.at("form")=="straight_beam","structural owners retain canonical v1 schemas");
    const auto column=std::get<RectangularColumn>(decode_building_entity(ce));
    const auto beam=std::get<Beam>(decode_building_entity(be));
    require_near(column.base_center.x,resized && !depth ? 2.5 : 2,"column centre X uses shared anchor rather than its own centre");
    require_near(column.base_center.y,resized && depth ? 1.5 : 1,"column centre Y uses shared anchor");
    require_near(column.width,resized && !depth ? 3 : 2,"column plan width");require_near(column.depth,resized && depth ? 3 : 2,"column plan depth");
    require_near(column.base_center.z,.4,"column base Z preserved");require_near(column.height,3,"column height preserved");require_near(column.rotation_radians,0,"column orientation preserved");
    require_near(beam.start.x,resized && !depth ? 2.5 : 2,"beam start X follows common anchor");
    require_near(beam.end.x,resized && !depth ? 8.5 : 6,"beam end X follows common anchor");
    require_near(beam.start.y,resized && depth ? 4.5 : 3,"beam start Y follows common anchor");
    require_near(beam.end.y,resized && depth ? 4.5 : 3,"beam end Y follows common anchor");
    require_near(beam.start.z,2.2,"beam start Z preserved");require_near(beam.end.z,2.2,"beam end Z preserved");
    require_near(beam.width,resized && depth ? 3 : 2,"beam transverse plan width uses the depth-axis factor");
    require_near(beam.depth,.4,"beam vertical section height preserved");require_near(beam.up.x,0,"beam up X preserved");
    require_near(beam.up.y,0,"beam up Y preserved");require_near(beam.up.z,1,"beam up Z preserved");
    require(!make_building_shape(column).IsNull() && !make_building_shape(beam).IsNull(),"both canonical structural owners generate real native solids");
    for (const auto& id:{f.component,f.column,f.beam}) {
        const auto& p=snapshot.entities().at(id).properties;
        require(p.at("property_id")=="property-1" && p.at("building_id")=="building-1" &&
                p.at("floor_id")=="floor-1" && p.at("layer_id")=="layer-1","physical owners retain complete drawing contexts");
    }
    return {{"component",vertices},{"column",{column.base_center.x,column.base_center.y,column.base_center.z,column.width,column.depth,column.height}},
            {"beam",{beam.start.x,beam.start.y,beam.start.z,beam.end.x,beam.end.y,beam.end.z,beam.width,beam.depth}}};
}
void preserved(const DocumentSnapshot& before, const DocumentSnapshot& after, const Fixture& f, const std::string& retired_dimension={}) {
    require(before.assets()==after.assets() && !after.assets().empty(),"mixed structural resize retains actual nonempty assets exactly");
    require(before.entities().size()==after.entities().size()+(retired_dimension.empty() ? 0 : 1),"ordinary structural resize changes only requested owners and explicitly retired carrier");
    for (const auto& [id,entity]:before.entities()) {
        if (id==retired_dimension) {require(!after.entities().contains(id),"explicitly retired dimension is absent");continue;}
        if (id!=f.component && id!=f.column && id!=f.beam && id!=f.catalog)
            require(after.entities().at(id)==entity,"unselected context entities remain exact");
        else require(after.entities().at(id).extensions==entity.extensions,"selected owners retain opaque extension data");
    }
    const auto original=AssemblyModel::from_json(before.entities().at(f.catalog).properties.at("model"));
    const auto resulting=AssemblyModel::from_json(after.entities().at(f.catalog).properties.at("model"));
    for (const auto& type:original.types()) {
        const auto found=std::find_if(resulting.types().begin(),resulting.types().end(),[&](const auto& item){return item.id==type.id;});
        require(found!=resulting.types().end() && *found==type,"private component edits preserve the original catalog type definition");
    }
    for (const auto& id:{f.column,f.beam})
        require(before.entities().at(id).properties.at("mark")==after.entities().at(id).properties.at("mark"),"structural marks remain exact");
}
void same_structure(const Json& a, const Json& b) {
    if (a.is_number() && b.is_number()) {require_near(a.get<double>(),b.get<double>(),"selection order/reopen preserves physical structure");return;}
    require(a.type()==b.type() && a.size()==b.size(),"selection order/reopen preserves semantic structure");
    if (a.is_object()) for (auto it=a.begin();it!=a.end();++it) same_structure(it.value(),b.at(it.key()));
    else if (a.is_array()) for (std::size_t i=0;i<a.size();++i) same_structure(a.at(i),b.at(i));
    else require(a==b,"selection order/reopen preserves semantic values");
}
void wait_until(MainWindow& window, const std::function<bool()>& predicate, const char* message) {
    QElapsedTimer deadline;deadline.start();
    while (!predicate() && deadline.elapsed()<10000) QApplication::processEvents(QEventLoop::AllEvents,20);
    if (!predicate()) throw std::runtime_error(std::string(message)+": "+window.lastError().toStdString());
}
void settle_without_publication(MainWindow& window, const DocumentSnapshot& before) {
    QElapsedTimer deadline;deadline.start();
    do {
        QApplication::processEvents(QEventLoop::AllEvents,20);unchanged(window,before);
        require(!canvas(window).entityTransformPreviewPending() && canvas(window).entityTransformPreview().empty(),
                "refused structural resize cannot resurrect a late preview or publication");
    } while (deadline.elapsed()<350);
}
QPointF completed_preview(MainWindow& window, const Fixture& f, const DocumentSnapshot& before) {
    auto& drawing=canvas(window);const auto handles=drawing.selectionAxisHandlePositions();
    require(handles.size()==4,"lifecycle drag starts from four actual structural mixed side grips");
    const auto press=*std::max_element(handles.begin(),handles.end(),[](QPointF a,QPointF b){return a.x()<b.x();});
    const auto release=press+QPointF(3*drawing.viewScale(),0);
    require(drawing.rect().contains(release.toPoint()),"structural lifecycle drag stays inside the canvas");
    mouse(drawing,QEvent::MouseButtonPress,press);mouse(drawing,QEvent::MouseMove,release);
    require(drawing.entityTransformPreviewPending(),"structural lifecycle preview uses actual asynchronous production");
    unchanged(window,before);
    wait_until(window,[&]{return !drawing.entityTransformPreviewPending() && !drawing.entityTransformPreview().empty();},
               "structural lifecycle candidate completes before review or authority replacement");
    plans(drawing.entityTransformPreview(),f,true,false);plans(drawing.entities(),f,false,false);
    unchanged(window,before);return release;
}
void mixed41_command(const DocumentSnapshot& after) {
    const auto& command=after.history().back().boundary_constraint_changes;
    require(command && command->mixed_selection_edit_completion && command->mixed_selection_edit_intent.at("version")==41,
            "native structural resize publishes sealed mixed41 authority");
    require(command_to_json(*command).at("version")==52,"native structural resize retains enclosing command52");
}
void fresh_open(MainWindow& window, const QString& directory, const QString& stem,
                const std::function<void(MainWindow&,const DocumentSnapshot&)>& inspect) {
    const auto path=QDir(directory).filePath(stem+".bldproj");
    const auto before_save=window.document().snapshot();
    require(window.saveProjectAs(path),"structural lifecycle project saves");
    const auto saved=window.document().snapshot();
    require(saved.entities()==before_save.entities() && saved.assets()==before_save.assets() &&
            saved.history().size()==before_save.history().size(),"save retains immediate pre-save entities, assets and history");
    require(window.createNewProject(),"release writer lease before structural lifecycle fresh Open");
    MainWindow reopened({},nullptr,QDir(directory).filePath(stem+"-reopened-library.json"));display(reopened);
    require(reopened.openProject(path) && reopened.document().is_editable(),"structural lifecycle fresh Open acquires editable writer lease");
    const auto loaded=reopened.document().snapshot();
    require(document_snapshot_digest(loaded)==document_snapshot_digest(saved),"fresh Open retains complete structural snapshot and history");
    auto* undo=reopened.findChild<QAction*>("undoCommand");
    require(undo && !undo->isEnabled() && !reopened.undoCommand(),"fresh Open starts a new Undo session despite retained history");
    unchanged(reopened,loaded);inspect(reopened,loaded);
}
Fixture site_fixture() {
    auto f=fixture();const auto source=f.document->snapshot();
    auto property=source.entities().at("property-1");
    property.properties["site_frame"]={{"version",1},{"origin_m",{100,200,30}},
        {"rotation_radians",std::numbers::pi/2},
        {"vertical_datum",{{"identifier","structural-fixture-datum"},{"height_at_origin_m",1500}}}};
    auto building=source.entities().at("building-1");
    building.properties["site_placement"]={{"version",1},{"translation_m",{10,20,3}},{"rotation_radians",0}};
    std::vector<EntityChange> changes{EntityChange::upsert(std::move(property)),EntityChange::upsert(std::move(building))};
    for (const auto& id:{f.component,f.column,f.beam}) {
        auto owner=source.entities().at(id);
        owner.properties["presentation_frame"]={{"version",1},{"mode","building"}};
        changes.push_back(EntityChange::upsert(std::move(owner)));
    }
    f.document->apply(ApplyEntityChanges{source.revision(),std::move(changes),{},"Enroll all three structural mixed owners in Site"});
    return f;
}
PlanCanvas& site_canvas(MainWindow& window) {
    auto* drawing=dynamic_cast<PlanCanvas*>(window.findChild<QWidget*>("architecturalPlanCanvas"));
    require(drawing,"actual architectural Site canvas must exist");return *drawing;
}
void display_site(MainWindow& window) {
    window.setAttribute(Qt::WA_DontShowOnScreen);window.resize(1400,900);window.setMetricUnits(true);window.show();
    require(window.setActiveLayer("layer-1"),"structural Site activates its actual source layer");
    window.setWorkspace(Workspace::architectural);
    auto* action=window.findChild<QAction*>("sitePlan");require(action,"actual Site Plan action must exist");
    if (!action->isChecked()) action->trigger();
    QApplication::processEvents();
    require(action->isChecked() && window.workspace()==Workspace::architectural && window.activeLayerId()=="layer-1",
            "structural Site uses the actual checked action, architectural workspace and source layer");
    auto* snap=window.findChild<QToolButton*>("snapTool");
    require(snap,"actual Site Snap toggle must exist");snap->setChecked(false);
    auto& drawing=site_canvas(window);drawing.setTool(CanvasTool::select);
    drawing.setOverviewMapEnabled(false);drawing.setGridEnabled(false);drawing.setSnapEnabled(false);drawing.setWallSnapEnabled(false);
    drawing.setViewTransform({78,214},45);
}
void site_frame(const CanvasSelectionFrame& actual,bool resized) {
    require_near(actual.center.x,78,"structural Site common frame centre X");
    require_near(actual.center.y,resized ? 215.5 : 214,"structural Site common frame centre Y");
    require_near(actual.width_metres,4,"structural Site common frame width");
    require_near(actual.depth_metres,resized ? 9 : 6,"structural Site common depth follows authored X resize");
    require_near(actual.rotation_radians,0,"structural Site cohort uses common canvas axes");
    require_near(actual.center.x-actual.width_metres/2,76,"structural Site physical left edge remains exact");
    require_near(actual.center.x+actual.width_metres/2,80,"structural Site physical right edge remains exact");
    require_near(actual.center.y-actual.depth_metres/2,211,"structural Site opposite physical Y edge stays anchored");
    require_near(actual.center.y+actual.depth_metres/2,resized ? 220 : 217,"structural Site physical positive Y edge follows authored X");
}
void site_plans(const std::vector<CanvasEntity>& scene,const Fixture& f,bool resized) {
    // Handwritten Gx expectations above, composed once with the independent
    // property/building pose F(x,y) = (80-y,210+x). No production frame or
    // resize builder contributes to these complete physical edge expectations.
    auto expected=expected_plans(resized,false);
    const auto presented=[](Vec2 p) {return Vec2{80-p.y,210+p.x};};
    for (auto& boundary:expected) for (auto& edge:boundary) {
        edge.start=presented(edge.start);edge.end=presented(edge.end);
    }
    exact_plan(scene,f.component,expected[0]);exact_plan(scene,f.column,expected[1]);exact_plan(scene,f.beam,expected[2]);
}
void require_site_frame(MainWindow& window,const Fixture& f,bool resized) {
    auto& drawing=site_canvas(window);site_plans(drawing.entities(),f,resized);
    for (const auto& id:{f.component,f.column,f.beam}) {
        const auto found=std::find_if(drawing.entities().begin(),drawing.entities().end(),[&](const auto& item) {
            return item.id==QString::fromStdString(id) && item.selected && !item.segments.empty();
        });
        require(found!=drawing.entities().end(),"Site publishes every selected physical structural owner");
    }
    const auto analytical=drawing.selectionAxisResizeCohortFrame();
    require(analytical.has_value(),"actual Site component, column and beam expose a common side resize frame");
    site_frame(*analytical,resized);
    require(drawing.selectionAxisHandlePositions().size()==4,"actual Site structural cohort paints four side grips");
}
Json site_scenario(const QString& directory,bool reverse) {
    const auto f=site_fixture();const auto stem=QString(reverse ? "site-beam-first" : "site-component-first");
    MainWindow window(f.document,nullptr,QDir(directory).filePath(stem+"-library.json"));ModalGuard guard;display_site(window);
    select(window,f,reverse);require_site_frame(window,f,false);guard.verify();
    auto& drawing=site_canvas(window);const auto before=window.document().snapshot();
    physical_structure(before,f,false,false);const auto committed=output(drawing);capture(drawing,stem+"-selected.png");
    const auto handles=drawing.selectionAxisHandlePositions();
    const auto press=*std::min_element(handles.begin(),handles.end(),[](QPointF a,QPointF b){return a.y()<b.y();});
    const auto release=press+QPointF(0,-3*drawing.viewScale());
    require(drawing.rect().contains(release.toPoint()),"authored X Site drag stays inside the actual architectural canvas");
    mouse(drawing,QEvent::MouseButtonPress,press);unchanged(window,before);
    mouse(drawing,QEvent::MouseMove,release);
    require(drawing.entityTransformPreviewPending(),"Site structural preview uses the actual asynchronous native producer");
    unchanged(window,before);
    wait(window,guard,[&]{return !drawing.entityTransformPreviewPending() && !drawing.entityTransformPreview().empty();},
         "Site exact three-owner preview completes");
    require(drawing.entityTransformPreview().size()==3,"Site exact detached preview contains precisely all three selected owners");
    site_plans(drawing.entityTransformPreview(),f,true);site_plans(drawing.entities(),f,false);unchanged(window,before);
    const auto captured=drawing.selectionAxisResizeCohortFrame();
    require(captured.has_value(),"Site drag retains its captured analytical frame");site_frame(*captured,false);
    const auto preview_handles=drawing.selectionAxisHandlePositions();
    require(preview_handles.size()==4,"Site preview retains all four painted side grips");
    const auto lowest=[](const std::vector<QPointF>& points) {
        return std::max_element(points.begin(),points.end(),[](QPointF a,QPointF b){return a.y()<b.y();})->y();
    };
    const auto highest=[](const std::vector<QPointF>& points) {
        return std::min_element(points.begin(),points.end(),[](QPointF a,QPointF b){return a.y()<b.y();})->y();
    };
    require_near(lowest(preview_handles),lowest(handles),"Site painted preview retains the opposite side anchor");
    // Marks affect annotation footprints; exact physical edges above supply
    // the displacement oracle while the painted positive grip moves outward.
    require(highest(preview_handles)<highest(handles),"Site painted positive grip moves outward");
    require(output(drawing)==committed,"Site detached exact preview stays outside committed print/export output");
    capture(drawing,stem+"-preview.png");mouse(drawing,QEvent::MouseButtonRelease,release);
    wait(window,guard,[&]{return window.document().revision()==before.revision()+1 && !drawing.entityTransformPreviewPending();},
         "Site release publishes all three physical owners in exactly one revision");
    const auto after=window.document().snapshot();
    require(after.history().size()==before.history().size()+1,"Site three-owner resize appends exactly one retained command");
    mixed41_command(after);
    require(drawing.entityTransformPreview().empty(),"Site release retires the exact detached preview");
    const auto result=physical_structure(after,f,true,false);preserved(before,after,f);require_site_frame(window,f,true);
    for (const auto& id:{f.component,f.column,f.beam})
        require(after.entities().at(id).properties.at("presentation_frame")==before.entities().at(id).properties.at("presentation_frame"),
                "Site resize retains every explicitly enrolled building presentation frame");
    require(output(drawing)!=committed,"Site release publishes resized physical geometry to committed output");
    capture(drawing,stem+"-committed.png");
    require(window.undoCommand() && window.document().snapshot().entities()==before.entities() && window.document().snapshot().assets()==before.assets(),
            "one Site Undo restores all three exact source owners and assets");
    require_site_frame(window,f,false);
    require(window.redoCommand() && window.document().snapshot().entities()==after.entities() && window.document().snapshot().assets()==after.assets(),
            "one Site Redo restores all three exact resized owners and assets");
    require_site_frame(window,f,true);
    fresh_open(window,directory,stem,[&](MainWindow& reopened,const DocumentSnapshot& loaded) {
        same_structure(result,physical_structure(loaded,f,true,false));preserved(before,loaded,f);
        display_site(reopened);select(reopened,f,reverse);require_site_frame(reopened,f,true);
        require(site_canvas(reopened).entityTransformPreview().empty() && !site_canvas(reopened).entityTransformPreviewPending(),
                "fresh Site Open restores controls without a retired preview");
        capture(site_canvas(reopened),stem+"-reopened.png");
    });guard.verify();return result;
}
class DimensionReviewDriver {
public:
    DimensionReviewDriver(MainWindow& window,const Fixture& f,const DocumentSnapshot& before,bool retire)
        : m_window(window),m_fixture(f),m_before(before),m_retire(retire) {
        m_timer.setInterval(10);QObject::connect(&m_timer,&QTimer::timeout,[this] {
            auto* dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget());if (!dialog) return;
            try {
                require(dialog->objectName()==QStringLiteral("AssemblyProfileDimensionReviewDialog") ||
                        dialog->windowTitle()==QStringLiteral("Review component dimensions"),
                        "structural mixed release opens the actual component profile dimension review");
                require(++m_reviews==1,"one affected profile carrier opens exactly one review");unchanged(m_window,m_before);
                auto* list=dialog->findChild<QListWidget*>();
                require(list && list->count()==1 && list->item(0)->toolTip()==QString::fromStdString(m_fixture.dimension),
                        "review contains the actual original-topology dimension carrier");
                auto* buttons=dialog->findChild<QDialogButtonBox*>();
                require(buttons && buttons->button(QDialogButtonBox::Cancel) && buttons->button(QDialogButtonBox::Ok),
                        "review exposes actual Cancel and Apply controls");
                require(!buttons->button(QDialogButtonBox::Ok)->isEnabled(),"publication requires an explicit profile dimension choice");
                if (!m_retire) {
                    buttons->button(QDialogButtonBox::Cancel)->click();
                    require(dialog->result()==QDialog::Rejected && !dialog->isVisible(),"actual Cancel rejects and closes review");
                } else {
                    auto* remove=dialog->findChild<QPushButton*>("dimensionRemove");
                    if (!remove) for (auto* button:dialog->findChildren<QPushButton*>())
                        if (button->text()==QStringLiteral("Remove dimension")) {remove=button;break;}
                    require(remove && remove->isEnabled(),"review exposes enabled explicit Remove dimension choice");
                    remove->click();unchanged(m_window,m_before);
                    require(buttons->button(QDialogButtonBox::Ok)->isEnabled(),"explicit removal enables Apply");
                    buttons->button(QDialogButtonBox::Ok)->click();
                    require(dialog->result()==QDialog::Accepted && !dialog->isVisible(),"actual Apply accepts the reviewed choice");
                }
            } catch (const std::exception& error) {if (m_failure.empty()) m_failure=error.what();dialog->reject();}
              catch (...) {if (m_failure.empty()) m_failure="unknown dimension review failure";dialog->reject();}
        });m_timer.start();
    }
    void verify() const {
        if (!m_failure.empty()) throw std::runtime_error(m_failure);
        require(m_reviews==1,"structural mixed release actually enters its profile dimension review");
    }
private:
    MainWindow& m_window;const Fixture& m_fixture;const DocumentSnapshot& m_before;bool m_retire;
    QTimer m_timer;int m_reviews=0;std::string m_failure;
};
void dimension_review(const QString& directory, bool retire) {
    const auto f=fixture(true);const auto stem=retire ? QStringLiteral("dimension-remove") : QStringLiteral("dimension-cancel");
    MainWindow window(f.document,nullptr,QDir(directory).filePath(stem+"-library.json"));display(window);
    select(window,f,false);require_mixed_frame(window,f);const auto before=window.document().snapshot();
    const auto decoded=decode_boundary_dimension_entity(before.entities().at(f.dimension));
    require(decoded.supported() && decoded.dimension->kind==BoundaryDimensionKind::assembly_profile,
            "review fixture contains a supported actual component profile dimension");
    require_near(decoded.dimension->resolve(before).segment_length(),2,"identified original profile edge resolves to two metres");
    physical_structure(before,f,false,false);DimensionReviewDriver review(window,f,before,retire);
    auto& drawing=canvas(window);const auto committed=output(drawing);const auto release=completed_preview(window,f,before);
    require(output(drawing)==committed,"reviewable structural preview stays outside committed output");
    capture(drawing,stem+"-preview.png");mouse(drawing,QEvent::MouseButtonRelease,release);review.verify();
    if (!retire) {
        wait_until(window,[&]{return !drawing.entityTransformPreviewPending() && drawing.entityTransformPreview().empty();},
                   "Cancel clears the entire provisional structural cohort");
        settle_without_publication(window,before);review.verify();
        require(output(drawing)==committed,"review Cancel retains exact committed output");physical_structure(window.document().snapshot(),f,false,false);
        require_near(decoded.dimension->resolve(window.document().snapshot()).segment_length(),2,"Cancel retains the original live profile dimension");
    } else {
        require(window.focusWidget()==&drawing,"accepted review restores captured canvas focus");QApplication::setActiveWindow(&window);
        wait_until(window,[&]{return window.document().revision()==before.revision()+1 && !drawing.entityTransformPreviewPending();},
                   "reviewed structural resize publishes all three owners in one command");
        review.verify();const auto after=window.document().snapshot();mixed41_command(after);
        require(after.history().size()==before.history().size()+1 && !after.entities().contains(f.dimension) &&
                after.entities().size()+1==before.entities().size(),"one reviewed command removes exactly the affected carrier");
        physical_structure(after,f,true,false);plans(drawing.entities(),f,true,false);
        preserved(before,after,f,f.dimension);
        require(output(drawing)!=committed,"reviewed Apply publishes resized structural geometry to committed output");
        require(window.undoCommand() && window.document().snapshot().entities()==before.entities() && window.document().snapshot().assets()==before.assets(),
                "one Undo restores all original owners and the retired profile dimension");
        require(window.redoCommand() && window.document().snapshot().entities()==after.entities() && window.document().snapshot().assets()==after.assets(),
                "one Redo restores all resized owners and the explicit dimension choice");
        fresh_open(window,directory,stem,[&](MainWindow& reopened,const DocumentSnapshot& loaded) {
            require(!loaded.entities().contains(f.dimension),"fresh Open retains explicit profile dimension removal");
            physical_structure(loaded,f,true,false);preserved(before,loaded,f,f.dimension);
            select(reopened,f,false);require_mixed_frame(reopened,f,true,false);
        });
    }
}
enum class StaleAuthority { viewport, selection, history };
void stale_release(const QString& directory, StaleAuthority authority) {
    const auto f=fixture();const auto stem=authority==StaleAuthority::viewport ? QStringLiteral("stale-viewport") :
        authority==StaleAuthority::selection ? QStringLiteral("stale-selection") : QStringLiteral("stale-history");
    // Create a real retained event to replace at the same displayed revision.
    if (authority==StaleAuthority::history) {
        const auto source=f.document->snapshot();
        auto retained=source.entities().at(f.component);retained.extensions["retained_history_fixture"]=true;
        f.document->apply(ApplyEntityChanges{source.revision(),{EntityChange::upsert(std::move(retained))},{},"Original structural history authority"});
    }
    MainWindow window(f.document,nullptr,QDir(directory).filePath(stem+"-library.json"));ModalGuard guard;display(window);
    select(window,f,false);require_mixed_frame(window,f);auto& drawing=canvas(window);const auto before=window.document().snapshot();
    const auto committed=output(drawing);const auto center=drawing.viewCenter();const auto scale=drawing.viewScale();
    const auto release=completed_preview(window,f,before);require(output(drawing)==committed,"stale candidate stays outside committed output");
    auto expected=before;
    if (authority==StaleAuthority::viewport) {
        drawing.zoomBy(1.2);require(drawing.viewScale()!=scale,"stale viewport actually changes captured view scale");
    } else if (authority==StaleAuthority::selection) {
        require(window.selectEntity(QString::fromStdString(f.component)) &&
                window.selectedEntityIds()==QStringList{QString::fromStdString(f.component)},"stale selection removes both structural owners from actual cohort");
    } else {
        require(before.revision()>0,"history replacement fixture has a retained source revision");
        auto replacement=Document::fork_at_revision(before,before.revision()-1);std::vector<EntityChange> changes;
        for (const auto& [id,entity]:before.entities()) { (void)id;changes.push_back(EntityChange::upsert(entity)); }
        replacement.apply(ApplyEntityChanges{replacement.revision(),std::move(changes),{},"Independently replaced structural history authority"});
        expected=replacement.snapshot();
        require(expected.document_id()==before.document_id() && expected.revision()==before.revision() &&
                expected.entities()==before.entities() && expected.assets()==before.assets(),"history-only source replacement retains exact displayed identity, revision, entities and assets");
        require(document_snapshot_digest(expected)!=document_snapshot_digest(before),"history-only replacement changes full source digest");
        window.document()=std::move(replacement);
    }
    QApplication::processEvents();unchanged(window,expected);mouse(drawing,QEvent::MouseButtonRelease,release);
    wait(window,guard,[&]{return !drawing.entityTransformPreviewPending() && drawing.entityTransformPreview().empty();},
         "stale structural release discards its complete candidate");
    settle_without_publication(window,expected);guard.verify();physical_structure(window.document().snapshot(),f,false,false);
    drawing.setViewTransform(center,scale);select(window,f,false);
    require(output(drawing)==committed,"stale structural refusal retains exact original committed output at original view");require_mixed_frame(window,f);
}
Fixture phase_fixture() {
    auto f=fixture();f.dimension=fresh("extent-dimension");f.hosted_catalog=fresh("hosted-catalog");
    f.hosted_instance=fresh("hosted-column-component");f.hosted_dimension=fresh("hosted-extent-dimension");
    const auto source=f.document->snapshot();std::vector<Entity> entities;
    for (const auto& [id,entity]:source.entities()) { (void)id;entities.push_back(entity); }
    const auto& context=source.entities().at(f.component).properties;
    const auto extent=[&](const std::string& id,const std::string& owner,const AssemblyInstancePropertyEditTarget& identity) {
        BoundaryDimension dimension;dimension.id=id;dimension.boundary_id=owner;
        dimension.kind=BoundaryDimensionKind::assembly_extent;dimension.text_position={6,.5};
        dimension.presentation=BoundaryDimensionPresentation{3.2,"#713ba2",true,false,true,.31};
        AssemblyDimensionTarget target;target.component=identity;target.axis=AssemblyDimensionAxis::width;
        dimension.assembly_target=target;auto carrier=encode_boundary_dimension_entity(dimension);
        for (const auto* key:{"property_id","building_id","floor_id","layer_id"}) carrier.properties[key]=context.at(key);
        carrier.extensions={{"structural_dimension_test",{{"preserve",true}}}};return carrier;
    };
    entities.push_back(extent(f.dimension,f.component,{f.catalog,f.component,f.component,{}}));
    AssemblyProfile profile;profile.id="hosted-profile";profile.outer=rectangle(0,0,.5,.25);
    profile.height_m=.5;AssemblyType type;type.id="hosted-type";type.name="Column hosted source profile";type.profiles={profile};
    type.properties={{"opaque","declared hosted default"}};
    AssemblyInstance instance;instance.id=f.hosted_instance;instance.type_id=type.id;
    instance.placement=AssemblyPlacement{f.column,{2,1},0,1,false,.4,1};
    instance.property_overrides={{"opaque","retained hosted source"}};
    Entity catalog{f.hosted_catalog,"assembly_model",{{"model",AssemblyModel::create({}, {type}, {instance}).to_json()}}};
    for (const auto* key:{"property_id","building_id","floor_id","layer_id"}) catalog.properties[key]=context.at(key);
    catalog.extensions={{"structural_hosted_test",{{"preserve",true}}}};entities.push_back(std::move(catalog));
    entities.push_back(extent(f.hosted_dimension,f.hosted_catalog,{f.hosted_catalog,f.hosted_instance,std::nullopt,{}}));
    const std::vector<std::string> baseline{f.component,f.column,f.beam,f.dimension,f.hosted_catalog,f.hosted_dimension};
    const auto phases=ModelPhases::create(baseline,baseline,
        {RemodelingAlternative{"resize-alternative","Native structural replacement",{}, {}}},"resize-alternative");
    entities.push_back({"resize-phases","model_phases",{{"model",phases.to_json()}},false});
    std::vector<Asset> assets;for (const auto& [id,asset]:source.assets()) { (void)id;assets.push_back(asset); }
    f.document=std::make_shared<Document>(Document::create_phase_import(std::move(entities),std::move(assets)));return f;
}
void retained_catalog_types(const DocumentSnapshot& before,const DocumentSnapshot& after,const std::string& catalog) {
    const auto original=AssemblyModel::from_json(before.entities().at(catalog).properties.at("model"));
    const auto resulting=AssemblyModel::from_json(after.entities().at(catalog).properties.at("model"));
    for (const auto& type:original.types()) {
        const auto found=std::find_if(resulting.types().begin(),resulting.types().end(),[&](const auto& value){return value.id==type.id;});
        require(found!=resulting.types().end() && *found==type,"baseline replacement preserves every original component type definition");
    }
    const auto& original_rows=before.entities().at(catalog).properties.at("model").at("types");
    const auto& final_rows=after.entities().at(catalog).properties.at("model").at("types");
    for (const auto& row:original_rows) {
        const auto found=std::find_if(final_rows.begin(),final_rows.end(),[&](const auto& value){return value.at("id")==row.at("id");});
        require(found!=final_rows.end() && found->dump()==row.dump(),"baseline replacement retains every original raw catalog type row exactly");
    }
}
void phase_scenario(const QString& directory) {
    const auto f=phase_fixture();MainWindow window(f.document,nullptr,QDir(directory).filePath("phase-library.json"));ModalGuard guard;display(window);
    require(window.selectModelPhaseRegistry("resize-phases") && window.selectRemodelingAlternative("resize-alternative"),
            "structural phase scenario selects actual registry and active remodeling alternative");
    select(window,f,false);require_mixed_frame(window,f);const auto before=window.document().snapshot();
    const auto original_dimension=decode_boundary_dimension_entity(before.entities().at(f.dimension));
    const auto original_hosted_dimension=decode_boundary_dimension_entity(before.entities().at(f.hosted_dimension));
    require(original_dimension.supported() && original_hosted_dimension.supported(),"baseline contains two actual supported extent dimensions");
    require_near(original_dimension.dimension->resolve(before).segment_length(),2,"baseline independent extent resolves original two-metre width");
    require_near(original_hosted_dimension.dimension->resolve(before).segment_length(),.5,"baseline hosted extent resolves original half-metre width");
    auto& drawing=canvas(window);const auto committed=output(drawing);const auto release=completed_preview(window,f,before);
    require(output(drawing)==committed,"baseline structural replacement preview stays outside committed output");
    mouse(drawing,QEvent::MouseButtonRelease,release);
    wait(window,guard,[&]{return window.document().revision()==before.revision()+1 && !drawing.entityTransformPreviewPending();},
         "baseline structural and component replacements publish in one command");
    guard.verify();const auto after=window.document().snapshot();mixed41_command(after);
    require(after.history().size()==before.history().size()+1 && after.assets()==before.assets(),"baseline replacement retains assets and appends exactly one history event");
    for (const auto& id:{f.component,f.column,f.beam,f.dimension,f.hosted_catalog,f.hosted_dimension}) {
        const auto& original=before.entities().at(id);const auto& retained=after.entities().at(id);
        require(retained==original && retained.properties.dump()==original.properties.dump() && retained.extensions.dump()==original.extensions.dump(),
                "baseline physical owners, hosted catalog and dimension carriers remain source exact");
    }
    retained_catalog_types(before,after,f.catalog);
    const auto model=ModelPhases::from_json(after.entities().at("resize-phases").properties.at("model"));
    require(model.baseline_ids()==ModelPhases::from_json(before.entities().at("resize-phases").properties.at("model")).baseline_ids(),
            "replacement preserves exact original baseline membership");
    const auto states=model.state(std::string{"resize-alternative"});
    for (const auto& id:{f.component,f.column,f.beam}) require(states.at(id)==ModelPhase::demolished,"active alternative retires each original physical owner");
    auto proposed=f;proposed.component.clear();proposed.column.clear();proposed.beam.clear();
    proposed.dimension.clear();proposed.hosted_catalog.clear();proposed.hosted_instance.clear();proposed.hosted_dimension.clear();
    for (const auto& [id,state]:states) if (state==ModelPhase::proposed) {
        require(!before.entities().contains(id),"proposed replacement uses fresh actual entity identities");const auto& entity=after.entities().at(id);
        if (entity.type=="assembly_instance") {require(proposed.component.empty(),"exactly one proposed independent component");proposed.component=id;}
        else if (entity.type=="column") {require(proposed.column.empty(),"exactly one proposed column");proposed.column=id;}
        else if (entity.type=="beam") {require(proposed.beam.empty(),"exactly one proposed beam");proposed.beam=id;}
        else if (entity.type=="assembly_model") {require(proposed.hosted_catalog.empty(),"exactly one proposed hosted catalog");proposed.hosted_catalog=id;}
        else if (entity.type=="dimension") {
            const auto decoded=decode_boundary_dimension_entity(entity);require(decoded.supported() && decoded.dimension->assembly_target,"proposed dimension retains typed qualified target");
            if (decoded.dimension->assembly_target->component.document_entity_id) {
                require(proposed.dimension.empty(),"exactly one proposed independent extent carrier");proposed.dimension=id;
            } else {require(proposed.hosted_dimension.empty(),"exactly one proposed hosted extent carrier");proposed.hosted_dimension=id;}
        }
    }
    require(!proposed.component.empty() && !proposed.column.empty() && !proposed.beam.empty() && !proposed.dimension.empty() &&
            !proposed.hosted_catalog.empty() && !proposed.hosted_dimension.empty(),"proposed roster contains all three owners, hosted catalog and both carried dimensions");
    const auto hosted_model=AssemblyModel::from_json(after.entities().at(proposed.hosted_catalog).properties.at("model"));
    require(hosted_model.instances().size()==1,"replacement copies exactly the inventoried hosted root");
    const auto& root=hosted_model.instances().front();proposed.hosted_instance=root.id;
    require(root.id!=f.hosted_instance && root.placement && root.placement->host_entity_id==proposed.column,
            "proposed hosted root receives fresh qualified identity and binds actual proposed column");
    const auto original_hosted_model=AssemblyModel::from_json(before.entities().at(f.hosted_catalog).properties.at("model"));
    require(hosted_model.types()==original_hosted_model.types() && root.property_overrides==original_hosted_model.instances().front().property_overrides,
            "copied structural hosted component retains exact type definitions and overrides");
    require(window.selectedEntityIds()==QStringList{QString::fromStdString(proposed.component),QString::fromStdString(proposed.column),QString::fromStdString(proposed.beam)},
            "actual selection remaps all three baseline physical owners to proposed identities");
    physical_structure(after,f,false,false);physical_structure(after,proposed,true,false);plans(drawing.entities(),proposed,true,false);require_mixed_frame(window,proposed,true,false);
    require(output(drawing)!=committed,"baseline replacement publishes proposed geometry to committed output");
    const auto copied=decode_boundary_dimension_entity(after.entities().at(proposed.dimension));
    const auto hosted_copy=decode_boundary_dimension_entity(after.entities().at(proposed.hosted_dimension));
    const auto independent=decode_document_assembly_instance(after.entities().at(proposed.component));
    const auto& identity=copied.dimension->assembly_target->component;
    require(copied.dimension->boundary_id==proposed.component && identity.document_entity_id==proposed.component &&
            identity.catalog_id==independent.assembly_catalog_id && identity.instance_id==independent.instance.id && identity.part_path.empty(),
            "copied independent dimension remaps complete qualified target");
    const auto& hosted_identity=hosted_copy.dimension->assembly_target->component;
    require(hosted_copy.dimension->boundary_id==proposed.hosted_catalog && !hosted_identity.document_entity_id &&
            hosted_identity.catalog_id==proposed.hosted_catalog && hosted_identity.instance_id==proposed.hosted_instance && hosted_identity.part_path.empty(),
            "copied hosted dimension remaps catalog, actual hosted root and selector");
    require(copied.dimension->presentation==original_dimension.dimension->presentation && hosted_copy.dimension->presentation==original_hosted_dimension.dimension->presentation &&
            after.entities().at(proposed.dimension).extensions==before.entities().at(f.dimension).extensions &&
            after.entities().at(proposed.hosted_dimension).extensions==before.entities().at(f.hosted_dimension).extensions,
            "both copied dimensions retain exact paper presentation and opaque metadata");
    require_near(copied.dimension->resolve(after).segment_length(),3,"proposed independent extent resolves resized three-metre width");
    require_near(hosted_copy.dimension->resolve(after).segment_length(),.5,"hosted type-owned extent retains physical half-metre width");
    require_near(original_dimension.dimension->resolve(after).segment_length(),2,"retained baseline independent extent still resolves original geometry");
    require_near(original_hosted_dimension.dimension->resolve(after).segment_length(),.5,"retained baseline hosted extent still resolves original geometry");
    require(window.undoCommand() && window.document().snapshot().entities()==before.entities() && window.document().snapshot().assets()==before.assets(),
            "one phase Undo restores exact owners, catalogs, registry and dimension graph");
    require(window.redoCommand() && window.document().snapshot().entities()==after.entities() && window.document().snapshot().assets()==after.assets(),
            "one phase Redo restores exact allocated replacements and dimension graph");
    fresh_open(window,directory,"phase",[&](MainWindow& reopened,const DocumentSnapshot& loaded) {
        physical_structure(loaded,f,false,false);physical_structure(loaded,proposed,true,false);
        require_near(decode_boundary_dimension_entity(loaded.entities().at(proposed.dimension)).dimension->resolve(loaded).segment_length(),3,
                     "fresh Open retains proposed independent extent geometry");
        require_near(decode_boundary_dimension_entity(loaded.entities().at(proposed.hosted_dimension)).dimension->resolve(loaded).segment_length(),.5,
                     "fresh Open retains proposed hosted extent geometry");
        require(reopened.selectModelPhaseRegistry("resize-phases") && reopened.selectRemodelingAlternative("resize-alternative"),"fresh Open restores actual alternative selection APIs");
        select(reopened,proposed,false);require_mixed_frame(reopened,proposed,true,false);
    });guard.verify();
}
Json scenario(const QString& directory, bool reverse, bool depth, bool frame_only) {
    const auto f=fixture();const auto stem=QString(reverse ? "beam-first" : "component-first")+(depth ? "-depth" : "-width");
    MainWindow window(f.document,nullptr,QDir(directory).filePath(stem+"-library.json"));ModalGuard guard;display(window);
    physical_structure(window.document().snapshot(),f,false,depth);select(window,f,reverse);guard.verify();
    auto& drawing=canvas(window);capture(drawing,stem+"-selected.png");
    require_mixed_frame(window,f);
    if (frame_only) return Json::object();
    plans(drawing.entities(),f,false,depth);
    const auto before=window.document().snapshot();const auto committed=output(drawing);
    const auto handles=drawing.selectionAxisHandlePositions();
    const auto press=depth ? *std::min_element(handles.begin(),handles.end(),[](QPointF a,QPointF b){return a.y()<b.y();})
                           : *std::max_element(handles.begin(),handles.end(),[](QPointF a,QPointF b){return a.x()<b.x();});
    const auto release=press+(depth ? QPointF(0,-2*drawing.viewScale()) : QPointF(3*drawing.viewScale(),0));
    require(drawing.rect().contains(release.toPoint()),"structural native drag remains inside canvas");
    mouse(drawing,QEvent::MouseButtonPress,press);unchanged(window,before);
    mouse(drawing,QEvent::MouseMove,release);
    require(drawing.entityTransformPreviewPending(),"structural mixed preview uses actual asynchronous native production");
    unchanged(window,before);
    wait(window,guard,[&]{return !drawing.entityTransformPreviewPending() && !drawing.entityTransformPreview().empty();},"structural mixed exact preview completes");
    require(drawing.entityTransformPreview().size()==3,"detached exact preview contains precisely all three selected owners");
    plans(drawing.entityTransformPreview(),f,true,depth);plans(drawing.entities(),f,false,depth);unchanged(window,before);
    const auto captured=drawing.selectionAxisResizeCohortFrame();require(captured.has_value(),"drag retains press-time analytical authority");frame(*captured,false,depth);
    const auto preview_handles=drawing.selectionAxisHandlePositions();
    require(preview_handles.size()==4,"admitted structural preview retains four actual painted side grips");
    const auto coordinate=[depth](QPointF point){return depth ? -point.y() : point.x();};
    const auto lower=[&](const std::vector<QPointF>& points) {
        return coordinate(*std::min_element(points.begin(),points.end(),[&](QPointF a,QPointF b){return coordinate(a)<coordinate(b);}));
    };
    const auto upper=[&](const std::vector<QPointF>& points) {
        return coordinate(*std::max_element(points.begin(),points.end(),[&](QPointF a,QPointF b){return coordinate(a)<coordinate(b);}));
    };
    require_near(lower(preview_handles),lower(handles),"painted structural preview retains common opposite side anchor");
    // Side grips move around annotation footprints. Exact physical edges are
    // checked above; grip placement need not equal the raw model displacement.
    require(upper(preview_handles)>upper(handles),"painted structural preview moves its positive grip outward");
    require(output(drawing)==committed,"detached structural preview stays out of committed print/export output");
    capture(drawing,stem+"-preview.png");mouse(drawing,QEvent::MouseButtonRelease,release);
    wait(window,guard,[&]{return window.document().revision()==before.revision()+1 && !drawing.entityTransformPreviewPending();},"one release publishes all three structural owners exactly once");
    const auto after=window.document().snapshot();
    require(after.history().size()==before.history().size()+1,"three-owner mixed resize produces one retained history event");
    mixed41_command(after);
    require(drawing.entityTransformPreview().empty(),"release retires detached structural preview");
    const auto result=physical_structure(after,f,true,depth);preserved(before,after,f);plans(drawing.entities(),f,true,depth);
    require_mixed_frame(window,f,true,depth);require(output(drawing)!=committed,"release publishes resized geometry to committed output");capture(drawing,stem+"-committed.png");
    require(window.undoCommand() && window.document().snapshot().entities()==before.entities() && window.document().snapshot().assets()==before.assets(),"one native Undo restores all three source owners and assets exactly");
    require(window.redoCommand() && window.document().snapshot().entities()==after.entities() && window.document().snapshot().assets()==after.assets(),"one native Redo restores the complete three-owner result exactly");
    const auto path=QDir(directory).filePath(stem+".bldproj");require(window.saveProjectAs(path),"mixed structural project saves");
    const auto saved=window.document().snapshot();require(saved.entities()==after.entities() && saved.assets()==after.assets(),"save retains exact mixed structural result");
    require(window.createNewProject(),"release saved writer lease before fresh ordinary Open");
    MainWindow reopened({},nullptr,QDir(directory).filePath(stem+"-reopened-library.json"));display(reopened);
    require(reopened.openProject(path) && reopened.document().is_editable(),"fresh ordinary Open obtains native writer ownership");
    const auto loaded=reopened.document().snapshot();
    require(document_snapshot_digest(loaded)==document_snapshot_digest(saved) && loaded.entities()==saved.entities() &&
            loaded.assets()==saved.assets() && loaded.history().size()==saved.history().size(),"fresh ordinary Open retains exact entities, assets and complete history");
    auto* undo=reopened.findChild<QAction*>("undoCommand");require(undo && !undo->isEnabled(),"fresh ordinary Open has its actual Undo action disabled");
    require(!reopened.undoCommand(),"fresh ordinary Open cannot navigate retained history across session boundary");unchanged(reopened,loaded);
    same_structure(result,physical_structure(loaded,f,true,depth));preserved(before,loaded,f);select(reopened,f,reverse);
    plans(canvas(reopened).entities(),f,true,depth);require_mixed_frame(reopened,f,true,depth);guard.verify();capture(canvas(reopened),stem+"-reopened.png");
    return result;
}
} // namespace

int main(int argc, char** argv) {
    sketch::testing::noninteractive_errors();qputenv("QT_QPA_PLATFORM","offscreen");
    QStandardPaths::setTestModeEnabled(true);QApplication application(argc,argv);QTemporaryDir settings;
    QCoreApplication::setOrganizationName("VertexTests");
    QCoreApplication::setApplicationName("MixedComponentStructuralResize-"+QUuid::createUuid().toString(QUuid::WithoutBraces));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,settings.path());
    QSettings::setPath(QSettings::IniFormat,QSettings::SystemScope,settings.path());
    try {
        require(settings.isValid(),"private temporary settings directory exists");
        const auto font_id=QFontDatabase::addApplicationFont(":/fonts/Inter.ttf");require(font_id>=0,"bundled Inter resource loads");
        const auto families=QFontDatabase::applicationFontFamilies(font_id);require(!families.isEmpty(),"bundled Inter font exposes a family");
        application.setFont(QFont(families.front(),10));QTemporaryDir directory;require(directory.isValid(),"private project directory exists");
        const bool frame_only=application.arguments().contains("--frame-only");
        const bool review_only=application.arguments().contains("--review-only");
        const bool lifecycle_only=application.arguments().contains("--lifecycle-only");
        const bool phase_only=application.arguments().contains("--phase-only");
        const bool site_only=application.arguments().contains("--site-only");
        if (!site_only && (frame_only || (!review_only && !lifecycle_only && !phase_only))) {
            for (const bool depth:{false,true}) {
                const auto forward=scenario(directory.path(),false,depth,frame_only);
                const auto reverse=scenario(directory.path(),true,depth,frame_only);
                if (!frame_only) same_structure(forward,reverse);
            }
        }
        if (!frame_only && !phase_only && !site_only) {
            dimension_review(directory.path(),false);dimension_review(directory.path(),true);
            if (!review_only) {
                stale_release(directory.path(),StaleAuthority::viewport);
                stale_release(directory.path(),StaleAuthority::selection);
                stale_release(directory.path(),StaleAuthority::history);
            }
        }
        if (!site_only && (phase_only || (!frame_only && !review_only && !lifecycle_only))) phase_scenario(directory.path());
        if (site_only || (!frame_only && !review_only && !lifecycle_only && !phase_only)) {
            const auto forward=site_scenario(directory.path(),false);
            const auto reverse=site_scenario(directory.path(),true);
            same_structure(forward,reverse);
        }
        std::cout<<"mixed_component_structural_resize_desktop_tests"
                 <<(frame_only ? " frame checks" : review_only ? " dimension review" : phase_only ? " phase qualification" : site_only ? " Site qualification" : " native lifecycle")
                 <<" passed\n";return 0;
    } catch (const std::exception& error) {
        std::cerr<<"mixed_component_structural_resize_desktop_tests: "<<error.what()<<'\n';return 1;
    }
}
