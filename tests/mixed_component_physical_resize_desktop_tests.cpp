#include "sketch/desktop/main_window.hpp"
#include "sketch/assembly_document_adapter.hpp"
#include "sketch/building_entity.hpp"
#include "sketch/boundary_dimension.hpp"
#include "sketch/document_digest.hpp"
#include "sketch/document_solid.hpp"
#include "sketch/roof_entity_codec.hpp"
#include "sketch/project_store.hpp"
#include "sketch/slab_geometry_edit.hpp"
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
#include <QImage>
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
#include <cstdint>
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
Json raw_boundary(const Boundary& boundary) {
    auto result=Json::array();
    for (const auto& edge:boundary)
        result.push_back({{"start",{edge.start.x,edge.start.y}},
                          {"end",{edge.end.x,edge.end.y}},
                          {"sweep_radians",edge.sweep_radians}});
    return result;
}
struct Fixture {
    std::shared_ptr<Document> document;
    std::string property=fresh("property"), building=fresh("building");
    std::string floor=fresh("floor"), layer=fresh("layer");
    std::string component=fresh("component"), slab=fresh("slab");
    std::string column=fresh("column"), beam=fresh("beam");
    std::string catalog=fresh("catalog"), type=fresh("type");
    std::string roof=fresh("roof"), child=fresh("skylight");
    std::string dimension;
    bool site=false;
};
Fixture fixture(bool with_dimension=false, bool site=false) {
    Fixture f;
    f.site=site;
    const Json context={{"property_id",f.property},{"building_id",f.building},
                        {"floor_id",f.floor},{"layer_id",f.layer}};
    AssemblyProfile profile;profile.id=fresh("profile");profile.outer=rectangle(0,0,2,1);
    profile.elevation_m=.25;profile.height_m=1.5;
    if (with_dimension) profile.topology=AssemblyProfileTopology{
        {"source-outer",{"source-e0","source-e1","source-e2","source-e3"},
                        {"source-v0","source-v1","source-v2","source-v3"}}, {}};
    AssemblyType type;type.id=f.type;type.name="Independent combined cohort component";type.profiles={profile};
    Entity catalog{f.catalog,"assembly_model",{{"model",AssemblyModel::create({}, {type}, {}).to_json()}}};
    catalog.extensions={{"fixture_metadata",{{"retain","catalog"},
        {"opaque_command",{{"version",52},{"mixed_selection_edit_intent",{{"version",42}}}}}}}};
    AssemblyInstance instance;instance.id=f.component;instance.type_id=f.type;
    instance.root_transform=AssemblyTransform{{5,1,.75},0,1};
    auto component=encode_document_assembly_instance({f.component,"assembly_instance",context},{f.catalog,instance});

    // Use the public slab document schema and typed decoder/solid producer,
    // as in the independent mixed-component/slab core fixture.
    auto slab_properties=context;
    slab_properties["boundary"]=raw_boundary(rectangle(1,0,3,3));
    slab_properties["holes"]=Json::array();slab_properties["thickness_m"]=.2;
    slab_properties["elevation_m"]=1.25;
    slab_properties["layers"]=Json::array({{{"id",fresh("slab-core")},{"thickness_m",.2}}});
    Entity slab{f.slab,"slab",std::move(slab_properties),false};
    Slab typed_slab;std::string slab_error;
    require(read_document_slab(slab,typed_slab,slab_error),"fixture slab must admit through the public typed document decoder");
    require(!make_slab(typed_slab).IsNull(),"fixture slab must generate a real native solid");
    auto column=encode_building_entity(RectangularColumn{f.column,{2,1,.4},2,2,3,0});
    auto beam=encode_building_entity(Beam{.id=f.beam,.start={2,3,2.2},.end={6,3,2.2},
                                       .up={0,0,1},.width=2,.depth=.4});
    SlopedRoofPanel panel{f.roof,{0,0,3},0,20,20,0,0,0,.2,
        {{f.child,1.5,.5,1,1,RoofSkylight{.06,.15,.024},0}}};
    auto roof=encode_building_entity(panel);
    // Explicit typed schema 4 retains the authored zero opening angle.
    roof.properties["version"]=4;
    for (auto& row:roof.properties.at("roof_openings")) {
        row["rotation_rad"]=0.0;
        row["opaque_child_metadata"]={{"keep","selected typed mouth"}};
    }
    for (auto* entity:{&component,&slab,&column,&beam,&roof}) {
        for (auto it=context.begin();it!=context.end();++it) entity->properties[it.key()]=it.value();
        entity->extensions={{"fixture_metadata",{{"retain",entity->id},{"opaque",Json::array({17,true,"source"})}}}};
    }
    for (const auto* entity:{&column,&beam})
        require(!make_building_shape(decode_building_entity(*entity)).IsNull(),
                "fixture structural owners must admit and generate real canonical native solids");
    const auto decoded_roof=std::get<SlopedRoofPanel>(decode_roof_entity(roof));
    require(decoded_roof.openings.size()==1 && decoded_roof.openings.front().id==f.child &&
            decoded_roof.openings.front().skylight.has_value(),"fixture roof must contain the genuine typed skylight");
    require(!make_building_shape(decoded_roof).IsNull(),"fixture unselected ordinary roof must generate a real native solid");
    // Hand-authored footprints: component [5,7] x [1,2], slab [1,4] x [0,3],
    // column [1,3] x [0,2], beam [2,6] x [2,4], mouth [1.5,2.5] x [.5,1.5].
    // The five-member union is [1,7] x [0,4]; the containing roof is excluded.
    std::vector<Entity> entities{
        {f.property,"property",{{"name","Combined physical resize"}},false},
        {f.building,"building",{{"property_id",f.property},{"name","Building"}},false},
        {f.floor,"floor",{{"building_id",f.building},{"name","Ground"},{"elevation_m",0}},false},
        {f.layer,"layer",{{"floor_id",f.floor},{"name","Plan"}},false},
        catalog,component,slab,column,beam,roof};
    if (with_dimension) {
        f.dimension=fresh("profile-dimension");
        BoundaryDimension dimension;dimension.id=f.dimension;dimension.boundary_id=f.component;
        dimension.kind=BoundaryDimensionKind::assembly_profile;dimension.text_position={6,.5};
        AssemblyDimensionTarget target;target.component={f.catalog,f.component,f.component,{}};
        target.profile=AssemblyProfileDimensionTarget{profile.id,"source-outer",
            AssemblyProfileDimensionMeasurement::edge_length,"source-e0",std::nullopt,std::nullopt};
        dimension.assembly_target=target;auto carrier=encode_boundary_dimension_entity(dimension);
        for (auto it=context.begin();it!=context.end();++it) carrier.properties[it.key()]=it.value();
        entities.push_back(std::move(carrier));
    }
    if (site) {
        entities[0].properties["site_frame"]={{"version",1},{"origin_m",{100,200,30}},
            {"rotation_radians",std::numbers::pi/2},
            {"vertical_datum",{{"identifier","combined-fixture-datum"},{"height_at_origin_m",1500}}}};
        entities[1].properties["site_placement"]={{"version",1},{"translation_m",{10,20,3}},
            {"rotation_radians",0}};
        for (auto& entity:entities)
            if (entity.id==f.component || entity.id==f.slab || entity.id==f.column ||
                entity.id==f.beam || entity.id==f.roof)
                entity.properties["presentation_frame"]={{"version",1},{"mode","building"}};
    }
    auto asset=Asset::create(fresh("retained-asset"),"application/octet-stream",
                            {std::byte{0x31},std::byte{0x42},std::byte{0x53}},
                            {{"fixture_metadata",{{"keep","asset envelope"},{"opaque",Json::array({7,"retained"})}}}});
    f.document=std::make_shared<Document>(Document::create(std::move(entities),{std::move(asset)}));
    return f;
}
PlanCanvas& canvas(MainWindow& window) {
    const auto name=window.workspace()==Workspace::architectural ? "architecturalPlanCanvas" : "measurementPlanCanvas";
    auto* result=dynamic_cast<PlanCanvas*>(window.findChild<QWidget*>(name));
    require(result,"actual active PlanCanvas must exist");return *result;
}
class ModalGuard {
public:
    ModalGuard() {
        m_timer.setInterval(10);QObject::connect(&m_timer,&QTimer::timeout,[this] {
            if (auto* dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget())) {
                m_title=dialog->windowTitle();if (m_title.isEmpty()) m_title="unnamed dialog";
                dialog->setAttribute(Qt::WA_DontShowOnScreen);dialog->reject();
            }
        });m_timer.start();
    }
    void verify() const {
        if (!m_title.isEmpty()) throw std::runtime_error("Unexpected combined physical resize modal: "+m_title.toStdString());
    }
    void pause() {m_timer.stop();}
    void resume() {m_timer.start();}
