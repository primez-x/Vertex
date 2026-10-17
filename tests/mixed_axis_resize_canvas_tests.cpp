#include "../src/desktop/plan_canvas.hpp"
#include "support/noninteractive_errors.hpp"

#include <QApplication>
#include <QFontDatabase>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QUuid>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
using namespace sketch;
using namespace sketch::desktop;
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
bool close(double a, double b) { return std::abs(a-b) < 1e-8; }
void mouse(PlanCanvas& canvas, QEvent::Type type, QPointF point) {
    QMouseEvent event(type, point, canvas.mapToGlobal(point.toPoint()),
        type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
        type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&canvas, &event);
}
QImage render(PlanCanvas& canvas, bool output = false) {
    QImage image(canvas.size(), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::white);
    QPainter painter(&image);
    if (output) canvas.renderScene(painter, image.rect(), true, Qt::white);
    else canvas.render(&painter);
    return image;
}
CanvasEntity rectangle(QString id, QString key, double left, double bottom,
                       double right, double top) {
    CanvasEntity entity;
    entity.id = std::move(id); entity.presentation_key = std::move(key);
    entity.type = "assembly_instance";
    entity.segments = {{{left,bottom},{right,bottom},0}, {{right,bottom},{right,top},0},
                       {{right,top},{left,top},0}, {{left,top},{left,bottom},0}};
    entity.filled = true; entity.fill_color = QColor(220,70,30);
    entity.fill_opacity = 1.0; entity.hatch_pattern = "solid";
    return entity;
}
struct Fixture {
    PlanCanvas canvas;
    std::vector<CanvasEntity> source{
        rectangle("component", "baseline", -2,-1,-1,0),
        rectangle("component", "proposed", -2,0,-1,1)};
    std::vector<CanvasRoofOpeningControls> children;
    CanvasSelectionFrame frame{{0,0},0,4,2};
    std::uint64_t serial{};
    int starts{}, previews{}, commits{}, individual_commits{};
    double sx{1}, sy{1};
    Vec2 anchor{};
    QPointF grip, end;
    bool inspect_release{};

    explicit Fixture(bool two_children = false, bool child_first = false) {
        children.push_back({{"actual-roof","mouth-a",37}, {{1.5,0},{1,0},{0,1},1,1,0,1.2,1}, "presented-roof"});
        if (two_children)
            children.push_back({{"actual-roof","mouth-b",37}, {{0.5,0.3},{1,0},{0,1},0.5,0.5,0,1.2,1}, "presented-roof"});
        canvas.resize(640,480); canvas.setGridEnabled(false); canvas.setSnapEnabled(false);
        canvas.setMetricUnits(true);
        canvas.setOverviewMapEnabled(false); canvas.setCanvasBackground(Qt::white);
        canvas.setEntities(source); canvas.setViewTransform({0,0},80);
        canvas.setRoofOpeningControls(children);
        std::vector<CanvasRoofOpeningTarget> targets;
        for (const auto& child : children) targets.push_back(child.target);
        if (child_first) canvas.setSelectedRoofOpenings(targets);
        canvas.setSelectedIds({"component"});
        if (!child_first) canvas.setSelectedRoofOpenings(targets);
        canvas.setSelectionTransformEnabled(false,false);
        canvas.setSelectionAxisResizeEnabled(true);
        canvas.setEntityAxisResizeRequested([&](QString, double, double, Vec2) {
            ++individual_commits; return false;
        });
        canvas.setEntitiesAxisResizeStarted([&](QStringList ids) {
            require(ids == QStringList{"component"}, "press contains only the ordinary owner");
            require(canvas.selectedEntityPresentations().size() == 2, "both owner profiles captured");
            ++starts;
        });
        canvas.setEntitiesAxisResizePreviewRequested([&](QStringList ids, double x, double y,
                                                       Vec2 fixed, std::uint64_t ticket) {
            require(ids == QStringList{"component"}, "preview never adds the child's host roof");
            ++previews; sx=x; sy=y; anchor=fixed; serial=ticket;
            require(canvas.markEntityTransformPreviewPending(ticket), "mixed axis preview owns async ticket");
            return std::optional<std::vector<CanvasEntity>>{};
        });
        canvas.setEntitiesAxisResizeRequested([&](QStringList ids, double x, double y, Vec2 fixed) {
            require(ids == QStringList{"component"} && close(x,sx) && close(y,sy) &&
                close(fixed.x,anchor.x) && close(fixed.y,anchor.y), "release retains owner-only operators");
            ++commits;
            if (inspect_release) {
                require(!canvas.entityTransformPreviewPending() && canvas.entityTransformPreview().empty(),
                    "provisional cohort is consumed before nested host review");
                require(!complete(), "consumed ticket cannot republish child provisional controls");
                QApplication::processEvents();
            }
            return false;
        });
        canvas.setSelectionAxisResizeCohort({"component"},frame);
    }

