#include "sketch/desktop/main_window.hpp"
#include "sketch/assembly_document_adapter.hpp"
#include "sketch/boundary_dimension.hpp"
#include "sketch/building_entity.hpp"
#include "sketch/document_digest.hpp"
#include "sketch/roof_entity_codec.hpp"
#include "../src/desktop/plan_canvas.hpp"
#include "support/noninteractive_errors.hpp"

#include <QApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFont>
#include <QFontDatabase>
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
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

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
    std::string component, catalog, type, roof, child, sibling, dimension;
};
Fixture fixture(bool with_dimension=false) {
    Fixture f;f.component=fresh("component");f.catalog=fresh("catalog");f.type=fresh("type");
    f.roof=fresh("roof");f.child=fresh("skylight");f.sibling=fresh("unselected-skylight");
    const Json context={{"property_id","property-1"},{"building_id","building-1"},
                        {"floor_id","floor-1"},{"layer_id","layer-1"}};
    AssemblyProfile profile;profile.id="profile";profile.outer=rectangle(0,0,2,1);
    profile.elevation_m=.25;profile.height_m=1.5;
    if (with_dimension) profile.topology=AssemblyProfileTopology{
        {"source-outer",{"source-e0","source-e1","source-e2","source-e3"},
                        {"source-v0","source-v1","source-v2","source-v3"}}, {}};
    AssemblyType type;type.id=f.type;type.name="Independent rectangular component";type.profiles={profile};
    Entity catalog{f.catalog,"assembly_model",{{"model",AssemblyModel::create({}, {type}, {}).to_json()}}};
    AssemblyInstance instance;instance.id=f.component;instance.type_id=f.type;
    instance.root_transform=AssemblyTransform{{5,1,.75},0,1};
    auto component=encode_document_assembly_instance({f.component,"assembly_instance",context},{f.catalog,instance});
    component.extensions["fixture_metadata"]={{"ordinary","retained"}};
    SlopedRoofPanel panel{f.roof,{0,0,3},0,20,20,0,0,0,.2,
        {{f.child,1,.5,2,1,RoofSkylight{.06,.15,.024},0},
         {f.sibling,12,8,1.5,1.25,RoofSkylight{.08,.2,.03},0}}};
    auto roof=encode_building_entity(panel,{{"fixture_metadata",{{"opaque",Json::array({"roof",17,true})}}}});
    // Explicit schema 4 owns rotation_rad even when the authored angle is zero.
    roof.properties["version"]=4;
    roof.properties["opaque_roof_metadata"]={{"keep","exact"}};
    for (auto it=context.begin();it!=context.end();++it) roof.properties[it.key()]=it.value();
    for (auto& row:roof.properties.at("roof_openings")) {
        row["rotation_rad"]=0.0;
        row["opaque_child_metadata"]={{"keep",row.at("id")==f.child ? "selected" : "sibling"}};
    }
    // The analytical selected union is [1,7] x [.5,2]. The large containing
    // roof and its second real typed child are deliberately not selected.
    std::vector<Entity> entities{
        {"property-1","property",{{"name","Component and typed skylight resize"}},false},
        {"building-1","building",{{"property_id","property-1"},{"name","Building"}},false},
        {"floor-1","floor",{{"building_id","building-1"},{"name","Ground"},{"elevation_m",0}},false},
        {"layer-1","layer",{{"floor_id","floor-1"},{"name","Plan"}},false},
        catalog,component,roof};
    if (with_dimension) {
        f.dimension=fresh("profile-dimension");
        BoundaryDimension dimension;dimension.id=f.dimension;dimension.boundary_id=f.component;
        dimension.kind=BoundaryDimensionKind::assembly_profile;dimension.text_position={6,.5};
        AssemblyDimensionTarget target;target.component={f.catalog,f.component,f.component,{}};
        target.profile=AssemblyProfileDimensionTarget{"profile","source-outer",
            AssemblyProfileDimensionMeasurement::edge_length,"source-e0",std::nullopt,std::nullopt};
        dimension.assembly_target=target;
        auto entity=encode_boundary_dimension_entity(dimension);
        for (auto it=context.begin();it!=context.end();++it) entity.properties[it.key()]=it.value();
        entities.push_back(std::move(entity));
    }
    f.document=std::make_shared<Document>(Document::create(std::move(entities)));
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
    auto& drawing=canvas(window);drawing.setOverviewMapEnabled(false);
    drawing.setGridEnabled(false);drawing.setSnapEnabled(false);drawing.setWallSnapEnabled(false);
    drawing.setViewTransform({5.5,1.25},45);
}
struct ModalGuard {
    QString unexpected;
    QTimer timer;
    ModalGuard() {
        timer.setInterval(10);
        QObject::connect(&timer,&QTimer::timeout,[this] {
            if (auto* dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget())) {
                unexpected=dialog->windowTitle();if (unexpected.isEmpty()) unexpected="unnamed dialog";
                dialog->setAttribute(Qt::WA_DontShowOnScreen);dialog->reject();
            }
        });timer.start();
    }
    void verify() const {
        if (!unexpected.isEmpty()) throw std::runtime_error("Unexpected native modal: "+unexpected.toStdString());
    }
};
// Modifiers belong to each event. Child selection always uses Ctrl; resize
// uses Shift for fine movement while every application/canvas snap is off.
void mouse(PlanCanvas& drawing, QEvent::Type kind, QPointF pixel, Qt::KeyboardModifiers modifiers) {
    QMouseEvent event(kind,pixel,drawing.mapToGlobal(pixel.toPoint()),
        kind==QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
        kind==QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton,modifiers);
    QApplication::sendEvent(&drawing,&event);
}
QPointF screen(const PlanCanvas& drawing, Vec2 point) {
    const auto center=drawing.viewCenter();
    return QRectF(drawing.rect()).center()+QPointF((point.x-center.x)*drawing.viewScale(),
                                                -(point.y-center.y)*drawing.viewScale());
}
void click(PlanCanvas& drawing, Vec2 point) {
    const auto pixel=screen(drawing,point);
    require(drawing.rect().contains(pixel.toPoint()),"actual selection click is inside the native canvas");
    mouse(drawing,QEvent::MouseButtonPress,pixel,Qt::ControlModifier);
    mouse(drawing,QEvent::MouseButtonRelease,pixel,Qt::ControlModifier);
    QApplication::processEvents();
}
const CanvasRoofOpeningControls& controls(const PlanCanvas& drawing, const Fixture& f,
                                         const std::string& child, Revision revision) {
    const auto& all=drawing.roofOpeningControls();
    const auto found=std::find_if(all.begin(),all.end(),[&](const auto& item) {
        return item.target.roof_id==QString::fromStdString(f.roof) &&
               item.target.opening_id==QString::fromStdString(child);
    });
    require(found!=all.end(),"actual typed child mouth controls must exist for its qualified roof/opening identity");
    require(found->target.source_revision==revision && found->presentation_owner_id==QString::fromStdString(f.roof),
            "actual typed child controls qualify the current source revision and presentation owner");
    require(std::count_if(all.begin(),all.end(),[&](const auto& item) {
        return item.target.roof_id==found->target.roof_id && item.target.opening_id==found->target.opening_id;
    })==1,"actual typed child controls have one unambiguous qualified target");
    return *found;
}
void mouth_frame(const CanvasRoofOpeningFrame& frame, bool resized) {
    require_near(frame.center.x,resized ? 2.5 : 2,"typed mouth centre X");
    require_near(frame.center.y,1,"typed mouth centre Y remains fixed");
    require_near(frame.width_metres,resized ? 3 : 2,"typed mouth physical width");
    require_near(frame.depth_metres,1,"typed mouth physical depth remains fixed");
    require_near(frame.rotation_radians,0,"typed mouth rotation remains zero");
    require_near(frame.reference_along.x,1,"flat roof mouth along X");
    require_near(frame.reference_along.y,0,"flat roof mouth along Y");
    require_near(frame.reference_across.x,0,"flat roof mouth across X");
    require_near(frame.reference_across.y,1,"flat roof mouth across Y");
    require_near(frame.width_surface_scale,1,"flat roof width surface scale");
    require_near(frame.depth_surface_scale,1,"flat roof depth surface scale");
}
void select(MainWindow& window, const Fixture& f, bool reverse, bool resized=false) {
    require(window.selectEntity({}),"clear actual semantic selection before ordered native clicks");
    auto& drawing=canvas(window);const auto revision=window.document().revision();
    const auto target=controls(drawing,f,f.child,revision).target;
    mouth_frame(controls(drawing,f,f.child,revision).frame,resized);
    (void)controls(drawing,f,f.sibling,revision);
    const Vec2 child{resized ? 2.5 : 2,1};
    if (reverse) {
        click(drawing,child);
        require(drawing.selectedIds().isEmpty() && drawing.selectedRoofOpenings()==std::vector{target},
                "first Ctrl-click selects the actual typed mouth without selecting its containing roof");
        require(window.selectEntity(QString::fromStdString(f.component),true),
                "actual window adds the independent component after the typed-child click");
    } else {
        require(window.selectEntity(QString::fromStdString(f.component)),
                "actual window selects the independent component before the typed-child click");
        require(drawing.selectedIds()==QStringList{QString::fromStdString(f.component)} && drawing.selectedRoofOpenings().empty(),
                "ordinary component selection excludes the containing roof");
        click(drawing,child);
    }
    QApplication::processEvents();
    require(drawing.selectedIds()==QStringList{QString::fromStdString(f.component)} &&
            drawing.selectedRoofOpenings()==std::vector{target},
            "semantic selection is exactly the ordinary component and qualified typed skylight; roof body and sibling are excluded");
    const auto hit=drawing.selectedRoofOpeningAtModelPoint(child);
    require(hit && *hit==target,"actual selected mouth hit resolves the qualified typed child");
    for (const auto& item:drawing.entities()) if (item.id==QString::fromStdString(f.roof))
        require(!item.selected,"the visible containing roof body is not an ordinary selected object");
}
void frame(const CanvasSelectionFrame& actual, bool resized) {
    require_near(actual.center.x,resized ? 5.5 : 4,"cohort analytical centre X");
    require_near(actual.center.y,1.25,"cohort analytical centre Y");
    require_near(actual.rotation_radians,0,"cohort horizontal plan axes");
    require_near(actual.width_metres,resized ? 9 : 6,"cohort analytical width excludes containing roof and sibling");
    require_near(actual.depth_metres,1.5,"cohort analytical depth excludes containing roof and sibling");
    require_near(actual.center.x-actual.width_metres/2,1,"opposite physical edge is anchored at X=1");
}
void require_mixed_frame(MainWindow& window, const Fixture& f, bool resized=false) {
    auto& drawing=canvas(window);
    const auto found=std::find_if(drawing.entities().begin(),drawing.entities().end(),[&](const auto& item) {
        return item.id==QString::fromStdString(f.component) && item.selected && !item.segments.empty();
    });
    require(found!=drawing.entities().end(),"fixture publishes the selected ordinary component with actual visible geometry");
    const auto analytical=drawing.selectionAxisResizeCohortFrame();
    require(analytical.has_value(),
        "MISSING COMPONENT + TYPED SKYLIGHT SIDE FRAME: independent assembly_instance + selected typed roof child must expose an analytical resize cohort");
    frame(*analytical,resized);
    require(drawing.selectionAxisHandlePositions().size()==4,
        "MISSING COMPONENT + TYPED SKYLIGHT SIDE HANDLES: semantic cohort must offer four actual painted side grips");
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
    QImage image(drawing.size(),QImage::Format_ARGB32_Premultiplied);
    QPainter painter(&image);
    drawing.renderSceneAt(painter,image.rect(),drawing.viewScale(),drawing.viewCenter(),Qt::white);
    painter.end();return image;
}
void capture(PlanCanvas& drawing, const QString& name) {
    const auto directory=qEnvironmentVariable("VERTEX_TEST_CAPTURE_DIR");
    if (directory.isEmpty()) return;
    require(QDir().mkpath(directory) && drawing.grab().save(QDir(directory).filePath(name)),
            "optional native regression capture must save");
}
Vec2 mapped(Vec2 p, const std::optional<AssemblyProfilePlanarAffine>& map) {
    if (!map) return p;
    const double x=p.x-map->source_origin.x,y=p.y-map->source_origin.y;
    return {map->target_origin.x+map->xx*x+map->xy*y,map->target_origin.y+map->yx*x+map->yy*y};
}
const Json& opening(const Entity& roof, const std::string& id) {
    const auto& rows=roof.properties.at("roof_openings");
    const auto found=std::find_if(rows.begin(),rows.end(),[&](const auto& row) { return row.at("id")==id; });
    require(found!=rows.end(),"roof retains original typed opening identity");return *found;
}
Json physical_structure(const DocumentSnapshot& snapshot, const Fixture& f, bool resized) {
    AssemblyExpansionBudget budget;
    const auto expanded=expand_document_assembly_instance(snapshot.entities().at(f.component),snapshot.entities(),budget);
    require(expanded.profiles.size()==1,"ordinary component retains exactly one complete profile");
    const auto& profile=expanded.profiles.front();const auto& pose=profile.transform;
    require(profile.profile.outer.size()==4 && profile.profile.holes.empty(),"component keeps straight rectangular topology");
    const auto expected=rectangle(resized ? 7 : 5,1,resized ? 3 : 2,1);
    auto points=Json::array();
    for (std::size_t i=0;i<4;++i) {
        const auto point=mapped(profile.profile.outer[i].start,profile.profile.planar_affine);
        const double x=pose.translation_m.x+pose.scale*(point.x*std::cos(pose.rotation_radians)-
            (pose.mirrored_y ? -point.y : point.y)*std::sin(pose.rotation_radians));
        const double y=pose.translation_m.y+pose.scale*(point.x*std::sin(pose.rotation_radians)+
            (pose.mirrored_y ? -point.y : point.y)*std::cos(pose.rotation_radians));
        require_near(x,expected[i].start.x,"component physical X follows x'=1+1.5*(x-1)");
        require_near(y,expected[i].start.y,"component physical Y is unchanged");
        require_near(profile.profile.outer[i].sweep_radians,0,"component edges remain straight");
        points.push_back({x,y});
    }
    const double base_z=pose.translation_m.z+pose.scale*pose.vertical_scale*profile.profile.elevation_m;
    const double height=pose.scale*pose.vertical_scale*profile.profile.height_m;
    require_near(base_z,1,"plan resize preserves component physical base Z");
    require_near(height,1.5,"plan resize preserves component physical height");
    const auto component=decode_document_assembly_instance(snapshot.entities().at(f.component));
    require(component.instance.root_transform && !component.instance.placement,"component remains an independent root");
    require_near(component.instance.root_transform->translation_m.z,.75,"component root Z stays fixed");
    const auto& component_properties=snapshot.entities().at(f.component).properties;
    require(component_properties.at("property_id")=="property-1" && component_properties.at("building_id")=="building-1" &&
            component_properties.at("floor_id")=="floor-1" && component_properties.at("layer_id")=="layer-1",
            "component retains exact drawing context");
    const auto& roof=snapshot.entities().at(f.roof);const auto& row=opening(roof,f.child);
    require(roof.properties.at("version")==4 && roof.properties.at("roof_openings").size()==2,
            "roof keeps schema 4 and both existing typed children");
    const auto decoded=std::get<SlopedRoofPanel>(decode_roof_entity(roof));
    const auto child=std::find_if(decoded.openings.begin(),decoded.openings.end(),[&](const auto& item) {return item.id==f.child;});
    require(child!=decoded.openings.end() && child->skylight.has_value(),"resized child remains an actual editable typed skylight");
    require_near(child->x,1,"skylight physical left mouth edge remains anchored");
    require_near(child->y,.5,"skylight physical mouth Y stays fixed");
    require_near(child->width,resized ? 3 : 2,"skylight physical width follows the same axis scale");
    require_near(child->depth,1,"skylight physical depth stays fixed");
    require_near(child->rotation_radians,0,"skylight rotation stays fixed");
    require_near(child->skylight->frame_width,.06,"skylight frame profile stays fixed");
    require_near(child->skylight->curb_height,.15,"skylight curb profile stays fixed");
    require_near(child->skylight->glazing_thickness,.024,"skylight glazing profile stays fixed");
    return {{"component_vertices",points},{"component_base_z",base_z},{"component_height",height},
        {"component_parts",profile.part_path.size()},{"component_overrides",component.instance.property_overrides},
        {"component_nested_overrides",component.instance.nested_overrides.size()},
        {"child_rectangle",{child->x,child->y,child->width,child->depth}},{"child_rotation",child->rotation_radians},
        {"child_profile",row.at("skylight")},{"child_metadata",row.at("opaque_child_metadata")}};
}
void unchanged(MainWindow& window, const DocumentSnapshot& before) {
    const auto after=window.document().snapshot();
    require(document_snapshot_digest(after)==document_snapshot_digest(before) && after.entities()==before.entities() &&
            after.assets()==before.assets() && after.history().size()==before.history().size(),
            "pending and admitted preview leave exact source, assets and history detached");
}
void preserved(const DocumentSnapshot& before, const DocumentSnapshot& after, const Fixture& f,
               bool retired_dimension=false) {
    const auto& old_roof=before.entities().at(f.roof);const auto& roof=after.entities().at(f.roof);
    auto old_body=old_roof,new_body=roof;
    old_body.properties.erase("roof_openings");new_body.properties.erase("roof_openings");
    // Derived changed-child receipts are child authority beside the roof's
    // extension envelope; they do not change its body or opaque metadata.
    old_body.extensions.erase("roof_opening_input");new_body.extensions.erase("roof_opening_input");
    require(old_body==new_body && old_body.properties.dump()==new_body.properties.dump() &&
            old_body.extensions.dump()==new_body.extensions.dump(),
            "unselected containing roof body, pose, thickness, context and opaque metadata remain exact");
    if (roof.extensions.contains("roof_opening_input")) {
        const auto& entries=roof.extensions.at("roof_opening_input").at("entries");
        require(entries.size()==1 && entries.contains(f.child),
                "derived quantity receipts belong only to the selected existing skylight");
    }
    require(opening(roof,f.sibling).dump()==opening(old_roof,f.sibling).dump(),
            "unselected sibling skylight remains byte-exact including its profile and opaque metadata");
    auto old_child=opening(old_roof,f.child),new_child=opening(roof,f.child);
    for (const auto* key:{"x_m","y_m","width_m","depth_m","rotation_rad"}) {old_child.erase(key);new_child.erase(key);}
    require(old_child.dump()==new_child.dump(),"selected child retains exact identity, skylight profile and opaque metadata");
    require(after.entities().at(f.component).extensions==before.entities().at(f.component).extensions,
            "ordinary component retains opaque metadata");
    if (retired_dimension)
        require(!f.dimension.empty() && !after.entities().contains(f.dimension),"only the reviewed dimension carrier is retired");
    for (const auto& [id,entity]:before.entities()) if (id!=f.component && id!=f.catalog && id!=f.roof &&
                                                     !(retired_dimension && id==f.dimension))
        require(after.entities().at(id)==entity,"resize preserves every unrelated original entity");
    require(after.assets()==before.assets(),"resize preserves exact source assets");
}
bool same_point(Vec2 a, Vec2 b) { return std::abs(a.x-b.x)<1e-7 && std::abs(a.y-b.y)<1e-7; }
void preview_rectangle(const std::vector<CanvasEntity>& proposed, const std::string& owner, const Boundary& expected) {
    // Inspect exact manufactured physical ink, not a canvas affine fallback.
    // Flat roof projection includes the cavity's actual mouth edges; the
    // manufactured skylight frame has its own separate clearance inside them.
    for (const auto& wanted:expected) {
        bool found=false;
        for (const auto& item:proposed) if (item.id==QString::fromStdString(owner))
            for (const auto& edge:item.segments)
                if (std::abs(edge.sweep_radians)<1e-7 &&
                    ((same_point(edge.start,wanted.start) && same_point(edge.end,wanted.end)) ||
                     (same_point(edge.start,wanted.end) && same_point(edge.end,wanted.start)))) found=true;
        if (!found) {
            std::string observed;
            for (const auto& item:proposed) if (item.id==QString::fromStdString(owner))
                for (const auto& edge:item.segments)
                    observed += " ["+std::to_string(edge.start.x)+","+std::to_string(edge.start.y)+" -> "+
                        std::to_string(edge.end.x)+","+std::to_string(edge.end.y)+"]";
            throw std::runtime_error("Exact manufactured preview missing physical edge for "+owner+": ("+
                std::to_string(wanted.start.x)+","+std::to_string(wanted.start.y)+") -> ("+
                std::to_string(wanted.end.x)+","+std::to_string(wanted.end.y)+"); observed"+observed);
        }
    }
}
void same_structure(const Json& a, const Json& b) {
    if (a.is_number() && b.is_number()) {require_near(a.get<double>(),b.get<double>(),"reversed selection preserves physical structure");return;}
    require(a.type()==b.type(),"reversed selection preserves semantic value kinds");
    if (a.is_array() || a.is_object()) {
        require(a.size()==b.size(),"reversed selection preserves semantic structure size");
        for (auto it=a.begin();it!=a.end();++it) {
            if (a.is_object()) {require(b.contains(it.key()),"reversed selection retains semantic fields");same_structure(it.value(),b.at(it.key()));}
            else same_structure(it.value(),b.at(static_cast<std::size_t>(it-a.begin())));
        }
    } else require(a==b,"reversed selection preserves semantic values");
}
QPointF completed_preview(MainWindow& window, const Fixture& f, const DocumentSnapshot& before,
                          const ModalGuard& guard) {
    auto& drawing=canvas(window);const auto committed_output=output(drawing);
    const auto handles=drawing.selectionAxisHandlePositions();
    require(handles.size()==4,"lifecycle gesture starts from four actual mixed-cohort side grips");
    const auto press=*std::max_element(handles.begin(),handles.end(),[](QPointF a,QPointF b) {return a.x()<b.x();});
    // Initial union [1,7] x [.5,2], centre (4,1.25). The right physical
    // edge goes 7 -> 10: sx=1.5, sy=1 about (1,1.25). Component becomes
    // [7,10] x [1,2]; typed mouth becomes [1,4] x [.5,1.5]. Z is invariant.
    const auto release=press+QPointF(3*drawing.viewScale(),0);
    require(drawing.rect().contains(release.toPoint()),"side gesture destination is inside the actual native canvas");
    mouse(drawing,QEvent::MouseButtonPress,press,Qt::ShiftModifier);
    unchanged(window,before);
    const auto pressed_frame=drawing.selectionAxisResizeCohortFrame();
    require(pressed_frame.has_value(),"side press retains its analytical cohort authority");frame(*pressed_frame,false);
    mouse(drawing,QEvent::MouseMove,release,Qt::ShiftModifier);
    if (!drawing.entityTransformPreviewPending())
        throw std::runtime_error("actual component and typed-child side resize starts an asynchronous exact native preview: "+
                                 window.lastError().toStdString());
    unchanged(window,before);
    require(output(drawing)==committed_output,"pending asynchronous resize stays out of committed print/export output");
    wait(window,guard,[&] {return !drawing.entityTransformPreviewPending() && !drawing.entityTransformPreview().empty();},
         "complete component and typed-child manufactured preview must finish");
    unchanged(window,before);
    const auto preview=drawing.entityTransformPreview();
    preview_rectangle(preview,f.component,rectangle(7,1,3,1));
    preview_rectangle(preview,f.roof,rectangle(1,.5,3,1));
    preview_rectangle(preview,f.roof,rectangle(12,8,1.5,1.25));
    preview_rectangle(preview,f.roof,rectangle(0,0,20,20));
    // Retained child controls remain bound to the source until release.
    mouth_frame(controls(drawing,f,f.child,before.revision()).frame,false);
    const auto captured_frame=drawing.selectionAxisResizeCohortFrame();
    require(captured_frame.has_value(),"completed preview retains its captured analytical authority");frame(*captured_frame,false);
    const auto preview_handles=drawing.selectionAxisHandlePositions();
    require(preview_handles.size()==4,"completed typed-child cohort preview retains four actual side grips");
    const auto left_x=[](const std::vector<QPointF>& p) {
        return std::min_element(p.begin(),p.end(),[](QPointF a,QPointF b) {return a.x()<b.x();})->x();
    };
    const auto right_x=[](const std::vector<QPointF>& p) {
        return std::max_element(p.begin(),p.end(),[](QPointF a,QPointF b) {return a.x()<b.x();})->x();
    };
    require_near(left_x(preview_handles),left_x(handles),"preview keeps the opposite actual painted side grip anchored");
    require_near(right_x(preview_handles)-right_x(handles),3*drawing.viewScale(),"preview side grips follow the exact physical pointer displacement");
    require(output(drawing)==committed_output,"completed manufactured resize stays out of committed print/export output");
    return release;
}
void no_provisional_preview(const PlanCanvas& drawing) {
    require(drawing.entityTransformPreview().empty() && !drawing.roofOpeningPreviewBoundary() &&
            !drawing.roofOpeningPreviewEdit() && !drawing.roofOpeningGroupPreviewBoundary() &&
            !drawing.roofOpeningGroupPreviewEdit() && drawing.roofOpeningGroupPreviewControls().empty(),
            "nested review and refused release retain no provisional entity or typed-child preview");
}
void settle_without_publication(MainWindow& window, const DocumentSnapshot& before, const ModalGuard& guard) {
    QElapsedTimer deadline;deadline.start();
    do {
        QApplication::processEvents(QEventLoop::AllEvents,20);guard.verify();unchanged(window,before);
        require(!canvas(window).entityTransformPreviewPending(),"refused cohort has no late pending worker preview");
        no_provisional_preview(canvas(window));
    } while (deadline.elapsed()<350);
}
// Drive the real nested event loop; catch callback failures and reject its
// modal so an assertion cannot leave an unattended native test blocked.
class DimensionReviewDriver {
public:
    DimensionReviewDriver(MainWindow& window,const Fixture& f,const DocumentSnapshot& before,
                          const QImage& committed_output,bool retire)
        : m_window(window),m_fixture(f),m_before(before),m_output(committed_output),m_retire(retire) {
        m_timer.setInterval(10);
        QObject::connect(&m_timer,&QTimer::timeout,[this] {
            auto* dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget());
            if (!dialog) return;
            try {
                require(dialog->objectName()==QStringLiteral("AssemblyProfileDimensionReviewDialog") ||
                        dialog->windowTitle()==QStringLiteral("Review component dimensions"),
                        "typed skylight mixed release opens the actual component profile review");
                require(++m_reviews==1,"one affected profile dimension opens exactly one native review");
                auto& drawing=canvas(m_window);
                // These assertions run at the first observation of the nested
                // review, before making any review decision.
                no_provisional_preview(drawing);unchanged(m_window,m_before);
                require(output(drawing)==m_output,"nested review exposes only the unchanged committed output");
                const auto target=controls(drawing,m_fixture,m_fixture.child,m_before.revision()).target;
                require(drawing.selectedIds()==QStringList{QString::fromStdString(m_fixture.component)} &&
                        drawing.selectedRoofOpenings()==std::vector{target},
                        "nested review retains the actual selected component and qualified typed mouth");
                mouth_frame(controls(drawing,m_fixture,m_fixture.child,m_before.revision()).frame,false);
                (void)controls(drawing,m_fixture,m_fixture.sibling,m_before.revision());
                physical_structure(m_window.document().snapshot(),m_fixture,false);
                auto* list=dialog->findChild<QListWidget*>();
                require(list && list->count()==1 && list->item(0)->toolTip()==QString::fromStdString(m_fixture.dimension),
                        "native review identifies the original-topology profile dimension carrier");
                auto* buttons=dialog->findChild<QDialogButtonBox*>();
                require(buttons && buttons->button(QDialogButtonBox::Cancel) && buttons->button(QDialogButtonBox::Ok),
                        "native profile review exposes its real Cancel and Apply buttons");
                require(!buttons->button(QDialogButtonBox::Ok)->isEnabled(),
                        "publication requires an explicit dimension decision");
                if (!m_retire) {
                    buttons->button(QDialogButtonBox::Cancel)->click();
                    require(dialog->result()==QDialog::Rejected && !dialog->isVisible(),
                            "actual Cancel rejects and closes the profile review");
                } else {
                    auto* remove=dialog->findChild<QPushButton*>("dimensionRemove");
                    if (!remove) for (auto* button:dialog->findChildren<QPushButton*>())
                        if (button->text()==QStringLiteral("Remove dimension")) {remove=button;break;}
                    require(remove && remove->isEnabled(),"native review offers an explicit enabled Remove dimension choice");
                    remove->click();unchanged(m_window,m_before);no_provisional_preview(drawing);
                    require(output(drawing)==m_output,"Remove decision leaves source and committed output unchanged before Apply");
                    require(buttons->button(QDialogButtonBox::Ok)->isEnabled(),
                            "explicit retirement enables the actual Apply reviewed dimensions button");
                    buttons->button(QDialogButtonBox::Ok)->click();
                    require(dialog->result()==QDialog::Accepted && !dialog->isVisible(),
                            "retirement is accepted only through the real Apply button");
                }
            } catch (const std::exception& error) {
                if (m_failure.empty()) m_failure=error.what();dialog->reject();
            } catch (...) {
                if (m_failure.empty()) m_failure="unknown native dimension-review callback failure";dialog->reject();
            }
        });m_timer.start();
    }
    void verify() const {
        if (!m_failure.empty()) throw std::runtime_error(m_failure);
        require(m_reviews==1,"identified mixed release actually executes exactly one native dimension review");
    }