private:
    QTimer m_timer;QString m_title;
};
void settle(const ModalGuard& guard) {
    for (int pass=0;pass<3;++pass) {
        QApplication::sendPostedEvents(nullptr,QEvent::LayoutRequest);
        QApplication::processEvents(QEventLoop::AllEvents,20);guard.verify();
    }
}
void display(MainWindow& window, const ModalGuard& guard, const Fixture& f) {
    window.setAttribute(Qt::WA_DontShowOnScreen);window.resize(1400,900);
    window.setWorkspace(Workspace::measurement);window.setMetricUnits(true);window.show();settle(guard);
    if (f.site) {
        require(window.setActiveLayer(QString::fromStdString(f.layer)),"Site activates the fixture's real source layer");
        window.setWorkspace(Workspace::architectural);
        auto* action=window.findChild<QAction*>("sitePlan");require(action,"actual Site Plan action must exist");
        if (!action->isChecked()) action->trigger();
        settle(guard);
        require(action->isChecked() && window.workspace()==Workspace::architectural &&
                window.activeLayerId()==QString::fromStdString(f.layer),"Site publishes the actual checked view and source layer");
    }
    auto* snap=window.findChild<QToolButton*>("snapTool");
    require(snap,"actual window Snap toggle must exist");snap->setChecked(false);
    auto& drawing=canvas(window);drawing.setOverviewMapEnabled(false);drawing.setGridEnabled(false);
    drawing.setTool(CanvasTool::select);drawing.setSnapEnabled(false);drawing.setWallSnapEnabled(false);
    drawing.setViewTransform(f.site ? Vec2{78,214} : Vec2{4,2},45);
    settle(guard);
}
Vec2 presented(Vec2 point, bool site) {return site ? Vec2{80-point.y,210+point.x} : point;}
Vec2 mouth_center(bool resized, bool depth) {
    return {resized && !depth ? 2.5 : 2,resized && depth ? 1.5 : 1};
}
const CanvasRoofOpeningControls& controls(const PlanCanvas& drawing, const Fixture& f, Revision revision,
                                         bool resized=false, bool depth=false) {
    const auto& all=drawing.roofOpeningControls();
    const auto found=std::find_if(all.begin(),all.end(),[&](const auto& item) {
        return item.target.roof_id==QString::fromStdString(f.roof) &&
               item.target.opening_id==QString::fromStdString(f.child);
    });
    require(found!=all.end(),"fixture must publish actual mouth controls for its qualified typed skylight");
    require(found->target.source_revision==revision && found->presentation_owner_id==QString::fromStdString(f.roof),
            "published child controls must qualify the current source revision and actual roof presentation owner");
    require(std::count_if(all.begin(),all.end(),[&](const auto& item) {return item.target==found->target;})==1,
            "published child controls must have one unambiguous qualified target");
    const auto& mouth=found->frame;
    const auto expected=presented(mouth_center(resized,depth),f.site);
    require_near(mouth.center.x,expected.x,"actual typed mouth centre X");require_near(mouth.center.y,expected.y,"actual typed mouth centre Y");
    require_near(mouth.width_metres,resized && !depth ? 1.5 : 1,"actual typed mouth width");
    require_near(mouth.depth_metres,resized && depth ? 1.5 : 1,"actual typed mouth depth");
    require_near(mouth.rotation_radians,0,"actual typed mouth rotation");
    require_near(mouth.reference_along.x,f.site ? 0 : 1,"actual flat roof mouth along X");require_near(mouth.reference_along.y,f.site ? 1 : 0,"actual flat roof mouth along Y");
    require_near(mouth.reference_across.x,f.site ? -1 : 0,"actual flat roof mouth across X");require_near(mouth.reference_across.y,f.site ? 0 : 1,"actual flat roof mouth across Y");
    require_near(mouth.width_surface_scale,1,"actual flat roof mouth width scale");require_near(mouth.depth_surface_scale,1,"actual flat roof mouth depth scale");
    return *found;
}
void click_child(PlanCanvas& drawing, const Fixture& f, const ModalGuard& guard, bool resized, bool depth) {
    const auto center=drawing.viewCenter();
    const auto point=presented(mouth_center(resized,depth),f.site);
    const auto pixel=QRectF(drawing.rect()).center()+QPointF((point.x-center.x)*drawing.viewScale(),-(point.y-center.y)*drawing.viewScale());
    require(drawing.rect().contains(pixel.toPoint()),"typed-child selection click must be inside the actual canvas");
    QMouseEvent press(QEvent::MouseButtonPress,pixel,drawing.mapToGlobal(pixel.toPoint()),
                      Qt::LeftButton,Qt::LeftButton,Qt::ControlModifier);
    QApplication::sendEvent(&drawing,&press);
    QMouseEvent release(QEvent::MouseButtonRelease,pixel,drawing.mapToGlobal(pixel.toPoint()),
                        Qt::LeftButton,Qt::NoButton,Qt::ControlModifier);
    QApplication::sendEvent(&drawing,&release);settle(guard);
}
QStringList ordinary_ids(const Fixture& f, bool reverse) {
    QStringList ids{QString::fromStdString(f.component),QString::fromStdString(f.slab),
                    QString::fromStdString(f.column),QString::fromStdString(f.beam)};
    if (reverse) std::reverse(ids.begin(),ids.end());
    return ids;
}
void select(MainWindow& window, const Fixture& f, bool reverse, const ModalGuard& guard,
            bool resized=false, bool depth=false) {
    require(window.selectEntity({}),"clear actual semantic selection before selecting all five members");settle(guard);
    auto& drawing=canvas(window);const auto target=controls(drawing,f,window.document().revision(),resized,depth).target;
    if (reverse) {
        click_child(drawing,f,guard,resized,depth);
        require(drawing.selectedIds().isEmpty() && drawing.selectedRoofOpenings()==std::vector{target},
                "first actual Ctrl-click must select the typed mouth without selecting its roof");
    }
    const auto ids=ordinary_ids(f,reverse);
    for (const auto& id:ids)
        require(window.selectEntity(id,true),"actual public selection must add every ordinary physical cohort member");
    if (!reverse) click_child(drawing,f,guard,resized,depth);
    settle(guard);
    // MainWindow's owner lookup also carries the typed child's roof identity.
    // The canvas exposes the ordinary roster separately from qualified children.
    // The painted roster follows publication order, independently of the
    // user's ordered selection requests. Compare its exact membership.
    auto painted_ids=drawing.selectedIds();auto expected_ids=ids;
    painted_ids.sort();expected_ids.sort();
    require(painted_ids==expected_ids &&
            drawing.selectedRoofOpenings()==std::vector{target},
            "actual semantic roster must contain exactly four ordinary owners and one qualified typed skylight");
    const auto hit=drawing.selectedRoofOpeningAtModelPoint(presented(mouth_center(resized,depth),f.site));
    require(hit && *hit==target,"actual selected mouth hit must resolve the qualified typed child");
    (void)controls(drawing,f,window.document().revision(),resized,depth);
}
void published_owners(const PlanCanvas& drawing, const Fixture& f) {
    const std::array<std::pair<std::string,Boundary>,4> expected{{
        {f.component,rectangle(5,1,2,1)},{f.slab,rectangle(1,0,3,3)},
        {f.column,rectangle(1,0,2,2)},{f.beam,rectangle(2,2,4,2)}}};
    const auto same=[](Vec2 a,Vec2 b) {return std::hypot(a.x-b.x,a.y-b.y)<1e-7;};
    for (const auto& [id,boundary]:expected) {
        Boundary actual;
        for (const auto& item:drawing.entities()) if (item.id==QString::fromStdString(id)) {
            require(item.selected && item.holes.empty(),"fixture must publish each selected physical owner without fabricated holes");
            actual.insert(actual.end(),item.segments.begin(),item.segments.end());
        }
        require(actual.size()==4,"fixture must publish all four real physical boundary edges for each selected owner");
        for (const auto& edge:boundary) {
            const auto found=std::find_if(actual.begin(),actual.end(),[&](const auto& candidate) {
                return std::abs(candidate.sweep_radians)<1e-7 &&
                    ((same(candidate.start,edge.start) && same(candidate.end,edge.end)) ||
                     (same(candidate.start,edge.end) && same(candidate.end,edge.start)));
            });
            require(found!=actual.end(),"fixture published geometry must match each hand-authored physical boundary edge");
            actual.erase(found);
        }
    }
    bool published_roof=false;
    for (const auto& item:drawing.entities()) if (item.id==QString::fromStdString(f.roof)) {
        require(!item.selected,"containing ordinary roof must remain unselected before common frame qualification");
        published_roof=published_roof || !item.segments.empty();
    }
    require(published_roof,"fixture must publish the real unselected containing roof");
}
// Return only missing-feature diagnostics so both ordered selections are
// exercised on the RED baseline. Fixture/geometry failures still throw first.
std::string frame_failure(const PlanCanvas& drawing) {
    const auto actual=drawing.selectionAxisResizeCohortFrame();
    if (!actual) return "MISSING COMBINED PHYSICAL SIDE FRAME: independent component + slab + canonical rectangular column + horizontal beam + typed skylight must expose one common four-side resize frame";
    require_near(actual->center.x,4,"combined physical frame centre X");
    require_near(actual->center.y,2,"combined physical frame centre Y");
    require_near(actual->width_metres,6,"combined physical frame width");
    require_near(actual->depth_metres,4,"combined physical frame depth");
    require_near(actual->rotation_radians,0,"combined physical frame horizontal plan axes");
    require_near(actual->center.x-actual->width_metres/2,1,"combined physical frame left edge");
    require_near(actual->center.x+actual->width_metres/2,7,"combined physical frame right edge");
    require_near(actual->center.y-actual->depth_metres/2,0,"combined physical frame bottom edge");
    require_near(actual->center.y+actual->depth_metres/2,4,"combined physical frame top edge");
    const auto grips=drawing.selectionAxisHandlePositions();
    if (grips.size()!=4) return "MISSING COMBINED PHYSICAL SIDE GRIPS: all five selected semantic members must offer four actual painted side grips";
    for (std::size_t i=0;i<grips.size();++i) {
        require(std::isfinite(grips[i].x()) && std::isfinite(grips[i].y()),"actual combined physical side grips must have finite screen positions");
        for (std::size_t j=0;j<i;++j)
            require(std::hypot(grips[i].x()-grips[j].x(),grips[i].y()-grips[j].y())>1,
                    "actual combined physical side grips must occupy four distinct screen positions");
    }
    return {};
}
void capture(PlanCanvas& drawing, const QString& name) {
    const auto directory=qEnvironmentVariable("VERTEX_TEST_CAPTURE_DIR");if (directory.isEmpty()) return;
    require(QDir().mkpath(directory) && drawing.grab().save(QDir(directory).filePath(name)),
            "optional combined physical native selection capture must save");
}
std::string scenario(const QString& directory, bool reverse) {
    const auto f=fixture();const auto before=f.document->snapshot();
    const auto stem=reverse ? QStringLiteral("skylight-beam-column-slab-component")
                            : QStringLiteral("component-slab-column-beam-skylight");
    ModalGuard guard;MainWindow window(f.document,nullptr,QDir(directory).filePath(stem+"-library.json"));
    display(window,guard,f);select(window,f,reverse,guard);
    auto& drawing=canvas(window);published_owners(drawing,f);
    const auto after=window.document().snapshot();
    require(document_snapshot_digest(after)==document_snapshot_digest(before) &&
            after.entities()==before.entities() && after.assets()==before.assets(),
            "actual selection must preserve exact source, retained assets and metadata, revision and history");
    capture(drawing,stem+"-selected.png");guard.verify();
    const auto failure=frame_failure(drawing);
    return failure.empty() ? std::string{} : stem.toStdString()+": "+failure;
}

