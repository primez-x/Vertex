#include "../src/desktop/plan_canvas.hpp"
#include "support/noninteractive_errors.hpp"

#include <QApplication>
#include <QDir>
#include <QFocusEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>

#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {
using namespace sketch;
using namespace sketch::desktop;
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
QPointF screen(Vec2 point) { return {320 + point.x*80, 240-point.y*80}; }
void mouse(PlanCanvas& canvas, QEvent::Type type, QPointF point) {
    QMouseEvent event(type,point,canvas.mapToGlobal(point.toPoint()),
        type==QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
        type==QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton,Qt::NoModifier);
    QApplication::sendEvent(&canvas,&event);
}
QImage render(PlanCanvas& canvas, bool output=false) {
    QImage image(canvas.size(),QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::white);
    QPainter painter(&image);
    if (output) canvas.renderSceneAt(painter,image.rect(),80,{0,0},Qt::white);
    else {
        // Public scene renderers expose committed output only; interactive
        // feedback is asserted through the widget's actual paint event.
        canvas.setCanvasBackground(Qt::white);
        canvas.render(&painter);
    }
    painter.end();
    return image;
}
int redPixels(const QImage& image) {
    int count=0;
    for (int y=0;y<image.height();++y) for (int x=0;x<image.width();++x) {
        const auto color=image.pixelColor(x,y);
        if (color.red()>175 && color.green()<90 && color.blue()<90) ++count;
    }
    return count;
}
CanvasEntity opening(double width=1) {
    CanvasEntity entity;
    entity.id="opening"; entity.type="opening"; entity.selected=true;
    entity.filled=true;entity.fill_color=QColor(20,170,90);entity.fill_opacity=1;entity.hatch_pattern="solid";
    entity.segments={{{0,-.15},{width,-.15}},{{width,-.15},{width,.15}},
                     {{width,.15},{0,.15}},{{0,.15},{0,-.15}}};
    entity.opening_width_controls=CanvasOpeningWidthControls{{0,0},{width,0},width,2,7};
    return entity;
}
struct Fixture {
    PlanCanvas canvas;
    int requests{}, commits{};
    double requested_scale{}, committed_scale{};
    std::uint64_t serial{};
    Fixture() {
        canvas.resize(640,480); canvas.setGridEnabled(false); canvas.setSnapEnabled(false);
        canvas.setOverviewMapEnabled(false); canvas.setMetricUnits(true);
        canvas.setEntities({opening()});
        canvas.setOpeningWidthResizeRequested([&](QString id,double scale,bool keep,std::uint64_t revision) {
            require(id=="opening" && keep && revision==7,"release lost captured opening identity or revision");
            ++commits; committed_scale=scale;
            return false; // Native admission may reject; canvas never persists the proposal.
        });
        canvas.setOpeningWidthPreviewRequested([&](QString id,double scale,bool keep,std::uint64_t revision)
            -> std::optional<std::vector<CanvasEntity>> {
            require(id=="opening" && keep && revision==7,"preview lost captured opening identity or revision");
            ++requests; requested_scale=scale;
            serial=canvas.openingWidthPreviewSerial();
            require(canvas.markOpeningWidthPreviewPending(serial),"current callback could not mark pending");
            return std::nullopt;
        });
    }
    void begin(double width=1.5) {
        mouse(canvas,QEvent::MouseButtonPress,screen({1,0}));
        mouse(canvas,QEvent::MouseMove,screen({width,0}));
    }
    void release(double width=1.5) { mouse(canvas,QEvent::MouseButtonRelease,screen({width,0})); }
    void cancel() {
        QKeyEvent escape(QEvent::KeyPress,Qt::Key_Escape,Qt::NoModifier);
        QApplication::sendEvent(&canvas,&escape);
    }
    std::vector<CanvasEntity> exact(double width=1.5) { return {opening(width)}; }
};

