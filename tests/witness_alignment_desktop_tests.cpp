#include "sketch/desktop/main_window.hpp"
#include "sketch/boundary_dimension.hpp"
#include "sketch/boundary_entity.hpp"
#include "sketch/wall_measurement.hpp"
#include "sketch/project_store.hpp"
#include "sketch/project_workspace.hpp"
#include "../src/desktop/plan_canvas.hpp"
#include "support/noninteractive_errors.hpp"
#include <QAction>
#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFont>
#include <QFontDatabase>
#include <QImage>
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
void require(bool value,const char* message) { if (!value) throw std::runtime_error(message); }
void events() { QCoreApplication::processEvents(QEventLoop::AllEvents,50); }
bool same(Vec2 a,Vec2 b) { return std::hypot(a.x-b.x,a.y-b.y)<1e-10; }
bool same_segments(const Boundary& a,const Boundary& b) {
    if (a.size()!=b.size()) return false;
    for (std::size_t i=0;i<a.size();++i)
        if (!same(a[i].start,b[i].start) || !same(a[i].end,b[i].end) ||
            a[i].sweep_radians!=b[i].sweep_radians) return false;
    return true;
}
void unchanged(const DocumentSnapshot& a,const DocumentSnapshot& b) {
    if (a.entities()!=b.entities() || a.assets()!=b.assets() || a.revision()!=b.revision() ||
        a.history().size()!=b.history().size())
        std::cerr << "unchanged mismatch: revision " << a.revision() << " -> " << b.revision()
                  << ", history " << a.history().size() << " -> " << b.history().size() << '\n';
    require(a.entities()==b.entities() && a.assets()==b.assets() && a.revision()==b.revision() &&
            a.history().size()==b.history().size(),"proposal/rejection preserves entities, assets, revision and history");
}
PlanCanvas& prepare(MainWindow& window) {
    window.setMetricUnits(true);
    window.setAttribute(Qt::WA_DontShowOnScreen,true);
    window.resize(1400,900); window.show(); QApplication::setActiveWindow(&window); events();
    auto* snap=window.findChild<QToolButton*>("snapTool");
    require(snap,"literal metre clicks use the actual retained Snap control"); snap->setChecked(false);
    auto* canvas=dynamic_cast<PlanCanvas*>(window.findChild<QWidget*>("measurementPlanCanvas"));
    require(canvas,"fixture uses the actual native measurement canvas");
    canvas->setOverviewMapEnabled(false); canvas->setSnapEnabled(false);
    canvas->setViewTransform({2,1},65); canvas->setFocus(); events(); return *canvas;
}
QPointF screen(const PlanCanvas& canvas,Vec2 point) {
    return QRectF(canvas.rect()).center()+QPointF((point.x-canvas.viewCenter().x)*canvas.viewScale(),
        -(point.y-canvas.viewCenter().y)*canvas.viewScale());
}
void mouse(PlanCanvas& canvas,QEvent::Type type,QPointF point,
           Qt::MouseButton button=Qt::NoButton,Qt::MouseButtons buttons=Qt::NoButton) {
    QMouseEvent event(type,point,canvas.mapToGlobal(point.toPoint()),button,buttons,Qt::NoModifier);
    QApplication::sendEvent(&canvas,&event); events();
}
void click(PlanCanvas& canvas,Vec2 point) {
    const auto p=screen(canvas,point); mouse(canvas,QEvent::MouseMove,p);
    mouse(canvas,QEvent::MouseButtonPress,p,Qt::LeftButton,Qt::LeftButton);
    mouse(canvas,QEvent::MouseButtonRelease,p,Qt::LeftButton);
}
void key(QWidget& widget,int code,Qt::KeyboardModifiers modifiers=Qt::NoModifier,
         const QString& text={},bool repeat=false) {
    QKeyEvent event(QEvent::KeyPress,code,modifiers,text,repeat); QApplication::sendEvent(&widget,&event); events();
}
void capture(PlanCanvas& canvas,const QString& name) {
    const auto directory=qEnvironmentVariable("VERTEX_TEST_CAPTURE_DIR"); if (directory.isEmpty()) return;
    events(); require(QDir().mkpath(directory) && canvas.grab().save(QDir(directory).filePath(name)),
        "actual native witness screenshot saves to capture directory");
}
Segment baseline(const Entity& entity) {
    const auto& v=entity.properties.at("baseline");
    return {{v.at("start").at(0).get<double>(),v.at("start").at(1).get<double>()},
            {v.at("end").at(0).get<double>(),v.at("end").at(1).get<double>()},v.value("sweep_radians",0.0)};
}
std::vector<std::string> ids(const DocumentSnapshot& snapshot,const char* type) {
    std::vector<std::string> result;
    for (const auto& [id,entity]:snapshot.entities()) if (entity.type==type) result.push_back(id);
    return result;
}
Entity added_wall(const DocumentSnapshot& before,const DocumentSnapshot& after) {
    std::optional<Entity> wall;
    for (const auto& id:ids(after,"wall")) if (!before.entities().contains(id)) {
        require(!wall,"acceptance appends exactly one physical wall"); wall=after.entities().at(id);
    }
    require(wall.has_value(),"acceptance persists an actual physical wall"); return *wall;
}
bool selected(const PlanCanvas& canvas) {
    return std::any_of(canvas.drawingWitnesses().begin(),canvas.drawingWitnesses().end(),
        [](const auto& w) { return w.selected; });
}
void witness(const PlanCanvas& canvas,bool horizontal,Vec2 start,Vec2 end,bool selection) {
    const auto& values=canvas.drawingWitnesses();
    const auto it=std::find_if(values.begin(),values.end(),[&](const auto& w) { return w.horizontal==horizontal; });
    require(it!=values.end(),"nonzero axis exposes an actual witness choice");
    require(same(it->segment.start,start) && same(it->segment.end,end) && it->segment.sweep_radians==0 &&
            it->selected==selection,"witness retains literal current-to-original-anchor segment and selection");
    require(it->dimension_text==PlanCanvas::drawingLengthText(std::hypot(end.x-start.x,end.y-start.y),true),
            "native witness label honestly formats the exact geometric length");
}
QAction& action(MainWindow& window,bool horizontal) {
    auto* value=window.findChild<QAction*>(horizontal ? "drawingAlignStartX" : "drawingAlignStartY");
    require(value,"actual drawing menu exposes X/Y alignment action"); return *value;
}
void propose(MainWindow& window,PlanCanvas& canvas,bool horizontal,bool menu=false) {
    if (menu) { require(action(window,horizontal).isEnabled(),"valid chain enables alignment menu");
        action(window,horizontal).trigger(); events(); }
    else key(canvas,horizontal ? Qt::Key_X : Qt::Key_Y,Qt::NoModifier,horizontal ? "x" : "y");
    require(window.lastError().isEmpty() && selected(canvas),"native command selects an ephemeral proposal");
}
void reopened(MainWindow& window,const DocumentSnapshot& expected,QTemporaryDir& directory,const QString& name) {
    const auto path=directory.filePath(name+".bldproj"); require(window.saveProjectAs(path),"ordinary Save persists result");
    MainWindow other({},nullptr,directory.filePath("reopen-library.json")); other.setAttribute(Qt::WA_DontShowOnScreen,true);
    require(other.openProject(path) && other.document().snapshot().entities()==expected.entities(),
            "ordinary reopen retains exact geometry, dimensions and source ownership");
}
std::size_t dimensions(const DocumentSnapshot& snapshot,const std::string& owner,BoundaryDimensionPlacement placement) {
    std::size_t count=0;
    for (const auto& [id,entity]:snapshot.entities()) {
        (void)id; if (!can_recognize_boundary_dimension_entity_type(entity.type)) continue;
        const auto value=decode_boundary_dimension_entity(entity);
        if (value.dimension && value.dimension->boundary_id==owner) {
            require(value.dimension->placement==placement,"closure retains ordinary dimension placement mode"); ++count;
        }
    }
    return count;
}
void appraisal(MainWindow& window) {
    auto property=window.document().snapshot().entities().at("property-1");
    property.properties["calculation_workflow"]="appraisal";
    property.properties["appraisal_policy"]={{"policy_kind","residential_declared"},{"version",1},
        {"property_kind","detached_single_family"},{"measurement_basis","exterior"}};
    window.document().apply(ApplyEntityChanges{window.document().revision(),{EntityChange::upsert(property)}, {},
        "Declare exterior appraisal fixture"});
}
// Literal geometry is independent of the production projection and snap helpers.
const Vec2 anchor{0.01317,-0.01931}, shoulder{4.01317,-1.01931}, endpoint{3.24567,2.34567};
const Vec2 x_target{0.01317,2.34567}, y_target{3.24567,-0.01931};
void start(MainWindow& window,PlanCanvas& canvas,bool measurement) {
    if (measurement) require(window.beginBoundaryDrawing(BoundaryAuthoringMode::draw_first,"measurement"),
                             "actual classified Draw First session starts");
    // Default Wall uses empty clicks, without a createWall action.
    click(canvas,anchor); click(canvas,shoulder); click(canvas,endpoint);
    if (measurement) require(canvas.boundaryDraftPreview() && canvas.boundaryDraftPreview()->segments.size()==2 &&
        same(canvas.boundaryDraftPreview()->segments.back().end,endpoint),"native snap-off draft has literal geometry");
    else require(canvas.wallPreview() && same(canvas.wallPreview()->start,endpoint) &&
        ids(window.document().snapshot(),"wall").size()==2,"default Wall clicks create literal physical chain");
}

