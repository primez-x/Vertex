#include "sketch/annotation_entity_codec.hpp"
#include "sketch/boundary_dimension.hpp"
#include "sketch/boundary_entity.hpp"
#include "sketch/constraint_entity.hpp"
#include "sketch/desktop/main_window.hpp"
#include "sketch/document_wall.hpp"
#include "support/noninteractive_errors.hpp"

#include <QApplication>
#include <QAction>
#include <QDir>
#include <QFontDatabase>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QPdfDocument>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>
#include <QUuid>

#include <algorithm>
#include <cmath>
#include <exception>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

void capture(QWidget& widget, const QString& name) {
    const auto path = qEnvironmentVariable("VERTEX_TEST_CAPTURE_DIR");
    if (path.isEmpty()) return;
    QDir().mkpath(path);
    widget.grab().save(QDir(path).filePath(name + QStringLiteral(".png")));
}

void review_measurement(sketch::desktop::MainWindow& window, bool accept, bool refresh = false) {
    auto* action = window.findChild<QAction*>(refresh ? QStringLiteral("refreshExteriorMeasurement")
                                                    : QStringLiteral("measureExteriorFromWalls"));
    if (!action) throw std::runtime_error("exterior measurement is available as a user command");
    std::exception_ptr failure;
    QTimer::singleShot(0,&window,[&] {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        try {
            if (!dialog || dialog->objectName() != QStringLiteral("wallMeasurementReview"))
                throw std::runtime_error("wall measurement command opens its review dialog");
            auto* area = dialog->findChild<QLabel*>(QStringLiteral("wallMeasurementArea"));
            if (!area || !area->text().contains(QStringLiteral("Exterior faces")) ||
                !area->text().contains(QStringLiteral("4 walls")))
                throw std::runtime_error("review shows exterior basis, wall count and measured area");
            capture(*dialog,QStringLiteral("exterior-measurement-review"));
            if (accept) dialog->accept(); else dialog->reject();
        } catch (...) {
            failure = std::current_exception();
            if (dialog) dialog->reject();
        }
    });
    action->trigger();
    if (failure) std::rethrow_exception(failure);
}

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

QStringList rectangle_walls(sketch::desktop::MainWindow& window) {
    const QStringList ids{
        window.createStraightWall({0.0, 0.0}, {4.0, 0.0}, QStringLiteral("exterior")),
        window.createStraightWall({4.0, 0.0}, {4.0, 3.0}, QStringLiteral("exterior")),
        window.createStraightWall({4.0, 3.0}, {0.0, 3.0}, QStringLiteral("exterior")),
        window.createStraightWall({0.0, 3.0}, {0.0, 0.0}, QStringLiteral("exterior"))};
    require(std::all_of(ids.begin(), ids.end(), [](const QString& id) { return !id.isEmpty(); }),
            "four physical walls create a closed 4 by 3 metre loop");
    return ids;
}

void select_walls(sketch::desktop::MainWindow& window, const QStringList& ids) {
    require(ids.size() == 4, "the wall-measurement fixture has four source walls");
    for (qsizetype i = 0; i < ids.size(); ++i)
        require(window.selectEntity(ids.at(i), i != 0),
                "each source wall selects without replacing the earlier walls");
    require(window.selectedEntityIds().size() == 4,
            "the complete closed wall loop remains selected");
}

std::vector<std::string> wall_ids(const sketch::DocumentSnapshot& snapshot) {
    std::vector<std::string> ids;
    for (const auto& [id, entity] : snapshot.entities())
        if (entity.type == "wall") ids.push_back(id);
    std::sort(ids.begin(), ids.end());
    return ids;
}

std::size_t count_type(const sketch::DocumentSnapshot& snapshot, std::string_view type) {
    return static_cast<std::size_t>(std::count_if(
        snapshot.entities().begin(), snapshot.entities().end(),
        [&](const auto& item) { return item.second.type == type; }));
}

double polygon_area(const sketch::IdentifiedBoundary& boundary) {
    long double twice_area = 0.0L;
    for (const auto& edge : boundary.segments)
        twice_area += static_cast<long double>(edge.segment.start.x) * edge.segment.end.y -
                      static_cast<long double>(edge.segment.end.x) * edge.segment.start.y;
    return std::abs(static_cast<double>(twice_area * 0.5L));
}