// A callback returning null after marking pending must retain immediate jamb
// feedback without claiming invalidity or changing the committed scene.
void pendingAndExactCompletion() {
    Fixture f;
    const auto initial=render(f.canvas), output=render(f.canvas,true);
    const auto before=f.canvas.openingWidthPreviewSerial();
    f.begin();
    require(f.requests==1 && f.serial>before && std::abs(f.requested_scale-1.5)<1e-8,
            "proposal must publish its new serial before the callback");
    require(f.commits==0 && render(f.canvas)!=initial,"pending drag must give immediate screen feedback");
    require(redPixels(render(f.canvas))==0,"pending preview must not paint false invalid feedback");
    require(render(f.canvas,true)==output && f.canvas.entities().front().opening_width_controls->width_metres==1,
            "pending proposal changed committed geometry or output");
    require(f.canvas.completeOpeningWidthPreview(f.serial,f.exact()),"current exact completion was rejected");
    require(f.canvas.openingWidthPreviewEntities().size()==1,"exact completion did not retain the opening projection");
    require(redPixels(render(f.canvas))==0 && render(f.canvas,true)==output,
            "valid completion changed output or painted invalid feedback");
    require(!f.canvas.completeOpeningWidthPreview(f.serial,std::nullopt),"duplicate completion was accepted");
    f.release();
    require(f.requests==1 && f.commits==1 && std::abs(f.committed_scale-1.5)<1e-8,
            "same-position valid release must request one final native resize");
    require(!f.canvas.completeOpeningWidthPreview(f.serial,f.exact()),"ended gesture accepted a completion");
    require(render(f.canvas,true)==output,"rejected native command changed committed output");
}

void successorRetainsDisplay() {
    Fixture f;const auto output=render(f.canvas,true);(void)render(f.canvas);f.begin();
    require(f.canvas.completeOpeningWidthPreview(f.serial,f.exact()),"first opening result admitted");
    const auto accepted=render(f.canvas);const auto old=f.serial;
    require(accepted.pixelColor(416,247).green()>120&&accepted.pixelColor(416,247).red()<80,
        "opening continuity sample must contain the accepted filled body");
    mouse(f.canvas,QEvent::MouseMove,screen({1.75,0}));
    require(f.requests==2 && f.serial>old,"successor opening request starts once");
    const auto successor=render(f.canvas);
    require(accepted.copy(411,246,15,3)==successor.copy(411,246,15,3),
        "pending opening successor keeps the accepted body behind the current jamb");
    // Accepted body ends at 1.5 m (x 440); the residual stretches it to the
    // current 1.75 m jamb (x 460) instead of freezing at the accepted width.
    const auto green=[](const QImage& image,int x,int y){const auto c=image.pixelColor(x,y);return c.green()>120&&c.red()<80;};
    require(!green(accepted,450,235)&&green(successor,450,235),
        "pending opening successor stretches the accepted body to the current width");
    const auto directory=qEnvironmentVariable("VERTEX_TEST_CAPTURE_DIR");
    if(!directory.isEmpty()) {
        require(QDir().mkpath(directory),"opening capture directory exists");
        require(accepted.save(QDir(directory).filePath("delayed-opening-accepted.png"))&&
            render(f.canvas).save(QDir(directory).filePath("delayed-opening-successor.png")),"opening captures save");
    }
    mouse(f.canvas,QEvent::MouseMove,screen({1.75,0}));
    require(f.requests==2,"identical effective opening width reuses request");
    f.release(1.75);
    require(f.commits==1 && std::abs(f.committed_scale-1.75)<1e-8 && render(f.canvas,true)==output,
        "opening release must use current native admission parameters rather than displayed width");
    require(!f.canvas.completeOpeningWidthPreview(old,f.exact()),"old opening display remains stale");
    f.cancel();

    Fixture snapped;snapped.canvas.setSnapEnabled(true);(void)render(snapped.canvas);snapped.begin();
    const auto first=snapped.serial;
    mouse(snapped.canvas,QEvent::MouseMove,screen({1.501,0}));
    require(snapped.requests==1&&snapped.serial==first,"different pointers with identical snapped width share exact work");
    snapped.cancel();
}

