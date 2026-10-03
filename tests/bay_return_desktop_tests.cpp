#include "sketch/desktop/main_window.hpp"
#include "sketch/boundary_dimension.hpp"
#include "sketch/boundary_entity.hpp"
#include "sketch/wall_measurement.hpp"
#include "../src/desktop/plan_canvas.hpp"
#include "support/noninteractive_errors.hpp"

#include <QAction>
#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFont>
#include <QFontDatabase>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMouseEvent>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QToolButton>
#include <QUuid>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <vector>

namespace {
using namespace sketch;
using namespace sketch::desktop;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void events() { QCoreApplication::processEvents(QEventLoop::AllEvents, 50); }
bool same(Vec2 a, Vec2 b) { return std::hypot(a.x-b.x, a.y-b.y) < 1e-10; }
bool same_segments(const Boundary& a, const Boundary& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i=0; i<a.size(); ++i)
        if (!same(a[i].start,b[i].start) || !same(a[i].end,b[i].end) ||
            a[i].sweep_radians != b[i].sweep_radians) return false;
    return true;
}
PlanCanvas& prepare(MainWindow& window) {
    // Literal metre endpoints must use the matching metric measurement magnets.
    window.setMetricUnits(true);
    window.setAttribute(Qt::WA_DontShowOnScreen, true);
    window.resize(1400,900);
    window.show();
    QApplication::setActiveWindow(&window);
    events();
    auto* snap=window.findChild<QToolButton*>("snapTool");
    require(snap,"bay fixture uses the actual persistent Snap control");
    snap->setChecked(false);
    auto* canvas=dynamic_cast<PlanCanvas*>(window.findChild<QWidget*>("measurementPlanCanvas"));
    require(canvas,"bay return fixture exposes the actual native measurement canvas");
    canvas->setOverviewMapEnabled(false);
    canvas->setSnapEnabled(false);
    canvas->setViewTransform({3,-1},65);
    canvas->setFocus();
    return *canvas;
}
QPointF screen(const PlanCanvas& canvas, Vec2 point) {
    return QRectF(canvas.rect()).center()+QPointF(
        (point.x-canvas.viewCenter().x)*canvas.viewScale(),
        -(point.y-canvas.viewCenter().y)*canvas.viewScale());
}
void mouse(PlanCanvas& canvas, QEvent::Type type, QPointF point,
           Qt::MouseButton button=Qt::NoButton, Qt::MouseButtons buttons=Qt::NoButton) {
    QMouseEvent event(type,point,canvas.mapToGlobal(point.toPoint()),button,buttons,Qt::NoModifier);
    QApplication::sendEvent(&canvas,&event);
    events();
}
void click(PlanCanvas& canvas, Vec2 point) {
    const auto position=screen(canvas,point);
    mouse(canvas,QEvent::MouseMove,position);
    mouse(canvas,QEvent::MouseButtonPress,position,Qt::LeftButton,Qt::LeftButton);
    mouse(canvas,QEvent::MouseButtonRelease,position,Qt::LeftButton);
}
void key(QWidget& widget, int code, Qt::KeyboardModifiers modifiers=Qt::NoModifier,
         const QString& text={}, bool autorepeat=false) {
    QKeyEvent event(QEvent::KeyPress,code,modifiers,text,autorepeat);
    QApplication::sendEvent(&widget,&event);
    events();
}
void cancel_pending(PlanCanvas& canvas) {
    const auto position=QRectF(canvas.rect()).center();
    mouse(canvas,QEvent::MouseButtonPress,position,Qt::RightButton,Qt::RightButton);
    mouse(canvas,QEvent::MouseButtonRelease,position,Qt::RightButton);
}
QAction& return_action(MainWindow& window) {
    auto* action=window.findChild<QAction*>("completeBayWindowReturnAction");
    require(action,"native drawing menu exposes Complete bay-window return");
    require(action->shortcut().isEmpty(),"B belongs to the canvas rather than a global typing shortcut");
    return *action;
}
void invoke_return(MainWindow& window, PlanCanvas& canvas, bool action) {
    if (action) {
        require(return_action(window).isEnabled(),"valid open bay exposes the menu action");
        return_action(window).trigger();
        events();
    } else key(canvas,Qt::Key_B,Qt::NoModifier,QStringLiteral("b"));
    require(window.lastError().isEmpty(),"native bay-return invocation succeeds without a validation error");
}
void capture(PlanCanvas& canvas, const QString& name) {
    const auto directory=qEnvironmentVariable("VERTEX_TEST_CAPTURE_DIR");
    if (directory.isEmpty()) return;
    events();
    require(QDir().mkpath(directory) && canvas.grab().save(QDir(directory).filePath(name)),
            "actual post-B canvas screenshot saves to the requested capture directory");
}
Segment baseline(const Entity& entity) {
    const auto& value=entity.properties.at("baseline");
    return {{value.at("start").at(0).get<double>(),value.at("start").at(1).get<double>()},
            {value.at("end").at(0).get<double>(),value.at("end").at(1).get<double>()},
            value.value("sweep_radians",0.0)};
}
std::vector<std::string> ids(const DocumentSnapshot& snapshot, const char* type) {
    std::vector<std::string> result;
    for (const auto& [id,entity] : snapshot.entities()) if (entity.type==type) result.push_back(id);
    return result;
}
Entity added_wall(const DocumentSnapshot& before, const DocumentSnapshot& after) {
    std::optional<Entity> wall;
    for (const auto& id : ids(after,"wall")) if (!before.entities().contains(id)) {
        require(!wall,"bay return commits exactly one physical wall");
        wall=after.entities().at(id);
    }
    require(wall.has_value(),"bay return creates a committed physical wall");
    return *wall;
}
void unchanged(const DocumentSnapshot& before, const DocumentSnapshot& after) {
    require(before.entities()==after.entities() && before.revision()==after.revision() &&
                before.history().size()==after.history().size(),
            "rejected bay return preserves entities, revision and history");
}
void reopened_equals(MainWindow& window, const DocumentSnapshot& expected,
                     QTemporaryDir& directory, const QString& name) {
    const auto path=directory.filePath(name+QStringLiteral(".bldproj"));
    require(window.saveProjectAs(path),"bay-return result saves through the actual project API");
    MainWindow reopened({},nullptr,directory.filePath(QStringLiteral("missing-text-library.json")));
    require(reopened.openProject(path),"bay-return project reopens through the actual project API");
    require(reopened.document().snapshot().entities()==expected.entities(),
            "native reopen retains exact committed geometry and dimensions");
}

