#include "sketch/desktop/main_window.hpp"
#include "sketch/assembly_document_adapter.hpp"
#include "sketch/boundary_dimension.hpp"
#include "sketch/document_digest.hpp"
#include "sketch/slab_plan_geometry.hpp"
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
#include <cmath>
#include <functional>
#include <iostream>
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
Json ring_json(const Boundary& ring) {
    auto result=Json::array();
    for (const auto& edge:ring)
        result.push_back({{"start",{edge.start.x,edge.start.y}},
                          {"end",{edge.end.x,edge.end.y}},{"sweep_radians",edge.sweep_radians}});
    return result;
}

struct Fixture {
    std::shared_ptr<Document> document;
    std::string component, slab, catalog, type, dimension;
};
Fixture fixture(bool with_dimension=false) {
    Fixture f;f.component=fresh("component");f.slab=fresh("slab");
    f.catalog=fresh("catalog");f.type=fresh("type");
    // One ordinary layer, no phases, hosts or holes. The ordinary fixture has
    // anonymous geometry; review cases opt into actual identified topology.
    // The independently placed component spans [5,7] x [1,2]; the slab spans
    // [1,4] x [0,3]. Their analytical union is [1,7] x [0,3].
    const Json context={{"property_id","property-1"},{"building_id","building-1"},
                        {"floor_id","floor-1"},{"layer_id","layer-1"}};
    AssemblyProfile profile;profile.id="profile";profile.outer=rectangle(0,0,2,1);
    profile.elevation_m=.25;profile.height_m=1.5;
    if (with_dimension) profile.topology=AssemblyProfileTopology{
        {"source-outer",{"source-e0","source-e1","source-e2","source-e3"},
                        {"source-v0","source-v1","source-v2","source-v3"}}, {}};
    AssemblyType type;type.id=f.type;type.name="Independent rectangular component";
    type.profiles={profile};
    Entity catalog{f.catalog,"assembly_model",{{"model",AssemblyModel::create({}, {type}, {}).to_json()}}};
    AssemblyInstance instance;instance.id=f.component;instance.type_id=f.type;
    instance.root_transform=AssemblyTransform{{5,1,.75},0,1};
    auto component=encode_document_assembly_instance({f.component,"assembly_instance",context},
                                                     {f.catalog,instance});
    auto slab_properties=context;
    slab_properties["boundary"]=ring_json(rectangle(1,0,3,3));
    slab_properties["holes"]=Json::array();slab_properties["thickness_m"]=.2;
    slab_properties["elevation_m"]=1.25;
    slab_properties["layers"]=Json::array({{{"id","core"},{"thickness_m",.2}}});
    std::vector<Entity> entities{
        {"property-1","property",{{"name","Mixed resize regression"}},false},
        {"building-1","building",{{"property_id","property-1"},{"name","Building"}},false},
        {"floor-1","floor",{{"building_id","building-1"},{"name","Ground"},{"elevation_m",0}},false},
        {"layer-1","layer",{{"floor_id","floor-1"},{"name","Plan"}},false},
        {f.slab,"slab",slab_properties,false},catalog,component};
    if (with_dimension) {
        f.dimension=fresh("profile-dimension");
        BoundaryDimension dimension;
        dimension.id=f.dimension;dimension.boundary_id=f.component;
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
    drawing.setViewTransform({5.5,1.5},45);
}
void select(MainWindow& window, const Fixture& f, bool reverse) {
    const auto first=QString::fromStdString(reverse ? f.slab : f.component);
    const auto second=QString::fromStdString(reverse ? f.component : f.slab);
    require(window.selectEntity(first) && window.selectEntity(second,true),
            "both ordinary source objects must be selectable in the actual window");
    const auto ids=window.selectedEntityIds();
    require(ids.size()==2 && ids.contains(first) && ids.contains(second),
            "actual window selection must retain both independent owners");
    require(ids==QStringList{first,second},"actual window must retain the deliberately reversed selection order");
    QApplication::processEvents();
}
void frame(const CanvasSelectionFrame& actual, double x, double width) {
    require_near(actual.center.x,x,"cohort analytical centre X");
    require_near(actual.center.y,1.5,"cohort analytical centre Y");
    require_near(actual.rotation_radians,0,"cohort must use horizontal plan axes");
    require_near(actual.width_metres,width,"cohort analytical width");
    require_near(actual.depth_metres,3,"cohort analytical depth");
    require_near(actual.center.x-actual.width_metres/2,1,"opposite physical edge stays anchored at X=1");
}
void require_mixed_frame(MainWindow& window, const Fixture& f) {
    auto& drawing=canvas(window);
    // Diagnose bad fixtures before reporting the intentional baseline failure.
    for (const auto& id:{f.component,f.slab}) {
        const auto found=std::find_if(drawing.entities().begin(),drawing.entities().end(),[&](const auto& item) {
            return item.id==QString::fromStdString(id) && item.selected && !item.segments.empty();
        });
        require(found!=drawing.entities().end(),"fixture must publish each selected physical owner with visible geometry");
    }
    const auto analytical=drawing.selectionAxisResizeCohortFrame();
    require(analytical.has_value(),
        "MISSING MIXED SIDE FRAME: ordinary independent assembly_instance + slab selection must expose an analytical resize cohort");
    frame(*analytical,4,6);
    require(drawing.selectionAxisHandlePositions().size()==4,
        "MISSING MIXED SIDE HANDLES: component + slab cohort must offer four actual painted side grips");
}
void mouse(PlanCanvas& drawing, QEvent::Type kind, QPointF pixel) {
    QMouseEvent event(kind,pixel,drawing.mapToGlobal(pixel.toPoint()),
        kind==QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
        kind==QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton,Qt::ShiftModifier);
    QApplication::sendEvent(&drawing,&event);
}
void wait(MainWindow& window, const std::function<bool()>& predicate, const char* message) {
    QElapsedTimer deadline;deadline.start();
    while (!predicate() && deadline.elapsed()<10000)
        QApplication::processEvents(QEventLoop::AllEvents,20);
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

// Decode the final authoring data, then evaluate its raw affine coefficients
// and rigid pose directly. Expected points below are handwritten global-axis
// arithmetic; no resize authoring/replay helper supplies the expected result.
Vec2 mapped(Vec2 p, const std::optional<AssemblyProfilePlanarAffine>& map) {
    if (!map) return p;
    const double x=p.x-map->source_origin.x,y=p.y-map->source_origin.y;
    return {map->target_origin.x+map->xx*x+map->xy*y,
            map->target_origin.y+map->yx*x+map->yy*y};
}
Json physical_structure(const DocumentSnapshot& snapshot, const Fixture& f, bool resized) {
    AssemblyExpansionBudget budget;
    const auto expanded=expand_document_assembly_instance(snapshot.entities().at(f.component),snapshot.entities(),budget);
    require(expanded.profiles.size()==1,"independent component retains exactly one complete profile");
    const auto& profile=expanded.profiles.front();const auto& pose=profile.transform;
    require(profile.profile.outer.size()==4 && profile.profile.holes.empty(),"component retains its straight rectangular topology");
    const auto expected_component=rectangle(resized ? 7 : 5,1,resized ? 3 : 2,1);
    auto component_points=Json::array();
    for (std::size_t i=0;i<4;++i) {
        const auto point=mapped(profile.profile.outer[i].start,profile.profile.planar_affine);
        const double x=pose.translation_m.x+pose.scale*(point.x*std::cos(pose.rotation_radians)-
            (pose.mirrored_y ? -point.y : point.y)*std::sin(pose.rotation_radians));
        const double y=pose.translation_m.y+pose.scale*(point.x*std::sin(pose.rotation_radians)+
            (pose.mirrored_y ? -point.y : point.y)*std::cos(pose.rotation_radians));
        require_near(x,expected_component[i].start.x,"component physical vertex X follows the shared opposite-edge anchor");
        require_near(y,expected_component[i].start.y,"component physical vertex Y remains unchanged");
        require_near(profile.profile.outer[i].sweep_radians,0,"component edges remain straight");
        component_points.push_back({x,y});
    }
    const double base_z=pose.translation_m.z+pose.scale*pose.vertical_scale*profile.profile.elevation_m;
    const double height=pose.scale*pose.vertical_scale*profile.profile.height_m;
    require_near(base_z,1,"plan resize preserves component physical base Z");
    require_near(height,1.5,"plan resize preserves component physical height");
    const auto component=decode_document_assembly_instance(snapshot.entities().at(f.component));
    require(component.instance.root_transform && !component.instance.placement,"component remains an independent root");
    require_near(component.instance.root_transform->translation_m.z,.75,"component root Z remains fixed");
    const auto& slab=snapshot.entities().at(f.slab).properties;
    require(slab.at("boundary").size()==4 && slab.at("holes").empty(),"slab retains its straight rectangular topology");
    const auto map=slab.contains("planar_affine")
        ? std::optional<AssemblyProfilePlanarAffine>{decode_slab_planar_affine(slab.at("planar_affine"))}
        : std::nullopt;
    const auto expected_slab=rectangle(1,0,resized ? 4.5 : 3,3);
    auto slab_points=Json::array();
    for (std::size_t i=0;i<4;++i) {
        const auto& edge=slab.at("boundary").at(i);const auto& raw=edge.at("start");
        const auto point=mapped({raw.at(0).get<double>(),raw.at(1).get<double>()},map);
        require_near(point.x,expected_slab[i].start.x,"slab physical vertex X follows the shared opposite-edge anchor");
        require_near(point.y,expected_slab[i].start.y,"slab physical vertex Y remains unchanged");
        require_near(edge.at("sweep_radians").get<double>(),0,"slab edges remain straight");
        slab_points.push_back({point.x,point.y});
    }
    require_near(slab.at("elevation_m").get<double>(),1.25,"plan resize preserves slab elevation");
    require_near(slab.at("thickness_m").get<double>(),.2,"plan resize preserves slab thickness");
    require(slab.at("layers")==Json::array({{{"id","core"},{"thickness_m",.2}}}),"slab layers remain exact");
    for (const auto& id:{f.component,f.slab}) {
        const auto& properties=snapshot.entities().at(id).properties;
        require(properties.at("property_id")=="property-1" && properties.at("building_id")=="building-1" &&
                properties.at("floor_id")=="floor-1" && properties.at("layer_id")=="layer-1",
                "resizing preserves each source object's drawing context");
    }
    // Compare physical structure across fresh UUIDs, including topology and
    // overrides, rather than requiring generated private-type IDs to match.
    return {{"component_vertices",component_points},{"component_base_z",base_z},{"component_height",height},
            {"component_parts",profile.part_path.size()},{"component_overrides",component.instance.property_overrides},
            {"component_nested_overrides",component.instance.nested_overrides.size()},
            {"slab_vertices",slab_points},{"slab_elevation",slab.at("elevation_m")},
            {"slab_thickness",slab.at("thickness_m")},{"slab_layers",slab.at("layers")}};
}
void preview_bounds(const std::vector<CanvasEntity>& proposed, const std::string& id,
                    double x0, double y0, double x1, double y1) {
    std::optional<Bounds2> bounds;
    for (const auto& item:proposed) if (item.id==QString::fromStdString(id))
        for (const auto& edge:item.segments) for (const auto point:{edge.start,edge.end}) {
            if (!bounds) bounds=Bounds2{point,point};
            else {
                bounds->minimum.x=std::min(bounds->minimum.x,point.x);bounds->minimum.y=std::min(bounds->minimum.y,point.y);
                bounds->maximum.x=std::max(bounds->maximum.x,point.x);bounds->maximum.y=std::max(bounds->maximum.y,point.y);
            }
        }
    require(bounds.has_value(),"exact manufactured preview must cover each selected family");
    require_near(bounds->minimum.x,x0,"preview physical minimum X");require_near(bounds->minimum.y,y0,"preview physical minimum Y");
    require_near(bounds->maximum.x,x1,"preview physical maximum X");require_near(bounds->maximum.y,y1,"preview physical maximum Y");
}
void same_structure(const Json& a, const Json& b) {
    if (a.is_number() && b.is_number()) {require_near(a.get<double>(),b.get<double>(),"reverse selection order preserves physical structure");return;}
    require(a.type()==b.type(),"reverse selection order preserves semantic value kinds");
    if (a.is_array() || a.is_object()) {
        require(a.size()==b.size(),"reverse selection order preserves semantic structure size");
        for (auto it=a.begin();it!=a.end();++it) {
            if (a.is_object()) {require(b.contains(it.key()),"reverse selection retains semantic fields");same_structure(it.value(),b.at(it.key()));}
            else same_structure(it.value(),b.at(static_cast<std::size_t>(it-a.begin())));
        }
    } else require(a==b,"reverse selection order preserves semantic values");
}

void unchanged(MainWindow& window, const DocumentSnapshot& before) {
    const auto after=window.document().snapshot();
    require(document_snapshot_digest(after)==document_snapshot_digest(before),
            "refused mixed resize preserves the full document snapshot including retained history");
    require(after.entities()==before.entities() && after.assets()==before.assets() &&
            after.history().size()==before.history().size(),
            "refused mixed resize publishes neither family, assets nor a history command");
}
void settle_without_publication(MainWindow& window, const DocumentSnapshot& before) {
    // Pump owner callbacks after refusal as well: a stale worker must never
    // resurrect the canceled preview or publish its previously captured cohort.
    QElapsedTimer deadline;deadline.start();
    do {
        QApplication::processEvents(QEventLoop::AllEvents,20);
        unchanged(window,before);
        require(!canvas(window).entityTransformPreviewPending() && canvas(window).entityTransformPreview().empty(),
                "refused mixed resize admits no late worker preview");
    } while (deadline.elapsed()<350);
}
QPointF completed_preview(MainWindow& window, const Fixture& f, const DocumentSnapshot& before) {
    auto& drawing=canvas(window);const auto handles=drawing.selectionAxisHandlePositions();
    require(handles.size()==4,"lifecycle gesture starts from four actual mixed-family side grips");
    const auto press=*std::max_element(handles.begin(),handles.end(),[](QPointF a,QPointF b){return a.x()<b.x();});
    const auto release=press+QPointF(3*drawing.viewScale(),0);
    require(drawing.rect().contains(release.toPoint()),"lifecycle gesture remains inside the actual canvas");
    mouse(drawing,QEvent::MouseButtonPress,press);mouse(drawing,QEvent::MouseMove,release);
    require(drawing.entityTransformPreviewPending(),"lifecycle preview uses the asynchronous native producer");
    unchanged(window,before);
    wait(window,[&]{return !drawing.entityTransformPreviewPending() && !drawing.entityTransformPreview().empty();},
         "lifecycle mixed-family preview must complete before authority is invalidated");
    preview_bounds(drawing.entityTransformPreview(),f.component,7,1,10,2);
    preview_bounds(drawing.entityTransformPreview(),f.slab,1,0,5.5,3);
    unchanged(window,before);return release;
}

// A timer drives the real modal event loop. Exceptions stay inside the Qt
// callback and every unrecognized/failed dialog is rejected to prevent a hang.
class DimensionReviewDriver {
public:
    DimensionReviewDriver(MainWindow& window,const Fixture& f,const DocumentSnapshot& before,bool retire)
        : m_window(window),m_fixture(f),m_before(before),m_retire(retire) {
        m_timer.setInterval(10);
        QObject::connect(&m_timer,&QTimer::timeout,[this] {
            auto* dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget());
            if (!dialog) return;
            try {
                require(dialog->objectName()==QStringLiteral("AssemblyProfileDimensionReviewDialog") ||
                        dialog->windowTitle()==QStringLiteral("Review component dimensions"),
                        "mixed dimension release must open the actual component profile review");
                require(++m_reviews==1,"one affected profile dimension opens exactly one review");
                unchanged(m_window,m_before);
                auto* list=dialog->findChild<QListWidget*>();
                require(list && list->count()==1 && list->item(0)->toolTip()==QString::fromStdString(m_fixture.dimension),
                        "review inventory must contain the actual original-topology dimension carrier");
                auto* buttons=dialog->findChild<QDialogButtonBox*>();
                require(buttons && buttons->button(QDialogButtonBox::Cancel) && buttons->button(QDialogButtonBox::Ok),
                        "component profile review exposes its real Cancel and Apply controls");
                require(!buttons->button(QDialogButtonBox::Ok)->isEnabled(),
                        "profile review requires an explicit dimension decision before publication");
                if (!m_retire) {
                    buttons->button(QDialogButtonBox::Cancel)->click();
                    require(dialog->result()==QDialog::Rejected && !dialog->isVisible(),
                            "actual profile-review Cancel closes and rejects the modal");
                } else {
                    auto* remove=dialog->findChild<QPushButton*>("dimensionRemove");
                    if (!remove) for (auto* button:dialog->findChildren<QPushButton*>())
                        if (button->text()==QStringLiteral("Remove dimension")) {remove=button;break;}
                    require(remove && remove->isEnabled(),"review must expose an enabled explicit Remove dimension choice");
                    remove->click();unchanged(m_window,m_before);
                    require(buttons->button(QDialogButtonBox::Ok)->isEnabled(),
                            "explicit dimension retirement enables the actual Apply control");
                    buttons->button(QDialogButtonBox::Ok)->click();
                    require(dialog->result()==QDialog::Accepted && !dialog->isVisible(),
                            "reviewed retirement must be accepted through the actual Apply button");
                }
            } catch (const std::exception& error) {
                if (m_failure.empty()) m_failure=error.what();
                dialog->reject();
            } catch (...) {
                if (m_failure.empty()) m_failure="unknown exception while driving dimension review";
                dialog->reject();
            }
        });
        m_timer.start();
    }
    void verify() const {
        if (!m_failure.empty()) throw std::runtime_error(m_failure);
        require(m_reviews==1,"identified mixed resize release must actually run its dimension review modal");
    }
private:
    MainWindow& m_window;
    const Fixture& m_fixture;
    const DocumentSnapshot& m_before;
    bool m_retire;
    QTimer m_timer;
    int m_reviews=0;
    std::string m_failure;
};

void dimension_review(const QString& directory,bool retire) {
    const auto f=fixture(true);
    const auto stem=retire ? QStringLiteral("dimension-remove") : QStringLiteral("dimension-cancel");
    MainWindow window(f.document,nullptr,QDir(directory).filePath(stem+"-library.json"));display(window);
    const auto before=window.document().snapshot();
    const auto decoded=decode_boundary_dimension_entity(before.entities().at(f.dimension));
    require(decoded.supported() && decoded.dimension->kind==BoundaryDimensionKind::assembly_profile,
            "review fixture contains a supported actual assembly_profile dimension");
    require_near(decoded.dimension->resolve(before).segment_length(),2,"original identified component edge resolves to its source length");
    physical_structure(before,f,false);select(window,f,false);require_mixed_frame(window,f);
    DimensionReviewDriver review(window,f,before,retire);
    auto& drawing=canvas(window);const auto committed_output=output(drawing);
    const auto release=completed_preview(window,f,before);
    require(output(drawing)==committed_output,"reviewable manufactured preview stays out of print/export output");
    capture(drawing,stem+"-preview.png");mouse(drawing,QEvent::MouseButtonRelease,release);
    review.verify();
    if (retire) {
        require(window.focusWidget()==&drawing,"accepted review returns stored focus to its captured canvas");
        // Offscreen has no window manager to reactivate the parent after a
        // modal closes. Simulate activation without setting product focus.
        QApplication::setActiveWindow(&window);
    }
    if (!retire) {
        wait(window,[&]{return !drawing.entityTransformPreviewPending() && drawing.entityTransformPreview().empty();},
             "Cancel clears the provisional whole-cohort preview");
        review.verify();unchanged(window,before);settle_without_publication(window,before);
        require(output(drawing)==committed_output,"Cancel retains the exact committed print/export scene");
        physical_structure(window.document().snapshot(),f,false);
    } else {
        wait(window,[&]{return window.document().revision()==before.revision()+1 && !drawing.entityTransformPreviewPending();},
             "reviewed retirement publishes both families in exactly one command");
        review.verify();const auto after=window.document().snapshot();
        require(after.history().size()==before.history().size()+1 && !after.entities().contains(f.dimension),
                "one reviewed mixed resize command retires exactly the affected dimension carrier");
        physical_structure(after,f,true);
        require(after.assets()==before.assets(),"reviewed mixed resize preserves exact source assets");
        for (const auto& [id,entity]:before.entities())
            if (id!=f.component && id!=f.slab && id!=f.catalog && id!=f.dimension)
                require(after.entities().at(id)==entity,"reviewed retirement preserves every unrelated original entity");
        require(window.undoCommand() && window.document().snapshot().entities()==before.entities() &&
                window.document().snapshot().assets()==before.assets(),"one Undo restores both original families and the retired dimension");
        require(window.redoCommand() && window.document().snapshot().entities()==after.entities() &&
                window.document().snapshot().assets()==after.assets(),"one Redo restores both resized families and dimension retirement");
    }
    capture(drawing,stem+"-finished.png");
}

void stale_release(const QString& directory,bool viewport) {
    const auto f=fixture();
    const auto stem=viewport ? QStringLiteral("stale-viewport") : QStringLiteral("stale-selection");
    MainWindow window(f.document,nullptr,QDir(directory).filePath(stem+"-library.json"));display(window);
    select(window,f,false);require_mixed_frame(window,f);
    const auto before=window.document().snapshot();auto& drawing=canvas(window);
    const auto committed_output=output(drawing);const auto center=drawing.viewCenter();const auto scale=drawing.viewScale();
    // Also close unexpected error/review modals in these dimension-free cases.
    QString unexpected_modal;QTimer guard;guard.setInterval(10);
    QObject::connect(&guard,&QTimer::timeout,[&] {
        if (auto* dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget())) {
            unexpected_modal=dialog->windowTitle();if (unexpected_modal.isEmpty()) unexpected_modal="unnamed dialog";
            dialog->reject();
        }
    });guard.start();
    const auto release=completed_preview(window,f,before);
    require(output(drawing)==committed_output,"completed stale candidate is excluded from committed output");
    capture(drawing,stem+"-preview.png");
    if (viewport) {
        drawing.zoomBy(1.2);
        require(drawing.viewScale()!=scale,"stale viewport case actually changes the captured native view");
    } else {
        require(window.selectEntity(QString::fromStdString(f.component)),"stale selection case changes actual window selection");
        require(window.selectedEntityIds()==QStringList{QString::fromStdString(f.component)},
                "stale selection actually removes the slab from the captured cohort");
    }
    QApplication::processEvents();unchanged(window,before);
    mouse(drawing,QEvent::MouseButtonRelease,release);
    wait(window,[&]{return !drawing.entityTransformPreviewPending() && drawing.entityTransformPreview().empty();},
         "stale release must discard its entire completed mixed-family candidate");
    settle_without_publication(window,before);
    require(unexpected_modal.isEmpty(),"stale dimension-free cohort must refuse without opening review");
    physical_structure(window.document().snapshot(),f,false);
    drawing.setViewTransform(center,scale);select(window,f,false);
    require(output(drawing)==committed_output,"stale refusal retains the original committed output at the original view");
    require_mixed_frame(window,f);capture(drawing,stem+"-refused.png");
}