QString appraisal_declarations(const char* area_use = "dwelling") {
    return QStringLiteral(
        R"({"appraisal_policy":{"policy_kind":"residential_declared","version":1,"property_kind":"detached_single_family","measurement_basis":"exterior"},"grade":"above","appraisal_facts":{"finish":"finished","access":"direct_interior","ceiling_eligibility":"standard","area_use":"%1","boundary_role":"measured_area"}})")
        .arg(QString::fromLatin1(area_use));
}

void choose_appraisal_workflow(sketch::desktop::MainWindow& window) {
    auto* workflow = window.findChild<QComboBox*>(QStringLiteral("calculationWorkflow"));
    require(workflow != nullptr, "the calculation workflow selector exists");
    const auto index = workflow->findData(QStringLiteral("appraisal"));
    require(index >= 0, "Appraisal is an available calculation workflow");
    workflow->setCurrentIndex(index);
}

std::vector<sketch::Entity> annotation_entities(const sketch::DocumentSnapshot& snapshot) {
    std::vector<sketch::Entity> result;
    for (const auto& [id, entity] : snapshot.entities()) {
        (void)id;
        if (entity.type == sketch::kAnnotationEntityType) result.push_back(entity);
    }
    return result;
}

void set_area_name(sketch::desktop::MainWindow& window, const QString& boundary_id,
                   const QString& name_value) {
    require(window.selectEntity(boundary_id), "select the derived area before naming it");
    auto* name = window.findChild<QLineEdit*>(QStringLiteral("areaName"));
    require(name != nullptr && name->isEnabled(), "the selected area exposes its Name field");
    name->setText(name_value);
    name->setModified(true);
    QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QApplication::sendEvent(name, &enter);
    QApplication::processEvents();
}

void set_area_appearance(sketch::desktop::MainWindow& window, const QString& boundary_id) {
    require(window.selectEntity(boundary_id), "select the derived area before editing appearance");
    auto* button = window.findChild<QPushButton*>(QStringLiteral("areaAppearanceButton"));
    require(button != nullptr && button->isEnabled(), "the selected area exposes its appearance editor");
    std::exception_ptr callback_failure;
    bool applied = false;
    QTimer::singleShot(0, &window, [&] {
        try {
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            require(dialog != nullptr && dialog->objectName() == QStringLiteral("areaAppearanceDialog"),
                    "the native area-appearance dialog opens");
            auto* outline = dialog->findChild<QLineEdit*>(QStringLiteral("areaOutlineColor"));
            auto* fill = dialog->findChild<QLineEdit*>(QStringLiteral("areaFillColor"));
            auto* pattern = dialog->findChild<QComboBox*>(QStringLiteral("areaFillPattern"));
            auto* hatch = dialog->findChild<QDoubleSpinBox*>(QStringLiteral("areaHatchScale"));
            auto* line_width = dialog->findChild<QDoubleSpinBox*>(QStringLiteral("areaLineWidthMm"));
            auto* visible = dialog->findChild<QCheckBox*>(QStringLiteral("areaVisible"));
            auto* buttons = dialog->findChild<QDialogButtonBox*>();
            require(outline && fill && pattern && hatch && line_width && visible && buttons &&
                        buttons->button(QDialogButtonBox::Apply),
                    "the native area-appearance controls are available");
            outline->setText(QStringLiteral("#253545"));
            fill->setText(QStringLiteral("#A4D9B0"));
            pattern->setCurrentIndex(pattern->findData(QStringLiteral("solid")));
            hatch->setValue(2.0);
            line_width->setValue(0.75);
            visible->setChecked(true);
            buttons->button(QDialogButtonBox::Apply)->click();
            applied = true;
        } catch (...) {
            callback_failure = std::current_exception();
            if (auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget())) dialog->reject();
        }
    });
    button->click();
    if (callback_failure) std::rethrow_exception(callback_failure);
    require(applied, "the area appearance change is applied");
}

std::vector<sketch::Entity> dimensions_for(const sketch::DocumentSnapshot& snapshot,
                                          const std::string& boundary_id) {
    std::vector<sketch::Entity> result;
    for (const auto& [id, entity] : snapshot.entities()) {
        (void)id;
        if (!sketch::can_recognize_boundary_dimension_entity_type(entity.type)) continue;
        const auto decoded = sketch::decode_boundary_dimension_entity(entity);
        if (decoded.dimension && decoded.dimension->boundary_id == boundary_id)
            result.push_back(entity);
    }
    return result;
}

