#include "sketch/desktop/main_window.hpp"

#include "sketch/boundary_dimension.hpp"
#include "sketch/boundary_entity.hpp"
#include "sketch/boundary_receipt.hpp"
#include "sketch/document_digest.hpp"
#include "sketch/desktop/boundary_input_dialog.hpp"
#include "support/noninteractive_errors.hpp"
#include "../src/desktop/plan_canvas.hpp"

#include <QApplication>
#include <QAbstractButton>
#include <QCoreApplication>
#include <QDialog>
#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
#include <QFont>
#include <QFontDatabase>
#include <QKeyEvent>
#include <QInputDialog>
#include <QLineEdit>
#include <QMessageBox>
#include <QMouseEvent>
#include <QRectF>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTimer>

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <functional>
#include <map>
#include <memory>
#include <numbers>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using sketch::BoundaryDimension;
using sketch::BoundaryDimensionPlacement;
using sketch::Boundary;
using sketch::DocumentSnapshot;
using sketch::Entity;
using sketch::IdentifiedBoundary;
using sketch::Vec2;
using sketch::BoundaryAuthoringMode;
using sketch::desktop::BoundaryDraftPreview;
using sketch::desktop::MainWindow;
using sketch::desktop::PlanCanvas;
using sketch::desktop::Workspace;

void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

void process_events() {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
}

bool same_point(Vec2 left, Vec2 right) noexcept {
    return left.x == right.x && left.y == right.y;
}

bool same_boundary(const Boundary& left, const Boundary& right) noexcept {
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (!same_point(left[index].start, right[index].start) ||
            !same_point(left[index].end, right[index].end) ||
            left[index].sweep_radians != right[index].sweep_radians) {
            return false;
        }
    }
    return true;
}

bool same_optional_point(const std::optional<Vec2>& left,
                         const std::optional<Vec2>& right) noexcept {
    if (left.has_value() != right.has_value()) return false;
    return !left.has_value() || same_point(*left, *right);
}

PlanCanvas* canvas(MainWindow& window, const QString& object_name) {
    auto* widget = window.findChild<QWidget*>(object_name);
    auto* result = dynamic_cast<PlanCanvas*>(widget);
    require(result != nullptr, "named plan canvas must be available");
    return result;
}

void prepare_window(MainWindow& window) {
    // CTest supplies the offscreen platform. This attribute also keeps a
    // direct invocation from asking the desktop window manager for a surface.
    window.setAttribute(Qt::WA_DontShowOnScreen, true);
    window.resize(1200, 800);
    window.show();
    process_events();
    window.fitView();
    process_events();
    require(canvas(window, QStringLiteral("measurementPlanCanvas"))->width() > 300,
            "measurement canvas must receive a usable layout");
    require(canvas(window, QStringLiteral("architecturalPlanCanvas"))->width() > 300,
            "architectural canvas must receive a usable layout");
}

QPoint model_to_canvas(const PlanCanvas& canvas, Vec2 model) {
    // Every workflow fixture calls MainWindow::fitView while the canvases are
    // empty. PlanCanvas then has its documented origin-centred 80 px/metre
    // transform. Snapping makes the integer event coordinates exact on the
    // quarter-metre grid even when a canvas has an odd pixel dimension.
    const QRectF viewport(canvas.rect());
    return QPointF(viewport.center().x() + model.x * 80.0,
                   viewport.center().y() - model.y * 80.0)
        .toPoint();
}

Vec2 canvas_to_model(const PlanCanvas& canvas, QPoint point) {
    const QRectF viewport(canvas.rect());
    return {(static_cast<double>(point.x()) - viewport.center().x()) / 80.0,
            (viewport.center().y() - static_cast<double>(point.y())) / 80.0};
}

Vec2 quarter_snap(Vec2 point) {
    constexpr double grid = 0.25;
    return {std::round(point.x / grid) * grid, std::round(point.y / grid) * grid};
}

void send_click_at_screen(PlanCanvas& canvas, QPoint point) {
    require(canvas.rect().adjusted(1, 1, -1, -1).contains(point),
            "workflow fixture screen point must be inside the plan canvas");
    const QPointF local(point);
    QMouseEvent press(QEvent::MouseButtonPress, local, local, Qt::LeftButton,
                      Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&canvas, &press);
    QMouseEvent release(QEvent::MouseButtonRelease, local, local, Qt::LeftButton,
                        Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&canvas, &release);
    process_events();
}

void send_click(PlanCanvas& canvas, Vec2 model) {
    send_click_at_screen(canvas, model_to_canvas(canvas, model));
}

void send_drag(PlanCanvas& canvas, Vec2 start, Vec2 end,
               Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    const QPointF from(model_to_canvas(canvas, start));
    const QPointF to(model_to_canvas(canvas, end));
    QMouseEvent press(QEvent::MouseButtonPress, from, from, Qt::LeftButton,
                      Qt::LeftButton, modifiers);
    QApplication::sendEvent(&canvas, &press);
    QMouseEvent move(QEvent::MouseMove, to, to, Qt::NoButton,
                     Qt::LeftButton, modifiers);
    QApplication::sendEvent(&canvas, &move);
    QMouseEvent release(QEvent::MouseButtonRelease, to, to, Qt::LeftButton,
                        Qt::NoButton, modifiers);
    QApplication::sendEvent(&canvas, &release);
    process_events();
}

void send_move_at_screen(PlanCanvas& canvas, QPoint point) {
    require(canvas.rect().adjusted(1, 1, -1, -1).contains(point),
            "workflow fixture cursor point must be inside the plan canvas");
    const QPointF local(point);
    QMouseEvent move(QEvent::MouseMove, local, local, Qt::NoButton,
                     Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&canvas, &move);
    process_events();
}

void send_key(PlanCanvas& canvas, int key, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    canvas.setFocus();
    QKeyEvent press(QEvent::KeyPress, key, modifiers);
    QApplication::sendEvent(&canvas, &press);
    QKeyEvent release(QEvent::KeyRelease, key, modifiers);
    QApplication::sendEvent(&canvas, &release);
    process_events();
}

BoundaryDraftPreview preview(PlanCanvas& canvas) {
    const auto value = canvas.boundaryDraftPreview();
    require(value.has_value(), "boundary draft preview must be present");
    return *value;
}

void require_same_document(const DocumentSnapshot& before,
                           const DocumentSnapshot& after,
                           std::string_view message) {
    require(sketch::document_snapshot_digest(before) == sketch::document_snapshot_digest(after), message);
}

Entity committed_boundary(const DocumentSnapshot& snapshot) {
    std::optional<Entity> result;
    for (const auto& [id, entity] : snapshot.entities()) {
        (void)id;
        if (!sketch::can_recognize_boundary_entity_type(entity.type) ||
            !entity.properties.contains("boundary_model_version")) {
            continue;
        }
        require(!result.has_value(), "workflow fixture must create one boundary");
        result = entity;
    }
    require(result.has_value(), "workflow fixture must create an identified boundary");
    return *result;
}

std::vector<BoundaryDimension> committed_dimensions(const DocumentSnapshot& snapshot) {
    std::vector<BoundaryDimension> result;
    for (const auto& [id, entity] : snapshot.entities()) {
        (void)id;
        if (!sketch::can_recognize_boundary_dimension_entity_type(entity.type)) {
            continue;
        }
        const auto decoded = sketch::decode_boundary_dimension_entity(entity);
        require(decoded.supported() && decoded.dimension.has_value(),
                "workflow dimensions must use the supported v1 model");
        result.push_back(*decoded.dimension);
    }
    return result;
}

std::map<std::string, std::string, std::less<>> label_texts(const PlanCanvas& canvas) {
    std::map<std::string, std::string, std::less<>> result;
    for (const auto& label : canvas.labels()) {
        require(!label.id.isEmpty() && !label.text.isEmpty(),
                "committed dimension labels must retain an ID and text");
        require(result.emplace(label.id.toStdString(), label.text.toStdString()).second,
                "each dimension label ID must be unique");
    }
    return result;
}

void require_rectangle(const IdentifiedBoundary& boundary) {
    const std::array<Vec2, 4> starts{{{0.0, 0.0}, {2.0, 0.0}, {2.0, 2.0}, {0.0, 2.0}}};
    const std::array<Vec2, 4> ends{{{2.0, 0.0}, {2.0, 2.0}, {0.0, 2.0}, {0.0, 0.0}}};
    require(boundary.segments.size() == starts.size(),
            "event-created boundary must contain the four rectangle edges");
    for (std::size_t index = 0; index < starts.size(); ++index) {
        const auto& segment = boundary.segments[index].segment;
        require(segment.start.x == starts[index].x && segment.start.y == starts[index].y &&
                    segment.end.x == ends[index].x && segment.end.y == ends[index].y &&
                    segment.sweep_radians == 0.0,
                "event-created rectangle endpoints must remain exact");
    }
}

