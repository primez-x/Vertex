#include "sketch/desktop/main_window.hpp"
#include "../src/desktop/plan_canvas.hpp"
#include "support/noninteractive_errors.hpp"

#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QFontDatabase>
#include <QKeyEvent>
#include <QPainter>
#include <QUuid>
#include <QMouseEvent>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {
using namespace sketch;
using namespace sketch::desktop;
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
bool closeEnough(double a, double b) { return std::abs(a-b) < 1e-8; }
bool closeEnough(Vec2 a, Vec2 b) { return closeEnough(a.x,b.x) && closeEnough(a.y,b.y); }
PlanCanvas& canvas(MainWindow& window, const char* name = "measurementPlanCanvas") {
    auto* result = dynamic_cast<PlanCanvas*>(window.findChild<QWidget*>(name));
    require(result != nullptr, "opening canvas is missing");
    return *result;
}
CanvasEntity retained(MainWindow& window, const QString& id, const char* name = "measurementPlanCanvas") {
    const auto& entities = canvas(window,name).entities();
    const auto found = std::find_if(entities.begin(),entities.end(),[&](const auto& e){return e.id==id;});
    require(found != entities.end(), "opening or host projection is missing");
    return *found;
}
nlohmann::json properties(MainWindow& window, const QString& id) {
    return window.document().snapshot().entities().at(id.toStdString()).properties;
}
void dimensions(MainWindow& window, const QString& id, double offset, double width) {
    const auto p = properties(window,id);
    require(closeEnough(p.at("offset_m").get<double>(),offset) && closeEnough(p.at("width_m").get<double>(),width),
            "mouse resize did not persist the expected offset and width");
}
void mouse(PlanCanvas& target, QEvent::Type type, QPointF point) {
    QMouseEvent event(type,point,target.mapToGlobal(point.toPoint()),
        type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
        type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton,Qt::NoModifier);
    QApplication::sendEvent(&target,&event);
    QApplication::processEvents();
}
QImage committedOutput(PlanCanvas& view) {
    QImage image(800,600,QImage::Format_ARGB32_Premultiplied);
    QPainter painter(&image);
    view.renderSceneAt(painter,QRectF(image.rect()),45.0,{4,0},Qt::white);
    painter.end();
    return image;
}
struct Fixture {
    MainWindow window;
    QString wall, opening;
    Vec2 direction;
    double scale{};
    Fixture(const QString& kind, bool rotated, bool sibling = false) : direction(rotated ? Vec2{0,1} : Vec2{1,0}) {
        window.setMetricUnits(true);
        window.resize(1500,1000);
        window.show();
        QApplication::processEvents();
        wall = window.createStraightWall({0,0},{direction.x*8,direction.y*8});
        require(!wall.isEmpty() && window.selectEntity(wall), "opening fixture wall creation failed");
        require(window.editSelectedHeight("3.2 m"), "fixture wall height edit failed");
        opening = window.createHostedOpening(kind,"2 m","1 m",kind=="window" ? "1 m" : "0 m","2 m",
            std::nullopt,kind=="door" ? std::optional<DoorOperation>{DoorOperation{false,true,90}} : std::nullopt);
        require(!opening.isEmpty(), "opening fixture creation failed");
        if (sibling) {
            require(window.selectEntity(wall) && !window.createHostedOpening("opening","4 m","1 m","0 m","2 m").isEmpty(),
                    "sibling opening fixture creation failed");
        }
        auto& view = canvas(window);
        view.setSnapEnabled(false);
        view.setOverviewMapEnabled(false);
        require(window.selectEntity(wall), "cannot select host for camera calibration");
        view.fitView();
        const auto frame = view.selectionBounds();
        require(frame.has_value(), "host has no camera calibration frame");
        const auto host = retained(window,wall);
        const auto bounds = boundary_bounds(host.segments);
        const auto span = rotated ? bounds.maximum.y-bounds.minimum.y : bounds.maximum.x-bounds.minimum.x;
        // selectionControlRect adds 6 px on each side and half the stroke.
        const bool baseline = host.segments.size()==1;
        const auto stroke = baseline ? std::max(host.thickness_metres,.04) : host.stroke_width_metres;
        const auto fixed_padding = stroke>0 ? 12.0 : 15.0;
        scale = ((rotated ? frame->height() : frame->width())-fixed_padding)/(span+std::max(stroke,0.0));
        require(scale>0 && window.selectEntity(opening), "opening camera calibration failed");
        require(retained(window,opening).opening_width_controls.has_value(), "selected plan opening has no width handles");
    }
    QPointF screen(Vec2 point) {
        const auto& view = canvas(window);
        const auto center = view.viewCenter();
        return QRectF(view.rect()).center()+QPointF((point.x-center.x)*scale,-(point.y-center.y)*scale);
    }
    QPointF pin(bool start) {
        const auto controls = retained(window,opening).opening_width_controls.value();
        return screen(start ? controls.start_jamb : controls.end_jamb);
    }
    QPointF target(bool start, double width) {
        const auto controls = retained(window,opening).opening_width_controls.value();
        const auto opposite = start ? controls.end_jamb : controls.start_jamb;
        const auto sign = start ? -1.0 : 1.0;
        return screen({opposite.x+sign*direction.x*width,opposite.y+sign*direction.y*width});
    }
    void drag(bool start, double width) {
        auto& view = canvas(window);
        const auto from=pin(start), to=target(start,width);
        const auto output=committedOutput(view);
        const auto revision=window.document().revision();
        mouse(view,QEvent::MouseButtonPress,from);
        mouse(view,QEvent::MouseMove,to);
        require(window.document().revision()==revision,"drag preview mutated the document before release");
        require(committedOutput(view)==output,"interactive opening preview leaked into print/export rendering");
        const auto capture=qEnvironmentVariable("VERTEX_TEST_CAPTURE_DIR");
        if (!capture.isEmpty() && !start && direction.x==1 && closeEnough(width,1.5)) {
            require(QDir().mkpath(capture) && view.grab().save(QDir(capture).filePath(
                QString("opening-%1-preview.png").arg(properties(window,opening).at("opening_kind").get<std::string>().c_str()))),
                "opening drag preview capture failed");
        }
        mouse(view,QEvent::MouseButtonRelease,to);
    }
};
bool sameBoundary(const Boundary& a, const Boundary& b) {
    if (a.size()!=b.size()) return false;
    for (std::size_t i=0;i<a.size();++i)
        if (!closeEnough(a[i].start,b[i].start) || !closeEnough(a[i].end,b[i].end) || !closeEnough(a[i].sweep_radians,b[i].sweep_radians)) return false;
    return true;
}
void successfulGestures() {
    for (const auto& kind : {QString("door"),QString("window"),QString("opening")})
        for (bool rotated : {false,true}) for (bool start : {false,true}) {
            Fixture f(kind,rotated);
            const auto original=properties(f.window,f.opening);
            const auto host_before=retained(f.window,f.wall).segments;
            const auto controls=retained(f.window,f.opening).opening_width_controls.value();
            require(closeEnough(controls.start_jamb,Vec2{2*f.direction.x,2*f.direction.y}) &&
                    closeEnough(controls.end_jamb,Vec2{3*f.direction.x,3*f.direction.y}),
                    "opening handles are displaced from the exact model jambs");
            const auto revision=f.window.document().revision();
            f.drag(start,1.5);
            require(f.window.document().revision()==revision+1, "completed resize must issue one document command");
            dimensions(f.window,f.opening,start ? 1.5 : 2,1.5);
            const auto changed=properties(f.window,f.opening);
            const auto resized=retained(f.window,f.opening);
            const auto after=resized.opening_width_controls.value();
            require(closeEnough(start ? after.end_jamb : after.start_jamb,start ? controls.end_jamb : controls.start_jamb),
                    "resize moved the opposite jamb");
            require(!sameBoundary(host_before,retained(f.window,f.wall).segments),
                    "host plan footprint was not regenerated after opening resize");
            if (kind=="door") {
                const auto arc=std::find_if(resized.segments.begin(),resized.segments.end(),[](const auto& s){return std::abs(s.sweep_radians)>1e-8;});
                require(arc!=resized.segments.end() && closeEnough(std::abs(arc->sweep_radians),std::acos(-1.0)/2),
                        "door resize lost its analytic quarter-circle swing");
                require(closeEnough(std::hypot(arc->end.x-arc->start.x,arc->end.y-arc->start.y),1.5*std::sqrt(2.0)),
                        "door swing arc was distorted rather than rebuilt at the new radius");
                require(changed.at("door_operation")==original.at("door_operation"),"resize altered door operation");
            }
            require(f.window.undoCommand() && properties(f.window,f.opening)==original,"undo did not restore exact opening properties");
            require(sameBoundary(host_before,retained(f.window,f.wall).segments),"undo did not restore host footprint");
            require(f.window.redoCommand() && properties(f.window,f.opening)==changed,"redo did not restore exact resized opening");
            QTemporaryDir directory;
            require(directory.isValid(),"temporary project directory unavailable");
            const auto path=directory.filePath("opening-width.bldproj");
            require(f.window.saveProjectAs(path) && f.window.openProject(path),"resized opening failed save/reopen");
            require(properties(f.window,f.opening)==changed,"save/reopen changed exact width or offset");
            require(f.window.selectEntity(f.opening),"reopened opening cannot be selected");
            f.window.setWorkspace(Workspace::architectural);
            QApplication::processEvents();
            require(retained(f.window,f.opening,"architecturalPlanCanvas").opening_width_controls.has_value(),
                    "architectural plan opening lacks width handles");
            const auto capture=qEnvironmentVariable("VERTEX_TEST_CAPTURE_DIR");
            if (!capture.isEmpty()) {
                require(QDir().mkpath(capture) && canvas(f.window,"architecturalPlanCanvas").grab().save(QDir(capture).filePath(
                    QString("opening-%1-%2-%3.png").arg(kind).arg(rotated ? "rotated" : "horizontal").arg(start ? "start" : "end"))),
                    "opening screenshot capture failed");
            }
            auto* views=f.window.findChild<QComboBox*>("architecturalView");
            require(views!=nullptr && views->count()>=3,"architectural view selector missing");
            views->setCurrentIndex(2);
            QApplication::processEvents();
            const auto& section_entities=canvas(f.window,"architecturalPlanCanvas").entities();
            require(std::none_of(section_entities.begin(),section_entities.end(),[](const auto& entity) {
                return entity.opening_width_controls.has_value();
            }),"section projection exposes plan opening width handles");
        }
}
void rejectedAndStaleGestures() {
    Fixture f("door",false,true);
    const auto original=properties(f.window,f.opening);
    auto revision=f.window.document().revision();
    f.drag(true,4); // End remains at 3 m, so the start would be outside the host.
    require(f.window.document().revision()==revision && properties(f.window,f.opening)==original,"out-of-host drag mutated the document");
    require(f.window.selectEntity(f.opening),"reselect rejected opening failed");
    f.drag(false,2.5); // End at 4.5 m overlaps sibling [4,5].
    require(f.window.document().revision()==revision && properties(f.window,f.opening)==original,"colliding resize mutated the document");
    require(f.window.selectEntity(f.opening),"reselect frame-constrained opening failed");
    f.drag(false,.1);
    require(f.window.document().revision()==revision && properties(f.window,f.opening)==original,
            "resize smaller than the manufactured frame mutated the document");
    require(f.window.selectEntity(f.opening),"reselect no-move opening failed");
    auto& view=canvas(f.window);
    auto pin=f.pin(false);
    mouse(view,QEvent::MouseButtonPress,pin);
    mouse(view,QEvent::MouseButtonRelease,pin);
    require(f.window.document().revision()==revision && properties(f.window,f.opening)==original,"release without movement issued a resize command");
    // A touch-sized hit halo must not jump the jamb on the first move.
    const auto halo=QPointF(5,0);
    mouse(view,QEvent::MouseButtonPress,f.pin(false)+halo);
    mouse(view,QEvent::MouseMove,f.target(false,1.5)+halo);
    mouse(view,QEvent::MouseButtonRelease,f.target(false,1.5)+halo);
    dimensions(f.window,f.opening,2,1.5);
    require(f.window.undoCommand() && properties(f.window,f.opening)==original &&
            f.window.selectEntity(f.opening),"halo gesture did not undo cleanly");
    revision=f.window.document().revision();
    pin=f.pin(false);
    mouse(view,QEvent::MouseButtonPress,pin);
    mouse(view,QEvent::MouseMove,f.target(false,1.5));
    QKeyEvent escape(QEvent::KeyPress,Qt::Key_Escape,Qt::NoModifier);
    QApplication::sendEvent(&view,&escape);
    mouse(view,QEvent::MouseButtonRelease,f.target(false,1.5));
    require(f.window.document().revision()==revision && properties(f.window,f.opening)==original,
            "Escape did not cancel the jamb resize");
    require(f.window.selectEntity(f.opening),"reselect release-only opening failed");
    mouse(view,QEvent::MouseButtonPress,f.pin(false));
    mouse(view,QEvent::MouseButtonRelease,f.target(false,1.5));
    dimensions(f.window,f.opening,2,1.5);
    require(f.window.undoCommand() && properties(f.window,f.opening)==original &&
            f.window.selectEntity(f.opening),"release-only gesture did not undo cleanly");
    pin=f.pin(false);
    const auto target=f.target(false,1.5);
    mouse(view,QEvent::MouseButtonPress,pin);
    mouse(view,QEvent::MouseMove,target);
    require(!f.window.createStraightWall({0,10},{2,10}).isEmpty(),"intervening source revision fixture failed");
    revision=f.window.document().revision();
    mouse(view,QEvent::MouseButtonRelease,target);
    require(f.window.document().revision()==revision && properties(f.window,f.opening)==original,"stale active resize mutated the opening");
}
} // namespace

int main(int argc, char** argv) {
    sketch::testing::noninteractive_errors();
    QStandardPaths::setTestModeEnabled(true);
    QApplication app(argc,argv);
    QCoreApplication::setApplicationName("Vertex-opening-resize-" + QUuid::createUuid().toString(QUuid::WithoutBraces));
    const auto font_id = QFontDatabase::addApplicationFont(":/fonts/Inter.ttf");
    if (font_id >= 0) QApplication::setFont(QFont(QFontDatabase::applicationFontFamilies(font_id).front(),10));
    try {
        successfulGestures();
        rejectedAndStaleGestures();
        std::cout << "hosted opening resize desktop checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "hosted_opening_resize_desktop_tests: " << error.what() << '\n';
        return 1;
    }
}
