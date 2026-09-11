#include "sketch/desktop/main_window.hpp"

#include "sketch/document.hpp"
#include "sketch/building_entity.hpp"
#include "sketch/annotation_entity_codec.hpp"
#include "sketch/desktop/building_object_dialog.hpp"
#include "support/noninteractive_errors.hpp"
#include "../src/desktop/plan_canvas.hpp"
#include "../src/desktop/draft_image_stamp.hpp"

#include <QApplication>
#include <QComboBox>
#include <QFile>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPageSize>
#include <QPdfWriter>
#include <QPushButton>
#include <QToolButton>
#include <QTemporaryDir>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <string_view>

namespace {

[[noreturn]] void fail(std::string_view message) {
    std::cerr << "desktop_smoke: " << message << '\n';
    std::exit(1);
}

void require(bool condition, std::string_view message) {
    if (!condition) {
        fail(message);
    }
}

QTreeWidgetItem* navigator_item(sketch::desktop::MainWindow& window, const QString& id) {
    auto* tree = window.findChild<QTreeWidget*>(QStringLiteral("projectNavigator"));
    require(tree != nullptr, "project navigator must exist");
    QTreeWidgetItem* result = nullptr;
    for (QTreeWidgetItemIterator item(tree); *item; ++item) {
        if ((*item)->data(0, Qt::UserRole).toString() == id) {
            require(result == nullptr, "navigator must list each identity exactly once");
            result = *item;
        }
    }
    return result;
}

void test_organization_context() {
    sketch::desktop::MainWindow window;
    const auto second_building = window.createBuilding("property-1", "Workshop");
    require(!second_building.isEmpty(), "create a second building");
    const auto second_floor = window.createFloor(second_building, "Upper floor");
    require(!second_floor.isEmpty(), "create a floor with a default layer atomically");
    const auto layer = window.activeLayerId();
    require(!layer.isEmpty() && layer != "layer-1", "new floor activates its own drawing layer");
    auto* drawing_context = window.findChild<QLabel*>(QStringLiteral("drawingContext"));
    require(drawing_context && drawing_context->wordWrap() &&
                drawing_context->text().contains("Workshop") && drawing_context->text().contains("Upper floor"),
            "the full active drawing context must remain readable beside a narrow layer selector");
    const auto wall = window.createStraightWall({1.0, 2.0}, {6.0, 2.0});
    require(!wall.isEmpty(), "draw on the selected floor");
    const auto snapshot = window.document().snapshot();
    require(snapshot.entities().at(wall.toStdString()).properties.at("floor_id") == second_floor.toStdString() &&
                snapshot.entities().at(wall.toStdString()).properties.at("layer_id") == layer.toStdString(),
            "new walls must use the active drawing context");
    auto* item = navigator_item(window, wall);
    require(item && item->parent() && item->parent()->data(0, Qt::UserRole).toString() == layer,
            "navigator shows the wall under its actual layer");
    require(window.renameOrganizationEntity(second_floor, "Studio"), "rename the actual floor");
    require(navigator_item(window, second_floor)->text(0) == "Studio", "navigator reflects semantic names");
    require(window.document().snapshot().entities().at(wall.toStdString()) == snapshot.entities().at(wall.toStdString()),
            "organization rename must not transform or rewrite object geometry");
    require(window.undoCommand() && window.redoCommand(), "organization rename participates in history");
    require(!window.setActiveLayer("missing-layer"), "missing layer cannot become active");
    require(window.setActiveLayer("layer-1"), "switch to original building's layer");
    const auto original_floor_wall = window.createStraightWall({0.0, 0.0}, {4.0, 0.0});
    require(window.document().snapshot().entities().at(original_floor_wall.toStdString()).properties.at("floor_id") == "floor-1",
            "context selection changes subsequent authoring only");
    const auto boundary = window.createBoundary({{{0.0, 0.0}, {4.0, 0.0}, 0.0},
        {{4.0, 0.0}, {4.0, 3.0}, 0.0}, {{4.0, 3.0}, {0.0, 3.0}, 0.0}, {{0.0, 3.0}, {0.0, 0.0}, 0.0}});
    require(!boundary.isEmpty() && window.setActiveLayer(layer), "retain selected source while changing drawing context");
    const auto mismatch_revision = window.document().revision();
    require(window.createSlabFromSelectedBoundary("150 mm", "0 m").isEmpty() &&
                window.document().revision() == mismatch_revision,
            "cross-context slab from selected boundary must fail without mutation");
    const auto preview_revision = window.document().revision();
    require(window.renameOrganizationEntity(second_building, "Workshop annex"), "intervening organization edit");
    const auto intervening_revision = window.document().revision();
    require(window.createStraightWall({0.0, 0.0}, {2.0, 0.0}, "interior", preview_revision).isEmpty() &&
                !window.renameOrganizationEntity(second_floor, "Stale rename", preview_revision) &&
                window.document().revision() == intervening_revision,
            "authoring and rename reject a preview revision invalidated by another command");
    const auto disposable_layer = window.createLayer(second_floor, "Temporary");
    require(!disposable_layer.isEmpty(), "create an independent layer");
    window.document().apply(sketch::ApplyEntityChanges{
        .expected_revision = window.document().revision(),
        .entity_changes = {sketch::EntityChange::erase(disposable_layer.toStdString())},
        .message = "remove empty active layer",
    });
    const auto revision = window.document().revision();
    require(window.createStraightWall({0.0, 0.0}, {1.0, 0.0}).isEmpty() &&
                window.document().revision() == revision,
            "deleted active context blocks drawing instead of choosing another floor");
    require(window.undoCommand() && window.setActiveLayer(disposable_layer), "restored layer can be explicitly reactivated");
    QTemporaryDir directory;
    require(directory.isValid(), "organization fixture needs a temporary directory");
    const auto path = directory.filePath("organization.bldproj");
    require(window.saveProjectAs(path) && window.openProject(path), "multiple buildings and floors save and reopen");
    require(window.activeLayerId().isEmpty(), "multi-layer reopen requires an explicit drawing context");
    require(navigator_item(window, wall) && navigator_item(window, second_floor)->text(0) == "Studio",
            "reopened hierarchy preserves objects and names");
    require(window.selectEntity(wall) && window.activeLayerId() == layer,
            "selecting a reopened object resolves its real drawing context");
}

void test_six_form_authoring_and_quantity_history() {
    using namespace sketch;
    desktop::MainWindow window;
    const std::vector<BuildingObject> objects{
        RectangularColumn{"workflow-column", {1.0, 2.0, 0.0}, 0.4, 0.6, 3.0, 0.2},
        CircularColumn{"workflow-round", {3.0, 2.0, 0.0}, 0.25, 3.0},
        Beam{"workflow-beam", {1.0, 2.0, 3.0}, {4.0, 2.0, 3.0}, {0.0, 0.0, 1.0}, 0.2, 0.3},
        StairFlight{"workflow-stair", {5.0, 0.0, 0.0}, 0.0, 4, 0.8, 0.25, 1.0, StairLanding{0.6, 0.15}},
        SlopedRoofPanel{"workflow-shed", {0.0, 0.0, 4.0}, 0.0, 4.0, 3.0, 1.0, std::atan(0.25), 0.2, 0.1},
        GableRoof{"workflow-gable", {8.0, 0.0, 4.0}, 0.2, 5.0, 4.0, 1.0, std::atan(0.5), 0.2, 0.1},
    };
    std::vector<QString> identities;
    for (const auto& object : objects) {
        auto entity = encode_building_entity(object, {{"fixture_metadata", "retained"}});
        const auto id = window.commitBuildingObject(entity, window.document().revision());
        require(!id.isEmpty() && navigator_item(window, id), "each building form is authorable and navigable");
        identities.push_back(id);
        const auto before = window.document().snapshot().entities().at(id.toStdString());
        auto edit = before;
        const auto key = before.type == "column" ? "height_m" : before.type == "beam" ? "width_m" :
                         before.type == "stair" ? "width_m" : "thickness_m";
        edit.properties[key] = edit.properties.at(key).get<double>() * 1.1;
        require(!window.commitBuildingObject(edit, window.document().revision(), true).isEmpty(),
                "each building form supports an atomic dimension edit");
        require(window.undoCommand() && window.document().snapshot().entities().at(id.toStdString()) == before,
                "each form's undo restores exact geometry and metadata");
        require(window.redoCommand() && window.selectEntity(id), "each form's edit can be redone and selected");
    }
    desktop::BuildingObjectDialog dialog(std::nullopt, false);
    auto* width = dialog.findChild<QLineEdit*>(QStringLiteral("buildingObjectWidth"));
    require(width != nullptr, "exact-entry fixture needs the column width field");
    width->setText(QStringLiteral("1/3 ft"));
    require(dialog.submit() && dialog.candidate(), "fractional building input submits");
    const auto exact_id = window.commitBuildingObject(*dialog.candidate(), window.document().revision());
    require(!exact_id.isEmpty(), "exact entry commits through the same document command");
    auto original = window.document().snapshot().entities().at(exact_id.toStdString());
    original.properties["future_dimension"] = 42;
    original.properties["quantity_entries"]["/future_dimension"] = {{"version", 99}, {"opaque", "retain"}};
    window.document().apply(ApplyEntityChanges{
        .expected_revision = window.document().revision(),
        .entity_changes = {EntityChange::upsert(original)},
        .message = "optional future quantity metadata fixture",
    });
    const auto receipt = original.properties.at("quantity_entries").at("/width_m");
    require(receipt.at("original_expression") == "1/3 ft" && receipt.at("exact_metres").at("numerator") == 127 &&
                receipt.at("exact_metres").at("denominator") == 1250, "exact rational and expression reach the document");
    desktop::BuildingObjectDialog edit_dialog(original, true);
    edit_dialog.findChild<QLineEdit*>(QStringLiteral("buildingObjectHeight"))->setText("3500 mm");
    require(edit_dialog.submit() && edit_dialog.candidate(), "edit another dimension through the dialog");
    require(!window.commitBuildingObject(*edit_dialog.candidate(), window.document().revision(), true).isEmpty(),
            "edited quantity records merge through the command boundary");
    require(window.document().snapshot().entities().at(exact_id.toStdString()).properties.at("quantity_entries").at("/width_m") == receipt,
            "editing height retains the original width expression");
    require(window.document().snapshot().entities().at(exact_id.toStdString()).properties.at("quantity_entries").at("/future_dimension") ==
                original.properties.at("quantity_entries").at("/future_dimension"),
            "unrelated edits preserve unknown optional quantity metadata");
    require(window.undoCommand() && window.redoCommand(), "exact entries participate in undo and redo");
    require(window.selectEntity(exact_id), "reselect exact object");
    auto programmatic = window.document().snapshot().entities().at(exact_id.toStdString());
    programmatic.properties["width_m"] = 0.2;
    require(!window.commitBuildingObject(programmatic, window.document().revision(), true).isEmpty(),
            "programmatic dimension edit remains supported");
    require(!window.document().snapshot().entities().at(exact_id.toStdString()).properties.at("quantity_entries").contains("/width_m"),
            "programmatic change must invalidate the stale width receipt");
    require(window.undoCommand() && window.selectEntity(exact_id), "undo restores the exact entry");
    auto forged = window.document().snapshot().entities().at(exact_id.toStdString());
    forged.properties["quantity_entries"]["/width_m"]["original_expression"] = "1/2 ft";
    const auto revision = window.document().revision();
    require(window.commitBuildingObject(forged, revision, true).isEmpty() && window.document().revision() == revision,
            "inconsistent quantity provenance rejects the entire edit");
    forged = window.document().snapshot().entities().at(exact_id.toStdString());
    forged.properties["quantity_entries"]["/version"] = {
        {"version", 1}, {"original_expression", "1 m"}, {"entered_unit", "m"},
        {"exact_metres", {{"numerator", 1}, {"denominator", 1}}}};
    require(window.commitBuildingObject(forged, revision, true).isEmpty() && window.document().revision() == revision,
            "quantity provenance cannot describe a dimensionless schema version as metres");
    forged = window.document().snapshot().entities().at(exact_id.toStdString());
    forged.properties["base_center_m"][0] = -1.0;
    forged.properties["quantity_entries"]["/base_center_m/0"] = {
        {"version", 1}, {"original_expression", "-1 m"}, {"entered_unit", "m"},
        {"exact_metres", {{"numerator", std::numeric_limits<std::uint64_t>::max()}, {"denominator", 1}}}};
    require(window.commitBuildingObject(forged, revision, true).isEmpty() && window.document().revision() == revision,
            "oversized unsigned quantity numerators cannot wrap into a valid signed measurement");
    QTemporaryDir directory;
    require(directory.isValid(), "six-form fixture needs a temporary directory");
    const auto path = directory.filePath("six-forms.bldproj");
    require(window.saveProjectAs(path) && window.openProject(path), "all forms save and reopen");
    auto* plan = dynamic_cast<desktop::PlanCanvas*>(window.findChild<QWidget*>("measurementPlanCanvas"));
    require(plan != nullptr, "shared plan/PDF scene is available");
    for (const auto& id : identities) {
        require(navigator_item(window, id) && window.selectEntity(id), "every reopened form remains selectable");
        require(std::any_of(plan->entities().begin(), plan->entities().end(), [&](const auto& entry) {
            return entry.id == id && !entry.segments.empty();
        }), "each reopened form appears in the shared vector output scene");
    }
    require(window.selectEntity({}), "clear selection before keyboard-style navigator selection");
    auto* tree = window.findChild<QTreeWidget*>(QStringLiteral("projectNavigator"));
    tree->setCurrentItem(navigator_item(window, identities.front()));
    QApplication::processEvents();
    require(window.selectedEntityId() == identities.front(),
            "navigator current-item selection must update the workspace without a mouse click");
    tree->setCurrentItem(navigator_item(window, identities.back()));
    require(window.openProject(path), "open while a navigator selection callback is queued");
    QApplication::processEvents();
    require(window.selectedEntityId().isEmpty(), "queued selection from an old document must not affect a reopened document");
    require(window.document().snapshot().entities().at(exact_id.toStdString()).properties.at("quantity_entries").at("/width_m") == receipt,
            "fractional input survives save/reopen without losing provenance");
    require(window.exportDraftPdf(directory.filePath("six-forms.pdf")), "the six-form scene exports through the shared PDF renderer");
    const auto svg = directory.filePath("six-forms.svg");
    require(window.exportDraftSvg(svg), "the six-form scene exports through the shared SVG renderer");
    QFile svg_file(svg);
    require(svg_file.open(QIODevice::ReadOnly | QIODevice::Text), "draft SVG should be readable");
    const auto svg_text = QString::fromUtf8(svg_file.readAll());
    require(svg_text.contains("<svg") && svg_text.contains("DRAFT"),
            "draft SVG should contain vector markup and its draft stamp");
}

}  // namespace