void selected_wall_loop_creates_and_refreshes_one_exterior_appraisal_area() {
    using sketch::desktop::MainWindow;

    MainWindow window;
    window.resize(1200, 800);
    window.show();
    QApplication::processEvents();
    window.setMetricUnits(true);
    choose_appraisal_workflow(window);

    const auto walls = rectangle_walls(window);
    select_walls(window, walls);
    const auto source_snapshot = window.document().snapshot();
    const auto source_ids = wall_ids(source_snapshot);
    require(source_ids.size() == 4, "the fixture contains exactly four source walls");
    const auto wall_thickness = source_snapshot.entities().at(source_ids.front())
                                    .properties.at("thickness_m").get<double>();
    require(wall_thickness > 0.0, "source wall thickness is read from persisted geometry");

    const auto stale_revision = source_snapshot.revision() - 1;
    require(window.createMeasurementBoundaryFromSelectedWalls(
                QStringLiteral("measurement"), stale_revision).isEmpty() &&
                window.document().revision() == source_snapshot.revision() &&
                window.document().snapshot().entities() == source_snapshot.entities(),
            "a stale creation revision rejects the command without changing the document");

    review_measurement(window,false);
    require(window.document().snapshot().entities() == source_snapshot.entities() &&
                window.document().revision() == source_snapshot.revision(),
            "canceling the exterior preview keeps the source document unchanged");
    review_measurement(window,true);
    const auto boundary_id = window.selectedEntityId();
    require(!boundary_id.isEmpty(), "a selected closed loop creates its appraisal measurement boundary");
    const auto created = window.document().snapshot();
    require(created.revision() == source_snapshot.revision() + 1,
            "wall measurement creation commits as one document command");
    require(dimensions_for(created,boundary_id.toStdString()).size() == 4,
            "the exterior measurement includes one bound length dimension per perimeter edge");
    require(count_type(created, "measurement_boundary") == 1 &&
                count_type(created, "room_boundary") == 0,
            "wall measurement creates one appraisal boundary without creating a room boundary");
    for (const auto& id : source_ids)
        require(created.entities().at(id) == source_snapshot.entities().at(id),
                "measurement creation leaves every source wall unchanged");

    const auto& derived_entity = created.entities().at(boundary_id.toStdString());
    require(derived_entity.type == "measurement_boundary",
            "the derived geometry retains the measurement-boundary semantic type");
    const auto& source = derived_entity.properties.at("wall_measurement_source");
    require(source.is_object() && source.at("version") == 1 && source.at("basis") == "exterior" &&
                source.at("walls").is_array() && source.at("walls").size() == 4,
            "the derived boundary persists exterior wall-source provenance");
    std::set<std::string> recorded_wall_ids;
    for (const auto& record : source.at("walls"))
        if (record.is_object() && record.contains("id") && record.at("id").is_string())
            recorded_wall_ids.insert(record.at("id").get<std::string>());
    require(recorded_wall_ids == std::set<std::string>(source_ids.begin(), source_ids.end()),
            "wall-source provenance names exactly the four selected wall identities");

    const auto initial_boundary = sketch::decode_identified_boundary_entity(derived_entity);
    const auto initial_area = polygon_area(initial_boundary);
    const auto expected_exterior_area = (4.0 + wall_thickness) * (3.0 + wall_thickness);
    require(std::abs(initial_area - expected_exterior_area) < 1e-8,
            "the analytical geometry measures the 4 by 3 metre rectangle at its exterior faces");

    const auto before_duplicate = window.document().snapshot();
    require(window.selectEntity(walls.front()), "one perimeter wall can identify its unbranched closed loop");
    const auto duplicate_id = window.createMeasurementBoundaryFromSelectedWalls();
    require(duplicate_id == boundary_id &&
                window.document().revision() == before_duplicate.revision() &&
                window.document().snapshot().entities() == before_duplicate.entities(),
            "repeating the same wall-loop command returns the existing boundary without a second revision");

    require(window.selectEntity(boundary_id), "reselect the derived boundary for appraisal declarations");
    require(window.editSelectedAppraisalFacts(appraisal_declarations()),
            "the derived exterior boundary accepts qualified residential facts");

    const auto garage_id = window.createBoundary(
        {{{1.0, 1.0}, {1.2, 1.0}, 0.0}, {{1.2, 1.0}, {1.2, 1.2}, 0.0},
         {{1.2, 1.2}, {1.0, 1.2}, 0.0}, {{1.0, 1.2}, {1.0, 1.0}, 0.0}},
        QStringLiteral("garage"));
    require(!garage_id.isEmpty() && window.selectEntity(garage_id),
            "an interior garage area can be selected as a valid deduction fixture");
    require(window.applySelectedAutoSubtract(boundary_id),
            "the derived area accepts an ordinary contained appraisal deduction");
    require(window.editSelectedAppraisalFacts(appraisal_declarations("garage")),
            "the deduction retains explicit garage facts");

    require(window.selectEntity(boundary_id), "reselect the derived boundary for appraisal declarations");
    auto* qualification = window.findChild<QLabel*>(QStringLiteral("appraisalQualification"));
    auto* gla = window.findChild<QLabel*>(QStringLiteral("appraisalGlaTotal"));
    require(qualification && qualification->text().startsWith(QStringLiteral("Qualified")) && gla,
            "the declared exterior area qualifies for the appraisal report");
    const auto garage_area = polygon_area(sketch::decode_identified_boundary_entity(
        window.document().snapshot().entities().at(garage_id.toStdString())));
    const auto net_area = initial_area - garage_area;
    require(gla->text().contains(QString::number(net_area, 'f', 2)) &&
                gla->text().contains(QStringLiteral("m²")),
            "the appraisal label shows exterior area net of the retained garage deduction in metric units");
    window.setMetricUnits(false);
    require(gla->text().contains(QString::number(net_area / (0.3048*0.3048),'f',2)) &&
                gla->text().contains(QStringLiteral("ft²")),
            "the same wall-derived net appraisal area calculates automatically in square feet");
    window.setMetricUnits(true);

    set_area_name(window, boundary_id, QStringLiteral("Exterior wall area"));
    require(window.editSelectedAreaAttributes(QStringLiteral(R"({"finish_note":"retained"})")),
            "an area attribute can coexist with derived geometry");
    set_area_appearance(window, boundary_id);
    const auto dimension_id = window.createAreaDimension(boundary_id, {0.0, 0.0});
    require(!dimension_id.isEmpty(), "the derived area accepts a semantic area dimension");

    const auto prepared = window.document().snapshot();
    const auto prepared_boundary = prepared.entities().at(boundary_id.toStdString());
    require(prepared_boundary.properties.at("name") == "Exterior wall area" &&
                prepared_boundary.properties.at("appraisal_facts").at("area_use") == "dwelling" &&
                prepared_boundary.properties.at("area_attributes").at("finish_note") == "retained" &&
                prepared_boundary.properties.at("deduction_ids") ==
                    std::vector<std::string>{garage_id.toStdString()},
            "the derived area stores its name, declared facts, appearance attributes and deduction before refresh");
    const auto appearance_before = annotation_entities(prepared);
    require(!appearance_before.empty(), "area appearance is retained in a project annotation provider");
    const auto dimensions_before = dimensions_for(prepared, boundary_id.toStdString());
    require(!dimensions_before.empty() &&
                std::any_of(dimensions_before.begin(), dimensions_before.end(), [&](const auto& entity) {
                    return entity.id == dimension_id.toStdString();
                }),
            "the area dimension is persisted alongside any dimensions created with the derived boundary");

    // A thickness-only edit changes the source geometry while preserving every
    // baseline. The derived boundary must be reported stale until explicitly refreshed.
    auto edited_wall = prepared.entities().at(source_ids.front());
    const auto old_thickness = edited_wall.properties.at("thickness_m").get<double>();
    edited_wall.properties["thickness_m"] = old_thickness + 0.1;
    window.document().apply(sketch::ApplyEntityChanges{
        prepared.revision(), {sketch::EntityChange::upsert(edited_wall)}, {},
        "test wall thickness edit"});
    const auto changed_source = window.document().snapshot();
    require(changed_source.entities().at(source_ids.front()).properties.at("baseline") ==
                prepared.entities().at(source_ids.front()).properties.at("baseline"),
            "the stale-source fixture changes only physical wall thickness");
    require(window.selectEntity(boundary_id), "the stale derived boundary remains selectable");
    require(qualification->text().contains(QStringLiteral("Unqualified")) &&
                qualification->text().contains(QStringLiteral("stale"), Qt::CaseInsensitive),
            "the appraisal report withholds a wall measurement whose source thickness changed");

    const auto stale_refresh = changed_source.revision() - 1;
    require(!window.refreshSelectedWallMeasurement(stale_refresh) &&
                window.document().revision() == changed_source.revision() &&
                window.document().snapshot().entities() == changed_source.entities(),
            "a stale refresh revision fails without partially updating the derived area");
    review_measurement(window,true,true);
    require(window.document().revision() == changed_source.revision()+1,
            "the refresh review applies the current wall measurement as one command");
    const auto refreshed = window.document().snapshot();
    require(refreshed.revision() == changed_source.revision() + 1 &&
                count_type(refreshed, "measurement_boundary") == 2 &&
                count_type(refreshed, "room_boundary") == 0,
            "refresh updates the existing derived measurement in one command");
    const auto& refreshed_entity = refreshed.entities().at(boundary_id.toStdString());
    require(refreshed_entity.type == "measurement_boundary" &&
                refreshed_entity.properties.at("name") == "Exterior wall area" &&
                refreshed_entity.properties.at("appraisal_facts").at("area_use") == "dwelling" &&
                refreshed_entity.properties.at("area_attributes").at("finish_note") == "retained" &&
                refreshed_entity.properties.at("deduction_ids") ==
                    std::vector<std::string>{garage_id.toStdString()} &&
                refreshed_entity.properties.at("wall_measurement_source") == source,
            "refresh preserves boundary identity, source IDs, name, facts, area attributes and deduction links");
    require(annotation_entities(refreshed) == appearance_before,
            "refresh preserves the existing area appearance provider exactly");
    const auto refreshed_boundary = sketch::decode_identified_boundary_entity(refreshed_entity);
    const auto refreshed_area = polygon_area(refreshed_boundary);
    require(std::abs(refreshed_area - initial_area) > 0.01,
            "refresh rebuilds exterior geometry from the edited wall thickness");
    require(qualification->text().startsWith(QStringLiteral("Qualified")) &&
                gla->text().contains(QString::number(refreshed_area - garage_area, 'f', 2)),
            "refresh restores qualified net appraisal totals from the current exterior geometry");
    capture(window,QStringLiteral("refreshed-exterior-appraisal-area"));
    const auto dimensions_after = dimensions_for(refreshed, boundary_id.toStdString());
    std::set<std::string> before_dimension_ids;
    std::set<std::string> after_dimension_ids;
    for (const auto& entity : dimensions_before) before_dimension_ids.insert(entity.id);
    for (const auto& entity : dimensions_after) after_dimension_ids.insert(entity.id);
    require(before_dimension_ids == after_dimension_ids &&
                after_dimension_ids.contains(dimension_id.toStdString()),
            "refresh retains every original dimension entity and identity");
    for (const auto& entity : dimensions_after) {
        const auto decoded = sketch::decode_boundary_dimension_entity(entity);
        require(decoded.dimension.has_value(), "each retained boundary dimension remains decodable");
        const auto resolution = sketch::resolve_boundary_dimension(*decoded.dimension, refreshed_entity);
        if (decoded.dimension->kind == sketch::BoundaryDimensionKind::area)
            require(std::abs(resolution.area_square_metres - refreshed_area) < 1e-8,
                    "the retained area dimension resolves against refreshed geometry");
        else if (decoded.dimension->kind == sketch::BoundaryDimensionKind::segment_length)
            require(std::isfinite(resolution.segment_length_metres) && resolution.segment_length_metres > 0.0,
                    "the retained length dimension resolves against refreshed geometry");
        else
            require(std::isfinite(resolution.angle_radians),
                    "the retained angle dimension resolves against refreshed geometry");
    }

    require(window.undoCommand() &&
                window.document().snapshot().entities() == changed_source.entities(),
            "undo restores the exact stale measurement and source-thickness edit state");
    require(window.redoCommand() &&
                window.document().snapshot().entities() == refreshed.entities(),
            "redo restores the complete refreshed measurement state");

    QTemporaryDir output;
    require(output.isValid(), "save/reopen verification has a temporary project directory");
    const auto path = output.filePath(QStringLiteral("wall-measurement.bldproj"));
    require(window.saveProjectAs(path) && window.openProject(path) &&
                window.document().snapshot().entities() == refreshed.entities(),
            "wall-source provenance and refreshed area metadata survive project save and reopen");
    require(window.selectEntity(boundary_id) &&
                window.findChild<QLabel*>(QStringLiteral("appraisalQualification"))->text()
                    .startsWith(QStringLiteral("Qualified")),
            "the reopened boundary recalculates as qualified from persisted appraisal facts");
    const auto pdf_path = output.filePath(QStringLiteral("exterior-measurement.pdf"));
    require(window.exportDraftPdf(pdf_path), "the wall-derived appraisal plan exports as a PDF");
    QPdfDocument pdf;
    require(pdf.load(pdf_path) == QPdfDocument::Error::None && pdf.pageCount() > 0,
            "the exterior measurement PDF opens as a real plan page");
    const auto text = pdf.getAllText(0).text();
    require(text.contains(QString::number(refreshed_area - garage_area,'f',2)),
            "the PDF retains the current net appraisal area");
    require(std::any_of(refreshed_boundary.segments.begin(),refreshed_boundary.segments.end(),
        [&](const auto& edge) {
            const auto length = std::hypot(edge.segment.end.x-edge.segment.start.x,
                                           edge.segment.end.y-edge.segment.start.y);
            return text.contains(QString::number(length,'f',3));
        }),
            "the PDF includes a physical exterior edge measurement rather than only wall baseline lengths");
}