void alignment(bool measurement,bool horizontal) {
    QTemporaryDir directory; require(directory.isValid(),"alignment uses temporary project storage");
    MainWindow window({},nullptr,directory.filePath("library.json")); auto& canvas=prepare(window);
    start(window,canvas,measurement); const auto before=window.document().snapshot();
    const auto draft=canvas.boundaryDraftPreview(); const auto target=horizontal ? x_target : y_target;
    witness(canvas,true,endpoint,x_target,false); witness(canvas,false,endpoint,y_target,false);
    // Only enable retained Snap after literal input: a snapped off-grid click is not an exact coordinate fixture.
    window.findChild<QToolButton*>("snapTool")->setChecked(true); events();
    for (const auto theme:{WorkspaceTheme::light,WorkspaceTheme::dark}) {
        window.setWorkspaceTheme(theme); canvas.setFocus();
        mouse(canvas,QEvent::MouseMove,screen(canvas,endpoint));
        const auto passive=canvas.grab().toImage();
        propose(window,canvas,horizontal,!horizontal); witness(canvas,horizontal,endpoint,target,true);
        require(std::count_if(canvas.drawingWitnesses().begin(),canvas.drawingWitnesses().end(),
            [](const auto& w) { return w.selected; })==1,"only one witness is selected");
        unchanged(before,window.document().snapshot());
        if (measurement) require(canvas.boundaryDraftPreview() &&
            same_segments(canvas.boundaryDraftPreview()->segments,draft->segments) &&
            canvas.boundaryDraftPreview()->labels.size()==draft->labels.size() &&
            canvas.boundaryDraftPreview()->pen_position && same(*canvas.boundaryDraftPreview()->pen_position,endpoint),
            "proposal changes no accepted edge, dimension or draft pen");
        else require(canvas.wallPreview() && same(canvas.wallPreview()->start,endpoint),"proposal cannot advance wall pen");
        const auto painted=canvas.grab().toImage(); const auto p=screen(canvas,target)*painted.devicePixelRatio();
        const QRect marker(qRound(p.x())-10,qRound(p.y())-10,21,21);
        require(!painted.isNull() && passive.copy(marker)!=painted.copy(marker),
                "selected witness marker actually renders at literal projected endpoint in light and dark");
        capture(canvas,QStringLiteral("witness-%1-%2-%3.png").arg(measurement ? "measurement" : "wall",
            horizontal ? "X" : "Y",theme==WorkspaceTheme::light ? "light" : "dark"));
    }
    for (int i=0;i<3;++i) propose(window,canvas,horizontal,!horizontal);
    key(canvas,horizontal ? Qt::Key_X : Qt::Key_Y,Qt::NoModifier,{},true);
    key(canvas,Qt::Key_Return,Qt::ControlModifier);
    require(selected(canvas),"Ctrl+Enter cannot accept a selected proposal"); unchanged(before,window.document().snapshot());
    const auto pan=QRectF(canvas.rect()).center();
    mouse(canvas,QEvent::MouseButtonPress,pan,Qt::MiddleButton,Qt::MiddleButton);
    key(canvas,Qt::Key_Return); unchanged(before,window.document().snapshot());
    if (measurement) require(canvas.boundaryDraftPreview()->segments.size()==2,"Enter during pan cannot append draft edge");
    mouse(canvas,QEvent::MouseButtonRelease,pan,Qt::MiddleButton);
    // A pan may discard the proposal; propose again after the idle transition.
    propose(window,canvas,horizontal,!horizontal);
    const auto proposed=std::find_if(canvas.drawingWitnesses().begin(),canvas.drawingWitnesses().end(),
        [](const auto& value) { return value.selected; })->segment;
    require(same(proposed.end,target),"proposed endpoint agrees with independent literal projection");
    key(canvas,Qt::Key_Return);
    const auto accepted=window.document().snapshot(); require(!selected(canvas),"Enter consumes proposal once");
    if (measurement) {
        unchanged(before,accepted); require(canvas.boundaryDraftPreview() && canvas.boundaryDraftPreview()->segments.size()==3 &&
            canvas.boundaryDraftPreview()->labels.size()==3 && same(canvas.boundaryDraftPreview()->segments.back().start,endpoint) &&
            canvas.boundaryDraftPreview()->segments.back().end.x==proposed.end.x &&
            canvas.boundaryDraftPreview()->segments.back().end.y==proposed.end.y,"Enter appends exact off-grid proposal and ordinary dimension");
        const auto after=*canvas.boundaryDraftPreview();
        require(window.undoCommand() && same_segments(canvas.boundaryDraftPreview()->segments,draft->segments),
                "draft Undo removes accepted alignment");
        require(window.redoCommand() && same_segments(canvas.boundaryDraftPreview()->segments,after.segments) &&
            canvas.boundaryDraftPreview()->labels.size()==after.labels.size(),"draft Redo restores exact alignment and dimension");
        key(canvas,Qt::Key_Return); const auto closed=window.document().snapshot(); const auto boundaries=ids(closed,"measurement_boundary");
        require(boundaries.size()==1 && !canvas.boundaryDraftPreview() && closed.revision()==before.revision()+1,
                "normal Enter without a proposal still closes and commits measurement once");
        const auto owner=decode_identified_boundary_entity(closed.entities().at(boundaries.front()));
        require(owner.segments.size()==4 && same(owner.segments[2].segment.end,target) &&
            dimensions(closed,owner.id,BoundaryDimensionPlacement::automatic)==4,"completed outline retains exact alignment and dimensions");
        require(window.undoCommand() && window.document().snapshot().entities()==before.entities() && window.redoCommand() &&
            window.document().snapshot().entities()==closed.entities(),"committed measurement Undo/Redo restores whole result");
        reopened(window,closed,directory,horizontal ? "measurement-X" : "measurement-Y");
    } else {
        const auto wall=added_wall(before,accepted); const auto edge=baseline(wall);
        require(same(edge.start,endpoint) && same(edge.end,target) && edge.end.x==proposed.end.x &&
            edge.end.y==proposed.end.y && accepted.revision()==before.revision()+1 &&
            std::abs(wall.properties.at("thickness_m").get<double>()-0.14)<1e-12,"Enter persists exact projection with ordinary wall thickness");
        require(canvas.wallPreview() && same(canvas.wallPreview()->start,target),"alignment continues ordinary physical Wall chain");
        require(window.undoCommand() && window.document().snapshot().entities()==before.entities() && canvas.wallPreview() &&
            same(canvas.wallPreview()->start,endpoint),"physical Undo restores prior geometry and pen");
        require(window.redoCommand() && window.document().snapshot().entities()==accepted.entities() && canvas.wallPreview() &&
            same(canvas.wallPreview()->start,target),"physical Redo restores literal projection and pen");
        const auto redone=window.document().snapshot();
        key(canvas,Qt::Key_Return); unchanged(redone,window.document().snapshot());
        require(!canvas.wallPreview() && canvas.boundaryDraftPreview() &&
                canvas.boundaryDraftPreview()->pen_position &&
                same(*canvas.boundaryDraftPreview()->pen_position,target) &&
                !canvas.boundaryDraftPreview()->rubber_band,
                "ordinary Wall Enter parks the exact accepted pen without changing geometry");
        key(canvas,Qt::Key_Escape); unchanged(redone,window.document().snapshot());
        reopened(window,accepted,directory,horizontal ? "wall-X" : "wall-Y");
    }
    require(action(window,true).shortcut().isEmpty() && action(window,false).shortcut().isEmpty(),
            "bare X/Y are canvas commands rather than global typing shortcuts");
}

