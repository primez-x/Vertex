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
    image.fill(Qt::white);
    QPainter painter(&image);
    if (output) canvas.renderScene(painter, image.rect(), true, Qt::white);
    else {
        // Public scene renderers expose committed output only; interactive
        // previews and controls are asserted through the actual paint event.
        canvas.setCanvasBackground(Qt::white);
        canvas.render(&painter);
    }
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
void compound_presentation_previews() {
    PlanCanvas canvas; setup(canvas);
    std::vector<CanvasEntity> profiles;
    for (int i=0;i<2;++i) {
        auto profile=entity();
        profile.id="assembly"; profile.presentation_key=i==0?"left":"right";
        const double x=i==0?-1.0:1.0;
        profile.segments={{{x-.3,-.3},{x+.3,-.3},0},{{x+.3,-.3},{x+.3,.3},0},
                          {{x+.3,.3},{x-.3,.3},0},{{x-.3,.3},{x-.3,-.3},0}};
        profile.filled=true; profile.fill_color=i==0?QColor(240,20,20):QColor(20,20,240);
        profile.type="assembly_instance"; profile.hatch_pattern="solid";
        profile.fill_opacity=1.0;
        profile.resize_frame=CanvasSelectionFrame{{0,0},0,2.6,.6};
        profiles.push_back(profile);
    }
    canvas.setEntities(profiles);
    const auto original=render(canvas), output=render(canvas,true);
    const auto colors=[&](const QImage& image,Vec2 a,Vec2 b,const char* phase) {
        const auto red=image.pixelColor(screen(a).toPoint());
        const auto blue=image.pixelColor(screen(b).toPoint());
        if(!(red.red()>red.blue()+80 && blue.blue()>blue.red()+80))
            throw std::runtime_error(QStringLiteral("%1: distinct assembly profile colors at expected positions were %2 and %3")
                .arg(QString::fromLatin1(phase),red.name(),blue.name()).toStdString());
    };
    capture(canvas,"compound-original.png");
    colors(original,{-1,0},{1,0},"original");
    int moves=0,rotations=0,starts=0;
    canvas.setEntitiesMoveRequested([&](QStringList ids,Vec2) {
        require(ids==QStringList{"assembly"},"compound move commits one semantic root");++moves;return false;
    });
    canvas.setEntitiesMovePreviewRequested([&](QStringList ids,Vec2 delta,std::uint64_t) {
        require(ids==QStringList{"assembly"},"compound preview retains one semantic root");
        auto proposed=profiles;
        for(auto& p:proposed) {
            for(auto& s:p.segments) {s.start={s.start.x+delta.x,s.start.y+delta.y};s.end={s.end.x+delta.x,s.end.y+delta.y};}
            p.resize_frame->center={p.resize_frame->center.x+delta.x,p.resize_frame->center.y+delta.y};
        }
        return std::optional<std::vector<CanvasEntity>>{proposed};
    });
    mouse(canvas,QEvent::MouseButtonPress,screen({0,0}));
    mouse(canvas,QEvent::MouseMove,screen({0,.8}));
    capture(canvas,"compound-move-preview.png");
    colors(render(canvas),{-1,.8},{1,.8},"move preview");
    require(render(canvas,true)==output,"compound move preview stays out of output");
    QKeyEvent escape(QEvent::KeyPress,Qt::Key_Escape,Qt::NoModifier);
    QApplication::sendEvent(&canvas,&escape);
    mouse(canvas,QEvent::MouseButtonRelease,screen({0,.8}));
    require(moves==0 && render(canvas)==original,"cancelled compound move preserves scene");
    mouse(canvas,QEvent::MouseButtonPress,screen({0,0}));
    mouse(canvas,QEvent::MouseMove,screen({0,.8}));
    mouse(canvas,QEvent::MouseButtonRelease,screen({0,.8}));
    require(moves==1 && render(canvas)==original,"rejected compound move restores both profiles");
    canvas.setEntityTransformStarted([&](QString id){require(id=="assembly","transform start is semantic");++starts;});
    canvas.setEntityTransformRequested([&](QString id,double,double) {
        require(id=="assembly","compound rotation commits one semantic root");++rotations;return false;
    });
    canvas.setEntityTransformPreviewRequested([&](QString,double scale,double angle,Vec2 pivot,std::uint64_t) {
        auto proposed=profiles;
        for(auto& p:proposed) {
            const auto transform=[&](Vec2 point){auto q=oriented((point.x-pivot.x)*scale,(point.y-pivot.y)*scale,angle);return Vec2{q.x+pivot.x,q.y+pivot.y};};
            for(auto& s:p.segments){s.start=transform(s.start);s.end=transform(s.end);}
            p.resize_frame->rotation_radians=angle;
        }
        return std::optional<std::vector<CanvasEntity>>{proposed};
    });
    const auto pin=canvas.selectionRotationHandlePosition();require(pin.has_value(),"compound rotation handle exists");
    const auto destination=QPointF(320-(240-pin->y()),240);
    mouse(canvas,QEvent::MouseButtonPress,*pin);
    mouse(canvas,QEvent::MouseMove,destination);
    capture(canvas,"compound-rotation-preview.png");
    colors(render(canvas),{0,-1},{0,1},"rotation preview");
    require(render(canvas,true)==output,"compound rotation preview stays out of output");
    QApplication::sendEvent(&canvas,&escape);
    mouse(canvas,QEvent::MouseButtonRelease,destination);
    require(rotations==0 && starts==1 && render(canvas)==original,"cancelled compound rotation preserves scene");
    mouse(canvas,QEvent::MouseButtonPress,*pin);
    mouse(canvas,QEvent::MouseMove,destination);
    mouse(canvas,QEvent::MouseButtonRelease,destination);
    require(rotations==1 && render(canvas)==original,"rejected compound rotation restores both profiles");
}
void axis_gestures() {
    PlanCanvas canvas; setup(canvas);
    int starts=0;
    canvas.setEntityTransformStarted([&](QString id) {
        require(id=="object","axis press captures semantic source");++starts;
    });
    int count=0; double sx=0,sy=0; Vec2 anchor;
    canvas.setEntityAxisResizeRequested([&](QString id,double x,double y,Vec2 fixed) {
        require(id=="object","axis resize must preserve identity");
        ++count;sx=x;sy=y;anchor=fixed;return false;
    });
    const auto original=render(canvas), output=render(canvas,true);
    const auto right=QPointF(407.5,240);
    mouse(canvas,QEvent::MouseButtonPress,right);
    mouse(canvas,QEvent::MouseMove,right+QPointF(80,0));
    require(starts==1,"axis source capture must precede first preview");
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
void preference_angle_snap_step() {
    constexpr double radians=std::numbers::pi/180;
    constexpr double radius=.5+(7.5+24)/80;
    PlanCanvas canvas;setup(canvas,30*radians);
    require(close_enough(canvas.angleSnapDegrees(),45),"angle snap defaults to 45 degrees");
    canvas.setAngleSnapDegrees(30);
    canvas.setAngleSnapDegrees(0);
    canvas.setAngleSnapDegrees(std::numeric_limits<double>::quiet_NaN());
    require(close_enough(canvas.angleSnapDegrees(),30),"invalid angle snap steps are ignored");
    double angle=30*radians, expected=0;
    int rotations=0;
    canvas.setEntityTransformRequested([&](QString,double,double delta) {
        require(close_enough(std::remainder(angle+delta,2*std::numbers::pi),
                             std::remainder(expected,2*std::numbers::pi)),
                "rotation must snap to the preference angle step");
        ++rotations;angle=expected;canvas.setEntities({entity(angle)});return true;
    });
    const auto drag=[&](double pointer_degrees,double committed_degrees,
                        Qt::KeyboardModifiers modifiers=Qt::NoModifier) {
        expected=committed_degrees*radians;
        const auto pin=screen(oriented(0,radius,angle));
        const auto destination=screen(oriented(0,radius,pointer_degrees*radians));
        mouse(canvas,QEvent::MouseButtonPress,pin,modifiers);
        mouse(canvas,QEvent::MouseMove,destination,modifiers);
        mouse(canvas,QEvent::MouseButtonRelease,destination,modifiers);
    };
    drag(65,60);      // 45-degree steps would give 45
    drag(137,150);    // 45-degree steps would give 135
    drag(97.5,97.5,Qt::ShiftModifier);
    require(rotations==3,"each preference-snapped rotation commits once");
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

void sustained_move_after_exact_preview() {
    PlanCanvas canvas;setup(canvas);
    auto body=entity();body.filled=true;body.fill_color=QColor(240,20,20);
    body.fill_opacity=1;body.hatch_pattern="solid";canvas.setEntities({body});
    std::uint64_t serial{};int commits{};
    canvas.setEntitiesMovePreviewRequested([&](QStringList,Vec2,std::uint64_t value)
        ->std::optional<std::vector<CanvasEntity>> {
        serial=value;require(canvas.markEntitiesMovePreviewPending(value),"sustained move marks pending");return {};
    });
    canvas.setEntitiesMoveRequested([&](QStringList,Vec2){++commits;return false;});
    const auto start=screen({0,0});
    mouse(canvas,QEvent::MouseButtonPress,start);
    mouse(canvas,QEvent::MouseMove,screen({1,0}));
    auto admitted=body;
    for(auto& segment:admitted.segments){segment.start.x+=1;segment.end.x+=1;}
    admitted.resize_frame->center.x+=1;
    require(canvas.completeEntitiesMovePreview(serial,std::vector<CanvasEntity>{admitted}),"first sustained result accepted");
    mouse(canvas,QEvent::MouseMove,screen({1.5,.25}));
    mouse(canvas,QEvent::MouseMove,screen({2,.5}));
    const auto pending=render(canvas);
    const auto red=[&](Vec2 point){const auto c=pending.pixelColor(screen(point).toPoint());return c.red()>180&&c.green()<100&&c.blue()<100;};
    require(red({2.7,.5})&&!red({.3,0}),
        "sustained movement must track the current pointer after an exact preview, without freezing at the accepted position");
    const auto frame=canvas.selectionBounds();
    require(frame&&std::abs(frame->center().x()-screen({2,.5}).x())<2,
        "sustained movement selection frame must follow the displayed body");
    mouse(canvas,QEvent::MouseButtonRelease,screen({2,.5}));
    require(commits==0&&canvas.entitiesMovePreviewPending(),"sustained display cannot authorize the pending release");
    QKeyEvent escape(QEvent::KeyPress,Qt::Key_Escape,Qt::NoModifier);QApplication::sendEvent(&canvas,&escape);
}

void delayed_body_previews() {
    for (int gesture=0; gesture<4; ++gesture) {
        PlanCanvas canvas;setup(canvas);
        CanvasEntity body;body.id="body";body.type="assembly_instance";body.selected=true;
        body.segments={{{-1,-.5},{1,-.5}},{{1,-.5},{1,.5}},{{1,.5},{-1,.5}},{{-1,.5},{-1,-.5}}};
        body.filled=true;body.fill_color=QColor(240,20,20);body.fill_opacity=1;body.hatch_pattern="solid";
        body.resize_frame=CanvasSelectionFrame{{0,0},0,2,1};
        canvas.setEntities({body});
        std::uint64_t serial{};int requests{},commits{};
        const auto pending=[&](std::uint64_t ticket) {
            serial=ticket;++requests;
            require(gesture==0?canvas.markEntitiesMovePreviewPending(ticket):canvas.markEntityTransformPreviewPending(ticket),
                "delayed body provider owns current request");
            return std::optional<std::vector<CanvasEntity>>{};
        };
        canvas.setEntitiesMovePreviewRequested([&](QStringList,Vec2,std::uint64_t ticket){return pending(ticket);});
        canvas.setEntityTransformPreviewRequested([&](QString,double,double,Vec2,std::uint64_t ticket){return pending(ticket);});
        canvas.setEntityAxisResizePreviewRequested([&](QString,double,double,Vec2,std::uint64_t ticket){return pending(ticket);});
        canvas.setEntitiesMoveRequested([&](QStringList,Vec2){++commits;return false;});
        canvas.setEntityTransformRequested([&](QString,double,double){++commits;return false;});
        canvas.setEntityAxisResizeRequested([&](QString,double,double,Vec2){++commits;return false;});
        const auto original=render(canvas),output=render(canvas,true);
        QPointF start,end;
        if(gesture==0){start={320,240};end=start+QPointF(80,-40);}
        else if(gesture==1){start=*canvas.selectionRotationHandlePosition();end={320+(start.y()-240),240-(start.x()-320)};}
        else if(gesture==2){start=canvas.selectionBounds()->bottomRight();end=start+QPointF(60,40);}
        else {start=canvas.selectionAxisHandlePositions().at(1);end=start+QPointF(80,0);}
        mouse(canvas,QEvent::MouseButtonPress,start);mouse(canvas,QEvent::MouseMove,end);
        require(requests==1,"body gesture must request delayed projection");
        const auto red=[](const QImage& image,int x,int y){const auto c=image.pixelColor(x,y);return c.red()>180&&c.green()<100&&c.blue()<100;};
        int changed=0;const auto first=render(canvas);
        for(int y=80;y<370;++y)for(int x=100;x<550;++x)changed+=red(original,x,y)!=red(first,x,y);
        require(changed>500,"initial pending exact work must visibly move the filled body");
        capture(canvas,QStringLiteral("delayed-body-%1-initial-pending.png").arg(gesture));
        mouse(canvas,QEvent::MouseMove,end);
        require(requests==1,"identical effective body proposal must retain its request");
        auto exact=body;for(auto& segment:exact.segments){segment.start.x+=.4;segment.end.x+=.4;}
        require(gesture==0?canvas.completeEntitiesMovePreview(serial,std::vector<CanvasEntity>{exact}):
            canvas.completeEntityTransformPreview(serial,std::vector<CanvasEntity>{exact}),"first body result admitted");
        const auto accepted=render(canvas);const auto old=serial;
        require(red(accepted,420,230),"continuity sample must contain accepted fill beyond committed body");
        capture(canvas,QStringLiteral("delayed-body-%1-accepted.png").arg(gesture));
        mouse(canvas,QEvent::MouseMove,end+QPointF(24,-20));
        require(requests==2&&serial>old,"successor body proposal owns a fresh request");
        const auto successor=render(canvas);
        // The accepted body stays the base (never restored to the committed
        // source), but the selected body follows the current pointer.
        int moved=0;
        for(int y=80;y<370;++y)for(int x=100;x<550;++x)moved+=red(accepted,x,y)!=red(successor,x,y);
        require(moved>50,"pending successor must move the selected body to the current pointer");
        if(gesture==0) {
            // Accepted body spans x 272..432, y 200..280; the residual is (+24,-20).
            require(red(successor,445,230)&&!red(accepted,445,230),
                "pending move successor must carry the accepted body by the residual");
            require(!red(successor,280,270)&&red(accepted,280,270),
                "pending move successor must not remain at the accepted position");
        }
        require(red(successor,300,215)||red(successor,330,230),
            "pending successor must still contain the accepted body instead of the committed source");
        require(render(canvas,true)==output&&canvas.entities().front().segments.front().start.x==-1&&commits==0,
            "displayed body never changes source or output");
        capture(canvas,QStringLiteral("delayed-body-%1.png").arg(gesture));
        mouse(canvas,QEvent::MouseButtonRelease,end+QPointF(24,-20));
        require(commits==0,"previous displayed body cannot admit pending successor release");
        require(!(gesture==0?canvas.completeEntitiesMovePreview(old,std::vector<CanvasEntity>{exact}):
            canvas.completeEntityTransformPreview(old,std::vector<CanvasEntity>{exact})),"old displayed result stays stale");
        QKeyEvent escape(QEvent::KeyPress,Qt::Key_Escape,Qt::NoModifier);QApplication::sendEvent(&canvas,&escape);
        require(!(gesture==0?canvas.completeEntitiesMovePreview(serial,std::vector<CanvasEntity>{exact}):
            canvas.completeEntityTransformPreview(serial,std::vector<CanvasEntity>{exact})),"cancel retires pending body result");
    }
}

QPointF gesture_start(PlanCanvas& canvas,int gesture) {
    if(gesture==0)return {320,240};
    if(gesture==1)return *canvas.selectionRotationHandlePosition();
    if(gesture==2)return canvas.selectionBounds()->bottomRight();
    return canvas.selectionAxisHandlePositions().at(1);
}
QPointF gesture_end(QPointF start,int gesture) {
    if(gesture==0)return start+QPointF(80,-40);
    if(gesture==1)return {320+(start.y()-240),240-(start.x()-320)};
    if(gesture==2)return start+QPointF(60,40);
    return start+QPointF(80,0);
}
CanvasEntity filled_body() {
    CanvasEntity body;body.id="body";body.type="assembly_instance";body.selected=true;
    body.segments={{{-1,-.5},{1,-.5}},{{1,-.5},{1,.5}},{{1,.5},{-1,.5}},{{-1,.5},{-1,-.5}}};
    body.filled=true;body.fill_color=QColor(240,20,20);body.fill_opacity=1;body.hatch_pattern="solid";
    body.resize_frame=CanvasSelectionFrame{{0,0},0,2,1};
    return body;
}
bool is_red(const QImage& image,int x,int y) {
    const auto c=image.pixelColor(x,y);return c.red()>180&&c.green()<100&&c.blue()<100;
}

void retry_after_rejection_requests_again() {
    for(int gesture=0;gesture<4;++gesture)for(int failure=0;failure<2;++failure) {
        PlanCanvas canvas;setup(canvas);canvas.setEntities({filled_body()});
        int requests{};
        const auto provider=[&] {
            ++requests;
            // An exception, or an unmarked decline, ends the sample unserved.
            if(failure==0)throw std::runtime_error("preview provider failed");
        };
        canvas.setEntitiesMovePreviewRequested([&](QStringList,Vec2,std::uint64_t)
            ->std::optional<std::vector<CanvasEntity>> {provider();return std::nullopt;});
        canvas.setEntityTransformPreviewRequested([&](QString,double,double,Vec2,std::uint64_t)
            ->std::optional<std::vector<CanvasEntity>> {provider();return std::nullopt;});
        canvas.setEntityAxisResizePreviewRequested([&](QString,double,double,Vec2,std::uint64_t)
            ->std::optional<std::vector<CanvasEntity>> {provider();return std::nullopt;});
        canvas.setEntitiesMoveRequested([](QStringList,Vec2){return false;});
        canvas.setEntityTransformRequested([](QString,double,double){return false;});
        canvas.setEntityAxisResizeRequested([](QString,double,double,Vec2){return false;});
        const auto start=gesture_start(canvas,gesture),end=gesture_end(start,gesture);
        mouse(canvas,QEvent::MouseButtonPress,start);mouse(canvas,QEvent::MouseMove,end);
        require(requests==1,"rejected sample requested once");
        mouse(canvas,QEvent::MouseMove,end);
        require(requests==2,"identical sample after a rejection or failure must request again");
        mouse(canvas,QEvent::MouseMove,end);
        require(requests==3,"a still-unserved identical sample keeps retrying");
        QKeyEvent escape(QEvent::KeyPress,Qt::Key_Escape,Qt::NoModifier);QApplication::sendEvent(&canvas,&escape);
    }
}

void invalid_sample_retires_displayed_result() {
    for(const bool axis:{false,true}) {
        PlanCanvas canvas;setup(canvas);
        auto body=filled_body();canvas.setEntities({body});
        std::uint64_t serial{};int requests{};bool invalid{};
        const auto pending=[&](std::uint64_t ticket,bool is_move) {
            serial=ticket;++requests;
            if(invalid)return std::optional<std::vector<CanvasEntity>>{std::vector<CanvasEntity>{}};
            require(is_move?canvas.markEntitiesMovePreviewPending(ticket):canvas.markEntityTransformPreviewPending(ticket),
                "sample marked pending");
            return std::optional<std::vector<CanvasEntity>>{};
        };
        canvas.setEntitiesMovePreviewRequested([&](QStringList,Vec2,std::uint64_t t){return pending(t,true);});
        canvas.setEntityAxisResizePreviewRequested([&](QString,double,double,Vec2,std::uint64_t t){return pending(t,false);});
        canvas.setEntitiesMoveRequested([](QStringList,Vec2){return false;});
        canvas.setEntityAxisResizeRequested([](QString,double,double,Vec2){return false;});
        const auto start=axis?canvas.selectionAxisHandlePositions().at(1):QPointF(320,240);
        const auto step=[&](double dx){return start+QPointF(dx,0);};
        mouse(canvas,QEvent::MouseButtonPress,start);
        mouse(canvas,QEvent::MouseMove,step(40));
        // The accepted body sits half a metre above where any local feedback would.
        auto accepted=body;for(auto& s:accepted.segments){s.start.y+=1.0;s.end.y+=1.0;}
        require(axis?canvas.completeEntityTransformPreview(serial,std::vector<CanvasEntity>{accepted})
                    :canvas.completeEntitiesMovePreview(serial,std::vector<CanvasEntity>{accepted}),"accepted result");
        require(is_red(render(canvas),320,240-80),"accepted result is displayed");
        invalid=true;
        mouse(canvas,QEvent::MouseMove,step(80));
        invalid=false;
        mouse(canvas,QEvent::MouseMove,step(120));
        require(requests==3,"later sample requests after an invalid one");
        const auto shown=render(canvas);
        // Local feedback from the source covers y 200..280 at x 440; the retired
        // accepted result (one metre higher) covered y 120..200 there instead.
        require(is_red(shown,440,240)&&!is_red(shown,440,160),
            "pending successor of an invalid sample must not resurrect the earlier accepted result");
        QKeyEvent escape(QEvent::KeyPress,Qt::Key_Escape,Qt::NoModifier);QApplication::sendEvent(&canvas,&escape);
    }
}

void display_only_tickets_follow_pointer_without_authorizing() {
    for(int gesture=0;gesture<4;++gesture) {
        PlanCanvas canvas;setup(canvas);
        const auto body=filled_body();canvas.setEntities({body});
        std::uint64_t serial{};int requests{},commits{};
        CanvasPreviewDisplayState ticket;
        const auto pending=[&](std::uint64_t value) {
            serial=value;++requests;
            ticket=gesture==0?canvas.entitiesMovePreviewDisplayState():canvas.entityTransformPreviewDisplayState();
            require(ticket.valid()&&ticket.serial()==value,"ticket captured from the current proposal");
            require(gesture==0?canvas.markEntitiesMovePreviewPending(value):canvas.markEntityTransformPreviewPending(value),
                "delayed provider owns the request");
            return std::optional<std::vector<CanvasEntity>>{};
        };
        canvas.setEntitiesMovePreviewRequested([&](QStringList,Vec2,std::uint64_t t){return pending(t);});
        canvas.setEntityTransformPreviewRequested([&](QString,double,double,Vec2,std::uint64_t t){return pending(t);});
        canvas.setEntityAxisResizePreviewRequested([&](QString,double,double,Vec2,std::uint64_t t){return pending(t);});
        canvas.setEntitiesMoveRequested([&](QStringList,Vec2){++commits;return false;});
        canvas.setEntityTransformRequested([&](QString,double,double){++commits;return false;});
        canvas.setEntityAxisResizeRequested([&](QString,double,double,Vec2){++commits;return false;});
        const auto display=[&](const CanvasPreviewDisplayState& state,const CanvasEntity& result) {
            return gesture==0?canvas.displayEntitiesMovePreview(state,{result}):canvas.displayEntityTransformPreview(state,{result});
        };
        const auto start=gesture_start(canvas,gesture),end=gesture_end(start,gesture);
        mouse(canvas,QEvent::MouseButtonPress,start);mouse(canvas,QEvent::MouseMove,end);
        const auto first=ticket;const auto first_serial=serial;
        mouse(canvas,QEvent::MouseMove,end+QPointF(24,-20));
        require(requests==2&&serial>first_serial,"successor request is the current serial");
        const auto current=serial;
        const auto before=render(canvas),output=render(canvas,true);
        auto exact=body;for(auto& s:exact.segments){s.start.x+=.4;s.end.x+=.4;}
        require(display(first,exact),"older same-gesture result is displayed while the successor is pending");
        require(serial==current&&(gesture==0?canvas.entitiesMovePreviewSerial()==current:canvas.entityTransformPreviewSerial()==current),
            "display never changes the current serial");
        require(gesture==0?canvas.entitiesMovePreviewPending():canvas.entityTransformPreviewPending(),
            "display never releases the pending current request");
        require(render(canvas)!=before&&render(canvas,true)==output,"displayed result repaints, screen only");
        require(canvas.entities().front().segments.front().start.x==-1&&commits==0,"display never changes source or commits");
        require(!display(first,exact),"a result no newer than the displayed one is refused");
        require(!(gesture==0?canvas.completeEntitiesMovePreview(first_serial,std::vector<CanvasEntity>{exact}):
                  canvas.completeEntityTransformPreview(first_serial,std::vector<CanvasEntity>{exact})),
            "an old serial still cannot complete the current request");
        require(!display(CanvasPreviewDisplayState{},exact),"an invalid state is inert");
        require(!canvas.previewReleasePending(),"the pointer still owns its pending preview");
        mouse(canvas,QEvent::MouseButtonRelease,end+QPointF(24,-20));
        QApplication::processEvents();
        require(commits==0,"a displayed older result cannot authorize the pending release");
        require(canvas.previewReleasePending(),"the scheduler sees a release waiting for the current result");
        require(gesture==0?canvas.entitiesMovePreviewPending():canvas.entityTransformPreviewPending(),
            "release still waits for the current serial");
        QKeyEvent escape(QEvent::KeyPress,Qt::Key_Escape,Qt::NoModifier);QApplication::sendEvent(&canvas,&escape);
        require(!canvas.previewReleasePending(),"cancellation retires the pending release");
        require(!display(first,exact)&&!display(ticket,exact),"cancellation retires every ticket");
        // A new gesture rejects tickets from the previous one even with a newer serial.
        mouse(canvas,QEvent::MouseButtonPress,start);mouse(canvas,QEvent::MouseMove,end);
        require(!display(first,exact),"a ticket from a previous gesture is refused");
        QApplication::sendEvent(&canvas,&escape);
        mouse(canvas,QEvent::MouseButtonRelease,end);
    }
}

void display_tickets_belong_to_their_canvas() {
    PlanCanvas first,second;setup(first);setup(second);
    const auto body=filled_body();first.setEntities({body});second.setEntities({body});
    CanvasPreviewDisplayState first_ticket,second_ticket;
    const auto configure=[&](PlanCanvas& canvas,CanvasPreviewDisplayState& ticket) {
        canvas.setEntitiesMovePreviewRequested([&canvas,&ticket](QStringList,Vec2,std::uint64_t serial)
            ->std::optional<std::vector<CanvasEntity>> {
            require(canvas.markEntitiesMovePreviewPending(serial),"cross-canvas fixture owns its request");
            ticket=canvas.entitiesMovePreviewDisplayState();return {};
        });
        mouse(canvas,QEvent::MouseButtonPress,{320,240});mouse(canvas,QEvent::MouseMove,{360,220});
    };
    configure(first,first_ticket);configure(second,second_ticket);
    require(first_ticket.serial()==second_ticket.serial(),"independent canvases may share serial counters");
    const auto older_second=second_ticket;
    mouse(second,QEvent::MouseMove,{400,200});
    require(!second.displayEntitiesMovePreview(first_ticket,{body}),"another canvas cannot supply a display ticket");
    require(second.displayEntitiesMovePreview(older_second,{body}),"the same canvas still accepts its older display ticket");
}

// Sliding a hosted opening along a curved wall is a rigid turn about the arc
// centre; a straight translation of its artwork would leave the wall.
void curved_hosted_opening_local_feedback() {
    constexpr double pi=std::numbers::pi,half=.3;
    PlanCanvas canvas;setup(canvas);
    const Segment host{{1,0},{0,-1},3*pi/2};
    const auto on_arc=[](double s){return Vec2{std::cos(s),std::sin(s)};};
    const auto station=pi/4;
    const auto centre=on_arc(station);
    const Vec2 tangent{-std::sin(station),std::cos(station)},normal{std::cos(station),std::sin(station)};
    const auto corner=[&](double along,double across) {
        return Vec2{centre.x+tangent.x*along+normal.x*across,centre.y+tangent.y*along+normal.y*across};
    };
    CanvasEntity opening;opening.id="opening";opening.type="opening";opening.selected=true;
    opening.filled=true;opening.fill_color=QColor(240,20,20);opening.fill_opacity=1;opening.hatch_pattern="solid";
    const auto a=corner(-half,-.05),b=corner(half,-.05),c=corner(half,.05),d=corner(-half,.05);
    opening.segments={{a,b,0},{b,c,0},{c,d,0},{d,a,0}};
    opening.opening_width_controls=CanvasOpeningWidthControls{on_arc(station-half),on_arc(station+half),2*half,2,7,host,station-half};
    canvas.setEntities({opening});
    canvas.setEntitiesMovePreviewRequested([&](QStringList,Vec2,std::uint64_t ticket)
        ->std::optional<std::vector<CanvasEntity>> {
        require(canvas.markEntitiesMovePreviewPending(ticket),"hosted move pending");return std::nullopt;});
    canvas.setEntitiesMoveRequested([](QStringList,Vec2){return false;});
    const auto start=screen(centre),end=screen(on_arc(pi/2));
    mouse(canvas,QEvent::MouseButtonPress,start);mouse(canvas,QEvent::MouseMove,end);
    const auto shown=render(canvas);
    // At 90 degrees the body lies along x; the unrotated translation would still
    // lie along the 45-degree tangent and miss this point 0.25 m from its centre.
    require(is_red(shown,int(std::lround(screen({-.25,1}).x())),int(std::lround(screen({-.25,1}).y()))),
        "a hosted opening on a curved wall turns about the arc centre while sliding");
    // Where an unturned (translated) body would still lie along its old tangent.
    require(!is_red(shown,int(std::lround(screen({-.1768,1.1768}).x())),int(std::lround(screen({-.1768,1.1768}).y()))),
        "a hosted opening on a curved wall is not carried by a straight translation");
    QKeyEvent escape(QEvent::KeyPress,Qt::Key_Escape,Qt::NoModifier);QApplication::sendEvent(&canvas,&escape);
}

void delayed_roof_opening_successors() {
    for(const bool group:{false,true}) {
        PlanCanvas canvas;setup(canvas);canvas.setEntities({});canvas.setMetricUnits(true);canvas.setSnapEnabled(true);
        std::vector<CanvasRoofOpeningControls> source{
            {{"roof","a",7},{{-.8,0},{1,0},{0,1},1,1,0,1,1},"roof"}};
        if(group)source.push_back({{"roof","b",7},{{.8,0},{1,0},{0,1},1,1,0,1,1},"roof"});
        canvas.setRoofOpeningControls(source);
        std::vector<CanvasRoofOpeningTarget> targets;for(const auto& control:source)targets.push_back(control.target);
        canvas.setSelectedRoofOpenings(targets);
        int requests{},commits{};std::uint64_t serial{};
        canvas.setRoofOpeningPreviewRequested([&](CanvasRoofOpeningEdit,std::uint64_t ticket)->std::optional<Boundary>{
            ++requests;serial=ticket;require(canvas.markRoofOpeningPreviewPending(ticket),"single roof request pending");return {};});
        canvas.setRoofOpeningGroupPreviewRequested([&](CanvasRoofOpeningGroupEdit,std::uint64_t ticket)->std::optional<Boundary>{
            ++requests;serial=ticket;require(canvas.markRoofOpeningGroupPreviewPending(ticket),"roof cohort pending");return {};});
        canvas.setRoofOpeningEditRequested([&](CanvasRoofOpeningEdit,std::uint64_t){++commits;return false;});
        canvas.setRoofOpeningGroupEditRequested([&](CanvasRoofOpeningGroupEdit,std::uint64_t){++commits;return false;});
        const auto output=render(canvas,true),original=render(canvas);
        const auto start=screen({-.8,0}),end=start+QPointF(40,-24);
        mouse(canvas,QEvent::MouseButtonPress,start);mouse(canvas,QEvent::MouseMove,end);
        require(requests==1,"roof drag requests one exact projection");
        require(original.copy(212,210,10,14)!=render(canvas).copy(212,210,10,14),
            "initial pending roof edit must move projected mouth strokes");
        capture(canvas,group?"delayed-roof-group-initial.png":"delayed-roof-single-initial.png");
        // Away from every mouth outline so only the manufactured ink is sampled.
        const Boundary ink{{{.4,-1.5},{.8,-1.5}},{{.8,-1.5},{.8,-1.3}},{{.8,-1.3},{.4,-1.3}},{{.4,-1.3},{.4,-1.5}}};
        auto admitted=source;for(auto& control:admitted)control.frame.center.x+=.5;
        require(group?canvas.completeRoofOpeningGroupPreview(serial,admitted,ink,true):canvas.completeRoofOpeningPreview(serial,ink),
            "roof projection admitted");
        const auto accepted=render(canvas);const auto old=serial;
        mouse(canvas,QEvent::MouseMove,end+QPointF(24,0));
        require(requests==2&&serial>old,"roof successor requested");
        require(group?canvas.roofOpeningGroupPreviewBoundary().has_value():canvas.roofOpeningPreviewBoundary().has_value(),
            "pending roof successor retains manufactured strokes");
        const auto successor=render(canvas);
        const auto ink_at=[](const QImage& image,int x,int y) {
            for(int dy=-2;dy<=2;++dy){const auto c=image.pixelColor(x,y+dy);if(c.blue()>180&&c.red()<120)return true;}
            return false;
        };
        // Accepted ink spans x 352..384 at y 360; the residual carries it +24 px
        // toward the current pointer instead of freezing or discarding it.
        require(ink_at(accepted,360,360)&&!ink_at(accepted,400,360),"accepted roof ink is displayed");
        require(!ink_at(successor,360,360)&&ink_at(successor,400,360),
            "pending roof successor carries the manufactured ink to the current pointer");
        capture(canvas,group?"delayed-roof-group-successor.png":"delayed-roof-single-successor.png");
        mouse(canvas,QEvent::MouseMove,end+QPointF(24.01,0));
        require(requests==2,"same snapped roof proposal retains current serial");
        mouse(canvas,QEvent::MouseButtonRelease,end+QPointF(24.01,0));
        require(commits==0&&render(canvas,true)==output,"displayed roof projection cannot authorize successor release");
        require(!(group?canvas.completeRoofOpeningGroupPreview(old,admitted,ink,true):canvas.completeRoofOpeningPreview(old,ink)),
            "old roof result rejected");
        QKeyEvent escape(QEvent::KeyPress,Qt::Key_Escape,Qt::NoModifier);QApplication::sendEvent(&canvas,&escape);
    }
}

void snapped_proposals_share_requests() {
    for(int gesture=0;gesture<4;++gesture) {
        PlanCanvas canvas;setup(canvas);canvas.setMetricUnits(true);canvas.setSnapEnabled(true);
        std::uint64_t serial{};int requests{};
        const auto pending=[&](std::uint64_t ticket) {
            ++requests;serial=ticket;
            require(gesture==0?canvas.markEntitiesMovePreviewPending(ticket):canvas.markEntityTransformPreviewPending(ticket),
                "snapped proposal can await projection");return std::optional<std::vector<CanvasEntity>>{};
        };
        canvas.setEntitiesMoveRequested([](QStringList,Vec2){return false;});
        canvas.setEntityTransformRequested([](QString,double,double){return false;});
        canvas.setEntityAxisResizeRequested([](QString,double,double,Vec2){return false;});
        canvas.setEntitiesMovePreviewRequested([&](QStringList,Vec2,std::uint64_t ticket){return pending(ticket);});
        canvas.setEntityTransformPreviewRequested([&](QString,double,double,Vec2,std::uint64_t ticket){return pending(ticket);});
        canvas.setEntityAxisResizePreviewRequested([&](QString,double,double,Vec2,std::uint64_t ticket){return pending(ticket);});
        (void)render(canvas);QPointF start,end;
        if(gesture==0){start={320,240};end=start+QPointF(80,-32);}
        else if(gesture==1){start=*canvas.selectionRotationHandlePosition();end={320+(start.y()-240),240-(start.x()-320)};}
        else if(gesture==2){start=canvas.selectionBounds()->bottomRight();end=start+QPointF(60,40);}
        else{start=canvas.selectionAxisHandlePositions().at(1);end=start+QPointF(80,0);}
        mouse(canvas,QEvent::MouseButtonPress,start);mouse(canvas,QEvent::MouseMove,end);
        require(requests==1,"snapped body proposal starts one request");const auto first=serial;
        mouse(canvas,QEvent::MouseMove,end+QPointF(.001,.001));
        const char* messages[]{"snapped move samples share exact work","snapped rotation samples share exact work",
            "snapped uniform resize samples share exact work","snapped axis resize samples share exact work"};
        require(requests==1&&serial==first,messages[gesture]);
        mouse(canvas,QEvent::MouseMove,end+QPointF(.05,.05),Qt::ShiftModifier);
        require(requests==2&&serial>first,"fine-input modifier still publishes changed effective proposal");
        QKeyEvent escape(QEvent::KeyPress,Qt::Key_Escape,Qt::NoModifier);QApplication::sendEvent(&canvas,&escape);
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
        sustained_move_after_exact_preview();retry_after_rejection_requests_again();
        invalid_sample_retires_displayed_result();display_only_tickets_follow_pointer_without_authorizing();
        display_tickets_belong_to_their_canvas();
        curved_hosted_opening_local_feedback();
        delayed_body_previews();delayed_roof_opening_successors();snapped_proposals_share_requests();compound_presentation_previews();axis_gestures();rotated_symbol_and_rotation();vector_frame_survives_projection_refresh();
        common_angle_snapping_and_cancelled_rotation();preference_angle_snap_step();
        dimensions_are_physical_and_screen_only();label_and_reference_rotation_preview();exact_presentation_move_admission();
        std::cout<<"Axis canvas controls tests passed\n";return 0;
    } catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}
}