// Literal endpoints are deliberately independent of the reflection implementation.
const std::vector<Vec2> ordinary{{0,0},{2,0},{3,1},{5,1}};
const std::vector<Vec2> rotated{{0,0},{0,2},{-1,3},{-1,5}};

void enable_exterior_appraisal(MainWindow& window) {
    auto property=window.document().snapshot().entities().at("property-1");
    property.properties["calculation_workflow"]="appraisal";
    property.properties["appraisal_policy"]={{"policy_kind","residential_declared"},{"version",1},
        {"property_kind","detached_single_family"},{"measurement_basis","exterior"}};
    window.document().apply(ApplyEntityChanges{window.document().revision(),
        {EntityChange::upsert(property)}, {}, "Declare exterior appraisal fixture"});
}

void physical_wall_return(bool action, bool rotate, bool finish) {
    QTemporaryDir directory;
    require(directory.isValid(),"wall return fixture has temporary storage");
    MainWindow window({},nullptr,directory.filePath(QStringLiteral("text-library.json")));
    auto& canvas=prepare(window);
    if (rotate) canvas.setViewTransform({0,3},65);
    if (finish) enable_exterior_appraisal(window);
    // No createWall action: these empty clicks exercise the default physical Wall mode.
    const auto& points=rotate ? rotated : ordinary;
    for (const auto point : points) click(canvas,point);
    const auto before=window.document().snapshot();
    require(ids(before,"wall").size()==3 && canvas.wallPreview(),
            "default empty canvas clicks leave three committed outline walls and an open chain");
    require(ids(before,"measurement_boundary").empty(),"an unfinished wall outline has no appraisal area");
    const Vec2 expected=rotate ? Vec2{0,6} : Vec2{6,0};
    invoke_return(window,canvas,action);
    const auto after=window.document().snapshot();
    const auto wall=added_wall(before,after);
    const auto edge=baseline(wall);
    if (!same(edge.start,points.back()) || !same(edge.end,expected))
        std::cerr << "actual return (" << edge.start.x << ',' << edge.start.y << ")->("
                  << edge.end.x << ',' << edge.end.y << "), expected ("
                  << points.back().x << ',' << points.back().y << ")->("
                  << expected.x << ',' << expected.y << ")\n";
    require(same(edge.start,points.back()) && same(edge.end,expected) && edge.sweep_radians==0 &&
                after.revision()==before.revision()+1,
            "B/menu appends the literal matched diagonal return in one revision");
    require(std::abs(wall.properties.at("thickness_m").get<double>()-0.14)<1e-12 &&
                std::abs(wall.properties.at("height_m").get<double>()-2.4384)<1e-12,
            "bay return retains default physical thickness and wall height");
    for (const auto& id : ids(after,"wall"))
        require(std::any_of(canvas.labels().begin(),canvas.labels().end(),[&](const auto& label) {
            return label.id==QString::fromStdString(id) && label.plan_only && !label.text.isEmpty() &&
                label.automatic_linear_placement.has_value();
        }),"every committed outline wall retains its actual dimensional canvas label");
    require(canvas.wallPreview() && same(canvas.wallPreview()->start,expected) &&
                ids(after,"measurement_boundary").empty(),
            "a bay return keeps the wall chain open without prematurely creating the whole appraisal area");
    capture(canvas,rotate ? QStringLiteral("bay-return-wall-rotated.png") : QStringLiteral("bay-return-wall-B.png"));
    require(window.undoCommand(),"active wall return supports normal Undo");
    require(window.document().snapshot().entities()==before.entities() && canvas.wallPreview() &&
                same(canvas.wallPreview()->start,points.back()),
            "Undo removes only the return and resumes at the front endpoint");
    require(window.redoCommand() && window.document().snapshot().entities()==after.entities() &&
                canvas.wallPreview() && same(canvas.wallPreview()->start,expected),
            "Redo restores the exact return and its continuation endpoint");
    if (finish) {
        for (const auto point : std::vector<Vec2>{{8,0},{8,-4},{0,-4}}) click(canvas,point);
        const auto before_close=window.document().snapshot();
        click(canvas,{0,0});
        const auto closed=window.document().snapshot();
        require(ids(closed,"wall").size()==8 && !canvas.wallPreview() &&
                    closed.revision()==before_close.revision()+1,
                "continuing the outline to its real anchor uses normal atomic wall closure");
        const auto measured=ids(closed,"measurement_boundary");
        require(measured.size()==1 && wall_measurement_source_current(closed,closed.entities().at(measured.front())) &&
                    exterior_wall_measurement_source_ids(closed.entities().at(measured.front()))==ids(closed,"wall"),
                "true closure creates the ordinary current appraisal measurement bound to every physical wall");
        const auto owner=decode_identified_boundary_entity(closed.entities().at(measured.front()));
        std::size_t dimensions=0;
        for (const auto& [id,entity] : closed.entities()) {
            (void)id;
            if (!can_recognize_boundary_dimension_entity_type(entity.type)) continue;
            const auto decoded=decode_boundary_dimension_entity(entity);
            if (decoded.dimension && decoded.dimension->boundary_id==owner.id) ++dimensions;
        }
        require(dimensions==owner.segments.size(),"normal appraisal closure retains every exterior edge dimension");
        reopened_equals(window,closed,directory,QStringLiteral("wall-bay-closed"));
    } else {
        cancel_pending(canvas);
        require(!canvas.wallPreview() && window.document().snapshot().entities()==after.entities(),
                "right cancel ends only the unfinished next wall and retains the committed bay return");
        reopened_equals(window,after,directory,QStringLiteral("wall-bay-open"));
    }
}