void auto_close(bool measurement,bool menu) {
    QTemporaryDir directory; require(directory.isValid(),"whole-outline closure uses temporary storage");
    MainWindow window({},nullptr,directory.filePath("library.json")); auto& canvas=prepare(window);
    if (measurement) require(window.beginBoundaryDrawing(BoundaryAuthoringMode::draw_first,"measurement"),"whole-outline measurement starts");
    else appraisal(window);
    const auto original=window.document().snapshot();
    for (const auto point:std::vector<Vec2>{{0,0},{4,0},{5,1},{5,4},{0,4}}) click(canvas,point);
    const auto before=window.document().snapshot(); auto* close=window.findChild<QAction*>("autoCloseBoundary");
    require(close,"existing auto-close menu remains available");
    if (menu) { require(close->isEnabled(),"whole outline enables closure action"); close->trigger(); events(); }
    else key(canvas,Qt::Key_A,Qt::NoModifier,"a");
    const auto closed=window.document().snapshot(); const auto boundaries=ids(closed,"measurement_boundary");
    require(boundaries.size()==1 && closed.revision()==before.revision()+1 && !canvas.wallPreview() && !canvas.boundaryDraftPreview(),
            "A/menu closes actual whole outline and commits exactly once");
    const auto& entity=closed.entities().at(boundaries.front()); const auto owner=decode_identified_boundary_entity(entity);
    if (measurement) require(owner.segments.size()==5 && same(owner.segments.front().segment.start,{0,0}) &&
        same(owner.segments.back().segment.start,{0,4}) && same(owner.segments.back().segment.end,{0,0}),
        "A uses original anchor rather than a recent local subchain");
    else {
        const auto edge=baseline(added_wall(before,closed));
        require(ids(closed,"wall").size()==5 && same(edge.start,{0,4}) && same(edge.end,{0,0}) &&
            wall_measurement_source_current(closed,entity) && exterior_wall_measurement_source_ids(entity)==ids(closed,"wall"),
            "physical A atomically adds closing wall and current whole exterior appraisal measurement");
    }
    require(dimensions(closed,owner.id,BoundaryDimensionPlacement::automatic)==owner.segments.size(),"A retains every ordinary dimension");
    require(window.undoCommand() && window.document().snapshot().entities()==(measurement ? original.entities() : before.entities()) &&
        window.redoCommand() && window.document().snapshot().entities()==closed.entities(),"closure Undo/Redo is atomic with derived exterior result");
    reopened(window,closed,directory,measurement ? "auto-close-measurement" : "auto-close-wall");
    require(close->shortcut().isEmpty(),"bare A belongs to focused canvas rather than global typing shortcut");
}

