#include "sketch/desktop/main_window.hpp"
#include "sketch/document.hpp"
#include "sketch/sheet_view_entity_codec.hpp"
#include "../src/desktop/plan_canvas.hpp"
#include "support/noninteractive_errors.hpp"

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QImage>
#include <QPainter>
#include <QFile>
#include <QFont>
#include <QFontDatabase>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QStandardPaths>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QUuid>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
void require(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "associative_section_desktop_tests: " << message << '\n';
        std::exit(1);
    }
}
bool close(double actual, double expected) { return std::abs(actual - expected) < 1e-6; }

sketch::CoordinatedView section(const sketch::desktop::MainWindow& window) {
    const auto model = sketch::decode_sheet_view_entity(
        window.document().snapshot().entities().at("sheet-view-1"));
    const auto found = std::find_if(model.views().begin(), model.views().end(),
        [](const auto& view) { return view.id == "view-section"; });
    require(found != model.views().end(), "persisted section exists");
    return *found;
}

template<typename View>
auto& overlay_by_id(View& view, const std::string& id) {
    const auto found = std::find_if(view.overlays.begin(), view.overlays.end(),
        [&](const auto& overlay) { return overlay.id == id; });
    require(found != view.overlays.end(), "section retains the requested stable overlay identity");
    return *found;
}

std::string overlay_id_for_axis(const sketch::CoordinatedView& view, sketch::SectionDimensionAxis axis) {
    const auto matches = [&](const auto& overlay) {
        return overlay.dimension_binding && overlay.dimension_binding->axis == axis;
    };
    require(std::count_if(view.overlays.begin(), view.overlays.end(), matches) == 1,
            "fixture has exactly one bound dimension for each measured axis");
    return std::find_if(view.overlays.begin(), view.overlays.end(), matches)->id;
}

int overlay_row(const QTableWidget& table, const std::string& id) {
    for (int row = 0; row < table.rowCount(); ++row)
        if (table.item(row, 0)->text() == QString::fromStdString(id)) return row;
    require(false, "annotation editor retains the requested stable overlay identity");
    return -1;
}

void edit_section(sketch::desktop::MainWindow& window,
                  const std::function<void(QDialog&, QTableWidget&)>& edit) {
    auto* action = window.findChild<QAction*>("manageNamedViews");
    require(action, "named sections action exists");
    QTimer::singleShot(0, &window, [&] {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        require(dialog, "named sections editor opens");
        auto* selection = dialog->findChild<QComboBox*>("namedViewSelection");
        require(selection && selection->findData(QStringLiteral("view-section")) >= 0,
                "editor exposes the stable section identity");
        selection->setCurrentIndex(selection->findData(QStringLiteral("view-section")));
        auto* table = dialog->findChild<QTableWidget*>("sectionOverlays");
        require(table && table->columnCount() == 11, "editor exposes dimension binding fields");
        edit(*dialog, *table);
        dialog->findChild<QPushButton*>("saveNamedView")->click();
    });
    action->trigger();
}

void select_section(sketch::desktop::MainWindow& window) {
    window.setWorkspace(sketch::desktop::Workspace::architectural);
    auto* selection = window.findChild<QComboBox*>("architecturalView");
    require(selection && selection->findData(QStringLiteral("view-section"), Qt::UserRole + 1) >= 0,
            "architectural selector exposes the persisted section");
    selection->setCurrentIndex(selection->findData(QStringLiteral("view-section"), Qt::UserRole + 1));
    QApplication::processEvents();
}

const sketch::desktop::CanvasEntity& dimension_line(
    const sketch::desktop::PlanCanvas& canvas, const std::string& id) {
    const auto key = QString::fromStdString("view-section/overlay/" + id);
    const auto found = std::find_if(canvas.entities().begin(), canvas.entities().end(),
        [&](const auto& entity) { return entity.id == key; });
    require(found != canvas.entities().end() && found->type == "section_overlay" &&
                found->dimension_end_ticks && found->segments.size() == 3,
            "bound dimension retains one dimension line and two source extension lines");
    return *found;
}
const sketch::desktop::CanvasLabel& dimension_label(
    const sketch::desktop::PlanCanvas& canvas, const std::string& id) {
    const auto key = QString::fromStdString("view-section/overlay/" + id);
    const auto found = std::find_if(canvas.labels().begin(), canvas.labels().end(),
        [&](const auto& label) { return label.id == key; });
    require(found != canvas.labels().end(), "bound dimension has a derived label");
    return *found;
}

