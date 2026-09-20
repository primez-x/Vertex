#include "sketch/desktop/main_window.hpp"

#include "sketch/building_entity.hpp"
#include "sketch/boundary_entity.hpp"
#include "sketch/project_store.hpp"
#include "support/noninteractive_errors.hpp"
#include "../src/desktop/plan_canvas.hpp"

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QFont>
#include <QFontDatabase>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QLabel>
#include <QPushButton>
#include <QPrintPreviewDialog>
#include <QPrinter>
#include <QStyle>
#include <QStyleOptionViewItem>
#include <QTemporaryDir>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string_view>

#include <nlohmann/json.hpp>

namespace {

using sketch::Entity;
using sketch::RectangularColumn;
using sketch::desktop::MainWindow;
using sketch::desktop::PlanCanvas;

void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

void process_events() {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
}

QTreeWidget* navigator(MainWindow& window) {
    auto* tree = window.findChild<QTreeWidget*>(QStringLiteral("projectNavigator"));
    require(tree != nullptr, "project navigator must exist");
    return tree;
}

QTreeWidgetItem* navigator_item(MainWindow& window, const QString& id) {
    QTreeWidgetItem* result = nullptr;
    for (QTreeWidgetItemIterator iterator(navigator(window)); *iterator; ++iterator) {
        if ((*iterator)->data(0, Qt::UserRole).toString() == id) {
            require(result == nullptr, "navigator must list each entity exactly once");
            result = *iterator;
        }
    }
    return result;
}

QTreeWidgetItem* preceding_visible_item(QTreeWidget* tree, QTreeWidgetItem* target) {
    QTreeWidgetItem* previous = nullptr;
    for (QTreeWidgetItemIterator iterator(tree); *iterator; ++iterator) {
        if ((*iterator)->isHidden()) {
            continue;
        }
        if (*iterator == target) {
            return previous;
        }
        previous = *iterator;
    }
    return nullptr;
}

PlanCanvas* canvas(MainWindow& window, const QString& object_name) {
    auto* widget = window.findChild<QWidget*>(object_name);
    auto* result = dynamic_cast<PlanCanvas*>(widget);
    require(result != nullptr, "plan canvas must be available");
    return result;
}

bool canvas_contains(PlanCanvas* canvas_widget, const QString& id) {
    return std::any_of(canvas_widget->entities().begin(), canvas_widget->entities().end(),
                       [&](const auto& entity) { return entity.id == id; });
}

QRect check_indicator(QTreeWidget* tree, QTreeWidgetItem* item) {
    QStyleOptionViewItem option;
    option.initFrom(tree);
    option.index = tree->indexFromItem(item, 0);
    option.rect = tree->visualItemRect(item);
    option.state |= QStyle::State_Enabled;
    option.features |= QStyleOptionViewItem::HasCheckIndicator;
    option.checkState = item->checkState(0);
    return tree->style()->subElementRect(QStyle::SE_ItemViewItemCheckIndicator,
                                         &option, tree);
}

void click_checkbox(QTreeWidget* tree, QTreeWidgetItem* item) {
    const auto point = check_indicator(tree, item).center();
    const auto global = tree->viewport()->mapToGlobal(point);
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(point), QPointF(global), Qt::LeftButton,
                      Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(tree->viewport(), &press);
    QMouseEvent release(QEvent::MouseButtonRelease, QPointF(point), QPointF(global), Qt::LeftButton,
                        Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(tree->viewport(), &release);
}

void press_key(QTreeWidget* tree, int key) {
    QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier);
    QApplication::sendEvent(tree, &press);
    QKeyEvent release(QEvent::KeyRelease, key, Qt::NoModifier);
    QApplication::sendEvent(tree, &release);
}

void press_space(QTreeWidget* tree) {
    press_key(tree, Qt::Key_Space);
}

void install_capture_font() {
    (void)QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/Inter.ttf"));
    QApplication::setFont(QFont(QStringLiteral("Inter"), 10));
}

void capture_workspaces(MainWindow& window) {
    const auto directory = qEnvironmentVariable("SKETCH_VISIBILITY_CAPTURE_DIR");
    if (directory.isEmpty()) {
        return;
    }
    require(QDir().mkpath(directory), "visibility capture directory must be writable");
    window.setWorkspace(sketch::desktop::Workspace::measurement);
    process_events();
    window.fitView();
    const auto filtered_pdf = QDir(directory).filePath(QStringLiteral("filtered-view.pdf"));
    require(window.exportDraftPdf(filtered_pdf) && QFileInfo::exists(filtered_pdf) &&
                QFileInfo(filtered_pdf).size() > 0,
            "filtered draft PDF capture failed");
    require(window.grab().save(QDir(directory).filePath(QStringLiteral("measurement.png"))),
            "measurement visibility capture failed");
    window.setWorkspace(sketch::desktop::Workspace::architectural);
    process_events();
    window.fitView();
    require(window.grab().save(QDir(directory).filePath(QStringLiteral("architectural.png"))),
            "architectural visibility capture failed");
}

void test_queued_navigator_events_do_not_cross_project_replacement() {
    MainWindow window;
    window.resize(900, 620);
    window.show();
    process_events();
    QTemporaryDir directory;
    require(directory.isValid(), "queued navigator fixture needs temporary storage");
    const auto path = directory.filePath(QStringLiteral("replacement.bldproj"));
    require(window.saveProjectAs(path), "queued navigator source must save");
    auto* floor = navigator_item(window, QStringLiteral("floor-1"));
    require(floor != nullptr && floor->checkState(0) == Qt::Checked,
            "queued navigator fixture needs a visible floor");
    click_checkbox(navigator(window), floor);
    // Deliberately do not process the queued itemChanged callback before open.
    require(window.openProject(path), "queued navigator replacement must open");
    const auto selected = window.selectedEntityId();
    process_events();
    require(window.entityVisible(QStringLiteral("floor-1")) && window.selectedEntityId() == selected,
            "queued old checkbox callback modified the replacement project");

    floor = navigator_item(window, QStringLiteral("floor-1"));
    require(floor != nullptr, "replacement floor row is missing");
    navigator(window)->setCurrentItem(floor);
    require(window.openProject(path), "queued selection replacement must open");
    const auto replacement_selection = window.selectedEntityId();
    process_events();
    require(window.selectedEntityId() == replacement_selection,
            "queued old selection callback selected a replacement entity with the same ID");
}

void test_output_refreshes_current_document_head() {
    QTemporaryDir direct_output;
    require(direct_output.isValid(), "direct output freshness fixture needs a temporary directory");

    MainWindow direct;
    direct.resize(900, 620);
    direct.show();
    process_events();
    const auto direct_wall = direct.createStraightWall({0.0, 0.0}, {8.0, 0.0});
    require(!direct_wall.isEmpty(), "direct output freshness wall must be created");
    const auto branch_path = std::filesystem::path(
        direct_output.filePath(QStringLiteral("branch-source.bldproj")).toStdWString());
    (void)sketch::ProjectStore::save(branch_path, direct.document().snapshot());
    auto branch_source = sketch::ProjectStore::load(branch_path);

    auto direct_moved = direct.document().snapshot().entities().at(direct_wall.toStdString());
    direct_moved.properties["baseline"]["start"] = {5.0, 5.0};
    direct_moved.properties["baseline"]["end"] = {13.0, 5.0};
    direct.document().apply(sketch::ApplyEntityChanges{
        .expected_revision = direct.document().revision(),
        .entity_changes = {sketch::EntityChange::upsert(direct_moved)},
        .message = "mutate direct output fixture",
    });

    const auto direct_pdf = direct_output.filePath(QStringLiteral("direct-current-head.pdf"));
    require(direct.exportDraftPdf(direct_pdf) && QFileInfo::exists(direct_pdf) &&
                QFileInfo(direct_pdf).size() > 0,
            "PDF output must synchronously refresh direct document mutations");
    const auto* direct_canvas = canvas(direct, QStringLiteral("measurementPlanCanvas"));
    require(direct_canvas != nullptr, "direct output freshness canvas must exist");
    const auto direct_rendered = std::find_if(
        direct_canvas->entities().begin(), direct_canvas->entities().end(),
        [&](const auto& entity) { return entity.id == direct_wall; });
    require(direct_rendered != direct_canvas->entities().end() && direct_rendered->segments.size() == 1 &&
                direct_rendered->segments.front().start.x == 5.0 &&
                direct_rendered->segments.front().start.y == 5.0 &&
                direct_rendered->segments.front().end.x == 13.0 &&
                direct_rendered->segments.front().end.y == 5.0,
            "PDF output must render the current direct document head");

    // Mutable Document access also permits replacing a head by an alternate
    // branch with exactly the same instance, document ID and revision number.
    const auto old_revision = direct.document().revision();
    direct.document() = std::move(branch_source.document);
    direct_moved.properties["baseline"]["start"] = {20.0, 5.0};
    direct_moved.properties["baseline"]["end"] = {28.0, 5.0};
    direct.document().apply(sketch::ApplyEntityChanges{
        .expected_revision = direct.document().revision(),
        .entity_changes = {sketch::EntityChange::upsert(direct_moved)},
        .message = "same revision alternate output head",
    });
    require(direct.document().revision() == old_revision, "alternate head fixture must reuse revision");
    require(direct.exportDraftPdf(direct_output.filePath(QStringLiteral("alternate-head.pdf"))),
            "alternate same-revision head export failed");
    const auto alternate_rendered = std::find_if(
        direct_canvas->entities().begin(), direct_canvas->entities().end(),
        [&](const auto& entity) { return entity.id == direct_wall; });
    require(alternate_rendered != direct_canvas->entities().end() &&
                alternate_rendered->segments.front().start.x == 20.0,
            "output cache accepted different geometry with identical document identity and revision");

    require(!direct.createDrawingSheet(QStringLiteral("P-201"), QStringLiteral("215.5"),
                                      QStringLiteral("330.2"), QStringLiteral("Print paper")).isEmpty(),
            "print fixture should select a persisted custom portrait sheet");
    require(direct.showPrintPreview(), "valid document should open a draft print preview");
    auto* print_preview = direct.findChild<QPrintPreviewDialog*>();
    require(print_preview != nullptr, "draft preview dialog is missing");
    const auto print_receipt = std::filesystem::temp_directory_path() /
        "vertex-print-preview-receipt.json";
    std::error_code remove_error;
    std::filesystem::remove(print_receipt, remove_error);
    const auto valid_print = direct_output.filePath(QStringLiteral("valid-print.pdf"));
    QPrinter valid_printer(QPrinter::HighResolution);
    valid_printer.setOutputFormat(QPrinter::PdfFormat);
    valid_printer.setOutputFileName(valid_print);
    print_preview->paintRequested(&valid_printer);
    require(std::filesystem::exists(print_receipt),
            "valid print preview should write a local driver receipt");
    QFile receipt_file(QString::fromStdWString(print_receipt.wstring()));
    require(receipt_file.open(QIODevice::ReadOnly | QIODevice::Text),
            "print receipt should be readable");
    const auto receipt_json = nlohmann::json::parse(receipt_file.readAll().toStdString());
    require(receipt_json.at("schema") == "vertex.print-receipt.v1" &&
                receipt_json.at("requested_sheet_mm") == nlohmann::json::array({215.5, 330.2}) &&
                receipt_json.at("driver_paper_mm").size() == 4 &&
                receipt_json.at("verification") == "preview-driver-evidence-only" &&
                receipt_json.at("physical_dpi").size() == 2 &&
                receipt_json.at("output_fingerprint").at("digest_sha256").is_string() &&
                receipt_json.at("output_fingerprint").at("manifest").is_object(),
            "print receipt should bind driver evidence to the rendered output fingerprint");
    const auto requested_paper = valid_printer.pageLayout().pageSize().size(QPageSize::Millimeter);
    require(std::abs(requested_paper.width() - 215.5) < 0.4 &&
                std::abs(requested_paper.height() - 330.2) < 0.4,
            "print preview should request the selected persisted paper dimensions");
    receipt_file.close();

    auto direct_invalid = direct.document().snapshot().entities().at(direct_wall.toStdString());
    direct_invalid.properties.erase("baseline");
    direct.document().apply(sketch::ApplyEntityChanges{
        .expected_revision = direct.document().revision(),
        .entity_changes = {sketch::EntityChange::upsert(direct_invalid)},
        .message = "invalidate direct output fixture",
    });
    const auto blocked_print = direct_output.filePath(QStringLiteral("invalid-print.pdf"));
    QPrinter printer(QPrinter::HighResolution);
    printer.setOutputFormat(QPrinter::PdfFormat);
    printer.setOutputFileName(blocked_print);
    print_preview->paintRequested(&printer);
    require(!QFileInfo::exists(blocked_print) &&
                direct.lastError().contains(QStringLiteral("Printing blocked")),
            "asynchronous print paint accepted geometry invalidated after preview opened");
    print_preview->close();
    require(!direct.showPrintPreview(), "invalid current head opened a new print preview");
    const auto direct_blocked = direct_output.filePath(QStringLiteral("direct-invalid.pdf"));
    require(!direct.exportDraftPdf(direct_blocked) && !QFileInfo::exists(direct_blocked) &&
                direct.lastError().contains(direct_wall),
            "PDF output must block an invalid direct document mutation immediately");

    auto shared_document = std::make_shared<sketch::Document>(sketch::Document::create());
    MainWindow shared(shared_document);
    shared.resize(900, 620);
    shared.show();
    process_events();
    const auto shared_wall = shared.createStraightWall({0.0, 0.0}, {6.0, 0.0});
    require(!shared_wall.isEmpty(), "shared output freshness wall must be created");

    auto shared_moved = shared_document->snapshot().entities().at(shared_wall.toStdString());
    shared_moved.properties["baseline"]["start"] = {-4.0, 2.0};
    shared_moved.properties["baseline"]["end"] = {2.0, 2.0};
    shared_document->apply(sketch::ApplyEntityChanges{
        .expected_revision = shared_document->revision(),
        .entity_changes = {sketch::EntityChange::upsert(shared_moved)},
        .message = "mutate externally shared output fixture",
    });
    const auto shared_pdf = direct_output.filePath(QStringLiteral("shared-current-head.pdf"));
    require(shared.exportDraftPdf(shared_pdf) && QFileInfo::exists(shared_pdf) &&
                QFileInfo(shared_pdf).size() > 0,
            "PDF output must synchronously refresh externally shared mutations");
    const auto* shared_canvas = canvas(shared, QStringLiteral("measurementPlanCanvas"));
    require(shared_canvas != nullptr, "shared output freshness canvas must exist");
    const auto shared_rendered = std::find_if(
        shared_canvas->entities().begin(), shared_canvas->entities().end(),
        [&](const auto& entity) { return entity.id == shared_wall; });
    require(shared_rendered != shared_canvas->entities().end() && shared_rendered->segments.size() == 1 &&
                shared_rendered->segments.front().start.x == -4.0 &&
                shared_rendered->segments.front().start.y == 2.0 &&
                shared_rendered->segments.front().end.x == 2.0 &&
                shared_rendered->segments.front().end.y == 2.0,
            "PDF output must render the current externally shared document head");

    auto shared_invalid = shared_document->snapshot().entities().at(shared_wall.toStdString());
    shared_invalid.properties.erase("baseline");
    shared_document->apply(sketch::ApplyEntityChanges{
        .expected_revision = shared_document->revision(),
        .entity_changes = {sketch::EntityChange::upsert(shared_invalid)},
        .message = "invalidate externally shared output fixture",
    });
    const auto shared_blocked = direct_output.filePath(QStringLiteral("shared-invalid.pdf"));
    require(!shared.exportDraftPdf(shared_blocked) && !QFileInfo::exists(shared_blocked) &&
                shared.lastError().contains(shared_wall),
            "PDF output must block an invalid externally shared mutation immediately");
}

void test_visibility_workflow() {
    MainWindow window;
    window.resize(1366, 768);
    window.show();
    process_events();

    const auto ground_wall = window.createStraightWall({0.0, 0.0}, {8.0, 0.0});
    require(!ground_wall.isEmpty(), "ground wall must be created");
    // Keep this fixture genuinely legacy so the visibility path proves that a
    // later identity promotion does not let auxiliary geometry replace the
    // canonical measurement geometry.
    const auto ground_boundary = QStringLiteral("legacy-ground-boundary");
    const nlohmann::json legacy_segments = {
        {{"start", {0.0, 1.0}}, {"end", {4.0, 1.0}}, {"sweep_radians", 0.0}},
        {{"start", {4.0, 1.0}}, {"end", {4.0, 4.0}}, {"sweep_radians", 0.0}},
        {{"start", {4.0, 4.0}}, {"end", {0.0, 4.0}}, {"sweep_radians", 0.0}},
        {{"start", {0.0, 4.0}}, {"end", {0.0, 1.0}}, {"sweep_radians", 0.0}},
    };
    const Entity legacy_entity{
        ground_boundary.toStdString(), "measurement_boundary",
        {{"floor_id", "floor-1"}, {"layer_id", "layer-1"},
         {"segments", legacy_segments}, {"classification", "measurement"},
         {"factor", 1.0}, {"factor_expression", "1"},
         {"factor_numerator", 1}, {"factor_denominator", 1}},
        false, nlohmann::json::object()};
    window.document().apply(sketch::ApplyEntityChanges{
        .expected_revision = window.document().revision(),
        .entity_changes = {sketch::EntityChange::upsert(legacy_entity)},
        .message = "legacy visibility fixture",
    });
    require(window.selectEntity(ground_boundary), "ground boundary must be created");
    auto* original_total = window.findChild<QLabel*>(QStringLiteral("calculationBuildingTotal"));
    require(original_total != nullptr, "boundary total must be available");
    const auto canonical_total = original_total->text();
    auto identified = sketch::upgrade_legacy_boundary_entity(
        window.document().snapshot().entities().at(ground_boundary.toStdString()));
    window.document().apply(sketch::ApplyEntityChanges{
        .expected_revision = window.document().revision(),
        .entity_changes = {sketch::EntityChange::upsert(identified)},
        .message = "explicit metadata-preserving identity upgrade",
    });
    identified.properties["boundary"] = {
        {{"start", {20.0, 0.0}}, {"end", {24.0, 0.0}}, {"sweep_radians", 0.0}},
        {{"start", {24.0, 0.0}}, {"end", {20.0, 4.0}}, {"sweep_radians", 0.0}},
        {{"start", {20.0, 4.0}}, {"end", {20.0, 0.0}}, {"sweep_radians", 0.0}},
    };
    window.document().apply(sketch::ApplyEntityChanges{
        .expected_revision = window.document().revision(),
        .entity_changes = {sketch::EntityChange::upsert(identified)},
        .message = "attach opaque auxiliary metadata to identified boundary",
    });
    require(window.selectEntity(ground_boundary), "refresh identified boundary");
    const auto& identified_plan = canvas(window, QStringLiteral("measurementPlanCanvas"))->entities();
    const auto identified_entry = std::find_if(identified_plan.begin(), identified_plan.end(),
        [&](const auto& entity) { return entity.id == ground_boundary; });
    require(identified_entry != identified_plan.end() && identified_entry->segments.size() == 4 &&
                identified_entry->segments.front().start.x == 0.0 && original_total->text() == canonical_total,
            "identified boundary auxiliary geometry must not override canonical plan or calculations");

    const auto upper_floor = window.createFloor(QStringLiteral("building-1"),
                                                QStringLiteral("Upper floor"));
    require(!upper_floor.isEmpty(), "second floor must be created");
    const auto upper_layer = window.activeLayerId();
    require(!upper_layer.isEmpty(), "second floor default layer must be active");
    const auto upper_wall = window.createStraightWall({0.0, 8.0}, {8.0, 8.0});
    require(!upper_wall.isEmpty(), "upper wall must be created");
    const auto upper_boundary = window.createBoundary({
        {{0.0, 12.0}, {3.0, 12.0}, 0.0},
        {{3.0, 12.0}, {3.0, 15.0}, 0.0},
        {{3.0, 15.0}, {0.0, 15.0}, 0.0},
        {{0.0, 15.0}, {0.0, 12.0}, 0.0},
    });
    require(!upper_boundary.isEmpty(), "upper floor must have a calculated area to hide");
    require(window.selectEntity(upper_wall), "upper wall must be selectable");
    const auto upper_opening = window.createHostedOpening(
        QStringLiteral("door"), QStringLiteral("1 m"), QStringLiteral("2 m"),
        QStringLiteral("0 m"), QStringLiteral("2 m"));
    require(!upper_opening.isEmpty(), "hosted opening must be created");
    const auto upper_layer_two = window.createLayer(upper_floor, QStringLiteral("Upper overlay"));
    require(!upper_layer_two.isEmpty(), "same-floor second layer must be created");
    const auto upper_overlay_wall = window.createStraightWall({0.0, 10.0}, {8.0, 10.0});
    require(!upper_overlay_wall.isEmpty(), "same-floor overlay wall must be created");
    require(window.setActiveLayer(upper_layer_two), "overlay layer must remain a valid destination");

    const auto before_visibility = window.document().snapshot();
    require(window.selectEntity(ground_boundary), "ground boundary must be selected for totals");
    const auto selected_before = window.selectedEntityId();
    const auto drawing_layer_before = window.activeLayerId();
    const auto ground_layer = drawing_layer_before;
    auto* building_total = window.findChild<QLabel*>(QStringLiteral("calculationBuildingTotal"));
    auto* living_total = window.findChild<QLabel*>(QStringLiteral("calculationLivingTotal"));
    require(building_total != nullptr && living_total != nullptr,
            "calculation totals must be visible in the inspector");
    const auto building_total_before = building_total->text();
    const auto living_total_before = living_total->text();
    require(building_total_before != QStringLiteral("—") &&
                living_total_before != QStringLiteral("—"),
            "fixture must expose calculated totals before filtering");

    auto* measurement = canvas(window, QStringLiteral("measurementPlanCanvas"));
    auto* architectural = canvas(window, QStringLiteral("architecturalPlanCanvas"));
    require(canvas_contains(measurement, ground_wall) && canvas_contains(measurement, upper_wall) &&
                canvas_contains(measurement, upper_overlay_wall),
            "all valid floor and layer geometry must render by default");
    require(measurement->entities().size() == architectural->entities().size(),
            "both plan canvases must start from one entity list");
    const auto upper_wall_entry = std::find_if(
        measurement->entities().begin(), measurement->entities().end(),
        [&](const auto& entity) { return entity.id == upper_wall; });
    require(upper_wall_entry != measurement->entities().end() && upper_wall_entry->segments.size() == 2,
            "hosted opening must split the upper wall in both plans");

    auto* tree = navigator(window);
    auto* upper_floor_item = navigator_item(window, upper_floor);
    auto* upper_layer_item = navigator_item(window, upper_layer);
    auto* upper_layer_two_item = navigator_item(window, upper_layer_two);
    require(upper_floor_item && upper_layer_item && upper_layer_two_item,
            "floor and both same-floor layers must be in the navigator");
    require((upper_floor_item->flags() & Qt::ItemIsUserCheckable) &&
                (upper_layer_item->flags() & Qt::ItemIsUserCheckable) &&
                (upper_layer_two_item->flags() & Qt::ItemIsUserCheckable) &&
                upper_floor_item->checkState(0) == Qt::Checked &&
                upper_layer_item->checkState(0) == Qt::Checked &&
                upper_layer_two_item->checkState(0) == Qt::Checked,
            "floor and layer rows must expose independent checked visibility controls");

    // Keyboard row navigation still selects a container. Space then toggles
    // only its view state and retains that selection.
    auto* preceding = preceding_visible_item(tree, upper_floor_item);
    require(preceding != nullptr, "a visible navigator row must precede the upper floor");
    tree->setCurrentItem(preceding);
    tree->setFocus();
    process_events();
    press_key(tree, Qt::Key_Down);
    process_events();
    require(window.selectedEntityId() == upper_floor,
            "keyboard Down navigation must select the floor row");
    press_space(tree);
    process_events();
    require(!window.entityVisible(upper_floor) && !window.entityVisible(upper_layer) &&
                !window.entityVisible(upper_layer_two) && !window.entityVisible(upper_wall) &&
                !window.entityVisible(upper_opening) && !window.entityVisible(upper_overlay_wall),
            "Space on a floor checkbox must hide its layers and hosted contents");
    require(window.selectedEntityId() == upper_floor,
            "toggling a floor checkbox must retain the selected floor");
    upper_layer_item = navigator_item(window, upper_layer);
    require(upper_layer_item && upper_layer_item->checkState(0) == Qt::Checked,
            "hiding a floor must not cascade into its layer check state");
    auto* visibility_label = window.findChild<QLabel*>(QStringLiteral("visibilitySummary"));
    require(visibility_label == nullptr,
            "visibility state must remain in the hierarchy eye controls without a redundant summary label");
    require(!upper_layer_item->text(0).contains(QStringLiteral("hidden"), Qt::CaseInsensitive) &&
                upper_layer_item->toolTip(0).contains(QStringLiteral("Effective state: hidden by its floor filter.")),
            "a layer row must keep its compact name while explaining inherited visibility on demand");
    require(!canvas_contains(measurement, upper_wall) && !canvas_contains(architectural, upper_wall) &&
                canvas_contains(measurement, ground_wall),
            "the same floor/layer mask must drive both plan canvases");
    window.showAllContainers();
    process_events();
    require(window.entityVisible(upper_floor) && window.entityVisible(upper_wall) &&
                window.entityVisible(upper_overlay_wall), "Show all must clear every container filter");
    require(window.selectedEntityId() == upper_floor,
            "Show all must preserve the selected entity");

    // Totals are contextual to a selected boundary. Keep that context fixed
    // while hiding a different floor that contributes a nonzero area.
    require(window.selectEntity(ground_boundary), "restore the calculation inspector context");
    require(window.setContainerVisible(upper_floor, false), "hide a floor with calculated area");
    require(!canvas_contains(measurement, upper_boundary) &&
                building_total->text() == building_total_before &&
                living_total->text() == living_total_before,
            "hiding a contributing area must leave full-document totals unchanged");
    window.showAllContainers();

    // A mouse click on the actual checkbox preserves an unrelated selected
    // object and its drawing destination while the row itself remains usable.
    require(window.setActiveLayer(upper_layer_two) && window.selectEntity(upper_overlay_wall),
            "restore an explicit destination and selected wall");
    const auto selected_wall_before_click = window.selectedEntityId();
    const auto drawing_layer_before_click = window.activeLayerId();
    upper_floor_item = navigator_item(window, upper_floor);
    require(upper_floor_item != nullptr, "upper floor row must be recreated");
    click_checkbox(tree, upper_floor_item);
    process_events();
    require(!window.entityVisible(upper_floor) && window.selectedEntityId() == selected_wall_before_click &&
                window.activeLayerId() == drawing_layer_before_click,
            "mouse floor checkbox must preserve selected entity and drawing destination");
    auto* drawing_context = window.findChild<QLabel*>(QStringLiteral("drawingContext"));
    require(drawing_context && drawing_context->text().contains(QStringLiteral("Hidden by the view filter")),
            "hidden drawing destination must remain explicitly readable in context");
    require(window.document().snapshot().revision() == before_visibility.revision(),
            "visibility changes must not advance the document revision");
    require(window.document().snapshot().entities() == before_visibility.entities() &&
                window.document().snapshot().history().size() == before_visibility.history().size(),
            "visibility changes must not change semantic entities or history");

    // Toggle one same-floor layer after restoring its parent. Its sibling stays
    // visible, proving floor masking and layer masking are independent.
    require(window.setContainerVisible(upper_floor, true), "upper floor must be shown again");
    require(window.setContainerVisible(upper_layer_two, false), "overlay layer must be hidden");
    require(window.entityVisible(upper_layer) && !window.entityVisible(upper_layer_two) &&
                window.entityVisible(upper_wall) && !window.entityVisible(upper_overlay_wall),
            "one same-floor layer must hide only its own hosted geometry");
    require(canvas_contains(measurement, upper_wall) && !canvas_contains(measurement, upper_overlay_wall) &&
                canvas_contains(architectural, upper_wall) && !canvas_contains(architectural, upper_overlay_wall),
            "layer masking must remain identical across both plans");

    // Keep a hidden floor and an independently hidden layer in the optional
    // capture fixture. The active destination remains readable while hidden.
    require(window.setActiveLayer(upper_layer_two) && window.selectEntity(upper_overlay_wall),
            "capture fixture must retain its active destination");
    require(window.setContainerVisible(upper_floor, false), "capture floor must be hidden");
    require(window.setContainerVisible(upper_layer_two, false), "capture layer must be hidden independently");
    capture_workspaces(window);
    window.showAllContainers();
    process_events();

    // A wall's canonical baseline remains authoritative even if an imported
    // auxiliary boundary describes a conflicting line.
    auto canonical_wall = sketch::Entity::create("wall", {
        {"floor_id", "floor-1"},
        {"layer_id", ground_layer.toStdString()},
        {"baseline", {{"start", {12.0, 12.0}}, {"end", {16.0, 12.0}},
                       {"sweep_radians", 0.0}}},
        {"boundary", {{{"start", {100.0, 100.0}}, {"end", {101.0, 100.0}},
                        {"sweep_radians", 0.0}}}},
        {"thickness_m", 0.14}, {"height_m", 2.4}, {"elevation_m", 0.0},
    });
    canonical_wall.id = "canonical-baseline-wall";
    auto revision = window.document().revision();
    window.document().apply(sketch::ApplyEntityChanges{
        .expected_revision = revision,
        .entity_changes = {sketch::EntityChange::upsert(canonical_wall)},
        .message = "add canonical baseline wall fixture",
    });
    require(window.selectEntity(ground_wall), "refresh after canonical wall fixture");
    const auto canonical_entry = std::find_if(
        measurement->entities().begin(), measurement->entities().end(),
        [](const auto& entity) { return entity.id == QStringLiteral("canonical-baseline-wall"); });
    require(canonical_entry != measurement->entities().end() &&
                canonical_entry->segments.size() == 1 &&
                canonical_entry->segments.front().start.x == 12.0 &&
                canonical_entry->segments.front().start.y == 12.0,
            "plan output must read a wall from its canonical baseline");

    // A malformed canonical slab member remains an output error even when
    // its valid organizational placement is hidden by the floor filter.
    auto malformed_slab = sketch::Entity::create("slab", {
        {"floor_id", upper_floor.toStdString()},
        {"layer_id", upper_layer.toStdString()},
        {"boundary", {{{"start", {0.0, 20.0}}, {"end", {4.0, 20.0}},
                        {"sweep_radians", 0.0}},
                       {{"start", "malformed"}, {"end", {4.0, 24.0}},
                        {"sweep_radians", 0.0}}}},
        {"holes", nlohmann::json::array()},
        {"thickness_m", 0.15}, {"elevation_m", 0.0},
    });
    malformed_slab.id = "malformed-hidden-slab";
    revision = window.document().revision();
    window.document().apply(sketch::ApplyEntityChanges{
        .expected_revision = revision,
        .entity_changes = {sketch::EntityChange::upsert(malformed_slab)},
        .message = "add malformed hidden slab fixture",
    });
    require(window.selectEntity(ground_wall), "refresh after malformed slab fixture");
    require(window.setContainerVisible(upper_floor, false), "hide malformed slab floor");
    QTemporaryDir malformed_output;
    require(malformed_output.isValid(), "malformed slab fixture needs a temporary directory");
    const auto malformed_pdf = malformed_output.filePath(QStringLiteral("malformed-slab.pdf"));
    require(!window.exportDraftPdf(malformed_pdf) && !QFileInfo::exists(malformed_pdf) &&
                window.lastError().contains(QStringLiteral("malformed-hidden-slab")),
            "malformed hidden slab geometry must still block PDF output");
    window.showAllContainers();
    process_events();

    // A malformed object remains part of full validation even when its floor is
    // hidden, so output cannot be made to succeed by changing the view.
    auto invalid = sketch::encode_building_entity(
        RectangularColumn{"", {20.0, 20.0, 0.0}, 0.3, 0.4, 3.0, 0.0});
    invalid.id = "invalid-hidden-column";
    invalid.properties["form"] = "unsupported-form";
    invalid.properties["floor_id"] = upper_floor.toStdString();
    invalid.properties["layer_id"] = upper_layer.toStdString();
    const auto invalid_revision = window.document().revision();
    window.document().apply(sketch::ApplyEntityChanges{
        .expected_revision = invalid_revision,
        .entity_changes = {sketch::EntityChange::upsert(invalid)},
        .message = "add hidden invalid geometry",
    });
    require(window.selectEntity(ground_wall), "refresh after adding invalid geometry");
    require(window.setContainerVisible(upper_floor, false), "hide invalid object's floor");
    QTemporaryDir output_directory;
    require(output_directory.isValid(), "visibility output fixture needs a temporary directory");
    const auto blocked_pdf = output_directory.filePath(QStringLiteral("blocked.pdf"));
    require(!window.exportDraftPdf(blocked_pdf) && !QFileInfo::exists(blocked_pdf) &&
                window.lastError().contains(QStringLiteral("blocked"), Qt::CaseInsensitive),
            "hidden invalid geometry must still block PDF output");

    // Filter state resets only after a successful replacement. A failed open
    // must leave the active hidden state intact.
    const auto project_path = output_directory.filePath(QStringLiteral("visibility.bldproj"));
    require(window.saveProjectAs(project_path), "visibility fixture project must save");
    require(window.openProject(project_path), "same-revision project must reopen");
    require(window.entityVisible(upper_floor) && window.entityVisible(upper_layer_two),
            "successful reopen must reset transient visibility filters");
    require(window.setActiveLayer(upper_layer_two), "reopened layer must be available for filter preservation");
    require(window.setContainerVisible(upper_floor, false) &&
                window.setContainerVisible(upper_layer_two, false),
            "reopened filters must be set before the failed open");
    require(!window.openProject(output_directory.filePath(QStringLiteral("missing.bldproj"))),
            "missing project open must fail");
    require(!window.entityVisible(upper_floor) && !window.entityVisible(upper_layer_two),
            "failed open must preserve the replacement filter state");
}

}  // namespace

int main(int argc, char** argv) {
    sketch::testing::noninteractive_errors();
    QApplication application(argc, argv);
    install_capture_font();
    try {
        test_output_refreshes_current_document_head();
        test_queued_navigator_events_do_not_cross_project_replacement();
        test_visibility_workflow();
        std::cout << "Visibility workflow tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "visibility_workflow_tests: " << error.what() << '\n';
        return 1;
    }
}