void manual_dimensions() {
    QTemporaryDir directory; require(directory.isValid(),"manual fixture uses temporary storage");
    MainWindow window({},nullptr,directory.filePath("library.json")); auto& canvas=prepare(window);
    require(window.beginBoundaryDrawing(BoundaryAuthoringMode::define_first,"measurement"),"actual manual Define First starts");
    const auto original=window.document().snapshot(); click(canvas,{0,0}); click(canvas,{4,0});
    const auto pending=*canvas.boundaryDraftPreview();
    require(!window.proposeDrawingAlignment(false) && !window.autoCloseActiveDrawing() &&
        same_segments(canvas.boundaryDraftPreview()->segments,pending.segments) &&
        canvas.boundaryDraftPreview()->instruction.contains("place this edge"),"pending manual dimension rejects alignment/A without consuming placement");
    unchanged(original,window.document().snapshot());
    mouse(canvas,QEvent::MouseMove,screen(canvas,{2,-0.5})); key(canvas,Qt::Key_Return);
    require(canvas.boundaryDraftPreview() && !canvas.boundaryDraftPreview()->instruction.contains("place this edge") &&
        same(canvas.boundaryDraftPreview()->labels.front().position,{2,-0.5}),
        "Define First second Enter anchors the displayed manual dimension without adding a side");
    unchanged(original,window.document().snapshot()); click(canvas,{3,3});
    require(!window.proposeDrawingAlignment(true) && !window.autoCloseActiveDrawing(),"second manual edge cannot be bypassed");
    click(canvas,{4,2}); propose(window,canvas,true); key(canvas,Qt::Key_Return);
    require(canvas.boundaryDraftPreview() && canvas.boundaryDraftPreview()->segments.size()==3 &&
        same(canvas.boundaryDraftPreview()->segments.back().end,{0,3}) && canvas.boundaryDraftPreview()->instruction.contains("place this edge"),
        "manual X accepts exact edge while preserving its dimension placement phase");
    require(!window.autoCloseActiveDrawing(),"A cannot automatically place accepted manual witness dimension");
    require(window.undoCommand() && canvas.boundaryDraftPreview()->segments.size()==2 && window.redoCommand() &&
        canvas.boundaryDraftPreview()->segments.size()==3 && canvas.boundaryDraftPreview()->instruction.contains("place this edge"),
        "manual alignment Undo/Redo retains pending dimension workflow");
    click(canvas,{1.5,3.5}); key(canvas,Qt::Key_A,Qt::NoModifier,"a");
    require(canvas.boundaryDraftPreview() && canvas.boundaryDraftPreview()->segments.size()==4 &&
        same(canvas.boundaryDraftPreview()->segments.back().start,{0,3}) && same(canvas.boundaryDraftPreview()->segments.back().end,{0,0}) &&
        canvas.boundaryDraftPreview()->instruction.contains("place this edge"),"A adds actual closing edge and leaves final manual dimension pending");
    unchanged(original,window.document().snapshot()); require(!window.autoCloseActiveDrawing(),"repeated A cannot bypass final manual placement");
    mouse(canvas,QEvent::MouseMove,screen(canvas,{-0.5,1.5})); key(canvas,Qt::Key_Return);
    const auto closed=window.document().snapshot(); const auto boundaries=ids(closed,"measurement_boundary");
    require(boundaries.size()==1 && !canvas.boundaryDraftPreview() && closed.revision()==original.revision()+1,
        "ordinary manual placement and Finish commit complete outline once");
    const auto owner=decode_identified_boundary_entity(closed.entities().at(boundaries.front()));
    require(owner.segments.size()==4 && dimensions(closed,owner.id,BoundaryDimensionPlacement::manual)==4,"all manual dimensions persist without substitutions");
    require(window.undoCommand() && window.document().snapshot().entities()==original.entities() && window.redoCommand() &&
        window.document().snapshot().entities()==closed.entities(),"manual whole result Undo/Redo is atomic");
    reopened(window,closed,directory,"manual-witness");
}