void inspect_committed_boundary(const DocumentSnapshot& snapshot,
                                const QString& expected_classification,
                                bool automatic_dimensions) {
    const auto entity = committed_boundary(snapshot);
    require(entity.type == "measurement_boundary",
            "boundary drawing must use the configured measurement boundary type");
    require(entity.properties.at("classification").is_string() &&
                entity.properties.at("classification").get<std::string>() ==
                    expected_classification.toStdString(),
            "boundary classification must remain independent from entity type");

    const auto boundary = sketch::decode_identified_boundary_entity(entity);
    require_rectangle(boundary);
    require(boundary.id == entity.id, "decoded boundary identity must match its entity");

    const auto& envelope = entity.properties.at("boundary_authoring");
    const auto decoded_receipts = sketch::decode_boundary_receipt_envelope(envelope);
    require(decoded_receipts.supported() && decoded_receipts.record.has_value(),
            "committed boundary must retain a supported construction envelope");
    require(decoded_receipts.record->schema_version ==
                sketch::boundary_receipt_schema_version_v2 &&
                decoded_receipts.record->replay_version == sketch::boundary_receipt_replay_version,
            "current interactive authoring must emit schema two replay-one receipts");
    require(decoded_receipts.record->boundary_id == boundary.id &&
                decoded_receipts.record->edges.size() == boundary.segments.size(),
            "receipt topology must retain the committed boundary identity and edge order");
    require(decoded_receipts.record->anchor.x == 0.0 &&
                decoded_receipts.record->anchor.y == 0.0,
            "receipt envelope must retain the captured anchor");
    for (std::size_t index = 0; index < boundary.segments.size(); ++index) {
        const auto& edge = decoded_receipts.record->edges[index];
        require(edge.segment_id == boundary.segments[index].segment_id &&
                    edge.start_vertex_id == boundary.segments[index].start_vertex_id &&
                    edge.end_vertex_id == boundary.segments[index].end_vertex_id &&
                    edge.receipt.segment_id == edge.segment_id,
                "receipt topology must match the stable boundary edge identities");
        if (index + 1 < boundary.segments.size()) {
            require(edge.receipt.kind == sketch::BoundaryConstructionKind::line_to_point &&
                        edge.receipt.chord_end.has_value() &&
                        !edge.receipt.distance.has_value() &&
                        !edge.receipt.heading.has_value(),
                    "clicked linework must use point-native schema two receipts");
        } else {
            require(edge.receipt.kind == sketch::BoundaryConstructionKind::line_closure &&
                        edge.receipt.closure_delta.has_value(),
                    "Enter closure must append a receipt-bearing closing edge");
        }
    }

    const auto dimensions = committed_dimensions(snapshot);
    require(dimensions.size() == boundary.segments.size(),
            "one committed dimension must be present for every boundary edge");
    std::set<std::string, std::less<>> segment_ids;
    for (const auto& dimension : dimensions) {
        require(dimension.boundary_id == boundary.id,
                "dimension must refer to its stable boundary ID");
        require(std::find_if(boundary.segments.begin(), boundary.segments.end(),
                             [&](const auto& edge) {
                                 return edge.segment_id == dimension.segment_id;
                             }) != boundary.segments.end(),
                "dimension must refer to a stable edge ID");
        require(segment_ids.insert(dimension.segment_id).second,
                "each edge must have exactly one committed dimension");
        if (automatic_dimensions) {
            require(dimension.placement == BoundaryDimensionPlacement::automatic &&
                        dimension.automatic_placement_version.has_value() &&
                        *dimension.automatic_placement_version == 2,
                    "Draw First dimensions must retain deterministic automatic placement");
        } else {
            require(dimension.placement == BoundaryDimensionPlacement::manual &&
                        !dimension.automatic_placement_version.has_value(),
                    "Define First dimensions must retain explicit manual placement");
        }
    }
}

void require_both_canvas_labels(MainWindow& window, std::size_t count) {
    const auto* measurement = canvas(window, QStringLiteral("measurementPlanCanvas"));
    const auto* architectural = canvas(window, QStringLiteral("architecturalPlanCanvas"));
    require(measurement->labels().size() == count && architectural->labels().size() == count,
            "committed dimension labels must appear in both workspace canvases");
    require(label_texts(*measurement) == label_texts(*architectural),
            "both workspace canvases must display the same dimension label values");
    const auto dimension_line_count = [](const PlanCanvas& value) {
        return static_cast<std::size_t>(std::count_if(
            value.entities().begin(), value.entities().end(), [](const auto& entity) {
                return entity.type == QStringLiteral("dimension_line");
            }));
    };
    require(dimension_line_count(*measurement) == count &&
                dimension_line_count(*architectural) == count,
            "committed straight dimensions must carry matching extension and dimension linework");
}

void test_draw_first_events_commit_receipts_labels_and_visibility() {
    MainWindow window;
    prepare_window(window);
    auto* measurement = canvas(window, QStringLiteral("measurementPlanCanvas"));
    const auto before = window.document().snapshot();
    require(window.beginBoundaryDrawing(BoundaryAuthoringMode::draw_first,
                                        QStringLiteral("living")),
            "Draw First must start with an explicit area classification");
    require(preview(*measurement).segments.empty(),
            "Draw First must begin with an empty draft");

    send_click(*measurement, {0.0, 0.0});
    const auto anchored = preview(*measurement);
    require(anchored.anchor.has_value() && anchored.anchor->x == 0.0 &&
                anchored.anchor->y == 0.0 && anchored.segments.empty(),
            "the first canvas click must capture the exact boundary anchor");
    send_click(*measurement, {2.0, 0.0});
    send_click(*measurement, {2.0, 2.0});
    send_click(*measurement, {0.0, 2.0});
    require(preview(*measurement).segments.size() == 3,
            "three measured clicks must retain three transient edges");

    send_key(*measurement, Qt::Key_Z, Qt::ControlModifier);
    require(preview(*measurement).segments.size() == 2,
            "Ctrl+Z on the canvas must undo one semantic draft edge");
    send_key(*measurement, Qt::Key_Y, Qt::ControlModifier);
    require(preview(*measurement).segments.size() == 3,
            "Ctrl+Y on the canvas must restore the same semantic draft edge");
    require_same_document(before, window.document().snapshot(),
                          "canvas draft undo and redo must not mutate the document");

    // Enter asks the session to append its explicit closing edge. The event
    // path therefore tests closure without clicking an already-degenerate
    // anchor point and without silently changing any prior endpoint.
    send_key(*measurement, Qt::Key_Return);
    require(!measurement->boundaryDraftPreview().has_value(),
            "successful Draw First Enter must finish the committed draft");
    const auto after = window.document().snapshot();
    require(after.revision() == before.revision() + 1 &&
                after.entities().size() == before.entities().size() + 5,
            "one Draw First boundary and four dimensions must be one document command");
    inspect_committed_boundary(after, QStringLiteral("living"), true);
    require(!window.selectedEntityId().isEmpty(),
            "committed boundary must become the selected entity");
    require_both_canvas_labels(window, 4);

    const auto imperial_labels = label_texts(*measurement);
    window.setMetricUnits(true);
    process_events();
    const auto metric_labels = label_texts(*measurement);
    require(metric_labels != imperial_labels,
            "switching metric units must update committed dimension label text");
    for (const auto& [id, text] : metric_labels) {
        (void)id;
        require(text.find("m") != std::string::npos,
                "metric committed dimension labels must use metre text");
    }
    require_both_canvas_labels(window, 4);

    const auto capture_directory = qEnvironmentVariable("SKETCH_BOUNDARY_WORKFLOW_CAPTURE_DIR");
    if (!capture_directory.isEmpty()) {
        require(QDir().mkpath(capture_directory), "workflow capture directory must be writable");
        window.fitView();
        process_events();
        require(window.grab().save(QDir(capture_directory).filePath("committed-boundary.png")),
                "committed boundary capture must be writable");
        const auto before_export = window.document().snapshot();
        require(window.exportDraftPdf(QDir(capture_directory).filePath("committed-boundary.pdf")),
                "event-created boundary must export through the actual PDF command");
        require_same_document(before_export, window.document().snapshot(),
                              "PDF export must not mutate the project");
    }

    QTemporaryDir directory;
    require(directory.isValid(), "boundary receipt persistence fixture needs a temporary directory");
    const auto path = directory.filePath(QStringLiteral("event-boundary.bldproj"));
    require(window.saveProjectAs(path) && QFileInfo::exists(path),
            "event-created boundary must save through the native project path");
    const auto saved = window.document().snapshot();
    require(window.openProject(path), "event-created boundary must reopen through the native project path");
    process_events();
    const auto reopened = window.document().snapshot();
    require(reopened.revision() == saved.revision() && reopened.entities() == saved.entities(),
            "reopening an event-created boundary must preserve its semantic document state");
    inspect_committed_boundary(reopened, QStringLiteral("living"), true);
    require_both_canvas_labels(window, 4);

    const auto layer_id = window.activeLayerId();
    require(!layer_id.isEmpty(), "saved boundary fixture must retain an active drawing layer");
    require(window.setContainerVisible(layer_id, false),
            "hiding the current drawing layer must be accepted");
    process_events();
    auto* architectural = canvas(window, QStringLiteral("architecturalPlanCanvas"));
    require(measurement->entities().empty() && architectural->entities().empty() &&
                measurement->labels().empty() && architectural->labels().empty(),
            "hiding the current layer must hide boundary geometry and dimensions in both canvases");
    window.showAllContainers();
    process_events();
    require(!measurement->entities().empty() && !architectural->entities().empty(),
            "showing all layers must restore boundary geometry in both canvases");
    require_both_canvas_labels(window, 4);
}