void physical_return_to_original_anchor() {
    QTemporaryDir directory;
    require(directory.isValid(),"true physical return closure has temporary storage");
    MainWindow window({},nullptr,directory.filePath(QStringLiteral("text-library.json")));
    auto& canvas=prepare(window);
    enable_exterior_appraisal(window);
    for (const auto point : std::vector<Vec2>{{6,0},{2,0},{3,1},{5,1}}) click(canvas,point);
    const auto before=window.document().snapshot();
    require(ids(before,"wall").size()==3 && canvas.wallPreview(),"true bay closure begins with three open walls");
    invoke_return(window,canvas,false);
    const auto closed=window.document().snapshot();
    const auto return_wall=added_wall(before,closed);
    require(same(baseline(return_wall).end,{6,0}) && !canvas.wallPreview() &&
                closed.revision()==before.revision()+1,"B itself closes the physical outline in one revision");
    const auto measured=ids(closed,"measurement_boundary");
    require(measured.size()==1 && wall_measurement_source_current(closed,closed.entities().at(measured.front())) &&
                exterior_wall_measurement_source_ids(closed.entities().at(measured.front()))==ids(closed,"wall"),
            "B closure atomically creates the current exterior measurement bound to all walls");
    require(window.undoCommand() && window.document().snapshot().entities()==before.entities(),
            "one Undo removes the B return and exterior area together, preserving the earlier walls");
    require(window.redoCommand() && window.document().snapshot().entities()==closed.entities(),
            "Redo restores the B return and current exterior area together");
    reopened_equals(window,closed,directory,QStringLiteral("physical-return-true-closure"));
}

