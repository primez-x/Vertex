#include "../src/desktop/plan_canvas.hpp"
#include "support/noninteractive_errors.hpp"

#include <QApplication>
#include <QDir>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <cmath>
#include <iostream>
#include <numbers>
#include <stdexcept>

namespace {
using namespace sketch;
using namespace sketch::desktop;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
bool close_enough(double a, double b) { return std::abs(a-b)<1e-6; }
QPointF screen(Vec2 p) { return {320 + p.x*80, 240-p.y*80}; }
Vec2 oriented(double x, double y, double angle) {
    return {x*std::cos(angle)-y*std::sin(angle), x*std::sin(angle)+y*std::cos(angle)};
}
void mouse(PlanCanvas& canvas, QEvent::Type type, QPointF p,
           Qt::KeyboardModifiers modifiers=Qt::NoModifier) {
    QMouseEvent event(type,p,canvas.mapToGlobal(p.toPoint()),
        type==QEvent::MouseMove?Qt::NoButton:Qt::LeftButton,
        type==QEvent::MouseButtonRelease?Qt::NoButton:Qt::LeftButton,modifiers);
    QApplication::sendEvent(&canvas,&event);
}
QImage render(PlanCanvas& canvas, bool output=false) {
    QImage image(canvas.size(), QImage::Format_ARGB32_Premultiplied);
    QPainter painter(&image);
    canvas.renderScene(painter, image.rect(), output, Qt::white);
    return image;
}
void capture(PlanCanvas& canvas, const QString& name) {
    const auto directory=qEnvironmentVariable("VERTEX_TEST_CAPTURE_DIR");
    if (directory.isEmpty()) return;
    require(QDir().mkpath(directory),"capture directory must be writable");
    require(render(canvas).save(QDir(directory).filePath(name)),"canvas capture must save");
}
CanvasEntity entity(double angle=0) {
    CanvasEntity result;
    result.id="object"; result.type="symbol"; result.selected=true;
    auto a=oriented(-1,-.5,angle), b=oriented(1,-.5,angle);
    auto c=oriented(1,.5,angle), d=oriented(-1,.5,angle);
    result.segments={{a,b,0},{b,c,0},{c,d,0},{d,a,0}};
    result.resize_frame=CanvasSelectionFrame{{0,0},angle,2,1};
    return result;
}
void setup(PlanCanvas& canvas, double angle=0) {
    canvas.resize(640,480); canvas.setGridEnabled(false); canvas.setSnapEnabled(false);
    canvas.setOverviewMapEnabled(false); canvas.setEntities({entity(angle)});
    canvas.setSelectionTransformEnabled(true,true); canvas.setSelectionAxisResizeEnabled(true);
}
void axis_gestures() {
    PlanCanvas canvas; setup(canvas);
    int count=0; double sx=0,sy=0; Vec2 anchor;
    canvas.setEntityAxisResizeRequested([&](QString id,double x,double y,Vec2 fixed) {
        require(id=="object","axis resize must preserve identity");
        ++count;sx=x;sy=y;anchor=fixed;return false;
    });
    const auto original=render(canvas), output=render(canvas,true);
    const auto right=QPointF(407.5,240);
    mouse(canvas,QEvent::MouseButtonPress,right);
    mouse(canvas,QEvent::MouseMove,right+QPointF(80,0));
    require(count==0,"axis preview must not commit");
    require(render(canvas)!=original,"axis resize must preview before release");
    require(render(canvas,true)==output,"axis preview and labels must not enter output");
    mouse(canvas,QEvent::MouseButtonRelease,right+QPointF(80,0));
    require(count==1 && close_enough(sx,1.5) && close_enough(sy,1) && close_enough(anchor.x,-1) && close_enough(anchor.y,0),
            "right drag must change width around the opposite physical edge");
    require(render(canvas)==original,"rejected axis resize must restore geometry and labels");
    const auto top=QPointF(320,192.5);
    mouse(canvas,QEvent::MouseButtonPress,top);
    // Release without a move event still uses the actual final pointer.
    mouse(canvas,QEvent::MouseButtonRelease,top+QPointF(0,-40));
    require(count==2 && close_enough(sx,1) && close_enough(sy,1.5) && close_enough(anchor.y,-.5),
            "top drag must independently change depth, including release-only movement");
    mouse(canvas,QEvent::MouseButtonPress,right);
    mouse(canvas,QEvent::MouseMove,right-QPointF(400,0));
    mouse(canvas,QEvent::MouseButtonRelease,right-QPointF(400,0));
    require(count==3 && sx>0 && sx<=.05 && close_enough(sy,1),"crossing anchor must clamp without reflection");
    mouse(canvas,QEvent::MouseButtonPress,right);
    mouse(canvas,QEvent::MouseMove,right+QPointF(80,0));
    QKeyEvent escape(QEvent::KeyPress,Qt::Key_Escape,Qt::NoModifier);
    QApplication::sendEvent(&canvas,&escape);
    mouse(canvas,QEvent::MouseButtonRelease,right+QPointF(80,0));
    require(count==3 && render(canvas)==original,"Escape must cancel axis preview without callback");
    canvas.setSelectionAxisResizeEnabled(false);
    mouse(canvas,QEvent::MouseButtonPress,right);
    mouse(canvas,QEvent::MouseButtonRelease,right+QPointF(80,0));
    require(count==3,"unsupported objects must not commit an axis resize");
}
void rotated_symbol_and_rotation() {
    constexpr double angle=std::numbers::pi/6;
    PlanCanvas canvas; setup(canvas,angle);
    auto symbol=entity(angle);
    symbol.resize_frame.reset();
    CanvasSvgSymbol artwork;
    artwork.document="<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 200 100'><path fill='#ef2020' d='M0 0H200V100H0Z'/><path fill='#1020ff' d='M0 0H50V100H0Z'/></svg>";
    artwork.view_box=artwork.footprint_view_box=QRectF(0,0,200,100);
    artwork.rotation_radians=angle; artwork.width_metres=2;artwork.depth_metres=1;
    symbol.svg_symbol=artwork; canvas.setEntities({symbol});
    int count=0; double sx=0,sy=0;Vec2 anchor;
    canvas.setEntityAxisResizeRequested([&](QString,double x,double y,Vec2 a) {
        ++count;sx=x;sy=y;anchor=a;return true;
    });
    const auto right=screen(oriented(1+7.5/80,0,angle));
    const auto delta=oriented(1,0,angle);
    const auto destination=right+QPointF(delta.x*80,-delta.y*80);
    const auto initial=render(canvas);
    mouse(canvas,QEvent::MouseButtonPress,right);
    mouse(canvas,QEvent::MouseMove,destination);
    const auto preview=render(canvas);
    require(preview!=initial,"rotated symbol must preview a local-axis resize");
    // Local point (1.5, .25) is inside the enlarged red artwork and outside the original footprint.
    const auto sample=screen(oriented(1.5,.25,angle)).toPoint();
    require(preview.pixelColor(sample).red()>200 && preview.pixelColor(sample).blue()<80,
            "rotated artwork preview must grow along its own X axis without world-axis skew");
    mouse(canvas,QEvent::MouseButtonRelease,destination);
    require(count==1 && close_enough(sx,1.5) && close_enough(sy,1) && close_enough(anchor.x,-std::sqrt(3.)/2) && close_enough(anchor.y,-.5),
            "rotated local-X drag must preserve the opposite rotated edge");

    double rotation=0; int rotations=0;
    canvas.setEntityTransformRequested([&](QString,double scale,double r) {
        require(close_enough(scale,1),"rotation must preserve dimensions"); rotation=r;++rotations;return true;
    });
    const auto pin=screen(oriented(0,.5+(7.5+24)/80,angle));
    const auto pin90=screen(oriented(0,.5+(7.5+24)/80,std::numbers::pi/2));
    mouse(canvas,QEvent::MouseButtonPress,pin);
    mouse(canvas,QEvent::MouseMove,pin90);
    capture(canvas,"symbol-rotation-90-preview.png");
    mouse(canvas,QEvent::MouseButtonRelease,pin90);
    require(rotations==1 && close_enough(rotation,std::numbers::pi/3),
            "rotation must snap absolute angle from 30 to 90 degrees");
    const auto fine=screen(oriented(0,.5+(7.5+24)/80,83.5*std::numbers::pi/180));
    mouse(canvas,QEvent::MouseButtonPress,pin,Qt::ShiftModifier);
    mouse(canvas,QEvent::MouseButtonRelease,fine,Qt::ShiftModifier);
    require(rotations==2 && close_enough(rotation,53.5*std::numbers::pi/180),
            "Shift must preserve fine absolute angles");
    auto updated=symbol;updated.svg_symbol->rotation_radians=std::numbers::pi/2;
    canvas.setEntities({updated});
    capture(canvas,"symbol-rotation-90-selected.png");
    mouse(canvas,QEvent::MouseButtonPress,pin90);
    mouse(canvas,QEvent::MouseButtonRelease,screen(oriented(0,.5+(7.5+24)/80,std::numbers::pi)));
    require(rotations==3 && close_enough(rotation,std::numbers::pi/2),
            "rotation pin must retain committed symbol orientation after projection refresh");
}
void vector_frame_survives_projection_refresh() {
    PlanCanvas canvas; setup(canvas,std::numbers::pi/6);
    int rotations=0, resizes=0;
    canvas.setEntityTransformRequested([&](QString,double,double delta) {
        require(close_enough(delta,std::numbers::pi/3),"vector rotation must commit the snapped model delta");
        ++rotations;
        canvas.setEntities({entity(std::numbers::pi/2)});
        return true;
    });
    const auto pin30=screen(oriented(0,.5+(7.5+24)/80,std::numbers::pi/6));
    const auto pin90=screen(oriented(0,.5+(7.5+24)/80,std::numbers::pi/2));
    mouse(canvas,QEvent::MouseButtonPress,pin30);
    mouse(canvas,QEvent::MouseButtonRelease,pin90);
    require(rotations==1,"vector rotation must commit once");
    canvas.setEntityAxisResizeRequested([&](QString,double sx,double sy,Vec2 anchor) {
        ++resizes;
        require(close_enough(sx,1.5) && close_enough(sy,1) && close_enough(anchor.x,0) && close_enough(anchor.y,-1),
                "vector frame must resize along the committed rotated local axis");
        return true;
    });
    // At 90 degrees, local X is vertical in the viewport. A stale world frame
    // or stale orientation would target a different control or wrong scales.
    const auto right90=QPointF(320,152.5);
    mouse(canvas,QEvent::MouseButtonPress,right90);
    mouse(canvas,QEvent::MouseButtonRelease,right90+QPointF(0,-80));
    require(resizes==1,"vector side handle must retain orientation after projection refresh");
}
void common_angle_snapping_and_cancelled_rotation() {
    constexpr double radians=std::numbers::pi/180;
    constexpr double radius=.5+(7.5+24)/80;
    PlanCanvas canvas;setup(canvas,30*radians);
    double angle=30*radians;
    double expected=0;
    int rotations=0;
    bool accept=true;
    canvas.setEntityTransformRequested([&](QString id,double scale,double delta) {
        require(id=="object" && close_enough(scale,1),"rotation must preserve identity and scale");
        require(close_enough(std::remainder(angle+delta,2*std::numbers::pi),
                             std::remainder(expected,2*std::numbers::pi)),
                "off-grid rotation must snap to an absolute common angle");
        ++rotations;
        if (accept) {
            angle=expected;
            canvas.setEntities({entity(angle)});
        }
        return accept;
    });
    const auto drag=[&](double pointer_degrees,double committed_degrees,
                        Qt::KeyboardModifiers modifiers=Qt::NoModifier) {
        expected=committed_degrees*radians;
        const auto pin=screen(oriented(0,radius,angle));
        const auto destination=screen(oriented(0,radius,pointer_degrees*radians));
        mouse(canvas,QEvent::MouseButtonPress,pin,modifiers);
        mouse(canvas,QEvent::MouseMove,destination,modifiers);
        capture(canvas,QStringLiteral("rotation-%1-preview.png").arg(committed_degrees));
        mouse(canvas,QEvent::MouseButtonRelease,destination,modifiers);
    };
    drag(52,45);
    drag(103,90);
    drag(169,180);
    drag(203.5,203.5,Qt::ShiftModifier);
    require(rotations==4,"each rotation must commit once from its persisted frame");
    const auto committed=render(canvas);
    accept=false;
    drag(257,270);
    require(rotations==5 && close_enough(angle,203.5*radians) && render(canvas)==committed,
            "rejected rotation must restore the committed object-relative frame");
    const auto pin=screen(oriented(0,radius,angle));
    const auto destination=screen(oriented(0,radius,270*radians));
    mouse(canvas,QEvent::MouseButtonPress,pin);
    mouse(canvas,QEvent::MouseMove,destination);
    QKeyEvent escape(QEvent::KeyPress,Qt::Key_Escape,Qt::NoModifier);
    QApplication::sendEvent(&canvas,&escape);
    mouse(canvas,QEvent::MouseButtonRelease,destination);
    require(rotations==5 && render(canvas)==committed,
            "Escape must restore the committed object-relative frame without a callback");
}
void dimensions_are_physical_and_screen_only() {
    PlanCanvas canvas;setup(canvas);
    const auto metric=render(canvas);const auto output=render(canvas,true);
    canvas.setMetricUnits(true);
    require(render(canvas)!=metric,"selection dimensions must display the configured units");
    require(render(canvas,true)==output,"dimension controls must be screen-only");
    auto selected=entity();selected.resize_frame->width_metres=3;
    canvas.setEntities({selected});
    require(render(canvas)!=metric,"selection dimensions must respond to physical metadata");
}
void label_and_reference_rotation_preview() {
    constexpr double initial_angle=std::numbers::pi/6;
    for (const bool text : {false,true}) {
        PlanCanvas canvas;setup(canvas);canvas.setEntities({});canvas.setSelectionAxisResizeEnabled(false);
        CanvasReference reference;
        CanvasLabel label;
        if (text) {
            label.id="text";label.text="Rotated label";label.text_height_metres=.5;
            label.rotation_radians=initial_angle;label.selected=true;
            canvas.setLabels({label});
        } else {
            reference.id="image";reference.image=QImage(200,100,QImage::Format_RGB32);
            reference.image.fill(QColor(240,20,20));
            {QPainter p(&reference.image);p.fillRect(0,0,50,100,QColor(20,20,240));}
            reference.rotation_degrees=30;reference.selected=true;reference.metres_per_source_unit=.01;
            canvas.setReferences({reference});
        }
        const auto turn_screen=[](QPointF point,double angle) {
            const auto x=point.x()-320,y=point.y()-240;
            return QPointF(320+x*std::cos(angle)+y*std::sin(angle),
                240-x*std::sin(angle)+y*std::cos(angle));
        };
        const auto initial=render(canvas);const auto output=render(canvas,true);
        int rotations=0;
        canvas.setEntityTransformRequested([&](QString id,double scale,double delta) {
            require(id==(text?QString("text"):QString("image")) && close_enough(scale,1) &&
                    close_enough(delta,std::numbers::pi/3),"label/reference must snap its actual 30 degree angle to 90");
            ++rotations;
            if(text){label.rotation_radians=std::numbers::pi/2;canvas.setLabels({label});}
            else{reference.rotation_degrees=90;canvas.setReferences({reference});}
            return true;
        });
        const auto actual_pin=canvas.selectionRotationHandlePosition();
        require(actual_pin.has_value(),"selected label/reference exposes its painted rotation handle");
        const auto pin30=*actual_pin;
        const auto pin90=turn_screen(pin30,std::numbers::pi/3);
        mouse(canvas,QEvent::MouseButtonPress,pin30);
        mouse(canvas,QEvent::MouseMove,pin90);
        const auto preview=render(canvas);
        require(preview!=initial && render(canvas,true)==output,"label/reference preview must be visible and excluded from output");
        if (!text) {
            const auto sample=preview.pixelColor(320,300);
            require(sample.blue()>200 && sample.red()<80,"reference artwork must rotate during drag before commit");
        }
        mouse(canvas,QEvent::MouseButtonRelease,pin90);
        const auto committed=render(canvas);
        const auto directory=qEnvironmentVariable("VERTEX_TEST_CAPTURE_DIR");
        if(!directory.isEmpty()) {
            require(QDir().mkpath(directory),"rotation capture directory must be writable");
            const auto stem=text?QStringLiteral("label-rotation"):QStringLiteral("reference-rotation");
            require(preview.save(QDir(directory).filePath(stem+"-preview.png")) &&
                committed.save(QDir(directory).filePath(stem+"-committed.png")),"actual rotation comparison captures must save");
        }
        require(rotations==1 && preview.copy(300,220,40,40)==committed.copy(300,220,40,40),
                text?"label actual content preview must match committed painting":"reference actual content preview must match committed painting");
        canvas.setEntityTransformRequested([&](QString,double,double delta) {
            ++rotations;require(close_enough(delta,std::numbers::pi/2),"label/reference pin must retain 90 degree orientation");return true;
        });
        const auto committed_pin=canvas.selectionRotationHandlePosition();
        require(committed_pin.has_value(),"committed label/reference retains a rotation handle");
        mouse(canvas,QEvent::MouseButtonPress,*committed_pin);
        mouse(canvas,QEvent::MouseButtonRelease,turn_screen(*committed_pin,std::numbers::pi/2));
        require(rotations==2,"label/reference persistent pin must start a second gesture");
    }
}

void exact_presentation_move_admission() {
    for(const bool text:{false,true})for(int timing=0;timing<6;++timing) {
        PlanCanvas canvas;setup(canvas);canvas.setEntities({});
        canvas.setSelectionTransformEnabled(false,false);canvas.setSelectionAxisResizeEnabled(false);
        CanvasLabel label;label.id="callout";label.text="Measured area";label.text_height_metres=.5;label.selected=true;
        CanvasReference reference;reference.id="image";reference.image=QImage(200,100,QImage::Format_RGB32);
        reference.image.fill(QColor(240,20,20));reference.metres_per_source_unit=.01;reference.selected=true;
        if(text)canvas.setLabels({label});else canvas.setReferences({reference});
        std::uint64_t serial=0;Vec2 delta;int commits=0,rejections=0;bool completed_before_release=false;
        const auto proposed_labels=[&] {
            auto moved=label;moved.position={label.position.x+delta.x,label.position.y+delta.y};
            return text?std::vector<CanvasLabel>{moved}:std::vector<CanvasLabel>{};
        };
        canvas.setEntitiesMoveRequested([&](QStringList ids,Vec2 offset) {
            require(ids==QStringList{text?"callout":"image"} && close_enough(offset.x,1) && close_enough(offset.y,.5),
                "exact presentation movement must commit its selected identity and final offset");
            ++commits;
            if(text){label.position={1,.5};canvas.setLabels({label});}
            else{reference.position={1,.5};canvas.setReferences({reference});}
            return true;
        });
        canvas.setEntitiesMoveRejected([&](QStringList,Vec2){++rejections;});
        canvas.setEntitiesMovePreviewRequested([&](QStringList,Vec2 offset,std::uint64_t value)
            ->std::optional<std::vector<CanvasEntity>> {
            serial=value;delta=offset;
            if(timing==4)throw std::runtime_error("Preview provider failed before deferral");
            require(canvas.markEntitiesMovePreviewPending(serial),"presentation move preview can be marked pending");
            if(timing==5)throw std::runtime_error("Preview provider failed after deferral");
            if(timing==3 || (timing==0 && completed_before_release))require(canvas.completeEntitiesMovePreview(serial,std::vector<CanvasEntity>{},proposed_labels()),
                "in-callback presentation completion accepted");
            return std::nullopt;
        });
        const auto bounds=canvas.selectionBounds();require(bounds.has_value(),"presentation selection frame exists");
        const auto start=bounds->center(),end=start+QPointF(80,-40);
        const auto original=render(canvas),output=render(canvas,true);
        mouse(canvas,QEvent::MouseButtonPress,start);mouse(canvas,QEvent::MouseMove,end);
        if(timing==0) {
            require(canvas.completeEntitiesMovePreview(serial,std::vector<CanvasEntity>{},proposed_labels()),
                "label-only/reference-only exact result accepted before release");
            completed_before_release=true;
        }
        if(timing==0 || timing==3)require(render(canvas)!=original && render(canvas,true)==output,
            "accepted presentation-only live move is visible and excluded from output");
        if(timing>=4)require(render(canvas)==original && !canvas.entitiesMovePreviewPending(),
            "a throwing provider must reject presentation preview and release pending ownership");
        mouse(canvas,QEvent::MouseButtonRelease,end);
        if(timing==1 || timing==2) {
            require(commits==0 && canvas.entitiesMovePreviewPending(),"released presentation edit awaits actual admission");
            require(canvas.completeEntitiesMovePreview(serial,timing==1?std::optional<std::vector<CanvasEntity>>(std::vector<CanvasEntity>{}):std::nullopt,
                timing==1?proposed_labels():std::vector<CanvasLabel>{}),"late presentation proposal completes");
        }
        QApplication::processEvents();
        const bool rejected=timing==2 || timing>=4;
        require(commits==(rejected?0:1) && rejections==(rejected?1:0) && !canvas.entitiesMovePreviewPending(),
            "presentation move admission must distinguish accepted empty geometry from rejected proposal");
        require(!canvas.completeEntitiesMovePreview(serial,std::vector<CanvasEntity>{},proposed_labels()),
            "completed presentation edit cannot be committed twice");
    }
}
}
int main(int argc,char**argv) {
    sketch::testing::noninteractive_errors();QApplication app(argc,argv);
    try {
        const auto font_id=QFontDatabase::addApplicationFont(":/fonts/Inter.ttf");
        require(font_id>=0,"bundled canvas font must load");
        const auto families=QFontDatabase::applicationFontFamilies(font_id);
        require(!families.isEmpty(),"bundled canvas font must expose its family");
        app.setFont(QFont(families.front(),10));
        axis_gestures();rotated_symbol_and_rotation();vector_frame_survives_projection_refresh();
        common_angle_snapping_and_cancelled_rotation();
        dimensions_are_physical_and_screen_only();label_and_reference_rotation_preview();exact_presentation_move_admission();
        std::cout<<"Axis canvas controls tests passed\n";return 0;
    } catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}
}