void test_define_first_events_place_manual_dimensions_and_close() {
    MainWindow window;
    prepare_window(window);
    auto* measurement = canvas(window, QStringLiteral("measurementPlanCanvas"));
    const auto before = window.document().snapshot();
    require(window.beginBoundaryDrawing(BoundaryAuthoringMode::define_first,
                                        QStringLiteral("garage")),
            "Define First must start with an explicit area classification");

    send_click(*measurement, {0.0, 0.0});
    send_click(*measurement, {2.0, 0.0});
    auto first_edge = preview(*measurement);
    require(first_edge.segments.size() == 1 && first_edge.labels.size() == 1 &&
                first_edge.instruction.contains(QStringLiteral("place this edge")),
            "Define First must enter a dimension phase after every clicked edge");
    send_click(*measurement, {1.0, 0.5});
    require(preview(*measurement).labels.size() == 1,
            "manual placement must resolve the first pending dimension");

    send_click(*measurement, {2.0, 2.0});
    send_click(*measurement, {1.5, 1.0});
    send_click(*measurement, {0.0, 2.0});
    send_click(*measurement, {1.0, 2.5});
    auto open_chain = preview(*measurement);
    require(open_chain.segments.size() == 3 && open_chain.labels.size() == 3,
            "Define First must retain three edges and three explicit placements");

    // Enter appends the closing edge and leaves its dimension pending. The
    // next click is its explicit manual text position; on placement the real
    // close-and-commit path accepts the chain.
    send_key(*measurement, Qt::Key_Return);
    auto closing_edge = preview(*measurement);
    require(closing_edge.segments.size() == 4 && closing_edge.labels.size() == 4 &&
                closing_edge.instruction.contains(QStringLiteral("place this edge")),
            "Define First Enter must append a receipt-bearing closing edge before commit");
    send_click(*measurement, {-0.5, 1.0});
    require(!measurement->boundaryDraftPreview().has_value(),
            "placing the closing dimension must finish Define First atomically");

    const auto after = window.document().snapshot();
    require(after.revision() == before.revision() + 1 &&
                after.entities().size() == before.entities().size() + 5,
            "Define First boundary and dimensions must commit as one document command");
    inspect_committed_boundary(after, QStringLiteral("garage"), false);
    require_both_canvas_labels(window, 4);
}

void test_workspace_switch_preserves_draft_and_uses_architectural_events() {
    MainWindow window;
    prepare_window(window);
    auto* measurement = canvas(window, QStringLiteral("measurementPlanCanvas"));
    auto* architectural = canvas(window, QStringLiteral("architecturalPlanCanvas"));
    const auto before = window.document().snapshot();
    require(window.beginBoundaryDrawing(BoundaryAuthoringMode::draw_first,
                                        QStringLiteral("porch")),
            "workspace draft fixture must start Draw First");
    send_click(*measurement, {0.0, 0.0});
    send_click(*measurement, {2.0, 0.0});
    const auto measurement_draft = preview(*measurement);
    require(measurement_draft.segments.size() == 1,
            "measurement canvas must own the initial live draft edge");

    window.setWorkspace(Workspace::architectural);
    process_events();
    require(window.workspace() == Workspace::architectural,
            "workspace switch must select the architectural workspace");
    const auto architectural_draft = preview(*architectural);
    require(same_boundary(architectural_draft.segments, measurement_draft.segments) &&
                same_optional_point(architectural_draft.anchor, measurement_draft.anchor),
            "switching workspaces must retain the same document-independent draft");
    require(same_boundary(preview(*measurement).segments, measurement_draft.segments),
            "the background measurement canvas must retain the live draft too");

    send_click(*architectural, {2.0, 2.0});
    send_click(*architectural, {0.0, 2.0});
    send_key(*architectural, Qt::Key_Return);
    require(!architectural->boundaryDraftPreview().has_value() &&
                !measurement->boundaryDraftPreview().has_value(),
            "architectural Enter must finish the retained shared draft");
    const auto after = window.document().snapshot();
    require(after.entities().size() == before.entities().size() + 5,
            "architectural event completion must create one boundary and four dimensions");
    inspect_committed_boundary(after, QStringLiteral("porch"), true);
    require_both_canvas_labels(window, 4);
}

void test_escape_cancels_without_document_mutation() {
    MainWindow window;
    prepare_window(window);
    auto* measurement = canvas(window, QStringLiteral("measurementPlanCanvas"));
    auto* architectural = canvas(window, QStringLiteral("architecturalPlanCanvas"));
    const auto before = window.document().snapshot();
    require(window.beginBoundaryDrawing(BoundaryAuthoringMode::draw_first,
                                        QStringLiteral("living")),
            "Escape fixture must start a boundary draft");
    send_click(*measurement, {0.0, 0.0});
    send_click(*measurement, {2.0, 0.0});
    require(measurement->boundaryDraftPreview().has_value(),
            "Escape fixture must have a live preview before cancellation");
    send_key(*measurement, Qt::Key_Escape);
    require(!measurement->boundaryDraftPreview().has_value() &&
                !architectural->boundaryDraftPreview().has_value(),
            "Escape must clear transient boundary previews in both workspaces");
    require(window.selectedEntityId().isEmpty(),
            "cancelling an uncommitted draft must not select an entity");
    require_same_document(before, window.document().snapshot(),
                          "Escape cancellation must leave the document unchanged");
}

bool discard_draft_for_layer(MainWindow& window, const QString& layer_id,
                             QMessageBox::StandardButton choice = QMessageBox::Discard,
                             std::function<void()> intervention = {}) {
    struct ModalState {
        bool saw_discard_prompt{};
        bool timed_out{};
    };
    const auto state = std::make_shared<ModalState>();
    QTimer poll;
    poll.setInterval(1);
    QObject::connect(&poll, &QTimer::timeout, &window, [state, choice, intervention] {
        auto* modal = QApplication::activeModalWidget();
        auto* message = qobject_cast<QMessageBox*>(modal);
        if (message == nullptr || state->saw_discard_prompt) return;
        state->saw_discard_prompt = true;
        if (intervention) intervention();
        auto* button = message->button(choice);
        require(button != nullptr, "draft confirmation must contain the requested button");
        button->click();
    });
    QTimer timeout;
    timeout.setSingleShot(true);
    timeout.setInterval(5000);
    QObject::connect(&timeout, &QTimer::timeout, &window, [state] {
        state->timed_out = true;
        for (auto* widget : QApplication::topLevelWidgets()) {
            if (auto* dialog = qobject_cast<QDialog*>(widget)) dialog->reject();
        }
    });
    poll.start();
    timeout.start();
    const auto changed = window.setActiveLayer(layer_id);
    poll.stop();
    timeout.stop();
    require(state->saw_discard_prompt && !state->timed_out,
            "context change must show a bounded discard confirmation");
    return changed;
}