    void drag(std::size_t side = 1) {
        const auto handles = canvas.selectionAxisHandlePositions();
        require(handles.size() == 4, "component and selected skylight expose four side grips");
        grip = handles.at(side);
        const QPointF deltas[]{{-80,0},{80,0},{0,-80},{0,80}};
        end = grip+deltas[side];
        mouse(canvas,QEvent::MouseButtonPress,grip);
        mouse(canvas,QEvent::MouseMove,end);
        require(starts == 1 && previews == 1 && commits == 0 && individual_commits == 0 &&
            canvas.entityTransformPreviewPending(), "single owner remains an asynchronous group with children");
    }
    std::vector<CanvasEntity> proposed() const {
        auto result = source;
        const auto transform = [&](Vec2 p) {
            return Vec2{anchor.x+(p.x-anchor.x)*sx,anchor.y+(p.y-anchor.y)*sy};
        };
        for (auto& entity : result)
            for (auto& segment : entity.segments) {
                segment.start=transform(segment.start); segment.end=transform(segment.end);
            }
        return result;
    }
    std::vector<CanvasRoofOpeningControls> manufactured() const {
        auto result = children;
        for (auto& child : result) {
            child.frame.center={anchor.x+(child.frame.center.x-anchor.x)*sx,
                                anchor.y+(child.frame.center.y-anchor.y)*sy};
            // Physical production may differ from an affine projected mouth.
            child.frame.width_metres *= sx*0.9;
            child.frame.depth_metres *= sy;
        }
        return result;
    }
    CanvasSelectionFrame exactFrame() const {
        double left=std::numeric_limits<double>::infinity(), bottom=left, right=-left, top=-left;
        const auto include = [&](Vec2 p) {
            left=std::min(left,p.x); right=std::max(right,p.x);
            bottom=std::min(bottom,p.y); top=std::max(top,p.y);
        };
        for (const auto& entity : proposed())
            for (const auto& segment : entity.segments) { include(segment.start); include(segment.end); }
        for (const auto& child : manufactured())
            for (const auto sign : {Vec2{-1,-1},Vec2{1,1}})
                include({child.frame.center.x+sign.x*child.frame.width_metres*.5,
                         child.frame.center.y+sign.y*child.frame.depth_metres*.5});
        return {{(left+right)*.5,(bottom+top)*.5},0,right-left,top-bottom};
    }
    bool complete() {
        return canvas.completeEntityTransformPreview(serial, proposed(), {}, {}, manufactured(), exactFrame());
    }
    void release() { mouse(canvas,QEvent::MouseButtonRelease,end); QApplication::processEvents(); }
};

