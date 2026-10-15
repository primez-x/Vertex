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
    double angle{};
    LibraryDrag(PlanCanvas& target, double rotation = 0, bool raw = false)
        : canvas(target), angle(rotation) {
        mime.setData("application/x-vertex-symbol", QJsonDocument(QJsonObject{
            {"id","fixture-symbol"},{"scale",1.0}}).toJson(QJsonDocument::Compact));
        canvas.setSymbolDragPreviewRequested([this](const QString&, double, Vec2 point) {
            preview=point;
            return std::optional<CanvasEntity>{symbol(point,angle)};
        });
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
    int commits{};
    ObjectDrag(PlanCanvas& target, CanvasEntity entity) : canvas(target),source(std::move(entity)) {
        canvas.setSelectedId(source.id);
        canvas.setEntitiesMovePreviewRequested([this](QStringList ids,Vec2 delta,std::uint64_t) {
            require(ids==QStringList{source.id},"move provider receives the captured selected ID");
            preview=delta;
            return std::optional<std::vector<CanvasEntity>>{
                std::vector<CanvasEntity>{translated(source,delta)}};
        });
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
void rigidGroupContact() {
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
        return std::optional<std::vector<CanvasEntity>>{{translated(first,delta),translated(second,delta)}};
    });
    canvas.setEntitiesMoveRequested([&](QStringList ids,Vec2 delta) {
        require(ids.size()==2,"group commit retains both semantic IDs");
        committed=delta; return true;
    });
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
        std::cout << "object_drag_snap_canvas_tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "object_drag_snap_canvas_tests: " << error.what() << '\n';
        return 1;
    }
}