void test_context_change_discards_draft_without_mutating_document() {
    MainWindow window;
    prepare_window(window);
    auto* measurement = canvas(window, QStringLiteral("measurementPlanCanvas"));
    const auto initial_layer = window.activeLayerId();
    const auto second_layer = window.createLayer(QStringLiteral("floor-1"),
                                                 QStringLiteral("Context target"));
    require(!second_layer.isEmpty() && second_layer != initial_layer,
            "context fixture must create a second drawing layer");
    require(window.setActiveLayer(initial_layer),
            "context fixture must restore its original drawing layer");
    const auto before = window.document().snapshot();

    require(window.beginBoundaryDrawing(BoundaryAuthoringMode::draw_first,
                                        QStringLiteral("living")),
            "context fixture must start a boundary draft");
    send_click(*measurement, {0.0, 0.0});
    send_click(*measurement, {2.0, 0.0});
    require(measurement->boundaryDraftPreview().has_value(),
            "context fixture must have a live draft before changing layer");
    const auto original_preview = preview(*measurement);
    require(!discard_draft_for_layer(window, second_layer, QMessageBox::Cancel),
            "cancelling the layer change must keep the draft");
    require(window.activeLayerId() == initial_layer &&
                same_boundary(preview(*measurement).segments, original_preview.segments),
            "cancelled layer change must preserve the original context and linework");
    require(!discard_draft_for_layer(window, second_layer, QMessageBox::Discard,
                [&] { require(window.undoCommand(), "queued draft undo must be available"); }),
            "discard approval must not discard a draft changed during the prompt");
    require(window.activeLayerId() == initial_layer && preview(*measurement).segments.empty(),
            "changed draft must remain available after stale discard rejection");
    require(window.redoCommand(), "draft edge must remain redoable after rejected discard");
    require(discard_draft_for_layer(window, second_layer),
            "discarding a boundary draft must permit a valid context change");
    process_events();
    require(window.activeLayerId() == second_layer,
            "context change must select the requested layer after discard");
    require(!measurement->boundaryDraftPreview().has_value() &&
                !canvas(window, QStringLiteral("architecturalPlanCanvas"))
                     ->boundaryDraftPreview().has_value(),
            "discarded context draft must be cleared from both canvases");
    require_same_document(before, window.document().snapshot(),
                          "discarding a draft during context change must not mutate the document");
}

void drive_boundary_modal(MainWindow& window, PlanCanvas& drawing, int key,
                          const std::function<void(QDialog*)>& respond) {
    bool responded = false;
    bool expired = false;
    QTimer poll;
    poll.setInterval(1);
    QObject::connect(&poll, &QTimer::timeout, &window, [&] {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if (!dialog || responded) return;
        responded = true;
        respond(dialog);
    });
    QTimer deadline;
    deadline.setSingleShot(true);
    QObject::connect(&deadline, &QTimer::timeout, &window, [&] {
        expired = true;
        if (auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget())) dialog->reject();
    });
    poll.start();
    deadline.start(5000);
    send_key(drawing, key);
    poll.stop();
    deadline.stop();
    require(responded && !expired, "keyboard command must reach its bounded native modal");
}

void test_precision_and_draw_first_classification_modals() {
    MainWindow window;
    prepare_window(window);
    window.setMetricUnits(true);
    auto* drawing = canvas(window, QStringLiteral("measurementPlanCanvas"));
    const auto original = window.document().snapshot();
    require(window.beginBoundaryDrawing(BoundaryAuthoringMode::draw_first),
            "Draw First must start before area classification");
    send_click(*drawing, {0, 0});
    drive_boundary_modal(window, *drawing, Qt::Key_D, [&](QDialog* modal) {
        auto* input = dynamic_cast<sketch::desktop::BoundaryInputDialog*>(modal);
        require(input != nullptr, "D must open the native precision segment form");
        input->findChild<QLineEdit*>("boundaryInputLength")->setText("2 m");
        input->findChild<QLineEdit*>("boundaryInputHeading")->setText("0 deg");
        require(input->submit(), "valid precise line must submit from the native form");
    });
    require(preview(*drawing).segments.size() == 1 &&
                same_point(preview(*drawing).segments.front().end, {2, 0}),
            "accepted precision form must update the live draft exactly");
    send_click(*drawing, {2, 2});
    send_click(*drawing, {0, 2});
    drive_boundary_modal(window, *drawing, Qt::Key_Return, [&](QDialog* modal) {
        require(qobject_cast<QInputDialog*>(modal) != nullptr,
                "closed Draw First area must request classification");
        modal->reject();
    });
    require_same_document(original, window.document().snapshot(),
                          "cancelling classification must preserve uncommitted linework");
    require(preview(*drawing).segments.size() == 4,
            "closed draft must remain available after classification cancellation");
    drive_boundary_modal(window, *drawing, Qt::Key_Return, [&](QDialog* modal) {
        auto* input = qobject_cast<QInputDialog*>(modal);
        require(input != nullptr, "Enter must reopen classification for the closed draft");
        require(window.undoCommand(), "classification prompt must retain local undo");
        input->setTextValue("living");
        input->accept();
    });
    require_same_document(original, window.document().snapshot(),
                          "changed draft must invalidate pending classification approval");
    drive_boundary_modal(window, *drawing, Qt::Key_Return, [&](QDialog* modal) {
        auto* input = qobject_cast<QInputDialog*>(modal);
        require(input != nullptr, "revised closed draft must request classification again");
        input->setTextValue("living");
        input->accept();
    });
    require(!drawing->boundaryDraftPreview(), "accepted classification must commit the live drawing");
    const auto boundary = committed_boundary(window.document().snapshot());
    require_rectangle(sketch::decode_identified_boundary_entity(boundary));
    require(boundary.properties.at("classification") == "living", "chosen classification must be retained");
    const auto receipt = sketch::decode_boundary_receipt_envelope(boundary.properties.at("boundary_authoring"));
    require(receipt.record->edges.front().receipt.kind == sketch::BoundaryConstructionKind::line_heading,
            "precision form must retain its exact input receipt through desktop commit");
    require_both_canvas_labels(window, 4);
}

void inspect_off_grid_point_commit(const DocumentSnapshot& before,
                                   const DocumentSnapshot& after,
                                   Vec2 expected_first_end) {
    require(after.revision() == before.revision() + 1 &&
                after.entities().size() == before.entities().size() + 5,
            "off-grid Draw First input must commit one boundary and four dimensions");
    const auto entity = committed_boundary(after);
    const auto boundary = sketch::decode_identified_boundary_entity(entity);
    require(boundary.segments.size() == 4,
            "off-grid rectangle fixture must retain four analytical edges");
    require(same_point(boundary.segments.front().segment.start, {0.0, 0.0}) &&
                same_point(boundary.segments.front().segment.end, expected_first_end),
            "snap-disabled click must retain the exact off-grid boundary endpoint");
    for (std::size_t index = 1; index < boundary.segments.size(); ++index) {
        require(same_point(boundary.segments[index - 1].segment.end,
                           boundary.segments[index].segment.start),
                "off-grid workflow edges must remain exactly joined");
    }
    require(same_point(boundary.segments.back().segment.end, {0.0, 0.0}),
            "off-grid workflow closure must return to its captured anchor");

    const auto receipt = sketch::decode_boundary_receipt_envelope(
        entity.properties.at("boundary_authoring"));
    require(receipt.supported() && receipt.record.has_value() &&
                receipt.record->schema_version == sketch::boundary_receipt_schema_version_v2 &&
                receipt.record->edges.size() == boundary.segments.size(),
            "off-grid workflow must retain a strict schema two construction envelope");
    const auto& first = receipt.record->edges.front().receipt;
    require(first.kind == sketch::BoundaryConstructionKind::line_to_point &&
                first.chord_end.has_value() && same_point(*first.chord_end, expected_first_end) &&
                !first.distance.has_value() && !first.heading.has_value(),
            "schema two point-native receipt must retain the exact off-grid endpoint only");
    require(receipt.record->edges.back().receipt.kind ==
                sketch::BoundaryConstructionKind::line_closure,
            "off-grid Enter must retain its explicit closing receipt");

    const auto dimensions = committed_dimensions(after);
    require(dimensions.size() == boundary.segments.size(),
            "off-grid workflow must retain one automatic dimension per edge");
    for (const auto& dimension : dimensions) {
        require(dimension.placement == BoundaryDimensionPlacement::automatic &&
                    dimension.automatic_placement_version.has_value() &&
                    *dimension.automatic_placement_version == 2,
                "off-grid Draw First dimensions must remain deterministic automatic placements");
    }
}