void exact_completion_and_release() {
    for (const bool child_first : {false,true}) {
        Fixture f(false,child_first);
        const auto committed=render(f.canvas,true), original=render(f.canvas);
        const auto generation=f.canvas.roofOpeningControlsGeneration();
        f.drag();
        require(close(f.sx,1.25) && close(f.sy,1) && close(f.anchor.x,-2) && close(f.anchor.y,0),
            "padded right grip keeps the opposite analytical edge fixed");
        require(f.complete() && f.canvas.entityTransformPreview().size()==2,
            "exact completion admits every keyed owner presentation and typed child");
        require(render(f.canvas)!=original && render(f.canvas,true)==committed,
            "manufactured preview changes interactive paint and leaves output committed");
        require(f.canvas.roofOpeningControlsGeneration()==generation &&
            f.canvas.roofOpeningControls().front().frame.width_metres==1,
            "preview controls never replace the committed control publication");
        const auto moved=f.manufactured().front().frame;
        const QPointF mouth_edge{320+(moved.center.x+moved.width_metres*.5)*80,240};
        const auto color=render(f.canvas).pixelColor(mouth_edge.toPoint());
        require(color.blue()>color.red()+40, "actual manufactured child mouth is painted");
        f.inspect_release=true;
        f.release();
        require(f.commits==1 && f.individual_commits==0 && render(f.canvas)==original,
            "one owner/child cohort releases once and rejection restores committed source");
        require(!f.complete() && render(f.canvas,true)==committed,
            "late completion cannot reinstate an admitted provisional cohort");
    }
    for (std::size_t side=0; side<4; ++side) {
        Fixture f;
        f.drag(side);
        const Vec2 anchors[]{{2,0},{-2,0},{0,-1},{0,1}};
        require(close(f.anchor.x,anchors[side].x) && close(f.anchor.y,anchors[side].y),
            "each side anchors its opposite physical edge independently of padding");
        require(side<2 ? close(f.sx,1.25) && close(f.sy,1) : close(f.sx,1) && close(f.sy,1.5),
            "side gesture scales exactly one analytical axis");
        f.release();
        require(f.commits==0 && f.canvas.entityTransformPreviewPending(), "release waits for exact child coverage");
        require(f.complete(), "released pending exact cohort completes");
        QApplication::processEvents();
        require(f.commits==1 && !f.canvas.entityTransformPreviewPending(), "released completion admits once");
    }
}

void pending_successor_retains_cohort() {
    Fixture f;const auto output=render(f.canvas,true);(void)render(f.canvas);f.drag();
    const auto initial=render(f.canvas);
    const auto initial_edge=320+(f.anchor.x+(2.0-f.anchor.x)*f.sx)*80;
    const auto initial_color=initial.pixelColor(int(std::lround(initial_edge)),215);
    require(initial_color.blue()>initial_color.red()+40,
        "typed child mouths must follow the pointer before the first exact cohort result");
    require(f.complete(),"first cohort admitted");
    const auto accepted=render(f.canvas);const auto old=f.serial;
    const auto old_sx=f.sx;
    const auto mouth=f.manufactured().front().frame;
    const auto mouth_edge=mouth.center.x+mouth.width_metres*.5;
    const auto handle_before=f.canvas.selectionAxisHandlePositions().at(1);
    mouse(f.canvas,QEvent::MouseMove,f.end+QPointF(24,0));
    require(f.previews==2 && f.serial>old,"successor cohort requested");
    const auto successor=render(f.canvas);
    // The accepted result stays the base: bodies, mouths and frame follow the
    // current pointer by the residual scale about the fixed anchor.
    const auto red=[](const QImage& image,int x,int y){const auto c=image.pixelColor(x,y);return c.red()>180&&c.green()<100&&c.blue()<100;};
    const auto blue_near=[](const QImage& image,double x,int y) {
        for(int dx=-3;dx<=3;++dx){const auto c=image.pixelColor(int(std::lround(x))+dx,y);if(c.blue()>c.red()+40)return true;}
        return false;
    };
    const auto ratio=f.sx/old_sx;
    require(ratio>1.01,"successor widens the cohort");
    const auto edge_after=320+(f.anchor.x+(mouth_edge-f.anchor.x)*ratio)*80;
    // The first owner body ends at x -0.75 m (screen 260) when accepted.
    const auto body_after=320+(f.anchor.x+(-0.75-f.anchor.x)*ratio)*80;
    const auto directory=qEnvironmentVariable("VERTEX_TEST_CAPTURE_DIR");
    if (!directory.isEmpty()) {
        require(accepted.save(directory+"/mixed-accepted.png")&&successor.save(directory+"/mixed-successor.png")&&
            initial.save(directory+"/mixed-initial.png"),"mixed cohort captures must save");
    }
    // Sample inside the fill, beyond the blue selection stroke at its edge.
    require(body_after>263 && red(successor,int(std::lround(body_after))-5,200) && !red(accepted,int(std::lround(body_after))-5,200),
        "pending successor carries accepted owner bodies to the current pointer");
    require(blue_near(accepted,320+mouth_edge*80,215) && !blue_near(accepted,edge_after,215) &&
            blue_near(successor,edge_after,215) && !blue_near(successor,320+mouth_edge*80,215),
        "pending successor carries the manufactured child mouth to the current pointer");
    require(f.canvas.selectionAxisHandlePositions().at(1).x()>handle_before.x()+15,
        "pending successor moves the cohort frame handles with the pointer");
    mouse(f.canvas,QEvent::MouseMove,f.end+QPointF(24,0));
    require(f.previews==2,"identical effective cohort reuses admission ticket");
    mouse(f.canvas,QEvent::MouseButtonRelease,f.end+QPointF(24,0));
    require(f.commits==0&&render(f.canvas,true)==output,"old cohort display cannot authorize release");
}