void guards() {
    MainWindow window; auto& canvas=prepare(window); const auto idle=window.document().snapshot();
    require(!window.proposeDrawingAlignment(true) && !window.autoCloseActiveDrawing(),"idle rejects alignment and A"); unchanged(idle,window.document().snapshot());
    click(canvas,{0,0}); require(!window.proposeDrawingAlignment(true) && !window.proposeDrawingAlignment(false) &&
        !window.autoCloseActiveDrawing(),"anchor-only zero proposals cannot create geometry");
    click(canvas,{4,0}); const auto one=window.document().snapshot();
    require(!window.proposeDrawingAlignment(false) && !window.autoCloseActiveDrawing(),"zero witness and degenerate one-edge outline fail closed"); unchanged(one,window.document().snapshot());
    click(canvas,{3,3}); const auto before=window.document().snapshot();
    for (const auto code:{Qt::Key_X,Qt::Key_Y,Qt::Key_A}) {
        for (const auto modifiers:{Qt::ControlModifier,Qt::AltModifier,Qt::MetaModifier}) {
            key(canvas,code,modifiers); require(!selected(canvas),"modified bare letters do not dispatch witnesses"); unchanged(before,window.document().snapshot());
        }
        key(canvas,code,Qt::NoModifier,{},true); require(!selected(canvas),"autorepeated letters cannot dispatch commands"); unchanged(before,window.document().snapshot());
    }
    const auto p=QRectF(canvas.rect()).center(); mouse(canvas,QEvent::MouseButtonPress,p,Qt::MiddleButton,Qt::MiddleButton);
    key(canvas,Qt::Key_X); key(canvas,Qt::Key_A);
    require(!selected(canvas) && !window.proposeDrawingAlignment(true) && !window.autoCloseActiveDrawing(),"pan/gesture blocks alignment and closure");
    unchanged(before,window.document().snapshot()); mouse(canvas,QEvent::MouseButtonRelease,p,Qt::MiddleButton);
    auto* height=window.findChild<QLineEdit*>("wallDrawHeight"); require(height,"guard uses actual inspector edit field");
    height->setFocus(); height->selectAll();
    for (const auto code:{Qt::Key_X,Qt::Key_Y,Qt::Key_A}) key(*height,code,Qt::NoModifier,QString(QChar(static_cast<char16_t>(code)).toLower()));
    require(height->text()=="xya" && !selected(canvas),"letters in inspector remain typed text"); unchanged(before,window.document().snapshot());
    height->setText("2.4 m"); canvas.setFocus(); key(canvas,Qt::Key_1,Qt::NoModifier,"1");
    auto* input=canvas.findChild<QLineEdit*>("drawingLengthInput"); require(input && input->hasFocus(),"native inline precise length editor has focus");
    for (const auto code:{Qt::Key_X,Qt::Key_Y,Qt::Key_A}) key(*input,code,Qt::NoModifier,QString(QChar(static_cast<char16_t>(code)).toLower()));
    require(input->text().contains("xya") && !selected(canvas),"X/Y/A in precise editor remain typed text"); unchanged(before,window.document().snapshot());
    key(*input,Qt::Key_Escape); canvas.setFocus();
    propose(window,canvas,true); height->setText("invalid height"); key(canvas,Qt::Key_Return);
    unchanged(before,window.document().snapshot()); require(canvas.wallPreview() && same(canvas.wallPreview()->start,{3,3}),"invalid dimensions cannot advance pen");
    height->setText("2.4 m"); canvas.setFocus(); propose(window,canvas,true);
    window.document().mark_read_only("Witness fixture read-only transition"); const auto read_only=window.document().snapshot();
    key(canvas,Qt::Key_Return); require(!window.proposeDrawingAlignment(true) && !window.autoCloseActiveDrawing(),"read-only context blocks stale acceptance and closure");
    unchanged(read_only,window.document().snapshot());
}

