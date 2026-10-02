#include "sketch/desktop/main_window.hpp"

#include "sketch/boundary_dimension.hpp"
#include "sketch/boundary_entity.hpp"
#include "sketch/boundary_receipt.hpp"
#include "sketch/document_digest.hpp"
#include "sketch/project_store.hpp"
#include "sketch/desktop/boundary_input_dialog.hpp"
#include "sketch/annotation_entity_codec.hpp"
#include "sketch/text_library.hpp"
#include "support/noninteractive_errors.hpp"
#include "../src/desktop/plan_canvas.hpp"

#include <QApplication>
#include <QAction>
#include <QTabWidget>
#include <QAbstractButton>
#include <QCoreApplication>
#include <QComboBox>
#include <QCloseEvent>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
#include <QFont>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QPainterPath>
#include <QPixmap>
#include <QPushButton>
#include <QKeyEvent>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMouseEvent>
#include <QRectF>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolButton>

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
    const QRectF viewport(canvas.rect());
    const auto center = canvas.viewCenter();
    return QPointF(viewport.center().x() + (model.x - center.x) * canvas.viewScale(),
                   viewport.center().y() - (model.y - center.y) * canvas.viewScale())
        .toPoint();
}

Vec2 canvas_to_model(const PlanCanvas& canvas, QPoint point) {
    const QRectF viewport(canvas.rect());
    const auto center = canvas.viewCenter();
    return {center.x + (static_cast<double>(point.x()) - viewport.center().x()) / canvas.viewScale(),
            center.y + (viewport.center().y() - static_cast<double>(point.y())) / canvas.viewScale()};
}