// Independent handwritten Gx(x,y)=(1+1.5*(x-1),y), Gy(x,y)=(x,1.5*y).
// The roof host does not participate in either transform.
std::array<Boundary,4> expected_plans(bool resized, bool depth) {
    if (!resized) return {rectangle(5,1,2,1),rectangle(1,0,3,3),rectangle(1,0,2,2),rectangle(2,2,4,2)};
    if (depth) return {rectangle(5,1.5,2,1.5),rectangle(1,0,3,4.5),rectangle(1,0,2,3),rectangle(2,3,4,3)};
    return {rectangle(7,1,3,1),rectangle(1,0,4.5,3),rectangle(1,0,3,2),rectangle(2.5,2,6,2)};
}
Boundary expected_mouth(bool resized, bool depth) {
    if (!resized) return rectangle(1.5,.5,1,1);
    return depth ? rectangle(1.5,.75,1,1.5) : rectangle(1.75,.5,1.5,1);
}
Boundary presented_boundary(Boundary boundary, bool site) {
    for (auto& edge:boundary) {edge.start=presented(edge.start,site);edge.end=presented(edge.end,site);}
    return boundary;
}
bool same_point(Vec2 a, Vec2 b) {return std::hypot(a.x-b.x,a.y-b.y)<1e-7;}
void exact_boundary(const std::vector<CanvasEntity>& scene, const std::string& id,
                    const Boundary& expected, bool complete=true) {
    Boundary actual;
    for (const auto& item:scene) if (item.id==QString::fromStdString(id)) {
        if (complete) require(item.holes.empty(),"ordinary solid owner retains no fabricated holes");
        actual.insert(actual.end(),item.segments.begin(),item.segments.end());
    }
    if (complete) require(actual.size()==expected.size(),"native owner publishes precisely four physical edges");
    for (const auto& edge:expected) {
        const auto found=std::find_if(actual.begin(),actual.end(),[&](const auto& candidate) {
            return std::abs(candidate.sweep_radians)<1e-7 &&
                ((same_point(candidate.start,edge.start) && same_point(candidate.end,edge.end)) ||
                 (same_point(candidate.start,edge.end) && same_point(candidate.end,edge.start)));
        });
        if (found==actual.end()) {
            std::cerr << "physical edge mismatch owner=" << id << " expected=" << raw_boundary(expected).dump()
                      << " remaining actual=" << raw_boundary(actual).dump() << '\n';
            throw std::runtime_error("native published physical edge matches the independent anchored transform");
        }
        actual.erase(found);
    }
}
void plans(const std::vector<CanvasEntity>& scene, const Fixture& f, bool resized, bool depth) {
    const auto expected=expected_plans(resized,depth);
    const std::array ids{f.component,f.slab,f.column,f.beam};
    for (std::size_t i=0;i<ids.size();++i)
        exact_boundary(scene,ids[i],presented_boundary(expected[i],f.site));
    // The genuine roof projection includes cavity-mouth and skylight-frame ink.
    exact_boundary(scene,f.roof,presented_boundary(expected_mouth(resized,depth),f.site),false);
    exact_boundary(scene,f.roof,presented_boundary(rectangle(0,0,20,20),f.site),false);
}
void frame(const PlanCanvas& drawing, const Fixture& f, bool resized=false, bool depth=false) {
    const auto actual=drawing.selectionAxisResizeCohortFrame();
    require(actual.has_value(),"all five semantic members expose their one analytical physical frame");
    const auto center=presented({resized && !depth ? 5.5 : 4,resized && depth ? 3.0 : 2.0},f.site);
    const double width=resized && !depth ? 9 : 6, height=resized && depth ? 6 : 4;
    require_near(actual->center.x,center.x,"common physical frame X");require_near(actual->center.y,center.y,"common physical frame Y");
    require_near(actual->width_metres,f.site ? height : width,"common displayed physical width");
    require_near(actual->depth_metres,f.site ? width : height,"common displayed physical depth");
    require_near(actual->rotation_radians,0,"common frame uses the published Plan/Site canvas axes");
    const auto grips=drawing.selectionAxisHandlePositions();
    require(grips.size()==4,"common physical frame paints four side grips");
    for (const auto& grip:grips)
        require(std::isfinite(grip.x()) && std::isfinite(grip.y()),"actual painted side grips retain finite native positions");
}
void roster(const PlanCanvas& drawing, const Fixture& f, Revision revision, bool resized=false, bool depth=false) {
    auto ids=drawing.selectedIds(),expected=ordinary_ids(f,false);ids.sort();expected.sort();
    const auto target=controls(drawing,f,revision,resized,depth).target;
    require(ids==expected && drawing.selectedRoofOpenings()==std::vector{target},
            "publication retains exact four-owner membership and one qualified typed child");
    for (const auto& id:expected) {
        const auto found=std::find_if(drawing.entities().begin(),drawing.entities().end(),[&](const auto& item) {
            return item.id==id && item.selected && !item.segments.empty();
        });
        require(found!=drawing.entities().end(),"every selected ordinary member has native published physical ink");
    }
    for (const auto& item:drawing.entities()) if (item.id==QString::fromStdString(f.roof))
        require(!item.selected,"qualified child's ordinary roof host remains unselected");
    const auto hit=drawing.selectedRoofOpeningAtModelPoint(presented(mouth_center(resized,depth),f.site));
    require(hit && *hit==target,"published mouth hit retains host/child/source qualification");
}
Json physical_structure(const DocumentSnapshot& snapshot, const Fixture& f, bool resized, bool depth) {
    const auto expected=expected_plans(resized,depth);
    AssemblyExpansionBudget budget;
    const auto expanded=expand_document_assembly_instance(snapshot.entities().at(f.component),snapshot.entities(),budget);
    require(expanded.profiles.size()==1,"component retains its one independent physical profile");
    const auto& profile=expanded.profiles.front();const auto& pose=profile.transform;
    require(profile.profile.outer.size()==4 && profile.profile.holes.empty(),"component retains complete four-edge profile");
    auto vertices=Json::array();
    for (std::size_t i=0;i<4;++i) {
        auto p=profile.profile.outer[i].start;
        if (profile.profile.planar_affine) {
            const auto& a=*profile.profile.planar_affine;const double x=p.x-a.source_origin.x,y=p.y-a.source_origin.y;
            p={a.target_origin.x+a.xx*x+a.xy*y,a.target_origin.y+a.yx*x+a.yy*y};
        }
        const double py=pose.mirrored_y ? -p.y : p.y;
        const double x=pose.translation_m.x+pose.scale*(p.x*std::cos(pose.rotation_radians)-py*std::sin(pose.rotation_radians));
        const double y=pose.translation_m.y+pose.scale*(p.x*std::sin(pose.rotation_radians)+py*std::cos(pose.rotation_radians));
        require_near(x,expected[0][i].start.x,"component physical X uses cohort anchor");
        require_near(y,expected[0][i].start.y,"component physical Y uses cohort anchor");
        require_near(profile.profile.outer[i].sweep_radians,0,"component straight edges stay straight");vertices.push_back({x,y});
    }
    require_near(pose.translation_m.z+pose.scale*pose.vertical_scale*profile.profile.elevation_m,1,"component base Z preserved");
    require_near(pose.scale*pose.vertical_scale*profile.profile.height_m,1.5,"component height preserved");
    const auto instance=decode_document_assembly_instance(snapshot.entities().at(f.component));
    require(instance.instance.root_transform && !instance.instance.placement,"component remains an independent root");
    require_near(instance.instance.root_transform->translation_m.z,.75,"component root Z preserved");
    Slab slab;std::string error;
    require(read_document_slab(snapshot.entities().at(f.slab),slab,error),"result slab remains typed editable authoring");
    require(slab.boundary.size()==4 && slab.holes.empty(),"slab retains its exact complete solid boundary");
    auto slab_physical=slab.boundary;
    if (slab.planar_affine) {
        const auto& a=*slab.planar_affine;
        const auto mapped=[&](Vec2 p) {const double x=p.x-a.source_origin.x,y=p.y-a.source_origin.y;
            return Vec2{a.target_origin.x+a.xx*x+a.xy*y,a.target_origin.y+a.yx*x+a.yy*y};};
        for (auto& edge:slab_physical) {edge.start=mapped(edge.start);edge.end=mapped(edge.end);}
    }
    for (std::size_t i=0;i<4;++i) {
        require_near(slab_physical[i].start.x,expected[1][i].start.x,"slab physical X uses cohort anchor");
        require_near(slab_physical[i].start.y,expected[1][i].start.y,"slab physical Y uses cohort anchor");
        require_near(slab_physical[i].end.x,expected[1][i].end.x,"slab edge end X");
        require_near(slab_physical[i].end.y,expected[1][i].end.y,"slab edge end Y");
        require_near(slab.boundary[i].sweep_radians,0,"slab straight edges stay straight");
    }
    require_near(slab.elevation,1.25,"slab base Z preserved");require_near(slab.thickness,.2,"slab physical thickness preserved");
    const auto& column_entity=snapshot.entities().at(f.column);const auto& beam_entity=snapshot.entities().at(f.beam);
    require(column_entity.properties.at("version")==1 && column_entity.properties.at("form")=="rectangular_column" &&
            beam_entity.properties.at("version")==1 && beam_entity.properties.at("form")=="straight_beam",
            "structural authoring retains canonical rectangular-column and horizontal-beam schemas");
    const auto column=std::get<RectangularColumn>(decode_building_entity(column_entity));
    const auto beam=std::get<Beam>(decode_building_entity(beam_entity));
    require_near(column.base_center.x,resized && !depth ? 2.5 : 2,"column centre X uses cohort anchor");
    require_near(column.base_center.y,resized && depth ? 1.5 : 1,"column centre Y uses cohort anchor");
    require_near(column.width,resized && !depth ? 3 : 2,"column width");require_near(column.depth,resized && depth ? 3 : 2,"column depth");
    require_near(column.base_center.z,.4,"column base Z preserved");require_near(column.height,3,"column height preserved");
    require_near(column.rotation_radians,0,"column orientation preserved");
    require_near(beam.start.x,resized && !depth ? 2.5 : 2,"beam start X uses cohort anchor");
    require_near(beam.end.x,resized && !depth ? 8.5 : 6,"beam end X uses cohort anchor");
    require_near(beam.start.y,resized && depth ? 4.5 : 3,"beam start Y uses cohort anchor");
    require_near(beam.end.y,resized && depth ? 4.5 : 3,"beam end Y uses cohort anchor");
    require_near(beam.start.z,2.2,"beam start Z preserved");require_near(beam.end.z,2.2,"beam end Z preserved");
    require_near(beam.width,resized && depth ? 3 : 2,"beam transverse width uses depth factor");require_near(beam.depth,.4,"beam section height preserved");
    require_near(beam.up.x,0,"beam up X preserved");require_near(beam.up.y,0,"beam up Y preserved");require_near(beam.up.z,1,"beam up Z preserved");
    const auto roof=std::get<SlopedRoofPanel>(decode_roof_entity(snapshot.entities().at(f.roof)));
    require(roof.openings.size()==1 && roof.openings.front().id==f.child && roof.openings.front().skylight,
            "result retains the selected genuine typed skylight identity");
    const auto& child=roof.openings.front();const auto mouth=expected_mouth(resized,depth);
    require_near(child.x,mouth[0].start.x,"typed mouth left uses cohort anchor");require_near(child.y,mouth[0].start.y,"typed mouth bottom uses cohort anchor");
    require_near(child.width,mouth[0].end.x-mouth[0].start.x,"typed mouth width");
    require_near(child.depth,mouth[1].end.y-mouth[1].start.y,"typed mouth depth");require_near(child.rotation_radians,0,"typed mouth angle preserved");
    require(!make_slab(slab).IsNull() && !make_building_shape(column).IsNull() &&
            !make_building_shape(beam).IsNull() && !make_building_shape(roof).IsNull(),"all physical owners still manufacture genuine native solids");
    return {{"component",vertices},{"slab",raw_boundary(slab_physical)},
        {"column",{column.base_center.x,column.base_center.y,column.width,column.depth}},
        {"beam",{beam.start.x,beam.start.y,beam.end.x,beam.end.y,beam.width}},
        {"mouth",{child.x,child.y,child.width,child.depth}}};
}
void same_structure(const Json& a, const Json& b) {
    if (a.is_number() && b.is_number()) {require_near(a.get<double>(),b.get<double>(),"selection order/Open retains physical structure");return;}
    require(a.type()==b.type() && a.size()==b.size(),"selection order/Open retains semantic structure");
    if (a.is_object()) for (auto it=a.begin();it!=a.end();++it) same_structure(it.value(),b.at(it.key()));
    else if (a.is_array()) for (std::size_t i=0;i<a.size();++i) same_structure(a.at(i),b.at(i));
    else require(a==b,"selection order/Open retains semantic values");
}
void preserved(const DocumentSnapshot& before, const DocumentSnapshot& after, const Fixture& f, bool retired=false) {
    require(before.assets()==after.assets() && !after.assets().empty(),"all retained assets and their opaque envelopes remain exact");
    require(before.entities().size()==after.entities().size()+(retired ? 1 : 0),"ordinary combined resize changes no unrelated roster member");
    for (const auto& [id,entity]:before.entities()) {
        if (retired && id==f.dimension) {require(!after.entities().contains(id),"only explicitly reviewed dimension is retired");continue;}
        if (id!=f.component && id!=f.slab && id!=f.column && id!=f.beam && id!=f.roof && id!=f.catalog)
            require(after.entities().at(id)==entity,"unselected context and unrelated carriers remain exact");
        else if (id==f.slab) {
            auto original_extensions=entity.extensions,actual_extensions=after.entities().at(id).extensions;
            const auto key=std::string(slab_geometry_derivations_key);
            if (after.entities().at(id)!=entity)
                require(actual_extensions.contains(key) && actual_extensions.at(key).at("operations").size()==1,
                        "one native slab resize retains its canonical geometry derivation");
            original_extensions.erase(key);actual_extensions.erase(key);
            require(original_extensions.dump()==actual_extensions.dump(),"slab retains every opaque extension beside its authored derivation");
        } else if (id!=f.roof) require(after.entities().at(id).extensions==entity.extensions,"selected owners and catalog retain opaque metadata");
        if (id==f.component || id==f.slab || id==f.column || id==f.beam || id==f.roof) {
            for (const auto* key:{"property_id","building_id","floor_id","layer_id"})
                require(after.entities().at(id).properties.at(key)==entity.properties.at(key),"all drawing contexts remain exact");
            if (f.site) require(after.entities().at(id).properties.at("presentation_frame")==entity.properties.at("presentation_frame"),
                                "every Site owner retains its explicit building presentation frame");
        }
    }
    auto old_roof=before.entities().at(f.roof),new_roof=after.entities().at(f.roof);
    old_roof.properties.erase("roof_openings");new_roof.properties.erase("roof_openings");
    old_roof.extensions.erase("roof_opening_input");new_roof.extensions.erase("roof_opening_input");
    require(old_roof==new_roof && old_roof.properties.dump()==new_roof.properties.dump() && old_roof.extensions.dump()==new_roof.extensions.dump(),
            "unselected roof host body, pose, thickness, context and opaque metadata stay exact");
    auto old_child=before.entities().at(f.roof).properties.at("roof_openings").at(0);
    auto new_child=after.entities().at(f.roof).properties.at("roof_openings").at(0);
    for (const auto* key:{"x_m","y_m","width_m","depth_m","rotation_rad"}) {old_child.erase(key);new_child.erase(key);}
    require(old_child.dump()==new_child.dump(),"selected skylight retains its complete profile and opaque child metadata");
    const auto original=AssemblyModel::from_json(before.entities().at(f.catalog).properties.at("model"));
    const auto resulting=AssemblyModel::from_json(after.entities().at(f.catalog).properties.at("model"));
    for (const auto& type:original.types()) {
        const auto found=std::find_if(resulting.types().begin(),resulting.types().end(),[&](const auto& item){return item.id==type.id;});
        require(found!=resulting.types().end() && *found==type,"component's original shared catalog type stays exact");
    }
}
void unchanged(MainWindow& window, const DocumentSnapshot& before) {
    const auto after=window.document().snapshot();
    require(document_snapshot_digest(after)==document_snapshot_digest(before) && after.entities()==before.entities() &&
            after.assets()==before.assets(),"preview/refusal preserves exact source, revision, saved revision, history and assets");
}
void wait(MainWindow& window, const ModalGuard& guard, const std::function<bool()>& predicate, const char* message) {
    QElapsedTimer deadline;deadline.start();
    while (!predicate() && deadline.elapsed()<10000) {QApplication::processEvents(QEventLoop::AllEvents,20);guard.verify();}
    guard.verify();if (!predicate()) throw std::runtime_error(std::string(message)+": "+window.lastError().toStdString());
}
QImage output(PlanCanvas& drawing) {
    QImage image(drawing.size(),QImage::Format_ARGB32_Premultiplied);QPainter painter(&image);
    drawing.renderSceneAt(painter,image.rect(),drawing.viewScale(),drawing.viewCenter(),Qt::white);painter.end();return image;
}
void mouse(PlanCanvas& drawing, QEvent::Type kind, QPointF pixel) {
    QMouseEvent event(kind,pixel,drawing.mapToGlobal(pixel.toPoint()),
        kind==QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
        kind==QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton,Qt::ShiftModifier);
    QApplication::sendEvent(&drawing,&event);
}
void no_preview(const PlanCanvas& drawing) {
    require(!drawing.entityTransformPreviewPending() && drawing.entityTransformPreview().empty() &&
            !drawing.roofOpeningPreviewPending() && !drawing.roofOpeningPreviewBoundary() && !drawing.roofOpeningPreviewEdit() &&
            !drawing.roofOpeningGroupPreviewPending() && !drawing.roofOpeningGroupPreviewBoundary() &&
            !drawing.roofOpeningGroupPreviewEdit() && drawing.roofOpeningGroupPreviewControls().empty(),
            "release/review/refusal consumes every provisional ordinary and typed-child preview ticket");
}
QPointF completed_preview(MainWindow& window, const Fixture& f, const DocumentSnapshot& before,
                          const ModalGuard& guard, bool depth) {
    auto& drawing=canvas(window);frame(drawing,f);roster(drawing,f,before.revision());
    const auto committed=output(drawing);const auto generation=drawing.roofOpeningControlsGeneration();
    const auto handles=drawing.selectionAxisHandlePositions();QPointF press,delta;
    if (!f.site && !depth) {
        press=*std::max_element(handles.begin(),handles.end(),[](QPointF a,QPointF b){return a.x()<b.x();});
        delta={3*drawing.viewScale(),0};
    } else if (!depth || !f.site) {
        press=*std::min_element(handles.begin(),handles.end(),[](QPointF a,QPointF b){return a.y()<b.y();});
        delta={0,-(depth ? 2 : 3)*drawing.viewScale()};
    } else {
        press=*std::min_element(handles.begin(),handles.end(),[](QPointF a,QPointF b){return a.x()<b.x();});
        delta={-2*drawing.viewScale(),0};
    }
    const auto release=press+delta;
    require(drawing.rect().contains(press.toPoint()) && drawing.rect().contains(release.toPoint()),"actual side gesture remains inside the native canvas");
    mouse(drawing,QEvent::MouseButtonPress,press);unchanged(window,before);frame(drawing,f);
    mouse(drawing,QEvent::MouseMove,release);
    require(drawing.entityTransformPreviewPending(),"actual controller side gesture starts asynchronous native manufacture");
    unchanged(window,before);require(output(drawing)==committed,"pending preview stays outside committed print/export output");
    wait(window,guard,[&]{return !drawing.entityTransformPreviewPending() && !drawing.entityTransformPreview().empty();},
         "complete five-member manufactured detached preview finishes");
    plans(drawing.entityTransformPreview(),f,true,depth);plans(drawing.entities(),f,false,depth);
    QStringList preview_ids;for (const auto& item:drawing.entityTransformPreview()) if (!preview_ids.contains(item.id)) preview_ids.push_back(item.id);
    auto expected=ordinary_ids(f,false);expected.push_back(QString::fromStdString(f.roof));preview_ids.sort();expected.sort();
    require(preview_ids==expected,"detached preview contains precisely four ordinary owners and the child's actual roof presentation owner");
    unchanged(window,before);frame(drawing,f);roster(drawing,f,before.revision());
    require(drawing.roofOpeningControlsGeneration()==generation,"detached manufacture retains the captured complete child-controls generation");
    require(output(drawing)==committed,"admitted detached preview stays outside committed print/export output");
    return release;
}
void mixed42_command(const DocumentSnapshot& after) {
    const auto& command=after.history().back().boundary_constraint_changes;
    require(command && command->mixed_selection_edit_completion && command->mixed_selection_edit_intent.at("version")==42,
            "combined side release publishes sealed mixed42 authority");
    require(command_to_json(*command).at("version")==52,"combined side release retains canonical enclosing command52");
    require(ProjectStore::required_format_version(after)==265 && ProjectStore::required_semantic_format_version(after)==265,
            "canonical retained combined command requires native and semantic reader265");
}
void fresh_open(const QString& directory, const QString& stem, const DocumentSnapshot& saved,
                const Fixture& f, const ModalGuard& guard, bool reverse, bool resized, bool depth) {
    MainWindow reopened({},nullptr,QDir(directory).filePath(stem+"-reopened-library.json"));
    require(reopened.openProject(QDir(directory).filePath(stem+".bldproj")) && reopened.document().is_editable(),
            "ordinary fresh Open acquires actual editable writer ownership");
    display(reopened,guard,f);const auto loaded=reopened.document().snapshot();
    require(document_snapshot_digest(loaded)==document_snapshot_digest(saved) && loaded.entities()==saved.entities() && loaded.assets()==saved.assets(),
            "ordinary fresh Open retains exact saved entities, assets, revisions and complete history");
    require(ProjectStore::required_format_version(loaded)==265 && ProjectStore::required_semantic_format_version(loaded)==265,
            "reader265 floor survives applied, undone and redone retained command history");
    auto* undo=reopened.findChild<QAction*>("undoCommand");
    require(undo && !undo->isEnabled() && !reopened.undoCommand() && !reopened.redoCommand(),
            "ordinary fresh Open starts a new Undo/Redo session despite retained history");
    unchanged(reopened,loaded);physical_structure(loaded,f,resized,depth);select(reopened,f,reverse,guard,resized,depth);
    auto& drawing=canvas(reopened);plans(drawing.entities(),f,resized,depth);frame(drawing,f,resized,depth);
    roster(drawing,f,loaded.revision(),resized,depth);no_preview(drawing);capture(drawing,stem+"-reopened.png");guard.verify();
}
Json gesture_scenario(const QString& directory, bool reverse, bool depth, bool site) {
    const auto f=fixture(false,site);
    const auto stem=QString(site ? "site-" : "plan-")+(depth ? "depth-" : "width-")+(reverse ? "child-first" : "component-first");
    std::cout<<"[physical desktop] "<<stem.toStdString()<<'\n'<<std::flush;
    ModalGuard guard;MainWindow window(f.document,nullptr,QDir(directory).filePath(stem+"-library.json"));display(window,guard,f);
    select(window,f,reverse,guard);auto& drawing=canvas(window);const auto before=window.document().snapshot();
    require(ProjectStore::required_format_version(before)<265 && ProjectStore::required_semantic_format_version(before)<265,
            "opaque command-shaped catalog metadata does not raise reader265 before canonical authoring exists");
    physical_structure(before,f,false,depth);plans(drawing.entities(),f,false,depth);const auto committed=output(drawing);
    capture(drawing,stem+"-selected.png");const auto release=completed_preview(window,f,before,guard,depth);capture(drawing,stem+"-preview.png");
    mouse(drawing,QEvent::MouseButtonRelease,release);
    wait(window,guard,[&]{return window.document().revision()==before.revision()+1 && !drawing.entityTransformPreviewPending();},
         "one actual side release publishes all five members exactly once");
    const auto after=window.document().snapshot();mixed42_command(after);
    require(after.history().size()==before.history().size()+1,"one release creates exactly one retained combined history event");
    no_preview(drawing);const auto result=physical_structure(after,f,true,depth);preserved(before,after,f);
    plans(drawing.entities(),f,true,depth);frame(drawing,f,true,depth);roster(drawing,f,after.revision(),true,depth);
    require(output(drawing)!=committed,"release publishes changed physical geometry into committed print/export output");capture(drawing,stem+"-committed.png");
    const auto save=[&](const QString& state) {
        const auto current=window.document().snapshot();require(window.saveProjectAs(QDir(directory).filePath(stem+state+".bldproj")),"native combined state saves");
        const auto saved=window.document().snapshot();require(saved.entities()==current.entities() && saved.assets()==current.assets(),"Save retains the current exact authoring graph and assets");
        require(ProjectStore::required_format_version(saved)==265 && ProjectStore::required_semantic_format_version(saved)==265,"saved retained command preserves reader265 floor");return saved;
    };
    const auto applied=save("-applied");
    require(window.undoCommand() && window.document().snapshot().entities()==before.entities() && window.document().snapshot().assets()==before.assets(),
            "one Undo restores exact five-member source authoring and retained assets");
    plans(drawing.entities(),f,false,depth);frame(drawing,f);roster(drawing,f,window.document().revision());
    require(output(drawing)==committed,"one Undo restores exact original committed physical output");
    const auto undone=save("-undone");
    require(window.redoCommand() && window.document().snapshot().entities()==after.entities() && window.document().snapshot().assets()==after.assets(),
            "one Redo restores exact complete five-member result");
    plans(drawing.entities(),f,true,depth);frame(drawing,f,true,depth);roster(drawing,f,window.document().revision(),true,depth);
    const auto redone=save("-redone");
    require(window.createNewProject(),"release all saved native writer leases before ordinary editable Open");
    fresh_open(directory,stem+"-applied",applied,f,guard,reverse,true,depth);
    fresh_open(directory,stem+"-undone",undone,f,guard,reverse,false,depth);
    fresh_open(directory,stem+"-redone",redone,f,guard,reverse,true,depth);
    guard.verify();return result;
}
void seed_history(Fixture& f) {
    const auto source=f.document->snapshot();auto retained=source.entities().at(f.component);
    retained.extensions["retained_history_fixture"]=true;
    f.document->apply(ApplyEntityChanges{source.revision(),{EntityChange::upsert(std::move(retained))},{},"Original combined history authority"});
}
DocumentSnapshot replace_history(MainWindow& window, const DocumentSnapshot& before) {
    require(before.revision()>0,"history-only stale fixture has a real retained event");
    auto replacement=Document::fork_at_revision(before,before.revision()-1);std::vector<EntityChange> changes;
    for (const auto& [id,entity]:before.entities()) {(void)id;changes.push_back(EntityChange::upsert(entity));}
    replacement.apply(ApplyEntityChanges{replacement.revision(),std::move(changes),{},"Independently replaced combined history authority"});
    const auto expected=replacement.snapshot();
    require(expected.document_id()==before.document_id() && expected.revision()==before.revision() &&
            expected.entities()==before.entities() && expected.assets()==before.assets() &&
            expected.saved_revision_optional()==before.saved_revision_optional(),
            "history replacement retains exact displayed identity, revision, saved revision, geometry and assets");
    require(document_snapshot_digest(expected)!=document_snapshot_digest(before),"history-only replacement changes full captured source digest");
    window.document()=std::move(replacement);return expected;
}
void settle_without_publication(MainWindow& window, const DocumentSnapshot& expected, const ModalGuard& guard) {
    QElapsedTimer deadline;deadline.start();
    do {QApplication::processEvents(QEventLoop::AllEvents,20);guard.verify();unchanged(window,expected);no_preview(canvas(window));}
    while (deadline.elapsed()<350);
}
enum class StaleAuthority { selection, viewport, controls, history };
void stale_release(const QString& directory, StaleAuthority authority) {
    auto f=fixture();if (authority==StaleAuthority::history) seed_history(f);
    const auto stem=QStringLiteral("stale-")+ (authority==StaleAuthority::selection ? "selection" :
        authority==StaleAuthority::viewport ? "viewport" : authority==StaleAuthority::controls ? "controls" : "history");
    std::cout<<"[physical desktop] "<<stem.toStdString()<<'\n'<<std::flush;
    ModalGuard guard;MainWindow window(f.document,nullptr,QDir(directory).filePath(stem+"-library.json"));display(window,guard,f);
    select(window,f,false,guard);auto& drawing=canvas(window);const auto before=window.document().snapshot();
    const auto committed=output(drawing);const auto center=drawing.viewCenter();const auto scale=drawing.viewScale();
    const auto generation=drawing.roofOpeningControlsGeneration();
    const auto release=completed_preview(window,f,before,guard,false);capture(drawing,stem+"-preview.png");auto expected=before;
    if (authority==StaleAuthority::selection) {
        require(window.selectEntity(QString::fromStdString(f.component)),"stale authority uses actual public selection change");
        require(drawing.selectedIds()==QStringList{QString::fromStdString(f.component)} && drawing.selectedRoofOpenings().empty(),
                "stale selection removes every physical lane and qualified child from captured cohort");
    } else if (authority==StaleAuthority::viewport) {
        drawing.zoomBy(1.2);require(drawing.viewScale()!=scale,"stale viewport changes actual captured view scale");
    } else if (authority==StaleAuthority::controls) {
        const auto source_controls=drawing.roofOpeningControls();drawing.setRoofOpeningControls(source_controls);
        require(drawing.roofOpeningControlsGeneration()!=generation,"identical republished controls still change captured generation");
        roster(drawing,f,before.revision());
    } else expected=replace_history(window,before);
    QApplication::processEvents();guard.verify();unchanged(window,expected);
    mouse(drawing,QEvent::MouseButtonRelease,release);
    wait(window,guard,[&]{return !drawing.entityTransformPreviewPending() && drawing.entityTransformPreview().empty();},
         "stale release retires complete five-member candidate");
    settle_without_publication(window,expected,guard);physical_structure(window.document().snapshot(),f,false,false);
    drawing.setViewTransform(center,scale);select(window,f,false,guard);plans(drawing.entities(),f,false,false);frame(drawing,f);
    require(output(drawing)==committed,"stale refusal retains exact original committed output at restored view");
    capture(drawing,stem+"-refused.png");guard.verify();
}
enum class ReviewAction { cancel, retire, stale_controls, stale_history };
// Use the real nested modal event loop and real dimension decisions. A callback
// assertion rejects the dialog so unattended native qualification cannot hang.
class DimensionReviewDriver {
public:
    DimensionReviewDriver(MainWindow& window, const Fixture& f, const DocumentSnapshot& before,
                          const QImage& committed, ReviewAction action, std::uint64_t source_generation)
        : m_window(window),m_fixture(f),m_before(before),m_output(committed),m_action(action),m_generation(source_generation) {
        m_timer.setInterval(10);QObject::connect(&m_timer,&QTimer::timeout,[this] {
            auto* dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget());if (!dialog) return;
            try {
                require(dialog->objectName()==QStringLiteral("AssemblyProfileDimensionReviewDialog") ||
                        dialog->windowTitle()==QStringLiteral("Review component dimensions"),
                        "combined physical release opens the actual component profile dimension review");
                require(++m_reviews==1,"one affected source profile carrier enters exactly one actual review");
                auto& drawing=canvas(m_window);no_preview(drawing);unchanged(m_window,m_before);
                require(output(drawing)==m_output,"nested review exposes unchanged committed output with provisional tickets consumed");
                roster(drawing,m_fixture,m_before.revision());plans(drawing.entities(),m_fixture,false,false);
                physical_structure(m_window.document().snapshot(),m_fixture,false,false);
                require(drawing.roofOpeningControlsGeneration()>m_generation,
                        "review consumes the provisional publication and rebinds source-qualified child controls with fresh generation");
                auto* list=dialog->findChild<QListWidget*>();
                require(list && list->count()==1 && list->item(0)->toolTip()==QString::fromStdString(m_fixture.dimension),
                        "actual review identifies the original-topology profile carrier");
                auto* buttons=dialog->findChild<QDialogButtonBox*>();
                require(buttons && buttons->button(QDialogButtonBox::Cancel) && buttons->button(QDialogButtonBox::Ok),
                        "actual review exposes real Cancel and Apply buttons");
                require(!buttons->button(QDialogButtonBox::Ok)->isEnabled(),"actual Apply requires an explicit dimension decision");
                if (m_action==ReviewAction::cancel) {
                    buttons->button(QDialogButtonBox::Cancel)->click();
                    require(dialog->result()==QDialog::Rejected && !dialog->isVisible(),"actual Cancel rejects the complete physical cohort");
                    return;
                }
                auto* remove=dialog->findChild<QPushButton*>("dimensionRemove");
                if (!remove) for (auto* button:dialog->findChildren<QPushButton*>())
                    if (button->text()==QStringLiteral("Remove dimension")) {remove=button;break;}
                require(remove && remove->isEnabled(),"actual review offers an explicit enabled Remove dimension decision");
                remove->click();unchanged(m_window,m_before);no_preview(drawing);
                require(output(drawing)==m_output && buttons->button(QDialogButtonBox::Ok)->isEnabled(),
                        "explicit removal enables Apply while source and output remain unchanged");
                if (m_action==ReviewAction::stale_controls || m_action==ReviewAction::stale_history) {
                    // Accepted callbacks run before the controller resumes; this
                    // exercises its final release fence after a genuine decision.
                    QObject::connect(dialog,&QDialog::accepted,dialog,[this] {
                        try {
                            if (m_action==ReviewAction::stale_history) m_expected=replace_history(m_window,m_before);
                            else {
                                auto& drawing=canvas(m_window);const auto generation=drawing.roofOpeningControlsGeneration();
                                const auto source_controls=drawing.roofOpeningControls();drawing.setRoofOpeningControls(source_controls);
                                require(drawing.roofOpeningControlsGeneration()!=generation,"accepted callback replaces actual controls generation");
                                roster(drawing,m_fixture,m_before.revision());m_expected=m_window.document().snapshot();
                            }
                        } catch (const std::exception& error) {m_failure=error.what();}
                    });
                }
                buttons->button(QDialogButtonBox::Ok)->click();
                require(dialog->result()==QDialog::Accepted && !dialog->isVisible(),"explicit retirement accepts only through actual Apply");
            } catch (const std::exception& error) {if (m_failure.empty()) m_failure=error.what();dialog->reject();}
              catch (...) {if (m_failure.empty()) m_failure="unknown combined native review callback failure";dialog->reject();}
        });m_timer.start();
    }
    void verify() const {
        if (!m_failure.empty()) throw std::runtime_error(m_failure);
        require(m_reviews==1,"combined release actually executes exactly one native dimension review");
        if (m_action==ReviewAction::stale_controls || m_action==ReviewAction::stale_history)
            require(m_expected.has_value(),"accepted stale callback actually replaces its captured authority");
    }
    DocumentSnapshot expected() const {return m_expected.value_or(m_before);}