void stale(bool measurement,int change) {
    MainWindow window; auto& canvas=prepare(window); start(window,canvas,measurement);
    const auto before=window.document().snapshot(); propose(window,canvas,true);
    if (change==0) {
        mouse(canvas,QEvent::MouseMove,screen(canvas,{2,4})); require(!selected(canvas),"mouse movement clears proposal");
        unchanged(before,window.document().snapshot()); key(canvas,Qt::Key_Return);
        if (!measurement) { unchanged(before,window.document().snapshot());
            require(!canvas.wallPreview() && canvas.boundaryDraftPreview() &&
                    canvas.boundaryDraftPreview()->pen_position &&
                    same(*canvas.boundaryDraftPreview()->pen_position,endpoint) &&
                    !canvas.boundaryDraftPreview()->rubber_band,
                    "normal Wall Enter after mouse invalidation parks the accepted pen without a stale edge"); }
        else {
            const auto snapshot=window.document().snapshot(); const auto boundaries=ids(snapshot,"measurement_boundary");
            require(boundaries.size()==1 && !canvas.boundaryDraftPreview(),"normal Enter after invalidation finishes measurement");
            const auto owner=decode_identified_boundary_entity(snapshot.entities().at(boundaries.front()));
            require(owner.segments.size()==3 && same(owner.segments.back().segment.start,endpoint) &&
                same(owner.segments.back().segment.end,anchor),"normal Finish closes directly without stale projection edge");
        }
    } else if (change==1) {
        click(canvas,{2,4}); require(!selected(canvas),"new click clears proposal");
        if (measurement) require(canvas.boundaryDraftPreview()->segments.size()==3 && same(canvas.boundaryDraftPreview()->segments.back().end,{2,4}),"new click uses literal endpoint");
        else require(same(baseline(added_wall(before,window.document().snapshot())).end,{2,4}),"new wall click uses endpoint rather than proposal");
    } else if (change==2) {
        key(canvas,Qt::Key_1,Qt::NoModifier,"1"); auto* input=canvas.findChild<QLineEdit*>("drawingLengthInput");
        require(input && input->hasFocus() && !selected(canvas),"new typed construction clears witness"); unchanged(before,window.document().snapshot()); key(*input,Qt::Key_Escape);
    } else if (change==3) {
        key(canvas,Qt::Key_Escape); require(!selected(canvas) && !canvas.wallPreview() && !canvas.boundaryDraftPreview(),"Esc ends actual drawing and proposal");
        key(canvas,Qt::Key_Return); unchanged(before,window.document().snapshot());
    } else if (change==4) {
        require(window.undoCommand() && !selected(canvas),"Undo clears ephemeral proposal before ordinary accepted edge Undo");
        if (measurement) require(canvas.boundaryDraftPreview()->segments.size()==1,"Undo only removes accepted draft edge");
        else require(ids(window.document().snapshot(),"wall").size()==1,"Undo only removes accepted wall");
        require(window.redoCommand() && !selected(canvas),"Redo never resurrects proposal");
    } else if (change==5) {
        window.setWorkspace(Workspace::architectural);
        events(); unchanged(before,window.document().snapshot());
        require(window.workspace()==Workspace::measurement && selected(canvas) &&
            window.lastError().contains("Finish or cancel"),
            "active drawing refuses a view switch and retains its unchanged proposal context");
    } else {
        auto property=before.entities().at("property-1"); property.properties["name"]="Source changed after proposal";
        window.document().apply(ApplyEntityChanges{before.revision(),{EntityChange::upsert(property)}, {}, "Fixture source revision change"});
        const auto changed=window.document().snapshot(); key(canvas,Qt::Key_Return); unchanged(changed,window.document().snapshot());
    }
}
void altered_owner(bool curved) {
    MainWindow window; auto& canvas=prepare(window); start(window,canvas,false); propose(window,canvas,true);
    const auto before=window.document().snapshot();
    const auto found=std::find_if(before.entities().begin(),before.entities().end(),[](const auto& pair) {
        return pair.second.type=="wall" && same(baseline(pair.second).end,endpoint);
    });
    require(found!=before.entities().end(),"source guard finds actual last drawing wall owner"); auto replacement=found->second;
    if (curved) replacement.properties["baseline"]["sweep_radians"]=0.5;
    else replacement.properties["baseline"]["start"][0]=4.25;
    try { window.document().apply(ApplyEntityChanges{before.revision(),{EntityChange::upsert(replacement)}, {}, "Fixture altered drawing owner"}); }
    catch (const DocumentError& error) {
        require(error.code()==DocumentErrorCode::constraint_violation,"invalid owner may fail actual hard endpoint admission contract");
        unchanged(before,window.document().snapshot()); return;
    }
    const auto changed=window.document().snapshot(); key(canvas,Qt::Key_Return);
    unchanged(changed,window.document().snapshot());
    if (curved) {
        require(window.proposeDrawingAlignment(true),"fresh proposal may use a valid changed curved owner");
        unchanged(changed,window.document().snapshot());
    } else {
        require(!window.proposeDrawingAlignment(true) && !window.autoCloseActiveDrawing(),
                "admitted disconnected owner independently rejects alignment and closure");
        unchanged(changed,window.document().snapshot());
    }
}
void populated_pointer_latency() {
    MainWindow window; auto& canvas=prepare(window);
    const auto seed_id=window.createStraightWall({100,100},{102,100});
    require(!seed_id.isEmpty(),"latency fixture has a valid physical wall on actual active organization");
    const auto source=window.document().snapshot();
    const auto& seed=source.entities().at(seed_id.toStdString());
    std::vector<EntityChange> changes;
    for (int i=0;i<300;++i) {
        const double x=110.0+5.0*(i%20),y=110.0+5.0*(i/20);
        auto properties=nlohmann::json{{"baseline",{{"start",{x,y}},{"end",{x+2,y}},{"sweep_radians",0.0}}},
            {"thickness_m",0.14},{"height_m",2.4},{"elevation_m",0.0},{"classification","interior"}};
        for (const auto* name:{"property_id","building_id","floor_id","layer_id","phase_id"})
            if (seed.properties.contains(name)) properties[name]=seed.properties.at(name);
        changes.push_back(EntityChange::upsert(Entity{"witness-latency-wall-"+std::to_string(i),
            "wall",std::move(properties),false,nlohmann::json::object()}));
    }
    window.document().apply(ApplyEntityChanges{source.revision(),std::move(changes),{},"Populate pointer latency fixture"});
    require(window.selectEntity({}),"clear selection before latency measurement draft");
    start(window,canvas,true);
    const auto before=window.document().snapshot(); const auto draft=*canvas.boundaryDraftPreview();
    QElapsedTimer timer; timer.start();
    for (int i=0;i<100;++i) mouse(canvas,QEvent::MouseMove,screen(canvas,{1.5+0.001*i,4.0+0.001*i}));
    const auto elapsed=timer.elapsed();
    std::cout<<"witness pointer latency: "<<elapsed<<" ms / 100 native moves with 301 unrelated walls\n";
    require(elapsed<1500,"populated measurement pointer moves stay below broad 1500 ms native fixture budget");
    unchanged(before,window.document().snapshot());
    require(canvas.boundaryDraftPreview() && same_segments(canvas.boundaryDraftPreview()->segments,draft.segments) &&
        canvas.boundaryDraftPreview()->labels.size()==draft.labels.size(),"latency path changes no accepted geometry or dimensions");
}
void imperial_alignment() {
    MainWindow window; auto& canvas=prepare(window); window.setMetricUnits(false); canvas.setFocus();
    click(canvas,{0,0}); click(canvas,{1.2192,0}); click(canvas,{1.2192,0.9144});
    const auto before=window.document().snapshot();
    key(canvas,Qt::Key_X,Qt::NoModifier,"x"); require(selected(canvas),"Imperial X selects a witness");
    const auto found=std::find_if(canvas.drawingWitnesses().begin(),canvas.drawingWitnesses().end(),
        [](const auto& value) {return value.selected;});
    require(same(found->segment.end,{0,0.9144}) && found->dimension_text==QStringLiteral("4 ft 0 in"),
        "Imperial witness shows a literal four-foot side");
    unchanged(before,window.document().snapshot());
    key(canvas,Qt::Key_Return); const auto accepted=window.document().snapshot();
    require(same(baseline(added_wall(before,accepted)).end,{0,0.9144}),"Imperial Enter accepts exact endpoint");
    key(canvas,Qt::Key_A,Qt::NoModifier,"a");
    require(ids(window.document().snapshot(),"wall").size()==4 && !canvas.wallPreview(),
        "Imperial A closes the physical outline");
}
void checkpoint_failure_rollback() {
    MainWindow window; auto& canvas=prepare(window); start(window,canvas,true);
    const auto draft=*canvas.boundaryDraftPreview();
    auto property=window.document().snapshot().entities().at("property-1");
    property.properties["name"]="External source change before checkpoint";
    window.document().apply(ApplyEntityChanges{window.document().revision(),{EntityChange::upsert(property)}, {},
        "Change source outside active workspace"});
    const auto changed=window.document().snapshot();
    const auto verify=[&](const char* phase) {
        unchanged(changed,window.document().snapshot());
        require(canvas.boundaryDraftPreview() && same_segments(canvas.boundaryDraftPreview()->segments,draft.segments) &&
            canvas.boundaryDraftPreview()->labels.size()==draft.labels.size() &&
            canvas.boundaryDraftPreview()->instruction==draft.instruction,
            "failed checkpoint rolls back candidate geometry, dimensions and phase");
        if (!window.lastError().contains("checkpoint failed"))
            std::cerr << "checkpoint rollback " << phase << " lastError: "
                      << window.lastError().toStdString() << '\n';
        require(window.lastError().contains("checkpoint failed"),
            "fixture reaches actual recovery checkpoint refusal rather than proposal rejection");
    };
    click(canvas,{2,4}); verify("click");
    key(canvas,Qt::Key_Return); verify("Enter");
}

