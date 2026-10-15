#include "../src/desktop/plan_canvas.hpp"
#include "support/noninteractive_errors.hpp"

#include <QApplication>
#include <QDir>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QMimeData>
#include <QMouseEvent>

#include <cmath>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>

namespace {
using namespace sketch;
using namespace sketch::desktop;
Vec2 operator+(Vec2 a, Vec2 b) { return {a.x+b.x,a.y+b.y}; }
Vec2 operator-(Vec2 a, Vec2 b) { return {a.x-b.x,a.y-b.y}; }
Vec2 operator*(Vec2 a, double scale) { return {a.x*scale,a.y*scale}; }
double dot(Vec2 a,Vec2 b) { return a.x*b.x+a.y*b.y; }
Vec2 rotate(Vec2 p,double angle) {
    return {std::cos(angle)*p.x-std::sin(angle)*p.y,
            std::sin(angle)*p.x+std::cos(angle)*p.y};
}
void expectAngle(double actual,double expected,const char* message) {
    if (std::abs(actual-expected)<1e-9) return;
    std::ostringstream detail;
    detail.precision(15);
    detail << message << ": expected " << expected << ", actual " << actual;
    throw std::runtime_error(detail.str());
}
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
void expect(Vec2 actual, Vec2 expected, const std::string& context) {
    if (std::hypot(actual.x-expected.x, actual.y-expected.y) < 1e-8) return;
    std::ostringstream message;
    message.precision(15);
    message << context << ": expected (" << expected.x << ", " << expected.y
            << "), actual (" << actual.x << ", " << actual.y << ')';
    throw std::runtime_error(message.str());
}
QPointF screen(const PlanCanvas& canvas, Vec2 point) {
    return QRectF(canvas.rect()).center() + QPointF(
        (point.x-canvas.viewCenter().x)*canvas.viewScale(),
        -(point.y-canvas.viewCenter().y)*canvas.viewScale());
}
Vec2 gridPoint(const PlanCanvas& canvas, Vec2 point) {
    const auto step=canvas.gridSpacingMetres();
    return {std::round(point.x/step)*step,std::round(point.y/step)*step};
}
CanvasEntity rectangle(QString id, QString type, Vec2 center, double width,
                       double depth, double angle = 0) {
    const Vec2 u{std::cos(angle), std::sin(angle)}, v{-u.y,u.x};
    const auto corner = [&](double x, double y) {
        return Vec2{center.x+u.x*x+v.x*y, center.y+u.y*x+v.y*y};
    };
    const auto a=corner(-width/2,-depth/2), b=corner(width/2,-depth/2);
    const auto c=corner(width/2,depth/2), d=corner(-width/2,depth/2);
    CanvasEntity entity;
    entity.id=id; entity.type=type; entity.filled=true;
    entity.segments={{a,b,0},{b,c,0},{c,d,0},{d,a,0}};
    return entity;
}
CanvasEntity symbol(Vec2 center, double angle = 0) {
    auto entity=rectangle("moving-symbol", "symbol", center, 1.2, .8, angle);
    CanvasSvgSymbol svg;
    svg.catalog_id=entity.id;
    svg.document=R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 120 80"><rect width="120" height="80" fill="white" stroke="black"/></svg>)";
    svg.view_box=svg.footprint_view_box=QRectF(0,0,120,80);
    svg.position=center; svg.width_metres=1.2; svg.depth_metres=.8;
    svg.rotation_radians=angle;
    entity.svg_symbol=svg;
    return entity;
}
CanvasEntity wall(bool vertical = true, double face = 2, double low = -3, double high = 3) {
    auto entity=vertical ? rectangle("vertical-wall", "wall", {face+.1,(low+high)/2}, .2, high-low)
                         : rectangle("horizontal-wall", "wall", {(low+high)/2,face+.1}, high-low,.2);
    entity.snap_segments=vertical ? Boundary{{{face+.1,low},{face+.1,high},0}}
                                  : Boundary{{{low,face+.1},{high,face+.1},0}};
    return entity;
}
CanvasEntity translated(CanvasEntity entity, Vec2 delta) {
    for (auto& segment : entity.segments) {
        segment.start=segment.start+delta; segment.end=segment.end+delta;
    }
    if (entity.svg_symbol) entity.svg_symbol->position=entity.svg_symbol->position+delta;
    return entity;
}
CanvasEntity transformed(CanvasEntity entity,Vec2 delta,double angle) {
    require(entity.svg_symbol.has_value() || entity.resize_frame.has_value(),
        "rotation fake requires physical symbol axes");
    const auto center=entity.svg_symbol ? entity.svg_symbol->position : entity.resize_frame->center;
    const auto transform=[&](Vec2 p) { return center+rotate(p-center,angle)+delta; };
    for (auto& segment:entity.segments) {
        segment.start=transform(segment.start); segment.end=transform(segment.end);
    }
    if (entity.svg_symbol) {
        entity.svg_symbol->position=center+delta;
        entity.svg_symbol->rotation_radians+=angle;
    }
    if (entity.resize_frame) {
        entity.resize_frame->center=center+delta;
        entity.resize_frame->rotation_radians+=angle;
    }
    return entity;
}
void setup(PlanCanvas& canvas, std::vector<CanvasEntity> entities, double scale = 100) {
    canvas.resize(900,700);
    canvas.setOverviewMapEnabled(false);
    canvas.setGridEnabled(false);
    canvas.setSnapEnabled(true);
    canvas.setWallSnapEnabled(true);
    canvas.setEntities(std::move(entities));
    canvas.setViewTransform({0,0},scale);
    canvas.show();
    QApplication::processEvents();
}
void capture(PlanCanvas& canvas, const QString& name) {
    const auto directory=qEnvironmentVariable("VERTEX_TEST_CAPTURE_DIR");
    if (directory.isEmpty()) return;
    require(QDir().mkpath(directory), "capture directory could not be created");
    require(canvas.grab().save(QDir(directory).filePath(name+".png")), "canvas capture failed");
}
struct LibraryDrag {
    PlanCanvas& canvas;
    QMimeData mime;
    std::optional<Vec2> preview, committed;
    double angle{}, preview_angle{}, committed_angle{};
    std::vector<double> requested_angles;
    LibraryDrag(PlanCanvas& target, double rotation = 0, bool raw = false, bool rotation_enabled = false)
        : canvas(target), angle(rotation) {
        mime.setData("application/x-vertex-symbol", QJsonDocument(QJsonObject{
            {"id","fixture-symbol"},{"scale",1.0}}).toJson(QJsonDocument::Compact));
        canvas.setSymbolDragPreviewRequested([this,rotation_enabled](const QString&, double, Vec2 point) {
            preview=point;
            if (!rotation_enabled) expectAngle(canvas.symbolDragPreviewRotationRadians(),0,
                "legacy library provider remains translation only");
            preview_angle=rotation_enabled ? canvas.symbolDragPreviewRotationRadians() : 0;
            requested_angles.push_back(preview_angle);
            return std::optional<CanvasEntity>{symbol(point,angle+preview_angle)};
        });
        if (rotation_enabled)
            canvas.setSymbolDropped([this](QString, double, Vec2 point,double rotation_delta) {
                committed=point; committed_angle=rotation_delta;
            },[raw](const QString&) { return raw; });
        else
            canvas.setSymbolDropped([this](QString, double, Vec2 point) { committed=point; },
                [raw](const QString&) { return raw; });
    }
    void enter(Vec2 point, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
        QDragEnterEvent event(screen(canvas,point).toPoint(),Qt::CopyAction,&mime,Qt::LeftButton,modifiers);
        QApplication::sendEvent(&canvas,&event);
        require(event.isAccepted(), "library drag enter rejected the valid symbol payload");
    }
    void move(Vec2 point, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
        QDragMoveEvent event(screen(canvas,point).toPoint(),Qt::CopyAction,&mime,Qt::LeftButton,modifiers);
        QApplication::sendEvent(&canvas,&event);
        require(event.isAccepted(), "library drag move rejected the valid symbol payload");
    }
    void drop(Vec2 point, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
        QDropEvent event(QPointF(screen(canvas,point).toPoint()),Qt::CopyAction,&mime,Qt::LeftButton,modifiers);
        QApplication::sendEvent(&canvas,&event);
        require(event.isAccepted() && committed.has_value(), "library drop did not commit");
    }
};
void libraryCase(const char* name, std::vector<CanvasEntity> entities, Vec2 raw,
                 Vec2 expected, double scale = 100, double angle = 0, bool fallback = false) {
    PlanCanvas canvas;
    setup(canvas,std::move(entities),scale);
    if (fallback) expected=gridPoint(canvas,raw);
    LibraryDrag drag(canvas,angle);
    drag.enter(raw);
    drag.move(raw);
    require(drag.preview.has_value(), "library move did not request preview geometry");
    capture(canvas,QString::fromLatin1(name));
    expect(*drag.preview,expected,std::string(name)+" preview center / wall-face translation");
    const auto preview=*drag.preview;
    drag.drop(raw);
    expect(*drag.committed,preview,std::string(name)+" drop must equal visible preview");
}
void libraryContacts() {
    // First RED must identify the wrong translation, not a missing API or host.
    libraryCase("right-edge-to-left-wall-face",{wall()}, {1.34,0},{1.4,0});
    libraryCase("left-edge-to-right-wall-face",{wall()}, {2.86,0},{2.8,0});
    libraryCase("top-edge-to-bottom-wall-face",{wall(false)}, {0,1.54},{0,1.6});
    libraryCase("bottom-edge-to-top-wall-face",{wall(false)}, {0,2.66},{0,2.6});
    libraryCase("nearest-edge-wins",{wall(),wall(false)}, {1.34,1.57},{1.34,1.6});
    const auto cabinet=rectangle("cabinet","symbol",{2.4,0},.8,2);
    libraryCase("object-edge-contact",{cabinet},{1.34,0},{1.4,0});
    auto dimension=cabinet;
    dimension.id="dimension"; dimension.type="dimension_line";
    libraryCase("dimension-is-not-a-contact-edge",{dimension},{1.34,0},{},100,0,true);
    auto otherFloor=wall();
    otherFloor.snap_segments.clear();
    libraryCase("ineligible-wall-is-not-a-contact-edge",{otherFloor},{1.34,0},{},100,0,true);
    for (double scale : {50.,100.,200.}) {
        const double inside=10/scale, outside=14/scale;
        libraryCase("ten-pixel-contact",{wall()}, {1.4-inside,0},{1.4,0},scale);
        libraryCase("fourteen-pixel-gap",{wall()}, {1.4-outside,0},{},scale,0,true);
    }
    libraryCase("distant-face",{wall()}, {0,0},{0,0});
    libraryCase("finite-face-no-extension",{wall(true,2,-1,1)}, {1.34,2},{},100,0,true);
    const double angle=std::atan2(-.6,.8);
    auto angled=rectangle("angled-wall","wall",{0,0},6,.2,angle);
    angled.snap_segments={{{-2.4,1.8},{2.4,-1.8},0}};
    // Rounding the Qt drag position is part of the actual input, so preserve
    // that tangential component while closing the perpendicular physical gap.
    const Vec2 raw{.34,.45};
    const double gap=raw.x*.6+raw.y*.8-.5;
    libraryCase("rotated-footprint-angled-face",{angled},raw,
        {raw.x-.6*gap,raw.y-.8*gap},100,angle);
}
void bypassAndGrid() {
    for (int mode=0;mode<4;++mode) {
        PlanCanvas canvas;
        setup(canvas,{wall()});
        canvas.setSnapEnabled(mode!=1);
        canvas.setRawPointInput(mode==2);
        LibraryDrag drag(canvas,0,mode==3);
        const auto modifiers=mode==0 ? Qt::ShiftModifier : Qt::NoModifier;
        drag.enter({1.34,.13},modifiers);
        drag.move({1.34,.13},modifiers);
        expect(*drag.preview,{1.34,.13},"Shift / Snap off / raw input / raw symbol preview bypass");
        drag.drop({1.34,.13},modifiers);
        expect(*drag.committed,{1.34,.13},"bypass drop remains literal");
    }
    PlanCanvas canvas;
    setup(canvas,{});
    canvas.setGridEnabled(true);
    LibraryDrag drag(canvas);
    const Vec2 raw{.37,.23};
    const double step=canvas.gridSpacingMetres();
    const Vec2 expected{std::round(raw.x/step)*step,std::round(raw.y/step)*step};
    drag.enter(raw); drag.move(raw);
    expect(*drag.preview,expected,"no edge retains grid fallback");
    drag.drop(raw);
    expect(*drag.committed,expected,"grid preview/drop equality");
}
void wallOutlineAndSceneReplacement() {
    PlanCanvas outline;
    CanvasEntity joined;
    joined.id="joined-wall"; joined.type="wall";
    joined.segments={{{0,0},{2,0},0},{{2,0},{2,2},0},{{2,2},{1.8,2},0},
        {{1.8,2},{1.8,.2},0},{{1.8,.2},{0,.2},0},{{0,.2},{0,0},0}};
    joined.snap_segments={{{0,.1},{1.9,.1},0},{{1.9,.1},{1.9,2},0}};
    joined.resize_frame=CanvasSelectionFrame{{1,1},0,2,2};
    setup(outline,{joined});
    LibraryDrag contact(outline);
    contact.enter({.7,.66}); contact.move({.7,.66});
    const bool physical=contact.preview && std::hypot(contact.preview->x-.7,contact.preview->y-.6)<1e-8;
    if (!physical) std::cerr << "joined-wall contact used its rectangular selection frame instead of its physical face\n";

    PlanCanvas replacement;
    setup(replacement,{wall()});
    LibraryDrag drag(replacement);
    drag.enter({1.34,0}); drag.move({1.34,0});
    const auto serial=replacement.symbolDragPreviewSerial();
    replacement.setEntities({wall(true,3)});
    const bool retired=!replacement.symbolDragPreviewActive() &&
        !replacement.completeSymbolDragPreview(serial,symbol({1.4,0}));
    if (!retired) std::cerr << "scene replacement retained or revived a library drag's old geometry/contact\n";
    require(physical && retired,"physical wall-outline and scene-replacement admission checks");
}
void mouse(PlanCanvas& canvas,QEvent::Type type,Vec2 point,
           Qt::KeyboardModifiers modifiers=Qt::NoModifier) {
    const auto position=screen(canvas,point);
    const auto button=type==QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton;
    const auto buttons=type==QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton;
    QMouseEvent event(type,position,canvas.mapToGlobal(position.toPoint()),button,buttons,modifiers);
    QApplication::sendEvent(&canvas,&event);
    QApplication::processEvents();
}
struct ObjectDrag {
    PlanCanvas& canvas;
    CanvasEntity source;
    std::optional<Vec2> preview, committed;
    double preview_angle{}, committed_angle{};
    int commits{};
    ObjectDrag(PlanCanvas& target, CanvasEntity entity, bool rotation_enabled = false)
        : canvas(target),source(std::move(entity)) {
        canvas.setSelectedId(source.id);
        canvas.setEntitiesMovePreviewRequested([this,rotation_enabled](QStringList ids,Vec2 delta,std::uint64_t) {
            require(ids==QStringList{source.id},"move provider receives the captured selected ID");
            preview=delta;
            if (!rotation_enabled) expectAngle(canvas.entitiesMovePreviewRotationRadians(),0,
                "legacy move provider remains translation only");
            preview_angle=rotation_enabled ? canvas.entitiesMovePreviewRotationRadians() : 0;
            return std::optional<std::vector<CanvasEntity>>{
                std::vector<CanvasEntity>{rotation_enabled ? transformed(source,delta,preview_angle)
                                                         : translated(source,delta)}};
        });
        if (rotation_enabled)
            canvas.setEntitiesMoveRequested([this](QStringList ids,Vec2 delta,double angle) {
                require(ids==QStringList{source.id},"rotation commit keeps captured selected ID");
                committed=delta; committed_angle=angle; ++commits; return true;
            });
        else
            canvas.setEntitiesMoveRequested([this](QStringList ids,Vec2 delta) {
                require(ids==QStringList{source.id},"commit keeps captured selected ID");
                committed=delta; ++commits; return true;
            });
    }
    void start(Vec2 press,Vec2 destination,Qt::KeyboardModifiers modifiers=Qt::NoModifier) {
        mouse(canvas,QEvent::MouseButtonPress,press,modifiers);
        mouse(canvas,QEvent::MouseMove,destination,modifiers);
        require(preview.has_value(),"real selected-symbol drag did not invoke exact preview provider");
    }
};
void existingObjectCases() {
    const Vec2 origin{-1,-1},press{-.8,-.9},pointer{1.54,.1};
    for (int mode=0;mode<4;++mode) {
        PlanCanvas canvas;
        const auto source=symbol(origin);
        setup(canvas,{source,wall()});
        canvas.setSnapEnabled(mode!=1);
        canvas.setRawPointInput(mode==2);
        const auto modifiers=mode==3 ? Qt::ShiftModifier : Qt::NoModifier;
        ObjectDrag drag(canvas,source);
        drag.start(press,pointer,modifiers);
        const Vec2 expected=mode==0 ? Vec2{2.4,1} : Vec2{2.34,1};
        expect(*drag.preview,expected,"existing symbol preserves grab offset and contacts its right edge");
        const auto geometry=canvas.entitiesMovePreview();
        require(geometry.size()==1 && geometry.front().svg_symbol.has_value(),"exact translated SVG preview retained");
        expect(geometry.front().svg_symbol->position,origin+expected,"preview geometry translates source footprint");
        require(geometry.front().svg_symbol->rotation_radians==source.svg_symbol->rotation_radians,
            "contact translation preserves symbol orientation");
        capture(canvas,"existing-object-contact");
        mouse(canvas,QEvent::MouseButtonRelease,pointer,modifiers);
        require(drag.commits==1,"completed object gesture commits exactly once");
        expect(*drag.committed,*drag.preview,"existing object preview/commit equality");
    }
    // Its old footprint is only 6 px away after this move. It must never be
    // admitted as an external target, including another presentation of its ID.
    PlanCanvas canvas;
    const auto source=symbol({0,0});
    auto duplicate=rectangle(source.id,"wall",{0,0},1.2,.8);
    duplicate.snap_segments=source.segments;
    setup(canvas,{source,duplicate});
    ObjectDrag drag(canvas,source);
    // Cross Qt's drag threshold before returning near the source footprint.
    drag.start({.2,.1},{.8,.1});
    mouse(canvas,QEvent::MouseMove,{.26,.1});
    const auto end=gridPoint(canvas,{.26,.1}),begin=gridPoint(canvas,{.2,.1});
    const Vec2 expected{end.x-begin.x,end.y-begin.y};
    expect(*drag.preview,expected,"all moved-ID presentations excluded; normal grid fallback");
    mouse(canvas,QEvent::MouseButtonRelease,{.26,.1});
    require(drag.commits==1,"self-exclusion gesture commits");
    expect(*drag.committed,expected,"self-exclusion commit retains grid fallback");
    PlanCanvas rotated;
    const double angle=std::atan2(-.6,.8);
    const auto rotatedSource=symbol(origin,angle);
    auto angledWall=rectangle("angled-wall","wall",{0,0},6,.2,angle);
    angledWall.snap_segments={{{-2.4,1.8},{2.4,-1.8},0}};
    setup(rotated,{rotatedSource,angledWall});
    ObjectDrag angledDrag(rotated,rotatedSource);
    angledDrag.start({-.84,-.88},{.50,.57});
    expect(*angledDrag.preview,{1.3016,1.3988},"rotated object contacts angled wall with grab offset");
    const auto angledPreview=rotated.entitiesMovePreview();
    require(angledPreview.size()==1 && angledPreview.front().svg_symbol &&
        angledPreview.front().svg_symbol->rotation_radians==angle,"angled contact preserves captured orientation");
    mouse(rotated,QEvent::MouseButtonRelease,{.50,.57});
    require(angledDrag.commits==1,"angled object drag commits once");
    expect(*angledDrag.committed,*angledDrag.preview,"angled object preview/commit equality");
}
void finalPositionAndInvalidation() {
    PlanCanvas canvas;
    setup(canvas,{wall()});
    LibraryDrag library(canvas);
    library.enter({0,0}); library.move({1.34,0});
    library.drop({2.86,0});
    expect(*library.committed,{2.8,0},"library drop recomputes contact at changed release position");
    library.enter({1.34,0});
    const auto serial=canvas.symbolDragPreviewSerial();
    QDragLeaveEvent leave;
    QApplication::sendEvent(&canvas,&leave);
    require(!canvas.symbolDragPreviewActive() &&
        !canvas.completeSymbolDragPreview(serial,symbol({1.4,0})),"drag leave invalidates stale library geometry");
    for (bool replace : {false,true}) {
        PlanCanvas moving;
        const auto source=symbol({-1,-1});
        setup(moving,{source,wall()});
        ObjectDrag drag(moving,source);
        drag.start({-.8,-.9},{1.54,.1});
        const auto moveSerial=moving.entitiesMovePreviewSerial();
        if (replace) moving.setEntities({source,wall(true,3)});
        else {
            QKeyEvent escape(QEvent::KeyPress,Qt::Key_Escape,Qt::NoModifier);
            QApplication::sendEvent(&moving,&escape);
        }
        require(!moving.completeEntitiesMovePreview(moveSerial,
            std::vector<CanvasEntity>{translated(source,{2.4,1})}),"canceled/replaced capture rejects late preview");
        mouse(moving,QEvent::MouseButtonRelease,{1.54,.1});
        require(drag.commits==0,"Escape or source replacement cannot commit captured contact");
    }
    PlanCanvas moving;
    const auto source=symbol({-1,-1});
    setup(moving,{source,wall()});
    ObjectDrag drag(moving,source);
    drag.start({-.8,-.9},{0,.1});
    mouse(moving,QEvent::MouseButtonRelease,{1.54,.1});
    require(drag.commits==1,"changed object release commits once");
    expect(*drag.committed,{2.4,1},"object release recomputes final footprint contact");
}
void rigidGroupContact(bool rotation_enabled=false) {
    PlanCanvas canvas;
    const auto first=symbol({-2,-1});
    auto second=symbol({-1,-1}); second.id="second-symbol";
    setup(canvas,{first,second,wall()});
    canvas.setSelectedIds({first.id,second.id});
    std::optional<Vec2> preview,committed;
    canvas.setEntitiesMovePreviewRequested([&](QStringList ids,Vec2 delta,std::uint64_t) {
        require(ids.contains(first.id) && ids.contains(second.id) && ids.size()==2,
            "group move retains both semantic IDs");
        preview=delta;
        expectAngle(canvas.entitiesMovePreviewRotationRadians(),0,"multi-symbol group cannot auto-rotate");
        return std::optional<std::vector<CanvasEntity>>{{translated(first,delta),translated(second,delta)}};
    });
    const auto commit=[&](QStringList ids,Vec2 delta) {
        require(ids.size()==2,"group commit retains both semantic IDs");
        committed=delta; return true;
    };
    if (rotation_enabled)
        canvas.setEntitiesMoveRequested([&](QStringList ids,Vec2 delta,double angle) {
            expectAngle(angle,0,"rotation-capable group commit remains translation only");
            return commit(std::move(ids),delta);
        });
    else canvas.setEntitiesMoveRequested(commit);
    mouse(canvas,QEvent::MouseButtonPress,{-1.8,-.9});
    mouse(canvas,QEvent::MouseMove,{.54,.1});
    require(preview.has_value(),"group move publishes its exact preview");
    expect(*preview,{2.4,1},"group contacts using its outer footprint while preserving the grab offset");
    const auto entities=canvas.entitiesMovePreview();
    require(entities.size()==2 && entities[0].svg_symbol && entities[1].svg_symbol,
        "group preview includes both original objects");
    expect({entities[1].svg_symbol->position.x-entities[0].svg_symbol->position.x,
            entities[1].svg_symbol->position.y-entities[0].svg_symbol->position.y},{1,0},
        "magnetic correction preserves rigid spacing within the group");
    mouse(canvas,QEvent::MouseButtonRelease,{.54,.1});
    require(committed.has_value(),"group drag commits");
    expect(*committed,*preview,"group preview and release agree");
}

CanvasEntity angledWall(double angle,double low=-3,double high=3) {
    const Vec2 u{std::cos(angle),std::sin(angle)};
    auto entity=rectangle("angled-wall","wall",u*((low+high)*.5),high-low,.2,angle);
    entity.snap_segments={{u*low,u*high,0}};
    return entity;
}
Vec2 libraryPointer(const PlanCanvas& canvas,Vec2 point) {
    const auto pixel=screen(canvas,point).toPoint();
    const auto center=QRectF(canvas.rect()).center();
    return {canvas.viewCenter().x+(pixel.x()-center.x())/canvas.viewScale(),
            canvas.viewCenter().y-(pixel.y()-center.y())/canvas.viewScale()};
}
void rotatingObjectContacts() {
    constexpr double angle=std::numbers::pi/12;
    const Vec2 u{std::cos(angle),std::sin(angle)},n{-u.y,u.x};
    const Vec2 origin{-1,2},grab{.2,.1};
    // Independent support geometry, matching the native +15 degree fixture:
    // admission uses the old axes; contact uses half the rotated depth.
    const double support=std::abs(n.x)*.6+std::abs(n.y)*.4;
    const auto raw=u*.4+n*(.1+support+.06);
    const auto pivot=raw+grab-rotate(grab,angle);
    const auto expected=u*dot(pivot,u)+n*.5;
    PlanCanvas canvas;
    const auto source=symbol(origin);
    setup(canvas,{source,angledWall(angle)});
    ObjectDrag drag(canvas,source,true);
    drag.start(origin+grab,raw+grab);
    expectAngle(drag.preview_angle,angle,"angled preview relative rotation");
    expect(*drag.preview,expected-origin,"angled proposal includes pivot and face correction");
    const auto preview=canvas.entitiesMovePreview();
    require(preview.size()==1 && preview.front().svg_symbol,"rotated exact preview retained");
    expect(preview.front().svg_symbol->position,expected,"host applies one exact translation");
    expectAngle(preview.front().svg_symbol->rotation_radians,angle,"host applies one exact rotation");
    expectAngle(dot(expected+rotate(grab,angle),u),dot(raw+grab,u),
        "grabbed local point has no tangential jump");
    expect(canvas.entities().front().svg_symbol->position,origin,"preview leaves source center untouched");
    expectAngle(canvas.entities().front().svg_symbol->rotation_radians,0,"preview leaves source angle untouched");
    const auto shown=*drag.preview;
    mouse(canvas,QEvent::MouseButtonRelease,raw+grab);
    require(drag.commits==1,"rotating move commits once");
    expect(*drag.committed,shown,"rotating release equals preview position");
    expectAngle(drag.committed_angle,angle,"rotating release preserves angle across reset");
    expectAngle(canvas.entitiesMovePreviewRotationRadians(),0,"completed gesture clears relative angle");

    PlanCanvas vertical;
    const auto verticalSource=symbol({-1,-1});
    setup(vertical,{verticalSource,wall()});
    ObjectDrag perpendicular(vertical,verticalSource,true);
    perpendicular.start({-.8,-.9},{1.54,.1});
    expectAngle(perpendicular.preview_angle,std::numbers::pi/2,"perpendicular tie chooses positive ninety");
    expect(*perpendicular.preview,{2.6,.9},"vertical alignment preserves tangential grab and uses half depth");
    mouse(vertical,QEvent::MouseButtonRelease,{1.54,.1});
    expectAngle(perpendicular.committed_angle,std::numbers::pi/2,"ninety degree release agrees");

    PlanCanvas finite;
    setup(finite,{verticalSource,wall(true,2,0,.2)});
    ObjectDrag endpoint(finite,verticalSource,true);
    endpoint.start({-.45,-1.35},{1.89,-.15});
    expectAngle(endpoint.preview_angle,0,"rotated support beyond finite span falls back to position only");
    expect(*endpoint.preview,{2.4,1.2},"finite-span fallback retains original contact proposal");
    mouse(finite,QEvent::MouseButtonRelease,{1.89,-.15});

    PlanCanvas object;
    setup(object,{verticalSource,rectangle("cabinet","symbol",{2.4,0},.8,2)});
    ObjectDrag edge(object,verticalSource,true);
    edge.start({-.8,-.9},{1.54,.1});
    expectAngle(edge.preview_angle,0,"object-edge target never rotates a free symbol");
    expect(*edge.preview,{2.4,1},"object-edge target retains existing translation");
    mouse(object,QEvent::MouseButtonRelease,{1.54,.1});

    // A source already near pi uses the short modulo-pi turn, not a flip.
    PlanCanvas modulo;
    const auto turned=symbol(origin,std::numbers::pi-.05);
    const double oldSupport=std::abs(std::sin(angle+.05))*.6+
        std::abs(std::cos(angle+.05))*.4;
    const auto turnedRaw=u*.4+n*(.1+oldSupport+.06);
    setup(modulo,{turned,angledWall(angle)});
    ObjectDrag nearest(modulo,turned,true);
    nearest.start(origin,turnedRaw);
    expectAngle(nearest.preview_angle,angle+.05,"nearest modulo-pi rotation avoids an artwork flip");
    mouse(modulo,QEvent::MouseButtonRelease,turnedRaw);

    for (bool freeSymbol : {false,true}) {
        PlanCanvas framed;
        auto frameSource=rectangle("frame-object",freeSymbol ? "symbol" : "component",{-1,-1},1.2,.8);
        frameSource.resize_frame=CanvasSelectionFrame{{-1,-1},0,1.2,.8};
        setup(framed,{frameSource,wall()});
        ObjectDrag frameDrag(framed,frameSource,true);
        frameDrag.start({-.8,-.9},{1.54,.1});
        expectAngle(frameDrag.preview_angle,freeSymbol ? std::numbers::pi/2 : 0,
            "physical frame admits free symbols and excludes native components");
        expect(*frameDrag.preview,freeSymbol ? Vec2{2.6,.9} : Vec2{2.4,1},
            "semantic eligibility keeps exact frame proposal");
        mouse(framed,QEvent::MouseButtonRelease,{1.54,.1});
    }
}

void rotatingReleaseAndInvalidation() {
    const Vec2 origin{-1,-1},press{-.8,-.9},pointer{1.54,.1};
    for (int mode=0;mode<6;++mode) {
        PlanCanvas canvas;
        const auto source=symbol(origin);
        setup(canvas,{source,wall()});
        ObjectDrag drag(canvas,source,true);
        drag.start(press,pointer);
        expectAngle(drag.preview_angle,std::numbers::pi/2,"initial rotating contact");
        const auto serial=canvas.entitiesMovePreviewSerial();
        if (mode<4) {
            if (mode==1) canvas.setSnapEnabled(false);
            if (mode==2) canvas.setRawPointInput(true);
            const auto release=mode==3 ? Vec2{0,.1} : pointer;
            mouse(canvas,QEvent::MouseButtonRelease,release,
                mode==0 ? Qt::ShiftModifier : Qt::NoModifier);
            require(drag.commits==1,"changed release proposal commits once");
            expectAngle(drag.committed_angle,0,"release bypass/distance clears old angle");
            expect(*drag.committed,mode==3 ? gridPoint(canvas,release)-gridPoint(canvas,press)
                                          : pointer-press,"release recomputes unrotated translation");
        } else {
            if (mode==4) canvas.setEntities({source,wall(true,3)});
            else {
                QKeyEvent escape(QEvent::KeyPress,Qt::Key_Escape,Qt::NoModifier);
                QApplication::sendEvent(&canvas,&escape);
            }
            expectAngle(canvas.entitiesMovePreviewRotationRadians(),0,"cancel/source change clears angle");
            require(!canvas.completeEntitiesMovePreview(serial,
                std::vector<CanvasEntity>{transformed(source,{2.6,.9},std::numbers::pi/2)}),
                "canceled rotation rejects stale exact output");
            mouse(canvas,QEvent::MouseButtonRelease,pointer);
            require(drag.commits==0,"canceled rotation cannot commit");
        }
    }
    PlanCanvas changed;
    const auto source=symbol(origin);
    setup(changed,{source,wall()});
    ObjectDrag release(changed,source,true);
    release.start(press,{0,.1});
    expectAngle(release.preview_angle,0,"distant preview has no rotation");
    mouse(changed,QEvent::MouseButtonRelease,pointer);
    expectAngle(release.committed_angle,std::numbers::pi/2,"release position enables fresh rotation");
    expect(*release.committed,{2.6,.9},"release recomputes rotated center from final pointer");
}

void rotatingLibraryContacts() {
    constexpr double angle=std::numbers::pi/12;
    const Vec2 u{std::cos(angle),std::sin(angle)},n{-u.y,u.x};
    PlanCanvas canvas;
    setup(canvas,{angledWall(angle)});
    LibraryDrag library(canvas,0,false,true);
    const double support=std::abs(n.x)*.6+std::abs(n.y)*.4;
    const auto input=u*.4+n*(.1+support+.06);
    const auto raw=libraryPointer(canvas,input);
    const auto expected=u*dot(raw,u)+n*.5;
    library.enter(input); library.move(input);
    expect(*library.preview,expected,"library aligned center touches same physical face");
    expectAngle(library.preview_angle,angle,"library provider reads relative rotation");
    require(library.requested_angles.size()==4,"each library pointer requests raw then final geometry");
    expectAngle(library.requested_angles[0],0,"first library raw request has zero relative rotation");
    expectAngle(library.requested_angles[2],0,"new library raw request clears previous rotation");
    const auto shown=*library.preview;
    library.drop(input);
    expect(*library.committed,shown,"library preview and drop centers agree");
    expectAngle(library.committed_angle,angle,"library final callback gets relative angle");
    expectAngle(canvas.symbolDragPreviewRotationRadians(),0,"completed library drop clears angle");

    for (int mode=0;mode<6;++mode) {
        PlanCanvas vertical;
        setup(vertical,{wall()});
        LibraryDrag drag(vertical,0,false,true);
        drag.enter({1.34,0}); drag.move({1.34,0});
        expectAngle(drag.preview_angle,std::numbers::pi/2,"library perpendicular tie rotates positive ninety");
        expect(*drag.preview,{1.6,0},"library uses its center as the grab anchor");
        const auto serial=vertical.symbolDragPreviewSerial();
        if (mode<4) {
            if (mode==1) vertical.setSnapEnabled(false);
            if (mode==2) vertical.setRawPointInput(true);
            const Vec2 point=mode==3 ? Vec2{0,0} : Vec2{1.34,0};
            drag.drop(point,mode==0 ? Qt::ShiftModifier : Qt::NoModifier);
            expectAngle(drag.committed_angle,0,"library final bypass/distance clears old rotation");
            expect(*drag.committed,point,"library final bypass/distance recomputes raw center");
        } else {
            if (mode==4) vertical.setEntities({wall(true,3)});
            else {
                QDragLeaveEvent leave;
                QApplication::sendEvent(&vertical,&leave);
            }
            expectAngle(vertical.symbolDragPreviewRotationRadians(),0,"library source/leave clears angle");
            require(!vertical.completeSymbolDragPreview(serial,symbol({1.6,0},std::numbers::pi/2)),
                "retired library rotation rejects late geometry");
        }
    }
    PlanCanvas rawCanvas;
    setup(rawCanvas,{wall()});
    LibraryDrag rawDrag(rawCanvas,0,true,true);
    rawDrag.enter({1.34,0}); rawDrag.move({1.34,0}); rawDrag.drop({1.34,0});
    expectAngle(rawDrag.committed_angle,0,"hosted/raw symbol provider never auto-rotates");
    expect(*rawDrag.committed,{1.34,0},"hosted/raw symbol provider keeps original point");

    for (double scale : {50.,100.,200.}) {
        for (double gap : {10.,14.}) {
            PlanCanvas threshold;
            setup(threshold,{wall()},scale);
            LibraryDrag drag(threshold,0,false,true);
            const Vec2 point{1.4-gap/scale,0};
            drag.enter(point); drag.move(point);
            expectAngle(drag.preview_angle,gap<12 ? std::numbers::pi/2 : 0,
                "rotation admission uses original unrotated twelve-pixel gap at every zoom");
            drag.drop(point);
            expectAngle(drag.committed_angle,drag.preview_angle,"threshold release keeps exact angle");
        }
    }
}

void asynchronousRotationAndRotationOnly() {
    PlanCanvas canvas;
    const auto source=symbol({-1,-1});
    setup(canvas,{source,wall()});
    canvas.setSelectedId(source.id);
    std::uint64_t pending{};
    Vec2 proposed{},committed{};
    double proposedAngle{},committedAngle{};
    int commits=0;
    canvas.setEntitiesMovePreviewRequested([&](QStringList,Vec2 delta,std::uint64_t serial)
            -> std::optional<std::vector<CanvasEntity>> {
        pending=serial; proposed=delta; proposedAngle=canvas.entitiesMovePreviewRotationRadians();
        require(canvas.markEntitiesMovePreviewPending(serial),"async rotation marks current proposal pending");
        return std::nullopt;
    });
    canvas.setEntitiesMoveRequested([&](QStringList ids,Vec2 delta,double angle) {
        require(ids==QStringList{source.id},"async rotation retains captured ID");
        committed=delta; committedAngle=angle; ++commits;
        expectAngle(canvas.entitiesMovePreviewRotationRadians(),0,"async callback follows gesture reset");
        return true;
    });
    mouse(canvas,QEvent::MouseButtonPress,{-.8,-.9});
    mouse(canvas,QEvent::MouseMove,{1.54,.1});
    expectAngle(proposedAngle,std::numbers::pi/2,"async provider captures relative preview angle");
    mouse(canvas,QEvent::MouseButtonRelease,{1.54,.1});
    require(commits==0 && canvas.entitiesMovePreviewPending(),"released async edit waits for exact output");
    require(canvas.completeEntitiesMovePreview(pending,
        std::vector<CanvasEntity>{transformed(source,proposed,proposedAngle)}),
        "released rotation accepts current exact output");
    QApplication::processEvents();
    require(commits==1,"async rotated edit commits once after admission");
    expect(committed,proposed,"async rotation commit preserves exact preview delta");
    expectAngle(committedAngle,proposedAngle,"async commit copies angle before reset");
    require(!canvas.completeEntitiesMovePreview(pending,
        std::vector<CanvasEntity>{transformed(source,proposed,proposedAngle)}),
        "completed async rotation cannot revive its old serial");

    // A narrow width/depth difference admits the old footprint within 12 px,
    // while the aligned final centre is precisely the original centre.
    auto squareish=symbol({1.6,0});
    squareish.svg_symbol->width_metres=.9;
    squareish.segments=rectangle(squareish.id,"symbol",{1.6,0},.9,.8).segments;
    PlanCanvas rotationOnly;
    setup(rotationOnly,{squareish,wall()});
    ObjectDrag only(rotationOnly,squareish,true);
    only.start({1.6,0},{0,0});
    mouse(rotationOnly,QEvent::MouseMove,{1.6,0});
    expect(*only.preview,{0,0},"rotation-only proposal has no translation");
    expectAngle(only.preview_angle,std::numbers::pi/2,"rotation-only proposal has a real turn");
    mouse(rotationOnly,QEvent::MouseButtonRelease,{1.6,0});
    require(only.commits==1,"nonzero rotation is not discarded by no-motion commit check");
    expectAngle(only.committed_angle,std::numbers::pi/2,"rotation-only commit preserves angle");
}

void independentProviderCapabilities() {
    const auto source=symbol({-1,-1});
    PlanCanvas moveCanvas;
    setup(moveCanvas,{source,wall()});
    ObjectDrag moving(moveCanvas,source,true);
    LibraryDrag legacyLibrary(moveCanvas);
    moving.start({-.8,-.9},{1.54,.1});
    expectAngle(moving.preview_angle,std::numbers::pi/2,"legacy library setter preserves move capability");
    mouse(moveCanvas,QEvent::MouseButtonRelease,{1.54,.1});

    PlanCanvas libraryCanvas;
    setup(libraryCanvas,{source,wall()});
    LibraryDrag library(libraryCanvas,0,false,true);
    ObjectDrag legacyMove(libraryCanvas,source);
    library.enter({1.34,0}); library.move({1.34,0}); library.drop({1.34,0});
    expectAngle(library.committed_angle,std::numbers::pi/2,"legacy move setter preserves library capability");
}
} // namespace

int main(int argc,char** argv) {
    sketch::testing::noninteractive_errors();
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM","offscreen");
    QApplication app(argc,argv);
    try {
        libraryContacts();
        bypassAndGrid();
        wallOutlineAndSceneReplacement();
        existingObjectCases();
        finalPositionAndInvalidation();
        rigidGroupContact();
        rigidGroupContact(true);
        rotatingObjectContacts();
        rotatingReleaseAndInvalidation();
        rotatingLibraryContacts();
        asynchronousRotationAndRotationOnly();
        independentProviderCapabilities();
        std::cout << "object_drag_snap_canvas_tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "object_drag_snap_canvas_tests: " << error.what() << '\n';
        return 1;
    }
}