void test_off_grid_snap_cursor_rubberband_and_point_receipt_in_both_canvases() {
    MainWindow window;
    prepare_window(window);
    window.setMetricUnits(true);
    auto* measurement = canvas(window, QStringLiteral("measurementPlanCanvas"));
    auto* architectural = canvas(window, QStringLiteral("architecturalPlanCanvas"));
    const auto expected_dpr_text = qEnvironmentVariable("SKETCH_BOUNDARY_CANVAS_EXPECTED_DPR");
    if (!expected_dpr_text.isEmpty()) {
        bool parsed = false;
        const auto expected_dpr = expected_dpr_text.toDouble(&parsed);
        require(parsed && std::isfinite(expected_dpr) && expected_dpr > 0.0,
                "expected display scale must be finite and positive");
        require(std::abs(measurement->devicePixelRatioF() - expected_dpr) < 0.001,
                "workflow canvas display scale must match the requested capture scale");
    }
    require(window.beginBoundaryDrawing(BoundaryAuthoringMode::draw_first,
                                        QStringLiteral("living")),
            "off-grid fixture must start Draw First with an explicit classification");

    // Start at a snapped origin, then move to a deliberately off-grid pixel
    // coordinate. This exercises the effective cursor value before any edge
    // is committed and gives the rubber band and status bar the same oracle.
    send_click(*measurement, {0.0, 0.0});
    const auto anchor_screen = model_to_canvas(*measurement, {0.0, 0.0});
    const auto measurement_screen = anchor_screen + QPoint(37, 0);
    const auto measurement_raw = canvas_to_model(*measurement, measurement_screen);
    const auto measurement_snapped = quarter_snap(measurement_raw);
    require(!same_point(measurement_raw, measurement_snapped),
            "measurement off-grid cursor fixture must differ from its quarter-grid snap");

    send_move_at_screen(*measurement, measurement_screen);
    auto snapped_draft = preview(*measurement);
    require(snapped_draft.pen_position.has_value() &&
                same_point(*snapped_draft.pen_position, {0.0, 0.0}) &&
                snapped_draft.rubber_band.has_value() &&
                same_point(snapped_draft.rubber_band->start, {0.0, 0.0}) &&
                same_point(snapped_draft.rubber_band->end, measurement_snapped),
            "snap-enabled Draw First rubber band must use the effective snapped cursor");
    const auto snapped_status = window.statusBar()->currentMessage();
    require(snapped_status.contains(QStringLiteral("Cursor")) &&
                snapped_status.contains(QStringLiteral("500.0 mm")),
            "snap-enabled cursor status must report the snapped measurement coordinate");

    measurement->setSnapEnabled(false);
    process_events();
    auto unsnapped_draft = preview(*measurement);
    require(unsnapped_draft.rubber_band.has_value() &&
                same_point(unsnapped_draft.rubber_band->end, measurement_raw),
            "disabling snap must recompute the current rubber band without a mouse move");
    const auto unsnapped_status = window.statusBar()->currentMessage();
    require(unsnapped_status != snapped_status,
            "disabling snap must recompute the cursor status without a mouse move");

    measurement->setSnapEnabled(true);
    process_events();
    require(preview(*measurement).rubber_band.has_value() &&
                same_point(preview(*measurement).rubber_band->end, measurement_snapped) &&
                window.statusBar()->currentMessage() == snapped_status,
            "re-enabling snap must restore the effective cursor and status without a move");

    // Record a cursor on the second canvas before toggling either canvas. The
    // MainWindow owns one authoring pointer, so an inactive canvas refresh must
    // not overwrite the active workspace's effective point or status.
    window.setWorkspace(Workspace::architectural);
    process_events();
    const auto architectural_anchor_screen = model_to_canvas(*architectural, {0.0, 0.0});
    const auto architectural_screen = architectural_anchor_screen + QPoint(-31, -29);
    const auto architectural_raw = canvas_to_model(*architectural, architectural_screen);
    const auto architectural_snapped = quarter_snap(architectural_raw);
    require(!same_point(architectural_raw, architectural_snapped),
            "architectural off-grid cursor fixture must differ from its quarter-grid snap");
    send_move_at_screen(*architectural, architectural_screen);
    const auto architectural_draft = preview(*architectural);
    require(architectural_draft.rubber_band.has_value() &&
                same_point(architectural_draft.rubber_band->end, architectural_snapped),
            "architectural snap-enabled rubber band must use the snapped cursor");
    const auto architectural_status = window.statusBar()->currentMessage();
    require(architectural_status.contains(QStringLiteral("Architectural")),
            "architectural cursor movement must update the workspace status");

    // This is an inactive-canvas toggle. It updates that canvas's own cursor,
    // but must not feed a stale measurement point into the shared session.
    measurement->setSnapEnabled(false);
    process_events();
    const auto after_inactive_toggle = preview(*architectural);
    require(after_inactive_toggle.rubber_band.has_value() &&
                same_point(after_inactive_toggle.rubber_band->end, architectural_snapped) &&
                window.statusBar()->currentMessage() == architectural_status,
            "inactive canvas snap toggles must not overwrite the active cursor state");
    architectural->setSnapEnabled(false);
    process_events();
    require(preview(*architectural).rubber_band.has_value() &&
                same_point(preview(*architectural).rubber_band->end, architectural_raw) &&
                window.statusBar()->currentMessage() != architectural_status,
            "active canvas snap disable must recompute its cursor without a move");

    // Return to the measurement canvas, keep snap disabled there, and commit a
    // four-edge rectangle from direct screen events. The first endpoint is
    // intentionally off-grid, making the persisted point-native receipt an
    // independent oracle from the transient rubber band.
    window.setWorkspace(Workspace::measurement);
    process_events();
    measurement->setSnapEnabled(false);
    process_events();
    send_move_at_screen(*measurement, measurement_screen);
    require(preview(*measurement).rubber_band.has_value() &&
                same_point(preview(*measurement).rubber_band->end, measurement_raw),
            "measurement snap-disabled cursor must remain at the off-grid point");
    send_click_at_screen(*measurement, measurement_screen);
    auto first_edge = preview(*measurement);
    require(first_edge.segments.size() == 1 &&
                same_point(first_edge.segments.front().end, measurement_raw) &&
                !first_edge.rubber_band.has_value(),
            "snap-disabled Draw First click must persist the unsnapped endpoint in linework");
    const auto second_screen = measurement_screen + QPoint(0, -160);
    const auto third_screen = anchor_screen + QPoint(0, -160);
    send_click_at_screen(*measurement, second_screen);
    send_click_at_screen(*measurement, third_screen);
    const auto before_commit = window.document().snapshot();
    send_key(*measurement, Qt::Key_Return);
    require(!measurement->boundaryDraftPreview().has_value(),
            "off-grid Draw First Enter must finish the shared draft");
    inspect_off_grid_point_commit(before_commit, window.document().snapshot(), measurement_raw);
}

void test_off_grid_define_first_pending_dimension_preview_and_placement() {
    for (const auto workspace : {Workspace::measurement, Workspace::architectural}) {
        for (const auto snap_placement : {true, false}) {
            MainWindow window;
            prepare_window(window);
            window.setMetricUnits(true);
            window.setWorkspace(workspace);
            process_events();
            auto* active_canvas = canvas(window, workspace == Workspace::measurement
                ? QStringLiteral("measurementPlanCanvas") : QStringLiteral("architecturalPlanCanvas"));
            const auto before = window.document().snapshot();
            require(window.beginBoundaryDrawing(BoundaryAuthoringMode::define_first,
                                                QStringLiteral("garage")),
                    "off-grid Define First fixture must start with an explicit classification");

            const auto anchor_screen = model_to_canvas(*active_canvas, {0.0, 0.0});
            send_click_at_screen(*active_canvas, anchor_screen);
            const auto edge_screen = anchor_screen + QPoint(37, 0);
            const auto edge_raw = canvas_to_model(*active_canvas, edge_screen);
            const auto edge_snapped = quarter_snap(edge_raw);
            require(!same_point(edge_raw, edge_snapped),
                    "Define First edge fixture must differ from its quarter-grid snap");

            // The first move is still the endpoint rubber band. Clicking that point
            // records the edge and enters the explicit per-edge dimension phase.
            send_move_at_screen(*active_canvas, edge_screen);
            const auto endpoint_preview = preview(*active_canvas);
            require(endpoint_preview.rubber_band.has_value() &&
                        same_point(endpoint_preview.rubber_band->end, edge_snapped),
                    "Define First endpoint preview must use the effective snapped cursor");
            send_click_at_screen(*active_canvas, edge_screen);
            const auto pending = preview(*active_canvas);
            require(pending.segments.size() == 1 && pending.labels.size() == 1 &&
                        same_point(pending.labels.front().position, edge_snapped) &&
                        pending.instruction.contains(QStringLiteral("place this edge")),
                    "Define First must preview the pending edge dimension at the snapped endpoint");

            // Move the pointer without clicking. The pending label must follow the
            // effective cursor, and toggling snap later must do the same without a
            // second move event.
            const auto dimension_screen = anchor_screen + QPoint(49, -31);
            const auto dimension_raw = canvas_to_model(*active_canvas, dimension_screen);
            const auto dimension_snapped = quarter_snap(dimension_raw);
            require(!same_point(dimension_raw, dimension_snapped),
                    "Define First dimension fixture must differ from its quarter-grid snap");
            send_move_at_screen(*active_canvas, dimension_screen);
            const auto moved_pending = preview(*active_canvas);
            require(moved_pending.segments.size() == 1 && moved_pending.labels.size() == 1 &&
                        same_point(moved_pending.labels.front().position, dimension_snapped) &&
                        moved_pending.instruction.contains(QStringLiteral("place this edge")),
                    "pending dimension preview must follow an off-grid snapped cursor move");

            active_canvas->setSnapEnabled(false);
            process_events();
            const auto unsnapped_pending = preview(*active_canvas);
            require(unsnapped_pending.labels.size() == 1 &&
                        same_point(unsnapped_pending.labels.front().position, dimension_raw),
                    "disabling snap must recompute the pending dimension without a mouse move");
            active_canvas->setSnapEnabled(true);
            process_events();
            const auto restored_pending = preview(*active_canvas);
            require(restored_pending.labels.size() == 1 &&
                        same_point(restored_pending.labels.front().position, dimension_snapped),
                    "re-enabling snap must restore the pending dimension position without a move");

            active_canvas->setSnapEnabled(snap_placement);
            const auto expected_placement = snap_placement ? dimension_snapped : dimension_raw;
            require(same_point(preview(*active_canvas).labels.front().position, expected_placement),
                    "manual placement must start at the displayed effective cursor");
            send_click_at_screen(*active_canvas, dimension_screen);
            const auto placed = preview(*active_canvas);
            require(placed.segments.size() == 1 && placed.labels.size() == 1 &&
                        same_point(placed.labels.front().position, expected_placement) &&
                        placed.instruction.contains(QStringLiteral("Click to place each node")),
                    "Define First manual placement must exactly retain the displayed label position");
            require_same_document(before, window.document().snapshot(),
                                  "Define First preview and dimension placement must not mutate the document");
            send_key(*active_canvas, Qt::Key_Escape);
            require(!active_canvas->boundaryDraftPreview().has_value(),
            "Define First off-grid fixture must cancel cleanly after preview assertions");
        }
    }
}