int main(int argc, char** argv) {
    sketch::testing::noninteractive_errors();
    QApplication application(argc, argv);
    {
        QTemporaryDir stamp_directory;
        require(stamp_directory.isValid(), "stamp test directory should be available");
        const auto path = stamp_directory.filePath("native.png");
        QImage original(640, 360, QImage::Format_ARGB32);
        original.fill(QColor(17, 31, 47, 90));
        require(original.save(path), "stamp fixture should be writable");
        const auto stamp = QStringLiteral("DRAFT — internal checkpoint");
        require(sketch::desktop::stampDraftImage(path, stamp), "native image should receive a draft stamp");
        const QImage stamped(path);
        require(stamped.width() == original.width() && stamped.height() > original.height(),
                "draft footer should extend the image without covering the model");
        require(stamped.copy(original.rect()) == original,
                "draft stamp should preserve model pixels including alpha");
        bool has_text = false;
        for (int y = original.height(); y < stamped.height(); ++y) {
            for (int x = 0; x < stamped.width(); ++x) {
                const auto pixel = stamped.pixelColor(x, y);
                if (pixel.red() > pixel.green() + 30 && pixel.red() > pixel.blue() + 30) has_text = true;
            }
        }
        require(has_text, "draft footer must contain visible red text");
        const auto missing = stamp_directory.filePath("missing.png");
        require(!sketch::desktop::stampDraftImage(missing, stamp), "unreadable image must fail stamping");
        require(!QFile::exists(missing), "failed stamping must not create an image");
        const auto blocked = stamp_directory.filePath("blocked.png");
        require(original.save(blocked), "blocked stamp fixture should be writable initially");
        require(QFile::setPermissions(blocked, QFileDevice::ReadOwner), "stamp fixture should become read-only");
        require(!sketch::desktop::stampDraftImage(blocked, stamp), "unwritable image must fail stamping");
        require(QFile::setPermissions(blocked, QFileDevice::ReadOwner | QFileDevice::WriteOwner),
                "stamp fixture permissions should be restored");
    }
    test_organization_context();
    test_six_form_authoring_and_quantity_history();
    auto document = std::make_shared<sketch::Document>(sketch::Document::create());
    sketch::desktop::MainWindow window(document);

    require(window.workspaceDocumentsShareDocument(),
            "measurement and architectural workspaces must share one document");
    require(window.document().snapshot().entities().contains("property-1"),
            "a new document must create the property scaffold before objects");
    require(window.document().snapshot().entities().contains("sheet-view-1") &&
                window.document().snapshot().entities().at("sheet-view-1").type == "sheet_view_model",
            "a new document must include a validated coordinated sheet/view model");
    require(window.document().snapshot().entities().contains("annotations-1") &&
                window.document().snapshot().entities().at("annotations-1").type == "annotation_state",
            "a new document must include a validated annotation state");
    require(window.document().snapshot().entities().contains("floor-1"),
            "a new document must create the floor scaffold before objects");
    const auto seeded_sheet = window.document().snapshot().entities().at("sheet-view-1");
    const auto& seeded_views = seeded_sheet.properties.at("model").at("views");
    const auto has_view = [&](const char* id, const char* kind) {
        return std::any_of(seeded_views.begin(), seeded_views.end(), [&](const auto& view) {
            return view.at("id") == id && view.at("kind") == kind;
        });
    };
    require(has_view("view-plan", "plan") && has_view("view-elevation", "elevation") &&
                has_view("view-section", "section"),
            "a new document must persist coordinated plan, elevation and section views");
    const auto& seeded_viewports = seeded_sheet.properties.at("model")
                                       .at("sheets").at(0).at("viewports");
    const auto has_viewport = [&](const char* id, const char* view_id) {
        return std::any_of(seeded_viewports.begin(), seeded_viewports.end(),
                           [&](const auto& viewport) {
                               return viewport.at("id") == id && viewport.at("view_id") == view_id;
                           });
    };
    require(has_viewport("viewport-plan", "view-plan") &&
                has_viewport("viewport-elevation", "view-elevation") &&
                has_viewport("viewport-section", "view-section"),
            "a new sheet must persist a viewport for each coordinated view");

    const auto label_id = window.createAnnotationLabel(
        QStringLiteral("bedroom"), QStringLiteral("Primary bedroom"), {1.0, 1.0});
    const auto symbol_id = window.createAnnotationSymbol(
        QStringLiteral("chair-w1-d1"), {2.0, 1.0});
    require(!label_id.isEmpty() && !symbol_id.isEmpty(),
            "annotation authoring should create a label and a symbol");
    auto annotation_state = decode_annotation_entity(
        window.document().snapshot().entities().at("annotations-1"));
    require(annotation_state.labels.size() == 1 && annotation_state.symbols.size() == 1,
            "annotation authoring should update the typed annotation entity");
    require(window.editAnnotation(label_id, QStringLiteral("Primary bedroom suite"),
                                  QStringLiteral("3.25"), QStringLiteral("2.5"),
                                  QStringLiteral("30"), QStringLiteral("1.5"), true),
            "annotation editing should use the typed command path");
    annotation_state = decode_annotation_entity(
        window.document().snapshot().entities().at("annotations-1"));
    require(annotation_state.labels.front().content == "Primary bedroom suite" &&
                std::abs(annotation_state.labels.front().placement.position.x - 3.25) < 1e-9 &&
                std::abs(annotation_state.labels.front().placement.position.y - 2.5) < 1e-9 &&
                std::abs(annotation_state.labels.front().placement.scale - 1.5) < 1e-9 &&
                annotation_state.labels.front().visible,
            "annotation editing should persist text, position, scale, and visibility");
    require(window.selectEntity(label_id), "a persisted annotation child should be selectable");
    require(window.deleteAnnotation(label_id), "annotation deletion should be undoable");
    annotation_state = decode_annotation_entity(
        window.document().snapshot().entities().at("annotations-1"));
    require(annotation_state.labels.empty() && annotation_state.symbols.size() == 1,
            "annotation deletion should remove only the selected child");
    require(window.undoCommand() && window.redoCommand() && window.undoCommand(),
            "annotation deletion should participate in normal undo and redo");
    annotation_state = decode_annotation_entity(
        window.document().snapshot().entities().at("annotations-1"));
    require(annotation_state.labels.size() == 1 && annotation_state.symbols.size() == 1,
            "annotation undo should restore the persisted child");

    QTemporaryDir reference_directory;
    require(reference_directory.isValid(), "reference fixture needs a temporary directory");
    const auto reference_path = reference_directory.filePath("trace.png");
    QImage reference_image(40, 20, QImage::Format_ARGB32);
    reference_image.fill(QColor(220, 240, 255));
    require(reference_image.save(reference_path, "PNG"), "reference fixture should save a PNG");
    const auto reference_id = window.importReferenceImage(reference_path);
    require(!reference_id.isEmpty(), "a local raster should import into the document");
    const auto reference_snapshot = window.document().snapshot();
    const auto reference_entity = reference_snapshot.entities().find(reference_id.toStdString());
    require(reference_entity != reference_snapshot.entities().end() &&
                reference_entity->second.type == "reference_asset",
            "reference import should create a typed reference entity");
    const auto reference_asset_id = reference_entity->second.properties.at("asset_id").get<std::string>();
    require(reference_snapshot.assets().contains(reference_asset_id),
            "reference import should retain source bytes in the project asset store");
    auto* reference_canvas = dynamic_cast<sketch::desktop::PlanCanvas*>(
        window.findChild<QWidget*>(QStringLiteral("measurementPlanCanvas")));
    require(reference_canvas != nullptr && reference_canvas->reference().has_value() &&
                !reference_canvas->reference()->image.isNull(),
            "reference import should feed the shared canvas underlay");
    require(window.calibrateReference(reference_id, QStringLiteral("0"), QStringLiteral("0"),
                                      QStringLiteral("40"), QStringLiteral("0"),
                                      QStringLiteral("2 ft")),
            "reference known-distance calibration should use typed document history");
    const auto calibrated_reference = window.document().snapshot().entities().at(reference_id.toStdString());
    require(calibrated_reference.properties.at("calibration_first_source") ==
                nlohmann::json::array({0.0, 0.0}) &&
                calibrated_reference.properties.at("calibration_second_source") ==
                nlohmann::json::array({40.0, 0.0}) &&
                calibrated_reference.properties.at("calibration_known_distance") == "2 ft" &&
                std::abs(calibrated_reference.properties.at("metres_per_source_unit").get<double>() -
                         0.6096 / 40.0) < 1e-12,
            "reference calibration should retain source points, expression, and exact scale");
    require(window.editReferenceTransform(reference_id, QStringLiteral("1.25"), QStringLiteral("-0.5"),
                                          QStringLiteral("0.02"), QStringLiteral("1.5"),
                                          QStringLiteral("15"), QStringLiteral("0.4"), true,
                                          false, true),
            "reference calibration and transform should use typed document history");
    const auto transformed_reference = window.document().snapshot().entities().at(reference_id.toStdString());
    require(transformed_reference.properties.at("position_m") ==
                nlohmann::json::array({1.25, -0.5}) &&
                transformed_reference.properties.at("metres_per_source_unit") == 0.02 &&
                transformed_reference.properties.at("rotation_degrees") == 15.0 &&
                transformed_reference.properties.at("intensity") == 0.4 &&
                transformed_reference.properties.at("flip_horizontal") == true,
            "reference transform should persist calibration, placement, and presentation");
    require(window.undoCommand() && window.redoCommand(),
            "reference transform should participate in undo and redo");

    QTemporaryDir pdf_directory;
    require(pdf_directory.isValid(), "PDF reference fixture needs a temporary directory");
    const auto reference_pdf_path = pdf_directory.filePath("trace.pdf");
    {
        QPdfWriter writer(reference_pdf_path);
        writer.setPageSize(QPageSize(QPageSize::A4));
        writer.setResolution(144);
        QPainter painter(&writer);
        require(painter.isActive(), "PDF reference fixture should open a writer");
        painter.setPen(QPen(QColor(60, 80, 100), 8));
        painter.drawRect(QRectF(40.0, 40.0, 500.0, 320.0));
        painter.drawLine(QPointF(40.0, 200.0), QPointF(540.0, 200.0));
        painter.end();
    }
    sketch::desktop::MainWindow pdf_window;
    const auto pdf_id = pdf_window.importReferenceImage(reference_pdf_path);
    require(!pdf_id.isEmpty(), "a local PDF first page should import into the document");
    const auto pdf_snapshot = pdf_window.document().snapshot();
    const auto pdf_entity = pdf_snapshot.entities().find(pdf_id.toStdString());
    require(pdf_entity != pdf_snapshot.entities().end(), "PDF import should create a reference entity");
    const auto source_asset_id = pdf_entity->second.properties.at("asset_id").get<std::string>();
    const auto render_asset_id = pdf_entity->second.properties.at("render_asset_id").get<std::string>();
    require(pdf_snapshot.assets().at(source_asset_id).media_type == "application/pdf" &&
                source_asset_id != render_asset_id && pdf_snapshot.assets().contains(render_asset_id),
            "PDF import should retain source bytes and a separate preview asset");
    const auto& preview_asset = pdf_snapshot.assets().at(render_asset_id);
    const QByteArray preview_raw(reinterpret_cast<const char*>(preview_asset.bytes.data()),
                                 static_cast<qsizetype>(preview_asset.bytes.size()));
    require(!QImage::fromData(preview_raw).isNull(), "PDF import should decode a local first-page preview");
    auto* pdf_canvas = dynamic_cast<sketch::desktop::PlanCanvas*>(
        pdf_window.findChild<QWidget*>(QStringLiteral("measurementPlanCanvas")));
    require(pdf_canvas != nullptr && pdf_canvas->reference().has_value() &&
                !pdf_canvas->reference()->image.isNull(),
            "PDF import should feed the shared canvas underlay");
    require(pdf_window.undoCommand() && pdf_window.redoCommand(),
            "PDF import should participate in normal document history");
    require(window.editArchitecturalViewPresentation(
                QStringLiteral("view-plan"), QStringLiteral("1.5"), QStringLiteral("80"),
                QStringLiteral("0.7"), QStringLiteral("0.25"), true,
                QStringLiteral("concrete"), QStringLiteral("2"), QStringLiteral("fine")),
            "architectural view presentation must commit through typed Document history");
    const auto edited_view_entity = window.document().snapshot().entities().at("sheet-view-1");
    const auto plan_view = std::find_if(
        edited_view_entity.properties.at("model").at("views").begin(),
        edited_view_entity.properties.at("model").at("views").end(),
        [](const auto& view) { return view.at("id") == "view-plan"; });
    require(plan_view != edited_view_entity.properties.at("model").at("views").end() &&
                plan_view->at("presentation").at("cut_depth_m") == 1.5 &&
                plan_view->at("presentation").at("detail") == "fine" &&
                plan_view->at("presentation").at("hatch_scale") == 2.0,
            "architectural view presentation edit must persist typed settings");
    require(window.undoCommand() && window.redoCommand(),
            "architectural view presentation edit must participate in normal document history");

    const auto boundary_id = window.createBoundary(
        sketch::Boundary{{{{0.0, 0.0}, {3.0, 0.0}, 0.0},
                          {{3.0, 0.0}, {3.0, 2.0}, 0.0},
                          {{3.0, 2.0}, {0.0, 2.0}, 0.0},
                          {{0.0, 2.0}, {0.0, 0.0}, 0.0}}});
    require(!boundary_id.isEmpty(), "closed boundary should be accepted as a document command");

    auto* calculation_status = window.findChild<QLabel*>(QStringLiteral("calculationStatus"));
    auto* base_area = window.findChild<QLabel*>(QStringLiteral("calculationBaseArea"));
    auto* net_area = window.findChild<QLabel*>(QStringLiteral("calculationNetArea"));
    auto* factored_area = window.findChild<QLabel*>(QStringLiteral("calculationFactoredArea"));
    auto* perimeter_value = window.findChild<QLabel*>(QStringLiteral("calculationPerimeter"));
    auto* rounding_value = window.findChild<QLabel*>(QStringLiteral("calculationRounding"));
    auto* building_total = window.findChild<QLabel*>(QStringLiteral("calculationBuildingTotal"));
    auto* living_total = window.findChild<QLabel*>(QStringLiteral("calculationLivingTotal"));
    require(calculation_status != nullptr && base_area != nullptr && net_area != nullptr &&
                factored_area != nullptr && perimeter_value != nullptr && rounding_value != nullptr &&
                building_total != nullptr && living_total != nullptr,
            "calculation inspector labels should be available for a selected boundary");
    require(!calculation_status->text().contains(QStringLiteral("blocked"), Qt::CaseInsensitive),
            "a classified boundary should have an active calculation profile rule");
    require(base_area->text().contains(QStringLiteral("64.58")) &&
                net_area->text().contains(QStringLiteral("64.58")) &&
                factored_area->text().contains(QStringLiteral("64.58")),
            "selected boundary should show base, net, and factored area in default imperial units");
    require(!perimeter_value->text().isEmpty() && perimeter_value->text() != QStringLiteral("—"),
            "selected boundary should show its perimeter");
    require(rounding_value->text().contains(QStringLiteral("unrounded"), Qt::CaseInsensitive) &&
                rounding_value->text().contains(QStringLiteral("difference"), Qt::CaseInsensitive),
            "selected boundary should show the area rounding explanation");

    require(window.editSelectedFactor(QStringLiteral("3/4")),
            "exact rational area factor should be editable from the inspector API");
    const auto factor_snapshot = window.document().snapshot();
    const auto& factor_entities = factor_snapshot.entities();
    const auto factor_entity = factor_entities.find(boundary_id.toStdString());
    require(factor_entity != factor_entities.end() &&
                factor_entity->second.properties.at("factor_expression") == "3/4" &&
                factor_entity->second.properties.at("factor_numerator").get<std::int64_t>() == 3 &&
                factor_entity->second.properties.at("factor_denominator").get<std::int64_t>() == 4 &&
                std::abs(factor_entity->second.properties.at("factor").get<double>() - 0.75) < 1e-12,
            "factor edit should preserve the expression, rational components, and numeric value");
    require(factored_area->text().contains(QStringLiteral("48.44")),
            "factored area should apply the exact rational factor");
    require(window.undoCommand(), "factor edit should be undoable");
    const auto factor_undo_snapshot = window.document().snapshot();
    const auto& factor_undo_entities = factor_undo_snapshot.entities();
    const auto factor_undo = factor_undo_entities.find(boundary_id.toStdString());
    require(factor_undo != factor_undo_entities.end() &&
                factor_undo->second.properties.at("factor_numerator").get<std::int64_t>() == 1,
            "undo should restore the original area factor");
    require(window.redoCommand(), "factor edit should be redoable");
    require(window.selectEntity(boundary_id), "boundary should be reselected after factor history");
    const auto factor_redo_snapshot = window.document().snapshot();
    const auto& factor_redo_entities = factor_redo_snapshot.entities();
    const auto factor_redo = factor_redo_entities.find(boundary_id.toStdString());
    require(factor_redo != factor_redo_entities.end() &&
                factor_redo->second.properties.at("factor_expression") == "3/4",
            "redo should restore the exact factor expression");

    require(window.editSelectedClassification(QStringLiteral("unassigned")),
            "boundary classification should be editable through the shared command path");
    require(calculation_status->text().contains(QStringLiteral("blocked"), Qt::CaseInsensitive) &&
                calculation_status->text().contains(QStringLiteral("no calculation profile rule"),
                                                     Qt::CaseInsensitive),
            "unknown classification should visibly block totals");
    const auto profile_before_snapshot = window.document().snapshot();
    const auto& profile_before_entities = profile_before_snapshot.entities();
    const auto profile_before = profile_before_entities.at("property-1");
    const auto profile_version_before =
        profile_before.properties.at("calculation_profile").at("version").get<unsigned>();
    require(window.setSelectedCalculationRule(true, true),
            "calculation profile rule should be assignable explicitly");
    const auto profile_after_snapshot = window.document().snapshot();
    const auto& profile_after_entities = profile_after_snapshot.entities();
    const auto profile_after = profile_after_entities.at("property-1");
    require(profile_after.properties.at("calculation_profile").at("version").get<unsigned>() ==
                profile_version_before + 1 &&
                profile_after.properties.at("calculation_profile").at("classifications")
                        .at("unassigned")
                        .at("building_total")
                        .get<bool>() &&
                profile_after.properties.at("calculation_profile").at("classifications")
                        .at("unassigned")
                        .at("living_total")
                        .get<bool>(),
            "profile edits should version and persist explicit building and living rules");
    require(!calculation_status->text().contains(QStringLiteral("blocked"), Qt::CaseInsensitive) &&
                !building_total->text().contains(QStringLiteral("—")) &&
                !living_total->text().contains(QStringLiteral("—")),
            "assigned profile rules should restore building and living totals");
    const auto overlapping_boundary_id = window.createBoundary(
        sketch::Boundary{{{{0.5, 0.5}, {1.5, 0.5}, 0.0},
                          {{1.5, 0.5}, {1.5, 1.5}, 0.0},
                          {{1.5, 1.5}, {0.5, 1.5}, 0.0},
                          {{0.5, 1.5}, {0.5, 0.5}, 0.0}}});
    require(!overlapping_boundary_id.isEmpty(), "overlapping test boundary should be created");
    require(calculation_status->text().contains(QStringLiteral("overlap"), Qt::CaseInsensitive) &&
                base_area->text() == QStringLiteral("—") && building_total->text() == QStringLiteral("—"),
            "overlapping boundaries should block totals instead of showing plausible values");
    require(window.undoCommand(), "overlapping test boundary should be undoable");
    require(window.selectEntity(boundary_id), "original boundary should be selectable after overlap undo");
    require(!calculation_status->text().contains(QStringLiteral("blocked"), Qt::CaseInsensitive),
            "undoing the overlap should restore valid totals");
    window.setMetricUnits(true);
    require(base_area->text().contains(QStringLiteral("6.00 m²")) &&
                factored_area->text().contains(QStringLiteral("4.50 m²")),
            "calculation values should refresh in metric display units");
    window.setMetricUnits(false);

    const auto wall_id = window.createStraightWall({0.0, 0.0}, {3.0, 0.0}, "exterior");
    require(!wall_id.isEmpty(), "straight wall should be accepted as a document command");
    require(window.selectEntity(wall_id), "created wall should be selectable");
    require(window.editSelectedClassification("party"),
            "wall classification should be editable from the inspector API");
    require(window.editSelectedHeight("8 ft"),
            "wall height should parse and update through the inspector API");
    require(window.editSelectedThickness("6 in"),
            "wall thickness should parse and update through the inspector API");

    require(window.undoCommand(), "wall property edit should be undoable");
    require(window.redoCommand(), "wall property edit should be redoable");

    // Hosted openings are separate semantic entities. The wall preview is
    // passed through the architecture kernel before the atomic document
    // command is applied, so an out-of-bounds opening must leave the revision
    // untouched.
    require(window.selectEntity(wall_id), "wall should remain selected for hosted opening creation");
    const auto revision_before_invalid_opening = window.document().revision();
    require(window.createHostedOpening(QStringLiteral("door"), QStringLiteral("1 m"),
                                       QStringLiteral("20 m"), QStringLiteral("0 m"),
                                       QStringLiteral("2 m"))
                .isEmpty(),
            "an opening outside the host wall must be rejected by the kernel preview");
    require(window.document().revision() == revision_before_invalid_opening,
            "rejected opening preview must not mutate the document");

    const auto opening_id = window.createHostedOpening(
        QStringLiteral("door"), QStringLiteral("0.25 m"), QStringLiteral("0.5 m"),
        QStringLiteral("0 m"), QStringLiteral("2 m"));
    require(!opening_id.isEmpty(), "a hosted door should be accepted on the selected wall");
    require(window.selectEntity(opening_id), "created opening should be selectable");
    require(window.editSelectedLength("0.6 m"),
            "opening width should use the quantity parser and inspector command");
    require(window.editSelectedHeight("1.9 m"),
            "opening height should use the quantity parser and inspector command");
    require(window.editSelectedClassification("window"),
            "opening classification should distinguish door and window semantics");

    require(window.selectEntity(boundary_id), "closed boundary should be selectable for slab creation");
    const auto revision_before_invalid_slab = window.document().revision();
    require(window.createSlabFromSelectedBoundary("0 m", "0 m").isEmpty(),
            "a zero thickness slab must be rejected before the document command");
    require(window.document().revision() == revision_before_invalid_slab,
            "rejected slab preview must not mutate the document");
    const auto slab_id = window.createSlabFromSelectedBoundary("0.15 m", "0 m");
    require(!slab_id.isEmpty(), "a slab should be created from the selected closed boundary");
    require(window.selectEntity(slab_id), "created slab should be selectable");
    require(window.editSelectedThickness("0.2 m"),
            "slab thickness should use the quantity parser and kernel preview");
    require(window.undoCommand(), "slab thickness edit should be undoable");
    const auto slab_undo_snapshot = window.document().snapshot();
    const auto& slab_undo_entities = slab_undo_snapshot.entities();
    const auto slab_after_undo = slab_undo_entities.find(slab_id.toStdString());
    require(slab_after_undo != slab_undo_entities.end() &&
                slab_after_undo->second.properties.at("thickness_m").get<double>() < 0.16,
            "undo should restore the previous slab thickness");
    require(window.redoCommand(), "slab thickness edit should be redoable");

    const auto architectural_snapshot = window.document().snapshot();
    const auto& architectural_entities = architectural_snapshot.entities();
    const auto opening = architectural_entities.find(opening_id.toStdString());
    require(opening != architectural_entities.end() && opening->second.type == "opening",
            "opening must be persisted as a semantic opening entity");
    require(opening->second.properties.at("wall_id").get<std::string>() == wall_id.toStdString(),
            "opening must retain its host wall reference");
    require(opening->second.properties.at("width_m").get<double>() > 0.59,
            "opening width must be stored in canonical metres");
    const auto slab = architectural_entities.find(slab_id.toStdString());
    require(slab != architectural_entities.end() && slab->second.type == "slab",
            "slab must be persisted as a semantic slab entity");
    require(slab->second.properties.at("boundary").is_array() &&
                slab->second.properties.at("holes").is_array(),
            "slab must persist its boundary and holes arrays");
    require(slab->second.properties.at("thickness_m").get<double>() > 0.19,
            "slab thickness must be stored in canonical metres");
    const auto schedules = window.scheduleSnapshot();
    const auto opening_row = std::find_if(schedules.snapshot.rows.begin(),
                                          schedules.snapshot.rows.end(),
        [&](const auto& row) { return row.object_id == opening_id.toStdString(); });
    require(opening_row != schedules.snapshot.rows.end() &&
                opening_row->cells.contains("area") &&
                !opening_row->cells.at("area").editable &&
                schedules.snapshot.revision == window.document().revision(),
            "shared document schedules must include hosted openings with a read-only calculated area");
    require(window.editScheduleCell(opening_id, QStringLiteral("width"), QStringLiteral("0.7 m")),
            "editable schedule source cells must commit through the document command path");
    require(window.document().snapshot().entities().at(opening_id.toStdString()).properties.at("width_m") == 0.7,
            "schedule edit must update the canonical opening property");
    require(window.undoCommand() &&
                window.document().snapshot().entities().at(opening_id.toStdString()).properties.at("width_m") == 0.6,
            "schedule edit must be undoable from the normal workspace history");
    require(window.redoCommand() &&
                window.document().snapshot().entities().at(opening_id.toStdString()).properties.at("width_m") == 0.7,
            "schedule edit must be redoable from the normal workspace history");

    window.setMetricUnits(true);
    require(window.metricUnits(), "metric display toggle should be observable");
    window.setWorkspace(sketch::desktop::Workspace::architectural);
    require(window.workspace() == sketch::desktop::Workspace::architectural,
            "architectural workspace should be selectable");
    window.setWorkspace(sketch::desktop::Workspace::measurement);

    QTemporaryDir temporary_directory;
    require(temporary_directory.isValid(), "smoke test needs a temporary directory");
    const auto project_path =
        std::filesystem::path(temporary_directory.path().toStdWString()) / "desktop-smoke.bldproj";
    const auto project_path_qstring = QString::fromStdWString(project_path.wstring());
    const auto pdf_path =
        std::filesystem::path(temporary_directory.path().toStdWString()) / "desktop-smoke.pdf";
    const auto pdf_path_qstring = QString::fromStdWString(pdf_path.wstring());
    const auto svg_path =
        std::filesystem::path(temporary_directory.path().toStdWString()) / "desktop-smoke.svg";
    const auto svg_path_qstring = QString::fromStdWString(svg_path.wstring());
    auto column = sketch::encode_building_entity(
        sketch::RectangularColumn{"", {1.0, 2.0, 0.0}, 0.3, 0.4, 3.0, 0.0},
        {{"private_note", "preserve this"}});
    column.properties["future_attribute"] = "preserve this too";
    const auto column_id = window.commitBuildingObject(column, window.document().revision());
    require(!column_id.isEmpty(), "building object must commit through the workspace command");
    require(navigator_item(window, column_id) != nullptr,
            "new building object must be selectable through the project navigator");
    auto* create_object_button = window.findChild<QToolButton*>(QStringLiteral("createBuildingObject"));
    auto* edit_object_button = window.findChild<QPushButton*>(QStringLiteral("editBuildingObject"));
    require(create_object_button && create_object_button->isEnabled() &&
                edit_object_button && !edit_object_button->isHidden() && edit_object_button->isEnabled(),
            "building object tools must be available for the current editable selection");
    auto* plan_widget = window.findChild<QWidget*>(QStringLiteral("measurementPlanCanvas"));
    auto* plan = dynamic_cast<sketch::desktop::PlanCanvas*>(plan_widget);
    require(plan != nullptr, "measurement canvas must expose its derived drawing geometry");
    const auto plan_column = std::find_if(plan->entities().begin(), plan->entities().end(),
        [&](const auto& entity) { return entity.id == column_id; });
    require(plan_column != plan->entities().end() && !plan_column->segments.empty(),
            "new building object must appear in the shared plan/PDF geometry");
    require(plan_column->segments.size() == 4 &&
                std::all_of(plan_column->segments.begin(), plan_column->segments.end(),
                    [](const auto& segment) { return segment.sweep_radians == 0.0; }),
            "shared plan/PDF rectangle must have exactly four straight edges");
    double min_x = 1e9, max_x = -1e9, min_y = 1e9, max_y = -1e9;
    for (const auto& segment : plan_column->segments) {
        for (const auto& point : {segment.start, segment.end}) {
            min_x = std::min(min_x, point.x); max_x = std::max(max_x, point.x);
            min_y = std::min(min_y, point.y); max_y = std::max(max_y, point.y);
        }
    }
    require(std::abs(min_x - 0.85) < 1e-7 && std::abs(max_x - 1.15) < 1e-7 &&
                std::abs(min_y - 1.8) < 1e-7 && std::abs(max_y - 2.2) < 1e-7,
            "column plan must use its real dimensions and world placement");
    auto* architectural_widget = window.findChild<QWidget*>(QStringLiteral("architecturalPlanCanvas"));
    auto* architectural = dynamic_cast<sketch::desktop::PlanCanvas*>(architectural_widget);
    auto* architectural_view = window.findChild<QComboBox*>(QStringLiteral("architecturalView"));
    require(architectural && architectural_view && architectural_view->count() == 3,
            "architectural view selector and canvas should be available");
    architectural_view->setCurrentText(QStringLiteral("Elevation"));
    const auto elevation_column = std::find_if(architectural->entities().begin(),
                                               architectural->entities().end(),
        [&](const auto& entity) { return entity.id == column_id; });
    require(elevation_column != architectural->entities().end() &&
                elevation_column->segments.size() == 4,
            "architectural elevation should project the same column as four edges");
    double elevation_min_y = 1e9, elevation_max_y = -1e9;
    for (const auto& segment : elevation_column->segments) {
        elevation_min_y = std::min({elevation_min_y, segment.start.y, segment.end.y});
        elevation_max_y = std::max({elevation_max_y, segment.start.y, segment.end.y});
    }
    require(std::abs(elevation_min_y) < 1e-7 && std::abs(elevation_max_y - 3.0) < 1e-7,
            "architectural elevation must preserve the column height");
    architectural_view->setCurrentText(QStringLiteral("Section @ 1.2 m"));
    const auto section_column = std::find_if(architectural->entities().begin(),
                                             architectural->entities().end(),
        [&](const auto& entity) { return entity.id == column_id; });
    require(section_column != architectural->entities().end() &&
                section_column->segments.size() == 4,
            "architectural section should intersect the same column with four edges");
    architectural_view->setCurrentText(QStringLiteral("Plan"));
    auto edited_column = column;
    edited_column.properties["height_m"] = 3.5;
    edited_column.extensions = nlohmann::json::object();
    edited_column.properties.erase("future_attribute");
    require(!window.commitBuildingObject(edited_column, window.document().revision(), true).isEmpty(),
            "building object dimensions must be editable");
    const auto edited_column_snapshot = window.document().snapshot();
    const auto& stored_column = edited_column_snapshot.entities().at(column_id.toStdString());
    require(stored_column.properties.at("height_m") == 3.5 &&
                stored_column.properties.at("future_attribute") == "preserve this too" &&
                stored_column.extensions.at("private_note") == "preserve this",
            "geometry edit must preserve unknown semantic fields and extensions");
    const auto stale_column_revision = window.document().revision();
    require(window.undoCommand(), "building object edit must be undoable");
    require(window.document().snapshot().entities().at(column_id.toStdString()).properties.at("height_m") == 3.0,
            "undo restores exact prior building dimensions");
    require(window.selectEntity(column_id), "reselect building object after undo");
    const auto before_stale = window.document().revision();
    require(window.commitBuildingObject(edited_column, stale_column_revision, true).isEmpty() &&
                window.document().revision() == before_stale,
            "stale object dialog must fail atomically");
    require(window.redoCommand(), "building object edit must be redoable");
    require(window.selectEntity(column_id), "reselect building object after redo");
    auto invalid_column = edited_column;
    invalid_column.properties["height_m"] = -1.0;
    const auto before_invalid_column = window.document().revision();
    require(window.commitBuildingObject(invalid_column, before_invalid_column, true).isEmpty() &&
                window.document().revision() == before_invalid_column,
            "invalid building geometry must not mutate the document");
    require(window.commitBuildingObject(column, before_invalid_column).isEmpty(),
            "object creation must reject an existing identity");
    require(window.editSheetMetadata(QStringLiteral("sheet-1"), QStringLiteral("A-102"),
                                     QStringLiteral("Sample property"), QStringLiteral("Issued plans"),
                                     QStringLiteral("Field designer"), QStringLiteral("2026-09-11")),
            "sheet metadata must commit through the typed Document command path");
    const auto edited_sheet_entity = window.document().snapshot().entities().at("sheet-view-1");
    require(edited_sheet_entity.properties.at("model").at("sheets").at(0).at("number") == "A-102" &&
                edited_sheet_entity.properties.at("model").at("sheets").at(0).at("title_block").at("title") == "Issued plans",
            "sheet metadata edit must persist in the canonical sheet/view entity");
    require(window.undoCommand() && window.redoCommand(),
            "sheet metadata edit must participate in normal document history");
    require(window.editSheetViewport(QStringLiteral("sheet-1"), QStringLiteral("viewport-plan"),
                                     QStringLiteral("15"), QStringLiteral("15"),
                                     QStringLiteral("390"), QStringLiteral("267"),
                                     QStringLiteral("75")),
            "sheet viewport must commit through the typed Document command path");
    const auto edited_viewport_entity = window.document().snapshot().entities().at("sheet-view-1");
    const auto& edited_viewports = edited_viewport_entity.properties.at("model")
                                       .at("sheets").at(0).at("viewports");
    const auto edited_viewport = std::find_if(
        edited_viewports.begin(), edited_viewports.end(), [](const auto& viewport) {
            return viewport.at("id") == "viewport-plan";
        });
    require(edited_viewport != edited_viewports.end() &&
                edited_viewport->at("bounds").at("width_mm") == 390.0 &&
                edited_viewport->at("scale_denominator") == 75.0,
            "sheet viewport edit must persist bounds and independent scale");
    require(window.undoCommand() && window.redoCommand(),
            "sheet viewport edit must participate in normal document history");
    require(window.editSheetSchedulePlacement(QStringLiteral("sheet-1"),
                                              QStringLiteral("schedule-doors"),
                                              QStringLiteral("235"), QStringLiteral("225"),
                                              QStringLiteral("175"), QStringLiteral("55")),
            "schedule placement must commit through the typed Document command path");
    const auto edited_schedule_entity = window.document().snapshot().entities().at("sheet-view-1");
    require(edited_schedule_entity.properties.at("model").at("sheets").at(0)
                    .at("schedules").at(0).at("bounds").at("x_mm") == 235.0 &&
                edited_schedule_entity.properties.at("model").at("sheets").at(0)
                    .at("schedules").at(0).at("bounds").at("height_mm") == 55.0,
            "schedule placement edit must persist its page bounds");
    require(window.undoCommand() && window.redoCommand(),
            "schedule placement edit must participate in normal document history");
    const auto revision_before_pdf = window.document().revision();
    require(window.exportDraftPdf(pdf_path_qstring), "draft PDF export should succeed locally");
    require(std::filesystem::file_size(pdf_path) > 0, "draft PDF should be nonempty");
    const auto pdf_fingerprint_path = std::filesystem::path(pdf_path.wstring() + L".fingerprint.json");
    require(std::filesystem::file_size(pdf_fingerprint_path) > 0,
            "draft PDF must have an adjacent output fingerprint");
    QFile pdf_fingerprint(QString::fromStdWString(pdf_fingerprint_path.wstring()));
    require(pdf_fingerprint.open(QIODevice::ReadOnly | QIODevice::Text),
            "draft PDF fingerprint should be readable");
    const auto pdf_fingerprint_json = nlohmann::json::parse(pdf_fingerprint.readAll().toStdString());
    require(pdf_fingerprint_json.at("output_kind") == "pdf" &&
                pdf_fingerprint_json.at("fingerprint").at("digest_sha256").is_string(),
            "draft PDF fingerprint must identify the output and digest");
    pdf_fingerprint.close();
    require(window.exportDraftSvg(svg_path_qstring), "draft SVG export should succeed locally");
    require(std::filesystem::file_size(svg_path) > 0, "draft SVG should be nonempty");
    QFile svg_output(svg_path_qstring);
    require(svg_output.open(QIODevice::ReadOnly | QIODevice::Text),
            "draft SVG should be readable for schedule placement verification");
    const auto svg_text = svg_output.readAll();
    require(svg_text.contains("DOORS SCHEDULE") && svg_text.contains("ELEVATION") &&
                svg_text.contains("SECTION"),
            "draft SVG should render the persisted schedule and coordinated view captions");
    svg_output.close();
    const auto svg_fingerprint_path = std::filesystem::path(svg_path.wstring() + L".fingerprint.json");
    require(std::filesystem::file_size(svg_fingerprint_path) > 0,
            "draft SVG must have an adjacent output fingerprint");
    auto* page_size = window.findChild<QComboBox*>(QStringLiteral("outputPageSize"));
    require(page_size && page_size->count() == 5, "output sheet selector should expose five page sizes");
    page_size->setCurrentText(QStringLiteral("A3"));
    require(window.exportDraftPdf(pdf_path_qstring), "A3 draft PDF export should succeed locally");
    require(std::filesystem::file_size(pdf_path) > 0, "A3 draft PDF should be nonempty");
    require(std::filesystem::file_size(pdf_fingerprint_path) > 0,
            "A3 draft PDF must refresh its adjacent output fingerprint");
    require(window.document().revision() == revision_before_pdf,
            "draft output must not mutate the semantic document");
    require(window.saveProjectAs(project_path_qstring),
            "save-as should persist the current snapshot");
    require(!window.document().dirty(), "saving the current head should clear dirty state");
    require(window.openProject(project_path_qstring),
            "saved project should reopen through the real project store");
    const auto reopened = window.document().snapshot();
    require(reopened.entities().at(column_id.toStdString()).properties.at("height_m") == 3.5 &&
                reopened.entities().at(column_id.toStdString()).extensions.at("private_note") == "preserve this",
            "save/reopen must preserve edited building object dimensions and metadata");
    require(reopened.entities().contains(wall_id.toStdString()),
            "reopened project should preserve the semantic wall entity");
    require(reopened.entities().contains(opening_id.toStdString()),
            "reopened project should preserve the hosted opening entity");
    require(reopened.entities().contains(slab_id.toStdString()),
            "reopened project should preserve the slab entity");
    require(reopened.entities().contains("sheet-view-1") &&
                reopened.entities().at("sheet-view-1").type == "sheet_view_model",
            "save/reopen must preserve the coordinated sheet/view model");
    require(reopened.entities().at(opening_id.toStdString()).properties.at("wall_id") ==
                wall_id.toStdString(),
            "reopened opening should keep the host reference");
    require(reopened.entities().at(boundary_id.toStdString()).properties.at("factor_expression") ==
                "3/4" &&
                reopened.entities().at("property-1").properties.at("calculation_profile")
                        .at("classifications")
                        .at("unassigned")
                        .at("living_total")
                        .get<bool>(),
            "save and reopen should preserve the exact factor and versioned profile rule");
    require(window.selectEntity(boundary_id), "reopened boundary should be selectable");
    require(edit_object_button->isHidden(), "building object editor must hide for a measurement boundary");
    require(!calculation_status->text().contains(QStringLiteral("blocked"), Qt::CaseInsensitive) &&
                base_area->text().contains(QStringLiteral("6.00 m²")) &&
                factored_area->text().contains(QStringLiteral("4.50 m²")),
            "calculation inspector should refresh from the reopened document");

    auto malformed = window.document().snapshot().entities().at(column_id.toStdString());
    malformed.properties["form"] = "unsupported-form";
    window.document().apply(sketch::ApplyEntityChanges{
        .expected_revision = window.document().revision(),
        .entity_changes = {sketch::EntityChange::upsert(malformed)},
        .message = "exercise unsupported loaded geometry",
    });
    require(window.selectEntity(column_id), "select malformed loaded building object");
    const auto protected_output =
        QString::fromStdWString((project_path.parent_path() / "must-not-exist.pdf").wstring());
    const auto protected_svg =
        QString::fromStdWString((project_path.parent_path() / "must-not-exist.svg").wstring());
    require(!window.exportDraftPdf(protected_output) && !window.exportDraftSvg(protected_svg) &&
                !window.showPrintPreview() &&
                !std::filesystem::exists(std::filesystem::path(protected_output.toStdWString())) &&
                !std::filesystem::exists(std::filesystem::path(protected_svg.toStdWString())),
            "invalid plan geometry must block output before creating a file or print dialog");
    auto* plan_error = window.findChild<QLabel*>(QStringLiteral("planGeometryError"));
    require(plan_error && !plan_error->isHidden() && !plan_error->text().isEmpty(),
            "a missing building projection must be visible in the workspace");
    for (int index = 1; index + 1 < argc; ++index) {
        if (std::string_view(argv[index]) == "--capture-invalid-plan") {
            window.setAttribute(Qt::WA_DontShowOnScreen, true);
            window.resize(1366, 768);
            window.show();
            QCoreApplication::processEvents();
            require(window.grab().save(QString::fromLocal8Bit(argv[index + 1])),
                    "invalid-plan workspace capture must be written");
            window.hide();
        }
    }
    require(window.undoCommand(), "undo restores renderable building geometry");
    require(plan_error->isHidden(), "geometry error must clear after the valid object is restored");
    return 0;
}