private:
    MainWindow& m_window;const Fixture& m_fixture;const DocumentSnapshot& m_before;const QImage& m_output;
    ReviewAction m_action;std::uint64_t m_generation;QTimer m_timer;int m_reviews=0;
    std::string m_failure;std::optional<DocumentSnapshot> m_expected;
};
void dimension_review(const QString& directory, ReviewAction action) {
    auto f=fixture(true);if (action==ReviewAction::stale_history) seed_history(f);
    const auto stem=QStringLiteral("review-")+(action==ReviewAction::cancel ? "cancel" : action==ReviewAction::retire ? "remove" :
        action==ReviewAction::stale_controls ? "stale-controls" : "stale-history");
    std::cout<<"[physical desktop] "<<stem.toStdString()<<'\n'<<std::flush;
    ModalGuard guard;MainWindow window(f.document,nullptr,QDir(directory).filePath(stem+"-library.json"));display(window,guard,f);
    select(window,f,false,guard);auto& drawing=canvas(window);const auto before=window.document().snapshot();
    const auto decoded=decode_boundary_dimension_entity(before.entities().at(f.dimension));
    require(decoded.supported() && decoded.dimension->kind==BoundaryDimensionKind::assembly_profile,
            "review fixture admits a supported real original-topology component dimension");
    require_near(decoded.dimension->resolve(before).segment_length(),2,"review fixture source edge measures exactly two metres");
    const auto committed=output(drawing);const auto generation=drawing.roofOpeningControlsGeneration();
    const auto release=completed_preview(window,f,before,guard,false);capture(drawing,stem+"-preview.png");guard.pause();
    DimensionReviewDriver review(window,f,before,committed,action,generation);
    mouse(drawing,QEvent::MouseButtonRelease,release);review.verify();
    QApplication::setActiveWindow(&window);guard.resume();
    if (action!=ReviewAction::retire) {
        const auto expected=review.expected();
        wait(window,guard,[&]{return !drawing.entityTransformPreviewPending() && drawing.entityTransformPreview().empty();},
             "Cancel/stale accepted review retires the entire five-member candidate");
        settle_without_publication(window,expected,guard);physical_structure(window.document().snapshot(),f,false,false);
        preserved(before,window.document().snapshot(),f);require(output(drawing)==committed,"Cancel/stale review preserves exact committed output");
        require_near(decoded.dimension->resolve(window.document().snapshot()).segment_length(),2,"Cancel/stale review retains original live profile dimension");
        if (action!=ReviewAction::cancel) require(!window.lastError().isEmpty(),"accepted stale review reports its refused publication");
    } else {
        wait(window,guard,[&]{return window.document().revision()==before.revision()+1 && !drawing.entityTransformPreviewPending();},
             "reviewed dimension retirement publishes all five members in exactly one revision");
        const auto after=window.document().snapshot();mixed42_command(after);no_preview(drawing);
        require(after.history().size()==before.history().size()+1 && !after.entities().contains(f.dimension),
                "one reviewed event combines complete physical resize and explicit carrier retirement");
        physical_structure(after,f,true,false);preserved(before,after,f,true);plans(drawing.entities(),f,true,false);
        frame(drawing,f,true,false);roster(drawing,f,after.revision(),true,false);
        require(output(drawing)!=committed,"reviewed Apply publishes changed committed physical output");
        require(window.undoCommand() && window.document().snapshot().entities()==before.entities() && window.document().snapshot().assets()==before.assets(),
                "one reviewed Undo restores all five original members and dimension exactly");
        require(window.redoCommand() && window.document().snapshot().entities()==after.entities() && window.document().snapshot().assets()==after.assets(),
                "one reviewed Redo restores all five resized members and explicit dimension retirement exactly");
        require(window.saveProjectAs(QDir(directory).filePath(stem+".bldproj")),"reviewed combined retirement saves");
        const auto saved=window.document().snapshot();require(window.createNewProject(),"release reviewed saved writer lease before ordinary Open");
        fresh_open(directory,stem,saved,f,guard,false,true,false);
    }
    review.verify();guard.verify();
}
} // namespace