void test_off_grid_snapped_commit_in_each_workspace() {
    for (const auto workspace : {Workspace::measurement, Workspace::architectural}) {
        MainWindow window;
        prepare_window(window);
        window.setWorkspace(workspace);
        process_events();
        auto* target = canvas(window, workspace == Workspace::measurement
            ? QStringLiteral("measurementPlanCanvas") : QStringLiteral("architecturalPlanCanvas"));
        require(window.beginBoundaryDrawing(BoundaryAuthoringMode::draw_first,
                                            QStringLiteral("living")),
                "snapped receipt fixture must start Draw First");
        const auto origin = model_to_canvas(*target, {0.0, 0.0});
        const auto endpoint = origin + QPoint(37, 0);
        const auto expected = quarter_snap(canvas_to_model(*target, endpoint));
        send_click_at_screen(*target, origin);
        send_move_at_screen(*target, endpoint);
        require(preview(*target).rubber_band &&
                    same_point(preview(*target).rubber_band->end, expected),
                "snapped endpoint must be visible before placing it");
        send_click_at_screen(*target, endpoint);
        send_click_at_screen(*target, endpoint + QPoint(0, -160));
        send_click_at_screen(*target, origin + QPoint(0, -160));
        const auto before = window.document().snapshot();
        send_key(*target, Qt::Key_Return);
        inspect_off_grid_point_commit(before, window.document().snapshot(), expected);
    }
}

void test_receipt_boundary_offset_copy() {
    MainWindow window;
    prepare_window(window);
    auto* target = canvas(window, QStringLiteral("measurementPlanCanvas"));
    require(window.beginBoundaryDrawing(BoundaryAuthoringMode::draw_first, "living"), "start copy fixture");
    send_click(*target, {0,0});
    send_click(*target, {2,0});
    send_click(*target, {2,2});
    send_click(*target, {0,2});
    send_key(*target, Qt::Key_Return);
    const auto source = window.document().snapshot();
    std::string boundary_id;
    for (const auto& [id, entity] : source.entities())
        if (entity.properties.contains("boundary_authoring")) boundary_id = id;
    require(!boundary_id.empty() && window.selectEntity(QString::fromStdString(boundary_id)), "select authored boundary");
    require(window.transformSelectedBoundary("0",false,false,"7 m","-2 m",true),
        "offset copy must preserve supported construction receipts");
    const auto copy_id = window.selectedEntityId().toStdString();
    const auto copied = window.document().snapshot();
    const auto& copy = copied.entities().at(copy_id);
    const auto original_record = sketch::decode_boundary_receipt_envelope(
        source.entities().at(boundary_id).properties.at("boundary_authoring"));
    const auto copy_record = sketch::decode_boundary_receipt_envelope(copy.properties.at("boundary_authoring"));
    const auto copy_replay = sketch::replay_boundary_construction(*copy_record.record);
    require(copy_record.supported() && copy_record.record->boundary_id == copy_id && copy_id != boundary_id &&
        copy_replay.anchor.x == original_record.record->anchor.x + 7 &&
        copy_replay.anchor.y == original_record.record->anchor.y - 2,
        "copied receipt owner and anchor must match the new boundary");
    std::size_t dimensions = 0;
    for (const auto& [id, entity] : copied.entities()) {
        if (source.entities().contains(id)) {
            require(source.entities().at(id) == entity, "offset copy must not mutate source entities");
            continue;
        }
        if (entity.type != "dimension") continue;
        const auto decoded = sketch::decode_boundary_dimension_entity(entity);
        require(decoded.supported() && decoded.dimension->boundary_id == copy_id,
            "copied dimensions must target the copied boundary");
        require(decoded.dimension->resolve(copy).segment_length() > 0, "copied dimension must resolve");
        const auto edge = std::find_if(copy_record.record->edges.begin(), copy_record.record->edges.end(),
            [&](const auto& item) { return item.segment_id == decoded.dimension->segment_id; });
        require(edge != copy_record.record->edges.end(), "copied label must have a receipt edge");
        const auto source_edge = original_record.record->edges.at(
            static_cast<std::size_t>(edge - copy_record.record->edges.begin())).segment_id;
        bool matched = false;
        for (const auto& [source_id, source_entity] : source.entities()) {
            if (source_entity.type != "dimension") continue;
            const auto source_dimension = sketch::decode_boundary_dimension_entity(source_entity);
            if (source_dimension.dimension->segment_id != source_edge) continue;
            require(decoded.dimension->text_position.x == source_dimension.dimension->text_position.x + 7 &&
                decoded.dimension->text_position.y == source_dimension.dimension->text_position.y - 2 &&
                decoded.dimension->placement == source_dimension.dimension->placement,
                "copy must translate label placement and retain placement mode");
            matched = true;
        }
        require(matched, "every copied dimension must originate from a source label");
        ++dimensions;
    }
    require(dimensions == 4, "offset copy must include all four dimension labels");
    require_both_canvas_labels(window, 8);
    require(window.undoCommand() && window.document().snapshot().entities() == source.entities() &&
        window.redoCommand() && window.document().snapshot().entities() == copied.entities(),
        "boundary and dimension copy must undo and redo atomically");
    QTemporaryDir directory;
    const auto path = directory.filePath("receipt-copy.bldproj");
    require(directory.isValid() && window.saveProjectAs(path) && window.openProject(path) &&
        window.document().snapshot().entities() == copied.entities(), "receipt copy must survive save/reopen exactly");
    require(window.selectEntity(QString::fromStdString(copy_id)) &&
        window.transformSelectedBoundary("0",false,false,"1 m","2 m",false),
        "receipt-backed boundary must translate in place through the typed command");
    const auto moved = window.document().snapshot();
    require(moved.history().back().boundary_translation.has_value() &&
        moved.history().back().boundary_translation->boundary_id == copy_id &&
        moved.entities().at(boundary_id) == source.entities().at(boundary_id),
        "in-place move must retain a typed history proof and preserve the other boundary");
    require(window.saveProjectAs(path) && window.openProject(path) &&
        window.document().snapshot().entities() == moved.entities() &&
        window.undoCommand() && window.document().snapshot().entities() == copied.entities() &&
        window.redoCommand() && window.document().snapshot().entities() == moved.entities(),
        "translated receipts and dimensions must save, reopen, undo, and redo exactly");
    require(window.selectEntity(QString::fromStdString(boundary_id)) &&
        window.transformSelectedBoundary("90",true,false,"8 m","0",true),
        "receipt-backed copy must support rotation and reflection without rewriting measurements");
    const auto rotated_id = window.selectedEntityId().toStdString();
    const auto rotated_state = window.document().snapshot();
    const auto& rotated_entity = rotated_state.entities().at(rotated_id);
    const auto rotated_record = sketch::decode_boundary_receipt_envelope(rotated_entity.properties.at("boundary_authoring"));
    require(rotated_record.supported() && rotated_record.record->schema_version == 3 &&
        rotated_record.record->transforms.size() == 1, "rotated receipt copy must record its coordinate transform");
    for (std::size_t i=0; i<original_record.record->edges.size(); ++i) {
        auto expected = original_record.record->edges[i].receipt;
        expected.segment_id = rotated_record.record->edges[i].segment_id;
        require(expected == rotated_record.record->edges[i].receipt,
            "rotation must preserve every original construction input except copied identity");
    }
    const auto rotated_boundary = sketch::decode_identified_boundary_entity(rotated_entity);
    require(std::abs(rotated_boundary.segments[0].segment.start.x-8)<1e-12 &&
        std::abs(rotated_boundary.segments[0].segment.start.y)<1e-12 &&
        std::abs(rotated_boundary.segments[0].segment.end.y-2)<1e-12,
        "rotated and reflected copy must have the expected world geometry");
    require_both_canvas_labels(window,12);
    require(window.transformSelectedBoundary("0",false,false,"0","3 m",false),
        "a transformed receipt copy must support subsequent in-place offsets");
    const auto shifted_rotated = window.document().snapshot();
    require(window.saveProjectAs(path) && window.openProject(path) &&
        window.document().snapshot().entities() == shifted_rotated.entities() &&
        window.undoCommand() && window.document().snapshot().entities() == rotated_state.entities(),
        "composed receipt transforms must survive storage and undo exactly");
    require(window.selectEntity(QString::fromStdString(boundary_id)) &&
        window.transformSelectedBoundary("90",true,false,"3 m","-1 m",false),
        "an original measured boundary must rotate and reflect in place");
    const auto in_place = window.document().snapshot();
    require(in_place.history().back().boundary_transform.has_value() &&
        !in_place.history().back().boundary_translation.has_value() &&
        in_place.history().back().boundary_transform->boundary_id == boundary_id &&
        window.selectedEntityId().toStdString() == boundary_id,
        "in-place rotation must preserve selection and retain its own transform proof");
    const auto in_place_record = sketch::decode_boundary_receipt_envelope(
        in_place.entities().at(boundary_id).properties.at("boundary_authoring"));
    require(in_place_record.record->edges == original_record.record->edges &&
        in_place_record.record->transforms.size() == 1,
        "in-place rotation must preserve original receipt inputs and topology identities");
    const auto operation = in_place.history().back().boundary_transform->transform;
    for (const auto& [id, before] : rotated_state.entities()) {
        if (id == boundary_id) continue;
        const auto& after = in_place.entities().at(id);
        if (before.type != "dimension" ||
            before.properties.at("target").at("entity_id") != boundary_id) {
            require(after == before, "in-place rotation must preserve unrelated objects");
            continue;
        }
        const auto before_dimension = sketch::decode_boundary_dimension_entity(before);
        const auto after_dimension = sketch::decode_boundary_dimension_entity(after);
        const auto expected = sketch::transform_point(before_dimension.dimension->text_position, operation);
        require(after_dimension.dimension->id == id &&
            after_dimension.dimension->text_position.x == expected.x &&
            after_dimension.dimension->text_position.y == expected.y &&
            after_dimension.dimension->segment_id == before_dimension.dimension->segment_id,
            "in-place rotation must transform dimension placement while retaining its identity and target");
    }
    require_both_canvas_labels(window,12);
    require(window.saveProjectAs(path) && window.openProject(path) &&
        window.document().snapshot().entities() == in_place.entities() &&
        window.undoCommand() && window.document().snapshot().entities() == rotated_state.entities() &&
        window.redoCommand() && window.document().snapshot().entities() == in_place.entities(),
        "in-place rotation and dimensions must save, reopen, undo and redo exactly");
}

