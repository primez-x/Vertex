#include "sketch/desktop/main_window.hpp"
#include "sketch/document.hpp"
#include "sketch/vertical_levels.hpp"
#include "sketch/wall_measurement.hpp"
#include "sketch/boundary_entity.hpp"
#include "../src/desktop/plan_canvas.hpp"
#include "support/noninteractive_errors.hpp"

#include <QApplication>
#include <QAction>
#include <QDialog>
#include <QDialogButtonBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTimer>
#include <QCoreApplication>
#include <QDir>
#include <QFont>
#include <QFontDatabase>
#include <QStandardPaths>

#include <cmath>
#include <exception>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

void capture(QWidget& widget, const QString& name) {
    const auto directory = qEnvironmentVariable("VERTEX_TEST_CAPTURE_DIR");
    if (directory.isEmpty()) return;
    QDir().mkpath(directory);
    widget.grab().save(QDir(directory).filePath(name + QStringLiteral(".png")));
}

double baseline_x(const sketch::Entity& entity, std::string_view endpoint,
                  std::string_view coordinate) {
    return entity.properties.at("baseline").at(std::string(endpoint))
        .at(coordinate == "x" ? 0 : 1).get<double>();
}

void insertion_dialog(sketch::desktop::MainWindow& window,
                      const std::function<void(QDialog&)>& inspect) {
    auto* action = window.findChild<QAction*>(QStringLiteral("insertBoundaryVertex"));
    require(action && action->isEnabled(), "Tools exposes wall vertex insertion");
    std::exception_ptr failure;
    QTimer poll;
    poll.setInterval(10);
    QObject::connect(&poll,&QTimer::timeout,&window,[&] {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if(!dialog)return;
        poll.stop();
        try {
            if(dialog->objectName()!=QStringLiteral("wallVertexInsertionDialog"))
                throw std::runtime_error("Tools opens wall insertion dialog; got "+dialog->objectName().toStdString());
            inspect(*dialog);
        } catch (...) {
            failure = std::current_exception();
            if (dialog) dialog->reject();
        }
    });
    poll.start();
    action->trigger();
    if (failure) std::rethrow_exception(failure);
}

void preview_and_apply(sketch::desktop::MainWindow& window) {
    const auto before = window.document().snapshot();
    insertion_dialog(window, [&](QDialog& dialog) {
        auto* fraction = dialog.findChild<QLineEdit*>("wallVertexFraction");
        auto* buttons = dialog.findChild<QDialogButtonBox*>("wallVertexInsertionButtons");
        auto* preview = dynamic_cast<sketch::desktop::PlanCanvas*>(
            dialog.findChild<QWidget*>("wallVertexInsertionPreview"));
        auto* summary = dialog.findChild<QLabel*>("wallVertexInsertionSummary");
        auto* freedom = dialog.findChild<QLabel*>("wallVertexInsertionFreedom");
        require(fraction && buttons && preview && summary, "wall insertion controls exist");
        for (const auto& invalid : {QStringLiteral("invalid"), QStringLiteral("0"), QStringLiteral("1")}) {
            fraction->setText(invalid);
            require(!buttons->button(QDialogButtonBox::Apply)->isEnabled(),
                    "invalid and endpoint fractions disable Apply");
        }
        fraction->setText(QStringLiteral("0.4"));
        require(buttons->button(QDialogButtonBox::Apply)->isEnabled() &&
                    !summary->text().isEmpty() && preview->entities().size() >= 4,
                "valid fraction previews original wall, both pieces and split marker");
        bool marker = false;
        for (const auto& entity : preview->entities())
            marker = marker || entity.id == QStringLiteral("split-point");
        require(marker, "actual preview contains the proposed split point");
        require(freedom && freedom->text().contains(QStringLiteral("+2")),
                "preview reports two new unconstrained seam coordinates without locking both pieces");
        require(window.document().snapshot().entities() == before.entities() &&
                    window.document().snapshot().revision() == before.revision(),
                "preview never mutates the document");
        capture(dialog, QStringLiteral("wall-vertex-insertion-preview"));
        capture(*preview, QStringLiteral("wall-vertex-insertion-preview-canvas"));
        buttons->button(QDialogButtonBox::Apply)->click();
    });
}