// An exact result whose body is taller than the source stretch, so it cannot be
// confused with the local feedback painted from the committed opening.
std::vector<CanvasEntity> tallExact(double width) {
    auto entity=opening(width);
    entity.segments={{{0,-.3},{width,-.3}},{{width,-.3},{width,.3}},{{width,.3},{0,.3}},{{0,.3},{0,-.3}}};
    return {entity};
}
bool greenAt(const QImage& image,int x,int y) {
    const auto c=image.pixelColor(x,y);return c.green()>120&&c.red()<80;
}

void retryAfterFailureAndInvalidRetirement() {
    {
        Fixture f;bool fail=true;
        f.canvas.setOpeningWidthPreviewRequested([&](QString,double,bool,std::uint64_t)
            ->std::optional<std::vector<CanvasEntity>> {
            ++f.requests;f.serial=f.canvas.openingWidthPreviewSerial();
            if(fail)throw std::runtime_error("opening provider failure");
            require(f.canvas.markOpeningWidthPreviewPending(f.serial),"retry sample pending");return std::nullopt;});
        f.begin();
        require(f.requests==1,"failing opening sample requested once");
        mouse(f.canvas,QEvent::MouseMove,screen({1.5,0}));
        require(f.requests==2,"identical opening width after a failure requests again");
        fail=false;mouse(f.canvas,QEvent::MouseMove,screen({1.5,0}));
        require(f.requests==3,"identical opening width retries once the provider recovers");
        mouse(f.canvas,QEvent::MouseMove,screen({1.5,0}));
        require(f.requests==3,"identical pending opening width shares its request");
        f.cancel();
    }
    {
        Fixture f;bool invalid=false;
        f.canvas.setOpeningWidthPreviewRequested([&](QString,double,bool,std::uint64_t)
            ->std::optional<std::vector<CanvasEntity>> {
            ++f.requests;f.serial=f.canvas.openingWidthPreviewSerial();
            if(invalid)return std::vector<CanvasEntity>{};
            require(f.canvas.markOpeningWidthPreviewPending(f.serial),"sample pending");return std::nullopt;});
        f.begin();
        require(f.canvas.completeOpeningWidthPreview(f.serial,tallExact(1.5)),"tall result accepted");
        // The tall body covers y 216..264; the committed source covers only 228..252.
        require(greenAt(render(f.canvas),400,220),"accepted tall result is displayed");
        invalid=true;mouse(f.canvas,QEvent::MouseMove,screen({1.6,0}));
        require(f.canvas.openingWidthPreviewEntities().empty(),"an invalid opening sample clears displayed entities");
        invalid=false;mouse(f.canvas,QEvent::MouseMove,screen({1.75,0}));
        require(f.requests==3&&!greenAt(render(f.canvas),400,220)&&greenAt(render(f.canvas),400,235),
            "pending successor of an invalid sample must not resurrect the earlier tall result");
        f.cancel();
    }
}