void measurement_return(bool action, bool rotate) {
    QTemporaryDir directory;
    require(directory.isValid(),"measurement bay fixture has temporary storage");
    MainWindow window({},nullptr,directory.filePath(QStringLiteral("text-library.json")));
    auto& canvas=prepare(window);
    if (rotate) canvas.setViewTransform({0,3},65);
    require(window.beginBoundaryDrawing(BoundaryAuthoringMode::draw_first,QStringLiteral("measurement")),
            "actual Draw First measurement session starts");
    const auto source=window.document().snapshot();
    const auto& points=rotate ? rotated : ordinary;
    for (const auto point : points) click(canvas,point);
    require(canvas.boundaryDraftPreview() && canvas.boundaryDraftPreview()->segments.size()==3,
            "mouse input creates the pre-bay outline and the latest entering/front pair");
    const auto before=*canvas.boundaryDraftPreview();
    const Vec2 expected=rotate ? Vec2{0,6} : Vec2{6,0};
    invoke_return(window,canvas,action);
    require(canvas.boundaryDraftPreview().has_value(),"B/menu leaves the measurement draft open");
    const auto after=*canvas.boundaryDraftPreview();
    require(after.segments.size()==4 && same(after.segments.back().start,points.back()) &&
                same(after.segments.back().end,expected) && after.labels.size()==4 &&
                after.pen_position && same(*after.pen_position,expected),
            "return appends the literal matched edge and its ordinary automatic draft dimension");
    unchanged(source,window.document().snapshot());
    capture(canvas,rotate ? QStringLiteral("bay-return-measurement-B-rotated.png") : QStringLiteral("bay-return-measurement-menu.png"));
    require(window.undoCommand() && canvas.boundaryDraftPreview() &&
                same_segments(canvas.boundaryDraftPreview()->segments,before.segments) &&
                canvas.boundaryDraftPreview()->labels.size()==before.labels.size(),
            "draft Undo removes only the return and its automatic dimension");
    require(window.redoCommand() && canvas.boundaryDraftPreview() &&
                same_segments(canvas.boundaryDraftPreview()->segments,after.segments) &&
                canvas.boundaryDraftPreview()->labels.size()==after.labels.size(),
            "draft Redo restores the exact return and its dimension");
    const std::vector<Vec2> continuation=rotate ? std::vector<Vec2>{{0,8},{4,8},{4,0}} :
        std::vector<Vec2>{{8,0},{8,-4},{0,-4}};
    for (const auto point : continuation) click(canvas,point);
    require(canvas.boundaryDraftPreview() && canvas.boundaryDraftPreview()->segments.size()==7,
            "measurement drafting accepts more outline edges after the bay return");
    key(canvas,Qt::Key_Return);
    const auto committed=window.document().snapshot();
    const auto boundaries=ids(committed,"measurement_boundary");
    require(boundaries.size()==1 && !canvas.boundaryDraftPreview() && committed.revision()==source.revision()+1,
            "normal Finish closes and commits the whole measured outline once");
    const auto boundary=decode_identified_boundary_entity(committed.entities().at(boundaries.front()));
    require(boundary.segments.size()==8 && same(boundary.segments[3].segment.start,points.back()) &&
                same(boundary.segments[3].segment.end,expected),
            "completed measurement retains the return within the full outline");
    reopened_equals(window,committed,directory,rotate ? QStringLiteral("measurement-bay-rotated") : QStringLiteral("measurement-bay"));
}

