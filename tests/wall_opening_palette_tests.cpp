#include "sketch/desktop/main_window.hpp"
#include "sketch/annotation_entity_codec.hpp"
#include "sketch/document.hpp"
#include "sketch/document_solid.hpp"
#include "sketch/document_schedule_adapter.hpp"
#include "../src/desktop/plan_canvas.hpp"
#include "support/noninteractive_errors.hpp"
#include <QApplication>
#include <QComboBox>
#include <QDragEnterEvent>
#include <QDir>
#include <QFontDatabase>
#include <QKeyEvent>
#include <QInputDialog>
#include <QTimer>
#include <QLineEdit>
#include <QMouseEvent>
#include <QDropEvent>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMimeData>
#include <QPdfDocument>
#include <QPushButton>
#include <QRectF>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QStandardPaths>
#include <QUuid>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

static void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

static bool same_point(sketch::Vec2 left, sketch::Vec2 right, double tolerance = 1e-8) {
    return std::hypot(left.x - right.x, left.y - right.y) <= tolerance;
}

static sketch::Wall read_wall(const sketch::Entity& entity) {
    std::vector<const sketch::Entity*> openings;
    sketch::Wall wall;
    std::string error;
    require(sketch::read_document_wall(entity, openings, wall, error),
            "semantic wall baseline decodes");
    return wall;
}

static std::vector<std::string> wall_ids(const sketch::DocumentSnapshot& snapshot) {
    std::vector<std::string> result;
    for (const auto& [id, entity] : snapshot.entities())
        if (entity.type == "wall") result.push_back(id);
    return result;
}

static std::string added_wall_id(const sketch::DocumentSnapshot& before,
                                 const sketch::DocumentSnapshot& after) {
    for (const auto& id : wall_ids(after))
        if (!before.entities().contains(id)) return id;
    throw std::runtime_error("mouse commit adds a semantic wall");
}

static std::size_t annotation_child_count(const sketch::DocumentSnapshot& snapshot) {
    std::size_t result = 0;
    for (const auto& [id, entity] : snapshot.entities()) {
        (void)id;
        if (entity.type != sketch::kAnnotationEntityType) continue;
        const auto state = sketch::decode_annotation_entity(entity);
        result += state.labels.size() + state.symbols.size();
    }
    return result;
}