Json scenario(const QString& directory, bool reverse, bool frame_only) {
    const auto f=fixture();const auto stem=reverse ? QStringLiteral("slab-first") : QStringLiteral("component-first");
    MainWindow window(f.document,nullptr,QDir(directory).filePath(stem+"-library.json"));display(window);
    // An unexpected review must fail promptly instead of blocking unattended CI.
    QString unexpected_modal;QTimer modal_guard;modal_guard.setInterval(10);
    QObject::connect(&modal_guard,&QTimer::timeout,[&] {
        if (auto* dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget())) {
            unexpected_modal=dialog->objectName();if (unexpected_modal.isEmpty()) unexpected_modal="unnamed dialog";
            dialog->setAttribute(Qt::WA_DontShowOnScreen);dialog->reject();
        }
    });modal_guard.start();
    physical_structure(window.document().snapshot(),f,false);
    select(window,f,reverse);require_mixed_frame(window,f);
    auto& drawing=canvas(window);capture(drawing,stem+"-selected.png");
    if (frame_only) return Json::object();

    const auto before=window.document().snapshot();const auto committed_output=output(drawing);
    const auto handles=drawing.selectionAxisHandlePositions();
    // Choose the actual painted right-side centre, including screen padding
    // and any annotation avoidance. A three-metre drag adds half the six-metre
    // width: x' = 1 + 1.5*(x-1), y' = y, with all physical Z unchanged.
    const auto press=*std::max_element(handles.begin(),handles.end(),[](QPointF a,QPointF b){return a.x()<b.x();});
    const auto release=press+QPointF(3*drawing.viewScale(),0);
    require(drawing.rect().contains(release.toPoint()),"gesture destination lies inside the actual native canvas");
    mouse(drawing,QEvent::MouseButtonPress,press);
    require(window.document().revision()==before.revision(),"side press captures source without a command");
    const auto pressed_frame=drawing.selectionAxisResizeCohortFrame();
    require(pressed_frame.has_value(),"side press retains the captured analytical cohort frame");frame(*pressed_frame,4,6);
    mouse(drawing,QEvent::MouseMove,release);
    require(drawing.entityTransformPreviewPending(),"actual mixed resize must mark its manufactured native preview pending asynchronously");
    require(window.document().snapshot().entities()==before.entities(),"pending preview does not publish source changes");
    wait(window,[&]{return !drawing.entityTransformPreviewPending() && !drawing.entityTransformPreview().empty();},
         "actual mixed-family exact preview must finish");
    require(unexpected_modal.isEmpty(),"dimension-free mixed resize preview must not open a review dialog");
    const auto preview=drawing.entityTransformPreview();
    preview_bounds(preview,f.component,7,1,10,2);preview_bounds(preview,f.slab,1,0,5.5,3);
    // The public cohort getter deliberately returns the press-time authority
    // while dragging. The painted grips use the admitted analytical preview
    // frame, so observe those controls to verify its changed width and anchor.
    const auto captured_frame=drawing.selectionAxisResizeCohortFrame();
    require(captured_frame.has_value(),"manufactured preview retains its press-time analytical authority");frame(*captured_frame,4,6);
    const auto preview_handles=drawing.selectionAxisHandlePositions();
    require(preview_handles.size()==4,"admitted analytical preview keeps four actual side grips");
    const auto left_x=[](const std::vector<QPointF>& points) {
        return std::min_element(points.begin(),points.end(),[](QPointF a,QPointF b){return a.x()<b.x();})->x();
    };
    const auto right_x=[](const std::vector<QPointF>& points) {
        return std::max_element(points.begin(),points.end(),[](QPointF a,QPointF b){return a.x()<b.x();})->x();
    };
    require_near(left_x(preview_handles),left_x(handles),"analytical preview keeps the opposite painted edge anchored");
    require_near(right_x(preview_handles)-right_x(handles),3*drawing.viewScale(),"analytical preview side frame grows by the physical pointer displacement");
    require(window.document().revision()==before.revision() && window.document().snapshot().entities()==before.entities(),
            "admitted mixed preview remains detached until release");
    require(output(drawing)==committed_output,"manufactured preview stays out of committed print/export scene");
    capture(drawing,stem+"-preview.png");
    mouse(drawing,QEvent::MouseButtonRelease,release);
    wait(window,[&]{return window.document().revision()==before.revision()+1 && !drawing.entityTransformPreviewPending();},
         "mixed side release must publish exactly one command");
    require(unexpected_modal.isEmpty(),"dimension-free mixed release must need no review dialog");
    const auto after=window.document().snapshot();
    require(after.history().size()==before.history().size()+1,"both physical families publish as one history event");
    const auto result=physical_structure(after,f,true);capture(drawing,stem+"-committed.png");
    require(window.undoCommand() && window.document().snapshot().entities()==before.entities() &&
            window.document().snapshot().assets()==before.assets(),"one native Undo restores exact mixed source geometry and assets");
    require(window.redoCommand() && window.document().snapshot().entities()==after.entities() &&
            window.document().snapshot().assets()==after.assets(),"one native Redo restores exact mixed result geometry and assets");
    const auto path=QDir(directory).filePath(stem+".bldproj");
    require(window.saveProjectAs(path),"native mixed resize project must save");
    const auto saved=window.document().snapshot();
    require(saved.entities()==after.entities() && saved.assets()==after.assets(),"native save retains the exact mixed resize result");
    require(window.createNewProject(),"release the saved mixed project's writer lease before editable reopening");
    MainWindow reopened({},nullptr,QDir(directory).filePath(stem+"-reopened-library.json"));display(reopened);
    require(reopened.openProject(path) && reopened.document().is_editable(),"native mixed resize project must reopen with writer ownership");
    const auto loaded=reopened.document().snapshot();
    require(loaded.entities()==saved.entities() && loaded.assets()==saved.assets(),"native reopen retains exact mixed source graph and assets");
    same_structure(result,physical_structure(loaded,f,true));
    select(reopened,f,reverse);
    const auto loaded_frame=canvas(reopened).selectionAxisResizeCohortFrame();
    require(loaded_frame.has_value() && canvas(reopened).selectionAxisHandlePositions().size()==4,
            "reopened mixed cohort still offers its four native side grips");frame(*loaded_frame,5.5,9);
    return result;
}
} // namespace

int main(int argc, char** argv) {
    sketch::testing::noninteractive_errors();
    // Respect an explicitly selected Qt platform; otherwise keep unattended
    // native tests off the user's desktop.
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM","offscreen");
    QStandardPaths::setTestModeEnabled(true);QApplication application(argc,argv);
    QTemporaryDir settings;
    QCoreApplication::setOrganizationName("VertexTests");
    QCoreApplication::setApplicationName("MixedFamilyResize-"+QUuid::createUuid().toString(QUuid::WithoutBraces));
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
            if (!review_only) {stale_release(directory.path(),true);stale_release(directory.path(),false);}
        }
        std::cout<<"mixed_family_resize_desktop_tests"
                 <<(frame_only ? " frame checks" : review_only ? " dimension review" : " native lifecycle")<<" passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr<<"mixed_family_resize_desktop_tests: "<<error.what()<<'\n';return 1;
    }
}