void displayOnlyTicketFollowsPointer() {
    Fixture f;CanvasPreviewDisplayState ticket;
    f.canvas.setOpeningWidthPreviewRequested([&](QString,double scale,bool,std::uint64_t)
        ->std::optional<std::vector<CanvasEntity>> {
        ++f.requests;f.requested_scale=scale;f.serial=f.canvas.openingWidthPreviewSerial();
        ticket=f.canvas.openingWidthPreviewDisplayState();
        require(ticket.valid()&&ticket.serial()==f.serial,"opening ticket captured from the current proposal");
        require(f.canvas.markOpeningWidthPreviewPending(f.serial),"ticket sample pending");return std::nullopt;});
    const auto output=render(f.canvas,true);
    f.begin();const auto first=ticket;const auto first_serial=f.serial;
    mouse(f.canvas,QEvent::MouseMove,screen({1.75,0}));
    require(f.requests==2&&f.serial>first_serial,"opening successor is current");
    const auto current=f.serial;
    require(!greenAt(render(f.canvas),400,220),"nothing tall before the older result arrives");
    require(f.canvas.displayOpeningWidthPreview(first,tallExact(1.5)),"older opening result is displayed");
    require(f.canvas.openingWidthPreviewSerial()==current&&f.canvas.openingWidthPreviewPending(),
        "display never changes or completes the current opening serial");
    const auto shown=render(f.canvas);
    // The tall body (y 216..264) stretches by 1.75/1.5 about the fixed jamb.
    require(greenAt(shown,400,220)&&greenAt(shown,450,220)&&!greenAt(shown,470,220),
        "older result is carried to the current pointer");
    require(render(f.canvas,true)==output&&f.canvas.entities().front().opening_width_controls->width_metres==1,
        "display never edits source or output");
    require(!f.canvas.displayOpeningWidthPreview(first,tallExact(1.5)),"no result newer than the displayed one is accepted twice");
    require(!f.canvas.completeOpeningWidthPreview(first_serial,tallExact(1.5)),
        "older serial cannot complete the current opening request");
    f.cancel();
    require(!f.canvas.displayOpeningWidthPreview(first,tallExact(1.5))&&!f.canvas.displayOpeningWidthPreview(ticket,tallExact(1.75)),
        "cancellation retires every opening ticket");
}

// Serial identity must distinguish repeated parameters, both within one drag
// and after cancel/new press; parameter equality cannot authorize old work.
void staleCompletions() {
    Fixture f; f.begin();
    const auto old=f.serial;
    mouse(f.canvas,QEvent::MouseMove,screen({1.75,0}));
    mouse(f.canvas,QEvent::MouseMove,screen({1.5,0}));
    require(f.serial>old && !f.canvas.completeOpeningWidthPreview(old,f.exact()),
            "same-parameter newer proposal accepted old exact geometry");
    require(!f.canvas.markOpeningWidthPreviewPending(old),"old serial was allowed to mark pending");
    const auto canceled=f.serial;
    f.cancel();
    require(f.canvas.openingWidthPreviewSerial()>canceled &&
            !f.canvas.completeOpeningWidthPreview(canceled,f.exact()),"canceled gesture accepted geometry");
    f.release();
    require(f.commits==0,"canceled release issued a command");
    f.begin();
    require(f.serial>canceled && !f.canvas.completeOpeningWidthPreview(canceled,f.exact()),
            "new gesture with same parameters accepted previous gesture geometry");
    require(f.canvas.completeOpeningWidthPreview(f.serial,f.exact()),"new gesture rejected current completion");
    f.cancel();
}

void invalidAndPendingRelease() {
    Fixture invalid; invalid.begin();
    require(invalid.canvas.completeOpeningWidthPreview(invalid.serial,std::nullopt),
            "current invalid result was not consumed");
    require(redPixels(render(invalid.canvas))>0,"invalid exact result must paint invalid feedback");
    invalid.release();
    require(invalid.requests==1 && invalid.commits==0,"invalid release must reject without restarting preview");

    Fixture pending; pending.begin();
    const auto serial=pending.serial;
    pending.release(); pending.release();
    require(pending.commits==1 && std::abs(pending.committed_scale-1.5)<1e-8,
            "pending release must request final native admission exactly once");
    require(!pending.canvas.completeOpeningWidthPreview(serial,pending.exact()),
            "pending release left stale completion live");

    Fixture releaseOnly;
    mouse(releaseOnly.canvas,QEvent::MouseButtonPress,screen({1,0})); releaseOnly.release(1.75);
    require(releaseOnly.requests==1 && releaseOnly.commits==1 &&
            std::abs(releaseOnly.committed_scale-1.75)<1e-8,"release-only movement lost final native admission");

    Fixture inverse; inverse.begin();
    const auto previous=inverse.serial;
    mouse(inverse.canvas,QEvent::MouseMove,screen({-.5,0}));
    require(!inverse.canvas.completeOpeningWidthPreview(previous,inverse.exact()),
            "crossing fixed jamb failed to invalidate previous exact work");
    require(redPixels(render(inverse.canvas))>0,"inverse width must remain invalid while work was pending");
    inverse.release(-.5);
    require(inverse.commits==0 && inverse.requests==1,"inverse width reached geometry admission");

    Fixture missing; missing.begin();
    auto host=opening(); host.id="host";
    require(missing.canvas.completeOpeningWidthPreview(missing.serial,std::vector<CanvasEntity>{host}),
            "malformed exact completion was not consumed");
    missing.release();
    require(missing.commits==0,"exact result lacking opening identity must reject");
}

