#include "sketch/desktop/main_window.hpp"
#include "sketch/measurement_linework.hpp"
#include "sketch/model_phases.hpp"
#include "sketch/project_store.hpp"
#include "sketch/project_workspace.hpp"
#include "sketch/workspace_history_record.hpp"
#include "../src/desktop/plan_canvas.hpp"
#include "support/noninteractive_errors.hpp"

#include <QApplication>
#include <QFont>
#include <QFontDatabase>
#include <QDir>
#include <QEventLoop>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QUuid>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace {
using namespace sketch;
using namespace sketch::desktop;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void events() { QCoreApplication::processEvents(QEventLoop::AllEvents, 50); }
bool points_near(Vec2 actual, Vec2 expected) {
    return std::abs(actual.x - expected.x) < 1e-10 && std::abs(actual.y - expected.y) < 1e-10;
}
PlanCanvas& prepare(MainWindow& window) {
    window.setAttribute(Qt::WA_DontShowOnScreen, true);
    window.resize(1400, 900); window.show(); events(); window.setMetricUnits(true);
    auto* canvas = dynamic_cast<PlanCanvas*>(window.findChild<QWidget*>(QStringLiteral("measurementPlanCanvas")));
    require(canvas, "real measurement canvas exists");
    canvas->setOverviewMapEnabled(false); canvas->setSnapEnabled(false); return *canvas;
}
void click(PlanCanvas& canvas, Vec2 point, Qt::MouseButton button = Qt::LeftButton) {
    const auto center = QRectF(canvas.rect()).center(); const auto view = canvas.viewCenter();
    const auto screen = center + QPointF((point.x-view.x)*canvas.viewScale(), -(point.y-view.y)*canvas.viewScale());
    QMouseEvent move(QEvent::MouseMove, screen, canvas.mapToGlobal(screen.toPoint()), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&canvas, &move);
    QMouseEvent press(QEvent::MouseButtonPress, screen, canvas.mapToGlobal(screen.toPoint()), button, button, Qt::NoModifier);
    QApplication::sendEvent(&canvas, &press);
    QMouseEvent release(QEvent::MouseButtonRelease, screen, canvas.mapToGlobal(screen.toPoint()), button, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&canvas, &release); events();
}
void key(PlanCanvas& canvas, int code, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    QKeyEvent event(QEvent::KeyPress, code, modifiers); QApplication::sendEvent(&canvas, &event); events();
}
Entity stroke(const DocumentSnapshot& snapshot) {
    const Entity* result = nullptr;
    for (const auto& [id, entity] : snapshot.entities()) if (entity.type == "measurement_linework") {
        require(!result, "continuation keeps one stroke entity"); result = &entity;
    }
    require(result, "stroke entity exists"); return *result;
}
MeasurementLinework model(const Entity& entity) {
    const auto decoded = decode_measurement_linework_model(entity.properties.at("model"));
    require(decoded.model.has_value(), "persisted measured stroke decodes"); return *decoded.model;
}
void pen(PlanCanvas& canvas, Vec2 expected, const char* message) {
    require(canvas.boundaryDraftPreview() && canvas.boundaryDraftPreview()->pen_position &&
        points_near(*canvas.boundaryDraftPreview()->pen_position, expected), message);
}
void start(MainWindow& window, Vec2 anchor = {0,0}) {
    require(window.beginMeasurementLinework() && window.appendMeasurementLineworkPoint(anchor), "start live measured stroke");
}
void capture(MainWindow& window, const QString& name) {
    const auto directory = qEnvironmentVariable("VERTEX_TEST_CAPTURE_DIR");
    if (directory.isEmpty()) return;
    require(QDir().mkpath(directory) && window.grab().save(QDir(directory).filePath(name)), "full native history capture saves");
}
void test_native_history_continuation() {
    MainWindow window; auto& canvas = prepare(window); start(window);
    click(canvas, {3,0}); const auto first = window.document().snapshot();
    click(canvas, {3,2}); const auto second = window.document().snapshot();
    const auto original = model(stroke(second));
    key(canvas, Qt::Key_Z, Qt::ControlModifier);
    require(window.document().snapshot().entities() == first.entities(), "Ctrl+Z removes exactly the latest edge");
    pen(canvas, {3,0}, "Ctrl+Z retains active measured pen at the preceding endpoint");
    capture(window, QStringLiteral("measured-stroke-after-undo.png"));
    key(canvas, Qt::Key_Y, Qt::ControlModifier);
    require(window.document().snapshot().entities() == second.entities(), "Ctrl+Y restores exact receipts and identities");
    pen(canvas, {3,2}, "Ctrl+Y retains active measured pen at restored endpoint");
    click(canvas, {5,2}); const auto continued = model(stroke(window.document().snapshot()));
    require(continued.stroke_id == original.stroke_id && continued.edges.size() == 3 &&
        continued.edges[0] == original.edges[0] && continued.edges[1] == original.edges[1] &&
        continued.edges[2].start_vertex_id == original.edges[1].end_vertex_id,
        "native click after Redo appends to same stable stroke and vertex");
    capture(window, QStringLiteral("measured-stroke-after-continuation.png"));
}
void test_anchor_history_and_branch() {
    MainWindow window; auto& canvas = prepare(window);
    const auto baseline = window.document().snapshot(); start(window, {0.125,-0.25});
    require(window.appendMeasurementLineworkHeading(QStringLiteral("1.25 m"), QStringLiteral("0 deg")), "typed first edge");
    const auto first = window.document().snapshot(); const auto first_model = model(stroke(first));
    require(window.appendMeasurementLineworkHeading(QStringLiteral("2.5 m"), QStringLiteral("90 deg")), "typed second edge");
    const auto second = window.document().snapshot();
    key(canvas, Qt::Key_Z, Qt::ControlModifier); key(canvas, Qt::Key_Z, Qt::ControlModifier);
    require(window.document().snapshot().entities() == baseline.entities(), "repeated Undo removes first edge without an empty entity");
    pen(canvas, {0.125,-0.25}, "first-edge Undo retains original exact anchor");
    key(canvas, Qt::Key_Y, Qt::ControlModifier); key(canvas, Qt::Key_Y, Qt::ControlModifier);
    require(window.document().snapshot().entities() == second.entities(), "repeated Redo restores all original typed receipts");
    key(canvas, Qt::Key_Z, Qt::ControlModifier);
    require(window.appendMeasurementLineworkHeading(QStringLiteral("4.125 m"), QStringLiteral("180 deg")), "typed continuation after Undo remains live");
    const auto branched = window.document().snapshot(); const auto current = model(stroke(branched));
    require(current.stroke_id == first_model.stroke_id && current.edges.size() == 2 &&
        current.edges.front() == first_model.edges.front() &&
        current.edges.back().start_vertex_id == first_model.edges.front().end_vertex_id &&
        current.edges.back().receipt.distance->original_expression == "4.125 m" &&
        current.edges.back().receipt.heading->original_expression == "180 deg" &&
        points_near(replay_measurement_linework(current).edges.back().segment.end, {-2.75,-0.25}),
        "branch preserves prefix IDs and typed expression with stable world replay");
    require(!window.document().can_redo(), "append after Undo clears document Redo");
    key(canvas, Qt::Key_Z, Qt::ControlModifier);
    require(window.document().snapshot().entities() == first.entities(), "one Undo removes only branched edge");
    key(canvas, Qt::Key_Z, Qt::ControlModifier);
    pen(canvas, {0.125,-0.25}, "branched first-edge Undo retains anchor");
    const auto revision = window.document().revision(); key(canvas, Qt::Key_Z, Qt::ControlModifier);
    require(window.document().revision() == revision && !canvas.boundaryDraftPreview(), "Undo of uncommitted anchor cancels locally");
    require(window.redoCommand() && !canvas.boundaryDraftPreview(), "Redo after local cancel restores history without resurrecting pen");
}
void test_uncommitted_anchor_preserves_other_history() {
    MainWindow window; auto& canvas = prepare(window);
    window.document().apply(NameRevision{window.document().revision(), "unrelated history"});
    const auto before = window.document().snapshot(); start(window, {7,8});
    key(canvas, Qt::Key_Z, Qt::ControlModifier);
    require(window.document().snapshot().revision() == before.revision() &&
        window.document().snapshot().entities() == before.entities() && !canvas.boundaryDraftPreview(),
        "anchor Undo leaves unrelated document command intact");
}
void test_noop_and_foreign_history_end_session() {
    MainWindow window; auto& canvas = prepare(window); start(window); click(canvas, {2,0});
    const auto first = window.document().snapshot();
    require(!window.redoCommand(), "current no-op Redo does not change history");
    pen(canvas,{2,0}, "current no-op Redo retains validated measured pen");
    require(window.appendMeasurementLineworkPoint({2,2}) && model(stroke(window.document().snapshot())).edges.size() == 2,
        "validated pen continues after no-op Redo");
    window.document().apply(NameRevision{window.document().revision(), "stale no-op fixture"});
    const auto stale = window.document().snapshot();
    require(!window.redoCommand() && !canvas.boundaryDraftPreview(), "stale no-op Redo ends authoring");
    require(!window.appendMeasurementLineworkPoint({4,2}) && window.document().snapshot().entities() == stale.entities(),
        "stale no-op Redo cannot revive or append from obsolete pen");
    start(window, {5,5}); click(canvas, {7,5});
    auto changed = window.document().snapshot().entities().at(stroke(first).id);
    changed.properties["history_fixture"] = "foreign";
    window.document().apply(ApplyEntityChanges{.expected_revision=window.document().revision(),
        .entity_changes={EntityChange::upsert(changed)}, .message="foreign document mutation"});
    require(window.undoCommand() && !canvas.boundaryDraftPreview(), "Undo of foreign command preserves document history but ends stale pen");
    require(!window.appendMeasurementLineworkHeading(QStringLiteral("1 m"), QStringLiteral("0 deg")), "foreign history cannot resume authoring");
    require(window.redoCommand() && !canvas.boundaryDraftPreview(), "foreign Redo never starts drawing");
}
void test_finish_close_retrace_and_reopen() {
    for (const auto ending : {static_cast<int>(Qt::Key_Return), static_cast<int>(Qt::Key_Escape), 0}) {
        MainWindow window; auto& canvas = prepare(window); start(window); click(canvas, {2,0});
        if (ending) key(canvas, ending); else click(canvas, {2,0}, Qt::RightButton);
        require(!canvas.boundaryDraftPreview(), "intentional finish clears pen");
        require(window.undoCommand() && window.redoCommand() && !canvas.boundaryDraftPreview(), "finished stroke history never restarts drawing");
    }
    MainWindow window; auto& canvas = prepare(window); start(window);
    click(canvas,{3,0}); click(canvas,{3,2}); click(canvas,{3,0});
    const auto retraced = window.document().snapshot();
    require(window.undoCommand() && window.redoCommand() && window.document().snapshot().entities() == retraced.entities(), "retraced stroke replays exact history");
    pen(canvas, {3,0}, "retraced Redo retains current pen");
    click(canvas,{0,0}); const auto closed = window.document().snapshot();
    require(model(stroke(closed)).closed && !canvas.boundaryDraftPreview(), "closing to anchor intentionally finishes stroke");
    require(window.undoCommand() && window.redoCommand() && window.document().snapshot().entities() == closed.entities() &&
        !canvas.boundaryDraftPreview(), "closed-stroke history does not resurrect pen");
    QTemporaryDir directory; const auto path = directory.filePath(QStringLiteral("history.bldproj"));
    require(window.saveProjectAs(path) && window.openProject(path), "save and reopen stroke history");
    require(window.document().snapshot().entities() == closed.entities() && !canvas.boundaryDraftPreview(), "reopen restores exact geometry without active drawing");
    require(window.undoCommand() && window.redoCommand() && !canvas.boundaryDraftPreview(), "reopened document history does not restart pen");
}
void test_save_active_stroke_and_reopen() {
    MainWindow window; auto& canvas = prepare(window); start(window, {0.125,-0.25});
    require(window.appendMeasurementLineworkHeading(QStringLiteral("1.25 m"), QStringLiteral("0 deg")), "typed active save edge");
    const auto retained = window.document().snapshot();
    QTemporaryDir directory; const auto path = directory.filePath(QStringLiteral("active-stroke.bldproj"));
    require(window.saveProjectAs(path), "save while measured stroke remains active");
    pen(canvas, {1.375,-0.25}, "saving preserves current active pen");
    require(window.openProject(path) && window.document().snapshot().entities() == retained.entities() &&
        !canvas.boundaryDraftPreview(), "active saved stroke reopens exactly without spontaneous drawing");
    require(window.undoCommand() && window.redoCommand() && window.document().snapshot().entities() == retained.entities() &&
        !canvas.boundaryDraftPreview(), "reopened active-stroke history stays ordinary document navigation");
}
void test_layer_workspace_and_phase_context() {
    MainWindow window; auto& canvas = prepare(window);
    const auto initial = window.document().snapshot();
    const auto original_layer = window.activeLayerId();
    const auto active = initial.entities().at(original_layer.toStdString());
    const auto floor = QString::fromStdString(active.properties.at("floor_id").get<std::string>());
    const auto sibling = window.createLayer(floor, QStringLiteral("History context layer"));
    require(!sibling.isEmpty(), "create valid alternate drawing layer");
    require(window.setActiveLayer(original_layer), "return from auto-selected new layer before context fixture");
    start(window); click(canvas,{2,0});
    require(window.setActiveLayer(sibling) && !canvas.boundaryDraftPreview(), "active layer change ends measured session");
    require(!window.appendMeasurementLineworkPoint({2,2}), "old-layer pen cannot append in new context");
    require(window.undoCommand() && window.redoCommand() && !canvas.boundaryDraftPreview(), "layer navigation history never revives old pen");
    start(window,{5,5}); click(canvas,{7,5});
    window.setWorkspace(Workspace::architectural);
    require(window.workspace() == Workspace::measurement, "active drawing refuses workspace change");
    pen(canvas,{7,5}, "refused workspace change preserves current pen");
    window.finishMeasurementLinework(); window.setWorkspace(Workspace::architectural);
    require(window.workspace() == Workspace::architectural && window.undoCommand() && window.redoCommand() &&
        !canvas.boundaryDraftPreview() && !window.appendMeasurementLineworkPoint({7,7}), "finished drawing cannot revive across workspace change");
    window.setWorkspace(Workspace::measurement);
    const auto source = window.document().snapshot(); std::vector<std::string> registry;
    for (const auto& [id, entity] : source.entities()) if (entity.type == "building" || entity.type == "floor") registry.push_back(id);
    const auto phases = ModelPhases::create(registry, registry, {{"history-alternative", "History alternative", {}, {}}});
    auto phase_entity = Entity::create("model_phases", {{"model", phases.to_json()}});
    window.document().apply(ApplyEntityChanges{.expected_revision=window.document().revision(),
        .entity_changes={EntityChange::upsert(phase_entity)}, .message="valid phase fixture"});
    start(window,{10,10}); click(canvas,{12,10});
    require(window.selectRemodelingAlternative(QStringLiteral("history-alternative")), "select valid semantic phase alternative");
    require(!window.appendMeasurementLineworkPoint({12,12}), "phase change invalidates existing measured source");
    require(window.undoCommand() && !canvas.boundaryDraftPreview(), "Undo of phase selection does not revive stale measured pen");
    require(window.redoCommand() && !canvas.boundaryDraftPreview(), "Redo of phase selection leaves drawing finished");
}
void test_workspace_history_continuation() {
    MainWindow seed; seed.document().mark_saved(seed.document().revision());
    ProjectWorkspace workspace(seed.document().snapshot());
    auto edit = workspace.prepare(NameRevision{workspace.snapshot().revision(), "workspace fixture"});
    (void)workspace.commit(edit); const auto capture = workspace.capture();
    const RecoveryLedger ledger{{"history", "workspace_history", encode_workspace_history_record(
        capture.document(), capture_workspace_history_record(capture), capture.active_boundary())}};
    QTemporaryDir directory; const auto path = directory.filePath(QStringLiteral("workspace.bldproj"));
    (void)ProjectStore::save_archive(std::filesystem::path(path.toStdWString()),
        ProjectArchiveSnapshot(capture.document(), ledger, ArchiveRole::ordinary));
    MainWindow window; auto& canvas = prepare(window); window.document().mark_saved(window.document().revision());
    require(window.openProject(path), "open workspace-backed recovery history");
    start(window); click(canvas,{2,0}); const auto first = window.document().snapshot();
    click(canvas,{2,3}); const auto second = window.document().snapshot();
    key(canvas,Qt::Key_Z,Qt::ControlModifier);
    require(window.document().snapshot().entities() == first.entities(), "workspace Undo removes own last edge");
    pen(canvas,{2,0}, "workspace Undo retains measured pen");
    key(canvas,Qt::Key_Y,Qt::ControlModifier);
    require(window.document().snapshot().entities() == second.entities(), "workspace Redo restores exact own last edge");
    click(canvas,{4,3}); require(model(stroke(window.document().snapshot())).edges.size() == 3, "workspace history supports click continuation");
}
}
int main(int argc, char** argv) {
    sketch::testing::noninteractive_errors(); QStandardPaths::setTestModeEnabled(true); QApplication application(argc,argv);
    QCoreApplication::setApplicationName(QStringLiteral("Vertex-linework-history-test-")+QUuid::createUuid().toString(QUuid::WithoutBraces));
    try {
        require(QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/Inter.ttf")) >= 0, "bundled Inter font loads");
        application.setFont(QFont(QStringLiteral("Inter"),10));
        test_native_history_continuation(); test_anchor_history_and_branch(); test_uncommitted_anchor_preserves_other_history();
        test_noop_and_foreign_history_end_session(); test_finish_close_retrace_and_reopen(); test_save_active_stroke_and_reopen();
        test_layer_workspace_and_phase_context(); test_workspace_history_continuation();
    } catch (const std::exception& error) {
        std::cerr << "measurement_linework_history_desktop_tests: " << error.what() << '\n'; return 1;
    }
    std::cout << "Measurement linework history desktop tests passed\n"; return 0;
}
