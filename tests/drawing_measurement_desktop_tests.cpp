#include "sketch/desktop/main_window.hpp"
#include "sketch/desktop/drawing_input_panel.hpp"
#include "sketch/document.hpp"
#include "sketch/document_digest.hpp"
#include "sketch/boundary_entity.hpp"
#include "sketch/measurement_linework.hpp"
#include "../src/desktop/plan_canvas.hpp"
#include "support/noninteractive_errors.hpp"

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFont>
#include <QFontDatabase>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPushButton>
#include <QMouseEvent>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolButton>
#include <QUuid>

#include <cmath>
#include <iostream>
#include <optional>
#include <stdexcept>

namespace {
using namespace sketch;
using namespace sketch::desktop;

void require(bool condition,const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void process_events() { QCoreApplication::processEvents(QEventLoop::AllEvents,50); }
PlanCanvas& prepare(MainWindow& window) {
    window.setAttribute(Qt::WA_DontShowOnScreen,true);
    window.resize(1400,900);
    window.show();
    QApplication::setActiveWindow(&window);
    process_events();
    auto* canvas=dynamic_cast<PlanCanvas*>(window.findChild<QWidget*>(QStringLiteral("measurementPlanCanvas")));
    require(canvas,"native measurement canvas is available");
    canvas->setOverviewMapEnabled(false);
    return *canvas;
}
QPointF screen(const PlanCanvas& canvas,Vec2 point) {
    const auto center=QRectF(canvas.rect()).center();
    const auto view=canvas.viewCenter();
    return center+QPointF((point.x-view.x)*canvas.viewScale(),-(point.y-view.y)*canvas.viewScale());
}
void mouse(PlanCanvas& canvas,QEvent::Type type,QPointF point,Qt::MouseButton button=Qt::NoButton,
           Qt::MouseButtons buttons=Qt::NoButton) {
    QMouseEvent event(type,point,canvas.mapToGlobal(point.toPoint()),button,buttons,Qt::NoModifier);
    QApplication::sendEvent(&canvas,&event);
    process_events();
}
void click(PlanCanvas& canvas,Vec2 point) {
    const auto location=screen(canvas,point);
    mouse(canvas,QEvent::MouseMove,location);
    mouse(canvas,QEvent::MouseButtonPress,location,Qt::LeftButton,Qt::LeftButton);
    mouse(canvas,QEvent::MouseButtonRelease,location,Qt::LeftButton);
}
void key(QWidget& widget,int value,const QString& text={}) {
    QKeyEvent event(QEvent::KeyPress,value,Qt::NoModifier,text);
    QApplication::sendEvent(&widget,&event);
    process_events();
}
Segment baseline(const Entity& entity) {
    const auto& value=entity.properties.at("baseline");
    return {{value.at("start").at(0).get<double>(),value.at("start").at(1).get<double>()},
            {value.at("end").at(0).get<double>(),value.at("end").at(1).get<double>()},0};
}
Segment new_wall(const DocumentSnapshot& before,const DocumentSnapshot& after) {
    std::optional<Segment> result;
    for (const auto& [id,entity] : after.entities())
        if (entity.type=="wall" && !before.entities().contains(id)) {
            require(!result,"one canvas click creates one wall");
            result=baseline(entity);
        }
    require(result.has_value(),"canvas endpoint click commits a physical wall");
    return *result;
}
bool close(Vec2 a,Vec2 b) { return std::hypot(a.x-b.x,a.y-b.y)<1e-10; }
bool stationary_right(PlanCanvas& canvas) {
    bool menu=false;
    // Prevent a regression to a modal drawing menu from blocking the fixture.
    QTimer observer;
    QObject::connect(&observer,&QTimer::timeout,[&] {
        if (auto* popup=qobject_cast<QMenu*>(QApplication::activePopupWidget())) {
            menu=true;
            popup->close();
        }
    });
    observer.start(1);
    const auto point=QRectF(canvas.rect()).center();
    mouse(canvas,QEvent::MouseButtonPress,point,Qt::RightButton,Qt::RightButton);
    mouse(canvas,QEvent::MouseButtonRelease,point,Qt::RightButton);
    observer.stop();
    return menu;
}
void right_drag(PlanCanvas& canvas) {
    const auto point=QRectF(canvas.rect()).center();
    const auto before=canvas.viewCenter();
    mouse(canvas,QEvent::MouseButtonPress,point,Qt::RightButton,Qt::RightButton);
    mouse(canvas,QEvent::MouseMove,point+QPointF(42,27),Qt::NoButton,Qt::RightButton);
    mouse(canvas,QEvent::MouseButtonRelease,point+QPointF(48,32),Qt::RightButton);
    require(std::abs(canvas.viewCenter().x-before.x+48/canvas.viewScale())<1e-9 &&
            std::abs(canvas.viewCenter().y-before.y-32/canvas.viewScale())<1e-9,
            "native right drag pans to its final release position");
}

void capture(PlanCanvas& canvas,const QString& name) {
    const auto directory=qEnvironmentVariable("VERTEX_TEST_CAPTURE_DIR");
    if (directory.isEmpty()) return;
    process_events();
    require(QDir().mkpath(directory) && canvas.grab().save(QDir(directory).filePath(name)),
            "native compact measurement and draft overview screenshot saves for review");
}

void test_overview_navigation_preserves_native_authoring_drafts() {
    for (const auto kind : {0,1,2}) {
        MainWindow window;
        auto& canvas=prepare(window);
        window.setMetricUnits(true);
        auto* snap=window.findChild<QToolButton*>(QStringLiteral("snapTool"));
        require(snap,"overview fixture has the persistent native Snap control");
        snap->setChecked(false);
        canvas.setSnapEnabled(false);
        canvas.setOverviewMapEnabled(true);
        canvas.setViewTransform({0,0},80);
        if (kind==0)
            require(window.beginBoundaryDrawing(BoundaryAuthoringMode::draw_first,QStringLiteral("measurement")),
                    "overview fixture starts native boundary drawing");
        if (kind==2)
            require(window.beginMeasurementLinework(),"overview fixture starts native measured-line drawing");
        click(canvas,{0,0});
        if (kind==0) click(canvas,{2,0});
        mouse(canvas,QEvent::MouseMove,screen(canvas,{1,2}));
        const auto wall=canvas.wallPreview();
        const auto draft=canvas.boundaryDraftPreview();
        require(kind==1 ? wall.has_value() : draft && draft->rubber_band,
                "overview fixture has an active native rubber-band endpoint");
        require(close(kind==1 ? wall->end : draft->rubber_band->end,{1,2}),
                "overview fixture preserves its explicit unsnapped endpoint");
        const auto before=window.document().snapshot();
        const auto same_point=[](const std::optional<Vec2>& a,const std::optional<Vec2>& b) {
            return a.has_value()==b.has_value() && (!a || (a->x==b->x && a->y==b->y));
        };
        const auto unchanged=[&] {
            require(window.document().revision()==before.revision() &&
                        window.document().snapshot().entities()==before.entities(),
                    "overview gesture must not create or edit document entities");
            if (kind==1) {
                const auto current=canvas.wallPreview();
                require(current && same_point(current->start,wall->start) && same_point(current->end,wall->end) &&
                            current->dimension_text==wall->dimension_text,
                        "overview press, move and release must retain the native pending wall endpoint and readout");
            } else {
                const auto current=canvas.boundaryDraftPreview();
                require(current && current->rubber_band &&
                            same_point(current->rubber_band->start,draft->rubber_band->start) &&
                            same_point(current->rubber_band->end,draft->rubber_band->end) &&
                            current->segments.size()==draft->segments.size() &&
                            same_point(current->anchor,draft->anchor) &&
                            same_point(current->pen_position,draft->pen_position),
                        "overview press, move and release must retain native boundary and measured-line authoring geometry");
                for (std::size_t index=0;index<draft->segments.size();++index) {
                    const auto& a=current->segments[index];
                    const auto& b=draft->segments[index];
                    require(a.start.x==b.start.x && a.start.y==b.start.y &&
                                a.end.x==b.end.x && a.end.y==b.end.y && a.sweep_radians==b.sweep_radians,
                            "overview navigation retains every accepted analytical draft segment exactly");
                }
            }
        };
        const auto map_center=canvas.overviewMapRect().adjusted(8,22,-8,-8).center();
        const auto scale=canvas.viewScale();
        mouse(canvas,QEvent::MouseMove,map_center);
        unchanged();
        mouse(canvas,QEvent::MouseButtonPress,map_center,Qt::LeftButton,Qt::LeftButton);
        unchanged();
        const auto centered=canvas.viewCenter();
        require(close(centered,kind==0 ? Vec2{1,1} : Vec2{0.5,1}),
                "native overview center matches the unchanged draft geometry bounds");
        mouse(canvas,QEvent::MouseMove,map_center+QPointF(16,-12),Qt::NoButton,Qt::LeftButton);
        unchanged();
        mouse(canvas,QEvent::MouseButtonRelease,map_center+QPointF(24,-16),Qt::LeftButton);
        unchanged();
        // These fixtures span exactly 2 m vertically; the overview's padding
        // makes that span 2.52 m. Its taller axis governs the map scale.
        const auto map_height=canvas.overviewMapRect().adjusted(8,22,-8,-8).height();
        const Vec2 released{centered.x+24*2.52/map_height,centered.y+16*2.52/map_height};
        require(canvas.viewScale()==scale && close(canvas.viewCenter(),released),
                "overview drag pans to the final release position without changing zoom");
        mouse(canvas,QEvent::MouseButtonPress,map_center,Qt::LeftButton,Qt::LeftButton);
        unchanged();
        mouse(canvas,QEvent::MouseButtonRelease,map_center,Qt::LeftButton);
        unchanged();
        require(close(canvas.viewCenter(),centered),
                "draft overview bounds stay stable throughout navigation and repeated center clicks");
        capture(canvas,QStringLiteral("compact-overview-%1.png").arg(kind==0 ? "boundary" : kind==1 ? "wall" : "measured-lines"));
    }
}

void test_clicked_wall_geometry_and_visible_lengths_at_zoom() {
    struct Example { bool metric; double scale; double step; const char* text; };
    for (const auto example : {Example{false,20,0.3048,"13 ft 0 in"},
                               Example{false,80,0.0762,"3 ft 3 in"},
                               Example{false,400,0.0254,"1 ft 1 in"},
                               Example{false,1600,0.00635,"3 1/4 in"},
                               Example{false,4000,0.0015875,"13/16 in"},
                               Example{true,20,0.5,"6.5 m"},
                               Example{true,80,0.1,"1.3 m"},
                               Example{true,400,0.02,"260 mm"},
                               Example{true,1600,0.005,"65 mm"},
                               Example{true,4000,0.002,"26 mm"}}) {
        MainWindow window;
        auto& canvas=prepare(window);
        window.setMetricUnits(example.metric);
        canvas.zoomBy(example.scale/canvas.viewScale(),QRectF(canvas.rect()).center());
        canvas.setSnapEnabled(false);
        const Vec2 origin{0.01317,-0.01931};
        click(canvas,origin);
        require(canvas.wallPreview() && close(canvas.wallPreview()->start,origin),
                "native mouse wall start retains its off-grid coordinates");
        canvas.setSnapEnabled(true);
        const auto raw_length=13.23*example.step;
        const Vec2 raw{origin.x+raw_length*std::cos(0.37),origin.y+raw_length*std::sin(0.37)};
        mouse(canvas,QEvent::MouseMove,screen(canvas,raw));
        require(canvas.wallPreview().has_value(),"native wall rubber band is visible");
        const auto preview=*canvas.wallPreview();
        require(std::abs(std::hypot(preview.end.x-origin.x,preview.end.y-origin.y)-13*example.step)<1e-10 &&
                preview.dimension_text==QString::fromLatin1(example.text),
                "native wall preview has the actual common length and exact fractional/metric label");
        const auto before=window.document().snapshot();
        click(canvas,raw);
        const auto committed=new_wall(before,window.document().snapshot());
        require(close(committed.start,origin) && close(committed.end,preview.end),
                "true clicked wall persists the same off-grid-origin geometry shown by its preview");
        const auto after=window.document().snapshot();
        right_drag(canvas);
        require(canvas.wallPreview() && window.document().snapshot().entities()==after.entities(),
                "right dragging retains the pending chain and all committed walls");
        require(!stationary_right(canvas) && !canvas.wallPreview() &&
                window.document().revision()==after.revision() &&
                window.document().snapshot().entities()==after.entities(),
                "stationary right click ends only the pending wall chain without a menu or history edit");
    }
    require(PlanCanvas::drawingLengthText(31.5*0.3048,false)==QStringLiteral("31 ft 6 in") &&
            PlanCanvas::drawingLengthText((31*12+6.25)*0.0254,false)==QStringLiteral("31 ft 6 1/4 in"),
            "drawing fractional labels retain whole feet, inches, and reduced fractions");
}

void test_arbitrary_drawing_lengths_are_readable_without_claiming_exactness() {
    struct Example { double metres; bool metric; const char* text; };
    for (const auto example : {Example{std::sqrt(5.0),true,"≈ 2.236 m"},
                               Example{0.1234567,true,"≈ 123 mm"},
                               Example{-0.1234567,true,"≈ -123 mm"},
                               Example{1.234,true,"1.234 m"},
                               Example{0.026,true,"26 mm"},
                               Example{0.1234567,false,"≈ 4 7/8 in"},
                               Example{-0.1234567,false,"≈ -4 7/8 in"},
                               Example{11.99*0.0254,false,"≈ 1 ft 0 in"},
                               Example{0.0015875,false,"1/16 in"},
                               Example{0.0,true,"0 mm"},
                               Example{0.0,false,"0 in"}}) {
        require(PlanCanvas::drawingLengthText(example.metres,example.metric)==
                    QString::fromUtf8(example.text),
                "drawing lengths use millimetres or sixteenths with an honest approximation marker");
    }
    for (const auto metric : {false,true}) {
        MainWindow window;
        auto& canvas=prepare(window);
        window.setMetricUnits(metric);
        auto* snap=window.findChild<QToolButton*>(QStringLiteral("snapTool"));
        require(snap,"arbitrary endpoint fixture has the persistent native Snap control");
        snap->setChecked(false);
        canvas.setSnapEnabled(false);
        click(canvas,{0,0});
        mouse(canvas,QEvent::MouseMove,screen(canvas,{1,2}));
        require(canvas.wallPreview() && close(canvas.wallPreview()->end,{1,2}) &&
                    canvas.wallPreview()->dimension_text.startsWith(QStringLiteral("≈ ")) &&
                    canvas.wallPreview()->dimension_text.size()<24,
                "arbitrary pending wall readout is compact while the endpoint retains exact geometry");
        capture(canvas,QStringLiteral("compact-arbitrary-%1.png").arg(metric ? "metric" : "imperial"));
        const auto before=window.document().snapshot();
        click(canvas,{1,2});
        require(close(new_wall(before,window.document().snapshot()).end,{1,2}),
                "rounded display never rounds committed wall geometry");
    }
}

void test_typed_exact_and_endpoint_priority() {
    for (const auto metric : {false,true}) {
        MainWindow window;
        auto& canvas=prepare(window);
        window.setMetricUnits(metric);
        canvas.setSnapEnabled(false);
        click(canvas,{0.01317,-0.01931});
        canvas.setSnapEnabled(true);
        // Type through the native canvas/input widgets, bypassing the mouse magnet.
        const auto expression=metric ? QStringLiteral("1.234567 m") : QStringLiteral("31 ft 6.2 in");
        canvas.setFocus();
        for (const auto character : expression) {
            auto* target=QApplication::focusWidget();
            require(target,"typed length has a native focused widget");
            key(*target,character.toUpper().unicode(),QString(character));
        }
        auto* input=canvas.findChild<QLineEdit*>(QStringLiteral("drawingLengthInput"));
        require(input && input->text()==expression,"native input receives the full precise expression");
        const auto before=window.document().snapshot();
        key(*input,Qt::Key_Right);
        const auto wall=new_wall(before,window.document().snapshot());
        const auto expected=metric ? 1.234567 : (31*12+6.2)*0.0254;
        require(std::abs(wall.end.x-wall.start.x-expected)<1e-11 && wall.start.y==wall.end.y,
                "typed wall length bypasses quantization and retains its exact requested geometry");
        require(!stationary_right(canvas),"right click cancels pending typed chain without a menu");

        // A raw arbitrary existing endpoint wins over the relative length ladder.
        const auto target_id=window.createStraightWall({1.1234567,2.2345678},{2.3456789,2.7890123});
        require(!target_id.isEmpty(),"exact endpoint target fixture is created");
        require(window.selectEntity({}),"clear selection before starting a new mouse wall");
        canvas.setSnapEnabled(false);
        click(canvas,{-1.234567,-0.345678});
        canvas.setSnapEnabled(true);
        const Vec2 target{1.1234567,2.2345678};
        mouse(canvas,QEvent::MouseMove,screen(canvas,target)+QPointF(1,1));
        require(canvas.wallPreview() && close(canvas.wallPreview()->end,target),
                "native preview preserves a true arbitrary existing endpoint");
        const auto before_endpoint=window.document().snapshot();
        click(canvas,target);
        const auto joined=new_wall(before_endpoint,window.document().snapshot());
        require(joined.end.x==target.x && joined.end.y==target.y,
                "native object snap commits exact existing coordinates rather than a rounded length");
        require(!stationary_right(canvas),"joined pending chain cancels without a menu");
    }
}

void test_measurement_draft_length_closure_and_right_cancel() {
    for (const auto metric : {false,true}) {
        MainWindow window;
        auto& canvas=prepare(window);
        window.setMetricUnits(metric);
        require(window.beginBoundaryDrawing(BoundaryAuthoringMode::draw_first,QStringLiteral("measurement")),
                "native measurement drafting starts");
        const auto original=window.document().snapshot();
        canvas.setSnapEnabled(false);
        const Vec2 origin{0.01317,-0.01931};
        click(canvas,origin);
        canvas.setSnapEnabled(true);
        const auto step=metric ? 0.1 : 0.0762;
        const auto length=13.23*step;
        const Vec2 raw{origin.x+length*std::cos(0.37),origin.y+length*std::sin(0.37)};
        mouse(canvas,QEvent::MouseMove,screen(canvas,raw));
        require(canvas.boundaryDraftPreview() && canvas.boundaryDraftPreview()->rubber_band,
                "native measurement draft exposes its live next segment");
        const auto segment=*canvas.boundaryDraftPreview()->rubber_band;
        require(std::abs(std::hypot(segment.end.x-origin.x,segment.end.y-origin.y)-13*step)<1e-10,
                "mouse measurement segment uses the same useful relative length ladder");
        click(canvas,raw);
        require(canvas.boundaryDraftPreview() && !canvas.boundaryDraftPreview()->segments.empty() &&
                close(canvas.boundaryDraftPreview()->segments.front().end,segment.end),
                "measurement click accepts exactly its live rubber-band endpoint");
        require(!canvas.boundaryDraftPreview()->labels.empty() &&
                canvas.boundaryDraftPreview()->labels.front().text==
                    (metric ? QStringLiteral("1.3 m") : QStringLiteral("3 ft 3 in")),
                "native draft dimension exposes the same exact useful length as its geometry");
        right_drag(canvas);
        require(canvas.boundaryDraftPreview() && window.document().revision()==original.revision(),
                "right panning preserves an uncommitted measurement draft");
        require(!stationary_right(canvas) && !canvas.boundaryDraftPreview() &&
                window.document().snapshot().entities()==original.entities() &&
                window.document().revision()==original.revision(),
                "stationary right click cancels the uncommitted measurement without a ghost menu or history edit");
    }
}

void test_mouse_wall_and_measurement_closure_retain_exact_off_grid_anchor() {
    for (const auto measurement : {false,true}) {
        MainWindow window;
        auto& canvas=prepare(window);
        if (measurement)
            require(window.beginBoundaryDrawing(BoundaryAuthoringMode::draw_first,QStringLiteral("measurement")),
                    "measurement closure fixture starts the actual drawing session");
        const Vec2 origin{0.01317,-0.01931};
        canvas.setSnapEnabled(false);
        click(canvas,origin);
        click(canvas,{origin.x+2.12345,origin.y+0.73456});
        click(canvas,{origin.x-0.87654,origin.y+1.93456});
        canvas.setSnapEnabled(true);
        // This closing length is deliberately not an increment. A nearby
        // pointer closes to the original exact node before length rounding.
        const auto location=screen(canvas,origin)+QPointF(1,1);
        mouse(canvas,QEvent::MouseMove,location);
        if (measurement) {
            require(canvas.boundaryDraftPreview() && canvas.boundaryDraftPreview()->rubber_band &&
                    close(canvas.boundaryDraftPreview()->rubber_band->end,origin),
                    "measurement closure preview uses its exact off-grid anchor");
        } else {
            require(canvas.wallPreview() && close(canvas.wallPreview()->end,origin),
                    "wall closure preview uses its exact existing chain anchor");
        }
        mouse(canvas,QEvent::MouseButtonPress,location,Qt::LeftButton,Qt::LeftButton);
        mouse(canvas,QEvent::MouseButtonRelease,location,Qt::LeftButton);
        const auto snapshot=window.document().snapshot();
        if (measurement) {
            bool found=false;
            for (const auto& [id,entity] : snapshot.entities()) {
                (void)id;
                if (entity.type!="measurement_boundary") continue;
                const auto boundary=decode_identified_boundary_entity(entity);
                require(boundary.segments.size()==3 &&
                        close(boundary.segments.front().segment.start,origin) &&
                        boundary.segments.back().segment.end.x==boundary.segments.front().segment.start.x &&
                        boundary.segments.back().segment.end.y==boundary.segments.front().segment.start.y,
                        "true clicked measurement closure commits the exact shared off-grid vertex");
                found=true;
            }
            require(found && !canvas.boundaryDraftPreview(),"clicked measurement closure commits and clears its draft");
        } else {
            int walls=0,anchored_ends=0;
            for (const auto& [id,entity] : snapshot.entities()) {
                (void)id;
                if (entity.type!="wall") continue;
                ++walls;
                const auto value=baseline(entity);
                if (close(value.end,origin)) ++anchored_ends;
            }
            require(walls==3 && anchored_ends==1 && !canvas.wallPreview(),
                    "true clicked wall closure commits its exact final endpoint and ends the chain");
        }
    }
}

void test_new_symbol_placement_right_cancel() {
    MainWindow window;
    auto& canvas=prepare(window);
    auto* list=window.findChild<QListWidget*>(QStringLiteral("symbolLibraryItems"));
    require(list && list->count()>0,"native symbol library contains placeable components");
    // Pick a free-standing component, avoiding doors/windows that require hosts.
    QListWidgetItem* chosen=nullptr;
    for (int index=0;index<list->count();++index) {
        auto* item=list->item(index);
        const auto id=item->data(Qt::UserRole).toString();
        if (id.contains(QStringLiteral("chair"))) { chosen=item; break; }
    }
    require(chosen,"native library offers a chair for new symbol placement");
    const auto chosen_id=chosen->data(Qt::UserRole).toString();
    const auto arm=[&] {
        // A successful placement rebuilds the list, so never reuse its item pointer.
        QListWidgetItem* current=nullptr;
        for (int index=0;index<list->count();++index)
            if (list->item(index)->data(Qt::UserRole).toString()==chosen_id) current=list->item(index);
        require(current,"new component remains available after library refresh");
        // Activation is the library's actual placement signal (double-click or Enter).
        list->itemActivated(current);
        process_events();
    };
    const auto before=window.document().snapshot();
    arm();
    right_drag(canvas);
    require(window.document().snapshot().entities()==before.entities(),
            "right drag during new symbol placement does not place the component");
    click(canvas,{1,1});
    const auto placed=window.document().snapshot();
    require(placed.revision()==before.revision()+1,"right dragging keeps new symbol placement armed");
    arm();
    require(!stationary_right(canvas) && window.document().revision()==placed.revision(),
            "stationary right click cancels new symbol placement without a ghost menu or history edit");
    click(canvas,{-1,-1});
    if (!canvas.wallPreview()) click(canvas,{-1,-1});
    require(window.document().revision()==placed.revision() && canvas.wallPreview(),
            "next click after cancellation starts the default wall instead of placing a ghost component");
    require(!stationary_right(canvas),"final pending wall cancellation has no menu");
}

void test_idle_selected_object_keeps_context_menu() {
    MainWindow window;
    auto& canvas=prepare(window);
    const auto id=window.createStraightWall({-1,0},{1,0});
    require(!id.isEmpty() && window.selectEntity(id),"idle contextual fixture selects a committed wall");
    const auto before=window.document().snapshot();
    require(stationary_right(canvas) && window.selectedEntityId()==id &&
            window.document().snapshot().entities()==before.entities(),
            "idle stationary right click retains the selected object's menu without cancelling selection");
}

void test_pen_up_travel_and_initial_wall_offset() {
    MainWindow window;
    auto& canvas=prepare(window);
    window.setMetricUnits(true);
    canvas.setViewTransform({2,1},80);
    const auto source=window.createStraightWall({0,0},{4,0});
    require(!source.isEmpty(),"travel fixture creates its structural baseline");
    require(window.selectEntity({}),"clear structural fixture selection");
    click(canvas,{-1,1});
    canvas.setFocus();
    mouse(canvas,QEvent::MouseMove,screen(canvas,{0,0}));
    key(canvas,Qt::Key_J);
    const auto type=[&](const QString& expression) {
        canvas.setFocus();
        for (const auto character : expression) {
            auto* focused=QApplication::focusWidget();
            require(focused,"travel input retains native focus");
            key(*focused,character.toUpper().unicode(),QString(character));
        }
    };
    type(QStringLiteral("1.25 m"));
    auto* input=window.findChild<QLineEdit*>(QStringLiteral("drawingLengthInput"));
    require(input,"travel fixture has compact exact entry");
    const auto revision=window.document().revision();
    key(*input,Qt::Key_Right);
    require(window.document().revision()==revision,"initial parallel offset does not commit a wall");
    if (!window.selectedEntityId().isEmpty())
        throw std::runtime_error((QStringLiteral("initial offset unexpectedly selected: ")+window.selectedEntityId()).toStdString());
    require(canvas.boundaryDraftPreview() && canvas.boundaryDraftPreview()->pen_position &&
            close(*canvas.boundaryDraftPreview()->pen_position,{1.25,0}),
            "initial offset retains its exact parked start after native focus events");
    mouse(canvas,QEvent::MouseMove,screen(canvas,{3,2}));
    require(!canvas.wallPreview(),
        "parked wall offset suppresses hover rubber band");
    const auto before=window.document().snapshot();
    type(QStringLiteral("2 m"));
    if (!window.selectedEntityId().isEmpty())
        throw std::runtime_error((QStringLiteral("resume typing unexpectedly selected: ")+window.selectedEntityId()).toStdString());
    key(*input,Qt::Key_Up);
    const auto after=window.document().snapshot();
    if (after.revision()==before.revision())
        throw std::runtime_error((QStringLiteral("perpendicular wall resume: ")+window.lastError()).toStdString());
    const auto wall=new_wall(before,after);
    require(close(wall.start,{1.25,0}) && close(wall.end,{1.25,2}),
        "perpendicular exact input creates the first wall from the offset start");
    const auto require_parked=[&](Vec2 expected,const char* message) {
        const auto preview=canvas.boundaryDraftPreview();
        if (!preview || !preview->pen_position || !close(*preview->pen_position,expected) || canvas.wallPreview())
            throw std::runtime_error(std::string(message)+"; selected="+window.selectedEntityId().toStdString()+
                "; error="+window.lastError().toStdString()+"; canvas_focus="+std::to_string(canvas.hasFocus())+
                "; parked_preview="+std::to_string(preview.has_value())+"; wall_preview="+
                std::to_string(canvas.wallPreview().has_value()));
    };
    key(canvas,Qt::Key_Return);
    require_parked({1.25,2},"Enter after exact wall commit retains its pen-up endpoint");
    const auto parked=window.document().snapshot();
    auto* empty_input=window.findChild<QLineEdit*>(QStringLiteral("drawingLengthInput"));
    require(empty_input && empty_input->text().isEmpty(),"parked navigation retains empty compact entry");
    mouse(canvas,QEvent::MouseMove,screen(canvas,{4,0}));
    empty_input->setFocus();
    require(empty_input->hasFocus(),"empty compact input owns the keyboard corner-jump event");
    key(*empty_input,Qt::Key_J);
    require_parked({4,0},"focused-input corner jump retains the chosen exact endpoint");
    mouse(canvas,QEvent::MouseMove,screen(canvas,{0,0}));
    canvas.setFocus();
    require(canvas.hasFocus(),"canvas owns the keyboard corner-jump event");
    key(canvas,Qt::Key_J);
    require_parked({0,0},"canvas corner jump retains the chosen exact endpoint");
    require(window.document().revision()==parked.revision(),"corner jump has no history edit");
    canvas.setFocus();
    require(canvas.hasFocus(),"canvas owns the keyboard endpoint-walk event");
    key(canvas,Qt::Key_Right);
    require_parked({4,0},"canvas arrow walks the connected endpoint without drawing");
    canvas.setFocus();
    key(canvas,Qt::Key_Left);
    require_parked({0,0},"canvas return walk retains its pen-up endpoint");
    empty_input->setFocus();
    require(empty_input->hasFocus(),"empty compact input owns the keyboard endpoint-walk event");
    key(*empty_input,Qt::Key_Right);
    require_parked({4,0},"focused-input arrow walks the connected endpoint without drawing");
    key(*empty_input,Qt::Key_Left);
    require_parked({0,0},"focused-input return walk retains its pen-up endpoint");
    require(window.document().revision()==parked.revision(),"repeated focused-input endpoint walks have no history edit");
    mouse(canvas,QEvent::MouseMove,screen(canvas,{3,3}));
    require(!canvas.wallPreview(),"pen-up endpoint walk suppresses hover rubber band");
    click(canvas,{0,2});
    if (window.document().revision()==parked.revision())
        throw std::runtime_error((QStringLiteral("walked wall click resume: ")+window.lastError()).toStdString());
    const auto resumed=new_wall(parked,window.document().snapshot());
    require(close(resumed.start,{0,0}) && close(resumed.end,{0,2}),
        "endpoint walk parks at the connected baseline end and deliberate click resumes");
    key(canvas,Qt::Key_Return);
    const auto retained=window.document().snapshot();
    require(window.selectEntity(source),"parked context fixture deliberately selects an existing wall");
    input->setText(QStringLiteral("1 m"));
    key(*input,Qt::Key_Up);
    require(window.document().revision()==retained.revision() &&
            window.document().snapshot().entities()==retained.entities() && !window.lastError().isEmpty(),
            "legitimate selection still rejects parked input without editing geometry");
}

void test_empty_keypad_enter_matches_keyboard() {
    DrawingInputPanel panel;
    panel.setAttribute(Qt::WA_DontShowOnScreen,true);
    panel.show();
    auto* toggle=panel.findChild<QToolButton*>(QStringLiteral("drawingKeypadToggle"));
    require(toggle,"compact drawing input exposes keypad");
    toggle->click();
    QPushButton* enter=nullptr;
    for (auto* button : panel.findChildren<QPushButton*>())
        if (button->text()==QStringLiteral("Enter")) enter=button;
    require(enter && enter->isVisible(),"expanded keypad exposes Enter");
    int empty_command=0, submitted=0;
    panel.setEmptyCommandRequested([&](int command) { empty_command=command; return true; });
    panel.setSubmitRequested([&](const QString& text,DrawingCardinalDirection direction) {
        require(text==QStringLiteral("2 m") && direction==DrawingCardinalDirection::right,
            "nonempty keypad Enter preserves exact direction submission");
        ++submitted; return true;
    });
    const auto press=[&] {
        const auto point=QRectF(enter->rect()).center();
        QMouseEvent down(QEvent::MouseButtonPress,point,enter->mapToGlobal(point.toPoint()),
            Qt::LeftButton,Qt::LeftButton,Qt::NoModifier);
        QMouseEvent up(QEvent::MouseButtonRelease,point,enter->mapToGlobal(point.toPoint()),
            Qt::LeftButton,Qt::NoButton,Qt::NoModifier);
        QApplication::sendEvent(enter,&down);
        QApplication::sendEvent(enter,&up);
        process_events();
    };
    press();
    require(empty_command==Qt::Key_Return && submitted==0,
        "empty keypad Enter dispatches pen-up rather than direction navigation");
    panel.beginText(QStringLiteral("2 m"));
    empty_command=0;
    press();
    require(empty_command==0 && submitted==1 && panel.inputText().isEmpty(),
        "nonempty keypad Enter submits and clears exact input once");
}

void test_focused_empty_enter_finishes_outline() {
    MainWindow window;
    auto& canvas=prepare(window);
    require(window.beginBoundaryDrawing(BoundaryAuthoringMode::draw_first,QStringLiteral("measurement")),
        "focused Enter fixture starts an outline");
    canvas.setSnapEnabled(false);
    const auto revision=window.document().revision();
    click(canvas,{0,0}); click(canvas,{2,0}); click(canvas,{2,2});
    auto* input=window.findChild<QLineEdit*>(QStringLiteral("drawingLengthInput"));
    require(input && input->text().isEmpty(),"outline retains empty exact input");
    input->setFocus();
    key(*input,Qt::Key_Return);
    require(!canvas.boundaryDraftPreview() && window.document().revision()==revision+1,
        "empty focused input Enter uses ordinary outline closure exactly once");
}

void test_measured_typed_travel_and_context_fence() {
    for (const bool metric : {false,true}) {
        MainWindow window;
        auto& canvas=prepare(window);
        window.setMetricUnits(metric);
        auto* snap=window.findChild<QToolButton*>(QStringLiteral("snapTool"));
        require(snap,"typed travel fixture has the persistent Snap control");
        snap->setChecked(false);
        canvas.setSnapEnabled(false);
        canvas.setViewTransform({0,0},80);
        require(window.beginMeasurementLinework(),"typed travel starts native measured lines");
        click(canvas,{0,0});
        click(canvas,{1,0});
        const auto saved=window.document().snapshot();
        bool exact_source=false;
        for (const auto& [id,entity] : saved.entities()) if (entity.type=="measurement_linework") {
            const auto decoded=decode_measurement_linework_model(entity.properties.at("model"));
            require(decoded.supported(),"typed travel source retains a supported canonical model");
            const auto replay=replay_measurement_linework(*decoded.model);
            require(!exact_source && replay.edges.size()==1 && close(replay.edges.front().segment.start,{0,0}) &&
                close(replay.edges.front().segment.end,{1,0}),"typed travel starts from the intended unsnapped one-metre source edge");
            exact_source=true;
        }
        require(exact_source,"typed travel fixture persists its exact source edge");
        canvas.setFocus();
        key(canvas,Qt::Key_Return);
        mouse(canvas,QEvent::MouseMove,screen(canvas,{6,4}));
        canvas.setFocus();
        key(canvas,Qt::Key_J);
        const auto type=[&](const QString& expression) {
            canvas.setFocus();
            for (const auto character : expression) {
                auto* focused=QApplication::focusWidget();
                require(focused,"typed travel keeps native exact input focus");
                key(*focused,character.toUpper().unicode(),QString(character));
            }
        };
        type(metric ? QStringLiteral("1.234567 m") : QStringLiteral("1 1/8 in"));
        auto* input=window.findChild<QLineEdit*>(QStringLiteral("drawingLengthInput"));
        require(input,"typed travel has exact input");
        key(*input,Qt::Key_Right);
        const auto travel=metric ? 1.234567 : 1.125*.0254;
        require(window.document().revision()==saved.revision() &&
                    window.document().snapshot().entities()==saved.entities(),
                "typed travel preserves saved measured geometry and history");
        mouse(canvas,QEvent::MouseMove,screen(canvas,{-3,-2}));
        const auto parked=canvas.boundaryDraftPreview();
        if (!parked || !parked->pen_position || !close(*parked->pen_position,{1+travel,0}) || parked->rubber_band)
            throw std::runtime_error("fractional or metric exact landing remains parked during hover; metric="+
                std::to_string(metric)+"; expected_x="+std::to_string(1+travel)+"; actual_x="+
                (parked && parked->pen_position ? std::to_string(parked->pen_position->x) : "unavailable")+
                "; actual_y="+(parked && parked->pen_position ? std::to_string(parked->pen_position->y) : "unavailable")+
                "; selected="+window.selectedEntityId().toStdString()+"; error="+window.lastError().toStdString());
        type(QStringLiteral("2 m"));
        key(*input,Qt::Key_Up);
        const auto drawn=window.document().snapshot();
        if (drawn.revision()!=saved.revision()+1)
            throw std::runtime_error("typed drawing resumes with one measured edge; selected="+
                window.selectedEntityId().toStdString()+"; error="+window.lastError().toStdString()+
                "; metric="+std::to_string(metric));
        std::optional<Segment> second;
        for (const auto& [id,entity] : drawn.entities()) if (entity.type=="measurement_linework" &&
            !saved.entities().contains(id)) {
            const auto& model=entity.properties.at("model");
            const auto decoded=decode_measurement_linework_model(model);
            require(decoded.supported(),"resumed measured stroke retains its supported canonical model");
            const auto replay=replay_measurement_linework(*decoded.model);
            require(replay.edges.size()==1,"travel creates no manufactured measured edge");
            second=replay.edges.front().segment;
        }
        require(second && close(second->start,{1+travel,0}) && close(second->end,{1+travel,2}),
                "resumed measured edge uses its exact relocated start");
        key(canvas,Qt::Key_Return);
        window.setMetricUnits(!metric);
        const auto changed=window.document().snapshot();
        click(canvas,{2,3});
        require(window.document().snapshot().entities()==changed.entities() &&
                    window.document().revision()==changed.revision(),
                "changed drawing context rejects a parked destination click without geometry");
        require(!window.lastError().isEmpty(),"context rejection explains why the parked input was not applied");
    }
}
void test_parked_drawing_rejects_replaced_head() {
    // A refreshed scene must not lend new authority to a retained pen anchor.
    for (const bool measured : {false,true}) for (const bool change_asset : {false,true})
        for (const auto operation : {0,1,2,3,4}) {
            MainWindow seed;
            const auto source_id=seed.createStraightWall({0,0},{4,0});
            require(!source_id.isEmpty(),"replacement fixture has a connected structural baseline");
            const auto seeded=seed.document().snapshot();
            std::vector<Entity> entities;
            for (const auto& [id,entity] : seeded.entities()) entities.push_back(entity);
            auto asset=Asset::create("parked-source","application/octet-stream",{std::byte{1}});
            auto document=std::make_shared<Document>(Document::create(std::move(entities),{asset}));
            MainWindow window(document);
            auto& canvas=prepare(window);
            window.setMetricUnits(true);
            auto* snap=window.findChild<QToolButton*>(QStringLiteral("snapTool"));
            require(snap,"replacement fixture has the persistent Snap control");
            snap->setChecked(false);
            canvas.setSnapEnabled(false);
            canvas.setViewTransform({2,1},80);
            require(window.selectEntity({}),"replacement fixture starts without a retained selection");
            if (measured) require(window.beginMeasurementLinework(),"replacement fixture starts measured drawing");
            // A click on the saved wall selects it in hybrid mode. J explicitly
            // parks the pen at its exact corner without drawing or picking it.
            mouse(canvas,QEvent::MouseMove,screen(canvas,{0,0}));
            canvas.setFocus();
            require(canvas.hasFocus(),"replacement fixture canvas owns the initial corner jump");
            key(canvas,Qt::Key_J);
            const auto parked=canvas.boundaryDraftPreview();
            if (!parked || !parked->pen_position || !close(*parked->pen_position,{0,0}) || parked->rubber_band)
                throw std::runtime_error("replacement fixture retains a pen-up anchor before replacing its source; measured="+
                    std::to_string(measured)+"; asset="+std::to_string(change_asset)+"; operation="+
                    std::to_string(operation)+"; selected="+window.selectedEntityId().toStdString()+"; error="+
                    window.lastError().toStdString()+"; parked_preview="+std::to_string(parked.has_value())+
                    "; wall_preview="+std::to_string(canvas.wallPreview().has_value()));
            const auto source=window.document().snapshot();
            auto altered=source;
            auto& record=const_cast<std::vector<RevisionRecord>&>(altered.history()).front();
            if (change_asset) record.assets.at(asset.id)=Asset::create(asset.id,asset.media_type,{std::byte{2}});
            else record.entities.at(source_id.toStdString()).extensions["replacement_fixture"]=true;
            window.document()=Document::fork(altered);
            const auto replaced=window.document().snapshot();
            require(replaced.document_id()==source.document_id() && replaced.revision()==source.revision() &&
                (change_asset ? replaced.assets()!=source.assets() : replaced.entities()!=source.entities()),
                "replacement preserves document identity and revision while changing retained authority");
            // Refresh projection without changing units, layer, tool or selection.
            window.setMetricUnits(true); process_events();
            auto* input=window.findChild<QLineEdit*>(QStringLiteral("drawingLengthInput"));
            require(input,"replaced source still exposes the native parked input before rejection");
            if (operation==0) { input->setFocus(); input->setText(QStringLiteral("1 m")); key(*input,Qt::Key_Right); }
            else if (operation==1) click(canvas,{0,2});
            else if (operation==2) {
                mouse(canvas,QEvent::MouseMove,screen(canvas,{4,0}));
                canvas.setFocus();
                require(canvas.hasFocus(),"replaced-source canvas owns its corner-jump event");
                key(canvas,Qt::Key_J);
            } else {
                input->setFocus();
                require(input->hasFocus(),"replaced-source compact input owns its empty command event");
                key(*input,operation==3 ? Qt::Key_Right : Qt::Key_Return);
            }
            const auto rejected=window.document().snapshot();
            require(rejected.revision()==replaced.revision() && rejected.entities()==replaced.entities() &&
                rejected.assets()==replaced.assets() && document_snapshot_digest(rejected)==document_snapshot_digest(replaced),
                "stale parked input preserves every entity, asset and history record");
            require(window.lastError().contains(QStringLiteral("parked drawing")) && !canvas.boundaryDraftPreview(),
                "stale parked input explains the changed source and retires the old anchor");
            click(canvas,{6,6});
            require(window.document().revision()==replaced.revision(),"fresh restart creates no connector from a stale anchor");
            click(canvas,{6,8});
            const auto restarted=window.document().snapshot();
            if (!measured) {
                const auto segment=new_wall(replaced,restarted);
                require(close(segment.start,{6,6}) && close(segment.end,{6,8}),"fresh wall restarts from the deliberate new anchor");
            } else {
                std::optional<Segment> segment;
                for (const auto& [id,entity] : restarted.entities()) if (entity.type=="measurement_linework" && !replaced.entities().contains(id)) {
                    const auto decoded=decode_measurement_linework_model(entity.properties.at("model"));
                    require(decoded.supported(),"fresh measured restart retains its canonical model");
                    const auto replay=replay_measurement_linework(*decoded.model);
                    require(replay.edges.size()==1 && !segment,"fresh measured restart persists one independent edge");
                    segment=replay.edges.front().segment;
                }
                require(segment && close(segment->start,{6,6}) && close(segment->end,{6,8}),"fresh measured restart uses the deliberate new anchor");
            }
        }
}

void test_parked_drawing_history_workspace_and_reopen_fences() {
    for (const bool measured : {false,true}) {
        MainWindow window;
        auto& canvas=prepare(window);
        window.setMetricUnits(true);
        auto* snap=window.findChild<QToolButton*>(QStringLiteral("snapTool"));
        require(snap,"history fixture has the persistent Snap control");
        snap->setChecked(false); canvas.setSnapEnabled(false);
        if (measured) require(window.beginMeasurementLinework(),"history fixture starts measured drawing");
        click(canvas,{0,0}); click(canvas,{2,0});
        const auto saved=window.document().snapshot();
        canvas.setFocus(); key(canvas,Qt::Key_Return);
        window.setWorkspace(Workspace::architectural);
        require(window.workspace()==Workspace::measurement && window.document().snapshot().entities()==saved.entities() &&
            !window.lastError().isEmpty(),"workspace transition refuses a live parked pen without editing committed geometry");
        require(window.undoCommand(),"parked undo retires its pen and undoes the committed edge");
        require(!canvas.boundaryDraftPreview() && !canvas.wallPreview(),"undo removes the retired parked preview");
        require(window.redoCommand() && window.document().snapshot().entities()==saved.entities(),"redo restores the committed geometry exactly");
        click(canvas,{6,6});
        require(window.document().snapshot().entities()==saved.entities(),"redo does not restore a stale parked anchor");
        key(canvas,Qt::Key_Return);
        key(canvas,Qt::Key_Escape);
        window.setWorkspace(Workspace::architectural);
        require(window.workspace()==Workspace::architectural,"cancelled parked pen permits the workspace transition");
        window.setWorkspace(Workspace::measurement);
        click(canvas,{7,7});
        require(window.document().snapshot().entities()==saved.entities(),"workspace return begins a new anchor without a stale connector");
        key(canvas,Qt::Key_Return);
        QTemporaryDir directory;
        const auto path=directory.filePath(QStringLiteral("parked.bldproj"));
        require(window.saveProjectAs(path) && window.openProject(path),"parked project saves and reopens through the native transition");
        require(!canvas.boundaryDraftPreview() && !canvas.wallPreview(),"reopen retires every local parked preview");
        const auto reopened=window.document().snapshot();
        click(canvas,{8,8});
        require(window.document().snapshot().entities()==reopened.entities() && window.document().revision()==reopened.revision(),
            "reopened drawing creates a fresh anchor without reconnecting the former parked pen");
    }
}
} // namespace

int main(int argc,char** argv) {
    sketch::testing::noninteractive_errors();
    QStandardPaths::setTestModeEnabled(true);
    QApplication application(argc,argv);
    QCoreApplication::setApplicationName(QStringLiteral("Vertex-drawing-measurement-test-")+
        QUuid::createUuid().toString(QUuid::WithoutBraces));
    const auto font=QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/Inter.ttf"));
    if (font>=0) QApplication::setFont(QFont(QFontDatabase::applicationFontFamilies(font).front(),10));
    try {
        if (QCoreApplication::arguments().contains(QStringLiteral("--travel-only"))) {
            test_empty_keypad_enter_matches_keyboard();
            test_focused_empty_enter_finishes_outline();
            test_pen_up_travel_and_initial_wall_offset();
            test_measured_typed_travel_and_context_fence();
            test_parked_drawing_rejects_replaced_head();
            test_parked_drawing_history_workspace_and_reopen_fences();
            std::cout<<"Drawing travel tests passed\n";
            return 0;
        }
        if (QCoreApplication::arguments().contains(QStringLiteral("--overview-navigation-only"))) {
            test_overview_navigation_preserves_native_authoring_drafts();
            std::cout<<"Overview authoring navigation tests passed\n";
            return 0;
        }
        test_clicked_wall_geometry_and_visible_lengths_at_zoom();
        test_arbitrary_drawing_lengths_are_readable_without_claiming_exactness();
        test_overview_navigation_preserves_native_authoring_drafts();
        test_typed_exact_and_endpoint_priority();
        test_measurement_draft_length_closure_and_right_cancel();
        test_mouse_wall_and_measurement_closure_retain_exact_off_grid_anchor();
        test_new_symbol_placement_right_cancel();
        test_idle_selected_object_keeps_context_menu();
        test_pen_up_travel_and_initial_wall_offset();
        test_empty_keypad_enter_matches_keyboard();
        test_focused_empty_enter_finishes_outline();
        test_measured_typed_travel_and_context_fence();
        test_parked_drawing_rejects_replaced_head();
        test_parked_drawing_history_workspace_and_reopen_fences();
    } catch (const std::exception& error) {
        std::cerr<<"drawing_measurement_desktop_tests: "<<error.what()<<'\n';
        return 1;
    }
    std::cout<<"Drawing measurement desktop tests passed\n";
    return 0;
}