private:
    MainWindow& m_window;
    const Fixture& m_fixture;
    const DocumentSnapshot& m_before;
    const QImage& m_output;
    bool m_retire;
    QTimer m_timer;
    int m_reviews=0;
    std::string m_failure;
};
void dimension_review(const QString& directory,bool retire) {
    const auto f=fixture(true);
    const auto stem=retire ? QStringLiteral("dimension-remove") : QStringLiteral("dimension-cancel");
    MainWindow window(f.document,nullptr,QDir(directory).filePath(stem+"-library.json"));
    ModalGuard guard;display(window);
    const auto before=window.document().snapshot();
    const auto decoded=decode_boundary_dimension_entity(before.entities().at(f.dimension));
    require(decoded.supported() && decoded.dimension->kind==BoundaryDimensionKind::assembly_profile,
            "optional identified fixture contains a supported real assembly_profile dimension");
    require_near(decoded.dimension->resolve(before).segment_length(),2,"identified source component edge has exact original length");
    physical_structure(before,f,false);select(window,f,false);require_mixed_frame(window,f);
    auto& drawing=canvas(window);const auto committed_output=output(drawing);
    const auto release=completed_preview(window,f,before,guard);
    capture(drawing,stem+"-preview.png");guard.timer.stop();
    DimensionReviewDriver review(window,f,before,committed_output,retire);
    mouse(drawing,QEvent::MouseButtonRelease,release,Qt::ShiftModifier);review.verify();
    if (!retire) {
        guard.timer.start();
        wait(window,guard,[&]{return !drawing.entityTransformPreviewPending() && drawing.entityTransformPreview().empty();},
             "Cancel retires the whole component and typed-child candidate");
        unchanged(window,before);settle_without_publication(window,before,guard);
        require(output(drawing)==committed_output,"Cancel retains the exact original committed output");
        physical_structure(window.document().snapshot(),f,false);preserved(before,window.document().snapshot(),f);
    } else {
        require(window.focusWidget()==&drawing,"accepted review returns stored focus to its captured native canvas");
        // Offscreen has no window manager to reactivate the parent on close.
        QApplication::setActiveWindow(&window);guard.timer.start();
        wait(window,guard,[&]{return window.document().revision()==before.revision()+1 && !drawing.entityTransformPreviewPending();},
             "reviewed retirement publishes the component and typed skylight together exactly once");
        review.verify();const auto after=window.document().snapshot();
        require(after.history().size()==before.history().size()+1,"retirement and both physical edits share one history event");
        no_provisional_preview(drawing);
        const auto result=physical_structure(after,f,true);preserved(before,after,f,true);
        mouth_frame(controls(drawing,f,f.child,after.revision()).frame,true);
        require(window.undoCommand() && window.document().snapshot().entities()==before.entities() &&
                window.document().snapshot().assets()==before.assets(),"one Undo restores component, roof, both children and retired dimension exactly");
        require(window.redoCommand() && window.document().snapshot().entities()==after.entities() &&
                window.document().snapshot().assets()==after.assets(),"one Redo restores the exact resize and dimension retirement");
        const auto path=QDir(directory).filePath(stem+".bldproj");
        require(window.saveProjectAs(path),"reviewed retirement project saves");
        const auto saved=window.document().snapshot();
        require(saved.entities()==after.entities() && saved.assets()==after.assets(),"save preserves the exact retired dimension result");
        require(window.createNewProject(),"release saved retirement project's writer lease before reopening");
        MainWindow reopened({},nullptr,QDir(directory).filePath(stem+"-reopened-library.json"));display(reopened);
        require(reopened.openProject(path) && reopened.document().is_editable(),"retirement project reopens with writer ownership");
        const auto loaded=reopened.document().snapshot();
        require(loaded.entities()==saved.entities() && loaded.assets()==saved.assets() && loaded.history().size()==saved.history().size(),
                "editable reopen preserves exact retired graph, assets and history");
        same_structure(result,physical_structure(loaded,f,true));preserved(before,loaded,f,true);
        require(!reopened.undoCommand() && reopened.document().snapshot().entities()==saved.entities() &&
                reopened.document().snapshot().assets()==saved.assets(),"ordinary reopening starts a fresh undo session while preserving saved history");
        select(reopened,f,false,true);require_mixed_frame(reopened,f,true);
        capture(canvas(reopened),stem+"-reopened.png");
    }
    guard.verify();capture(drawing,stem+"-finished.png");
}
enum class StaleAuthority { selection, viewport, source };
void stale_release(const QString& directory,StaleAuthority stale) {
    const auto f=fixture();
    const auto stem=stale==StaleAuthority::viewport ? QStringLiteral("stale-viewport") :
        stale==StaleAuthority::source ? QStringLiteral("stale-source") : QStringLiteral("stale-selection");
    MainWindow window(f.document,nullptr,QDir(directory).filePath(stem+"-library.json"));
    ModalGuard guard;display(window);select(window,f,false);require_mixed_frame(window,f);
    const auto before=window.document().snapshot();auto& drawing=canvas(window);
    const auto committed_output=output(drawing);const auto center=drawing.viewCenter();const auto scale=drawing.viewScale();
    const auto release=completed_preview(window,f,before,guard);capture(drawing,stem+"-preview.png");
    if (stale==StaleAuthority::viewport) {
        drawing.zoomBy(1.2);
        require(drawing.viewScale()!=scale,"stale viewport actually changes the captured native view");
    } else if (stale==StaleAuthority::selection) {
        require(window.selectEntity(QString::fromStdString(f.component)),"stale selection changes actual window selection");
        require(drawing.selectedIds()==QStringList{QString::fromStdString(f.component)} && drawing.selectedRoofOpenings().empty(),
                "stale selection actually removes the qualified typed mouth from the captured cohort");
    } else {
        require(window.editProjectSubject(QStringLiteral("Changed during completed skylight preview"),{},{},QStringLiteral("{}")),
                "stale source changes the actual document through a public native command");
        require(window.document().revision()==before.revision()+1 &&
                window.document().snapshot().history().size()==before.history().size()+1,
                "stale source actually publishes exactly one independent metadata command");
        for (const auto& [id,entity]:before.entities()) if (id!="property-1")
            require(window.document().snapshot().entities().at(id)==entity,"source invalidation leaves all physical source owners exact");
    }
    QApplication::processEvents();guard.verify();
    const auto expected=window.document().snapshot();
    if (stale!=StaleAuthority::source) unchanged(window,before);
    mouse(drawing,QEvent::MouseButtonRelease,release,Qt::ShiftModifier);
    wait(window,guard,[&]{return !drawing.entityTransformPreviewPending() && drawing.entityTransformPreview().empty();},
         "stale release discards the completed whole component and typed-child candidate");
    settle_without_publication(window,expected,guard);physical_structure(window.document().snapshot(),f,false);
    preserved(expected,window.document().snapshot(),f);
    drawing.setViewTransform(center,scale);select(window,f,false);
    require(output(drawing)==committed_output,"stale refusal retains exact original committed physical output at the original view");
    require_mixed_frame(window,f);guard.verify();capture(drawing,stem+"-refused.png");
}
Json scenario(const QString& directory, bool reverse, bool frame_only) {
    const auto f=fixture();const auto stem=reverse ? QStringLiteral("skylight-first") : QStringLiteral("component-first");
    MainWindow window(f.document,nullptr,QDir(directory).filePath(stem+"-library.json"));
    ModalGuard guard;display(window);
    physical_structure(window.document().snapshot(),f,false);select(window,f,reverse);guard.verify();
    auto& drawing=canvas(window);capture(drawing,stem+"-selected.png");
    require_mixed_frame(window,f);
    if (frame_only) return Json::object();
    const auto before=window.document().snapshot();
    const auto release=completed_preview(window,f,before,guard);
    capture(drawing,stem+"-preview.png");
    mouse(drawing,QEvent::MouseButtonRelease,release,Qt::ShiftModifier);
    wait(window,guard,[&] {return window.document().revision()==before.revision()+1 && !drawing.entityTransformPreviewPending();},
         "one actual side release must publish one complete mixed resize event");
    const auto after=window.document().snapshot();
    require(after.history().size()==before.history().size()+1,"one release publishes both selected families as exactly one history event");
    require(drawing.entityTransformPreview().empty(),"release retires the admitted detached preview");
    const auto result=physical_structure(after,f,true);preserved(before,after,f);
    mouth_frame(controls(drawing,f,f.child,after.revision()).frame,true);
    capture(drawing,stem+"-committed.png");
    require(window.undoCommand() && window.document().snapshot().entities()==before.entities() &&
            window.document().snapshot().assets()==before.assets(),"one Undo restores exact component, roof, both typed children and assets");
    require(window.redoCommand() && window.document().snapshot().entities()==after.entities() &&
            window.document().snapshot().assets()==after.assets(),"one Redo restores the exact complete mixed resize result and assets");
    const auto path=QDir(directory).filePath(stem+".bldproj");
    require(window.saveProjectAs(path),"native component and typed-child project must save");
    const auto saved=window.document().snapshot();
    require(saved.entities()==after.entities() && saved.assets()==after.assets(),"save retains the exact mixed resize result");
    require(window.createNewProject(),"release saved project's writer lease before editable reopening");
    MainWindow reopened({},nullptr,QDir(directory).filePath(stem+"-reopened-library.json"));display(reopened);
    require(reopened.openProject(path) && reopened.document().is_editable(),"native mixed resize project reopens with writer ownership");
    const auto loaded=reopened.document().snapshot();
    require(loaded.entities()==saved.entities() && loaded.assets()==saved.assets(),"editable reopen retains exact resized authoring graph and assets");
    require(loaded.history().size()==saved.history().size(),"editable reopen retains the saved mixed resize history");
    require(!reopened.undoCommand() && reopened.document().snapshot().entities()==saved.entities() &&
            reopened.document().snapshot().assets()==saved.assets(),"ordinary reopening starts a fresh undo session while preserving saved history");
    same_structure(result,physical_structure(loaded,f,true));preserved(before,loaded,f);
    select(reopened,f,reverse,true);require_mixed_frame(reopened,f,true);guard.verify();
    capture(canvas(reopened),stem+"-reopened.png");
    return result;
}
} // namespace

