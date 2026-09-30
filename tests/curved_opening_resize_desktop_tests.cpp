#include "sketch/desktop/main_window.hpp"
#include "../src/desktop/plan_canvas.hpp"
#include "support/noninteractive_errors.hpp"

#include <QApplication>
#include <QDir>
#include <QFontDatabase>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QUuid>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <numbers>
#include <stdexcept>

namespace {
using namespace sketch;
using namespace sketch::desktop;
constexpr double pi = std::numbers::pi;
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
bool closeEnough(double a, double b) { return std::abs(a-b)<1e-7; }
bool closeEnough(Vec2 a, Vec2 b) { return closeEnough(a.x,b.x)&&closeEnough(a.y,b.y); }
double distance(Vec2 a, Vec2 b) { return std::hypot(a.x-b.x,a.y-b.y); }
PlanCanvas& canvas(MainWindow& window) {
    auto* view=dynamic_cast<PlanCanvas*>(window.findChild<QWidget*>("measurementPlanCanvas"));
    require(view!=nullptr,"measurement plan canvas missing");
    return *view;
}
CanvasEntity retained(MainWindow& window, const QString& id) {
    const auto& entities=canvas(window).entities();
    auto found=std::find_if(entities.begin(),entities.end(),[&](const auto& e){return e.id==id;});
    require(found!=entities.end(),"curved host or opening absent from plan");
    return *found;
}
nlohmann::json properties(MainWindow& window,const QString& id) {
    return window.document().snapshot().entities().at(id.toStdString()).properties;
}
void mouse(PlanCanvas& view,QEvent::Type type,QPointF point) {
    QMouseEvent event(type,point,view.mapToGlobal(point.toPoint()),
        type==QEvent::MouseMove?Qt::NoButton:Qt::LeftButton,
        type==QEvent::MouseButtonRelease?Qt::NoButton:Qt::LeftButton,Qt::NoModifier);
    QApplication::sendEvent(&view,&event);
    QApplication::processEvents();
}
QImage committedOutput(PlanCanvas& view) {
    QImage image(800,600,QImage::Format_ARGB32_Premultiplied);
    QPainter painter(&image);
    view.renderSceneAt(painter,QRectF(image.rect()),35,{0,0},Qt::white);
    painter.end();
    return image;
}
// Independent circle reconstruction keeps the desktop checks from merely
// echoing the production station helper.
struct Circle { Vec2 center; double radius; };
Circle circle(const Segment& arc) {
    const auto dx=arc.end.x-arc.start.x,dy=arc.end.y-arc.start.y;
    const auto factor=.5/std::tan(arc.sweep_radians/2);
    const Vec2 center{(arc.start.x+arc.end.x)/2-dy*factor,
                      (arc.start.y+arc.end.y)/2+dx*factor};
    return {center,distance(center,arc.start)};
}
struct Fixture {
    MainWindow window;
    QString wall,opening,kind;
    Segment baseline;
    Circle host_circle;
    double scale{};
    Fixture(QString opening_kind,double sweep,double rotation,Vec2 translation,bool sibling=false)
        : kind(std::move(opening_kind)) {
        window.setMetricUnits(true);
        window.resize(1500,1000);
        window.show();
        QApplication::processEvents();
        const auto transform=[&](Vec2 p){return Vec2{translation.x+p.x*std::cos(rotation)-p.y*std::sin(rotation),
            translation.y+p.x*std::sin(rotation)+p.y*std::cos(rotation)};};
        baseline={transform({0,0}),transform({8,0}),sweep};
        host_circle=circle(baseline);
        wall=window.createCurvedWall(baseline.start,baseline.end,QString::number(sweep,'g',17)+" rad");
        require(!wall.isEmpty()&&window.selectEntity(wall),"curved wall creation failed");
        require(window.editSelectedHeight("3.2 m"),"curved wall height edit failed");
        require(window.editSelectedThickness("0.3 m"),"curved wall thickness edit failed");
        opening=window.createHostedOpening(kind,"2 m","1 m",kind=="window"?"1 m":"0 m","2 m",
            std::nullopt,kind=="door"?std::optional<DoorOperation>{DoorOperation{false,true,90}}:std::nullopt);
        require(!opening.isEmpty(),"curved opening creation failed");
        if(kind=="door") {
            require(window.selectEntity(opening) && window.editSelectedOpeningAssembly(
                "0.08 m","0.25 m","0.04 m","0 m","0 m"),
                "curved door explicit frame-depth setup failed");
        }
        if (sibling) require(window.selectEntity(wall)&&!window.createHostedOpening("opening","4 m","1 m","0 m","2 m").isEmpty(),
            "curved sibling creation failed");
        auto& view=canvas(window);
        view.setSnapEnabled(false);
        view.setOverviewMapEnabled(false);
        require(window.selectEntity(wall),"host selection failed");
        view.fitView();
        const auto frame=view.selectionBounds();
        require(frame.has_value(),"curved wall calibration frame missing");
        const auto host=retained(window,wall);
        const double stroke=host.segments.size()==1?std::max(host.thickness_metres,.04):host.stroke_width_metres;
        require(host.resize_frame.has_value(),"curved host transform frame missing");
        const auto axes=*host.resize_frame;
        const auto c=std::abs(std::cos(axes.rotation_radians)),s=std::abs(std::sin(axes.rotation_radians));
        scale=(frame->width()-(stroke>0?12.0:15.0)*(c+s))/
            (axes.width_metres*c+axes.depth_metres*s+std::max(stroke,0.0)*(c+s));
        require(scale>0&&window.selectEntity(opening),"curved opening calibration failed");
        require(retained(window,opening).opening_width_controls.has_value(),"curved opening width handles missing");
    }
    Vec2 station(double value) const {
        const double start=std::atan2(baseline.start.y-host_circle.center.y,baseline.start.x-host_circle.center.x);
        const double angle=start+std::copysign(value/host_circle.radius,baseline.sweep_radians);
        return {host_circle.center.x+host_circle.radius*std::cos(angle),host_circle.center.y+host_circle.radius*std::sin(angle)};
    }
    QPointF screen(Vec2 p) const {
        auto& view=canvas(const_cast<MainWindow&>(window));
        const auto center=view.viewCenter();
        return QRectF(view.rect()).center()+QPointF((p.x-center.x)*scale,-(p.y-center.y)*scale);
    }
    QPointF pin(bool start) {
        const auto c=retained(window,opening).opening_width_controls.value();
        return screen(start?c.start_jamb:c.end_jamb);
    }
    void drag(bool start,double target_station,QPointF halo={}) {
        auto& view=canvas(window);
        const auto from=pin(start)+halo,to=screen(station(target_station))+halo;
        const auto revision=window.document().revision();
        const auto output=committedOutput(view);
        mouse(view,QEvent::MouseButtonPress,from);
        mouse(view,QEvent::MouseMove,to);
        require(window.document().revision()==revision,"curved preview mutated document");
        require(committedOutput(view)==output,"curved preview leaked into output rendering");
        const auto capture=qEnvironmentVariable("VERTEX_TEST_CAPTURE_DIR");
        if (!capture.isEmpty()&&kind=="door") require(QDir().mkpath(capture)&&view.grab().save(QDir(capture).filePath("curved-door-preview.png")),"curved preview capture failed");
        mouse(view,QEvent::MouseButtonRelease,to);
    }
};
void dimensions(Fixture& f,double offset,double width) {
    const auto p=properties(f.window,f.opening);
    require(closeEnough(p.at("offset_m").get<double>(),offset)&&closeEnough(p.at("width_m").get<double>(),width),
        "curved jamb drag persisted chord distance instead of arc stations");
}
void artwork(Fixture& f,double offset,double width) {
    const auto entity=retained(f.window,f.opening);
    const auto controls=entity.opening_width_controls.value();
    require(closeEnough(controls.start_jamb,f.station(offset))&&closeEnough(controls.end_jamb,f.station(offset+width))&&
        closeEnough(controls.width_metres,width),"curved controls do not match semantic arc stations");
    std::size_t arcs=0;
    for(const auto& segment:entity.segments) if(std::abs(segment.sweep_radians)>1e-8) {
        ++arcs;
        const auto geometry=circle(segment);
        if(f.kind=="window"||f.kind=="opening"||closeEnough(geometry.center,f.host_circle.center)) {
            require(closeEnough(geometry.center,f.host_circle.center),"curved threshold or window is not concentric with its host");
            require(std::abs(geometry.radius-f.host_circle.radius)<.5,"curved window radius is displaced from wall faces");
        } else {
            const auto chord=distance(f.station(offset),f.station(offset+width));
            require(geometry.radius > chord-.5 && geometry.radius < chord &&
                closeEnough(std::abs(segment.sweep_radians),pi/2),
                "manufactured door swing must follow the trimmed clear leaf instead of the nominal cut");
            require(distance(geometry.center,f.station(offset)) < .5 ||
                    distance(geometry.center,f.station(offset+width)) < .5,
                "manufactured door swing hinge must remain within its frame jamb");
        }
    }
    require(arcs>0,"curved opening lost analytic arc linework");
}
void successful(const QString& kind,double sweep,double rotation,Vec2 translation,bool start) {
    Fixture f(kind,sweep,rotation,translation);
    artwork(f,2,1);
    const auto original=properties(f.window,f.opening);
    const auto controls=retained(f.window,f.opening).opening_width_controls.value();
    const auto revision=f.window.document().revision();
    f.drag(start,start?1.5:3.5);
    require(f.window.document().revision()==revision+1,"curved resize did not issue exactly one command");
    const double offset=start?1.5:2;
    dimensions(f,offset,1.5);
    artwork(f,offset,1.5);
    const auto after=retained(f.window,f.opening).opening_width_controls.value();
    require(closeEnough(start?after.end_jamb:after.start_jamb,start?controls.end_jamb:controls.start_jamb),"curved resize moved opposite jamb");
    const auto changed=properties(f.window,f.opening);
    auto metadata_before=original,metadata_after=changed;
    metadata_before.erase("offset_m");metadata_before.erase("width_m");
    metadata_after.erase("offset_m");metadata_after.erase("width_m");
    require(metadata_before==metadata_after,"resize changed sill, frame, height, or operation metadata");
    require(f.window.undoCommand()&&properties(f.window,f.opening)==original,"curved resize undo failed");
    require(f.window.redoCommand()&&properties(f.window,f.opening)==changed,"curved resize redo failed");
    QTemporaryDir directory;
    require(directory.isValid(),"temporary directory unavailable");
    const auto path=directory.filePath("curved-opening.bldproj");
    require(f.window.saveProjectAs(path)&&f.window.openProject(path)&&properties(f.window,f.opening)==changed,"curved resize save/reopen failed");
    require(f.window.selectEntity(f.opening),"reopened curved opening selection failed");
    artwork(f,offset,1.5);
    const auto capture=qEnvironmentVariable("VERTEX_TEST_CAPTURE_DIR");
    if(!capture.isEmpty()&&kind=="window") require(QDir().mkpath(capture)&&canvas(f.window).grab().save(QDir(capture).filePath("curved-window-selected.png")),"curved selected capture failed");
}
void rejectedAndCancelled() {
    Fixture f("door",pi/2,pi/4,{7,-9},true);
    const auto original=properties(f.window,f.opening);
    const auto revision=f.window.document().revision();
    f.drag(false,4.5);
    require(f.window.document().revision()==revision&&properties(f.window,f.opening)==original,"colliding curved resize committed");
    require(f.window.selectEntity(f.opening),"reselection failed");
    f.drag(true,-.5);
    require(f.window.document().revision()==revision&&properties(f.window,f.opening)==original,"out-of-host curved resize committed");
    require(f.window.selectEntity(f.opening),"reselection failed");
    // A fixed screen-space halo changes its angular projection around the arc.
    // Subtract the press station so grabbing beside a jamb does not jump it.
    const auto halo_station=[&](double station) {
        auto point=f.station(station);
        point.x+=5/f.scale;
        const auto exact=f.station(station);
        const auto initial=std::atan2(exact.y-f.host_circle.center.y,exact.x-f.host_circle.center.x);
        const auto displaced=std::atan2(point.y-f.host_circle.center.y,point.x-f.host_circle.center.x);
        return station+std::copysign(1.0,f.baseline.sweep_radians)*f.host_circle.radius*
            std::remainder(displaced-initial,2*pi);
    };
    const auto halo_width=1+halo_station(3.5)-halo_station(3);
    f.drag(false,3.5,{5,0});
    dimensions(f,2,halo_width);
    require(f.window.undoCommand()&&properties(f.window,f.opening)==original&&f.window.selectEntity(f.opening),"halo resize undo failed");
    auto& view=canvas(f.window);
    const auto cancel_revision=f.window.document().revision();
    mouse(view,QEvent::MouseButtonPress,f.pin(false));
    mouse(view,QEvent::MouseMove,f.screen(f.host_circle.center));
    mouse(view,QEvent::MouseButtonRelease,f.screen(f.host_circle.center));
    require(f.window.document().revision()==cancel_revision&&properties(f.window,f.opening)==original,"undefined center projection committed");
    require(f.window.selectEntity(f.opening),"reselection failed");
    mouse(view,QEvent::MouseButtonPress,f.pin(false));
    mouse(view,QEvent::MouseMove,f.screen(f.station(3.5)));
    QKeyEvent escape(QEvent::KeyPress,Qt::Key_Escape,Qt::NoModifier);
    QApplication::sendEvent(&view,&escape);
    mouse(view,QEvent::MouseButtonRelease,f.screen(f.station(3.5)));
    require(f.window.document().revision()==cancel_revision&&properties(f.window,f.opening)==original,"Escape failed to cancel curved resize");
}
} // namespace
int main(int argc,char** argv) {
    sketch::testing::noninteractive_errors();
    QStandardPaths::setTestModeEnabled(true);
    QApplication app(argc,argv);
    QCoreApplication::setApplicationName("Vertex-curved-opening-"+QUuid::createUuid().toString(QUuid::WithoutBraces));
    const auto font=QFontDatabase::addApplicationFont(":/fonts/Inter.ttf");
    if(font>=0) QApplication::setFont(QFont(QFontDatabase::applicationFontFamilies(font).front(),10));
    try {
        successful("window",pi/2,pi/4,{11,-5},false);
        successful("window",-pi/2,pi/4,{-8,13},true);
        successful("door",pi,pi/4,{9,8},true);
        successful("door",-pi,-pi/4,{-10,-8},false);
        // Stations 2..3.5 cross atan2's +pi/-pi seam on this major arc.
        successful("opening",3*pi/2,pi/8,{5,-12},false);
        rejectedAndCancelled();
        std::cout<<"curved opening resize desktop checks passed\n";
        return 0;
    } catch(const std::exception& error) {
        std::cerr<<"curved_opening_resize_desktop_tests: "<<error.what()<<'\n';
        return 1;
    }
}
