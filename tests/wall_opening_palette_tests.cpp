#include "sketch/desktop/main_window.hpp"
#include "sketch/document.hpp"
#include "sketch/document_solid.hpp"
#include "sketch/document_schedule_adapter.hpp"
#include "../src/desktop/plan_canvas.hpp"
#include "support/noninteractive_errors.hpp"
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QFontDatabase>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPushButton>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QStandardPaths>
#include <QUuid>
#include <cmath>
#include <iostream>
#include <stdexcept>

static void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
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
        canvas->setSnapEnabled(false);
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
        field("wallDrawThickness")->setText(QStringLiteral("240 mm"));
        field("wallDrawHeight")->setText(QStringLiteral("2.8 m"));
        window.setWorkspace(sketch::desktop::Workspace::architectural);
        auto* views = window.findChild<QComboBox*>(QStringLiteral("architecturalView"));
        if (views && views->count() > 1) views->setCurrentIndex(1);
        action("libraryWall");
        require(window.workspace() == sketch::desktop::Workspace::measurement,
                "wall authoring returns a projected workspace to conventional plan coordinates");
        click({150, 200});
        click({630, 200});
        std::string wall_id;
        const auto wall_snapshot = window.document().snapshot();
        for (const auto& [id, entity] : wall_snapshot.entities()) {
            if (entity.type != "wall") continue;
            wall_id = id;
            require(std::abs(entity.properties.at("thickness_m").get<double>() - 0.24) < 1e-10, "wall thickness comes from palette units");
            require(std::abs(entity.properties.at("height_m").get<double>() - 2.8) < 1e-10, "wall height comes from palette units");
        }
        require(!wall_id.empty(), "two actual canvas clicks create a semantic wall in 2D");
        bool footprint = false;
        for (const auto& entity : canvas->entities()) {
            if (entity.id.toStdString() != wall_id) continue;
            require(entity.segments.size() == 4, "2D wall retains four physical outline edges");
            const auto bounds = sketch::boundary_bounds(entity.segments);
            footprint = std::abs(bounds.maximum.y - bounds.minimum.y - 0.24) < 1e-10;
        }
        require(footprint, "retained wall footprint has the exact physical thickness");
        action("libraryDoor");
        mouse(QEvent::MouseMove, {290, 200}, Qt::NoButton, Qt::NoButton);
        require(canvas->boundaryDraftPreview() && !canvas->boundaryDraftPreview()->segments.empty(), "hosted door previews on wall before commit");
        click({290, 200});
        action("libraryWindow");
        click({510, 200});
        int opening_count = 0;
        const auto opening_snapshot = window.document().snapshot();
        for (const auto& [id, entity] : opening_snapshot.entities()) {
            if (entity.type != "opening") continue;
            ++opening_count;
            require(entity.properties.at("wall_id") == wall_id, "opening is hosted by the drawn wall");
            require(entity.properties.at("offset_m").get<double>() > 0, "opening stores click-derived host offset");
        }
        require(opening_count == 2, "door and window actions commit hosted openings");
        action("libraryDoorway");
        require(field("openingDrawSill")->text() == QStringLiteral("0 m"), "doorway defaults to floor level");
        click({400, 200});
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
        click({400, 200});
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
        click({510, 200});
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
        const auto capture_directory = qEnvironmentVariable("VERTEX_TEST_CAPTURE_DIR");
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