void splitting_a_selected_wall_preserves_geometry_metadata_and_history() {
    using namespace sketch;
    using sketch::desktop::MainWindow;

    MainWindow window;
    require(window.createNewProject(), "a fresh native project is created");
    window.setMetricUnits(true);

    auto scaffold = window.document().snapshot();
    auto floor = scaffold.entities().at("floor-1");
    const VerticalLevelGraph graph({{"ground", 0.0}, {"upper", 3.0}},
                                   {{"ground-upper", "ground", "upper"}});
    Entity levels{"wall-vertex-insertion-levels", "vertical_levels",
                  {{"model", nlohmann::json::parse(graph.serialize())}},
                  false, nlohmann::json::object()};
    floor.properties["vertical_level_binding"] =
        VerticalLevelBinding{levels.id, "upper"}.to_json();
    window.document().apply(ApplyEntityChanges{
        .expected_revision = scaffold.revision(),
        .entity_changes = {EntityChange::upsert(levels), EntityChange::upsert(floor)},
        .message = "prepare level-bound wall insertion fixture",
    });

    const auto wall_id = window.createStraightWall(
        {0.0, 0.0}, {10.0, 0.0}, QStringLiteral("load-bearing partition"));
    require(!wall_id.isEmpty(), "the straight source wall is created");
    auto metadata_snapshot = window.document().snapshot();
    auto metadata_wall = metadata_snapshot.entities().at(wall_id.toStdString());
    metadata_wall.extensions["vendor_wall_metadata"] =
        {{"retain", 17}, {"opaque", "preserve across the split"}};
    window.document().apply(ApplyEntityChanges{
        .expected_revision = metadata_snapshot.revision(),
        .entity_changes = {EntityChange::upsert(metadata_wall)},
        .message = "add opaque wall metadata for insertion fixture",
    });
    require(window.selectEntity(wall_id), "the source wall is selected for vertex insertion");

    const auto source = window.document().snapshot();
    const auto original = source.entities().at(wall_id.toStdString());
    require(original.properties.contains("vertical_placement") &&
                original.properties.at("vertical_placement").at("mode") == "level",
            "the source wall has a level-relative placement to preserve");
    require(original.properties.at("classification") == "load-bearing partition" &&
                original.extensions.contains("vendor_wall_metadata"),
            "the source wall has authored classification and opaque metadata");

    window.resize(1200, 800);
    window.show();
    QApplication::processEvents();
    require(window.selectEntity(wall_id),"select the shown source wall before opening its tool");
    insertion_dialog(window, [](QDialog& dialog) { dialog.reject(); });
    require(window.document().snapshot().revision() == source.revision() &&
                window.document().snapshot().entities() == source.entities(),
            "Cancel leaves source entities and revision unchanged");
    preview_and_apply(window);
    const auto split = window.document().snapshot();
    require(split.revision() == source.revision() + 1,
            "wall splitting is one atomic document command");

    const Entity* first = nullptr;
    const Entity* second = nullptr;
    std::size_t wall_count = 0;
    for (const auto& [id, entity] : split.entities()) {
        (void)id;
        if (entity.type != "wall") continue;
        ++wall_count;
        if (entity.id == original.id) first = &entity;
        else second = &entity;
    }
    require(wall_count == 2 && first && second && second->id != original.id,
            "the original wall remains the first piece and the second piece gets a fresh ID");
    require(std::abs(baseline_x(*first, "end", "x") - 4.0) < 1e-10 &&
                std::abs(baseline_x(*second, "start", "x") - 4.0) < 1e-10 &&
                std::abs(baseline_x(*second, "end", "x") - 10.0) < 1e-10,
            "the two straight baselines meet at x=4 and retain the original x=10 endpoint");

    for (const auto* piece : {first, second}) {
        require(piece->properties.at("thickness_m") == original.properties.at("thickness_m") &&
                    piece->properties.at("classification") == original.properties.at("classification") &&
                    piece->properties.at("floor_id") == original.properties.at("floor_id") &&
                    piece->properties.at("layer_id") == original.properties.at("layer_id") &&
                    piece->properties.at("vertical_placement") == original.properties.at("vertical_placement"),
                "each split piece retains physical thickness, classification, floor, layer and level placement");
        require(piece->extensions.at("vendor_wall_metadata") == original.extensions.at("vendor_wall_metadata"),
                "each split piece retains opaque extension metadata");
    }
    require(source.entities().at(original.id) == original &&
                std::abs(baseline_x(source.entities().at(original.id), "end", "x") - 10.0) < 1e-10,
            "the pre-split source snapshot remains immutable after the document changes");

    window.resize(1200, 800);
    window.show();
    QApplication::processEvents();
    capture(window, QStringLiteral("wall-vertex-insertion-split"));

    require(window.undoCommand() && window.document().snapshot().entities() == source.entities(),
            "Undo restores the exact source wall, metadata, levels and project state");
    require(window.redoCommand() && window.document().snapshot().entities() == split.entities(),
            "Redo restores the exact two-piece wall split");

    require(window.selectEntity(wall_id), "select a split piece for stale preview check");
    std::exception_ptr stale_failure;
    auto changed = window.document().snapshot();
    insertion_dialog(window, [&](QDialog& dialog) {
        auto* buttons = dialog.findChild<QDialogButtonBox*>("wallVertexInsertionButtons");
        require(buttons && buttons->button(QDialogButtonBox::Apply)->isEnabled(),
                "stale check starts with a valid preview");
        auto entity = changed.entities().at(wall_id.toStdString());
        entity.extensions["stale_preview_test"] = true;
        window.document().apply(ApplyEntityChanges{
            .expected_revision = changed.revision(),
            .entity_changes = {EntityChange::upsert(entity)},
            .message = "change source while insertion dialog is open",
        });
        changed = window.document().snapshot();
        QTimer::singleShot(250, &dialog, [&, buttons] {
            try {
                require(!buttons->button(QDialogButtonBox::Apply)->isEnabled(),
                        "document revision change disables stale Apply");
            } catch (...) { stale_failure = std::current_exception(); }
            dialog.reject();
        });
    });
    if (stale_failure) std::rethrow_exception(stale_failure);
    require(window.document().snapshot().entities() == changed.entities() &&
                window.document().snapshot().revision() == changed.revision(),
            "stale preview does not apply an additional command");
    const auto other=window.createStraightWall({20,0},{24,0},QStringLiteral("partition"));
    require(!other.isEmpty() && window.selectEntity(wall_id) && window.selectEntity(other,true),
            "multi-selection fixture selects two walls");
    const auto multi=window.document().snapshot();
    insertion_dialog(window,[&](QDialog& dialog){
        auto* buttons=dialog.findChild<QDialogButtonBox*>("wallVertexInsertionButtons");
        auto* status=dialog.findChild<QLabel*>("wallVertexInsertionStatus");
        require(buttons && !buttons->button(QDialogButtonBox::Apply)->isEnabled() &&
                status && status->text().contains(QStringLiteral("one wall")),
                "real dialog refuses splitting only the primary object of a multiple selection");
        dialog.reject();
    });
    require(window.document().snapshot().entities()==multi.entities() &&
            window.document().snapshot().revision()==multi.revision(),"multiple selection refuses atomically");
}