void test_associative_section() {
    sketch::desktop::MainWindow window;
    window.setMetricUnits(true);
    // Author a real level binding through its modal editor. The wall's local
    // elevation and the section origin must both affect witness placement.
    auto* levels_action = window.findChild<QAction*>("verticalLevels");
    require(levels_action, "vertical level editor exists");
    QTimer::singleShot(0, &window, [&] {
        auto* dialog = window.findChild<QDialog*>("verticalLevelsDialog");
        require(dialog, "vertical level dialog opens");
        dialog->findChild<QLineEdit*>("verticalLevelId")->setText("upper");
        dialog->findChild<QLineEdit*>("verticalLevelElevation")->setText("3.25");
        dialog->findChild<QPushButton*>("saveVerticalLevel")->click();
        auto* floor = dialog->findChild<QComboBox*>("verticalFloorBindingFloor");
        auto* level = dialog->findChild<QComboBox*>("verticalFloorBindingLevel");
        require(floor && level && level->findData(QStringLiteral("upper")) >= 0,
                "new level can be assigned to the current floor");
        floor->setCurrentIndex(floor->findData(QStringLiteral("floor-1")));
        level->setCurrentIndex(level->findData(QStringLiteral("upper")));
        dialog->findChild<QPushButton*>("saveVerticalFloorBinding")->click();
        dialog->reject();
    });
    levels_action->trigger();
    const auto wall = window.createStraightWall({5, 0}, {9, 0});
    require(!wall.isEmpty() && window.selectEntity(wall), "real source wall is created");
    require(window.editSelectedHeight("3 m") && window.editSelectedElevation("0.4 m"),
            "source wall dimensions and level-relative elevation are authored");
    require(window.document().snapshot().entities().at(wall.toStdString())
                .properties.at("vertical_placement").at("mode") == "level",
            "wall uses the floor's authoritative level placement");
    const auto before_annotations = window.document().revision();
    edit_section(window, [&](QDialog& dialog, QTableWidget& table) {
        dialog.findChild<QLineEdit*>("namedViewOrigin")->setText("1, -2, 1");
        dialog.findChild<QLineEdit*>("namedViewDirection")->setText("0, 1, 0");
        dialog.findChild<QLineEdit*>("namedViewUp")->setText("0, 0, 1");
        dialog.findChild<QLineEdit*>("namedViewCut")->setText("2");
        for (int row = 0; row < 2; ++row) {
            dialog.findChild<QPushButton*>("addSectionOverlay")->click();
            table.item(row, 1)->setText("dimension");
            // These explicit endpoints and authored text must never become
            // the source of a linked measurement or its displayed label.
            table.item(row, 2)->setText("100");
            table.item(row, 3)->setText("200");
            table.item(row, 4)->setText("1099");
            table.item(row, 5)->setText("200");
            table.item(row, 6)->setText("stale authored label");
            auto* source = qobject_cast<QComboBox*>(table.cellWidget(row, 8));
            auto* axis = qobject_cast<QComboBox*>(table.cellWidget(row, 9));
            require(source && axis && source->findData(wall) >= 0,
                    "dimension source selector stores the real wall's stable identity");
            source->setCurrentIndex(source->findData(wall));
            axis->setCurrentIndex(axis->findData(static_cast<int>(row == 0
                ? sketch::SectionDimensionAxis::horizontal : sketch::SectionDimensionAxis::vertical)));
            table.item(row, 10)->setText(row == 0 ? "0.75" : "1");
        }
        // Even a syntactically valid selector value cannot bypass source
        // geometry admission. The modal editor must preserve the document.
        auto* source = qobject_cast<QComboBox*>(table.cellWidget(0, 8));
        source->addItem("Unsupported floor", QStringLiteral("floor-1"));
        source->setCurrentIndex(source->findData(QStringLiteral("floor-1")));
        const auto unchanged = window.document().snapshot();
        dialog.findChild<QPushButton*>("saveNamedView")->click();
        require(dialog.isVisible() && window.document().revision() == unchanged.revision() &&
                    window.document().snapshot().entities() == unchanged.entities() &&
                    !dialog.findChild<QLabel*>("namedViewError")->text().isEmpty(),
                "unsupported source is rejected visibly without a partial annotation command");
        source->setCurrentIndex(source->findData(wall));
    });
    require(window.document().revision() == before_annotations + 1,
            "two bindings and the section frame commit as one document command");
    const auto authored = section(window);
    require(authored.overlays.size() == 2, "section persists exactly two annotation records");
    const auto width_id = overlay_id_for_axis(authored, sketch::SectionDimensionAxis::horizontal);
    const auto height_id = overlay_id_for_axis(authored, sketch::SectionDimensionAxis::vertical);
    for (const auto& overlay : authored.overlays)
        require(overlay.dimension_binding && overlay.dimension_binding->object_id == wall.toStdString(),
                "both persisted dimensions retain the source wall identity");
    select_section(window);
    auto* canvas = dynamic_cast<sketch::desktop::PlanCanvas*>(
        window.findChild<QWidget*>("architecturalPlanCanvas"));
    require(canvas, "architectural canvas exists");
    const auto check = [&](double width, double height, double width_offset) {
        const auto& horizontal = dimension_line(*canvas, width_id).segments.back();
        const auto& vertical = dimension_line(*canvas, height_id).segments.back();
        // Floor 3.25 + local base 0.4 - section origin 1 = 2.65 m.
        const auto top = 2.65 + height;
        require(close(horizontal.start.x, 4) && close(horizontal.end.x, 4 + width) &&
                    close(horizontal.start.y, top + width_offset) &&
                    close(horizontal.end.y, top + width_offset),
                "width line follows source extent and translated section/level placement");
        require(close(vertical.start.x, 5 + width) && close(vertical.end.x, 5 + width) &&
                    close(vertical.start.y, 2.65) && close(vertical.end.y, top),
                "height line follows resolved level elevation and authoritative wall height");
        require(dimension_label(*canvas, width_id).text == QString::number(width, 'f', 3) + " m" &&
                    dimension_label(*canvas, height_id).text == QString::number(height, 'f', 3) + " m",
                "labels measure authoritative source extents rather than cached endpoints or text");
        require(std::count_if(canvas->entities().begin(), canvas->entities().end(),
                    [](const auto& entity) { return entity.type == "section_overlay"; }) == 2,
                "derived geometry contains one overlay per persisted dimension");
    };
    check(4, 3, 0.75);
    const auto annotated_entities = window.document().snapshot().entities();
    window.setMetricUnits(false);
    require(dimension_label(*canvas, width_id).text.contains('\'') &&
                dimension_label(*canvas, height_id).text.contains('\'') &&
                window.document().snapshot().entities() == annotated_entities,
            "display units update derived dimension labels without modifying source entities");
    window.setMetricUnits(true);
    check(4, 3, 0.75);
    edit_section(window, [&](QDialog&, QTableWidget& table) {
        table.item(overlay_row(table, width_id), 10)->setText("1.25");
    });
    check(4, 3, 1.25);
    require(window.undoCommand(), "dimension placement edit is undoable"); check(4, 3, 0.75);
    require(window.redoCommand(), "dimension placement edit is redoable"); check(4, 3, 1.25);
    require(window.selectEntity(wall) && window.editSelectedLength("5.5 m"),
            "normal wall length command edits the same source");
    check(5.5, 3, 1.25);
    require(window.undoCommand(), "source length edit is undoable"); check(4, 3, 1.25);
    require(window.redoCommand(), "source length edit is redoable"); check(5.5, 3, 1.25);
    const auto before_height = window.document().snapshot();
    require(window.editSelectedHeight("4.25 m"), "normal wall height command edits the source");
    check(5.5, 4.25, 1.25);
    require(window.document().snapshot().entities().size() == before_height.entities().size(),
            "derived dimension updates do not insert persisted entities");
    require(window.undoCommand(), "source height edit is undoable"); check(5.5, 3, 1.25);
    require(window.redoCommand(), "source height edit is redoable"); check(5.5, 4.25, 1.25);
    const auto current = window.document().snapshot();
    require(std::count_if(current.entities().begin(), current.entities().end(),
                [](const auto& entry) { return entry.second.type == "wall"; }) == 1,
            "source edits retain exactly one semantic wall");
    const auto final_section = section(window);
    const auto final_overlays = final_section.overlays;
    require(final_overlays.size() == 2, "source edits retain exactly two annotation records");
    for (const auto& id : {width_id, height_id}) {
        const auto& original = overlay_by_id(authored, id);
        const auto& updated = overlay_by_id(final_section, id);
        require(updated.start_m == original.start_m && updated.end_m == original.end_m &&
                    updated.dimension_binding && original.dimension_binding &&
                    updated.dimension_binding->axis == original.dimension_binding->axis &&
                    updated.dimension_binding->object_id == original.dimension_binding->object_id,
                "source changes preserve overlay identities and do not rewrite detached coordinate fields");
    }
    QTemporaryDir directory;
    require(directory.isValid(), "output fixture has a temporary directory");
    const auto svg = directory.filePath("associative-section.svg");
    require(window.exportDraftSvg(svg) && window.exportDraftPdf(directory.filePath("associative-section.pdf")),
            "printable output accepts resolved section dimensions");
    QFile output(svg);
    require(output.open(QIODevice::ReadOnly), "SVG output can be read");
    const auto bytes = output.readAll();
    require(bytes.contains("5.500 m") && bytes.contains("4.250 m") &&
                !bytes.contains("stale authored label") && !bytes.contains("999.000 m"),
            "shared printable output uses current source dimensions");
    const auto capture = qEnvironmentVariable("VERTEX_TEST_CAPTURE_DIR");
    if (!capture.isEmpty()) {
        require(QDir().mkpath(capture), "section capture directory is available");
        canvas->resize(1000, 650);
        canvas->setGridEnabled(true);
        canvas->fitView();
        QImage image(canvas->size(), QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::white);
        QPainter painter(&image);
        canvas->renderScene(painter, QRectF(canvas->rect()));
        painter.end();
        require(image.save(QDir(capture).filePath("associative-section-dimensions.png")),
                "linked section capture saves the actual canvas scene");
    }
    const auto path = directory.filePath("associative-section.bldproj");
    require(window.saveProjectAs(path) && window.openProject(path), "linked section saves and reopens");
    select_section(window);
    check(5.5, 4.25, 1.25);
    require(window.document().snapshot().entities() == current.entities() &&
                section(window).overlays == final_overlays,
            "reopening preserves exact semantic entities, stable bindings, and dimension placement");

    auto snapshot = window.document().snapshot();
    auto model = sketch::decode_sheet_view_entity(snapshot.entities().at("sheet-view-1"));
    auto invalid = section(window);
    overlay_by_id(invalid, width_id).dimension_binding->object_id = "missing-source";
    overlay_by_id(invalid, width_id).object_id = "missing-source";
    bool rejected = false;
    try {
        auto replacement = sketch::make_sheet_view_entity("sheet-view-1", model.with_view(invalid));
        window.document().apply(sketch::ApplyEntityChanges{snapshot.revision(),
            {sketch::EntityChange::upsert(std::move(replacement))}, {}, "reject corrupt dimension binding"});
    } catch (const sketch::DocumentError&) { rejected = true; }
    require(rejected && window.document().revision() == snapshot.revision() &&
                window.document().snapshot().entities() == snapshot.entities(),
            "corrupt source binding cannot enter document history or leave stale dimension data");
    require(window.selectEntity(wall) && window.deleteSelection(),
            "deleting a source wall removes its dependent section dimensions atomically");
    const auto deleted = window.document().snapshot();
    require(!deleted.entities().contains(wall.toStdString()) && section(window).overlays.empty() &&
                std::none_of(canvas->entities().begin(), canvas->entities().end(),
                    [](const auto& entity) { return entity.type == "section_overlay"; }) &&
                std::none_of(canvas->labels().begin(), canvas->labels().end(),
                    [&](const auto& label) {
                        return label.id == QString::fromStdString("view-section/overlay/" + width_id) ||
                            label.id == QString::fromStdString("view-section/overlay/" + height_id);
                    }),
            "source deletion leaves no persisted dimensions, derived lines, or stale labels");
    require(window.undoCommand() && window.document().snapshot().entities() == snapshot.entities(),
            "one undo restores the source wall and both dimension bindings");
    check(5.5, 4.25, 1.25);
    require(window.redoCommand() && window.document().snapshot().entities() == deleted.entities(),
            "one redo deletes the same semantic source and annotations again");
    require(window.undoCommand(), "final source deletion undo restores the linked section");
    check(5.5, 4.25, 1.25);

    // Deleting the last member of a restricted view must not broaden it to
    // the surviving document objects. Keep a second actual wall available.
    const auto other_wall = window.createStraightWall({15, 0}, {17, 0});
    require(!other_wall.isEmpty(), "restricted view fixture has a surviving second wall");
    require(window.editArchitecturalViewPresentation("view-section", "2", "100", "0.5", "0.18",
                true, "solid", "1", "medium", wall),
            "section is restricted to the first wall through the public presentation command");
    const auto has_source = [&](const QString& id) {
        return std::any_of(canvas->entities().begin(), canvas->entities().end(),
            [&](const auto& entity) { return entity.id == id; });
    };
    require(has_source(wall) && !has_source(other_wall) &&
                section(window).object_ids == std::vector<std::string>{wall.toStdString()},
            "restricted section displays only the named source wall");
    const auto restricted = window.document().snapshot();
    require(window.selectEntity(wall) && window.deleteSelection(),
            "last restricted source can be deleted atomically");
    const auto empty_section = section(window);
    require(empty_section.restrict_to_objects && empty_section.object_ids.empty() &&
                !has_source(wall) && !has_source(other_wall) &&
                window.document().snapshot().entities().contains(other_wall.toStdString()),
            "restricted empty section keeps the surviving second wall hidden");
    const auto restricted_empty = window.document().snapshot();
    require(window.editArchitecturalViewPresentation("view-section", "2", "100", "0.6", "0.18",
                true, "solid", "1", "fine", ""),
            "presentation-only edit accepts the restricted section's empty source field");
    const auto edited_empty = section(window);
    require(edited_empty.restrict_to_objects && edited_empty.object_ids.empty() && !has_source(other_wall) &&
                close(edited_empty.presentation.cut_line_mm, 0.6) &&
                edited_empty.presentation.detail == sketch::ViewDetail::fine,
            "changing line weight and detail preserves an explicitly empty source restriction");
    require(window.undoCommand() &&
                window.document().snapshot().entities() == restricted_empty.entities() && !has_source(other_wall),
            "undoing presentation retains the empty source restriction");
    require(window.undoCommand() && window.document().snapshot().entities() == restricted.entities() &&
                has_source(wall) && !has_source(other_wall),
            "undo restores the original source filter without admitting the second wall");
    check(5.5, 4.25, 1.25);

    // Detach through the actual source selector, then delete its former
    // owner. The detached annotation must no longer carry a cleanup reference.
    edit_section(window, [&](QDialog&, QTableWidget& table) {
        const auto row = overlay_row(table, width_id);
        auto* source = qobject_cast<QComboBox*>(table.cellWidget(row, 8));
        require(source && source->currentData().toString() == wall,
                "width dimension initially references the original source");
        source->setCurrentIndex(source->findData(QString{}));
        require(source->currentData().toString().isEmpty(), "Detached removes the source selector identity");
        table.item(row, 2)->setText("1"); table.item(row, 3)->setText("2");
        table.item(row, 4)->setText("3"); table.item(row, 5)->setText("2");
    });
    const auto detached_section = section(window);
    require(detached_section.overlays.size() == 2 &&
                !overlay_by_id(detached_section, width_id).dimension_binding &&
                overlay_by_id(detached_section, width_id).object_id.empty() &&
                dimension_label(*canvas, width_id).text == "2.000 m",
            "detaching persists explicit geometry and clears the former source reference");
    const auto detached = window.document().snapshot();
    require(window.selectEntity(wall) && window.deleteSelection(),
            "former source can be deleted after detaching one dimension");
    const auto surviving_section = section(window);
    require(surviving_section.restrict_to_objects && surviving_section.object_ids.empty() &&
                surviving_section.overlays.size() == 1 &&
                overlay_by_id(surviving_section, width_id) == overlay_by_id(detached_section, width_id) &&
                !has_source(other_wall) && dimension_label(*canvas, width_id).text == "2.000 m",
            "detached annotation survives former-source deletion while its linked sibling is removed");
    const auto detached_key = QString::fromStdString("view-section/overlay/" + width_id);
    const auto detached_line = std::find_if(canvas->entities().begin(), canvas->entities().end(),
        [&](const auto& entity) { return entity.id == detached_key; });
    require(detached_line != canvas->entities().end() && detached_line->segments.size() == 1 &&
                close(detached_line->segments.front().start.x, 1) &&
                close(detached_line->segments.front().start.y, 2) &&
                close(detached_line->segments.front().end.x, 3) &&
                close(detached_line->segments.front().end.y, 2),
            "surviving detached line retains its authored endpoints without source witnesses");
    require(window.undoCommand() && window.document().snapshot().entities() == detached.entities(),
            "undo restores the source and linked sibling while retaining the detached annotation");
}
} // namespace

int main(int argc, char** argv) {
    sketch::testing::noninteractive_errors();
    QStandardPaths::setTestModeEnabled(true);
    QApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("Vertex-associative-section-test-") +
        QUuid::createUuid().toString(QUuid::WithoutBraces));
    const auto font_id = QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/Inter.ttf"));
    require(font_id >= 0, "test loads bundled Inter font");
    const auto families = QFontDatabase::applicationFontFamilies(font_id);
    require(!families.isEmpty(), "bundled font has a family");
    application.setFont(QFont(families.front(), 10));
    test_associative_section();
    std::cout << "Associative section desktop workflow passed\n";
}