void test_unified_pointer_clicks_to_draw_and_drags_to_pan() {
    {
        MainWindow window;
        prepare_window(window);
        auto* measurement = canvas(window, QStringLiteral("measurementPlanCanvas"));
        require(window.findChild<QWidget*>(QStringLiteral("selectTool")) == nullptr &&
                    window.findChild<QWidget*>(QStringLiteral("drawFirstBoundary")) == nullptr &&
                    window.findChild<QWidget*>(QStringLiteral("defineFirstBoundary")) == nullptr,
                "the primary canvas toolbar must not expose competing Select, Draw First, or Define First modes");

        const auto before = window.document().snapshot();
        send_click(*measurement, {0.0, 0.0});
        const auto anchored = preview(*measurement);
        require(anchored.anchor.has_value() && same_point(*anchored.anchor, {0.0, 0.0}) &&
                    anchored.segments.empty(),
                "the first empty-canvas click must start a measured boundary on the unified pointer surface");
        send_click(*measurement, {2.0, 0.0});
        const auto draft = preview(*measurement);
        require(draft.segments.size() == 1 &&
                    same_point(draft.segments.front().start, {0.0, 0.0}) &&
                    same_point(draft.segments.front().end, {2.0, 0.0}),
                "the next click must drop a node and retain the measured edge");
        const auto before_pan = measurement->viewCenter();
        send_drag(*measurement, {-2.0, -2.0}, {-1.0, -1.0});
        const auto after_pan = measurement->viewCenter();
        require(after_pan.x != before_pan.x && after_pan.y != before_pan.y &&
                    preview(*measurement).segments.size() == 1,
                "empty-canvas drag must pan without adding a boundary node");
        require_same_document(before, window.document().snapshot(),
                              "an open click-authored boundary must remain a transient draft");

        send_key(*measurement, Qt::Key_Escape);
        require(!measurement->boundaryDraftPreview().has_value(),
                "Escape must cancel the temporary unified-pointer draft");
        require(window.beginBoundaryDrawing(BoundaryAuthoringMode::draw_first,
                                            QStringLiteral("living")),
                "close-node fixture must start with a known classification");
        send_click(*measurement, {0.0, 0.0});
        send_click(*measurement, {2.0, 0.0});
        send_click(*measurement, {2.0, 2.0});
        send_click(*measurement, {0.0, 2.0});
        send_click(*measurement, {0.0, 0.0});
        require(!measurement->boundaryDraftPreview().has_value() &&
                    window.document().snapshot().revision() == before.revision() + 1,
                "clicking the highlighted first node must close and commit the measured area");
    }

    {
        MainWindow window;
        prepare_window(window);
        window.setWorkspace(Workspace::architectural);
        process_events();
        auto* architectural = canvas(window, QStringLiteral("architecturalPlanCanvas"));
        const auto wall_before = window.document().snapshot();
        send_click(*architectural, {-2.0, -1.0});
        send_click(*architectural, {2.0, -1.0});
        const auto wall_after = window.document().snapshot();
        require(wall_after.revision() == wall_before.revision() + 1,
                "two empty-canvas clicks in the architectural workspace must commit one wall command");
        const auto wall_count = static_cast<std::size_t>(std::count_if(
            wall_after.entities().begin(), wall_after.entities().end(), [](const auto& entry) {
                return entry.second.type == "wall";
            }));
        require(wall_count == 1,
                "architectural click drawing must create a real wall rather than a canvas-only stroke");
    }
}