int main(int argc, char** argv) {
    sketch::testing::noninteractive_errors();qputenv("QT_QPA_PLATFORM","offscreen");
    QStandardPaths::setTestModeEnabled(true);QApplication application(argc,argv);QTemporaryDir settings;
    QCoreApplication::setOrganizationName("VertexTests");
    QCoreApplication::setApplicationName("MixedComponentPhysicalResize-"+QUuid::createUuid().toString(QUuid::WithoutBraces));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,settings.path());
    QSettings::setPath(QSettings::IniFormat,QSettings::SystemScope,settings.path());
    try {
        require(settings.isValid(),"private temporary settings directory must exist");
        const auto font_id=QFontDatabase::addApplicationFont(":/fonts/Inter.ttf");require(font_id>=0,"bundled Inter resource must load");
        const auto families=QFontDatabase::applicationFontFamilies(font_id);require(!families.isEmpty(),"bundled Inter resource must expose a family");
        application.setFont(QFont(families.front(),10));
        QTemporaryDir directory;require(directory.isValid(),"private fixture directory must exist");
        // Keep the root's fast RED invocation genuinely limited to the two
        // actual ordered frame scenarios. Default execution qualifies delivery.
        const bool frame_only=application.arguments().contains("--frame-only");
        const bool site_only=application.arguments().contains("--site-only");
        const bool review_only=application.arguments().contains("--review-only");
        const bool stale_only=application.arguments().contains("--stale-only");
        const auto forward=scenario(directory.path(),false);
        const auto reverse=scenario(directory.path(),true);
        if (!forward.empty() || !reverse.empty())
            throw std::runtime_error(forward+(forward.empty() || reverse.empty() ? "" : "\n")+reverse);
        if (!frame_only) {
            if (!review_only && !stale_only) for (const bool site:{false,true}) for (const bool depth:{false,true}) {
                if (site_only && !site) continue;
                const auto forward=gesture_scenario(directory.path(),false,depth,site);
                const auto reverse=gesture_scenario(directory.path(),true,depth,site);same_structure(forward,reverse);
            }
            if (!site_only && !stale_only) for (const auto action:{ReviewAction::cancel,ReviewAction::retire,ReviewAction::stale_controls,ReviewAction::stale_history})
                dimension_review(directory.path(),action);
            if (!site_only && !review_only) for (const auto authority:{StaleAuthority::selection,StaleAuthority::viewport,StaleAuthority::controls,StaleAuthority::history})
                stale_release(directory.path(),authority);
        }
        std::cout<<"mixed_component_physical_resize_desktop_tests"<<(frame_only ? " frame checks" : " native lifecycle")<<" passed\n";return 0;
    } catch (const std::exception& error) {
        std::cerr<<"mixed_component_physical_resize_desktop_tests: "<<error.what()<<'\n';return 1;
    }
}