void pending_dimension_guards() {
    for (const bool read_only : {true,false}) {
        MainWindow window; auto& canvas=prepare(window);
        require(window.beginBoundaryDrawing(BoundaryAuthoringMode::define_first,"measurement"),
                "pending dimension guard starts actual Define First");
        click(canvas,{0,0}); click(canvas,{4,0});
        mouse(canvas,QEvent::MouseMove,screen(canvas,{2,-0.5}));
        const auto draft=*canvas.boundaryDraftPreview();
        if (read_only) window.document().mark_read_only("Pending dimension fixture read-only transition");
        else {
            auto property=window.document().snapshot().entities().at("property-1");
            property.properties["name"]="Source changed before manual Enter";
            window.document().apply(ApplyEntityChanges{window.document().revision(),{EntityChange::upsert(property)}, {},
                "Change pending dimension source"});
        }
        const auto changed=window.document().snapshot(); key(canvas,Qt::Key_Return);
        unchanged(changed,window.document().snapshot());
        require(canvas.boundaryDraftPreview() && same_segments(canvas.boundaryDraftPreview()->segments,draft.segments) &&
            canvas.boundaryDraftPreview()->labels.size()==draft.labels.size() &&
            canvas.boundaryDraftPreview()->instruction.contains("place this edge") &&
            window.lastError().contains("context changed or is read-only"),
            "manual Enter refuses stale/read-only dimension without advancing draft phase");
        if (read_only) {
            click(canvas,{2,-0.5}); unchanged(changed,window.document().snapshot());
            require(canvas.boundaryDraftPreview()->instruction.contains("place this edge"),
                    "read-only manual mouse placement also preserves pending dimension");
        }
    }
}

void read_only_draft_finish() {
    MainWindow window; auto& canvas=prepare(window); start(window,canvas,true);
    const auto draft=*canvas.boundaryDraftPreview();
    window.document().mark_read_only("Draw First fixture read-only transition");
    const auto before=window.document().snapshot();
    key(canvas,Qt::Key_Return); unchanged(before,window.document().snapshot());
    require(canvas.boundaryDraftPreview() && same_segments(canvas.boundaryDraftPreview()->segments,draft.segments) &&
        canvas.boundaryDraftPreview()->labels.size()==draft.labels.size() &&
        canvas.boundaryDraftPreview()->instruction==draft.instruction && window.lastError().contains("read-only"),
        "read-only ordinary Finish cannot close or reclassify the draft");
    click(canvas,{2,4}); unchanged(before,window.document().snapshot());
    require(same_segments(canvas.boundaryDraftPreview()->segments,draft.segments),
            "read-only drawing click cannot append an uncheckpointed edge");
}