void refused_completion() {
    for (int defect=0; defect<15; ++defect) {
        Fixture f(true);
        const auto output=render(f.canvas,true), original=render(f.canvas);
        f.drag();
        auto entities=f.proposed();
        std::optional<std::vector<CanvasRoofOpeningControls>> children=f.manufactured();
        auto frame=f.exactFrame();
        switch (defect) {
        case 0: children.reset(); break;
        case 1: children->pop_back(); break;
        case 2: children->push_back(children->front()); break;
        case 3: (*children)[1]=children->front(); break;
        case 4: std::reverse(children->begin(),children->end()); break;
        case 5: children->front().target.roof_id="wrong-roof"; break;
        case 6: ++children->front().target.source_revision; break;
        case 7: children->front().presentation_owner_id="wrong-presentation"; break;
        case 8: children->front().frame.reference_across={1,0}; break;
        case 9: entities.pop_back(); break;
        case 10: frame.width_metres=std::numeric_limits<double>::quiet_NaN(); break;
        case 11: entities[1]=entities.front(); break;
        case 12: entities.front().type="wrong-type"; break;
        case 13: children->front().target.opening_id="wrong-mouth"; break;
        case 14: children->front().frame.center.x=std::numeric_limits<double>::infinity(); break;
        }
        require(f.canvas.completeEntityTransformPreview(f.serial,entities,{}, {},children,frame),
            "invalid current exact proposal is consumed as a rejected completion");
        require(f.canvas.entityTransformPreview().empty() && render(f.canvas,true)==output,
            "missing, duplicate, reordered or stale coverage rejects the complete cohort");
        f.release();
        require(f.commits==0 && f.individual_commits==0 && render(f.canvas)==original && !f.complete(),
            "rejected cohort never releases either lane or accepts a late exact result");
    }
}

void stale_capture() {
    for (const bool admitted : {false,true}) for (int mutation=0; mutation<8; ++mutation) {
        Fixture f(true);
        const auto output=render(f.canvas,true);
        f.drag();
        if (admitted) require(f.complete(), "stale-input fixture first admits an exact whole-cohort preview");
        const auto generation=f.canvas.roofOpeningControlsGeneration();
        switch (mutation) {
        case 0: f.canvas.setRoofOpeningControls(f.children); break;
        case 1: f.canvas.setSelectedRoofOpenings({f.children[1].target,f.children[0].target}); break;
        case 2: f.canvas.setSelectedIds({}); break;
        case 3: f.canvas.setViewTransform({0.1,0},80); break;
        case 4: f.canvas.setMetricUnits(false); break;
        case 5: { auto frame=f.frame; frame.width_metres+=0.1;
            f.canvas.setSelectionAxisResizeCohort({"component"},frame); break; }
        case 6: { auto font=f.canvas.font(); font.setPointSize(font.pointSize()+1); f.canvas.setFont(font); break; }
        case 7: f.canvas.setEntities(f.source); break;
        }
        if (mutation==0 || mutation==7)
            require(f.canvas.roofOpeningControlsGeneration()>generation, "republished source retires control generation");
        require(!f.complete() && !f.canvas.entityTransformPreviewPending() && render(f.canvas,true)==output,
            "source, ordered selection, units, viewport, font or frame changes retire the whole pending cohort");
        f.release();
        require(f.commits==0 && f.individual_commits==0, "retired capture cannot partially release an owner");
    }
}

