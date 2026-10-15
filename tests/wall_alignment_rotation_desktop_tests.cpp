#include "sketch/desktop/main_window.hpp"
#include "sketch/annotation_entity_codec.hpp"
#include "../src/desktop/plan_canvas.hpp"
#include "support/noninteractive_errors.hpp"

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QMimeData>
#include <QMouseEvent>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QUuid>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {
using namespace sketch;
using namespace sketch::desktop;
constexpr double angle = 0.26179938779914943654; // +15 degrees, nearest modulo pi to zero.
const Vec2 tangent{std::cos(angle), std::sin(angle)};
const Vec2 normal{-tangent.y, tangent.x};
Vec2 operator+(Vec2 a, Vec2 b) { return {a.x+b.x,a.y+b.y}; }
Vec2 operator-(Vec2 a, Vec2 b) { return {a.x-b.x,a.y-b.y}; }
Vec2 operator*(Vec2 p, double scale) { return {p.x*scale,p.y*scale}; }
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
bool close_enough(double a, double b) { return std::abs(a-b) < 1e-7; }
double dot(Vec2 a, Vec2 b) { return a.x*b.x+a.y*b.y; }
Vec2 rotated(Vec2 p) {
    return {std::cos(angle)*p.x-std::sin(angle)*p.y,
            std::sin(angle)*p.x+std::cos(angle)*p.y};
}
template<class Predicate> void await(Predicate ready, const char* message) {
    QElapsedTimer timer;
    timer.start();
    do {
        QApplication::processEvents(QEventLoop::AllEvents, 20);
        if (ready()) return;
    } while (timer.elapsed() < 5000);
    require(false, message);
}
CanvasSvgSymbol retained(PlanCanvas& canvas, const QString& id) {
    const auto& entities=canvas.entities();
    const auto found=std::find_if(entities.begin(),entities.end(),
        [&](const auto& item){return item.id==id;});
    require(found!=entities.end() && found->svg_symbol,"retained sofa SVG missing");
    return *found->svg_symbol;
}
SymbolInstance persisted(MainWindow& window, const QString& id) {
    const auto snapshot=window.document().snapshot();
    for (const auto& [key,entity] : snapshot.entities()) {
        (void)key;
        if (entity.type!=kAnnotationEntityType) continue;
        for (const auto& symbol : decode_annotation_entity(entity).symbols)
            if (symbol.id==id.toStdString()) return symbol;
    }
    throw std::runtime_error("persisted sofa missing");
}
void capture(MainWindow& window, const QString& name) {
    const auto directory=qEnvironmentVariable("VERTEX_TEST_CAPTURE_DIR");
    if (directory.isEmpty()) return;
    require(QDir().mkpath(directory) && window.grab().save(QDir(directory).filePath(name)),
            "full-window wall alignment capture failed");
}
void unchangedArtwork(const SymbolInstance& actual, const SymbolInstance& before) {
    require(actual.pinned_svg==before.pinned_svg && !actual.pinned_svg.empty() &&
        close_enough(actual.placement.scale,before.placement.scale) &&
        close_enough(actual.width_scale,before.width_scale) &&
        close_enough(actual.depth_scale,before.depth_scale) &&
        actual.flip_horizontal==before.flip_horizontal && actual.flip_vertical==before.flip_vertical,
        "wall alignment changed SVG bytes, scale, proportions or flips");
}
void placement(const SymbolInstance& actual, Vec2 position, double rotation) {
    require(close_enough(actual.placement.position.x,position.x) &&
        close_enough(actual.placement.position.y,position.y) &&
        close_enough(actual.placement.rotation_radians,rotation),
        "persisted position and rotation did not agree with expected placement");
}
struct Fixture {
    MainWindow window;
    PlanCanvas* canvas{};
    explicit Fixture(bool design=false, bool named=false, bool curved=false) {
        window.setWorkspace(design ? Workspace::architectural : Workspace::measurement);
        window.resize(1500,1000); window.show(); QApplication::processEvents();
        auto* thickness=window.findChild<QLineEdit*>("wallDrawThickness");
        require(thickness,"wall thickness control missing");
        thickness->setText("200 mm");
        const auto wall=curved ? window.createCurvedWall({-4,0},{4,0},"180 deg") :
            window.createStraightWall(tangent*(-4),tangent*4);
        require(!wall.isEmpty(),"physical wall fixture failed");
        require(close_enough(window.document().snapshot().entities().at(wall.toStdString())
            .properties.at("thickness_m").get<double>(),.2),"wall fixture thickness is not 0.2 m");
        canvas=dynamic_cast<PlanCanvas*>(window.findChild<QWidget*>(
            design ? "architecturalPlanCanvas" : "measurementPlanCanvas"));
        require(canvas && canvas->isVisible(),"visible plan canvas missing");
        if (named) {
            auto* views=window.findChild<QComboBox*>("architecturalView");
            require(views,"named plan selector missing");
            const auto index=views->findData(QStringLiteral("view-plan"),Qt::UserRole+1);
            require(index>=3,"persisted named plan missing");
            views->setCurrentIndex(index);
            QApplication::processEvents();
        }
    }
    void configure() {
        canvas->setTool(CanvasTool::select); canvas->setGridEnabled(false);
        canvas->setSnapEnabled(true); canvas->setWallSnapEnabled(true);
        canvas->fitView(); QApplication::processEvents();
    }
    QPointF screen(Vec2 point) const {
        const auto center=canvas->viewCenter();
        return QRectF(canvas->rect()).center()+QPointF(
            (point.x-center.x)*canvas->viewScale(),-(point.y-center.y)*canvas->viewScale());
    }
    void mouse(QEvent::Type type, QPointF point, Qt::KeyboardModifiers modifiers) {
        QMouseEvent event(type,point,canvas->mapToGlobal(point.toPoint()),
            type==QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
            type==QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton,modifiers);
        QApplication::sendEvent(canvas,&event);
    }
};

