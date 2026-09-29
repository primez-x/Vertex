#include "sketch/desktop/main_window.hpp"
#include "sketch/sheet_view_entity_codec.hpp"
#include "support/noninteractive_errors.hpp"

#include <QApplication>
#include <QFile>
#include <QFont>
#include <QFontDatabase>
#include <QGroupBox>
#include <QImage>
#include <QLineEdit>
#include <QPdfDocument>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSize>
#include <QTemporaryDir>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Window = sketch::desktop::MainWindow;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void requireAction(bool condition, const Window& window, const char* message) {
    require(condition, std::string(message) + ": " + window.lastError().toStdString());
}

template <typename Widget>
Widget& control(Window& window, const char* name) {
    auto* result = window.findChild<Widget*>(QString::fromLatin1(name));
    require(result != nullptr, std::string("Missing multipage editor control: ") + name);
    return *result;
}

sketch::SheetViewModel sheetModel(const Window& window) {
    const auto snapshot = window.document().snapshot();
    for (const auto& [id, entity] : snapshot.entities()) {
        (void)id;
        if (entity.type == sketch::kSheetViewEntityType) {
            return sketch::decode_sheet_view_entity(entity);
        }
    }
    throw std::runtime_error("Desktop project has no persisted sheet/view graph");
}

const sketch::DrawingSheet& sheet(const sketch::SheetViewModel& model, const std::string& id) {
    const auto found = std::find_if(model.sheets().begin(), model.sheets().end(),
        [&](const auto& candidate) { return candidate.id == id; });
    require(found != model.sheets().end(), "Persisted page identity was lost: " + id);
    return *found;
}

const sketch::CoordinatedView& planView(const sketch::SheetViewModel& model) {
    const auto found = std::find_if(model.views().begin(), model.views().end(),
        [](const auto& view) { return view.kind == sketch::CoordinatedViewKind::plan; });
    require(found != model.views().end(), "Project has no shared plan view");
    return *found;
}

const sketch::SheetViewport& planViewport(const sketch::SheetViewModel& model,
                                         const std::string& sheet_id) {
    const auto& page = sheet(model, sheet_id);
    const auto& view = planView(model);
    const auto found = std::find_if(page.viewports.begin(), page.viewports.end(),
        [&](const auto& viewport) { return viewport.view_id == view.id; });
    require(found != page.viewports.end(), "Page lost its shared plan viewport: " + sheet_id);
    return *found;
}

nlohmann::json readManifest(const QString& pdf) {
    QFile file(pdf + QStringLiteral(".fingerprint.json"));
    require(file.open(QIODevice::ReadOnly), "Output must publish a readable fingerprint receipt");
    return nlohmann::json::parse(file.readAll().toStdString());
}

void requirePageSize(const QPdfDocument& pdf, int page, double width_mm, double height_mm) {
    const auto points = pdf.pagePointSize(page);
    require(std::abs(points.width() * 25.4 / 72.0 - width_mm) < 0.4 &&
                std::abs(points.height() * 25.4 / 72.0 - height_mm) < 0.4,
            "PDF page order must retain each page's own physical dimensions");
}