void define_first_manual_dimension() {
    MainWindow window;
    auto& canvas=prepare(window);
    require(window.beginBoundaryDrawing(BoundaryAuthoringMode::define_first,QStringLiteral("measurement")),
            "actual Define First measurement starts with manual dimensions");
    click(canvas,{0,0});
    click(canvas,{1,1});
    click(canvas,{0,1}); // Place the first edge's manual dimension.
    click(canvas,{3,1});
    require(canvas.boundaryDraftPreview() && canvas.boundaryDraftPreview()->segments.size()==2 &&
                canvas.boundaryDraftPreview()->labels.size()==2 &&
                canvas.boundaryDraftPreview()->instruction.contains(QStringLiteral("place this edge")),
            "Define First front edge waits for its manual dimension");
    const auto document=window.document().snapshot();
    const auto pending=*canvas.boundaryDraftPreview();
    require(!window.completeBayWindowReturn() && window.lastError().contains("dimension",Qt::CaseInsensitive) &&
                canvas.boundaryDraftPreview() && same_segments(canvas.boundaryDraftPreview()->segments,pending.segments),
            "bay completion rejects an outstanding manual dimension without consuming it");
    unchanged(document,window.document().snapshot());
    click(canvas,{2,1.5}); // Complete the front's normal manual-placement step.
    invoke_return(window,canvas,false);
    require(canvas.boundaryDraftPreview() && canvas.boundaryDraftPreview()->segments.size()==3 &&
                same(canvas.boundaryDraftPreview()->segments.back().end,{4,0}) &&
                canvas.boundaryDraftPreview()->labels.size()==3 &&
                canvas.boundaryDraftPreview()->instruction.contains(QStringLiteral("place this edge")),
            "Define First return creates one edge while retaining its pending manual-dimension workflow");
    require(!window.completeBayWindowReturn() && window.lastError().contains("dimension",Qt::CaseInsensitive),
            "a second B cannot bypass the new return's manual dimension");
    require(window.undoCommand() && canvas.boundaryDraftPreview()->segments.size()==2 &&
                window.redoCommand() && canvas.boundaryDraftPreview()->segments.size()==3,
            "Define First pending return supports draft Undo and Redo");
    click(canvas,{3.5,0.8});
    require(canvas.boundaryDraftPreview()->segments.size()==3 && canvas.boundaryDraftPreview()->labels.size()==3,
            "manual placement dimensions the return without adding or closing geometry");
    unchanged(document,window.document().snapshot());
    key(canvas,Qt::Key_Escape);
}