void existingObject(bool design=false, bool named=false, bool shift=false,
                    double gap_pixels=6, bool curved=false) {
    Fixture f(design,named,curved);
    const Vec2 original{1.2,2.5}, grab{.23,.13};
    const auto id=f.window.createAnnotationSymbol("svg-v2-04_living-sofa-three-seat",original);
    require(!id.isEmpty(),"genuine sofa fixture failed");
    QTemporaryDir directory;
    require(directory.isValid(),"temporary project directory failed");
    const auto path=directory.filePath("wall-alignment.bldproj");
    require(f.window.saveProjectAs(path) && f.window.openProject(path),"fixture save/reopen failed");
    auto* undo=f.window.findChild<QAction*>("undoCommand");
    require(undo && !undo->isEnabled(),"fixture history must start empty");
    require(f.window.selectEntity(id),"sofa selection failed");
    f.configure();
    const auto before=retained(*f.canvas,id);
    const auto saved=persisted(f.window,id);
    require(close_enough(before.rotation_radians,0) && close_enough(before.width_metres,2.25) &&
        close_enough(before.depth_metres,.95) && before.document.contains("sofa-three-seat--artwork"),
        "fixture must load the genuine unrotated 2.25 by 0.95 m SVG sofa");
    // Attraction is admitted using ORIGINAL unrotated support, not rotated support.
    const auto support=std::abs(normal.x)*before.width_metres/2+
                       std::abs(normal.y)*before.depth_metres/2;
    const auto raw=curved ? Vec2{0,-4-.1-before.depth_metres/2-gap_pixels/f.canvas->viewScale()} :
        tangent*.83+normal*(support+.1+gap_pixels/f.canvas->viewScale());
    const bool align=!shift && gap_pixels<12 && !curved;
    // Rotate about the pressed local point first, then correct only along normal.
    const auto pivot_center=raw+grab-rotated(grab);
    const auto step=f.canvas->gridSpacingMetres();
    const auto grid=[step](Vec2 p) {
        return Vec2{std::round(p.x/step)*step,std::round(p.y/step)*step};
    };
    const auto expected=align ? tangent*dot(pivot_center,tangent)+normal*(.1+before.depth_metres/2) :
        shift ? raw : original+grid(raw+grab)-grid(original+grab);
    const auto press=f.screen(original+grab), target=f.screen(raw+grab);
    require(QRectF(f.canvas->rect()).adjusted(15,15,-15,-15).contains(press) &&
        QRectF(f.canvas->rect()).adjusted(15,15,-15,-15).contains(target),"drag fixture clipped");
    const auto modifiers=shift ? Qt::ShiftModifier : Qt::NoModifier;
    f.mouse(QEvent::MouseButtonPress,press,modifiers);
    f.mouse(QEvent::MouseMove,target,modifiers);
    await([&]{return !f.canvas->entitiesMovePreviewPending() &&
        !f.canvas->entitiesMovePreview().empty();},"MainWindow drag preview missing");
    const auto previews=f.canvas->entitiesMovePreview();
    const auto found=std::find_if(previews.begin(),previews.end(),[&](const auto& item){return item.id==id;});
    require(found!=previews.end() && found->svg_symbol,"preview sofa artwork missing");
    const auto live=*found->svg_symbol;
    const auto prefix=QString(named ? "named-plan" : design ? "design" : "measure")+
        (curved ? "-curved" : shift ? "-shift" : gap_pixels>12 ? "-distant" : "-aligned");
    capture(f.window,prefix+"-preview.png");
    // First behavioral assertion intentionally exposes the baseline's missing rotation.
    require(close_enough(live.rotation_radians,align ? angle : 0),
        "wall alignment preview must rotate width axis by nearest modulo-pi delta +15 degrees; bypass stays zero");
    require(close_enough(live.position.x,expected.x) && close_enough(live.position.y,expected.y),
        "preview position lost physical contact or rotated grabbed-point pivot");
    if (align) require(close_enough(dot(live.position,normal)-before.depth_metres/2,.1) &&
        close_enough(dot(live.position,tangent),dot(raw+grab,tangent)-dot(rotated(grab),tangent)),
        "aligned footprint must touch physical face and preserve tangential pressed point");
    placement(persisted(f.window,id),original,0);
    const auto source=retained(*f.canvas,id);
    require(close_enough(source.position.x,original.x) && close_enough(source.position.y,original.y) &&
        close_enough(source.rotation_radians,0),"preview mutated retained source placement");
    require(live.document==before.document && live.flip_horizontal==before.flip_horizontal &&
        live.flip_vertical==before.flip_vertical && close_enough(live.width_metres,before.width_metres) &&
        close_enough(live.depth_metres,before.depth_metres),"preview mutated artwork or physical dimensions");
    f.mouse(QEvent::MouseButtonRelease,target,modifiers);
    await([&]{return !f.canvas->entitiesMovePreviewPending() && undo->isEnabled();},"drag did not commit");
    placement(persisted(f.window,id),live.position,live.rotation_radians);
    unchangedArtwork(persisted(f.window,id),saved);
    const auto committed=retained(*f.canvas,id);
    require(close_enough(committed.rotation_radians,live.rotation_radians) &&
        close_enough(committed.position.x,live.position.x) && close_enough(committed.position.y,live.position.y) &&
        committed.document==before.document,"retained commit differs from preview");
    capture(f.window,prefix+"-committed.png");
    require(f.window.undoCommand(),"placement cannot undo");
    placement(persisted(f.window,id),original,0);
    require(!undo->isEnabled(),"position plus rotation must be one undo command");
    unchangedArtwork(persisted(f.window,id),saved);
    require(f.window.redoCommand(),"placement cannot redo");
    placement(persisted(f.window,id),expected,align ? angle : 0);
    require(f.window.saveProject() && f.window.openProject(path),"placement save/reopen failed");
    placement(persisted(f.window,id),expected,align ? angle : 0);
    unchangedArtwork(persisted(f.window,id),saved);
    require(!undo->isEnabled(),"reopened placement must have empty undo history");
}