void recovered_tolerance_rejection() {
    QTemporaryDir directory; require(directory.isValid(),"nondefault tolerance recovery uses temporary storage");
    MainWindow seed; seed.setAttribute(Qt::WA_DontShowOnScreen,true);
    ProjectWorkspace workspace(seed.document().snapshot());
    BoundaryAuthoringOptions options; options.geometry_tolerance_metres=0.01;
    BoundaryAuthoringSession session(BoundaryAuthoringMode::draw_first,options);
    session.set_classification("measurement");
    (void)session.anchor({0,0}); (void)session.add_line_to({1,0}); (void)session.add_line_to({2,0.001});
    BoundaryActiveRecovery active{capture_boundary_recovery_source(workspace.snapshot(),
        {"property-1","building-1","floor-1","layer-1"}),session.recovery_checkpoint()};
    auto checkpoint=workspace.prepare_boundary_checkpoint(active); (void)workspace.commit(checkpoint);
    const auto history=capture_workspace_history_record(workspace.capture());
    RecoveryLedger ledger{{"history","workspace_history",encode_workspace_history_record(workspace.snapshot(),history,active)},
        {"active","boundary_active",encode_boundary_active_recovery(active)}};
    const auto path=directory.filePath("nondefault-tolerance.bldproj");
    (void)ProjectStore::save_archive(std::filesystem::path(path.toStdWString()),
        ProjectArchiveSnapshot(workspace.snapshot(),ledger,ArchiveRole::ordinary));
    MainWindow window({},nullptr,directory.filePath("library.json")); auto& canvas=prepare(window);
    require(window.openProject(path) && canvas.boundaryDraftPreview() && canvas.boundaryDraftPreview()->segments.size()==2,
            "ordinary native reopen restores thin open triangle with nondefault authoring tolerance");
    const auto before=window.document().snapshot(); const auto draft=*canvas.boundaryDraftPreview();
    const auto before_path=directory.filePath("before-A.bldproj");
    require(window.saveProjectAs(before_path),"capture actual workspace checkpoint before rejected A");
    const auto before_archive=ProjectStore::load_archive(std::filesystem::path(before_path.toStdWString()),ArchiveRole::ordinary);
    require(before_archive.supported() && before_archive.recovery.decoded->active &&
        BoundaryAuthoringSession::from_recovery_checkpoint(before_archive.recovery.decoded->active->checkpoint)
            .options().geometry_tolerance_metres==0.01,"native recovery retains declared 0.01m tolerance");
    require(!window.autoCloseActiveDrawing(),"A rejects thin triangle overlap using recovered session tolerance");
    unchanged(before,window.document().snapshot());
    require(canvas.boundaryDraftPreview() && same_segments(canvas.boundaryDraftPreview()->segments,draft.segments) &&
        canvas.boundaryDraftPreview()->labels.size()==draft.labels.size() &&
        canvas.boundaryDraftPreview()->instruction==draft.instruction,
        "failed nondefault-tolerance A preserves draft geometry, dimensions and phase");
    const auto after_path=directory.filePath("after-A.bldproj");
    require(window.saveProjectAs(after_path),"capture actual workspace checkpoint after rejected A");
    const auto after_archive=ProjectStore::load_archive(std::filesystem::path(after_path.toStdWString()),ArchiveRole::ordinary);
    require(after_archive.supported() && after_archive.recovery.decoded->active==before_archive.recovery.decoded->active &&
        after_archive.archive->recovery().size()==before_archive.archive->recovery().size(),
        "failed A preserves exact recovered active checkpoint and retained workspace ledger");
    for (std::size_t i=0;i<before_archive.archive->recovery().size();++i)
        require(before_archive.archive->recovery()[i].envelope==after_archive.archive->recovery()[i].envelope,
                "failed A changes no serialized workspace checkpoint envelope");
}
} // namespace

int main(int argc,char** argv) {
    sketch::testing::noninteractive_errors(); QStandardPaths::setTestModeEnabled(true); QApplication app(argc,argv);
    QCoreApplication::setOrganizationName("Vertex-tests");
    QCoreApplication::setApplicationName(QStringLiteral("Vertex-witness-test-")+QUuid::createUuid().toString(QUuid::WithoutBraces));
    try {
        require(QFontDatabase::addApplicationFont(":/fonts/Inter.ttf")>=0,"bundled font loads for actual native rendering");
        app.setFont(QFont("Inter",10));
        for (const auto measurement:{false,true}) {
            std::cout << "alignment " << (measurement ? "measurement" : "wall") << '\n';
            alignment(measurement,true); alignment(measurement,false);
            std::cout << "auto close " << (measurement ? "measurement" : "wall") << '\n';
            auto_close(measurement,false); auto_close(measurement,true);
            for (int change=0;change<7;++change) {
                std::cout << "stale " << measurement << " " << change << '\n';
                stale(measurement,change);
            }
        }
        manual_dimensions(); guards(); altered_owner(false); altered_owner(true);
        populated_pointer_latency(); recovered_tolerance_rejection();
        checkpoint_failure_rollback();
        imperial_alignment();
        pending_dimension_guards();
        read_only_draft_finish();
        std::cout<<"witness_alignment_desktop_tests passed\n"; return 0;
    } catch (const std::exception& error) {
        std::cerr<<"witness_alignment_desktop_tests: "<<error.what()<<'\n'; return 1;
    }
}