void measurement_return_to_original_anchor() {
    QTemporaryDir directory;
    require(directory.isValid(),"true measurement return closure has temporary storage");
    MainWindow window({},nullptr,directory.filePath(QStringLiteral("text-library.json")));
    auto& canvas=prepare(window);
    require(window.beginBoundaryDrawing(BoundaryAuthoringMode::draw_first,QStringLiteral("measurement")),
            "true return closure starts an actual classified measurement draft");
    const auto original=window.document().snapshot();
    for (const auto point : std::vector<Vec2>{{6,0},{2,0},{3,1},{5,1}}) click(canvas,point);
    require(canvas.boundaryDraftPreview() && canvas.boundaryDraftPreview()->segments.size()==3,
            "original outline anchor is the literal matched return endpoint");
    invoke_return(window,canvas,false);
    const auto closed=window.document().snapshot();
    const auto boundaries=ids(closed,"measurement_boundary");
    require(!canvas.boundaryDraftPreview() && boundaries.size()==1 && closed.revision()==original.revision()+1,
            "B reaching the actual outline anchor uses normal measurement closure and one document revision");
    const auto& entity=closed.entities().at(boundaries.front());
    const auto owner=decode_identified_boundary_entity(entity);
    require(owner.segments.size()==4 && same(owner.segments.front().segment.start,{6,0}) &&
                same(owner.segments.back().segment.start,{5,1}) &&
                owner.segments.back().segment.end.x==owner.segments.front().segment.start.x &&
                owner.segments.back().segment.end.y==owner.segments.front().segment.start.y,
            "true B closure retains the exact shared original anchor and no extra local bay chord");
    std::size_t dimensions=0;
    for (const auto& [id,dimension_entity] : closed.entities()) {
        (void)id;
        if (!can_recognize_boundary_dimension_entity_type(dimension_entity.type)) continue;
        const auto decoded=decode_boundary_dimension_entity(dimension_entity);
        if (decoded.dimension && decoded.dimension->boundary_id==owner.id) {
            require(decoded.dimension->placement==BoundaryDimensionPlacement::automatic,
                    "true B closure keeps normal automatic Draw First dimensions");
            ++dimensions;
        }
    }
    require(dimensions==4,"true B closure commits all four ordinary edge dimensions");
    require(window.undoCommand() && window.document().snapshot().entities()==original.entities() &&
                window.redoCommand() && window.document().snapshot().entities()==closed.entities(),
            "closed measurement Undo/Redo retains the complete return, boundary and dimensions");
    reopened_equals(window,closed,directory,QStringLiteral("measurement-return-true-closure"));
}

void negative_contexts_and_key_routing() {
    MainWindow window;
    auto& canvas=prepare(window);
    const auto idle=window.document().snapshot();
    require(!window.completeBayWindowReturn() && !window.lastError().isEmpty(),"no drawing chain rejects the helper with an error");
    unchanged(idle,window.document().snapshot());
    click(canvas,{0,0});
    click(canvas,{1,1});
    const auto one=window.document().snapshot();
    require(!window.completeBayWindowReturn() && !window.lastError().isEmpty(),"one committed edge is insufficient for a bay return");
    unchanged(one,window.document().snapshot());
    click(canvas,{3,1});
    const auto valid=window.document().snapshot();
    const auto pan_point=QRectF(canvas.rect()).center();
    mouse(canvas,QEvent::MouseButtonPress,pan_point,Qt::MiddleButton,Qt::MiddleButton);
    key(canvas,Qt::Key_B,Qt::NoModifier,QStringLiteral("b"));
    unchanged(valid,window.document().snapshot());
    require(!window.completeBayWindowReturn() && window.lastError().contains("gesture",Qt::CaseInsensitive),
            "an active canvas pan rejects the bay command before mutation");
    unchanged(valid,window.document().snapshot());
    mouse(canvas,QEvent::MouseButtonRelease,pan_point,Qt::MiddleButton);
    for (const auto modifiers : {Qt::ControlModifier,Qt::AltModifier,Qt::MetaModifier}) {
        key(canvas,Qt::Key_B,modifiers,QStringLiteral("b"));
        unchanged(valid,window.document().snapshot());
    }
    key(canvas,Qt::Key_B,Qt::NoModifier,QStringLiteral("b"),true);
    unchanged(valid,window.document().snapshot());
    auto* height=window.findChild<QLineEdit*>("wallDrawHeight");
    require(height,"physical wall has its real editable height control");
    height->setFocus();
    key(*height,Qt::Key_B,Qt::NoModifier,QStringLiteral("b"));
    unchanged(valid,window.document().snapshot());
    require(height->text().contains(QLatin1Char('b')),"typing b edits the focused field without dispatching a bay return");
    height->setText(QStringLiteral("8 ft"));
    canvas.setFocus();
    height->setText(QStringLiteral("invalid height"));
    require(!window.completeBayWindowReturn() && window.lastError().contains("dimension",Qt::CaseInsensitive),
            "unrepresentable wall dimensions reject the return before committing a wall");
    unchanged(valid,window.document().snapshot());
    require(canvas.wallPreview() && same(canvas.wallPreview()->start,{3,1}),
            "failed physical dimensions retain the pending endpoint");
    height->setText(QStringLiteral("8 ft"));
    auto* input=canvas.findChild<QLineEdit*>("drawingLengthInput");
    key(canvas,Qt::Key_1,Qt::NoModifier,QStringLiteral("1"));
    input=canvas.findChild<QLineEdit*>("drawingLengthInput");
    require(input && input->hasFocus(),"typed drawing length uses the actual inline input widget");
    key(*input,Qt::Key_B,Qt::NoModifier,QStringLiteral("b"));
    unchanged(valid,window.document().snapshot());
    require(input->text().contains(QLatin1Char('b')),"b inside drawing-length entry remains typed text");
    key(*input,Qt::Key_Escape);
    window.document().mark_read_only("Bay return fixture read-only transition");
    const auto read_only=window.document().snapshot();
    require(!window.completeBayWindowReturn() && window.lastError().contains("read-only",Qt::CaseInsensitive),
            "read-only transition rejects bay completion with its relevant error");
    unchanged(read_only,window.document().snapshot());
}