int main(int argc, char** argv) {
    sketch::testing::noninteractive_errors();
    qputenv("QT_QPA_PLATFORM","offscreen");
    QStandardPaths::setTestModeEnabled(true);QApplication application(argc,argv);
    QTemporaryDir settings;
    QCoreApplication::setOrganizationName("VertexTests");
    QCoreApplication::setApplicationName("MixedComponentSkylightResize-"+QUuid::createUuid().toString(QUuid::WithoutBraces));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,settings.path());
    QSettings::setPath(QSettings::IniFormat,QSettings::SystemScope,settings.path());
    try {
        require(settings.isValid(),"private temporary settings directory must exist");
        const auto font_id=QFontDatabase::addApplicationFont(":/fonts/Inter.ttf");
        require(font_id>=0,"bundled Inter resource must load");
        const auto families=QFontDatabase::applicationFontFamilies(font_id);
        require(!families.isEmpty(),"bundled Inter resource must expose its family");
        application.setFont(QFont(families.front(),10));
        QTemporaryDir directory;require(directory.isValid(),"private project output directory must exist");
        const bool frame_only=application.arguments().contains("--frame-only");
        const bool review_only=application.arguments().contains("--review-only");
        const bool lifecycle_only=application.arguments().contains("--lifecycle-only");
        if (frame_only || (!review_only && !lifecycle_only)) {
            const auto forward=scenario(directory.path(),false,frame_only);
            const auto reverse=scenario(directory.path(),true,frame_only);
            if (!frame_only) same_structure(forward,reverse);
        }
        if (!frame_only) {
            dimension_review(directory.path(),false);dimension_review(directory.path(),true);
            if (!review_only) {
                stale_release(directory.path(),StaleAuthority::selection);
                stale_release(directory.path(),StaleAuthority::viewport);
                stale_release(directory.path(),StaleAuthority::source);
            }
        }
        std::cout<<"mixed_component_skylight_resize_desktop_tests"
                 <<(frame_only ? " frame checks" : review_only ? " dimension review" : " native lifecycle")<<" passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr<<"mixed_component_skylight_resize_desktop_tests: "<<error.what()<<'\n';return 1;
    }
}