void libraryObject() {
    Fixture f;
    f.configure(); f.canvas->setViewTransform({0,0},100);
    QApplication::processEvents();
    QMimeData mime;
    mime.setData("application/x-vertex-symbol",QJsonDocument(QJsonObject{
        {"id","svg-v2-04_living-sofa-three-seat"},{"scale",1.0}}).toJson(QJsonDocument::Compact));
    const auto support=std::abs(normal.x)*2.25/2+std::abs(normal.y)*.95/2;
    const auto point=f.screen(tangent*.83+normal*(support+.1+6/f.canvas->viewScale())).toPoint();
    const auto center=QRectF(f.canvas->rect()).center();
    const Vec2 raw{f.canvas->viewCenter().x+(point.x()-center.x())/f.canvas->viewScale(),
                   f.canvas->viewCenter().y-(point.y()-center.y())/f.canvas->viewScale()};
    require(std::abs(dot(raw,normal)-support-.1)*f.canvas->viewScale()<12,
        "rounded library pointer must approach original footprint within twelve pixels");
    const auto source=f.window.document().snapshot();
    QDragEnterEvent enter(point,Qt::CopyAction,&mime,Qt::LeftButton,Qt::NoModifier);
    QApplication::sendEvent(f.canvas,&enter);
    QDragMoveEvent move(point,Qt::CopyAction,&mime,Qt::LeftButton,Qt::NoModifier);
    QApplication::sendEvent(f.canvas,&move);
    require(enter.isAccepted() && move.isAccepted() && f.canvas->symbolDragPreviewActive(),
        "actual native library drag preview missing");
    require(f.window.document().snapshot().entities()==source.entities(),"library preview mutated source");
    capture(f.window,"library-aligned-preview.png");
    QDropEvent drop(QPointF(point),Qt::CopyAction,&mime,Qt::LeftButton,Qt::NoModifier);
    QApplication::sendEvent(f.canvas,&drop);
    require(drop.isAccepted(),"native library drop rejected");
    const auto& entities=f.canvas->entities();
    const auto found=std::find_if(entities.begin(),entities.end(),[](const auto& item){return item.svg_symbol.has_value();});
    require(found!=entities.end(),"library drop omitted sofa");
    const auto svg=*found->svg_symbol;
    require(close_enough(svg.rotation_radians,angle),"library drop must persist +15 degree wall alignment");
    require(close_enough(svg.width_metres,2.25) && close_enough(svg.depth_metres,.95) &&
        close_enough(dot(svg.position,normal)-.95/2,.1) &&
        close_enough(dot(svg.position,tangent),dot(raw,tangent)),"library placement lost physical face contact");
    placement(persisted(f.window,found->id),svg.position,angle);
    capture(f.window,"library-aligned-committed.png");
    require(f.window.undoCommand() && f.window.document().snapshot().entities()==source.entities(),
        "one undo must restore exact source before library drop");
}
} // namespace

int main(int argc, char** argv) {
    sketch::testing::noninteractive_errors();
    QStandardPaths::setTestModeEnabled(true);
    QApplication app(argc,argv);
    QCoreApplication::setApplicationName(QStringLiteral("Vertex-wall-alignment-test-")+
        QUuid::createUuid().toString(QUuid::WithoutBraces));
    try {
        existingObject(); existingObject(true); existingObject(true,true);
        libraryObject(); existingObject(false,false,true);
        existingObject(false,false,false,24); existingObject(false,false,false,6,true);
        std::cout << "wall alignment rotation desktop checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "wall_alignment_rotation_desktop_tests: " << error.what() << '\n';
        return 1;
    }
}