void sceneAndFocusCancellation() {
    for (bool focus : {false,true}) {
        Fixture f; f.begin(); const auto serial=f.serial;
        if (focus) {
            QFocusEvent event(QEvent::FocusOut,Qt::OtherFocusReason);
            QApplication::sendEvent(&f.canvas,&event);
        } else f.canvas.setEntities({opening()});
        require(f.canvas.openingWidthPreviewSerial()>serial &&
                !f.canvas.completeOpeningWidthPreview(serial,f.exact()),"scene/focus cancellation accepted stale work");
        f.release();
        require(f.commits==0,"scene/focus cancellation left an active resize");
    }
}

void synchronousContract() {
    Fixture f;
    f.canvas.setOpeningWidthPreviewRequested([&](QString,double scale,bool,std::uint64_t)
        -> std::optional<std::vector<CanvasEntity>> { ++f.requests; return f.exact(scale); });
    f.begin(); f.release();
    require(f.requests==1 && f.commits==1,"legacy synchronous valid callback contract changed");
    f.canvas.setOpeningWidthPreviewRequested([](QString,double,bool,std::uint64_t)
        -> std::optional<std::vector<CanvasEntity>> { return std::nullopt; });
    f.begin();
    require(redPixels(render(f.canvas))>0,"legacy null preview no longer rejects immediately");
    f.release();
    require(f.commits==1,"legacy rejected preview reached resize command");
}

void callbackFailureAndReplacement() {
    Fixture failure;
    failure.canvas.setOpeningWidthPreviewRequested([&](QString,double,bool,std::uint64_t)
        -> std::optional<std::vector<CanvasEntity>> {
        failure.serial=failure.canvas.openingWidthPreviewSerial();
        require(failure.canvas.markOpeningWidthPreviewPending(failure.serial),"failure callback could not mark pending");
        throw std::runtime_error("projection failure");
    });
    failure.begin();
    require(redPixels(render(failure.canvas))>0 &&
            !failure.canvas.completeOpeningWidthPreview(failure.serial,failure.exact()),
            "throwing callback left deferred projection live");
    failure.release();
    require(failure.commits==0,"failed projection reached final admission");

    Fixture replacement;
    replacement.canvas.setOpeningWidthPreviewRequested([&](QString,double,bool,std::uint64_t)
        -> std::optional<std::vector<CanvasEntity>> {
        replacement.serial=replacement.canvas.openingWidthPreviewSerial();
        require(replacement.canvas.markOpeningWidthPreviewPending(replacement.serial),"replacement callback could not mark pending");
        replacement.canvas.setEntities({opening()});
        return replacement.exact();
    });
    const auto output=render(replacement.canvas,true);
    replacement.begin(); replacement.release();
    require(replacement.commits==0 && render(replacement.canvas,true)==output &&
            !replacement.canvas.completeOpeningWidthPreview(replacement.serial,replacement.exact()),
            "synchronous scene replacement revived canceled geometry");
}
} // namespace

int main(int argc,char** argv) {
    sketch::testing::noninteractive_errors();
    QApplication app(argc,argv);
    try {
        successorRetainsDisplay();retryAfterFailureAndInvalidRetirement();displayOnlyTicketFollowsPointer();
        pendingAndExactCompletion(); staleCompletions(); invalidAndPendingRelease();
        sceneAndFocusCancellation(); synchronousContract(); callbackFailureAndReplacement();
        std::cout << "deferred opening preview checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "deferred_opening_preview_tests: " << error.what() << '\n';
        return 1;
    }
}