void test_dimension_presentation_editing() {
    MainWindow window;
    prepare_window(window);
    auto* target = canvas(window, QStringLiteral("measurementPlanCanvas"));
    require(window.beginBoundaryDrawing(BoundaryAuthoringMode::draw_first, "living"), "start dimension fixture");
    for (const auto point : {Vec2{0,0}, Vec2{2,0}, Vec2{2,2}, Vec2{0,2}}) send_click(*target, point);
    send_key(*target, Qt::Key_Return);
    const auto source = window.document().snapshot();
    std::string id;
    for (const auto& [candidate, entity] : source.entities()) if (entity.type == "dimension") { id = candidate; break; }
    require(!id.empty(), "drawing must create semantic dimensions");
    const auto original = *sketch::decode_boundary_dimension_entity(source.entities().at(id)).dimension;
    const auto dimension_id = QString::fromStdString(id);
    const auto x = QString::number(original.text_position.x, 'g', 17) + " m";
    const auto y = QString::number(original.text_position.y, 'g', 17) + " m";
    require(window.selectEntity(dimension_id) &&
        window.editBoundaryDimension(dimension_id,x,y,"4","#bb2244",true,true,true,"30"),
        "dimension presentation must be editable without changing its measured geometry");
    const auto styled = window.document().snapshot();
    const auto styled_dimension = *sketch::decode_boundary_dimension_entity(styled.entities().at(id)).dimension;
    require(styled_dimension.presentation && styled_dimension.presentation->text_height_mm == 4 &&
        styled_dimension.presentation->bold && styled_dimension.presentation->italic &&
        styled_dimension.placement == original.placement &&
        styled_dimension.automatic_placement_version == original.automatic_placement_version &&
        styled_dimension.boundary_id == original.boundary_id && styled_dimension.segment_id == original.segment_id &&
        same_point(styled_dimension.text_position, original.text_position),
        "style-only edits must retain placement origin, exact position and stable references");
    for (const auto& [other_id, entity] : source.entities())
        if (other_id != id) require(styled.entities().at(other_id) == entity, "dimension style must not change other entities");
    for (const auto name : {"measurementPlanCanvas", "architecturalPlanCanvas"}) {
        const auto& labels = canvas(window, QString::fromLatin1(name))->labels();
        const auto label = std::find_if(labels.begin(), labels.end(), [&](const auto& item) { return item.id == dimension_id; });
        require(label != labels.end() && label->paper_height_mm == 4 && label->bold && label->italic &&
            label->color == QColor("#bb2244") && std::abs(label->rotation_radians - std::numbers::pi/6) < 1e-12,
            "both canvases must receive paper-space style and rotation");
    }
    auto* position_x = window.findChild<QLineEdit*>("dimensionPositionX");
    auto* position_y = window.findChild<QLineEdit*>("dimensionPositionY");
    auto* apply = window.findChild<QAbstractButton*>("applyBoundaryDimension");
    require(position_x && position_y && apply && apply->isEnabled(), "dimension inspector controls must be available");
    position_x->setText("2 m"); position_y->setText("-1 m");
    apply->click(); process_events();
    const auto shown = window.document().snapshot();
    const auto placed = *sketch::decode_boundary_dimension_entity(shown.entities().at(id)).dimension;
    require(same_point(placed.text_position,{2,-1}) && placed.placement == BoundaryDimensionPlacement::manual &&
        !placed.automatic_placement_version && placed.presentation == styled_dimension.presentation,
        "inspector placement edit must retain style and mark the position manual");
    const auto unchanged = sketch::document_snapshot_digest(shown);
    require(!window.editBoundaryDimension(dimension_id,"2 m","-1 m","0","#bb2244",true,true,true,"30") &&
        !window.editBoundaryDimension(dimension_id,"2 m","-1 m","4","bad",true,true,true,"30") &&
        sketch::document_snapshot_digest(window.document().snapshot()) == unchanged,
        "invalid dimension styles must reject atomically");
    require(window.editBoundaryDimension(dimension_id,"2 m","-1 m","4","#bb2244",true,true,false,"30"),
        "individual dimensions must be hideable");
    const auto hidden = window.document().snapshot();
    const auto hidden_dimension = *sketch::decode_boundary_dimension_entity(hidden.entities().at(id)).dimension;
    require_both_canvas_labels(window,3);
    QTemporaryDir directory;
    const auto path = directory.filePath("styled-dimensions.bldproj");
    require(directory.isValid() && window.saveProjectAs(path) && window.openProject(path) &&
        window.document().snapshot().entities() == hidden.entities(), "dimension style and visibility must survive save/reopen");
    require_both_canvas_labels(window,3);
    require(window.undoCommand() && window.document().snapshot().entities() == shown.entities(),
        "visibility change must undo after reopening");
    require_both_canvas_labels(window,4);
    require(window.redoCommand() && window.document().snapshot().entities() == hidden.entities(),
        "visibility change must redo exactly");
    require(window.selectEntity(QString::fromStdString(original.boundary_id)) &&
        window.transformSelectedBoundary("90",false,true,"0","0",false),
        "styled dimensions must remain attached during a boundary transform");
    const auto transformed = window.document().snapshot();
    const auto after_transform = *sketch::decode_boundary_dimension_entity(transformed.entities().at(id)).dimension;
    require(after_transform.presentation == hidden_dimension.presentation,
        "boundary transforms must retain the complete dimension presentation");
    require(!after_transform.presentation->visible && after_transform.boundary_id == original.boundary_id &&
        after_transform.segment_id == original.segment_id && after_transform.presentation->text_height_mm == 4,
        "transforms must preserve hidden style and stable dimension targets");
    require(window.selectEntity(dimension_id), "hidden dimensions must remain selectable for editing");
    process_events();
    const auto capture = qEnvironmentVariable("SKETCH_DIMENSION_CAPTURE");
    if (!capture.isEmpty()) {
        auto* group = window.findChild<QWidget*>("dimensionProperties");
        require(group && group->grab().save(capture), "dimension inspector screenshot must save");
    }
}

void test_semantic_angle_and_area_dimension_creation() {
    MainWindow window;
    prepare_window(window);
    auto* target = canvas(window, QStringLiteral("measurementPlanCanvas"));
    require(window.beginBoundaryDrawing(BoundaryAuthoringMode::draw_first, "living"),
            "start semantic dimension fixture");
    for (const auto point : {Vec2{0, 0}, Vec2{2, 0}, Vec2{2, 2}, Vec2{0, 2}})
        send_click(*target, point);
    send_key(*target, Qt::Key_Return);
    const auto before = window.document().snapshot();
    const auto boundary_entity = committed_boundary(before);
    const auto boundary = sketch::decode_identified_boundary_entity(boundary_entity);
    require(boundary.segments.size() == 4, "semantic dimension fixture must be rectangular");
    window.setMetricUnits(true);
    const auto angle_id = window.createAngleDimension(
        QString::fromStdString(boundary.id),
        QString::fromStdString(boundary.segments[0].segment_id),
        QString::fromStdString(boundary.segments[3].segment_id),
        QString::fromStdString(boundary.segments[0].start_vertex_id), {0.75, 0.75});
    const auto area_id = window.createAreaDimension(QString::fromStdString(boundary.id), {1.0, 1.0});
    require(!angle_id.isEmpty() && !area_id.isEmpty() && angle_id != area_id,
            "semantic angle and area dimensions must be creatable");
    const auto after = window.document().snapshot();
    const auto angle = sketch::decode_boundary_dimension_entity(after.entities().at(angle_id.toStdString()));
    const auto area = sketch::decode_boundary_dimension_entity(after.entities().at(area_id.toStdString()));
    require(angle.supported() && area.supported() &&
                angle.dimension->kind == sketch::BoundaryDimensionKind::angle &&
                area.dimension->kind == sketch::BoundaryDimensionKind::area,
            "created dimensions must retain semantic kinds");
    const auto resolved_angle = angle.dimension->resolve(after.entities().at(boundary.id));
    const auto resolved_area = area.dimension->resolve(after.entities().at(boundary.id));
    require(std::abs(resolved_angle.angle_radians - std::numbers::pi / 2.0) < 1e-12 &&
                std::abs(resolved_area.area_square_metres - 4.0) < 1e-12,
            "created semantic dimensions must resolve current geometry");
    const auto labels = label_texts(*target);
    require(labels.at(angle_id.toStdString()) == "90.0°" &&
                labels.at(area_id.toStdString()) == "4.00 m²",
            "semantic dimension labels must use the active unit system");
    const auto angle_line_count = static_cast<std::size_t>(std::count_if(
        target->entities().begin(), target->entities().end(), [](const auto& entity) {
            return entity.type == QStringLiteral("dimension_line");
        }));
    require(angle_line_count == 5,
            "angle dimensions must add one semantic overlay while areas remain label-only");
    require(window.undoCommand() && window.redoCommand(),
            "semantic dimension creation must use normal undo and redo history");
}

void install_test_font() {
    const auto font_id = QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/Inter.ttf"));
    require(font_id >= 0, "boundary workflow test must load the bundled Inter font");
    const auto families = QFontDatabase::applicationFontFamilies(font_id);
    require(!families.isEmpty(), "bundled workflow font must expose a family");
    QApplication::setFont(QFont(families.front(), 10));
}

}  // namespace

int main(int argc, char** argv) {
    sketch::testing::noninteractive_errors();
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    try {
        install_test_font();
        test_unified_pointer_clicks_to_draw_and_drags_to_pan();
        test_draw_first_events_commit_receipts_labels_and_visibility();
        test_define_first_events_place_manual_dimensions_and_close();
        test_workspace_switch_preserves_draft_and_uses_architectural_events();
        test_escape_cancels_without_document_mutation();
        test_context_change_discards_draft_without_mutating_document();
        test_precision_and_draw_first_classification_modals();
        test_off_grid_snap_cursor_rubberband_and_point_receipt_in_both_canvases();
        test_off_grid_define_first_pending_dimension_preview_and_placement();
        test_off_grid_snapped_commit_in_each_workspace();
        test_receipt_boundary_offset_copy();
        test_dimension_presentation_editing();
        test_semantic_angle_and_area_dimension_creation();
        std::cout << "Boundary workflow event tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "boundary_workflow_tests: " << error.what() << '\n';
        return 1;
    }
}