void testMultipageUserWorkflow() {
    QTemporaryDir directory;
    require(directory.isValid(), "Multipage fixture needs a temporary directory");
    Window window;
    require(window.workspaceDocumentsShareDocument(),
            "Multipage presentation must use the same authoritative document in both workspaces");

    // Exercise the actual inspector buttons, rather than writing metadata directly.
    requireAction(window.selectEntity(QStringLiteral("property-1")), window,
                  "Subject property must be selectable");
    require(!control<QGroupBox>(window, "projectDetails").isHidden(),
            "Selecting the property must expose the subject editor");
    const auto original_property = window.document().snapshot().entities().at("property-1");
    control<QLineEdit>(window, "projectSubjectName").setText(QStringLiteral("Cedar House"));
    control<QLineEdit>(window, "projectSubjectAddress").setText(QStringLiteral("42 Cedar Lane"));
    control<QLineEdit>(window, "projectSubjectReference").setText(QStringLiteral("CASE-2048"));
    control<QPlainTextEdit>(window, "projectSubjectAttributes").setPlainText(
        QStringLiteral("{\"parcel\":\"B-7\",\"zone\":\"R-2\"}"));
    const auto subject_revision = window.document().revision();
    control<QPushButton>(window, "applyProjectDetails").click();
    const auto subject_property = window.document().snapshot().entities().at("property-1");
    require(window.document().revision() == subject_revision + 1 &&
                subject_property.properties.at("subject") == nlohmann::json({
                    {"name", "Cedar House"}, {"address", "42 Cedar Lane"},
                    {"reference", "CASE-2048"},
                    {"attributes", {{"parcel", "B-7"}, {"zone", "R-2"}}}}),
            "Subject inspector must commit every field as one history command");
    require(window.undoCommand() &&
                window.document().snapshot().entities().at("property-1") == original_property &&
                window.redoCommand() &&
                window.document().snapshot().entities().at("property-1") == subject_property,
            "Subject inspector edit must undo and redo without losing attributes");

    const sketch::Boundary boundary{{{{0, 0}, {6, 0}, 0}, {{6, 0}, {6, 4}, 0},
                                      {{6, 4}, {0, 4}, 0}, {{0, 4}, {0, 0}, 0}}};
    const auto area_id = window.createBoundary(boundary, QStringLiteral("living"));
    const auto wall_id = window.createStraightWall({0, 0}, {6, 0}, QStringLiteral("exterior"));
    requireAction(!area_id.isEmpty() && !wall_id.isEmpty() && window.selectEntity(area_id),
                  window, "Shared model fixture must contain a selectable attributed area");
    require(!control<QGroupBox>(window, "areaAttributes").isHidden(),
            "Selecting a closed area must expose its attributes editor");
    const auto original_area = window.document().snapshot().entities().at(area_id.toStdString());
    control<QPlainTextEdit>(window, "areaAttributesJson").setPlainText(
        QStringLiteral("{\"use\":\"conditioned\",\"finish\":\"oak\"}"));
    const auto area_revision = window.document().revision();
    control<QPushButton>(window, "applyAreaAttributes").click();
    const auto attributed_area = window.document().snapshot().entities().at(area_id.toStdString());
    require(window.document().revision() == area_revision + 1 &&
                attributed_area.properties.at("area_attributes") ==
                    nlohmann::json({{"use", "conditioned"}, {"finish", "oak"}}),
            "Area inspector must commit descriptive attributes to the shared model");
    require(window.undoCommand() &&
                window.document().snapshot().entities().at(area_id.toStdString()) == original_area &&
                window.redoCommand() &&
                window.document().snapshot().entities().at(area_id.toStdString()) == attributed_area,
            "Area attributes must undo and redo atomically");

    const auto before_invalid_attributes = window.document().snapshot();
    control<QPlainTextEdit>(window, "areaAttributesJson").setPlainText(
        QStringLiteral("{\"finish\":17}"));
    control<QPushButton>(window, "applyAreaAttributes").click();
    require(window.document().revision() == before_invalid_attributes.revision() &&
                window.document().snapshot().entities() == before_invalid_attributes.entities() &&
                !window.lastError().isEmpty(),
            "Non-string area attributes must fail without modifying shared data or history");

    auto model = sheetModel(window);
    const auto first_id = model.sheet_order().front();
    const auto plan_id = planView(model).id;
    auto references = std::vector<std::string>{area_id.toStdString(), wall_id.toStdString()};
    std::sort(references.begin(), references.end());
    requireAction(window.editArchitecturalViewPresentation(QString::fromStdString(plan_id),
                      "1.2", "100", "0.5", "0.18", true, "solid", "1", "fine",
                      area_id + QStringLiteral(",") + wall_id, QStringLiteral("-1,8,-1,6")),
                  window, "Shared plan sources and presentation must be editable");
    const auto portrait = window.createDrawingSheet("A-202", "215.5", "330.2", "Area details");
    requireAction(!portrait.isEmpty(), window, "User must be able to append a portrait page");
    const auto second_id = portrait.toStdString();
    require(sheetModel(window).sheet_order() == std::vector<std::string>{first_id, second_id},
            "New page must append without regenerating existing identities");

    model = sheetModel(window);
    const auto first_viewport_id = planViewport(model, first_id).id;
    const auto second_viewport_id = planViewport(model, second_id).id;
    requireAction(window.editSheetViewport(QString::fromStdString(first_id),
                      QString::fromStdString(first_viewport_id), "12", "14", "140", "105", "125"),
                  window, "First page must accept its own placement and scale");
    const auto first_page_before = sheet(sheetModel(window), first_id);
    const auto geometry_before = window.document().snapshot();
    requireAction(window.editSheetViewport(portrait, QString::fromStdString(second_viewport_id),
                      "15", "20", "100", "100", "50"),
                  window, "Second page must accept an independent placement and scale");
    model = sheetModel(window);
    require(sheet(model, first_id) == first_page_before &&
                planViewport(model, first_id).scale_denominator == 125 &&
                planViewport(model, second_id).scale_denominator == 50 &&
                planViewport(model, first_id).view_id == planViewport(model, second_id).view_id &&
                planView(model).object_ids == references &&
                window.document().snapshot().entities().at(area_id.toStdString()) ==
                    geometry_before.entities().at(area_id.toStdString()) &&
                window.document().snapshot().entities().at(wall_id.toStdString()) ==
                    geometry_before.entities().at(wall_id.toStdString()),
            "Independent page presentation must retain stable links to one unchanged shared model");
    const auto before_invalid_viewport = window.document().snapshot();
    require(!window.editSheetViewport(portrait, QString::fromStdString(second_viewport_id),
                                     "15", "20", "100", "100", "0") &&
                window.document().revision() == before_invalid_viewport.revision() &&
                window.document().snapshot().entities() == before_invalid_viewport.entities(),
            "Invalid page scale must fail without changing the other page or shared model");

    requireAction(window.editSheetMetadata(QString::fromStdString(first_id), "A-101",
                      "Cedar House", "Site overview", "Surveyor", "2026-09-29") &&
                      window.editSheetMetadata(portrait, "A-202", "Cedar House", "Area details",
                                               "Surveyor", "2026-09-29"),
                  window, "Each page must retain an independent title block");
    const auto callout_id = window.addSheetCallout(QString::fromStdString(first_id), "See details",
        portrait, QString::fromStdString(second_viewport_id), "180", "150");
    requireAction(!callout_id.isEmpty(), window, "Pages must support stable cross-page references");
    const auto protected_snapshot = window.document().snapshot();
    require(!window.removeDrawingSheet(portrait) &&
                window.document().revision() == protected_snapshot.revision() &&
                window.document().snapshot().entities() == protected_snapshot.entities(),
            "Removing a referenced page must fail without leaving a dangling callout");

    const auto disposable = window.createDrawingSheet("TEMP", "297", "210", "Working page");
    requireAction(!disposable.isEmpty() && window.removeDrawingSheet(disposable), window,
                  "Unreferenced working pages must be removable");
    require(window.undoCommand() && sheetModel(window).sheet_order().back() == disposable.toStdString() &&
                window.redoCommand() && sheetModel(window).sheet_order().size() == 2,
            "Page deletion must restore the same identity through undo and redo");
    const auto before_reorder = sheetModel(window).to_json();
    requireAction(window.moveDrawingSheet(portrait, -1), window, "User must be able to reorder pages");
    const auto order = std::vector<std::string>{second_id, first_id};
    require(sheetModel(window).sheet_order() == order && window.undoCommand() &&
                sheetModel(window).to_json() == before_reorder && window.redoCommand() &&
                sheetModel(window).sheet_order() == order,
            "Page reorder must be undoable and preserve all page definitions");
    const auto end_snapshot = window.document().snapshot();
    require(!window.moveDrawingSheet(portrait, -1) &&
                window.document().revision() == end_snapshot.revision() &&
                window.document().snapshot().entities() == end_snapshot.entities(),
            "Moving the first page past the end must leave the project untouched");

    const auto project = directory.filePath(QStringLiteral("multipage.bldproj"));
    requireAction(window.saveProjectAs(project), window, "Complete multipage project must save");
    const auto saved = window.document().snapshot();
    requireAction(window.createNewProject(), window, "Fixture must release saved project ownership");
    Window reopened;
    requireAction(reopened.openProject(project), reopened, "Independent desktop window must reopen project");
    require(reopened.document().snapshot().entities() == saved.entities() &&
                sheetModel(reopened).sheet_order() == order,
            "Save/reopen must retain subject, area attributes, page order, view settings, and model links");
    requireAction(reopened.selectEntity(QStringLiteral("property-1")), reopened,
                  "Reopened subject must be selectable");
    require(control<QLineEdit>(reopened, "projectSubjectAddress").text() == "42 Cedar Lane" &&
                control<QLineEdit>(reopened, "projectSubjectReference").text() == "CASE-2048" &&
                control<QPlainTextEdit>(reopened, "projectSubjectAttributes").toPlainText().contains("B-7"),
            "Reopened subject must repopulate the user's inspector");
    requireAction(reopened.selectEntity(area_id), reopened, "Reopened area must be selectable");
    require(control<QPlainTextEdit>(reopened, "areaAttributesJson").toPlainText().contains("conditioned"),
            "Reopened area must repopulate its inspector attributes");

    const auto output_snapshot = reopened.document().snapshot();
    requireAction(reopened.selectOutputSheet(portrait), reopened, "Portrait output page must be selectable");
    const auto set_pdf = directory.filePath(QStringLiteral("drawing-set.pdf"));
    requireAction(reopened.exportDrawingSetPdf(set_pdf), reopened, "Ordered drawing set must export");
    {
        QPdfDocument pdf;
        require(pdf.load(set_pdf) == QPdfDocument::Error::None && pdf.pageCount() == 2,
                "Drawing set PDF must contain every surviving page exactly once");
        requirePageSize(pdf, 0, 215.5, 330.2);
        requirePageSize(pdf, 1, sheet(sheetModel(reopened), first_id).width_mm,
                        sheet(sheetModel(reopened), first_id).height_mm);
        for (int page = 0; page < pdf.pageCount(); ++page) {
            require(!pdf.render(page, QSize(240, 330)).isNull(),
                    "Every exported page must decode and render through Qt PDF");
        }
    }
    const auto first_receipt = readManifest(set_pdf);
    require(first_receipt.at("output_kind") == "pdf-set" &&
                first_receipt.at("sheet_order") == order,
            "Drawing set receipt must bind exact ordered membership");
    requireAction(reopened.selectOutputSheet(QString::fromStdString(first_id)), reopened,
                  "Overview output page must be selectable");
    const auto second_set_pdf = directory.filePath(QStringLiteral("drawing-set-other-selection.pdf"));
    requireAction(reopened.exportDrawingSetPdf(second_set_pdf), reopened,
                  "Complete drawing set must export after local page selection");
    const auto second_receipt = readManifest(second_set_pdf);
    require(first_receipt.at("fingerprint") == second_receipt.at("fingerprint") &&
                second_receipt.at("sheet_order") == order &&
                reopened.document().revision() == output_snapshot.revision() &&
                reopened.document().snapshot().entities() == output_snapshot.entities(),
            "Local output selection and export must preserve data and the complete-set fingerprint");
    const auto selected_pdf = directory.filePath(QStringLiteral("selected-page.pdf"));
    requireAction(reopened.exportDraftPdf(selected_pdf), reopened, "Selected page output must remain available");
    QPdfDocument selected;
    require(selected.load(selected_pdf) == QPdfDocument::Error::None && selected.pageCount() == 1,
            "Selected-page PDF must contain only the chosen overview page");
    const auto reopened_model = sheetModel(reopened);
    requirePageSize(selected, 0, sheet(reopened_model, first_id).width_mm,
                    sheet(reopened_model, first_id).height_mm);

    // Area metadata is shared project data, so a subsequent edit invalidates the
    // complete set while retaining both pages' independently configured views.
    requireAction(reopened.selectEntity(area_id) && reopened.editSelectedAreaAttributes(
                      QStringLiteral("{\"use\":\"conditioned\",\"finish\":\"tile\"}")),
                  reopened, "Shared area attributes must remain editable after reopening");
    require(sheetModel(reopened).to_json() == reopened_model.to_json(),
            "Editing shared area data must preserve all page/view definitions");
    const auto changed_pdf = directory.filePath(QStringLiteral("drawing-set-changed-area.pdf"));
    requireAction(reopened.exportDrawingSetPdf(changed_pdf), reopened,
                  "Drawing set must regenerate from the changed shared model");
    const auto changed_receipt = readManifest(changed_pdf);
    require(changed_receipt.at("sheet_order") == order &&
                changed_receipt.at("fingerprint").at("digest_sha256") !=
                    first_receipt.at("fingerprint").at("digest_sha256"),
            "Complete-set fingerprint must change when shared area metadata changes");
}

void testLastPageCannotBeRemoved() {
    Window window;
    const auto model = sheetModel(window);
    require(model.sheet_order().size() == 1, "New desktop fixture must start with one page");
    const auto before = window.document().snapshot();
    require(!window.removeDrawingSheet(QString::fromStdString(model.sheet_order().front())) &&
                window.document().revision() == before.revision() &&
                window.document().snapshot().entities() == before.entities(),
            "Deleting the final page must reject atomically");
}
} // namespace

int main(int argc, char** argv) {
    sketch::testing::noninteractive_errors();
    QApplication app(argc, argv);
    try {
        const auto font_id = QFontDatabase::addApplicationFont(":/fonts/Inter.ttf");
        const auto families = QFontDatabase::applicationFontFamilies(font_id);
        require(!families.isEmpty(), "Bundled test font failed to load");
        app.setFont(QFont(families.front(), 10));
        testMultipageUserWorkflow();
        testLastPageCannotBeRemoved();
        std::cout << "multipage user workflow checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