void altered_chain_rejected(bool curved) {
    MainWindow window;
    auto& canvas=prepare(window);
    click(canvas,{0,0}); click(canvas,{1,1}); click(canvas,{3,1});
    const auto source=window.document().snapshot();
    auto wall=*std::find_if(source.entities().begin(),source.entities().end(),[&](const auto& pair) {
        return pair.second.type=="wall" && same(baseline(pair.second).end,{3,1});
    });
    auto replacement=wall.second;
    if (curved) replacement.properties["baseline"]["sweep_radians"]=0.5;
    else replacement.properties["baseline"]["start"][0]=1.25;
    // The document may reject this malformed replacement at admission because
    // the clicked wall has hard endpoint relations. If admitted, the bay
    // command must independently reject the now-stale drawing chain.
    try {
        window.document().apply(ApplyEntityChanges{source.revision(),{EntityChange::upsert(replacement)}, {},
            curved ? "Fixture external curved baseline" : "Fixture external disconnected baseline"});
    } catch (const DocumentError& error) {
        require(error.code()==DocumentErrorCode::constraint_violation,
                "malformed chain replacement fails the actual entity admission contract");
        unchanged(source,window.document().snapshot());
        require(canvas.wallPreview() && same(canvas.wallPreview()->start,{3,1}),
                "document admission rejects malformed chain edits without advancing drawing");
        return;
    }
    const auto changed=window.document().snapshot();
    require(!window.completeBayWindowReturn() && !window.lastError().isEmpty(),
            "curved or externally disconnected pending chain fails closed");
    unchanged(changed,window.document().snapshot());
    require(canvas.wallPreview() && same(canvas.wallPreview()->start,{3,1}),
            "failed validation does not advance the pending wall endpoint");
}

} // namespace

int main(int argc, char** argv) {
    sketch::testing::noninteractive_errors();
    QStandardPaths::setTestModeEnabled(true);
    QApplication app(argc,argv);
    QCoreApplication::setOrganizationName(QStringLiteral("Vertex-tests"));
    QCoreApplication::setApplicationName(QStringLiteral("Vertex-bay-return-test-")+
        QUuid::createUuid().toString(QUuid::WithoutBraces));
    try {
        require(QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/Inter.ttf"))>=0,
                "bundled font loads for actual bay-return rendering");
        app.setFont(QFont(QStringLiteral("Inter"),10));
        physical_wall_return(false,false,true);
        physical_wall_return(true,true,false);
        physical_return_to_original_anchor();
        measurement_return(true,false);
        measurement_return(false,true);
        measurement_return_to_original_anchor();
        define_first_manual_dimension();
        negative_contexts_and_key_routing();
        altered_chain_rejected(true);
        altered_chain_rejected(false);
        std::cout << "bay_return_desktop_tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "bay_return_desktop_tests: " << error.what() << '\n';
        return 1;
    }
}