void admission() {
    Fixture f;
    f.canvas.setRoofOpeningControls({});
    require(f.canvas.selectionAxisHandlePositions().empty(), "unavailable actual child controls refuse all axis grips");
    f.canvas.setRoofOpeningControls(f.children);
    require(f.canvas.selectionAxisHandlePositions().size()==4, "republished qualified controls restore complete cohort");
    auto incomplete=f.source;
    incomplete[1].selected=false;
    incomplete[0].selected=true;
    f.canvas.setEntities(incomplete); f.canvas.setRoofOpeningControls(f.children);
    require(f.canvas.selectionAxisHandlePositions().empty(), "unselected same-owner profile refuses partial ordinary admission");

    Fixture host;
    auto roof=rectangle("actual-roof","body",-3,-2,3,2);
    auto scene=host.source; scene.push_back(roof);
    host.canvas.setEntities(scene); host.canvas.setRoofOpeningControls(host.children);
    host.canvas.setSelectedIds({"component","actual-roof"});
    host.canvas.setSelectionAxisResizeCohort({"component","actual-roof"},host.frame);
    require(host.canvas.selectionAxisHandlePositions().empty(), "containing actual roof cannot enter the ordinary cohort");

    Fixture ordinary;
    ordinary.canvas.setSelectedRoofOpenings({});
    require(ordinary.canvas.selectionAxisHandlePositions().size()==4,
        "legacy individual ordinary axis route remains available with its own opt-in");
    auto sibling=rectangle("sibling","body",1,-1,2,1);
    auto scene2=ordinary.source; scene2.push_back(sibling);
    ordinary.canvas.setEntities(scene2); ordinary.canvas.setSelectedIds({"component","sibling"});
    ordinary.canvas.setSelectionAxisResizeCohort({"component","sibling"},ordinary.frame);
    require(ordinary.canvas.selectionAxisHandlePositions().size()==4, "ordinary-only two-owner axis cohort remains available");

    Fixture bounded;
    auto children=bounded.children;
    for (int index=1; index<999; ++index) {
        auto child=children.front(); child.target.opening_id=QStringLiteral("mouth-%1").arg(index);
        children.push_back(child);
    }
    std::vector<CanvasRoofOpeningTarget> targets;
    for (const auto& child : children) targets.push_back(child.target);
    bounded.canvas.setRoofOpeningControls(children); bounded.canvas.setSelectedRoofOpenings(targets);
    require(bounded.canvas.selectionAxisHandlePositions().size()==4, "1000 semantic members remain admissible");
    auto extra=children.front(); extra.target.opening_id="mouth-over-limit";
    children.push_back(extra); targets.push_back(extra.target);
    bounded.canvas.setRoofOpeningControls(children); bounded.canvas.setSelectedRoofOpenings(targets);
    require(bounded.canvas.selectionAxisHandlePositions().empty(), "1001 semantic members refuse the whole cohort");

    Fixture invisible;
    invisible.canvas.setSelectedIds({"component","unpresented-owner"});
    require(invisible.canvas.selectionAxisHandlePositions().empty(), "invisible ordinary members cannot be omitted from admission");
    Fixture ambiguous;
    auto duplicate=ambiguous.children; duplicate.push_back(duplicate.front());
    ambiguous.canvas.setRoofOpeningControls(duplicate);
    require(ambiguous.canvas.selectionAxisHandlePositions().empty(), "ambiguous child source controls refuse complete cohort");

    Fixture labeled;
    CanvasLabel label;
    label.id="component"; label.text="Authored annotation"; label.selected=true;
    labeled.canvas.setLabels({label});
    require(labeled.canvas.selectionAxisHandlePositions().empty(), "unsupported authored label refuses axis admission");
    label.plan_only=true; label.avoid_components=true; label.callout_role="area_name";
    labeled.canvas.setLabels({label});
    labeled.canvas.setSelectedGeneratedLabelPresentations({{"component","area_name",{}}});
    require(labeled.canvas.selectionAxisHandlePositions().empty(), "independently selected generated label refuses axis admission");

    Fixture referenced;
    CanvasReference reference;
    reference.id="component"; reference.image=QImage(2,2,QImage::Format_ARGB32);
    reference.image.fill(Qt::gray); reference.selected=true;
    referenced.canvas.setReferences({reference});
    require(referenced.canvas.selectionAxisHandlePositions().empty(), "selected references refuse axis admission");
}