void splitting_a_measured_source_wall_preserves_current_receipt_and_persistence() {
    using namespace sketch;
    sketch::desktop::MainWindow window;
    require(window.createNewProject(), "create measured source fixture");
    window.setMetricUnits(true);
    const QStringList walls{
        window.createStraightWall({0, 0}, {10, 0}, QStringLiteral("exterior")),
        window.createStraightWall({10, 0}, {10, 5}, QStringLiteral("exterior")),
        window.createStraightWall({10, 5}, {0, 5}, QStringLiteral("exterior")),
        window.createStraightWall({0, 5}, {0, 0}, QStringLiteral("exterior"))};
    for (qsizetype i = 0; i < walls.size(); ++i)
        require(!walls[i].isEmpty() && window.selectEntity(walls[i], i != 0),
                "select the closed exterior source wall loop");
    const auto owner_id = window.createMeasurementBoundaryFromSelectedWalls();
    require(!owner_id.isEmpty(), "derive a measured area through the public authoring API");
    const auto before = window.document().snapshot();
    const auto& owner = before.entities().at(owner_id.toStdString());
    require(wall_measurement_source_current(before, owner), "fixture receipt is source-current");
    const auto original_boundary = decode_identified_boundary_entity(owner);
    require(window.selectEntity(walls.front()), "select one measured source wall");
    window.resize(1200, 800);
    window.show();
    QApplication::processEvents();
    require(window.selectEntity(walls.front()),"select the shown measured source wall before opening its tool");
    preview_and_apply(window);
    const auto after = window.document().snapshot();
    const auto& split_owner = after.entities().at(owner_id.toStdString());
    require(after.revision() == before.revision() + 1 &&
                wall_measurement_source_current(after, split_owner) &&
                split_owner.properties.at("wall_measurement_source").at("walls").size() == 5 &&
                decode_identified_boundary_entity(split_owner).segments.size() == original_boundary.segments.size() + 1,
            "one atomic split updates measured outline and its current five-wall source receipt");
    require(window.undoCommand() && window.document().snapshot().entities() == before.entities(),
            "Undo restores source walls and measured owner together");
    require(window.redoCommand() && window.document().snapshot().entities() == after.entities(),
            "Redo restores source walls and measured owner together");
    QTemporaryDir directory;
    require(directory.isValid(), "temporary project directory exists");
    const auto path = directory.filePath(QStringLiteral("wall-insertion.bldproj"));
    require(window.saveProjectAs(path) && window.openProject(path), "split project saves and reopens");
    const auto reopened = window.document().snapshot();
    require(reopened.entities() == after.entities() &&
                wall_measurement_source_current(reopened, reopened.entities().at(owner_id.toStdString())),
            "save/reopen retains exact split entities and current measured provenance");
    require(window.undoCommand() && window.document().snapshot().entities() == before.entities() &&
                window.redoCommand() && window.document().snapshot().entities() == after.entities(),
            "saved atomic split history remains undoable and redoable after reopen");
}

} // namespace

int main(int argc, char** argv) {
    sketch::testing::noninteractive_errors();
    QStandardPaths::setTestModeEnabled(true);
    QApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("Vertex-wall-vertex-insertion-test"));
    try {
        const auto font_id = QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/Inter.ttf"));
        require(font_id >= 0, "bundled Inter font is available for the desktop test");
        application.setFont(QFont(QFontDatabase::applicationFontFamilies(font_id).front(), 10));
        splitting_a_selected_wall_preserves_geometry_metadata_and_history();
        splitting_a_measured_source_wall_preserves_current_receipt_and_persistence();
        std::cout << "wall_vertex_insertion_desktop_tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "wall_vertex_insertion_desktop_tests: " << error.what() << '\n';
        return 1;
    }
}