int main(int argc, char** argv) {
    sketch::testing::noninteractive_errors();
    QStandardPaths::setTestModeEnabled(true);
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("Vertex-wall-opening-test-") + QUuid::createUuid().toString(QUuid::WithoutBraces));
    const auto font_id = QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/Inter.ttf"));
    if (font_id >= 0) QApplication::setFont(QFont(QFontDatabase::applicationFontFamilies(font_id).front(), 10));
    try {
        sketch::desktop::MainWindow window;
        window.resize(1500, 950);
        window.show();
        window.setWorkspace(sketch::desktop::Workspace::measurement);
        for (auto* tabs : window.findChildren<QTabWidget*>())
            for (int i = 0; i < tabs->count(); ++i)
                if (tabs->tabText(i).contains(QStringLiteral("Library"), Qt::CaseInsensitive)) tabs->setCurrentIndex(i);
        QApplication::processEvents();
        auto* canvas = dynamic_cast<sketch::desktop::PlanCanvas*>(window.findChild<QWidget*>(QStringLiteral("measurementPlanCanvas")));
        require(canvas != nullptr, "measurement canvas exists");
        const auto field = [&](const char* name) {
            auto* edit = window.findChild<QLineEdit*>(QString::fromLatin1(name));
            require(edit != nullptr, "palette dimension field exists");
            return edit;
        };
        const auto action = [&](const char* name) {
            auto* button = window.findChild<QPushButton*>(QString::fromLatin1(name));
            require(button && button->isVisible(), "wall/opening action is visible in Library");
            button->click();
        };
        const auto mouse = [&](QEvent::Type type, QPointF point, Qt::MouseButton button, Qt::MouseButtons buttons) {
            QMouseEvent event(type, point, canvas->mapToGlobal(point.toPoint()), button, buttons, Qt::NoModifier);
            QApplication::sendEvent(canvas, &event);
        };
        const auto click = [&](QPointF point) {
            mouse(QEvent::MouseMove, point, Qt::NoButton, Qt::NoButton);
            mouse(QEvent::MouseButtonPress, point, Qt::LeftButton, Qt::LeftButton);
            mouse(QEvent::MouseButtonRelease, point, Qt::LeftButton, Qt::NoButton);
        };
        const auto model_to_canvas = [&](sketch::Vec2 point) {
            const auto center = canvas->viewCenter();
            const auto scale = canvas->viewScale();
            const auto viewport = QRectF(canvas->rect());
            return QPointF(viewport.center().x() + (point.x - center.x) * scale,
                           viewport.center().y() - (point.y - center.y) * scale);
        };
        const auto drop_catalog_symbol = [](sketch::desktop::PlanCanvas* target_canvas,
                                            const QString& symbol_id,
                                            double scale,
                                            QPointF point) {
            QMimeData mime;
            mime.setData("application/x-vertex-symbol",
                         QJsonDocument(QJsonObject{{QStringLiteral("id"), symbol_id},
                                                   {QStringLiteral("scale"), scale}})
                             .toJson(QJsonDocument::Compact));
            QDragEnterEvent enter(point.toPoint(), Qt::CopyAction, &mime,
                                  Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(target_canvas, &enter);
            require(enter.isAccepted(), "plan canvas accepts the catalog symbol drag payload");
            QDropEvent drop(point, Qt::CopyAction, &mime,
                            Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(target_canvas, &drop);
            require(drop.isAccepted(), "plan canvas accepts the catalog symbol drop");
        };
        const auto capture_directory = qEnvironmentVariable("VERTEX_TEST_CAPTURE_DIR");
        const auto save_capture = [&](QWidget& widget, const QString& file_name) {
            if (capture_directory.isEmpty()) return;
            require(QDir().mkpath(capture_directory), "wall workflow capture directory exists");
            require(widget.grab().save(QDir(capture_directory).filePath(file_name)),
                    "wall workflow capture saves");
        };
        auto* drawing_mode = window.findChild<QComboBox*>(QStringLiteral("drawingMode"));
        require(drawing_mode && drawing_mode->isVisible(), "common drawing mode is visible in the plan workspace");
        require(drawing_mode->accessibleName() == QStringLiteral("Drawing mode") &&
                    drawing_mode->currentData().toString() == QStringLiteral("wall"),
                "a fresh idle plan defaults to the physical Wall mode");
        field("wallDrawThickness")->setText(QStringLiteral("240 mm"));
        field("wallDrawHeight")->setText(QStringLiteral("2.8 m"));
        window.setMetricUnits(true);
        QApplication::processEvents();
        require(drawing_mode->currentData().toString() == QStringLiteral("wall"),
                "switching to the conventional plan keeps Wall as the fresh default");
        save_capture(window, QStringLiteral("wall-mode-full-window.png"));

        // An ordinary idle click in the default Wall mode starts a physical
        // wall draft. There is no Library action or explicit wall-tool call.
        click({150, 200});
        require(wall_ids(window.document().snapshot()).empty(),
                "the first idle click anchors a wall draft without prematurely committing it");
        mouse(QEvent::MouseMove, {630, 200}, Qt::NoButton, Qt::NoButton);
        require(canvas->wallPreview().has_value(), "wall pointer motion shows a thickness and length preview");
        const auto first_preview = *canvas->wallPreview();
        require(std::abs(first_preview.thickness_metres - 0.24) < 1e-10 &&
                    !first_preview.dimension_text.isEmpty() && first_preview.dimension_text.endsWith(QLatin1Char('m')),
                "live wall preview shows the palette thickness and a unit-bearing metric length");
        save_capture(*canvas, QStringLiteral("wall-mode-live-preview.png"));
        // The second click lands at the preview target and commits that same
        // analytical centerline into the document.
        click({630, 200});
        auto first_walls = window.document().snapshot();
        require(wall_ids(first_walls).size() == 1, "two idle canvas clicks commit one physical wall");
        const auto first_wall_id = wall_ids(first_walls).front();
        auto first_wall = read_wall(first_walls.entities().at(first_wall_id));
        require(same_point(first_wall.baseline.start, first_preview.start) &&
                    same_point(first_wall.baseline.end, first_preview.end),
                "committed wall baseline exactly matches its visible preview");
        require(std::abs(first_walls.entities().at(first_wall_id).properties.at("thickness_m").get<double>() - 0.24) < 1e-10 &&
                    std::abs(first_walls.entities().at(first_wall_id).properties.at("height_m").get<double>() - 2.8) < 1e-10,
                "default Wall mode creates the physical wall using palette units");

        const auto first_label = std::find_if(canvas->labels().begin(), canvas->labels().end(),
            [&](const auto& label) { return label.id.toStdString() == first_wall_id; });
        require(first_label != canvas->labels().end() && first_label->plan_only,
                "a committed wall receives a derived plan label keyed to its semantic wall");
        require(first_label->text == first_preview.dimension_text &&
                    first_label->text.endsWith(QLatin1Char('m')),
                "committed wall label retains the measured length and its metric unit");
        const auto first_label_text = first_label->text;
        const auto first_label_position = first_label->position;
        const auto wall_length = sketch::segment_length(first_wall.baseline);
        const auto wall_midpoint = sketch::Vec2{
            (first_wall.baseline.start.x + first_wall.baseline.end.x) * 0.5,
            (first_wall.baseline.start.y + first_wall.baseline.end.y) * 0.5};
        const auto wall_normal = sketch::Vec2{
            -(first_wall.baseline.end.y - first_wall.baseline.start.y) / wall_length,
            (first_wall.baseline.end.x - first_wall.baseline.start.x) / wall_length};
        const auto label_offset = first_wall.thickness * 0.5 + 0.14;
        require(same_point(first_label_position,
                    {wall_midpoint.x + wall_normal.x * label_offset,
                     wall_midpoint.y + wall_normal.y * label_offset}),
                "derived wall length label is positioned in model coordinates beside its baseline");

        QTemporaryDir label_output_directory;
        require(label_output_directory.isValid(), "temporary wall label output directory exists");
        const auto label_pdf_path = label_output_directory.filePath(QStringLiteral("wall-length-label.pdf"));
        require(window.exportDraftPdf(label_pdf_path), "committed wall label exports through the ordinary plan PDF path");
        QPdfDocument label_pdf;
        require(label_pdf.load(label_pdf_path) == QPdfDocument::Error::None && label_pdf.pageCount() > 0,
                "wall label PDF reopens as a plan page");
        require(label_pdf.getAllText(0).text().simplified().contains(first_label_text.simplified()),
                "ordinary plan PDF retains the committed wall length and units");

        // Continue the chain through two more segments and ensure every
        // preview target is the geometry retained by the corresponding wall.
        const auto first_end_screen = model_to_canvas(first_wall.baseline.end);
        const QPointF second_target = first_end_screen + QPointF(0, 140);
        const auto before_second = window.document().snapshot();
        mouse(QEvent::MouseMove, second_target, Qt::NoButton, Qt::NoButton);
        require(canvas->wallPreview().has_value(), "continued wall chain previews the next connected segment");
        const auto second_preview = *canvas->wallPreview();
        require(same_point(second_preview.start, first_wall.baseline.end),
                "continued wall starts at the prior committed endpoint");
        click(second_target);
        const auto after_second = window.document().snapshot();
        const auto second_wall_id = added_wall_id(before_second, after_second);
        const auto second_wall = read_wall(after_second.entities().at(second_wall_id));
        require(same_point(second_wall.baseline.start, second_preview.start) &&
                    same_point(second_wall.baseline.end, second_preview.end),
                "second committed wall matches its preview geometry");
        const auto aligned_corner = sketch::Vec2{first_wall.baseline.start.x,
                                                  second_wall.baseline.end.y};
        mouse(QEvent::MouseMove, model_to_canvas(aligned_corner) + QPointF(8, -3),
              Qt::NoButton, Qt::NoButton);
        require(canvas->wallPreview() && same_point(canvas->wallPreview()->end, aligned_corner),
                "a rectangle corner resolves both endpoint alignment and the current wall axis");
        const QPointF third_target = second_target + QPointF(140, 0);
        const auto before_third = window.document().snapshot();
        mouse(QEvent::MouseMove, third_target, Qt::NoButton, Qt::NoButton);
        require(canvas->wallPreview().has_value(), "wall chain remains active after its second committed segment");
        const auto third_preview = *canvas->wallPreview();
        require(same_point(third_preview.start, second_wall.baseline.end),
                "third connected wall starts at the second wall endpoint");
        click(third_target);
        const auto after_third = window.document().snapshot();
        const auto third_wall_id = added_wall_id(before_third, after_third);
        const auto third_wall = read_wall(after_third.entities().at(third_wall_id));
        require(same_point(third_wall.baseline.start, third_preview.start) &&
                    same_point(third_wall.baseline.end, third_preview.end) &&
                    same_point(third_wall.baseline.start, second_wall.baseline.end),
                "third commit preserves the connected wall chain and preview geometry");

        QKeyEvent escape_wall_chain(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
        QApplication::sendEvent(canvas, &escape_wall_chain);
        require(window.selectEntity(QString::fromStdString(third_wall_id)),
                "connected wall selects before the next idle gesture");
        const auto before_snap_wall = window.document().snapshot();
        const auto start_point = sketch::Vec2{first_wall.baseline.start.x,
                                               first_wall.baseline.start.y - 2.0};
        const auto start_screen = model_to_canvas(start_point);
        click(start_screen);
        require(window.selectedEntityId().isEmpty() &&
                    wall_ids(window.document().snapshot()).size() == wall_ids(before_snap_wall).size(),
                "first empty click deselects the current wall before drawing can begin");
        click(start_screen);
        require(wall_ids(window.document().snapshot()).size() == wall_ids(before_snap_wall).size(),
                "second empty click anchors a new wall without adding geometry");
        const QPointF near_first_endpoint = model_to_canvas(first_wall.baseline.start) + QPointF(5, -4);
        mouse(QEvent::MouseMove, near_first_endpoint, Qt::NoButton, Qt::NoButton);
        require(canvas->wallPreview().has_value(), "endpoint snap remains visible in a live wall preview");
        const auto endpoint_preview = *canvas->wallPreview();
        require(same_point(endpoint_preview.end, first_wall.baseline.start) &&
                    !endpoint_preview.dimension_text.isEmpty(),
                "wall endpoint snapping moves the preview target onto the analytical endpoint");
        save_capture(*canvas, QStringLiteral("wall-endpoint-snap-preview.png"));
        click(near_first_endpoint);
        const auto after_endpoint_snap = window.document().snapshot();
        const auto endpoint_wall_id = added_wall_id(before_snap_wall, after_endpoint_snap);
        const auto endpoint_wall = read_wall(after_endpoint_snap.entities().at(endpoint_wall_id));
        require(same_point(endpoint_wall.baseline.start, endpoint_preview.start) &&
                    same_point(endpoint_wall.baseline.end, endpoint_preview.end) &&
                    same_point(endpoint_wall.baseline.end, first_wall.baseline.start),
                "endpoint-snapped wall commits the same snapped preview geometry");

        const auto second_baseline_midpoint = sketch::Vec2{
            (second_wall.baseline.start.x + second_wall.baseline.end.x) * 0.5,
            (second_wall.baseline.start.y + second_wall.baseline.end.y) * 0.5};
        const auto second_baseline_length = sketch::segment_length(second_wall.baseline);
        const auto second_baseline_normal = sketch::Vec2{
            -(second_wall.baseline.end.y - second_wall.baseline.start.y) / second_baseline_length,
            (second_wall.baseline.end.x - second_wall.baseline.start.x) / second_baseline_length};
        const auto off_wall_distance = std::min(0.10, 8.0 / canvas->viewScale());
        const auto off_wall_target = sketch::Vec2{
            second_baseline_midpoint.x + second_baseline_normal.x * off_wall_distance,
            second_baseline_midpoint.y + second_baseline_normal.y * off_wall_distance};
        const auto before_on_wall_snap = window.document().snapshot();
        const auto off_wall_screen = model_to_canvas(off_wall_target);
        mouse(QEvent::MouseMove, off_wall_screen, Qt::NoButton, Qt::NoButton);
        require(canvas->wallPreview().has_value(), "on-wall snap remains visible in a live wall preview");
        const auto on_wall_preview = *canvas->wallPreview();
        require(same_point(on_wall_preview.start, first_wall.baseline.start) &&
                    same_point(on_wall_preview.end, second_baseline_midpoint),
                "wall preview projects its endpoint onto the existing wall baseline");
        save_capture(*canvas, QStringLiteral("wall-on-wall-snap-preview.png"));
        click(off_wall_screen);
        const auto after_on_wall_snap = window.document().snapshot();
        const auto on_wall_id = added_wall_id(before_on_wall_snap, after_on_wall_snap);
        const auto on_wall = read_wall(after_on_wall_snap.entities().at(on_wall_id));
        require(same_point(on_wall.baseline.start, on_wall_preview.start) &&
                    same_point(on_wall.baseline.end, on_wall_preview.end) &&
                    same_point(on_wall.baseline.end, second_baseline_midpoint),
                "on-wall-snapped segment commits the exact projected preview target");
        QApplication::sendEvent(canvas, &escape_wall_chain);

        require(window.selectEntity(QString::fromStdString(on_wall_id)),
                "connected wall selects before the empty-drag gesture");
        const auto pan_before = canvas->viewCenter();
        const auto walls_before_pan = wall_ids(window.document().snapshot()).size();
        const QPointF pan_start(canvas->width() * 0.10, canvas->height() * 0.90);
        const QPointF pan_end = pan_start + QPointF(28, -17);
        mouse(QEvent::MouseButtonPress, pan_start, Qt::LeftButton, Qt::LeftButton);
        mouse(QEvent::MouseMove, pan_end, Qt::NoButton, Qt::LeftButton);
        mouse(QEvent::MouseButtonRelease, pan_end, Qt::LeftButton, Qt::NoButton);
        const auto pan_after = canvas->viewCenter();
        require(!same_point(pan_before, pan_after) &&
                    wall_ids(window.document().snapshot()).size() == walls_before_pan,
                "empty canvas drag pans the view without changing selected wall geometry");
        save_capture(*canvas, QStringLiteral("wall-final-connected-walls.png"));

        const auto wall_count_before_measurement = wall_ids(window.document().snapshot()).size();
        std::size_t area_count_before_measurement = 0;
        const auto before_measurement = window.document().snapshot();
        for (const auto& [id, entity] : before_measurement.entities()) {
            (void)id;
            if (entity.type == "measurement_boundary") ++area_count_before_measurement;
        }
        const auto measurement_index = drawing_mode->findData(QStringLiteral("measurement"));
        require(measurement_index >= 0, "drawing mode offers an explicit Measurement choice");
        drawing_mode->setCurrentIndex(measurement_index);
        require(drawing_mode->currentData().toString() == QStringLiteral("measurement"),
                "explicit Measurement choice is active");
        const auto area_left = canvas->width() * 0.12;
        const auto area_top = canvas->height() * 0.66;
        const auto area_width = std::min(160.0, canvas->width() * 0.18);
        const auto area_height = std::min(140.0, canvas->height() * 0.18);
        const QPointF area_p0(area_left, area_top);
        const QPointF area_p1(area_left + area_width, area_top);
        const QPointF area_p2(area_left + area_width, area_top + area_height);
        const QPointF area_p3(area_left, area_top + area_height);
        require(area_p2.x() < canvas->width() - 10 && area_p2.y() < canvas->height() - 10,
                "measurement acceptance rectangle fits inside the live canvas");
        if (!window.selectedEntityId().isEmpty()) {
            click(area_p0);
            require(window.selectedEntityId().isEmpty(),
                    "mode change retains the deselect-first canvas interaction");
        }
        click(area_p0);
        click(area_p1);
        click(area_p2);
        click(area_p3);
        bool classification_accepted = false;
        QTimer::singleShot(0, &window, [&] {
            for (auto* widget : QApplication::topLevelWidgets()) {
                auto* dialog = qobject_cast<QInputDialog*>(widget);
                if (!dialog || dialog->objectName() != QStringLiteral("boundaryClassificationDialog")) continue;
                classification_accepted = true;
                dialog->accept();
                break;
            }
        });
        QKeyEvent close_measurement(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
        QApplication::sendEvent(canvas, &close_measurement);
        require(classification_accepted,
                "measurement closes through the user-facing area classification dialog");
        const auto after_measurement = window.document().snapshot();
        std::size_t area_count_after_measurement = 0;
        for (const auto& [id, entity] : after_measurement.entities()) {
            (void)id;
            if (entity.type == "measurement_boundary") ++area_count_after_measurement;
        }
        require(area_count_after_measurement == area_count_before_measurement + 1 &&
                    wall_ids(after_measurement).size() == wall_count_before_measurement,
                "explicit Measurement commits an area without creating an additional wall");
        std::string measured_area_id;
        for (const auto& [id, entity] : after_measurement.entities())
            if (entity.type == "measurement_boundary" && !before_measurement.entities().contains(id))
                measured_area_id = id;
        require(!measured_area_id.empty(), "explicit Measurement stores a semantic area boundary");
        const auto area_projection = std::find_if(canvas->entities().begin(), canvas->entities().end(),
            [&](const auto& entity) { return entity.id.toStdString() == measured_area_id; });
        require(area_projection != canvas->entities().end() && area_projection->segments.size() == 4,
                "measurement clicks retain a four-edge area on the plan canvas");
        const auto area_bounds = sketch::boundary_bounds(area_projection->segments);
        require(area_bounds.maximum.x > area_bounds.minimum.x &&
                    area_bounds.maximum.y > area_bounds.minimum.y,
                "explicit Measurement creates a nonzero measured area");
        const auto wall_mode_index = drawing_mode->findData(QStringLiteral("wall"));
        require(wall_mode_index >= 0, "drawing mode retains the physical Wall choice");
        drawing_mode->setCurrentIndex(wall_mode_index);

        // Keep the established Library wall authoring path and palette fields.
        window.setWorkspace(sketch::desktop::Workspace::architectural);
        auto* views = window.findChild<QComboBox*>(QStringLiteral("architecturalView"));
        if (views && views->count() > 1) views->setCurrentIndex(1);
        action("libraryWall");
        require(window.workspace() == sketch::desktop::Workspace::measurement,
                "wall authoring returns a projected workspace to conventional plan coordinates");
        const QPointF library_wall_start(canvas->width() * 0.38, canvas->height() * 0.72);
        const QPointF library_wall_end(canvas->width() * 0.78, canvas->height() * 0.72);
        const auto before_library_wall = window.document().snapshot();
        click(library_wall_start);
        click(library_wall_end);
        const auto wall_snapshot = window.document().snapshot();
        const auto wall_id = added_wall_id(before_library_wall, wall_snapshot);
        require(std::abs(wall_snapshot.entities().at(wall_id).properties.at("thickness_m").get<double>() - 0.24) < 1e-10, "Library wall thickness comes from palette units");
        require(std::abs(wall_snapshot.entities().at(wall_id).properties.at("height_m").get<double>() - 2.8) < 1e-10, "Library wall height comes from palette units");
        require(wall_ids(wall_snapshot).size() == wall_count_before_measurement + 1,
                "two Library wall clicks create a separate semantic wall in 2D");
        auto hosted_wall = read_wall(wall_snapshot.entities().at(wall_id));
        bool footprint = false;
        for (const auto& entity : canvas->entities()) {
            if (entity.id.toStdString() != wall_id) continue;
            require(entity.segments.size() == 4, "2D wall retains four physical outline edges");
            const auto bounds = sketch::boundary_bounds(entity.segments);
            footprint = std::abs(bounds.maximum.y - bounds.minimum.y - 0.24) < 1e-10;
        }
        require(footprint, "retained wall footprint has the exact physical thickness");

        // Preserve the hybrid idle gestures: a blank drag pans, while dragging
        // a selected wall changes its model geometry through the canvas path.
        QKeyEvent finish_library_wall(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
        QApplication::sendEvent(canvas, &finish_library_wall);
        require(window.selectEntity(QString::fromStdString(wall_id)),
                "Library wall can be selected before checking idle gestures");
        for (const auto& label : canvas->labels())
            require(!label.plan_only || !label.selected,
                    "derived wall labels do not acquire separate selection controls");
        // Stay above the interactive overview map in the bottom-right corner.
        const QPointF blank_point(canvas->width() * 0.90, canvas->height() * 0.50);
        click(blank_point);
        require(window.selectedEntityId().isEmpty() &&
                    wall_ids(window.document().snapshot()).size() == wall_ids(wall_snapshot).size(),
                "first blank click deselects the selected Library wall");
        click(model_to_canvas({(hosted_wall.baseline.start.x + hosted_wall.baseline.end.x) * 0.5,
                               (hosted_wall.baseline.start.y + hosted_wall.baseline.end.y) * 0.5}));
        require(window.selectedEntityId().toStdString() == wall_id,
                "idle canvas click selects the physical wall");
        const auto wall_before_move_snapshot = window.document().snapshot();
        const auto wall_before_move = read_wall(wall_before_move_snapshot.entities().at(wall_id));
        const auto move_start = model_to_canvas({(wall_before_move.baseline.start.x + wall_before_move.baseline.end.x) * 0.5,
                                                  (wall_before_move.baseline.start.y + wall_before_move.baseline.end.y) * 0.5});
        const auto move_end = move_start + QPointF(24, -18);
        mouse(QEvent::MouseButtonPress, move_start, Qt::LeftButton, Qt::LeftButton);
        mouse(QEvent::MouseMove, move_end, Qt::NoButton, Qt::LeftButton);
        mouse(QEvent::MouseButtonRelease, move_end, Qt::LeftButton, Qt::NoButton);
        hosted_wall = read_wall(window.document().snapshot().entities().at(wall_id));
        require(!same_point(hosted_wall.baseline.start, wall_before_move.baseline.start) &&
                    std::abs(sketch::segment_length(hosted_wall.baseline) -
                             sketch::segment_length(wall_before_move.baseline)) < 1e-8,
                "dragging a selected wall moves its physical baseline without changing its length");

        const auto hosted_point = [&](double fraction) {
            return model_to_canvas({hosted_wall.baseline.start.x +
                                        (hosted_wall.baseline.end.x - hosted_wall.baseline.start.x) * fraction,
                                    hosted_wall.baseline.start.y +
                                        (hosted_wall.baseline.end.y - hosted_wall.baseline.start.y) * fraction});
        };
        const auto hosted_door_point = hosted_point(0.27);
        const auto hosted_window_point = hosted_point(0.73);
        const auto hosted_doorway_point = hosted_point(0.50);
        const auto door_symbol_id = QStringLiteral("svg-v2-09_doors-door-hinged-760-left");
        const auto window_symbol_id = QStringLiteral("svg-v2-10_windows-window-casement");
        const auto annotations_before_catalog_openings = annotation_child_count(window.document().snapshot());
        drop_catalog_symbol(canvas, door_symbol_id, 1.0, hosted_door_point);
        drop_catalog_symbol(canvas, window_symbol_id, 1.0, hosted_window_point);
        int opening_count = 0;
        const auto opening_snapshot = window.document().snapshot();
        std::set<std::string> opening_kinds;
        std::string hosted_door_id;
        std::string hosted_window_id;
        for (const auto& [id, entity] : opening_snapshot.entities()) {
            if (entity.type != "opening") continue;
            ++opening_count;
            require(entity.properties.at("wall_id") == wall_id, "opening is hosted by the drawn wall");
            require(entity.properties.at("offset_m").get<double>() > 0, "opening stores click-derived host offset");
            const auto kind = entity.properties.at("opening_kind").get<std::string>();
            opening_kinds.insert(kind);
            const auto catalog_id = entity.properties.at("catalog_symbol_id").get<std::string>();
            const auto width = entity.properties.at("width_m").get<double>();
            if (kind == "door") {
                hosted_door_id = id;
                require(catalog_id == door_symbol_id.toStdString(),
                        "hosted door retains its catalog symbol identity");
                require(std::abs(width - 0.760) < 1e-10,
                        "760 mm door uses its leaf opening width, not the 880 mm artwork footprint");
            } else if (kind == "window") {
                hosted_window_id = id;
                require(catalog_id == window_symbol_id.toStdString(),
                        "hosted window retains its catalog symbol identity");
                require(std::abs(width - 1.2) < 1e-10,
                        "hosted window width matches its sourced catalog width");
            }
        }
        require(opening_count == 2 && opening_kinds.contains("door") && opening_kinds.contains("window") &&
                    !hosted_door_id.empty() && !hosted_window_id.empty(),
                "catalog door and window drops commit their hosted opening kinds");
        require(annotation_child_count(opening_snapshot) == annotations_before_catalog_openings,
                "catalog door and window drops add no annotation symbols");
        click(hosted_window_point);
        require(window.selectedEntityId().toStdString() == hosted_window_id,
                "clicking the catalog window center selects its hosted opening");
        click(hosted_point(0.05));
        require(window.selectedEntityId().toStdString() == wall_id,
                "clicking the remaining wall body selects the wall around hosted openings");
        {
            sketch::desktop::MainWindow unhosted_window;
            unhosted_window.resize(1000, 700);
            unhosted_window.show();
            unhosted_window.setWorkspace(sketch::desktop::Workspace::measurement);
            QApplication::processEvents();
            auto* unhosted_canvas = dynamic_cast<sketch::desktop::PlanCanvas*>(
                unhosted_window.findChild<QWidget*>(QStringLiteral("measurementPlanCanvas")));
            require(unhosted_canvas != nullptr, "unhosted-drop measurement canvas exists");
            const auto revision_before_unhosted_drop = unhosted_window.document().revision();
            drop_catalog_symbol(unhosted_canvas, door_symbol_id, 1.0,
                                QRectF(unhosted_canvas->rect()).center());
            require(unhosted_window.document().revision() == revision_before_unhosted_drop,
                    "catalog opening drop without a host leaves the document unchanged");
            require(unhosted_window.lastError().contains(QStringLiteral("needs a host wall"),
                                                         Qt::CaseInsensitive),
                    "catalog opening drop without a host reports a useful error");
        }
        action("libraryDoorway");
        require(field("openingDrawSill")->text() == QStringLiteral("0 m"), "doorway defaults to floor level");
        click(hosted_doorway_point);
        const auto doorway_id = window.selectedEntityId().toStdString();
        const auto doorway_snapshot = window.document().snapshot();
        const auto& doorway = doorway_snapshot.entities().at(doorway_id);
        require(doorway.type == "opening" && doorway.properties.at("opening_kind") == "opening",
                "doorway action creates a bare semantic opening");
        require(!doorway.properties.contains("opening_assembly") && !doorway.properties.contains("door_operation"),
                "bare doorway has no frame, glazing or door leaf");
        bool doorway_symbol = false;
        for (const auto& entity : canvas->entities())
            if (entity.id.toStdString() == doorway_id)
                doorway_symbol = entity.segments.size() == 3 && !entity.filled;
        require(doorway_symbol, "bare doorway has selectable unfilled jamb and threshold geometry");
        click(hosted_doorway_point);
        require(window.selectedEntityId().toStdString() == doorway_id, "doorway is selectable on the actual plan canvas");
        require(window.undoCommand(), "bare doorway placement is undoable");
        require(!window.document().snapshot().entities().contains(doorway_id), "undo removes bare doorway");
        require(window.redoCommand(), "bare doorway placement is redoable");
        require(window.selectEntity(QString::fromStdString(doorway_id)), "restored doorway selects");
        require(window.editSelectedLength(QStringLiteral("800 mm")), "bare doorway width is editable");
        require(std::abs(window.document().snapshot().entities().at(doorway_id).properties.at("width_m").get<double>() - 0.8) < 1e-10,
                "doorway dimensions use the ordinary semantic inspector");
        const auto verify_geometry = [&](const sketch::DocumentSnapshot& snapshot) {
            std::vector<const sketch::Entity*> openings;
            double void_volume = 0;
            for (const auto& [id, entity] : snapshot.entities()) {
                if (entity.type != "opening" || entity.properties.at("wall_id") != wall_id) continue;
                openings.push_back(&entity);
                void_volume += entity.properties.at("width_m").get<double>() *
                    entity.properties.at("height_m").get<double>() * 0.24;
            }
            sketch::Wall wall;
            std::string error;
            require(sketch::read_document_wall(snapshot.entities().at(wall_id), openings, wall, error),
                    "wall and all hosted voids decode");
            const auto expected = sketch::segment_length(wall.baseline) * wall.thickness * wall.height - void_volume;
            require(std::abs(sketch::solid_volume(sketch::make_wall(wall)) - expected) < 1e-7,
                    "derived 3D wall subtracts the bare doorway and other openings");
            const auto schedules = sketch::build_document_schedules(snapshot);
            for (const auto& row : schedules.snapshot.rows)
                require(row.object_id != doorway_id, "bare doorway does not become a door or window schedule row");
            for (const auto& diagnostic : schedules.diagnostics)
                require(diagnostic.find(doorway_id) == std::string::npos, "valid bare doorway has no schedule diagnostic");
        };
        verify_geometry(window.document().snapshot());
        const auto revision = window.document().revision();
        action("libraryWindow");
        click(hosted_window_point);
        require(window.document().revision() == revision, "overlapping opening fails admission without mutation");
        QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
        QApplication::sendEvent(canvas, &escape);
        require(window.undoCommand(), "opening placement is undoable");
        require(window.redoCommand(), "opening placement is redoable");
        QTemporaryDir directory;
        require(directory.isValid(), "temporary archive directory exists");
        const auto path = directory.filePath(QStringLiteral("wall-openings.bldproj"));
        require(window.saveProjectAs(path), "wall and openings save");
        require(window.openProject(path), "wall and openings reopen");
        opening_count = 0;
        const auto reopened = window.document().snapshot();
        for (const auto& [id, entity] : reopened.entities())
            if (entity.type == "opening" && entity.properties.at("wall_id") == wall_id) ++opening_count;
        require(opening_count == 3, "reopened archive retains door, window and bare doorway");
        require(reopened.entities().at(doorway_id).properties.at("opening_kind") == "opening" &&
                    !reopened.entities().at(doorway_id).properties.contains("opening_assembly"),
                "reopened doorway remains a bare void");
        verify_geometry(reopened);
        if (!capture_directory.isEmpty()) {
            require(QDir().mkpath(capture_directory), "capture directory exists");
            require(!window.createAnnotationSymbol(QStringLiteral("svg-v2-02_kitchen-base-cabinet-600"), {-3, 0}).isEmpty(),
                    "floor cabinet places at its sourced physical size");
            require(!window.createAnnotationSymbol(QStringLiteral("svg-v2-02_kitchen-wall-cabinet-900"), {-1.8, 0}).isEmpty(),
                    "overhead cabinet places at its sourced physical size");
            require(!window.createAnnotationSymbol(QStringLiteral("svg-v2-02_kitchen-fridge-french-door"), {0, 0}).isEmpty(),
                    "refrigerator places at its sourced physical size");
            require(!window.createAnnotationSymbol(QStringLiteral("svg-v2-04_living-sofa-three-seat"), {2.5, -1.5}).isEmpty(),
                    "restyled sofa places with exact artwork");
            canvas->fitView();
            QApplication::processEvents();
            require(window.grab().save(QDir(capture_directory).filePath(QStringLiteral("wall-opening-library.png"))),
                    "full Library workflow screenshot saves");
        }
        std::cout << "Wall/opening Library workflow passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