void wall_measurement_admission_failures_are_atomic() {
    using sketch::desktop::MainWindow;

    MainWindow open_loop;
    const auto open_ids = rectangle_walls(open_loop);
    auto open_source = open_loop.document().snapshot();
    auto broken_wall = open_source.entities().at(open_ids.back().toStdString());
    broken_wall.properties["baseline"]["start"][0] = -0.25;
    bool locked_corner_rejected = false;
    try {
        open_loop.document().apply(sketch::ApplyEntityChanges{
            open_source.revision(), {sketch::EntityChange::upsert(broken_wall)}, {},
            "test breaking a connected wall corner"});
    } catch (const std::exception&) { locked_corner_rejected = true; }
    require(locked_corner_rejected && open_loop.document().snapshot().entities() == open_source.entities(),
            "a raw wall edit cannot silently break its persisted corner connection");
    std::vector<sketch::EntityChange> open_changes{sketch::EntityChange::upsert(broken_wall)};
    for (const auto& [id, entity] : open_source.entities()) {
        if (entity.type != "constraint") continue;
        const auto decoded = sketch::decode_constraint_entity(entity);
        if (decoded.supported() && std::any_of(decoded.constraint->bindings.begin(), decoded.constraint->bindings.end(),
            [&](const auto& binding) { return binding.owner_id == broken_wall.id; }))
            open_changes.push_back(sketch::EntityChange::erase(id));
    }
    open_loop.document().apply(sketch::ApplyEntityChanges{
        open_source.revision(), std::move(open_changes), {},
        "test explicitly disconnecting an open wall loop"});
    select_walls(open_loop, open_ids);
    const auto open_before_attempt = open_loop.document().snapshot();
    require(open_loop.createMeasurementBoundaryFromSelectedWalls().isEmpty() &&
                open_loop.document().revision() == open_before_attempt.revision() &&
                open_loop.document().snapshot().entities() == open_before_attempt.entities(),
            "an open selected wall chain is rejected without creating or changing an area");

    MainWindow read_only;
    const auto read_only_ids = rectangle_walls(read_only);
    select_walls(read_only, read_only_ids);
    read_only.document().mark_read_only("wall measurement test fixture");
    const auto read_only_before = read_only.document().snapshot();
    require(read_only.createMeasurementBoundaryFromSelectedWalls().isEmpty() &&
                read_only.document().revision() == read_only_before.revision() &&
                read_only.document().snapshot().entities() == read_only_before.entities(),
            "a read-only document refuses wall measurement creation without mutation");
}

}  // namespace

int main(int argc, char** argv) {
    sketch::testing::noninteractive_errors();
    QStandardPaths::setTestModeEnabled(true);
    QApplication app(argc, argv);
    const auto font_id = QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/Inter.ttf"));
    if (font_id >= 0)
        QApplication::setFont(QFont(QFontDatabase::applicationFontFamilies(font_id).front(),10));
    QCoreApplication::setApplicationName(
        QStringLiteral("Vertex-wall-measurement-test-") +
        QUuid::createUuid().toString(QUuid::WithoutBraces));
    try {
        selected_wall_loop_creates_and_refreshes_one_exterior_appraisal_area();
        wall_measurement_admission_failures_are_atomic();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