void legacy_mixed_routes() {
    Fixture axis;
    axis.canvas.setMixedSelectionRigidTransformEnabled(true);
    axis.canvas.setMixedSelectionScaleEnabled(true);
    axis.canvas.setSelectionTransformEnabled(true,true);
    axis.canvas.setEntitiesTransformStarted([](QStringList) { throw std::runtime_error("axis entered legacy callback"); });
    axis.canvas.setEntitiesTransformPreviewRequested([](QStringList,double,double,Vec2,std::uint64_t) {
        return std::optional<std::vector<CanvasEntity>>{};
    });
    axis.canvas.setEntitiesTransformRequested([](QStringList,double,double,Vec2) { return false; });
    axis.drag(); require(axis.complete(), "mixed opt-ins retain directional cohort priority");
    axis.release(); require(axis.commits==1, "directional release keeps owner-only axis callback");

    for (const bool rotate : {false,true}) {
        Fixture legacy;
        legacy.canvas.setSelectionAxisResizeCohort({},std::nullopt);
        legacy.canvas.setMixedSelectionRigidTransformEnabled(true);
        legacy.canvas.setMixedSelectionScaleEnabled(true);
        legacy.canvas.setSelectionTransformEnabled(true,true);
        int starts=0, releases=0;
        legacy.canvas.setEntitiesTransformStarted([&](QStringList ids) {
            require(ids==QStringList{"component"}, "legacy mixed route keeps its owner roster"); ++starts;
        });
        legacy.canvas.setEntitiesTransformPreviewRequested([&](QStringList,double,double,Vec2,std::uint64_t serial) {
            legacy.serial=serial;
            require(legacy.canvas.markEntityTransformPreviewPending(serial), "legacy preview captures typed authority");
            return std::optional<std::vector<CanvasEntity>>{};
        });
        legacy.canvas.setEntitiesTransformRequested([&](QStringList,double,double,Vec2) { ++releases; return false; });
        require(legacy.canvas.selectionAxisHandlePositions().empty(), "legacy mixed route retains its own control family");
        const auto bounds=legacy.canvas.selectionBounds();
        require(bounds.has_value(), "legacy complete mixed frame remains available");
        const auto rotation=legacy.canvas.selectionRotationHandlePosition();
        require(rotation.has_value(), "legacy rigid rotation handle remains available");
        const auto grip=rotate ? *rotation : bounds->bottomRight();
        const auto end=grip+QPointF(40,40);
        mouse(legacy.canvas,QEvent::MouseButtonPress,grip);
        mouse(legacy.canvas,QEvent::MouseMove,end);
        require(starts==1 && legacy.canvas.entityTransformPreviewPending(), "legacy uniform/rigid callback remains captured");
        require(legacy.complete(), "legacy exact child completion remains admitted");
        mouse(legacy.canvas,QEvent::MouseButtonRelease,end);
        require(releases==1 && legacy.commits==0, "legacy mixed release retains its existing callback");
    }
}
}

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM","offscreen");
    sketch::testing::noninteractive_errors();
    QStandardPaths::setTestModeEnabled(true);
    QApplication app(argc,argv);
    QTemporaryDir settings;
    QCoreApplication::setOrganizationName("VertexTests");
    QCoreApplication::setApplicationName("MixedAxisResizeCanvas-"+QUuid::createUuid().toString(QUuid::WithoutBraces));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,settings.path());
    QSettings::setPath(QSettings::IniFormat,QSettings::SystemScope,settings.path());
    try {
        require(settings.isValid(), "private settings directory exists");
        const auto font_id=QFontDatabase::addApplicationFont(":/fonts/Inter.ttf");
        require(font_id>=0, "bundled canvas font loads");
        const auto families=QFontDatabase::applicationFontFamilies(font_id);
        require(!families.isEmpty(), "bundled font exposes its family");
        app.setFont(QFont(families.front(),10));
        pending_successor_retains_cohort();exact_completion_and_release(); refused_completion(); stale_capture(); admission(); legacy_mixed_routes();
        std::cout << "Mixed axis resize canvas tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