Vec2 snap_to_known_grid(Vec2 point, double grid) {
    // Independent fixture oracle: at the empty-view 80 px/metre zoom the
    // metric minor grid is 20 cm and the imperial minor grid is one foot.
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

std::map<std::string, std::string, std::less<>> straight_dimension_label_texts(
    const PlanCanvas& canvas) {
    std::set<QString> ids;
    for (const auto& entity : canvas.entities()) {
        if (entity.type == QStringLiteral("dimension_line")) ids.insert(entity.id);
    }
    std::map<std::string, std::string, std::less<>> result;
    for (const auto& label : canvas.labels()) {
        if (ids.contains(label.id)) {
            result.emplace(label.id.toStdString(), label.text.toStdString());
        }
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
    const auto dimension_ids = [](const PlanCanvas& value) {
        std::set<QString> ids;
        for (const auto& entity : value.entities()) {
            if (entity.type == QStringLiteral("dimension_line")) ids.insert(entity.id);
        }
        return ids;
    };
    const auto dimension_label_texts = [](const PlanCanvas& value,
                                          const std::set<QString>& ids) {
        std::multiset<QString> texts;
        for (const auto& label : value.labels()) {
            if (ids.contains(label.id)) texts.insert(label.text);
        }
        return texts;
    };
    const auto measurement_ids = dimension_ids(*measurement);
    const auto architectural_ids = dimension_ids(*architectural);
    require(dimension_label_texts(*measurement, measurement_ids).size() == count &&
                dimension_label_texts(*architectural, architectural_ids).size() == count,
            "committed dimension labels must appear in both workspace canvases");
    require(dimension_label_texts(*measurement, measurement_ids) ==
                dimension_label_texts(*architectural, architectural_ids),
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
    window.setMetricUnits(true); // The exact two-metre rectangle lies on the 20 cm grid.
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

    window.setMetricUnits(false);
    process_events();
    const auto imperial_labels = straight_dimension_label_texts(*measurement);
    window.setMetricUnits(true);
    process_events();
    const auto metric_labels = straight_dimension_label_texts(*measurement);
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
    window.setMetricUnits(true); // The exact two-metre rectangle lies on the 20 cm grid.
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

void test_workspace_switch_preserves_draft_on_refusal_and_allows_finished_drawing() {
    MainWindow window;
    prepare_window(window);
    window.setMetricUnits(true); // The exact two-metre rectangle lies on the 20 cm grid.
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
    auto* mode2d = window.findChild<QAction*>(QStringLiteral("workspace2D"));
    auto* mode3d = window.findChild<QAction*>(QStringLiteral("workspace3D"));
    auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("workspaceTabs"));
    require(mode2d && mode3d && tabs, "workspace transition fixture needs the real mode controls");
    const auto require_retained = [&] {
        const auto retained = preview(*measurement);
        require(window.workspace() == Workspace::measurement && tabs->currentIndex() == 0 &&
                    mode2d->isChecked() && !mode3d->isChecked() &&
                    same_boundary(retained.segments, measurement_draft.segments) &&
                    same_optional_point(retained.anchor, measurement_draft.anchor) &&
                    window.document().snapshot().entities() == before.entities(),
                "refused view switches must restore the visible mode controls and preserve the exact unfinished draft");
    };
    require_retained();
    mode3d->trigger();
    process_events();
    require_retained();
    tabs->setCurrentIndex(1);
    process_events();
    require_retained();
    // A projected architectural view is not a world-XY authoring surface.
    // Finish the retained draft on its conventional 2D canvas before viewing 3D.
    send_click(*measurement, {2.0, 2.0});
    send_click(*measurement, {0.0, 2.0});
    send_key(*measurement, Qt::Key_Return);
    require(!architectural->boundaryDraftPreview().has_value() &&
                !measurement->boundaryDraftPreview().has_value(),
            "2D Enter must finish the retained draft");
    const auto after = window.document().snapshot();
    require(after.entities().size() == before.entities().size() + 5,
            "2D event completion must create one boundary and four dimensions");
    inspect_committed_boundary(after, QStringLiteral("porch"), true);
    require_both_canvas_labels(window, 4);
    mode3d->trigger();
    process_events();
    require(window.workspace() == Workspace::architectural && tabs->currentIndex() == 1 &&
                !mode2d->isChecked() && mode3d->isChecked() &&
                window.document().snapshot().entities() == after.entities(),
            "completed drawings must switch into 3D with consistent controls and unchanged geometry");
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
                          const std::function<void(QDialog*)>& respond,
                          std::string_view failure = "keyboard command must reach its bounded native modal",
                          Qt::KeyboardModifiers modifiers = Qt::NoModifier,
                          const std::function<void()>& invoke = {}) {
    bool responded = false;
    bool expired = false;
    bool precision_modal = false;
    QTimer poll;
    poll.setInterval(1);
    QObject::connect(&poll, &QTimer::timeout, &window, [&] {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if (!dialog || responded) return;
        responded = true;
        precision_modal=dynamic_cast<sketch::desktop::BoundaryInputDialog*>(dialog)!=nullptr;
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
    if (invoke) invoke();
    else send_key(drawing, key,modifiers);
    poll.stop();
    deadline.stop();
    require(responded && !expired, failure);
    if (precision_modal)
        require(window.focusWidget()==&drawing,"precision form must restore stored focus to its invoking canvas");
    // The offscreen platform has no window manager to reactivate the parent
    // after a modal closes. Reactivation must use the product's stored target;
    // the fixture deliberately does not call drawing.setFocus() here.
    QApplication::setActiveWindow(&window);
    process_events();
}

void test_keyboard_only_boundary_authoring() {
    const auto accept_by_keyboard=[](sketch::desktop::BoundaryInputDialog* input) {
        QKeyEvent enter(QEvent::KeyPress,Qt::Key_Return,Qt::NoModifier);
        QApplication::sendEvent(input,&enter);
        require(input->result()==QDialog::Accepted && input->candidate(),"Enter must accept the valid native precision form");
    };
    for (const auto mode : {BoundaryAuthoringMode::draw_first,BoundaryAuthoringMode::define_first}) {
        MainWindow window; prepare_window(window); window.setMetricUnits(false);
        QApplication::setActiveWindow(&window); process_events();
        auto* drawing=canvas(window,QStringLiteral("measurementPlanCanvas"));
        const auto before=window.document().snapshot();
        if (mode==BoundaryAuthoringMode::draw_first) {
            drive_boundary_modal(window,*drawing,Qt::Key_K,[&](QDialog* modal) {
                require(modal->windowTitle()=="Command search","Ctrl+K must open command search");
                auto* search=modal->findChild<QLineEdit*>(); require(search,"command search field missing");
                search->setText("Start measured boundary with point input");
                QKeyEvent enter(QEvent::KeyPress,Qt::Key_Return,Qt::NoModifier);
                QApplication::sendEvent(search,&enter);
            },"Ctrl+K must expose the actual Draw First start command",Qt::ControlModifier);
        } else {
            drive_boundary_modal(window,*drawing,Qt::Key_D,[&](QDialog* modal) {
                auto* classification=qobject_cast<QInputDialog*>(modal);
                require(classification,"Define First shortcut must request classification");
                classification->setTextValue("living"); classification->accept();
            },"Ctrl+Shift+D must start Define First through actual keyboard UI",Qt::ControlModifier|Qt::ShiftModifier);
        }
        drive_boundary_modal(window,*drawing,Qt::Key_D,[&](QDialog* modal) {
            auto* input=dynamic_cast<sketch::desktop::BoundaryInputDialog*>(modal);
            require(input!=nullptr,"D must open native keyboard anchor form");
            auto* x=input->findChild<QLineEdit*>("boundaryInputEndX");
            auto* y=input->findChild<QLineEdit*>("boundaryInputEndY");
            require(x && y,"keyboard anchor must expose unit-aware coordinates");
            x->setText("invalid"); y->setText("0 ft");
            require(!input->submit() && !input->candidate(),"invalid keyboard anchor must remain atomic");
            x->setText("0 ft"); accept_by_keyboard(input);
        },"D must allow an exact keyboard anchor without pointer input");
        require(drawing->hasFocus(),"accepted precision input must restore focus to its canvas");
        require(preview(*drawing).segments.empty(),"keyboard anchor must not create an edge");
        const std::array<const char*,3> lengths{"12 ft","8 ft","12 ft"};
        const std::array<const char*,3> headings{"0 deg","90 deg","180 deg"};
        const auto place_dimension=[&](const char* x,const char* y) {
            drive_boundary_modal(window,*drawing,Qt::Key_D,[&](QDialog* modal) {
                auto* input=dynamic_cast<sketch::desktop::BoundaryInputDialog*>(modal);
                require(input!=nullptr,"D must open pending dimension placement");
                input->findChild<QLineEdit*>("boundaryInputEndX")->setText(x);
                input->findChild<QLineEdit*>("boundaryInputEndY")->setText(y);
                accept_by_keyboard(input);
            });
        };
        for (std::size_t index=0;index<lengths.size();++index) {
            drive_boundary_modal(window,*drawing,Qt::Key_D,[&](QDialog* modal) {
                auto* input=dynamic_cast<sketch::desktop::BoundaryInputDialog*>(modal);
                require(input!=nullptr,"D must open analytical segment form");
                if (index>0) require(input->findChild<QLineEdit*>("boundaryInputLength")->text()==lengths[index-1],
                    "successful precision input must remain available on re-entry");
                input->findChild<QLineEdit*>("boundaryInputLength")->setText(lengths[index]);
                input->findChild<QLineEdit*>("boundaryInputHeading")->setText(headings[index]);
                accept_by_keyboard(input);
            });
            require(preview(*drawing).segments.size()==index+1,"keyboard edge count differs");
            if (index==0) {
                send_key(*drawing,Qt::Key_Z,Qt::ControlModifier);
                require(preview(*drawing).segments.empty(),"keyboard draft undo must remove edge without committing");
                send_key(*drawing,Qt::Key_Y,Qt::ControlModifier);
                require(preview(*drawing).segments.size()==1,"keyboard draft redo must restore edge");
            }
            if (mode==BoundaryAuthoringMode::define_first)
                place_dimension(index==1 ? "13 ft" : "6 ft",index==0 ? "-1 ft" : "9 ft");
            if (index==0) {
                const auto draft=preview(*drawing);
                drive_boundary_modal(window,*drawing,Qt::Key_D,[&](QDialog* modal) {
                    auto* input=dynamic_cast<sketch::desktop::BoundaryInputDialog*>(modal);
                    input->findChild<QLineEdit*>("boundaryInputLength")->setText("999 ft");
                    QKeyEvent escape(QEvent::KeyPress,Qt::Key_Escape,Qt::NoModifier);
                    QApplication::sendEvent(input,&escape);
                });
                require(same_boundary(preview(*drawing).segments,draft.segments),"cancelled precision dialog changed draft");
                require(drawing->hasFocus(),"Escape must restore focus to the same canvas");
                window.setMetricUnits(true);
                drive_boundary_modal(window,*drawing,Qt::Key_D,[&](QDialog* modal) {
                    auto* input=dynamic_cast<sketch::desktop::BoundaryInputDialog*>(modal);
                    require(input->findChild<QLineEdit*>("boundaryInputLength")->text()=="1 m",
                        "unit change must reset retained measurements instead of reinterpreting them");
                    input->reject();
                });
                window.setMetricUnits(false);
            }
        }
        require_same_document(before,window.document().snapshot(),"keyboard draft operations changed Document");
        if (mode==BoundaryAuthoringMode::draw_first) {
            drive_boundary_modal(window,*drawing,Qt::Key_Return,[&](QDialog* modal) {
                auto* classification=qobject_cast<QInputDialog*>(modal); require(classification,"Draw First finish must classify area");
                classification->setTextValue("living"); classification->accept();
            });
        } else send_key(*drawing,Qt::Key_Return);
        if (mode==BoundaryAuthoringMode::define_first) {
            place_dimension("-1 ft","4 ft"); send_key(*drawing,Qt::Key_Return);
        }
        const auto after=window.document().snapshot();
        const auto owner=committed_boundary(after);
        const auto geometry=sketch::boundary_geometry(sketch::decode_identified_boundary_entity(owner));
        require(geometry.size()==4 && std::abs(std::abs(sketch::signed_area(geometry))-96*0.3048*0.3048)<1e-8,
            "keyboard-only rectangle must calculate exactly 96 square feet");
        require(after.revision()==before.revision()+1,"keyboard boundary must publish one command");
        require(window.undoCommand() && window.document().snapshot().entities()==before.entities() &&
            window.redoCommand() && window.document().snapshot().entities()==after.entities(),"keyboard commit must undo and redo once");
        QTemporaryDir directory; require(directory.isValid(),"keyboard fixture directory unavailable");
        const auto path=directory.filePath("keyboard-boundary.bldproj");
        require(window.saveProjectAs(path) && window.openProject(path) &&
            window.document().snapshot().entities()==after.entities(),"keyboard authored receipt must save and reopen");
    }
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

void test_saved_boundary_draft_resumes_without_unsaved_warning() {
    MainWindow window;
    prepare_window(window);
    window.setMetricUnits(true);
    auto* drawing = canvas(window, QStringLiteral("measurementPlanCanvas"));
    require(window.beginBoundaryDrawing(BoundaryAuthoringMode::draw_first,
                                        QStringLiteral("living")),
            "saved draft fixture must start drawing");
    send_click(*drawing, {0, 0});
    drive_boundary_modal(window, *drawing, Qt::Key_D, [&](QDialog* modal) {
        auto* input = dynamic_cast<sketch::desktop::BoundaryInputDialog*>(modal);
        require(input != nullptr, "saved draft must accept exact input through the native dialog");
        input->findChild<QLineEdit*>("boundaryInputLength")->setText("2 m");
        input->findChild<QLineEdit*>("boundaryInputHeading")->setText("0 deg");
        require(input->submit(), "saved draft precision line must submit");
    });
    send_click(*drawing, {2, 2});
    send_key(*drawing, Qt::Key_Z, Qt::ControlModifier);
    const auto original = preview(*drawing);
    require(original.segments.size() == 1 && window.windowTitle().endsWith(" *"),
            "unsaved draft input must mark the project dirty");

    // Observe real native prompts and cancel them so a regression cannot hang.
    bool prompted = false;
    bool save_changes_prompt = false;
    bool choose_save = false;
    int prompt_count = 0;
    QTimer modal_responder;
    modal_responder.setInterval(1);
    QObject::connect(&modal_responder, &QTimer::timeout, &window, [&] {
        if (auto* modal = qobject_cast<QDialog*>(QApplication::activeModalWidget())) {
            prompted = true;
            ++prompt_count;
            if (auto* message = qobject_cast<QMessageBox*>(modal)) {
                save_changes_prompt = message->button(QMessageBox::Save) &&
                    message->button(QMessageBox::Discard) && message->button(QMessageBox::Cancel);
                const auto choice = choose_save && save_changes_prompt
                    ? QMessageBox::Save : QMessageBox::Cancel;
                if (auto* button = message->button(choice)) button->click();
                else message->reject();
            } else modal->reject();
        }
    });
    modal_responder.start();
    require(!window.saveProject() && prompted && window.windowTitle().endsWith(" *") &&
                same_boundary(preview(*drawing).segments, original.segments),
            "cancelling Save As must retain dirty draft input");
    modal_responder.stop();

    QTemporaryDir directory;
    require(directory.isValid(), "saved draft directory must exist");
    const auto path = directory.filePath(QStringLiteral("unfinished.bldproj"));
    require(window.saveProjectAs(path), "ordinary Save As must persist the unfinished draft");
    require(!window.windowTitle().endsWith(" *"),
            "a saved unfinished draft must not retain the unsaved title marker");
    require(!window.statusBar()->currentMessage().contains(QStringLiteral("unsaved")),
            "successful draft save must not claim the draft is unsaved");
    const auto saved = sketch::ProjectStore::load_archive(
        std::filesystem::path(path.toStdWString()), sketch::ArchiveRole::ordinary);
    require(saved.supported() && saved.recovery.decoded->active.has_value(),
            "ordinary project save must contain the active exact-input checkpoint");
    const auto checkpoint = *saved.recovery.decoded->active;

    send_move_at_screen(*drawing, model_to_canvas(*drawing, {1, 1}));
    require(window.saveProject() && !window.windowTitle().endsWith(" *") &&
                !window.statusBar()->currentMessage().contains(QStringLiteral("unsaved")),
            "cursor motion after Save must not dirty the saved semantic draft");
    prompted = false;
    modal_responder.start();
    const bool reopened = window.openProject(path);
    modal_responder.stop();
    require(reopened && !prompted, "saved draft must allow Open without a discard prompt");
    prompted = false;
    modal_responder.start();
    const bool created = window.createNewProject();
    modal_responder.stop();
    require(created && !prompted, "saved draft must allow New without a discard prompt");
    require(window.openProject(path), "ordinary saved draft must reopen writable after New");
    require(!window.windowTitle().endsWith(" *") &&
                same_boundary(preview(*drawing).segments, original.segments) &&
                same_optional_point(preview(*drawing).anchor, original.anchor),
            "reopened draft must retain its exact geometry and clean saved state");
    require(window.saveProject(), "resumed draft checkpoint must remain saveable");
    const auto restored = sketch::ProjectStore::load_archive(
        std::filesystem::path(path.toStdWString()), sketch::ArchiveRole::ordinary);
    require(restored.supported() && restored.recovery.decoded->active == checkpoint,
            "ordinary reopen must retain exact input receipts, identities, context and redo position");
    send_key(*drawing, Qt::Key_Y, Qt::ControlModifier);
    require(preview(*drawing).segments.size() == 2 &&
                same_point(preview(*drawing).segments.back().end, {2, 2}) &&
                window.windowTitle().endsWith(" *"),
            "continuing restored draft redo must restore the exact input and mark it dirty");
    const auto invalid_path = directory.filePath(QStringLiteral("directory.bldproj"));
    require(QDir().mkdir(invalid_path), "failed draft save destination must be a directory");
    require(!window.saveProjectAs(invalid_path) && window.windowTitle().endsWith(" *") &&
                preview(*drawing).segments.size() == 2,
            "failed Save As must retain the changed dirty draft");

    prompted = false;
    modal_responder.start();
    QCloseEvent dirty_close;
    QApplication::sendEvent(&window, &dirty_close);
    modal_responder.stop();
    require(prompted && save_changes_prompt && !dirty_close.isAccepted() && drawing->boundaryDraftPreview(),
            "changed draft must offer Save, Discard and Cancel, and Cancel must retain it");
    choose_save = true;
    prompted = false;
    prompt_count = 0;
    modal_responder.start();
    QCloseEvent save_close;
    QApplication::sendEvent(&window, &save_close);
    modal_responder.stop();
    choose_save = false;
    require(save_close.isAccepted() && prompted && save_changes_prompt && prompt_count == 1 &&
                !window.windowTitle().endsWith(" *") && drawing->boundaryDraftPreview(),
            "Save during close must persist the changed draft without an extra discard prompt");
    prompted = false;
    modal_responder.start();
    QCloseEvent saved_close;
    QApplication::sendEvent(&window, &saved_close);
    modal_responder.stop();
    require(saved_close.isAccepted() && !prompted && !window.windowTitle().endsWith(" *"),
            "saved draft must close without demanding discard");
    require(window.openProject(path), "saved draft remains resumable after close approval");
    send_click(*drawing, {0, 2});
    send_key(*drawing, Qt::Key_Return);
    require(!drawing->boundaryDraftPreview(), "resumed exact-input draft must finish normally");
    const auto finished = committed_boundary(window.document().snapshot());
    require_rectangle(sketch::decode_identified_boundary_entity(finished));
    const auto receipts = sketch::decode_boundary_receipt_envelope(
        finished.properties.at("boundary_authoring"));
    require(receipts.supported() && receipts.record->edges.front().receipt.kind ==
                sketch::BoundaryConstructionKind::line_heading,
            "finishing the resumed draft must retain its original precision input receipt");
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
    require(measurement->viewScale() == 80.0, "metric cursor fixture must use the known empty-view zoom");
    const auto measurement_snapped = snap_to_known_grid(measurement_raw, 0.2);
    require(!same_point(measurement_raw, measurement_snapped),
            "measurement off-grid cursor fixture must differ from its 20 cm grid snap");

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
                snapped_status.contains(QStringLiteral("400.0 mm")),
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

    // The 3D switch is refused during a 2D draft. Events/toggles from the
    // inactive canvas must not overwrite the owning canvas's cursor or nodes.
    const auto architectural_anchor_screen = model_to_canvas(*architectural, {0.0, 0.0});
    const auto architectural_screen = architectural_anchor_screen + QPoint(-31, -29);
    const auto architectural_raw = canvas_to_model(*architectural, architectural_screen);
    require(architectural->viewScale() == 80.0, "architectural cursor fixture must use the known empty-view zoom");
    const auto architectural_snapped = snap_to_known_grid(architectural_raw, 0.2);
    require(!same_point(architectural_raw, architectural_snapped),
            "architectural off-grid cursor fixture must differ from its 20 cm grid snap");
    send_move_at_screen(*architectural, architectural_screen);
    architectural->setSnapEnabled(false);
    process_events();
    send_click_at_screen(*architectural, architectural_screen);
    const auto retained = preview(*measurement);
    require(same_boundary(retained.segments, snapped_draft.segments) && retained.rubber_band &&
                same_point(retained.rubber_band->end, measurement_snapped) &&
                window.statusBar()->currentMessage() == snapped_status,
            "inactive canvas input and snap toggles must preserve the active draft and cursor");

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
            const auto before = window.document().snapshot();
            require(window.beginBoundaryDrawing(BoundaryAuthoringMode::define_first,
                                                QStringLiteral("garage")),
                    "off-grid Define First fixture must start with an explicit classification");
            require(window.workspace() == Workspace::measurement,
                    "Define First from either initial workspace must activate its 2D authoring surface");
            auto* active_canvas = canvas(window, QStringLiteral("measurementPlanCanvas"));

            const auto anchor_screen = model_to_canvas(*active_canvas, {0.0, 0.0});
            send_click_at_screen(*active_canvas, anchor_screen);
            const auto edge_screen = anchor_screen + QPoint(37, 0);
            const auto edge_raw = canvas_to_model(*active_canvas, edge_screen);
            require(active_canvas->viewScale() == 80.0, "dimension cursor fixture must use the known empty-view zoom");
            const auto edge_snapped = snap_to_known_grid(edge_raw, 0.2);
            require(!same_point(edge_raw, edge_snapped),
                    "Define First edge fixture must differ from its 20 cm grid snap");

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
            const auto dimension_snapped = snap_to_known_grid(dimension_raw, 0.2);
            require(!same_point(dimension_raw, dimension_snapped),
                    "Define First dimension fixture must differ from its 20 cm grid snap");
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
        require(window.beginBoundaryDrawing(BoundaryAuthoringMode::draw_first,
                                            QStringLiteral("living")),
                "snapped receipt fixture must start Draw First");
        require(window.workspace() == Workspace::measurement,
                "Draw First from either initial workspace must activate its 2D authoring surface");
        auto* target = canvas(window, QStringLiteral("measurementPlanCanvas"));
        const auto origin = model_to_canvas(*target, {0.0, 0.0});
        const auto endpoint = origin + QPoint(37, 0);
        require(target->viewScale() == 80.0, "imperial receipt fixture must use the known empty-view zoom");
        const auto expected = snap_to_known_grid(canvas_to_model(*target, endpoint), 0.3048);
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
    window.setMetricUnits(true); // Keep the exact two-metre fixture on its metric grid.
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
        window.setMetricUnits(true); // The exact two-metre click fixture lies on the 20 cm grid.
        auto* drawing_mode = window.findChild<QComboBox*>(QStringLiteral("drawingMode"));
        require(drawing_mode != nullptr, "unified measured-area fixture needs the draw mode selector");
        drawing_mode->setCurrentIndex(drawing_mode->findData(QStringLiteral("measurement")));
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
        window.setWorkspace(Workspace::measurement);
        process_events();
        // Physical architectural walls are authored on the conventional 2D
        // world-XY surface. The architectural workspace displays 3D/views.
        auto* wall_canvas = canvas(window, QStringLiteral("measurementPlanCanvas"));
        const auto wall_before = window.document().snapshot();
        send_click(*wall_canvas, {-2.0, -1.0});
        send_click(*wall_canvas, {2.0, -1.0});
        const auto wall_after = window.document().snapshot();
        require(wall_after.revision() == wall_before.revision() + 1,
                "two empty-canvas clicks in the 2D workspace must commit one wall command");
        const auto wall_count = static_cast<std::size_t>(std::count_if(
            wall_after.entities().begin(), wall_after.entities().end(), [](const auto& entry) {
                return entry.second.type == "wall";
            }));
        require(wall_count == 1,
                "architectural click drawing must create a real wall rather than a canvas-only stroke");
    }
}

void send_inline_key(QWidget& target, int key,
                     Qt::KeyboardModifiers modifiers = Qt::NoModifier,
                     const QString& text = {}) {
    QKeyEvent press(QEvent::KeyPress, key, modifiers, text);
    QApplication::sendEvent(&target, &press);
    QKeyEvent release(QEvent::KeyRelease, key, modifiers, text);
    QApplication::sendEvent(&target, &release);
    process_events();
}

void type_inline_length(PlanCanvas& drawing, const QString& text) {
    require(!text.isEmpty(), "inline typing fixture must contain a length");
    drawing.setFocus();
    for (const auto character : text) {
        auto* target = QApplication::focusWidget();
        require(target != nullptr, "inline typing must retain a focused native widget");
        send_inline_key(*target, character.toUpper().unicode(), Qt::NoModifier,
                        QString(character));
    }
}

void capture_inline_widget(QWidget& widget, const QString& filename) {
    const auto directory = qEnvironmentVariable("VERTEX_TEST_CAPTURE_DIR");
    if (directory.isEmpty()) return;
    require(QDir().mkpath(directory), "inline UI capture directory must be writable");
    require(widget.grab().save(QDir(directory).filePath(filename)),
            "native inline drawing UI capture must save successfully");
}

void test_inline_wall_cardinal_length_chain() {
    MainWindow window;
    prepare_window(window);
    window.setMetricUnits(false);
    QApplication::setActiveWindow(&window);
    process_events();
    auto* drawing = canvas(window, QStringLiteral("measurementPlanCanvas"));
    auto* thickness = window.findChild<QLineEdit*>(QStringLiteral("wallDrawThickness"));
    auto* height = window.findChild<QLineEdit*>(QStringLiteral("wallDrawHeight"));
    require(thickness && height, "inline wall fixture needs the existing physical wall settings");
    thickness->setText(QStringLiteral("7 in"));
    height->setText(QStringLiteral("9 ft"));
    const auto original = window.document().snapshot();
    const auto original_layer = window.activeLayerId();
    send_click(*drawing, {0.0, 0.0});

    // This is deliberately the first new-feature assertion: the baseline
    // must fail after an actual default-wall anchor, before any test-only API.
    auto* panel = drawing->findChild<QWidget*>(QStringLiteral("drawingInputPanel"));
    require(panel && panel->isVisible(),
            "a wall anchor must reveal the native inline drawing input panel");
    auto* length = panel->findChild<QLineEdit*>(QStringLiteral("drawingLengthInput"));
    auto* right = panel->findChild<QToolButton*>(QStringLiteral("drawingDirectionRight"));
    auto* up = panel->findChild<QToolButton*>(QStringLiteral("drawingDirectionUp"));
    auto* left = panel->findChild<QToolButton*>(QStringLiteral("drawingDirectionLeft"));
    auto* down = panel->findChild<QToolButton*>(QStringLiteral("drawingDirectionDown"));
    require(length && right && up && left && down && right->isChecked(),
            "inline drawing must expose the length field and visibly selected default Right direction");
    require(right->arrowType() == Qt::RightArrow && up->arrowType() == Qt::UpArrow &&
                left->arrowType() == Qt::LeftArrow && down->arrowType() == Qt::DownArrow,
            "inline directions must use native cardinal arrow controls");
    require_same_document(original, window.document().snapshot(),
                          "anchoring a wall must leave the document unchanged");

    const std::array<QString, 4> lengths{QStringLiteral("12 ft 6 in"), QStringLiteral("8 ft"),
                                         QStringLiteral("12 ft 6 in"), QStringLiteral("8 ft")};
    const std::array<int, 4> keys{Qt::Key_Right, Qt::Key_Up, Qt::Key_Left, Qt::Key_Down};
    const std::array<Vec2, 4> endpoints{{{3.81, 0.0}, {3.81, 2.4384},
                                          {0.0, 2.4384}, {0.0, 0.0}}};
    Vec2 previous{};
    std::vector<std::string> wall_ids;
    for (std::size_t index = 0; index < lengths.size(); ++index) {
        const auto before_edge = window.document().snapshot();
        if (index == 3) {
            type_inline_length(*drawing, QStringLiteral("99 ft"));
            send_inline_key(*length, Qt::Key_Escape);
            require(length->text().isEmpty() && drawing->hasFocus() && panel->isVisible(),
                    "Escape in the field must clear pending text, retain the chain, and focus the canvas");
            require_same_document(before_edge, window.document().snapshot(),
                                  "clearing inline text must retain every committed wall");
        }
        type_inline_length(*drawing, lengths[index]);
        require(length->hasFocus() && length->text() == lengths[index],
                "typing a length on the canvas must forward every character to the native field");
        send_inline_key(*length, keys[index]);
        const auto committed = window.document().snapshot();
        require(committed.revision() == original.revision() + index + 1,
                "each typed wall edge must immediately create one history command");
        std::string id;
        for (const auto& [candidate, entity] : committed.entities()) {
            if (entity.type == "wall" && !before_edge.entities().contains(candidate)) {
                require(id.empty(), "one inline submission must create only one wall");
                id = candidate;
            }
        }
        require(!id.empty(), "a typed cardinal edge must commit a physical wall");
        wall_ids.push_back(id);
        const auto& properties = committed.entities().at(id).properties;
        const auto& baseline = properties.at("baseline");
        const Vec2 start{baseline.at("start").at(0).get<double>(),
                         baseline.at("start").at(1).get<double>()};
        const Vec2 end{baseline.at("end").at(0).get<double>(),
                       baseline.at("end").at(1).get<double>()};
        require(same_point(start, previous),
                "typed wall edges must share the exact canonical previous endpoint");
        require(std::abs(end.x - endpoints[index].x) < 1e-12 &&
                    std::abs(end.y - endpoints[index].y) < 1e-12 &&
                    ((index % 2 == 0 && end.y == start.y) ||
                     (index % 2 == 1 && end.x == start.x)) &&
                    baseline.at("sweep_radians") == 0.0,
                "typed cardinal lengths must bypass the foot grid and preserve the unchanged coordinate exactly");
        require(std::abs(properties.at("thickness_m").get<double>() - 0.1778) < 1e-12 &&
                    std::abs(properties.at("height_m").get<double>() - 2.7432) < 1e-12 &&
                    !window.metricUnits() && window.activeLayerId() == original_layer,
                "typed wall commits must retain physical wall depth, height, units, and layer settings");
        const auto input = sketch::decode_construction_receipt(properties.at("original_drawing_input"));
        const auto replay = sketch::replay_construction_receipt(input,
            sketch::ConstructionReplayContext{start, std::nullopt, std::nullopt,
                                               sketch::default_geometry_tolerance_metres});
        require(input.kind == sketch::BoundaryConstructionKind::line_rise_run &&
                    input.segment_id == id && input.rise && input.run &&
                    same_point(input.start, start) && same_point(replay.segment.end, end) &&
                    (index % 2 == 0 ? input.run : input.rise)->original_expression.find(
                        lengths[index].toStdString()) != std::string::npos,
                "typed wall provenance must retain exact replayable signed input and original expression");
        previous = end;
        require(length->text().isEmpty(), "a successful edge must consume its pending length");
    }
    require(same_point(previous, {0.0, 0.0}),
            "four typed cardinal walls must close at the exact original anchor");
    const auto perimeter = window.document().snapshot();
    require(!panel->isVisible(), "returning to the first wall anchor must finish the closed chain");
    send_click(*drawing, {-1.524, -1.524});
    require(panel->isVisible(), "a new empty-canvas anchor must start another wall chain");
    send_key(*drawing, Qt::Key_Escape);
    require(!panel->isVisible(), "Escape on the canvas must finish an open wall chain");
    require_same_document(perimeter, window.document().snapshot(),
                          "finishing the chain must retain the committed perimeter");
    require(window.undoCommand() &&
                !window.document().snapshot().entities().contains(wall_ids.back()) &&
                window.document().snapshot().entities().contains(wall_ids.front()),
            "wall undo must remove only the last immediately committed edge");
    require(window.redoCommand() &&
                window.document().snapshot().entities() == perimeter.entities(),
            "wall redo must restore the exact typed perimeter and canonical endpoints");
    QTemporaryDir directory;
    require(directory.isValid(), "typed wall save fixture needs a temporary directory");
    const auto path = directory.filePath(QStringLiteral("inline-wall-perimeter.bldproj"));
    require(window.saveProjectAs(path) && window.openProject(path) &&
                window.document().snapshot().entities() == perimeter.entities(),
            "typed wall geometry and physical settings must survive save and reopen");
}

void test_inline_wall_validation_units_enter_and_keypad() {
    for (const bool metric : {false, true}) {
        MainWindow window;
        prepare_window(window);
        window.setMetricUnits(metric);
        QApplication::setActiveWindow(&window);
        process_events();
        auto* drawing = canvas(window, QStringLiteral("measurementPlanCanvas"));
        send_click(*drawing, {0, 0});
        auto* panel = drawing->findChild<QWidget*>(QStringLiteral("drawingInputPanel"));
        require(panel && panel->isVisible(), "validation fixture must show actual inline controls");
        auto* length = panel->findChild<QLineEdit*>(QStringLiteral("drawingLengthInput"));
        auto* error = panel->findChild<QLabel*>(QStringLiteral("drawingInputError"));
        auto* up = panel->findChild<QToolButton*>(QStringLiteral("drawingDirectionUp"));
        auto* keypad_toggle = panel->findChild<QToolButton*>(QStringLiteral("drawingKeypadToggle"));
        auto* keypad = panel->findChild<QWidget*>(QStringLiteral("drawingKeypad"));
        require(length && error && up && keypad_toggle && keypad,
                "inline drawing must provide error feedback and an expandable native keypad");
        require(!keypad->isVisible(), "the compact drawing keypad must initially be collapsed");
        keypad_toggle->click();
        process_events();
        require(keypad->isVisible(), "the native keypad toggle must expand the keypad");
        for (const auto digit : {QStringLiteral("1"), QStringLiteral("2")}) {
            QAbstractButton* button = nullptr;
            for (auto* candidate : keypad->findChildren<QAbstractButton*>()) {
                if (candidate->text() == digit) button = candidate;
            }
            require(button && button->isVisible(), "expanded keypad must expose actual digit buttons");
            button->click();
            process_events();
        }
        require(length->text() == QStringLiteral("12"),
                "native keypad clicks must populate the same length field used by keyboard drawing");
        capture_inline_widget(window, metric ? QStringLiteral("inline-keypad-metric.png")
                                            : QStringLiteral("inline-keypad-imperial.png"));
        send_inline_key(*length, Qt::Key_Escape);
        const auto anchored = window.document().snapshot();
        for (const auto invalid : {QStringLiteral("0"), QStringLiteral("-2"),
                                  QStringLiteral("12 malformed")}) {
            length->setText(invalid);
            send_inline_key(*length, Qt::Key_Right);
            require_same_document(anchored, window.document().snapshot(),
                                  "zero, negative, and malformed inline lengths must reject without creating walls");
            require(panel->isVisible() && error->isVisible() && !error->text().isEmpty() &&
                        length->text() == invalid,
                    "invalid inline input must retain the pending text and show native error feedback");
        }
        capture_inline_widget(window, metric ? QStringLiteral("inline-invalid-metric.png")
                                            : QStringLiteral("inline-invalid-imperial.png"));
        length->setText(QStringLiteral("2"));
        send_inline_key(*length, Qt::Key_Up);
        require(up->isChecked(), "a successful Up key must visibly retain the Up direction");
        type_inline_length(*drawing, QStringLiteral("1 1/4"));
        send_inline_key(*length, Qt::Key_Return);
        const auto after_enter = window.document().snapshot();
        require(after_enter.revision() == anchored.revision() + 2,
                "Enter must commit another wall using the visibly retained direction");
        bool found_end = false;
        const auto expected_y = metric ? 3.25 : 0.9906;
        for (const auto& [id, entity] : after_enter.entities()) {
            (void)id;
            if (entity.type != "wall") continue;
            const auto& end = entity.properties.at("baseline").at("end");
            if (end.at(0).get<double>() == 0.0 &&
                std::abs(end.at(1).get<double>() - expected_y) < 1e-12) found_end = true;
        }
        require(found_end, "implicit fractional lengths must use metres or feet and remain exactly cardinal");
        type_inline_length(*drawing, QStringLiteral("3"));
        window.setMetricUnits(!metric);
        process_events();
        const auto changed_units = window.document().snapshot();
        send_inline_key(*length, Qt::Key_Right);
        require_same_document(changed_units, window.document().snapshot(),
                              "changing unit basis must not reinterpret and commit a stale pending length");
        send_key(*drawing, Qt::Key_Escape);
    }
}

void test_inline_measurement_receipts_local_history_and_define_first() {
    for (const auto mode : {BoundaryAuthoringMode::draw_first, BoundaryAuthoringMode::define_first}) {
        MainWindow window;
        prepare_window(window);
        window.setMetricUnits(true);
        QApplication::setActiveWindow(&window);
        process_events();
        auto* drawing = canvas(window, QStringLiteral("measurementPlanCanvas"));
        require(window.beginBoundaryDrawing(mode, QStringLiteral("living")),
                "inline measured boundary fixture must start the existing authoring session");
        const auto original = window.document().snapshot();
        send_click(*drawing, {0, 0});
        auto* panel = drawing->findChild<QWidget*>(QStringLiteral("drawingInputPanel"));
        auto* length = panel ? panel->findChild<QLineEdit*>(QStringLiteral("drawingLengthInput")) : nullptr;
        require(panel && panel->isVisible() && length,
                "a measured boundary anchor must expose the same native inline length controls");
        const std::array<QString, 3> lengths{QStringLiteral("2.125 m"), QStringLiteral("1.375 m"),
                                             QStringLiteral("2.125 m")};
        const std::array<int, 3> keys{Qt::Key_Right, Qt::Key_Up, Qt::Key_Left};
        const std::array<Vec2, 3> endpoints{{{2.125, 0}, {2.125, 1.375}, {0, 1.375}}};
        for (std::size_t index = 0; index < lengths.size(); ++index) {
            type_inline_length(*drawing, lengths[index]);
            send_inline_key(*length, keys[index]);
            const auto draft = preview(*drawing);
            require(draft.segments.size() == index + 1 &&
                        same_point(draft.segments.back().end, endpoints[index]),
                    "inline measured lengths must bypass the metric grid and retain exact cardinal endpoints");
            require_same_document(original, window.document().snapshot(),
                                  "inline measured edges must remain local until boundary closure");
            if (mode == BoundaryAuthoringMode::define_first) {
                length->setText(QStringLiteral("4 m"));
                send_inline_key(*length, Qt::Key_Right);
                require(same_boundary(preview(*drawing).segments, draft.segments),
                        "Define First must refuse another typed edge while dimension placement is pending");
                send_inline_key(*length, Qt::Key_Escape);
                drive_boundary_modal(window, *drawing, Qt::Key_D, [&](QDialog* modal) {
                    auto* input = dynamic_cast<sketch::desktop::BoundaryInputDialog*>(modal);
                    require(input, "D must retain the existing pending-dimension precision form");
                    if (index == 0) {
                        input->findChild<QLineEdit*>(QStringLiteral("boundaryInputEndX"))->setText(QStringLiteral("invalid"));
                        require(!input->submit(), "the retained precision dialog must reject malformed placement input");
                        capture_inline_widget(*input, QStringLiteral("inline-define-first-invalid-dialog.png"));
                    }
                    input->findChild<QLineEdit*>(QStringLiteral("boundaryInputEndX"))->setText(QStringLiteral("1 m"));
                    input->findChild<QLineEdit*>(QStringLiteral("boundaryInputEndY"))->setText(QStringLiteral("-1 m"));
                    require(input->submit(), "precision input must place the pending Define First dimension");
                });
            }
            if (index == 0) {
                send_key(*drawing, Qt::Key_Z, Qt::ControlModifier);
                if (mode == BoundaryAuthoringMode::define_first) {
                    require(same_boundary(preview(*drawing).segments, draft.segments),
                            "Define First first undo must remove manual dimension placement, retaining its edge");
                    send_key(*drawing, Qt::Key_Z, Qt::ControlModifier);
                }
                require(preview(*drawing).segments.empty(), "inline measured edge undo must stay within the draft");
                send_key(*drawing, Qt::Key_Y, Qt::ControlModifier);
                if (mode == BoundaryAuthoringMode::define_first)
                    send_key(*drawing, Qt::Key_Y, Qt::ControlModifier);
                require(same_boundary(preview(*drawing).segments, draft.segments),
                        "inline measured edge redo must restore exact geometry and local authoring state");
            }
        }
        send_key(*drawing, Qt::Key_Return);
        if (mode == BoundaryAuthoringMode::define_first) {
            send_click(*drawing, {-0.4, 0.6});
        }
        require(!drawing->boundaryDraftPreview(), "canvas Enter must close the inline measured boundary");
        const auto committed = window.document().snapshot();
        require(committed.revision() == original.revision() + 1,
                "the entire measured boundary must commit as one command");
        const auto entity = committed_boundary(committed);
        const auto boundary = sketch::decode_identified_boundary_entity(entity);
        const auto receipts = sketch::decode_boundary_receipt_envelope(entity.properties.at("boundary_authoring"));
        require(receipts.supported() && receipts.record && receipts.record->edges.size() == 4 &&
                    boundary.segments.size() == 4 && committed_dimensions(committed).size() == 4,
                "inline measured closure must retain receipt topology and one semantic dimension per edge");
        for (std::size_t index = 0; index < lengths.size(); ++index) {
            const auto& receipt = receipts.record->edges[index].receipt;
            require(receipt.kind == sketch::BoundaryConstructionKind::line_rise_run && receipt.rise && receipt.run &&
                        receipt.segment_id == boundary.segments[index].segment_id &&
                        same_point(boundary.segments[index].segment.end, endpoints[index]),
                    "typed cardinal measured edges must persist exact replayable rise/run receipts");
            const auto& entered = index == 1 ? *receipt.rise : *receipt.run;
            require(entered.original_expression.find(lengths[index].toStdString()) != std::string::npos,
                    "cardinal receipts must retain the actual typed length expression");
        }
        require(receipts.record->edges.back().receipt.kind == sketch::BoundaryConstructionKind::line_closure,
                "canvas Enter must retain the existing receipt-bearing closure command");
    }
}

void test_inline_wall_rejects_stale_source_and_selection() {
    MainWindow window;
    prepare_window(window);
    window.setMetricUnits(true);
    QApplication::setActiveWindow(&window);
    process_events();
    auto* drawing = canvas(window, QStringLiteral("measurementPlanCanvas"));
    send_click(*drawing, {0, 0});
    auto* length = drawing->findChild<QLineEdit*>(QStringLiteral("drawingLengthInput"));
    require(length, "stale-source fixture must use the actual inline field");
    type_inline_length(*drawing, QStringLiteral("2 m"));
    send_inline_key(*length, Qt::Key_Right);
    const auto first = window.document().snapshot();
    std::string wall_id;
    for (const auto& [id, entity] : first.entities()) {
        if (entity.type == "wall") wall_id = id;
    }
    require(!wall_id.empty(), "stale-source fixture must first commit a wall");
    type_inline_length(*drawing, QStringLiteral("3 m"));
    auto changed_wall = first.entities().at(wall_id);
    changed_wall.properties["height_m"] = 3.5;
    window.document().apply(sketch::ApplyEntityChanges{
        first.revision(), {sketch::EntityChange::upsert(changed_wall)}, {}, "inline stale-source fixture"});
    const auto changed_source = window.document().snapshot();
    send_inline_key(*length, Qt::Key_Up);
    require_same_document(changed_source, window.document().snapshot(),
                          "a source revision changed outside the inline action must reject its stale pending edge");
    send_key(*drawing, Qt::Key_Escape);
    send_click(*drawing, {-2, -2});
    type_inline_length(*drawing, QStringLiteral("3 m"));
    require(window.selectEntity(QString::fromStdString(wall_id)),
            "selection guard fixture must select the existing physical wall");
    const auto selected = window.document().snapshot();
    send_inline_key(*length, Qt::Key_Right);
    require_same_document(selected, window.document().snapshot(),
                          "changing selection must not commit the former wall chain's pending length");
}

void test_inline_measurement_held_enter_does_not_finish() {
    MainWindow window;
    prepare_window(window);
    window.setMetricUnits(true);
    QApplication::setActiveWindow(&window);
    process_events();
    auto* drawing = canvas(window, QStringLiteral("measurementPlanCanvas"));
    require(window.beginBoundaryDrawing(BoundaryAuthoringMode::draw_first, QStringLiteral("living")),
            "held Enter fixture must start a classified measured outline");
    send_click(*drawing, {0, 0});
    auto* length = drawing->findChild<QLineEdit*>(QStringLiteral("drawingLengthInput"));
    require(length, "held Enter fixture must use the actual inline control");
    type_inline_length(*drawing, QStringLiteral("2 m"));
    send_inline_key(*length, Qt::Key_Right);
    auto* up = drawing->findChild<QToolButton*>(QStringLiteral("drawingDirectionUp"));
    require(up, "held Enter fixture needs the retained direction button");
    up->click(); // Select Up with an empty field, without adding an edge.
    type_inline_length(*drawing, QStringLiteral("1 m"));
    send_inline_key(*length, Qt::Key_Return);
    const auto original = window.document().snapshot();
    const auto draft = preview(*drawing).segments;
    // Focus has returned from inline entry to the canvas, just as when the
    // operating system continues delivering repeats from a held Enter key.
    QKeyEvent repeated(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier, QString{}, true, 1);
    QApplication::sendEvent(drawing, &repeated);
    process_events();
    require_same_document(original, window.document().snapshot(),
                          "a repeated Enter after edge entry must not commit an unfinished outline");
    require(drawing->boundaryDraftPreview() && same_boundary(preview(*drawing).segments, draft),
            "a repeated Enter must leave the exact unfinished outline and its edge history unchanged");
    type_inline_length(*drawing, QStringLiteral("3 m"));
    send_inline_key(*length, Qt::Key_Escape);
    QKeyEvent repeated_escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier, QString{}, true, 1);
    QApplication::sendEvent(drawing, &repeated_escape);
    process_events();
    require_same_document(original, window.document().snapshot(),
                          "holding Escape to clear length input must not change the saved drawing");
    require(drawing->boundaryDraftPreview() && same_boundary(preview(*drawing).segments, draft),
            "holding Escape in the length field must not discard the unfinished outline");
}

void test_physical_wall_precision_heading() {
    MainWindow window;
    prepare_window(window);
    window.setMetricUnits(true);
    QApplication::setActiveWindow(&window);
    process_events();
    auto* drawing = canvas(window, QStringLiteral("measurementPlanCanvas"));
    auto* thickness = window.findChild<QLineEdit*>(QStringLiteral("wallDrawThickness"));
    auto* height = window.findChild<QLineEdit*>(QStringLiteral("wallDrawHeight"));
    require(thickness && height, "wall precision fixture needs physical wall settings");
    thickness->setText(QStringLiteral("0.2 m"));
    height->setText(QStringLiteral("3 m"));
    send_click(*drawing, {0, 0});
    const auto anchored = window.document().snapshot();
    drive_boundary_modal(window, *drawing, Qt::Key_D, [&](QDialog* modal) {
        auto* input = dynamic_cast<sketch::desktop::BoundaryInputDialog*>(modal);
        require(input, "physical wall precision must share the analytical construction form");
        input->findChild<QComboBox*>(QStringLiteral("boundaryInputMethod"))->setCurrentIndex(0);
        input->findChild<QLineEdit*>(QStringLiteral("boundaryInputLength"))->setText(QStringLiteral("2 m"));
        input->findChild<QLineEdit*>(QStringLiteral("boundaryInputHeading"))->setText(QStringLiteral("45 deg"));
        capture_inline_widget(*input, QStringLiteral("wall-heading-input.png"));
        require(input->submit(), "a physical wall length and heading must validate");
    }, "D on an anchored physical Wall must open precise construction input");
    const auto after = window.document().snapshot();
    require(after.revision() == anchored.revision() + 1,
            "one precise wall must commit one history command");
    bool found = false;
    for (const auto& [id, entity] : after.entities()) {
        if (entity.type != "wall" || anchored.entities().contains(id)) continue;
        found = true;
        const auto& baseline = entity.properties.at("baseline");
        const auto& end = baseline.at("end");
        require(std::abs(end.at(0).get<double>() - std::sqrt(2.0)) < 1e-12 &&
                    std::abs(end.at(1).get<double>() - std::sqrt(2.0)) < 1e-12,
                "45-degree wall must retain exact analytical endpoints without grid rounding");
        require(entity.properties.at("thickness_m") == 0.2 && entity.properties.at("height_m") == 3.0,
                "precision must preserve physical wall settings");
        const auto receipt = sketch::decode_construction_receipt(entity.properties.at("original_drawing_input"));
        require(receipt.kind == sketch::BoundaryConstructionKind::line_heading &&
                    receipt.segment_id == id && receipt.heading && receipt.distance &&
                    receipt.heading->original_expression == "45 deg",
                "precise wall must retain entered angle and length provenance");
    }
    require(found, "accepted heading input must create an actual wall");
}

Entity single_new_wall(const DocumentSnapshot& before, const DocumentSnapshot& after) {
    std::optional<Entity> result;
    require(after.revision() == before.revision() + 1,
            "one precision submission must publish exactly one wall command");
    for (const auto& [id, entity] : before.entities()) {
        require(after.entities().contains(id) && after.entities().at(id) == entity,
                "creating a precision wall must preserve all existing entities");
    }
    for (const auto& [id, entity] : after.entities()) {
        if (entity.type != "wall" || before.entities().contains(id)) continue;
        require(!result, "one precise edge must create only one physical wall");
        result = entity;
    }
    require(result.has_value(), "precise construction must publish an actual physical wall");
    return *result;
}

sketch::Segment wall_baseline(const Entity& wall) {
    const auto& baseline = wall.properties.at("baseline");
    return {{baseline.at("start").at(0).get<double>(), baseline.at("start").at(1).get<double>()},
            {baseline.at("end").at(0).get<double>(), baseline.at("end").at(1).get<double>()},
            baseline.at("sweep_radians").get<double>()};
}

void test_physical_wall_precision_methods_and_persistence() {
    struct Case {
        int method;
        sketch::BoundaryConstructionKind kind;
        std::map<QString, QString> fields;
        sketch::Segment expected;
    };
    const auto pi = std::numbers::pi;
    const std::array<Case, 6> cases{{
        {1, sketch::BoundaryConstructionKind::line_rise_run,
            {{"Rise", "300 cm"}, {"Run", "125 cm"}}, {{0, 0}, {1.25, 3}, 0}},
        {3, sketch::BoundaryConstructionKind::line_to_point,
            {{"EndX", "-2 m"}, {"EndY", "150 cm"}}, {{0, 0}, {-2, 1.5}, 0}},
        {4, sketch::BoundaryConstructionKind::arc_chord_angle,
            {{"EndX", "2 m"}, {"EndY", "0 m"}, {"Sweep", "90 deg"}}, {{0, 0}, {2, 0}, pi / 2}},
        {5, sketch::BoundaryConstructionKind::arc_chord_height,
            {{"EndX", "2 m"}, {"EndY", "0 m"}, {"Height", "50 cm"}},
            {{0, 0}, {2, 0}, 4 * std::atan(0.5)}},
        {6, sketch::BoundaryConstructionKind::arc_chord_length,
            {{"EndX", "2 m"}, {"EndY", "0 m"}, {"ArcLength", "250 cm"}},
            sketch::arc_from_chord_arc_length({0, 0}, {2, 0}, 2.5, true)},
        {7, sketch::BoundaryConstructionKind::arc_start_tangent,
            {{"Tangent", "0 deg"}, {"ArcLength", "2 m"}, {"Sweep", "90 deg"}},
            {{0, 0}, {4 / pi, 4 / pi}, pi / 2}}
    }};
    for (const bool metric : {false, true}) {
        for (const auto& example : cases) {
            MainWindow window;
            prepare_window(window);
            window.setMetricUnits(metric);
            QApplication::setActiveWindow(&window);
            process_events();
            auto* drawing = canvas(window, QStringLiteral("measurementPlanCanvas"));
            window.findChild<QLineEdit*>(QStringLiteral("wallDrawThickness"))->setText("7 in");
            window.findChild<QLineEdit*>(QStringLiteral("wallDrawHeight"))->setText("9 ft");
            send_click(*drawing, {0, 0});
            const auto anchored = window.document().snapshot();
            drive_boundary_modal(window, *drawing, Qt::Key_D, [&](QDialog* modal) {
                auto* input = dynamic_cast<sketch::desktop::BoundaryInputDialog*>(modal);
                require(input && input->windowTitle().contains("wall", Qt::CaseInsensitive),
                        "physical wall precision must identify its actual object type");
                input->findChild<QComboBox*>("boundaryInputMethod")->setCurrentIndex(example.method);
                for (const auto& [suffix, text] : example.fields) {
                    auto* field = input->findChild<QLineEdit*>(QStringLiteral("boundaryInput") + suffix);
                    require(field, "precision method must expose its required native input");
                    field->setText(text);
                }
                if (example.method == 6) input->findChild<QCheckBox*>("boundaryInputClockwise")->setChecked(true);
                if (metric && example.method == 4) capture_inline_widget(*input, QStringLiteral("wall-arc-input.png"));
                require(input->submit(), "the native physical wall construction must validate");
            });
            const auto accepted = window.document().snapshot();
            const auto wall = single_new_wall(anchored, accepted);
            const auto baseline = wall_baseline(wall);
            require(same_point(baseline.start, example.expected.start) &&
                        std::hypot(baseline.end.x - example.expected.end.x,
                                   baseline.end.y - example.expected.end.y) < 1e-12 &&
                        std::abs(baseline.sweep_radians - example.expected.sweep_radians) < 1e-12,
                    "physical walls must retain the exact line or analytical arc, not a chord approximation");
            require(std::abs(wall.properties.at("thickness_m").get<double>() - 0.1778) < 1e-12 &&
                        std::abs(wall.properties.at("height_m").get<double>() - 2.7432) < 1e-12,
                    "every precision method must use the visible physical wall depth and height");
            const auto receipt = sketch::decode_construction_receipt(wall.properties.at("original_drawing_input"));
            require(receipt.kind == example.kind && receipt.segment_id == wall.id,
                    "all precision methods must preserve their original construction kind and wall identity");
            const auto replay = sketch::replay_construction_receipt(receipt,
                sketch::ConstructionReplayContext{baseline.start, std::nullopt, std::nullopt,
                    sketch::default_geometry_tolerance_metres});
            require(same_point(replay.segment.start, baseline.start) && same_point(replay.segment.end, baseline.end) &&
                        replay.segment.sweep_radians == baseline.sweep_radians,
                    "saved wall input must replay to its original authoritative geometry exactly");
            if (example.method == 6 || example.method == 7) {
                const double expected_length = example.method == 6 ? 2.5 : 2.0;
                require(std::abs(sketch::segment_length(baseline) - expected_length) < 1e-12,
                        "arc-length entry must remain a real physical arc length");
            }
            send_key(*drawing, Qt::Key_Escape);
            require(window.undoCommand() && window.document().snapshot().entities() == anchored.entities() &&
                        window.redoCommand() && window.document().snapshot().entities() == accepted.entities(),
                    "precise wall creation and connections must undo and redo atomically");
            QTemporaryDir directory;
            require(directory.isValid(), "wall precision persistence needs a temporary directory");
            const auto path = directory.filePath("wall-precision.bldproj");
            require(window.saveProjectAs(path) && window.openProject(path) &&
                        window.document().snapshot().entities() == accepted.entities(),
                    "analytical wall geometry, settings and construction provenance must survive reopening");
            if (metric && example.method == 7) capture_inline_widget(window, "wall-tangent-arc.png");
        }
    }
}

void test_physical_wall_precision_anchor_relative_and_modal_guards() {
    MainWindow window;
    prepare_window(window);
    window.setMetricUnits(true);
    QApplication::setActiveWindow(&window);
    process_events();
    auto* drawing = canvas(window, QStringLiteral("measurementPlanCanvas"));
    const auto original = window.document().snapshot();
    drive_boundary_modal(window, *drawing, Qt::Key_D, [&](QDialog* modal) {
        auto* input = dynamic_cast<sketch::desktop::BoundaryInputDialog*>(modal);
        require(input && input->windowTitle().contains("wall", Qt::CaseInsensitive),
                "D before drawing must offer the default Wall's exact start point");
        input->findChild<QLineEdit*>("boundaryInputEndX")->setText("0 m");
        input->findChild<QLineEdit*>("boundaryInputEndY")->setText("0 m");
        require(input->submit(), "keyboard-only wall start must validate");
    });
    require_same_document(original, window.document().snapshot(), "exact wall start must not add geometry");
    const auto enter = [&](int method, const std::map<QString, QString>& fields) {
        const auto before = window.document().snapshot();
        drive_boundary_modal(window, *drawing, Qt::Key_D, [&](QDialog* modal) {
            auto* input = dynamic_cast<sketch::desktop::BoundaryInputDialog*>(modal);
            require(input, "relative wall chain must use the analytical native form");
            input->findChild<QComboBox*>("boundaryInputMethod")->setCurrentIndex(method);
            for (const auto& [suffix, text] : fields)
                input->findChild<QLineEdit*>(QStringLiteral("boundaryInput") + suffix)->setText(text);
            require(input->submit(), "a relative wall chain edge must validate");
        });
        return single_new_wall(before, window.document().snapshot());
    };
    const auto first = enter(0, {{"Length", "2 m"}, {"Heading", "0 deg"}});
    const auto relative = enter(2, {{"Length", "2 m"}, {"Turn", "90 deg"}});
    require(std::hypot(wall_baseline(relative).end.x - 2, wall_baseline(relative).end.y - 2) < 1e-12,
            "relative turns must use the actual preceding straight wall heading");
    const auto arc = enter(4, {{"EndX", "4 m"}, {"EndY", "2 m"}, {"Sweep", "90 deg"}});
    const auto tangent_relative = enter(2, {{"Length", "2 m"}, {"Turn", "90 deg"}});
    const auto endpoint = wall_baseline(tangent_relative).end;
    require(std::hypot(endpoint.x - (4 - std::sqrt(2.0)), endpoint.y - (2 + std::sqrt(2.0))) < 1e-12,
            "relative turn after a curved wall must use its ending tangent rather than its chord");
    const auto receipt = sketch::decode_construction_receipt(tangent_relative.properties.at("original_drawing_input"));
    const auto replay = sketch::replay_construction_receipt(receipt,
        sketch::ConstructionReplayContext{wall_baseline(arc).end, wall_baseline(arc), std::nullopt,
            sketch::default_geometry_tolerance_metres});
    require(same_point(replay.segment.end, endpoint), "relative wall provenance must replay with its explicit previous baseline");
    const auto& replay_context = tangent_relative.properties.at("original_drawing_input_context");
    require(replay_context.at("version") == 1 &&
                replay_context.at("previous_segment") == arc.properties.at("baseline") &&
                replay_context.at("expected_start") == arc.properties.at("baseline").at("end") &&
                replay_context.at("tolerance_metres").get<double>() > 0,
            "relative wall provenance must persist its explicit historical replay context");
    const auto chain = window.document().snapshot();
    drive_boundary_modal(window, *drawing, Qt::Key_D, [&](QDialog* modal) { modal->reject(); });
    require_same_document(chain, window.document().snapshot(), "cancelled precision input must retain the wall chain");
    drive_boundary_modal(window, *drawing, Qt::Key_D, [&](QDialog* modal) {
        auto* input = dynamic_cast<sketch::desktop::BoundaryInputDialog*>(modal);
        input->findChild<QComboBox*>("boundaryInputMethod")->setCurrentIndex(0);
        input->findChild<QLineEdit*>("boundaryInputLength")->setText("1 m");
        input->findChild<QLineEdit*>("boundaryInputHeading")->setText("0 deg");
        window.setMetricUnits(false);
        require(input->submit(), "a modal candidate may validate before its stale unit context is rejected");
    });
    require_same_document(chain, window.document().snapshot(), "a unit change must invalidate the pending precision command");
    window.setMetricUnits(true);
    std::optional<DocumentSnapshot> mutated;
    drive_boundary_modal(window, *drawing, Qt::Key_D, [&](QDialog* modal) {
        auto* input = dynamic_cast<sketch::desktop::BoundaryInputDialog*>(modal);
        input->findChild<QComboBox*>("boundaryInputMethod")->setCurrentIndex(0);
        input->findChild<QLineEdit*>("boundaryInputLength")->setText("1 m");
        input->findChild<QLineEdit*>("boundaryInputHeading")->setText("0 deg");
        auto changed = window.document().snapshot().entities().at(first.id);
        changed.properties["height_m"] = 3.5;
        window.document().apply(sketch::ApplyEntityChanges{window.document().revision(),
            {sketch::EntityChange::upsert(changed)}, {}, "wall precision stale-source fixture"});
        mutated = window.document().snapshot();
        require(input->submit(), "source guard is separate from detached construction validation");
    });
    require(mutated.has_value(), "stale source fixture must make its explicit mutation");
    require_same_document(*mutated, window.document().snapshot(), "a changed revision must reject the pending wall command");
    send_key(*drawing, Qt::Key_Escape);
    const auto finished = window.document().snapshot();
    QTemporaryDir directory;
    require(directory.isValid(), "relative wall provenance needs a temporary project");
    const auto path = directory.filePath("wall-relative-context.bldproj");
    require(window.saveProjectAs(path) && window.openProject(path) &&
                window.document().snapshot().entities() == finished.entities(),
            "relative wall context must survive reopening alongside the current wall geometry");
}

void test_precision_curve_editor_preserves_unedited_geometry() {
    for (const bool metric : {false, true}) {
        MainWindow window;
        prepare_window(window);
        window.setMetricUnits(metric);
        QApplication::setActiveWindow(&window);
        process_events();
        auto* drawing = canvas(window, QStringLiteral("measurementPlanCanvas"));
        send_click(*drawing, {0, 0});
        const auto before = window.document().snapshot();
        drive_boundary_modal(window, *drawing, Qt::Key_D, [&](QDialog* modal) {
            auto* input = dynamic_cast<sketch::desktop::BoundaryInputDialog*>(modal);
            require(input, "curve editor fixture needs a precise analytical arc");
            input->findChild<QComboBox*>("boundaryInputMethod")->setCurrentIndex(7);
            input->findChild<QLineEdit*>("boundaryInputTangent")->setText("13 deg");
            input->findChild<QLineEdit*>("boundaryInputArcLength")->setText("2.125 m");
            input->findChild<QLineEdit*>("boundaryInputSweep")->setText("37 deg");
            require(input->submit(), "precision-created arc must validate before opening its editor");
        });
        const auto wall = single_new_wall(before, window.document().snapshot());
        send_key(*drawing, Qt::Key_Escape);
        require(window.selectEntity(QString::fromStdString(wall.id)), "the exact curve must remain selectable");
        auto* edit = window.findChild<QAbstractButton*>("editCurvedWall");
        require(edit && edit->isEnabled(), "a selected curved wall must expose its curve editor");
        drive_boundary_modal(window, *drawing, 0, [&](QDialog* modal) {
            auto* sweep = modal->findChild<QLineEdit*>("curvedWallSweep");
            require(sweep && sketch::parse_angle(sweep->text().toStdString()).radians == wall_baseline(wall).sweep_radians,
                    "curve editor defaults must preserve the exact sweep of a precision-created wall");
            auto* buttons = modal->findChild<QDialogButtonBox*>("curvedWallButtons");
            require(buttons, "the actual curve editor must expose Apply");
            buttons->button(QDialogButtonBox::Apply)->click();
            require(modal->result() == QDialog::Accepted, "unchanged precise curve values must remain valid");
        }, "selected curve editor must open", Qt::NoModifier, [&] { edit->click(); });
        const auto after = window.document().snapshot().entities().at(wall.id);
        require(after.properties.at("baseline") == wall.properties.at("baseline"),
                "applying untouched displayed curve coordinates must not round the saved geometry");
        require(after.properties.at("original_drawing_input") == wall.properties.at("original_drawing_input") &&
                    after.properties.at("original_drawing_input_context") == wall.properties.at("original_drawing_input_context"),
                "curve editing must preserve the original input and its historical context");
    }
}

void test_wall_precision_preserves_pending_text_placement() {
    for (const bool start_during_modal : {false, true}) {
        MainWindow window;
        prepare_window(window);
        QApplication::setActiveWindow(&window);
        process_events();
        auto* drawing = canvas(window, QStringLiteral("measurementPlanCanvas"));
        const sketch::TextLibraryEntry entry{"guard-text", "Guard text", "Notes", "Guarded note", {}};
        const auto original = window.document().snapshot();
        if (start_during_modal) {
            drive_boundary_modal(window, *drawing, Qt::Key_D, [&](QDialog* modal) {
                auto* input = dynamic_cast<sketch::desktop::BoundaryInputDialog*>(modal);
                require(input, "placement guard fixture needs the Wall anchor form");
                input->findChild<QLineEdit*>("boundaryInputEndX")->setText("0 m");
                input->findChild<QLineEdit*>("boundaryInputEndY")->setText("0 m");
                require(window.beginTextPlacement(entry), "a pending text command must retain its own placement surface");
                require(input->submit(), "detached anchor input can validate while placement ownership changes");
            });
        } else {
            require(window.beginTextPlacement(entry), "the text placement fixture must start");
            bool unexpected_modal = false;
            QTimer::singleShot(0, &window, [&] {
                if (auto* modal = qobject_cast<QDialog*>(QApplication::activeModalWidget())) {
                    unexpected_modal = true;
                    modal->reject();
                }
            });
            send_key(*drawing, Qt::Key_D);
            require(!unexpected_modal, "D must not start Wall precision while text placement owns canvas clicks");
        }
        require_same_document(original, window.document().snapshot(), "competing precision input must change no document data");
        auto* input_panel = drawing->findChild<QWidget*>("drawingInputPanel");
        require(input_panel && !input_panel->isVisible(),
                "refused Wall precision must not install a wall chain over text placement");
        send_click(*drawing, {1, 1});
        const auto placed = window.document().snapshot();
        require(placed.revision() == original.revision() + 1,
                "the original text placement must still accept its next canvas click");
        bool found = false;
        for (const auto& [id, entity] : placed.entities()) {
            (void)id;
            require(entity.type != "wall", "the text placement click must not add a wall");
            if (entity.type != "annotation_state") continue;
            const auto annotations = sketch::decode_annotation_entity(entity);
            for (const auto& label : annotations.labels) found |= label.content == "Guarded note";
        }
        require(found, "the retained placement must commit the intended text label");
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
    window.setMetricUnits(true); // Preserve the exact four-square-metre area fixture.
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
    const auto angle_overlay = std::find_if(target->entities().begin(), target->entities().end(),
        [&](const auto& entity) { return entity.id == angle_id; });
    require(angle_overlay != target->entities().end() && !angle_overlay->dimension_end_ticks,
            "angle dimensions must not receive segment-length endpoint ticks");
    require(std::count_if(target->entities().begin(), target->entities().end(),
                [](const auto& entity) { return entity.dimension_end_ticks; }) == 4,
            "only the four automatic segment-length dimensions should render endpoint ticks");
    require(window.undoCommand() && window.redoCommand(),
            "semantic dimension creation must use normal undo and redo history");
}

void test_concave_room_label_stays_inside_room() {
    MainWindow window;
    prepare_window(window);
    const Boundary concave_room{
        {{0.0, 0.0}, {6.0, 0.0}, 0.0},
        {{6.0, 0.0}, {6.0, 2.0}, 0.0},
        {{6.0, 2.0}, {2.0, 2.0}, 0.0},
        {{2.0, 2.0}, {2.0, 6.0}, 0.0},
        {{2.0, 6.0}, {0.0, 6.0}, 0.0},
        {{0.0, 6.0}, {0.0, 0.0}, 0.0},
    };
    const auto room_id = window.createRoomBoundary(concave_room, QStringLiteral("L Room"));
    require(!room_id.isEmpty(), "concave room fixture must be creatable");
    const auto* target = canvas(window, QStringLiteral("measurementPlanCanvas"));
    const auto label = std::find_if(target->labels().begin(), target->labels().end(),
        [&](const auto& candidate) { return candidate.id == room_id; });
    require(label != target->labels().end() && label->text == QStringLiteral("L Room") &&
                label->plan_only && !label->show_background,
            "room projection must create a plain plan-owned label");
    require(!(label->position.x > 2.0 && label->position.y > 2.0),
            "concave room label must not be placed in the missing corner of its bounds");
}

void test_long_room_label_footprint_avoids_narrow_notch() {
    MainWindow window;
    prepare_window(window);
    const std::vector<Vec2> vertices{{0, 0}, {10, 0}, {10, 4}, {5.75, 4},
                                     {5.75, 1.7}, {5.7, 1.7}, {5.7, 4}, {0, 4}};
    Boundary boundary;
    QPainterPath room;
    room.moveTo(vertices.front().x, vertices.front().y);
    for (std::size_t i = 0; i < vertices.size(); ++i) {
        const auto end = vertices[(i + 1) % vertices.size()];
        boundary.push_back({vertices[i], end, 0.0});
        room.lineTo(end.x, end.y);
    }
    room.closeSubpath();
    const auto id = window.createRoomBoundary(boundary, QString(20, QLatin1Char('W')));
    require(!id.isEmpty(), "notched long-label room must be creatable");
    const auto* target = canvas(window, QStringLiteral("measurementPlanCanvas"));
    const auto label = std::find_if(target->labels().begin(), target->labels().end(),
        [&](const auto& value) { return value.id == id; });
    require(label != target->labels().end(), "long label must fit somewhere in the room");
    auto font = target->font();
    font.setPixelSize(16); // 0.20 m at the canvas's 80 px/m model layout scale.
    auto footprint = QFontMetricsF(font, target).boundingRect(label->text);
    footprint.moveCenter(QPointF());
    footprint.adjust(-5, -3, 5, 3);
    const QRectF model_rect(label->position.x + footprint.left() / 80.0,
                            label->position.y + footprint.top() / 80.0,
                            footprint.width() / 80.0, footprint.height() / 80.0);
    require(room.contains(model_rect),
            "entire font-aware long-label footprint must avoid the concave notch");
}

void test_room_label_avoids_components_and_retains_no_fit_names() {
    MainWindow window;
    prepare_window(window);
    const Boundary room{{{0, 0}, {8, 0}, 0}, {{8, 0}, {8, 6}, 0},
                        {{8, 6}, {0, 6}, 0}, {{0, 6}, {0, 0}, 0}};
    const auto id = window.createRoomBoundary(room, QStringLiteral("Office"));
    require(!id.isEmpty(), "component-overlap room fixture must be creatable");
    const auto symbol = window.createAnnotationSymbol(QStringLiteral("desk"), {4, 3});
    require(!symbol.isEmpty(), "room-label component fixture must be creatable");
    const auto* target = canvas(window, QStringLiteral("measurementPlanCanvas"));
    const auto label = std::find_if(target->labels().begin(), target->labels().end(),
        [&](const auto& value) { return value.id == id; });
    const auto component = std::find_if(target->entities().begin(), target->entities().end(),
        [&](const auto& value) { return value.id == symbol; });
    require(label != target->labels().end() && component != target->entities().end(),
            "room label and component must remain visible");
    const auto bounds = sketch::boundary_bounds(component->segments);
    auto font = target->font();
    font.setPixelSize(16);
    auto footprint = QFontMetricsF(font, target).boundingRect(label->text);
    footprint.moveCenter(QPointF());
    footprint.adjust(-5, -3, 5, 3);
    const QRectF label_rect(label->position.x + footprint.left() / 80.0,
                            label->position.y + footprint.top() / 80.0,
                            footprint.width() / 80.0, footprint.height() / 80.0);
    const QRectF component_rect(bounds.minimum.x - 0.1, bounds.minimum.y - 0.1,
        bounds.maximum.x - bounds.minimum.x + 0.2, bounds.maximum.y - bounds.minimum.y + 0.2);
    require(!label_rect.intersects(component_rect),
            "font-aware room placement must retain component clearance");
    const auto huge_id = window.createRoomBoundary(room, QString(500, QLatin1Char('W')));
    require(!huge_id.isEmpty(), "unplaceable label must not reject its valid owning room");
    const auto outside=std::find_if(target->labels().begin(),target->labels().end(),
        [&](const auto& value){return value.id==huge_id;});
    require(outside!=target->labels().end() && outside->text.compare(QString(500,QLatin1Char('W')),Qt::CaseInsensitive)==0 &&
            outside->leader_start && !outside->show_background,
        "a no-fit derived name must retain text outside its owner with a leader");
}

void test_appraisal_draw_category_survives_workflow_switches() {
    MainWindow window;
    prepare_window(window);
    auto* workflow = window.findChild<QComboBox*>(QStringLiteral("calculationWorkflow"));
    require(workflow != nullptr, "appraisal draw fixture needs a workflow selector");
    const auto appraisal = workflow->findData(QStringLiteral("appraisal"));
    const auto measurement = workflow->findData(QStringLiteral("measurement"));
    require(appraisal >= 0 && measurement >= 0,
            "area workflow selector must expose Measurement and Appraisal");
    workflow->setCurrentIndex(appraisal);
    require(window.beginBoundaryDrawing(BoundaryAuthoringMode::draw_first,
                                        QStringLiteral("garage")),
            "appraisal drawing must accept an explicit garage category");
    auto* target = canvas(window, QStringLiteral("measurementPlanCanvas"));
    target->setSnapEnabled(false); // Exact 4 m² geometry, displayed in Imperial units.
    send_click(*target, {0.0, 0.0});
    send_click(*target, {2.0, 0.0});
    send_click(*target, {2.0, 2.0});
    send_click(*target, {0.0, 2.0});
    send_key(*target, Qt::Key_Return);
    const auto id = window.selectedEntityId();
    require(!id.isEmpty(), "interactive appraisal drawing must commit a selected boundary");
    workflow->setCurrentIndex(measurement);
    auto switched = window.document().snapshot().entities().at(id.toStdString());
    require(switched.properties.at("classification") == "measurement" &&
                switched.properties.at("measurement_classification") == "measurement" &&
                switched.properties.at("appraisal_category") == "garage",
            "leaving Appraisal must preserve the interactively drawn category before restoring Measurement");
    workflow->setCurrentIndex(appraisal);
    auto* garage = window.findChild<QLabel*>(QStringLiteral("appraisalGarageTotal"));
    auto* gla = window.findChild<QLabel*>(QStringLiteral("appraisalGlaTotal"));
    require(garage && garage->text().contains(QStringLiteral("43.06")) &&
                gla && gla->text().contains(QStringLiteral("0.00")),
            "returning to Appraisal must restore the drawn garage category and automatic total");
}

void install_test_font() {
    const auto font_id = QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/Inter.ttf"));
    require(font_id >= 0, "boundary workflow test must load the bundled Inter font");
    const auto families = QFontDatabase::applicationFontFamilies(font_id);
    require(!families.isEmpty(), "bundled workflow font must expose a family");
    QApplication::setFont(QFont(families.front(), 10));
}

void test_adaptive_grid_boundary_commit() {
    MainWindow window;
    prepare_window(window);
    window.setMetricUnits(true);
    auto* drawing = canvas(window, QStringLiteral("measurementPlanCanvas"));
    auto* views = canvas(window, QStringLiteral("architecturalPlanCanvas"));
    require(std::abs(drawing->gridSpacingMetres() - 0.2) < 1e-12 &&
                std::abs(views->gridSpacingMetres() - 0.2) < 1e-12,
            "both workspaces must receive the metric measurement grid");
    require(window.beginBoundaryDrawing(BoundaryAuthoringMode::draw_first, "living"),
            "adaptive-grid workflow must start a measured area");
    send_click(*drawing, {0, 0});
    send_move_at_screen(*drawing, model_to_canvas(*drawing, {0.37, 0.01}));
    require(preview(*drawing).rubber_band &&
                same_point(preview(*drawing).rubber_band->end, {0.4, 0}),
            "metric cursor must preview a 20 cm grid point");
    send_click(*drawing, {0.37, 0.01});
    drawing->zoomBy(10.0, QRectF(drawing->rect()).center());
    require(std::abs(drawing->gridSpacingMetres() - 0.02) < 1e-12 &&
                same_point(preview(*drawing).segments.front().end, {0.4, 0}),
            "closer zoom must refine the grid without rounding an existing draft edge");
    send_click(*drawing, {0.435, 0.235});
    require(same_point(preview(*drawing).segments.back().end, {0.44, 0.24}),
            "closer metric node placement must use the displayed 2 cm interval");
    window.setMetricUnits(false);
    require(std::abs(drawing->gridSpacingMetres() - 0.0254) < 1e-12 &&
                std::abs(views->gridSpacingMetres() - 0.3048) < 1e-12 &&
                same_point(preview(*drawing).segments.back().end, {0.44, 0.24}),
            "unit changes must update both zooms without changing measured draft geometry");
    send_click(*drawing, {0, 0.23});
    send_key(*drawing, Qt::Key_Return);
    require(!drawing->boundaryDraftPreview(), "adaptive-grid measured area must close and commit");
    const auto boundary = sketch::decode_identified_boundary_entity(
        committed_boundary(window.document().snapshot()));
    require(boundary.segments.size() == 4 &&
                same_point(boundary.segments[0].segment.end, {0.4, 0}) &&
                same_point(boundary.segments[1].segment.end, {0.44, 0.24}) &&
                same_point(boundary.segments[2].segment.end, {0, 0.2286}),
            "committed point receipts must retain the actual grid coordinates at each zoom and unit");
}

}  // namespace

int main(int argc, char** argv) {
    sketch::testing::noninteractive_errors();
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    try {
        install_test_font();
        if (application.arguments().contains(QStringLiteral("--wall-placement-guard-only"))) {
            test_wall_precision_preserves_pending_text_placement();
            std::cout << "Wall precision placement guards passed\n";
            return 0;
        }
        if (application.arguments().contains(QStringLiteral("--wall-curve-editor-only"))) {
            test_precision_curve_editor_preserves_unedited_geometry();
            std::cout << "Precise curve editor workflow passed\n";
            return 0;
        }
        if (application.arguments().contains(QStringLiteral("--wall-precision-only"))) {
            test_physical_wall_precision_heading();
            test_physical_wall_precision_methods_and_persistence();
            test_physical_wall_precision_anchor_relative_and_modal_guards();
            test_precision_curve_editor_preserves_unedited_geometry();
            test_wall_precision_preserves_pending_text_placement();
            std::cout << "Physical wall precision workflows passed\n";
            return 0;
        }
        if (application.arguments().contains(QStringLiteral("--inline-drawing-only"))) {
            test_inline_measurement_held_enter_does_not_finish();
            test_inline_wall_cardinal_length_chain();
            test_inline_wall_validation_units_enter_and_keypad();
            test_inline_measurement_receipts_local_history_and_define_first();
            test_inline_wall_rejects_stale_source_and_selection();
            std::cout << "Inline drawing native UI workflows passed\n";
            return 0;
        }
        if (application.arguments().contains(QStringLiteral("--workspace-transitions-only"))) {
            test_workspace_switch_preserves_draft_on_refusal_and_allows_finished_drawing();
            test_unified_pointer_clicks_to_draw_and_drags_to_pan();
            std::cout << "Workspace transition workflows passed\n";
            return 0;
        }
        if (application.arguments().contains(QStringLiteral("--adaptive-grid-only"))) {
            test_adaptive_grid_boundary_commit();
            test_keyboard_only_boundary_authoring();
            std::cout << "Adaptive grid boundary workflows passed\n";
            return 0;
        }
        test_adaptive_grid_boundary_commit();
        if (application.arguments().contains(QStringLiteral("--keyboard-authoring-only"))) {
            test_keyboard_only_boundary_authoring(); return 0;
        }
        if (application.arguments().contains(QStringLiteral("--saved-draft-only"))) {
            test_saved_boundary_draft_resumes_without_unsaved_warning();
            return 0;
        }
        if (application.arguments().contains(QStringLiteral("--room-labels-only"))) {
            test_concave_room_label_stays_inside_room();
            test_long_room_label_footprint_avoids_narrow_notch();
            test_room_label_avoids_components_and_retains_no_fit_names();
            return 0;
        }
        const auto run_test = [](const char* name, void (*test)()) {
            try {
                test();
            } catch (const std::exception& error) {
                throw std::runtime_error(std::string(name) + ": " + error.what());
            }
        };
        run_test("unified_pointer_clicks_to_draw_and_drags_to_pan", test_unified_pointer_clicks_to_draw_and_drags_to_pan);
        run_test("inline_wall_cardinal_length_chain", test_inline_wall_cardinal_length_chain);
        run_test("physical_wall_precision_heading", test_physical_wall_precision_heading);
        run_test("physical_wall_precision_methods_and_persistence", test_physical_wall_precision_methods_and_persistence);
        run_test("physical_wall_precision_anchor_relative_and_modal_guards", test_physical_wall_precision_anchor_relative_and_modal_guards);
        run_test("precision_curve_editor_preserves_unedited_geometry", test_precision_curve_editor_preserves_unedited_geometry);
        run_test("wall_precision_preserves_pending_text_placement", test_wall_precision_preserves_pending_text_placement);
        run_test("inline_measurement_held_enter_does_not_finish", test_inline_measurement_held_enter_does_not_finish);
        run_test("inline_wall_validation_units_enter_and_keypad", test_inline_wall_validation_units_enter_and_keypad);
        run_test("inline_measurement_receipts_local_history_and_define_first", test_inline_measurement_receipts_local_history_and_define_first);
        run_test("inline_wall_rejects_stale_source_and_selection", test_inline_wall_rejects_stale_source_and_selection);
        run_test("draw_first_events_commit_receipts_labels_and_visibility", test_draw_first_events_commit_receipts_labels_and_visibility);
        run_test("define_first_events_place_manual_dimensions_and_close", test_define_first_events_place_manual_dimensions_and_close);
        run_test("workspace_switch_preserves_draft_on_refusal_and_allows_finished_drawing", test_workspace_switch_preserves_draft_on_refusal_and_allows_finished_drawing);
        run_test("escape_cancels_without_document_mutation", test_escape_cancels_without_document_mutation);
        run_test("context_change_discards_draft_without_mutating_document", test_context_change_discards_draft_without_mutating_document);
        run_test("precision_and_draw_first_classification_modals", test_precision_and_draw_first_classification_modals);
        run_test("keyboard_only_boundary_authoring", test_keyboard_only_boundary_authoring);
        run_test("saved_boundary_draft_resumes_without_unsaved_warning", test_saved_boundary_draft_resumes_without_unsaved_warning);
        run_test("off_grid_snap_cursor_rubberband_and_point_receipt_in_both_canvases", test_off_grid_snap_cursor_rubberband_and_point_receipt_in_both_canvases);
        run_test("off_grid_define_first_pending_dimension_preview_and_placement", test_off_grid_define_first_pending_dimension_preview_and_placement);
        run_test("off_grid_snapped_commit_in_each_workspace", test_off_grid_snapped_commit_in_each_workspace);
        run_test("receipt_boundary_offset_copy", test_receipt_boundary_offset_copy);
        run_test("dimension_presentation_editing", test_dimension_presentation_editing);
        run_test("semantic_angle_and_area_dimension_creation", test_semantic_angle_and_area_dimension_creation);
        run_test("appraisal_draw_category_survives_workflow_switches", test_appraisal_draw_category_survives_workflow_switches);
        run_test("concave_room_label_stays_inside_room", test_concave_room_label_stays_inside_room);
        run_test("long_room_label_footprint_avoids_narrow_notch", test_long_room_label_footprint_avoids_narrow_notch);
        run_test("room_label_avoids_components_and_retains_no_fit_names", test_room_label_avoids_components_and_retains_no_fit_names);
        std::cout << "Boundary workflow event tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "boundary_workflow_tests: " << error.what() << '\n';
        return 1;
    }
}
