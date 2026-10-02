#include "sketch/annotation_entity_codec.hpp"
#include "sketch/appraisal_document.hpp"
#include "sketch/boundary_dimension.hpp"
#include "sketch/boundary_entity.hpp"
#include "sketch/constraint_entity.hpp"
#include "sketch/desktop/main_window.hpp"
#include "sketch/document_wall.hpp"
#include "sketch/document_digest.hpp"
#include "sketch/geometry.hpp"
#include "sketch/vertical_levels.hpp"
#include "sketch/wall_measurement.hpp"
#include "../src/desktop/plan_canvas.hpp"
#include "support/noninteractive_errors.hpp"

#include <QApplication>
#include <QAction>
#include <QDir>
#include <QFontDatabase>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QDoubleSpinBox>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPushButton>
#include <QPdfDocument>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTableWidget>
#include <QTimer>
#include <QUuid>

#include <algorithm>
#include <cmath>
#include <exception>
#include <iostream>
#include <numbers>
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

void review_measurement(sketch::desktop::MainWindow& window, bool accept, bool refresh = false,
                        std::size_t included = 4, std::size_t excluded = 0,
                        QString expected_area = {}, QString expected_perimeter = {},
                        QString capture_name = QStringLiteral("exterior-measurement-review"),
                        bool require_analytic_arc = false) {
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
                !area->text().contains(QStringLiteral("%1 walls").arg(included)))
                throw std::runtime_error("review shows exterior basis, wall count and measured area");
            if (!expected_area.isEmpty() && !area->text().contains(expected_area))
                throw std::runtime_error("review shows the exact analytical exterior area");
            if (!expected_perimeter.isEmpty()) {
                auto* perimeter = dialog->findChild<QLabel*>(QStringLiteral("wallMeasurementPerimeter"));
                if (!perimeter || !perimeter->text().contains(expected_perimeter))
                    throw std::runtime_error("review shows the exact analytical exterior perimeter");
            }
            auto* exclusions = dialog->findChild<QLabel*>(QStringLiteral("wallMeasurementExclusions"));
            auto* preview = dynamic_cast<sketch::desktop::PlanCanvas*>(
                dialog->findChild<QWidget*>(QStringLiteral("wallMeasurementPreview")));
            if (!exclusions || !exclusions->text().contains(QStringLiteral("%1 excluded").arg(excluded)) ||
                !preview || preview->entities().size() != included + excluded + 1)
                throw std::runtime_error("review discloses excluded candidates and previews all candidate baselines");
            if (excluded != 0) {
                std::size_t excluded_shapes = 0;
                for (const auto& shape : preview->entities())
                    if (shape.stroke_color == QColor(190,115,35)) ++excluded_shapes;
                if (excluded_shapes != excluded)
                    throw std::runtime_error("excluded partition baselines are visually distinct in the review");
            }
            if (require_analytic_arc) {
                const auto curved_shapes = std::count_if(
                    preview->entities().begin(), preview->entities().end(), [](const auto& shape) {
                        return std::any_of(shape.segments.begin(), shape.segments.end(), [](const auto& edge) {
                            return std::abs(edge.sweep_radians) > 1e-9;
                        });
                    });
                if (curved_shapes < 2)
                    throw std::runtime_error("review previews the source and exterior as analytical arcs");
            }
            capture(*dialog,capture_name);
            if (accept) dialog->accept(); else dialog->reject();
        } catch (...) {
            failure = std::current_exception();
            if (dialog) dialog->reject();
        }
    });
    action->trigger();
    QApplication::processEvents();
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

sketch::Boundary analytical_boundary(const sketch::Entity& entity) {
    const auto identified = sketch::decode_identified_boundary_entity(entity);
    sketch::Boundary result;
    result.reserve(identified.segments.size());
    for (const auto& edge : identified.segments) result.push_back(edge.segment);
    return result;
}

double analytical_area(const sketch::Entity& entity) {
    return std::abs(sketch::signed_area(analytical_boundary(entity)));
}

double analytical_perimeter(const sketch::Entity& entity) {
    double result = 0.0;
    for (const auto& edge : analytical_boundary(entity)) result += sketch::segment_length(edge);
    return result;
}

std::size_t curved_edge_count(const sketch::Entity& entity) {
    const auto boundary = analytical_boundary(entity);
    return static_cast<std::size_t>(std::count_if(boundary.begin(), boundary.end(), [](const auto& edge) {
        return std::abs(edge.sweep_radians) > 1e-9;
    }));
}

QString metric_area_text(double area) {
    return QStringLiteral("%1 m²").arg(area, 0, 'f', 2);
}

QString metric_perimeter_text(double perimeter) {
    return QStringLiteral("%1 m").arg(perimeter, 0, 'f', 3);
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

void require_analytic_wall_dimensions(const sketch::DocumentSnapshot& snapshot,
                                      const sketch::Entity& boundary_entity,
                                      double expected_perimeter,
                                      double expected_arc_length) {
    const auto identified = sketch::decode_identified_boundary_entity(boundary_entity);
    const auto dimensions = dimensions_for(snapshot, boundary_entity.id);
    require(dimensions.size() == identified.segments.size(),
            "the derived exterior retains one physical dimension for every analytical edge");
    double measured_perimeter = 0.0;
    bool found_arc_dimension = false;
    for (const auto& entity : dimensions) {
        const auto decoded = sketch::decode_boundary_dimension_entity(entity);
        require(decoded.dimension &&
                    decoded.dimension->kind == sketch::BoundaryDimensionKind::segment_length,
                "each wall-derived perimeter dimension remains a stable physical edge measurement");
        const auto edge = std::find_if(identified.segments.begin(), identified.segments.end(),
            [&](const auto& candidate) {
                return candidate.segment_id == decoded.dimension->segment_id;
            });
        require(edge != identified.segments.end(),
                "every physical perimeter dimension references an existing identified edge");
        const auto resolution = decoded.dimension->resolve(boundary_entity);
        measured_perimeter += resolution.segment_length_metres;
        if (std::abs(edge->segment.sweep_radians) > 1e-9) {
            found_arc_dimension = true;
            require(std::abs(resolution.segment_length_metres - expected_arc_length) < 1e-8,
                    "the curved exterior dimension measures the exact arc length, not its chord");
        }
    }
    require(found_arc_dimension && std::abs(measured_perimeter - expected_perimeter) < 1e-8,
            "the saved edge dimensions sum to the exact analytical exterior perimeter");
}

void curved_d_exterior_measurement_stays_analytic_through_refresh_and_output() {
    using sketch::desktop::MainWindow;

    MainWindow window;
    window.resize(1200, 800);
    window.show();
    QApplication::processEvents();
    window.setMetricUnits(true);
    choose_appraisal_workflow(window);

    const auto curved_wall = window.createCurvedWall(
        {-2.0, 0.0}, {2.0, 0.0}, QStringLiteral("180 deg"), QStringLiteral("exterior"));
    const QStringList walls{
        curved_wall,
        window.createStraightWall({2.0, 0.0}, {2.0, 3.0}, QStringLiteral("exterior")),
        window.createStraightWall({2.0, 3.0}, {-2.0, 3.0}, QStringLiteral("exterior")),
        window.createStraightWall({-2.0, 3.0}, {-2.0, 0.0}, QStringLiteral("exterior"))};
    require(std::all_of(walls.begin(), walls.end(), [](const auto& id) { return !id.isEmpty(); }),
            "the D-shaped physical shell has one semicircular and three straight walls");
    const auto source_before = window.document().snapshot();
    const auto source_ids = wall_ids(source_before);
    require(source_ids.size() == 4, "the curved fixture contains exactly four physical source walls");
    const auto& arc_source = source_before.entities().at(curved_wall.toStdString());
    const auto original_sweep = arc_source.properties.at("baseline").at("sweep_radians").get<double>();
    require(std::abs(original_sweep - std::numbers::pi) < 1e-12,
            "the source wall stores one analytical semicircle rather than faceted chords");
    const auto initial_thickness = arc_source.properties.at("thickness_m").get<double>();
    for (const auto& id : source_ids)
        require(std::abs(source_before.entities().at(id).properties.at("thickness_m").get<double>() -
                         initial_thickness) < 1e-12,
                "every shell wall begins with the same physical thickness");

    const auto initial_offset = initial_thickness / 2.0;
    const auto initial_area = (4.0 + 2.0 * initial_offset) * (3.0 + initial_offset) +
        (std::numbers::pi / 2.0) * std::pow(2.0 + initial_offset, 2.0);
    const auto initial_perimeter = 2.0 * (3.0 + initial_offset) +
        (4.0 + 2.0 * initial_offset) + std::numbers::pi * (2.0 + initial_offset);
    select_walls(window, walls);
    const auto selected_source = window.document().snapshot();
    review_measurement(window, false, false, 4, 0, metric_area_text(initial_area),
                       metric_perimeter_text(initial_perimeter),
                       QStringLiteral("curved-exterior-create-review"), true);
    require(window.document().snapshot().entities() == selected_source.entities(),
            "canceling analytical curved-wall review leaves the complete source document unchanged");
    review_measurement(window, true, false, 4, 0, metric_area_text(initial_area),
                       metric_perimeter_text(initial_perimeter),
                       QStringLiteral("curved-exterior-create-review-accepted"), true);

    const auto boundary_id = window.selectedEntityId();
    require(!boundary_id.isEmpty(), "accepting the real Tools review creates a D-shaped exterior measurement");
    const auto created = window.document().snapshot();
    require(created.revision() == selected_source.revision() + 1,
            "curved exterior creation commits as one atomic document command");
    for (const auto& id : source_ids)
        require(created.entities().at(id) == source_before.entities().at(id),
                "curved measurement creation leaves all authoritative source walls byte-for-byte unchanged");
    const auto& measured = created.entities().at(boundary_id.toStdString());
    const auto identified = sketch::decode_identified_boundary_entity(measured);
    require(measured.type == "measurement_boundary" && identified.segments.size() == 4 &&
                curved_edge_count(measured) == 1,
            "the persisted D-shaped outline retains one true semicircle and three exact lines");
    const auto& provenance = measured.properties.at("wall_measurement_source");
    std::set<std::string> recorded_ids;
    bool valid_contexts = true;
    for (const auto& record : provenance.at("walls")) {
        const auto id = record.at("id").get<std::string>();
        recorded_ids.insert(id);
        const auto& source_wall = source_before.entities().at(id);
        const auto& context = record.at("context");
        valid_contexts = valid_contexts && context.is_object() &&
            context.value("floor_id", std::string{}) ==
                source_wall.properties.at("floor_id").get<std::string>() &&
            context.value("layer_id", std::string{}) ==
                source_wall.properties.at("layer_id").get<std::string>();
    }
    require(provenance.at("version") == 1 && provenance.at("basis") == "exterior" &&
                recorded_ids == std::set<std::string>(source_ids.begin(), source_ids.end()) &&
                valid_contexts &&
                std::abs(created.entities().at(curved_wall.toStdString())
                             .properties.at("baseline").at("sweep_radians").get<double>() -
                         original_sweep) < 1e-12 &&
                sketch::wall_measurement_source_current(created, measured),
            "the source receipt retains the curved wall identity and validates against its analytical baseline");
    require(std::abs(analytical_area(measured) - initial_area) < 1e-8 &&
                std::abs(analytical_perimeter(measured) - initial_perimeter) < 1e-8,
            "the generated exterior has the exact semicircle-plus-rectangle area and perimeter");
    const auto initial_arc_length = std::numbers::pi * (2.0 + initial_offset);
    require_analytic_wall_dimensions(created, measured, initial_perimeter, initial_arc_length);

    require(window.selectEntity(boundary_id) &&
                window.editSelectedAppraisalFacts(appraisal_declarations()),
            "the curved wall-derived measurement accepts declared appraisal facts");
    auto* qualification = window.findChild<QLabel*>(QStringLiteral("appraisalQualification"));
    auto* gla = window.findChild<QLabel*>(QStringLiteral("appraisalGlaTotal"));
    require(qualification && qualification->text().startsWith(QStringLiteral("Qualified")) &&
                gla && gla->text().contains(metric_area_text(initial_area)),
            "qualified appraisal totals use the exact analytical curved exterior area");
    const auto prepared = window.document().snapshot();

    const auto changed_thickness = initial_thickness + 0.1;
    std::vector<sketch::EntityChange> thickness_changes;
    for (const auto& id : source_ids) {
        auto wall = prepared.entities().at(id);
        wall.properties["thickness_m"] = changed_thickness;
        thickness_changes.push_back(sketch::EntityChange::upsert(std::move(wall)));
    }
    window.document().apply(sketch::ApplyEntityChanges{
        prepared.revision(), std::move(thickness_changes), {},
        "test changing curved exterior source thickness"});
    const auto changed_source = window.document().snapshot();
    for (const auto& id : source_ids)
        require(changed_source.entities().at(id).properties.at("baseline") ==
                    prepared.entities().at(id).properties.at("baseline"),
                "thickness refresh fixture changes physical thickness without changing source curves");
    require(!sketch::wall_measurement_source_current(
                changed_source, changed_source.entities().at(boundary_id.toStdString())),
            "a changed source thickness makes the exact curved outline stale");
    require(window.selectEntity(boundary_id), "the stale curved measurement remains selectable");
    require(qualification->text().contains(QStringLiteral("Unqualified")) &&
                qualification->text().contains(QStringLiteral("stale"), Qt::CaseInsensitive) &&
                gla->text() == QStringLiteral("—"),
            "a stale curved outline withholds qualified appraisal totals instead of reporting old area");

    const auto refreshed_offset = changed_thickness / 2.0;
    const auto refreshed_area = (4.0 + 2.0 * refreshed_offset) * (3.0 + refreshed_offset) +
        (std::numbers::pi / 2.0) * std::pow(2.0 + refreshed_offset, 2.0);
    const auto refreshed_perimeter = 2.0 * (3.0 + refreshed_offset) +
        (4.0 + 2.0 * refreshed_offset) + std::numbers::pi * (2.0 + refreshed_offset);
    review_measurement(window, false, true, 4, 0, metric_area_text(refreshed_area),
                       metric_perimeter_text(refreshed_perimeter),
                       QStringLiteral("curved-exterior-refresh-review"), true);
    require(window.document().snapshot().entities() == changed_source.entities(),
            "canceling curved-wall refresh review leaves the stale source and outline untouched");
    review_measurement(window, true, true, 4, 0, metric_area_text(refreshed_area),
                       metric_perimeter_text(refreshed_perimeter),
                       QStringLiteral("curved-exterior-refresh-review-accepted"), true);
    const auto refreshed = window.document().snapshot();
    require(refreshed.revision() == changed_source.revision() + 1,
            "refreshing the curved exterior commits as one document command");
    for (const auto& id : source_ids)
        require(refreshed.entities().at(id).properties.at("baseline") ==
                    changed_source.entities().at(id).properties.at("baseline") &&
                    refreshed.entities().at(id).properties.at("thickness_m") == changed_thickness,
                "refresh changes only the derived outline and retains the edited analytical wall sources");
    const auto& refreshed_entity = refreshed.entities().at(boundary_id.toStdString());
    require(curved_edge_count(refreshed_entity) == 1 &&
                std::abs(analytical_area(refreshed_entity) - refreshed_area) < 1e-8 &&
                std::abs(analytical_perimeter(refreshed_entity) - refreshed_perimeter) < 1e-8 &&
                sketch::wall_measurement_source_current(refreshed, refreshed_entity),
            "refresh restores the exact new physical area and perimeter while keeping the true arc and current receipt");
    const auto refreshed_arc_length = std::numbers::pi * (2.0 + refreshed_offset);
    require_analytic_wall_dimensions(refreshed, refreshed_entity,
                                     refreshed_perimeter, refreshed_arc_length);
    require(qualification->text().startsWith(QStringLiteral("Qualified")) &&
                gla->text().contains(metric_area_text(refreshed_area)),
            "refresh restores qualified appraisal totals from the changed curved-wall thickness");
    capture(window, QStringLiteral("curved-exterior-refreshed-area"));

    const auto before_dimensions = dimensions_for(prepared, boundary_id.toStdString());
    const auto after_dimensions = dimensions_for(refreshed, boundary_id.toStdString());
    std::set<std::string> before_ids;
    std::set<std::string> after_ids;
    for (const auto& entity : before_dimensions) before_ids.insert(entity.id);
    for (const auto& entity : after_dimensions) after_ids.insert(entity.id);
    require(before_ids == after_ids,
            "refresh preserves the identity of every analytical physical perimeter dimension");
    require(window.undoCommand() &&
                window.document().snapshot().entities() == changed_source.entities() &&
                window.redoCommand() && window.document().snapshot().entities() == refreshed.entities(),
            "curved measurement refresh undoes and redoes the complete stale/current outline transaction");

    QTemporaryDir output;
    require(output.isValid(), "curved measurement output verification has a temporary directory");
    const auto project_path = output.filePath(QStringLiteral("curved-wall-measurement.bldproj"));
    require(window.saveProjectAs(project_path) && window.openProject(project_path) &&
                window.document().snapshot().entities() == refreshed.entities(),
            "curved source provenance and exact refreshed outline survive native save/reopen");
    const auto reopened = window.document().snapshot();
    const auto& reopened_boundary = reopened.entities().at(boundary_id.toStdString());
    require(curved_edge_count(reopened_boundary) == 1 &&
                std::abs(analytical_area(reopened_boundary) - refreshed_area) < 1e-8 &&
                std::abs(analytical_perimeter(reopened_boundary) - refreshed_perimeter) < 1e-8 &&
                sketch::wall_measurement_source_current(reopened, reopened_boundary),
            "native reopen restores exact analytical arc area, perimeter and live source provenance");
    require(window.selectEntity(boundary_id) &&
                window.findChild<QLabel*>(QStringLiteral("appraisalQualification"))->text()
                    .startsWith(QStringLiteral("Qualified")),
            "the reopened curved outline recalculates as qualified from its saved appraisal facts");

    const auto pdf_path = output.filePath(QStringLiteral("curved-exterior-measurement.pdf"));
    require(window.exportDraftPdf(pdf_path), "the refreshed curved exterior exports to its draft plan PDF");
    QPdfDocument pdf;
    require(pdf.load(pdf_path) == QPdfDocument::Error::None && pdf.pageCount() > 0,
            "the curved exterior PDF opens as a real plan page");
    const auto text = pdf.getAllText(0).text();
    require(text.contains(QString::number(refreshed_area, 'f', 2)) &&
                text.contains(QString::number(refreshed_arc_length, 'f', 3)),
            "the PDF prints exact analytical exterior area and curved-edge physical dimension");
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

void an_unsplit_partition_selects_its_exterior_shell() {
    sketch::desktop::MainWindow window;
    window.setMetricUnits(true);
    choose_appraisal_workflow(window);
    const auto perimeter = rectangle_walls(window);
    const auto partition = window.createStraightWall({2.0,0.0},{2.0,3.0},QStringLiteral("partition"));
    require(!partition.isEmpty() && window.selectEntity(partition), "an unsplit interior partition is selectable");
    const auto before = window.document().snapshot();
    require(count_type(before,"wall") == 5, "T contacts retain the four original unsplit perimeter walls");
    review_measurement(window,false,false,4,1);
    require(window.document().revision() == before.revision() &&
                window.document().snapshot().entities() == before.entities(),
            "canceling partition-shell review preserves every source and revision");
    review_measurement(window,true,false,4,1);
    const auto owner_id = window.selectedEntityId();
    const auto created = window.document().snapshot();
    require(count_type(created,"measurement_boundary") == 1, "partition selection creates one exterior owner");
    const auto& owner = created.entities().at(owner_id.toStdString());
    std::set<std::string> provenance;
    for (const auto& record : owner.properties.at("wall_measurement_source").at("walls"))
        provenance.insert(record.at("id").get<std::string>());
    std::set<std::string> expected;
    for (const auto& id : perimeter) expected.insert(id.toStdString());
    require(provenance == expected && !provenance.contains(partition.toStdString()),
            "saved provenance contains only full exterior perimeter walls");
    for (const auto& id : wall_ids(before))
        require(created.entities().at(id) == before.entities().at(id), "recognition leaves all wall geometry unchanged");
    const auto thickness = before.entities().at(perimeter.front().toStdString()).properties.at("thickness_m").get<double>();
    const double exterior_area = (4.0+thickness)*(3.0+thickness);
    require(std::abs(polygon_area(sketch::decode_identified_boundary_entity(owner))-exterior_area) < 1e-8,
            "the partition does not subtract from the exterior footprint");
    require(dimensions_for(created,owner.id).size() == 4, "exterior dimensions follow the four shell edges");
    require(window.editSelectedAppraisalFacts(appraisal_declarations()), "the shell accepts residential declarations");
    window.setMetricUnits(false);
    auto* gla = window.findChild<QLabel*>(QStringLiteral("appraisalGlaTotal"));
    require(gla && gla->text().contains(QString::number(exterior_area/(0.3048*0.3048),'f',2)) &&
                gla->text().contains(QStringLiteral("ft²")), "appraisal square-foot totals use the full exterior footprint");
    for (qsizetype i = 0; i < perimeter.size(); ++i)
        require(window.selectEntity(perimeter[i],i != 0), "select explicit shell candidates");
    require(window.selectEntity(partition,true), "include the partition in explicit candidates");
    const auto before_duplicate = window.document().snapshot();
    require(window.createMeasurementBoundaryFromSelectedWalls() == owner_id &&
                window.document().revision() == before_duplicate.revision(),
            "explicit shell and partition candidates reuse the same owner without double counting");
    // Newly connected walls must not be imported into a retained version-1 refresh source.
    const auto addition = window.createStraightWall({4.0,1.5},{5.0,1.5},QStringLiteral("new branch"));
    require(!addition.isEmpty() && window.selectEntity(owner_id), "a later branch does not change the retained owner selection");
    const auto before_refresh = window.document().snapshot();
    review_measurement(window,true,true,4,0);
    require(window.document().snapshot().entities() == before_refresh.entities() &&
                window.document().revision() == before_refresh.revision(),
            "refresh uses only retained perimeter provenance and ignores newly connected geometry");
    QTemporaryDir output;
    require(output.isValid(), "partition shell persistence has a temporary output directory");
    const auto path = output.filePath(QStringLiteral("partition-shell.bldproj"));
    require(window.saveProjectAs(path) && window.openProject(path) &&
                window.document().snapshot().entities() == before_refresh.entities(),
            "recognized shell and excluded partition survive save and reopen");
}

void split_perimeter_with_t_branches_and_internal_chord() {
    sketch::desktop::MainWindow window;
    const QStringList perimeter{
        window.createStraightWall({0,0},{2,0}), window.createStraightWall({2,0},{4,0}),
        window.createStraightWall({4,0},{4,3}), window.createStraightWall({4,3},{2,3}),
        window.createStraightWall({2,3},{0,3}), window.createStraightWall({0,3},{0,0})};
    const auto chord = window.createStraightWall({2,0},{2,3},QStringLiteral("partition"));
    const auto branch = window.createStraightWall({2,1.5},{3,1.5},QStringLiteral("partition"));
    const auto crossing = window.createStraightWall({1,1},{3,2},QStringLiteral("partition"));
    require(!chord.isEmpty() && !branch.isEmpty() && !crossing.isEmpty() && window.selectEntity(crossing),
            "a proper crossing of an internal chord can identify the split shell and its T branch");
    const auto before = window.document().snapshot();
    review_measurement(window,true,false,6,3);
    const auto created = window.document().snapshot();
    const auto& owner = created.entities().at(window.selectedEntityId().toStdString());
    const auto& records = owner.properties.at("wall_measurement_source").at("walls");
    require(records.size() == 6, "split shell source retains all six full perimeter IDs");
    for (const auto& record : records)
        require(record.at("id") != chord.toStdString() && record.at("id") != branch.toStdString() &&
                    record.at("id") != crossing.toStdString(),
                "T branches, proper crossings and internal chords are excluded from split-shell provenance");
    for (const auto& id : wall_ids(before))
        require(created.entities().at(id) == before.entities().at(id), "split-shell recognition does not alter walls");
    const auto thickness = before.entities().at(perimeter.front().toStdString()).properties.at("thickness_m").get<double>();
    require(std::abs(polygon_area(sketch::decode_identified_boundary_entity(owner))-(4+thickness)*(3+thickness)) < 1e-8,
            "split-shell measurement includes the complete exterior area");
}

void automatic_candidates_respect_placement_and_phase() {
    sketch::desktop::MainWindow window;
    const auto perimeter = rectangle_walls(window);
    const auto initial = window.document().snapshot();
    const auto seed = initial.entities().at(perimeter.front().toStdString());
    const auto floor = QString::fromStdString(seed.properties.at("floor_id").get<std::string>());
    const auto other_layer = window.createLayer(floor,QStringLiteral("Other drawing layer"));
    require(!other_layer.isEmpty(), "a separate layer exists for candidate isolation");
    const auto snapshot = window.document().snapshot();
    std::vector<sketch::EntityChange> changes;
    for (const auto& suffix : {"elevation","phase","layer"}) {
        auto other = seed;
        other.id = std::string("wall-other-") + suffix;
        other.properties["baseline"] = {{"start",{0.0,0.0}},{"end",{-1.0,0.0}},{"sweep_radians",0.0}};
        if (std::string_view(suffix) == "elevation") other.properties["elevation_m"] = 3.0;
        if (std::string_view(suffix) == "phase") other.properties["phase_id"] = "other-design-phase";
        if (std::string_view(suffix) == "layer") other.properties["layer_id"] = other_layer.toStdString();
        changes.push_back(sketch::EntityChange::upsert(std::move(other)));
    }
    window.document().apply(sketch::ApplyEntityChanges{snapshot.revision(),std::move(changes),{},"test other placement candidates"});
    require(window.selectEntity(perimeter.front()), "select one shell wall with coincident foreign candidates");
    review_measurement(window,true,false,4,0);
    const auto owner_id = window.selectedEntityId();
    const auto before = window.document().snapshot();
    require(before.entities().at(owner_id.toStdString()).properties.at("wall_measurement_source").at("walls").size() == 4,
            "automatic discovery excludes coincident walls in another layer, phase or elevation");
    for (qsizetype i = 0; i < perimeter.size(); ++i)
        require(window.selectEntity(perimeter[i],i != 0), "select explicit shell candidates for mixed elevation refusal");
    require(window.selectEntity(QStringLiteral("wall-other-elevation"),true), "an explicit foreign-elevation wall is selectable");
    require(window.createMeasurementBoundaryFromSelectedWalls().isEmpty() &&
                window.document().revision() == before.revision() &&
                window.document().snapshot().entities() == before.entities(),
            "mixed-elevation explicit candidates fail without reusing or changing the owner");
}

void level_bound_walls_share_a_physical_plane_despite_arithmetic_roundoff() {
    sketch::desktop::MainWindow window;
    const auto perimeter = rectangle_walls(window);
    const auto initial = window.document().snapshot();
    const auto graph = sketch::VerticalLevelGraph({{"ground",0.0},{"upper",3.0}},
                                                 {{"storey","ground","upper"}});
    sketch::Entity levels{"wall-measurement-levels","vertical_levels",
        {{"model",nlohmann::json::parse(graph.serialize())}}};
    auto floor = initial.entities().at(initial.entities().at(perimeter.front().toStdString())
                                         .properties.at("floor_id").get<std::string>());
    floor.properties["vertical_level_binding"] = {
        {"version",1},{"graph_id",levels.id},{"level_id","upper"}};
    std::vector<sketch::EntityChange> changes{
        sketch::EntityChange::upsert(levels),sketch::EntityChange::upsert(floor)};
    for (qsizetype i = 0; i < perimeter.size(); ++i) {
        auto wall = initial.entities().at(perimeter[i].toStdString());
        wall.properties["elevation_m"] = i == 0 ? 0.1 : 0.2;
        wall.properties["vertical_placement"] = {
            {"version",1},{"mode","level"},{"offset_m",i == 0 ? -2.8 : -2.9}};
        changes.push_back(sketch::EntityChange::upsert(std::move(wall)));
    }
    window.document().apply(sketch::ApplyEntityChanges{
        initial.revision(),std::move(changes),{},"test mathematically coplanar level placements"});
    const auto before = window.document().snapshot();
    require(window.selectEntity(perimeter.front()), "one level-bound wall selects the physical shell");
    review_measurement(window,false,false,4,0);
    require(window.document().revision() == before.revision() &&
                window.document().snapshot().entities() == before.entities(),
            "coplanar level preview discovers every wall without rewriting local elevations or offsets");
    select_walls(window,perimeter);
    const auto owner_id = window.createMeasurementBoundaryFromSelectedWalls();
    require(!owner_id.isEmpty(), "explicit selection accepts equal physical planes computed through different level arithmetic");
    const auto created = window.document().snapshot();
    require(created.entities().at(owner_id.toStdString()).properties.at("wall_measurement_source").at("walls").size() == 4,
            "the equal-plane exterior source retains all four perimeter identities");
    for (const auto& [id, entity] : before.entities())
        require(created.entities().at(id) == entity, "physical plane comparison preserves every source property and level entity");
    const auto thickness = before.entities().at(perimeter.front().toStdString()).properties.at("thickness_m").get<double>();
    require(std::abs(polygon_area(sketch::decode_identified_boundary_entity(created.entities().at(owner_id.toStdString()))) -
                         (4.0+thickness)*(3.0+thickness)) < 1e-8,
            "tolerating level roundoff preserves the complete physical exterior area");
    require(window.selectEntity(perimeter.front()), "repeat the automatic command from the original plane seed");
    require(window.createMeasurementBoundaryFromSelectedWalls() == owner_id &&
                window.document().revision() == created.revision(),
            "single-wall discovery reuses the coplanar source owner without double counting");
}

void replaced_wall_sources_repair_existing_appraisal_area(bool metric, bool curved) {
    using sketch::desktop::MainWindow;
    MainWindow window;
    window.setAttribute(Qt::WA_DontShowOnScreen);
    window.resize(1200,800);
    window.show();
    QApplication::processEvents();
    window.setMetricUnits(metric);
    choose_appraisal_workflow(window);
    auto* repair = window.findChild<QAction*>("replaceExteriorMeasurementSources");
    require(repair,"existing exterior measurements must expose Replace source walls as a repair command");
    QStringList walls;
    if (curved) {
        walls = {window.createCurvedWall({0,0},{4,0},"90 deg","exterior"),
            window.createStraightWall({4,0},{4,3},"exterior"),
            window.createStraightWall({4,3},{0,3},"exterior"),
            window.createStraightWall({0,3},{0,0},"exterior")};
    } else walls = rectangle_walls(window);
    select_walls(window,walls);
    const auto area = window.createMeasurementBoundaryFromSelectedWalls();
    require(!area.isEmpty() && window.selectEntity(area) &&
                window.editSelectedAppraisalFacts(appraisal_declarations()),
        "repair fixture starts with a qualified exterior measurement");
    const auto garage = window.createBoundary({{{1,1},{1.2,1},0},{{1.2,1},{1.2,1.2},0},
        {{1.2,1.2},{1,1.2},0},{{1,1.2},{1,1},0}},"garage");
    require(!garage.isEmpty() && window.selectEntity(garage) && window.applySelectedAutoSubtract(area) &&
                window.editSelectedAppraisalFacts(appraisal_declarations("garage")),
        "repair fixture retains a declared garage deduction");
    set_area_name(window,area,"Repaired exterior");
    require(window.editSelectedAreaAttributes(R"({"finish_note":"retain me"})"),
        "repair fixture retains area attributes");
    set_area_appearance(window,area);
    const auto prepared = window.document().snapshot();
    auto* qualified = window.findChild<QLabel*>("appraisalQualification");
    require(qualified && qualified->text().startsWith("Qualified"),"prepared exterior is qualified");
    require(window.selectEntity(walls.front()) && window.deleteSelection(),
        "ordinary deletion removes a perimeter source wall");
    const auto replacement = curved ? window.createCurvedWall({0,0},{4,0},"90 deg","exterior")
        : window.createStraightWall({0,0},{4,0},"exterior");
    const auto isolated = window.createStraightWall({10,10},{11,10},"exterior");
    require(!replacement.isEmpty() && !isolated.isEmpty() && replacement!=walls.front() && window.selectEntity(area),
        "redrawing the same perimeter produces a new wall identity");
    const auto before = window.document().snapshot();
    require(!window.refreshSelectedWallMeasurement() && window.document().snapshot().entities()==before.entities() &&
                qualified->text().startsWith("Unqualified"),
        "ordinary Refresh refuses missing retained IDs without silently discovering new walls");
    const auto review = [&](bool accept, bool stale_units=false, bool stale_selection=false) {
        require(window.selectEntity(area),"select the stale exterior area for source repair");
        std::exception_ptr failure;
        bool opened=false;
        QTimer::singleShot(0,&window,[&] {
            auto* dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget());
            try {
                require(dialog && dialog->objectName()=="wallMeasurementSourceReview", "repair opens its source-choice review");
                opened=true;
                auto* seed=dialog->findChild<QComboBox*>("wallMeasurementSourceSeed");
                auto* buttons=dialog->findChild<QDialogButtonBox*>("wallMeasurementSourceButtons");
                auto* summary=dialog->findChild<QLabel*>("wallMeasurementSourceChanges");
                auto* status=dialog->findChild<QLabel*>("wallMeasurementSourceStatus");
                require(seed && buttons && summary && status && seed->findData(replacement)>=0,
                    "repair lets the user choose a current shell wall");
                require(seed->findData(isolated)>=0,"the repair chooser includes the isolated wall in this layer");
                seed->setCurrentIndex(seed->findData(isolated));
                QApplication::processEvents();
                require(!buttons->button(QDialogButtonBox::Apply)->isEnabled() && !status->text().isEmpty() &&
                            window.document().snapshot().entities()==before.entities(),
                    "an open shell refuses Apply without changing the owner or sources");
                seed->setCurrentIndex(seed->findData(replacement));
                QApplication::processEvents();
                require(buttons->button(QDialogButtonBox::Apply)->isEnabled() &&
                            summary->text().contains("1 added") && summary->text().contains("1 removed"),
                    "repair previews and discloses the exact source replacement");
                require(window.document().snapshot().entities()==before.entities(),"repair review is detached");
                const auto capture_name=QStringLiteral("%1-source-repair-%2")
                    .arg(curved ? QStringLiteral("curved") : QStringLiteral("straight"))
                    .arg(metric ? QStringLiteral("metric") : QStringLiteral("imperial")).toUtf8();
                capture(*dialog,capture_name.constData());
                if (stale_units) {
                    window.setMetricUnits(!metric);
                    buttons->button(QDialogButtonBox::Apply)->click();
                    require(window.document().snapshot().entities()==before.entities() && !status->text().isEmpty(),
                        "unit-stale repair is refused without publishing the captured proposal");
                    dialog->reject();
                } else if (stale_selection) {
                    require(window.selectEntity(replacement),"change the selection while a repair proposal is open");
                    buttons->button(QDialogButtonBox::Apply)->click();
                    require(window.document().snapshot().entities()==before.entities() && !status->text().isEmpty(),
                        "selection-stale repair is refused without publishing the captured proposal");
                    dialog->reject();
                } else buttons->button(accept ? QDialogButtonBox::Apply : QDialogButtonBox::Cancel)->click();
            } catch (...) { failure=std::current_exception(); if (dialog) dialog->reject(); }
        });
        repair->trigger();
        if (failure) std::rethrow_exception(failure);
        require(opened,"repair command opens its actual modal");
    };
    review(false);
    require(window.document().snapshot().entities()==before.entities() && window.document().revision()==before.revision(),
        "Cancel retains the stale owner and project history");
    review(true,true);
    window.setMetricUnits(metric);
    review(true,false,true);
    review(true);
    const auto after=window.document().snapshot();
    const auto& owner=after.entities().at(area.toStdString());
    require(after.revision()==before.revision()+1 && count_type(after,"measurement_boundary")==2 &&
                sketch::wall_measurement_source_current(after,owner) && qualified->text().startsWith("Qualified"),
        "repair commits one edit to the retained exterior owner and restores appraisal qualification");
    for (const auto* key : {"name","appraisal_facts","area_attributes","deduction_ids","factor"})
        require(owner.properties.at(key)==prepared.entities().at(area.toStdString()).properties.at(key),
            "repair must preserve area metadata, exact factor and deduction links");
    require(annotation_entities(after)==annotation_entities(before) &&
                std::abs(analytical_area(owner)-analytical_area(prepared.entities().at(area.toStdString())))<1e-8 &&
                curved_edge_count(owner)==(curved ? 1U : 0U),
        "equivalent source repair retains styling and the original analytical quantities");
    const auto old_dimensions=dimensions_for(before,area.toStdString());
    const auto new_dimensions=dimensions_for(after,area.toStdString());
    require(old_dimensions.size()==new_dimensions.size(),"equivalent source repair retains every bound dimension");
    for (const auto& dimension : old_dimensions)
        require(after.entities().contains(dimension.id),"equivalent source repair preserves dimension identities");
    require(window.undoCommand() && window.document().snapshot().entities()==before.entities() &&
                window.redoCommand() && window.document().snapshot().entities()==after.entities(),
        "source repair undoes and redoes geometry and provenance together");
    QTemporaryDir directory;
    MainWindow reopened;
    require(directory.isValid() && window.saveProjectAs(directory.filePath("source-repair.bldproj")) &&
                reopened.openProject(directory.filePath("source-repair.bldproj")) &&
                reopened.document().snapshot().entities()==after.entities(),
        "repaired appraisal facts, relationships, geometry and sources survive save/reopen");
}

void split_source_replacement_reviews_manual_references() {
    sketch::desktop::MainWindow window;
    window.setMetricUnits(true);
    const auto walls=rectangle_walls(window);
    select_walls(window,walls);
    const auto area=window.createMeasurementBoundaryFromSelectedWalls();
    require(!area.isEmpty(),"split source repair starts with a measured exterior");
    const auto original=sketch::decode_identified_boundary_entity(
        window.document().snapshot().entities().at(area.toStdString()));
    const auto manual=window.createLengthDimension(area,
        QString::fromStdString(original.segments.front().segment_id),{2,-1});
    require(!manual.isEmpty() && window.selectEntity(walls.front()) && window.deleteSelection(),
        "split fixture retains a manual dimension and replaces one source wall");
    const auto first=window.createStraightWall({0,0},{2,0});
    const auto second=window.createStraightWall({2,0},{4,0});
    require(!first.isEmpty() && !second.isEmpty() && window.selectEntity(area),"split replacement walls exist");
    const auto before=window.document().snapshot();
    auto* repair=window.findChild<QAction*>("replaceExteriorMeasurementSources");
    require(repair,"split repair has a source replacement action");
    for (const bool accept : {false,true}) {
        std::exception_ptr failure;
        bool references_opened=false;
        QTimer::singleShot(0,&window,[&] {
            auto* dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget());
            try {
                require(dialog && dialog->objectName()=="wallMeasurementSourceReview","split replacement opens source review");
                auto* seed=dialog->findChild<QComboBox*>("wallMeasurementSourceSeed");
                auto* buttons=dialog->findChild<QDialogButtonBox*>("wallMeasurementSourceButtons");
                require(seed && buttons && seed->findData(first)>=0,"split repair offers the replacement shell");
                seed->setCurrentIndex(seed->findData(first));
                require(buttons->button(QDialogButtonBox::Apply)->isEnabled(),"split topology can proceed to explicit reference review");
                QTimer::singleShot(0,&window,[&] {
                    auto* review=qobject_cast<QDialog*>(QApplication::activeModalWidget());
                    try {
                        require(review && review->objectName()=="boundaryReferenceReview","Apply reviews changed topology references");
                        references_opened=true;
                        auto* choices=review->findChild<QTableWidget*>("boundaryReferenceChoices");
                        auto* mappings=review->findChild<QTableWidget*>("boundaryReferenceMappings");
                        auto* reference_buttons=review->findChild<QDialogButtonBox*>("boundaryReferenceButtons");
                        require(choices && mappings && reference_buttons && choices->rowCount()==1 && mappings->rowCount()==1,
                            "only the attached manual length dimension needs an explicit replacement edge");
                        require(!reference_buttons->button(QDialogButtonBox::Apply)->isEnabled(),"unresolved references cannot apply");
                        if (!accept) { review->reject(); return; }
                        auto* decision=qobject_cast<QComboBox*>(choices->cellWidget(0,1));
                        auto* target=qobject_cast<QComboBox*>(mappings->cellWidget(0,1));
                        require(decision && target && target->count()>1,"reference controls have actual choices");
                        decision->setCurrentIndex(1); // Explicitly keep and map.
                        target->setCurrentIndex(1);   // Explicitly choose displayed E1.
                        require(reference_buttons->button(QDialogButtonBox::Apply)->isEnabled(),"reviewed reference mapping validates");
                        require(window.document().snapshot().entities()==before.entities(),"reference preview is detached");
                        capture(*review,"split-source-reference-review");
                        reference_buttons->button(QDialogButtonBox::Apply)->click();
                    } catch (...) { failure=std::current_exception(); if (review) review->reject(); }
                });
                buttons->button(QDialogButtonBox::Apply)->click();
                if (!accept || failure) dialog->reject();
            } catch (...) { failure=std::current_exception(); if (dialog) dialog->reject(); }
        });
        repair->trigger();
        if (failure) std::rethrow_exception(failure);
        require(references_opened,"changed topology uses the real reference planner");
        if (!accept) require(window.document().snapshot().entities()==before.entities() && window.document().revision()==before.revision(),
            "Cancel in reference review preserves the original measurement, source IDs and manual dimension");
    }
    const auto after=window.document().snapshot();
    const auto& owner=after.entities().at(area.toStdString());
    const auto edited=sketch::decode_identified_boundary_entity(owner);
    const auto dimension=sketch::decode_boundary_dimension_entity(after.entities().at(manual.toStdString()));
    require(after.revision()==before.revision()+1 && edited.segments.size()==5 &&
                sketch::wall_measurement_source_current(after,owner) && dimension.supported() &&
                dimension.dimension->segment_id==edited.segments.front().segment_id,
        "split replacement commits the new source and explicit manual edge mapping once");
    require(std::abs(analytical_area(owner)-analytical_area(before.entities().at(area.toStdString())))<1e-8 &&
                window.undoCommand() && window.document().snapshot().entities()==before.entities() &&
                window.redoCommand() && window.document().snapshot().entities()==after.entities(),
        "a split shell retains exact area and undoes/redoes geometry, sources and references atomically");
}

void changed_same_count_sources_review_manual_references(bool metric, bool curved) {
    sketch::desktop::MainWindow window;
    window.setAttribute(Qt::WA_DontShowOnScreen);
    window.resize(1200,800);
    window.show();
    window.setMetricUnits(metric);
    choose_appraisal_workflow(window);
    const auto make_walls = [&](double width) {
        return QStringList{curved ? window.createCurvedWall({0,0},{width,0},"90 deg","exterior")
                                  : window.createStraightWall({0,0},{width,0},"exterior"),
            window.createStraightWall({width,0},{width,3},"exterior"),
            window.createStraightWall({width,3},{0,3},"exterior"),
            window.createStraightWall({0,3},{0,0},"exterior")};
    };
    const auto walls=make_walls(4);
    select_walls(window,walls);
    const auto area=window.createMeasurementBoundaryFromSelectedWalls();
    require(!area.isEmpty() && window.selectEntity(area) && window.editSelectedAppraisalFacts(appraisal_declarations()),
        "changed shell begins with qualified appraisal facts");
    const auto garage=window.createBoundary({{{1,1},{1.2,1},0},{{1.2,1},{1.2,1.2},0},
        {{1.2,1.2},{1,1.2},0},{{1,1.2},{1,1},0}},"garage");
    require(!garage.isEmpty() && window.selectEntity(garage) && window.applySelectedAutoSubtract(area) &&
                window.editSelectedAppraisalFacts(appraisal_declarations("garage")),"changed shell retains a garage deduction");
    set_area_name(window,area,"Expanded exterior");
    set_area_appearance(window,area);
    const auto original=sketch::decode_identified_boundary_entity(window.document().snapshot().entities().at(area.toStdString()));
    const auto manual=window.createLengthDimension(area,QString::fromStdString(original.segments.front().segment_id),{2,-1});
    require(!manual.isEmpty(),"changed shell retains a manually positioned dimension");
    select_walls(window,walls);
    require(window.deleteSelection(),"replace the original physical shell without deleting its measurement");
    const auto replacements=make_walls(5);
    require(std::none_of(replacements.begin(),replacements.end(),[](const auto& id){return id.isEmpty();}) &&
                window.selectEntity(area),"expanded shell has the same wall count and fresh wall IDs");
    const auto before=window.document().snapshot();
    auto* repair=window.findChild<QAction*>("replaceExteriorMeasurementSources");
    require(repair,"changed shell has a source repair action");
    for (const int step : {0,1,2}) {
        const bool accept=step==2;
        const bool stale_units=step==1;
        std::exception_ptr failure;
        bool references_opened=false;
        QTimer::singleShot(0,&window,[&] {
            auto* dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget());
            try {
                require(dialog && dialog->objectName()=="wallMeasurementSourceReview","changed shell opens source review");
                auto* seed=dialog->findChild<QComboBox*>("wallMeasurementSourceSeed");
                auto* buttons=dialog->findChild<QDialogButtonBox*>("wallMeasurementSourceButtons");
                require(seed && buttons && seed->findData(replacements.front())>=0,"changed shell is available for review");
                seed->setCurrentIndex(seed->findData(replacements.front()));
                require(buttons->button(QDialogButtonBox::Apply)->isEnabled(),
                    "changed geometry with equal edge count must proceed to explicit dimension mapping");
                QApplication::processEvents();
                capture(*dialog,QStringLiteral("same-count-source-%1-%2").arg(metric ? "metric" : "imperial").arg(curved ? "curved" : "straight"));
                QTimer::singleShot(0,&window,[&] {
                    auto* review=qobject_cast<QDialog*>(QApplication::activeModalWidget());
                    try {
                        require(review && review->objectName()=="boundaryReferenceReview","equal count does not silently infer references");
                        references_opened=true;
                        auto* choices=review->findChild<QTableWidget*>("boundaryReferenceChoices");
                        auto* mappings=review->findChild<QTableWidget*>("boundaryReferenceMappings");
                        auto* reference_buttons=review->findChild<QDialogButtonBox*>("boundaryReferenceButtons");
                        require(choices && mappings && reference_buttons && choices->rowCount()==1 && mappings->rowCount()==1 &&
                            !reference_buttons->button(QDialogButtonBox::Apply)->isEnabled(),"unreviewed manual reference blocks Apply");
                        if (!accept && !stale_units) { review->reject(); return; }
                        auto* decision=qobject_cast<QComboBox*>(choices->cellWidget(0,1));
                        auto* target=qobject_cast<QComboBox*>(mappings->cellWidget(0,1));
                        require(decision && target && target->count()>1,"mapping offers actual replacement edges");
                        decision->setCurrentIndex(1);
                        target->setCurrentIndex(1);
                        require(reference_buttons->button(QDialogButtonBox::Apply)->isEnabled() &&
                            window.document().snapshot().entities()==before.entities(),"mapped preview remains detached");
                        capture(*review,QStringLiteral("same-count-reference-%1-%2").arg(metric ? "metric" : "imperial").arg(curved ? "curved" : "straight"));
                        if (stale_units) window.setMetricUnits(!metric);
                        reference_buttons->button(QDialogButtonBox::Apply)->click();
                        if (stale_units && review->isVisible()) review->reject();
                    } catch (...) { failure=std::current_exception(); if (review) review->reject(); }
                });
                buttons->button(QDialogButtonBox::Apply)->click();
                if (!accept || failure) dialog->reject();
            } catch (...) { failure=std::current_exception(); if (dialog) dialog->reject(); }
        });
        repair->trigger();
        if (failure) std::rethrow_exception(failure);
        require(references_opened,"equal-count changed geometry uses explicit reference review");
        if (!accept) require(window.document().snapshot().entities()==before.entities() &&
            window.document().revision()==before.revision(),"Cancel preserves facts, dimensions, geometry and history");
        if (stale_units) window.setMetricUnits(metric);
    }
    const auto after=window.document().snapshot();
    const auto& owner=after.entities().at(area.toStdString());
    const auto edited=sketch::decode_identified_boundary_entity(owner);
    const auto dimension=sketch::decode_boundary_dimension_entity(after.entities().at(manual.toStdString()));
    require(after.revision()==before.revision()+1 && edited.segments.size()==original.segments.size() &&
        sketch::wall_measurement_source_current(after,owner) && count_type(after,"measurement_boundary")==2 &&
        analytical_area(owner)>analytical_area(before.entities().at(area.toStdString())) && curved_edge_count(owner)==(curved ? 1U : 0U),
        "changed shell updates one retained owner with its exact new analytical geometry and sources");
    for (const auto* key : {"name","appraisal_facts","deduction_ids","factor"})
        require(owner.properties.at(key)==before.entities().at(area.toStdString()).properties.at(key),"repair retains appraisal facts and deductions");
    for (const auto& edge : edited.segments) for (const auto& old : original.segments)
        require(edge.segment_id!=old.segment_id && edge.start_vertex_id!=old.start_vertex_id && edge.end_vertex_id!=old.end_vertex_id,
            "changed geometry receives fresh children rather than guessed positional identities");
    require(dimension.supported() && dimension.dimension->segment_id==edited.segments.front().segment_id &&
        after.entities().at(manual.toStdString()).properties.at("text_position")==before.entities().at(manual.toStdString()).properties.at("text_position"),
        "explicit mapping retains the manual dimension's presentation");
    const auto old_dimensions=dimensions_for(before,area.toStdString());
    const auto new_dimensions=dimensions_for(after,area.toStdString());
    require(old_dimensions.size()==new_dimensions.size(),"changed shell regenerates all automatic dimensions");
    for (const auto& old : old_dimensions)
        if (old.id!=manual.toStdString()) require(!after.entities().contains(old.id),"retired automatic dimension IDs are never reused");
    auto* qualified=window.findChild<QLabel*>("appraisalQualification");
    require(qualified && qualified->text().startsWith("Qualified") && window.undoCommand() &&
        window.document().snapshot().entities()==before.entities() && window.redoCommand() &&
        window.document().snapshot().entities()==after.entities(),"qualified source repair undoes and redoes atomically");
    QTemporaryDir directory;
    sketch::desktop::MainWindow reopened;
    require(directory.isValid() && window.saveProjectAs(directory.filePath("changed-shell.bldproj")) &&
        reopened.openProject(directory.filePath("changed-shell.bldproj")) && reopened.document().snapshot().entities()==after.entities(),
        "same-count source repair preserves reviewed references through native reopen");
    require(window.selectEntity(area),"select repaired owner for clone");
    if (!window.transformSelectedBoundary("0",false,false,"10 m","0 m",true))
        throw std::runtime_error("Repaired outline clone refused: "+window.lastError().toStdString());
    const auto cloned=window.document().snapshot();
    sketch::desktop::MainWindow clone_reopened;
    require(window.saveProjectAs(directory.filePath("changed-shell-clone.bldproj")) &&
        clone_reopened.openProject(directory.filePath("changed-shell-clone.bldproj")) &&
        clone_reopened.document().snapshot().entities()==cloned.entities(),"fresh topology proof survives clone and native reopen");
    require(window.undoCommand() && window.document().snapshot().entities()==after.entities(),"cloned topology history undoes without affecting its source");
    require(window.selectEntity(area),"select repaired owner for group copy");
    for (const auto& wall : replacements) require(window.selectEntity(wall,true),"include each current source in group copy");
    require(window.copySelection() && window.pasteSelection(),"repaired owner and source walls paste as a linked group");
    const auto pasted=window.document().snapshot();
    std::optional<sketch::Entity> pasted_owner;
    for (const auto& [id,value] : pasted.entities())
        if (!after.entities().contains(id) && value.type=="measurement_boundary" &&
            value.properties.contains("wall_measurement_source")) pasted_owner=value;
    require(pasted_owner && sketch::wall_measurement_source_current(pasted,*pasted_owner),
        "pasted fresh-topology source proof agrees with its remapped physical walls");
}

void appraisal_area_clone_retains_its_calculation_dependencies(bool metric, bool sourced, bool commercial) {
    sketch::desktop::MainWindow window;
    window.setAttribute(Qt::WA_DontShowOnScreen);
    window.resize(1200,800);
    window.show();
    QApplication::processEvents();
    window.setMetricUnits(metric);
    choose_appraisal_workflow(window);
    const auto declarations=[&](bool deduction) {
        if (!commercial) return appraisal_declarations(deduction ? "garage" : "dwelling");
        return QStringLiteral(R"({"appraisal_policy":{"policy_kind":"light_commercial_declared","version":1,"property_kind":"light_commercial","measurement_basis":"exterior"},"grade":"above","appraisal_facts":{"finish":"finished","access":"direct_interior","ceiling_eligibility":"standard","area_use":"%1","boundary_role":"measured_area"}})")
            .arg(deduction ? "commercial_service" : "commercial_occupiable");
    };
    QString unrelated_wall;
    if (sourced) unrelated_wall=window.createStraightWall({0,-3},{4,-3});
    QString source_opening;
    QString area;
    if (sourced) {
        const auto walls=rectangle_walls(window);
        require(window.selectEntity(walls.front()),"select source wall for the hosted-window fixture");
        source_opening=window.createHostedOpening("window","1 m","0.8 m","0.8 m","1.2 m");
        require(!source_opening.isEmpty(),"copy fixture includes a genuinely hosted window");
        select_walls(window,walls);
        area=window.createMeasurementBoundaryFromSelectedWalls();
    } else area=window.createBoundary({{{0,0},{4,0},0},{{4,0},{4,3},0},
        {{4,3},{0,3},0},{{0,3},{0,0},0}},"finished");
    require(!area.isEmpty() && window.selectEntity(area) && window.editSelectedAppraisalFacts(declarations(false)),
        "clone fixture begins with a declared appraisal parent");
    if (commercial) {
        const auto deduction_layer=window.createLayer("floor-1","Service measurements");
        require(!deduction_layer.isEmpty() && window.setActiveLayer(deduction_layer),
            "valid deduction fixture uses a separate layer on the same floor");
    }
    const auto garage=window.createBoundary({{{1,1},{2,1},0},{{2,1},{2,2},0},
        {{2,2},{1,2},0},{{1,2},{1,1},0}},"garage");
    require(!garage.isEmpty() && window.selectEntity(garage) && window.editSelectedAppraisalFacts(declarations(true)) &&
        window.applySelectedAutoSubtract(area),"clone fixture has a declared garage or service-area deduction");
    set_area_name(window,area,"Independent appraisal copy");
    set_area_appearance(window,area);
    if (sourced) {
        bool styled=false;
        for (auto entity : annotation_entities(window.document().snapshot())) {
            auto& overrides=entity.properties.at("state").at("overrides");
            for (const auto& record : overrides)
                if (record.at("target_id")==area.toStdString()) {
                    auto opening_style=record;
                    opening_style["target_kind"]="object";
                    opening_style["target_id"]=source_opening.toStdString();
                    overrides.push_back(std::move(opening_style));
                    sketch::validate_annotation_entity(entity);
                    window.document().apply(sketch::ApplyEntityChanges{window.document().revision(),
                        {sketch::EntityChange::upsert(entity)},{},"Styled hosted window fixture"});
                    styled=true;
                    break;
                }
            if (styled) break;
        }
        require(styled,"hosted opening fixture has a genuine persisted appearance override");
    }
    const auto before=window.document().snapshot();
    std::string property_id;
    for (const auto& [id,value] : before.entities()) if (value.type=="property") property_id=id;
    const auto original_report=sketch::build_appraisal_document_report(before,property_id);
    require(original_report.qualified && original_report.calculation,"original parent and garage have qualified totals");
    const auto original_trace=std::find_if(original_report.calculation->calculation.areas.begin(),
        original_report.calculation->calculation.areas.end(),[&](const auto& trace){return trace.area_id==area.toStdString();});
    require(original_trace!=original_report.calculation->calculation.areas.end() &&
        std::abs(original_trace->net_square_metres-(analytical_area(before.entities().at(area.toStdString()))-1.0))<1e-8,
        "original net is its analytical exterior minus exactly one square metre");
    require(window.selectEntity(area),"select parent for a translated independent copy");
    if (!window.transformSelectedBoundary("0",false,false,"10 m","0 m",true))
        throw std::runtime_error("Appraisal clone refused: "+window.lastError().toStdString());
    const auto after=window.document().snapshot();
    std::optional<sketch::Entity> cloned_owner;
    for (const auto& [id,value] : after.entities())
        if (!before.entities().contains(id) && value.type=="measurement_boundary" && value.properties.contains("deduction_ids"))
            cloned_owner=value;
    require(cloned_owner && cloned_owner->properties.at("deduction_ids").size()==1,"copied parent retains exactly one deduction");
    const auto cloned_garage=cloned_owner->properties.at("deduction_ids").front().get<std::string>();
    require(cloned_garage!=garage.toStdString() && !before.entities().contains(cloned_garage) && after.entities().contains(cloned_garage),
        "copying a parent must copy its deduction instead of sharing the original garage");
    for (const auto* key : {"name","appraisal_facts","factor"})
        require(cloned_owner->properties.at(key)==before.entities().at(area.toStdString()).properties.at(key),
            "copied parent retains its name, declarations and exact factor");
    const auto appearance = [&](const sketch::DocumentSnapshot& snapshot,const std::string& target) {
        for (const auto& entity : annotation_entities(snapshot))
            for (auto value : entity.properties.at("state").at("overrides"))
                if (value.at("target_id")==target) { value.erase("target_id"); return value; }
        return nlohmann::json{};
    };
    require(!appearance(before,area.toStdString()).is_null() &&
        appearance(after,cloned_owner->id)==appearance(before,area.toStdString()),"copied parent retains its owned appearance override");
    const auto report=sketch::build_appraisal_document_report(after,property_id);
    require(report.qualified && report.calculation,"copied appraisal dependencies remain current and qualified");
    const auto parent_category=commercial ? sketch::AppraisalAreaCategory::commercial_occupiable :
        sketch::AppraisalAreaCategory::above_grade_finished;
    const auto child_category=commercial ? sketch::AppraisalAreaCategory::commercial_service :
        sketch::AppraisalAreaCategory::garage;
    require(std::abs(report.calculation->property.by_category.at(parent_category).total.square_metres-
        2*original_report.calculation->property.by_category.at(parent_category).total.square_metres)<1e-8 &&
        std::abs(report.calculation->property.by_category.at(child_category).total.square_metres-2)<1e-8 &&
        report.calculation->calculation.areas.size()==4,
        "property category totals count each independently copied net and garage exactly once");
    const auto trace=std::find_if(report.calculation->calculation.areas.begin(),report.calculation->calculation.areas.end(),
        [&](const auto& value){return value.area_id==cloned_owner->id;});
    require(trace!=report.calculation->calculation.areas.end() && std::abs(trace->net_square_metres-original_trace->net_square_metres)<1e-8 &&
        trace->deductions.size()==1 && trace->deductions.front().id==cloned_garage,
        "copied net area uses the copied garage once and matches the original unrounded net");
    require(after.entities().at(area.toStdString())==before.entities().at(area.toStdString()) &&
        after.entities().at(garage.toStdString())==before.entities().at(garage.toStdString()) &&
        after.revision()==before.revision()+1,"copy is one edit and does not alter original calculations or geometry");
    if (sourced) {
        require(sketch::wall_measurement_source_current(after,*cloned_owner),"copied exterior uses its own current physical walls");
        for (const auto& wall : cloned_owner->properties.at("wall_measurement_source").at("walls"))
            require(!before.entities().contains(wall.at("id").get<std::string>()),"copied exterior provenance never retains original wall IDs");
        std::size_t copied_openings=0;
        for (const auto& [id,entity] : after.entities())
            if (!before.entities().contains(id) && entity.type=="opening") {
                require(appearance(after,id)==appearance(before,source_opening.toStdString()),
                    "copied hosted window preserves its own independent appearance");
                ++copied_openings;
            }
        require(copied_openings==1,"required hosted window is copied once with its physical host");
    }
    require(window.undoCommand() && window.document().snapshot().entities()==before.entities() && window.redoCommand() &&
        window.document().snapshot().entities()==after.entities(),"copy dependencies undo and redo atomically");
    QTemporaryDir directory;
    sketch::desktop::MainWindow reopened;
    require(directory.isValid() && window.saveProjectAs(directory.filePath("appraisal-copy.bldproj")) &&
        reopened.openProject(directory.filePath("appraisal-copy.bldproj")) && reopened.document().snapshot().entities()==after.entities() &&
        sketch::build_appraisal_document_report(reopened.document().snapshot(),property_id).qualified,
        "copied dependencies and qualified totals survive save and reopen");
    require(window.selectEntity(QString::fromStdString(cloned_garage)),"select only the copied garage for editing");
    const auto child=sketch::decode_identified_boundary_entity(after.entities().at(cloned_garage));
    const auto shrink_corner=[&](std::size_t index,sketch::Vec2 delta) {
        const auto start=child.segments[index].segment.start;
        return window.moveSelectedBoundaryVertex(QString::fromStdString(child.segments[index].start_vertex_id),
            {start.x+delta.x,start.y+delta.y});
    };
    require(shrink_corner(1,{-.2,0}) && shrink_corner(2,{-.2,-.2}) && shrink_corner(3,{0,-.2}),
        "copied garage corners remain independently editable");
    const auto edited=window.document().snapshot();
    const auto edited_report=sketch::build_appraisal_document_report(edited,property_id);
    require(edited_report.qualified && edited_report.calculation &&
        edited.entities().at(garage.toStdString())==before.entities().at(garage.toStdString()),
        "editing the copied deduction does not alter the original garage or disable valid totals");
    const auto edited_trace=std::find_if(edited_report.calculation->calculation.areas.begin(),
        edited_report.calculation->calculation.areas.end(),[&](const auto& value){return value.area_id==cloned_owner->id;});
    require(edited_trace!=edited_report.calculation->calculation.areas.end() &&
        std::abs(edited_trace->net_square_metres-original_trace->net_square_metres-.36)<1e-8 &&
        std::abs(analytical_area(edited.entities().at(cloned_garage))-.64)<1e-8,
        "copy net increases by exactly the copied garage's independent reduction");
    require(std::abs(edited_report.calculation->property.by_category.at(parent_category).total.square_metres-
        report.calculation->property.by_category.at(parent_category).total.square_metres-.36)<1e-8 &&
        std::abs(edited_report.calculation->property.by_category.at(child_category).total.square_metres-1.64)<1e-8,
        "property totals move the copied deduction's exact area between categories without double counting");
    for (int index=0;index<3;++index) require(window.undoCommand(),"undo each copied garage corner edit");
    require(window.document().snapshot().entities()==after.entities() && window.undoCommand() &&
        window.document().snapshot().entities()==before.entities(),"return to the original drawing for clipboard copy");
    require(window.selectEntity(area),"select parent for a quarter-turn independent copy");
    if (!window.transformSelectedBoundary("90",false,false,"10 m","10 m",true))
        throw std::runtime_error("Rotated appraisal clone refused: "+window.lastError().toStdString());
    const auto rotated=window.document().snapshot();
    const auto rotated_id=window.selectedEntityId().toStdString();
    const auto rotated_report=sketch::build_appraisal_document_report(rotated,property_id);
    require(rotated.entities().contains(rotated_id) && rotated_report.qualified && rotated_report.calculation,
        "quarter-turn copied parent and dependencies retain qualified totals");
    const auto rotated_trace=std::find_if(rotated_report.calculation->calculation.areas.begin(),
        rotated_report.calculation->calculation.areas.end(),[&](const auto& value){return value.area_id==rotated_id;});
    require(rotated_trace!=rotated_report.calculation->calculation.areas.end() &&
        std::abs(rotated_trace->net_square_metres-original_trace->net_square_metres)<1e-8,
        "quarter-turn copy preserves the independent net area");
    if (sourced) require(sketch::wall_measurement_source_current(rotated,rotated.entities().at(rotated_id)),
        "quarter-turn copy uses accurately rotated physical source walls");
    require(window.undoCommand() && window.document().snapshot().entities()==before.entities(),
        "rotated copy is a single reversible edit");
    require(window.selectEntity(area) && window.copySelection(),"copy only the original parent through actual clipboard actions");
    if (sourced) {
        auto* clipboard=QGuiApplication::clipboard();
        const auto valid=clipboard->text();
        auto payload=nlohmann::json::parse(valid.toStdString());
        bool changed_host=false;
        for (auto& entity : payload.at("entities"))
            if (entity.at("type")=="opening") {
                entity["properties"]["wall_id"]=unrelated_wall.toStdString();
                changed_host=true;
            }
        require(changed_host,"malformed area clipboard fixture contains a real hosted window");
        const auto unchanged=window.document().snapshot();
        const auto digest=sketch::document_snapshot_digest(unchanged);
        clipboard->setText(QString::fromStdString(payload.dump()));
        require(!window.pasteSelection() && !window.lastError().isEmpty() &&
            window.document().revision()==unchanged.revision() &&
            window.document().snapshot().entities()==unchanged.entities() &&
            window.document().snapshot().history().size()==unchanged.history().size() &&
            sketch::document_snapshot_digest(window.document().snapshot())==digest,
            "incomplete area payload cannot attach a copied opening to an original host or partially alter history");
        clipboard->setText(valid);
    }
    require(window.pasteSelection(),"paste the complete original parent graph");
    const auto pasted=window.document().snapshot();
    const auto pasted_id=window.selectedEntityId().toStdString();
    require(pasted.entities().contains(pasted_id) && pasted.entities().at(pasted_id).type=="measurement_boundary",
        "pasting keeps the copied parent as the user selection");
    const auto pasted_child=pasted.entities().at(pasted_id).properties.at("deduction_ids").front().get<std::string>();
    require(!before.entities().contains(pasted_child) && pasted_child!=cloned_garage && pasted.entities().contains(pasted_child),
        "clipboard copy remaps the required deduction independently");
    const auto pasted_report=sketch::build_appraisal_document_report(pasted,property_id);
    const auto pasted_status=std::find_if(pasted_report.boundaries.begin(),pasted_report.boundaries.end(),
        [&](const auto& value){return value.boundary_id==pasted_id;});
    require(pasted_status!=pasted_report.boundaries.end() && pasted_status->qualification.qualified && pasted_status->measurement &&
        std::abs(pasted_status->measurement->net_square_metres-original_trace->net_square_metres)<1e-8,
        "copied relationships remain valid even when initial overlapping placement withholds property totals");
    auto* canvas=dynamic_cast<sketch::desktop::PlanCanvas*>(window.findChild<QWidget*>("measurementPlanCanvas"));
    require(canvas && window.selectEntity(QString::fromStdString(pasted_id)),"select the pasted parent on the actual canvas");
    window.fitView();
    canvas->setSnapEnabled(false);
    canvas->setWallSnapEnabled(false);
    const auto pasted_boundary=analytical_boundary(pasted.entities().at(pasted_id));
    const auto bounds=sketch::boundary_bounds(pasted_boundary);
    const sketch::Vec2 model_start{bounds.minimum.x+3,bounds.minimum.y+1.5};
    const auto desired_scale=std::min(30.0,(canvas->width()*.5-24)/
        (std::abs(model_start.x+10-canvas->viewCenter().x)+1));
    canvas->zoomBy(desired_scale/canvas->viewScale(),QRectF(canvas->rect()).center());
    const auto pixel=[&](sketch::Vec2 point) {
        const auto center=QRectF(canvas->rect()).center();
        const auto view=canvas->viewCenter();
        return QPointF(center.x()+(point.x-view.x)*canvas->viewScale(),center.y()-(point.y-view.y)*canvas->viewScale());
    };
    const auto from=pixel(model_start),to=pixel({model_start.x+10,model_start.y});
    require(canvas->rect().contains(from.toPoint()) && canvas->rect().contains(to.toPoint()),"copy move uses points visible inside the canvas");
    QMouseEvent press(QEvent::MouseButtonPress,from,from,Qt::LeftButton,Qt::LeftButton,Qt::NoModifier);
    QMouseEvent move(QEvent::MouseMove,to,to,Qt::NoButton,Qt::LeftButton,Qt::NoModifier);
    QMouseEvent release(QEvent::MouseButtonRelease,to,to,Qt::LeftButton,Qt::NoButton,Qt::NoModifier);
    QApplication::sendEvent(canvas,&press);
    QApplication::sendEvent(canvas,&move);
    QElapsedTimer preview_wait;
    preview_wait.start();
    while ((canvas->entitiesMovePreviewPending() || canvas->entitiesMovePreview().empty()) && preview_wait.elapsed()<10000)
        QCoreApplication::processEvents(QEventLoop::AllEvents,50);
    require(!canvas->entitiesMovePreviewPending() && !canvas->entitiesMovePreview().empty() &&
        window.document().snapshot().entities()==pasted.entities() && window.document().revision()==pasted.revision(),
        "selected parent has a complete transient dependency preview before release");
    const auto preview=canvas->entitiesMovePreview();
    std::set<std::string> moving_ids{pasted_id,pasted_child};
    if (sourced) {
        for (const auto& wall : pasted.entities().at(pasted_id).properties.at("wall_measurement_source").at("walls"))
            moving_ids.insert(wall.at("id").get<std::string>());
        for (const auto& [id,entity] : pasted.entities())
            if (entity.type=="opening" && moving_ids.contains(entity.properties.at("wall_id").get<std::string>())) moving_ids.insert(id);
    }
    for (const auto& id : moving_ids) {
        const auto rendered=std::find_if(canvas->entities().begin(),canvas->entities().end(),
            [&](const auto& entity){return entity.id.toStdString()==id;});
        const auto proposed=std::find_if(preview.begin(),preview.end(),
            [&](const auto& entity){return entity.id.toStdString()==id;});
        require(rendered!=canvas->entities().end() && proposed!=preview.end(),
            "parent drag preview contains every required deduction, wall and hosted opening");
        // Coincident original/pasted walls have joined outlines that change
        // when the copy separates. Their authoritative baselines still follow
        // the exact rigid gesture; that is the invariant to compare here.
        const auto& old_geometry=rendered->type=="wall" ? rendered->snap_segments : rendered->segments;
        const auto& next_geometry=proposed->type=="wall" ? proposed->snap_segments : proposed->segments;
        require(!old_geometry.empty() && old_geometry.size()==next_geometry.size(),"required preview retains analytical geometry");
        for (std::size_t index=0;index<old_geometry.size();++index) {
            const auto& old=old_geometry[index];
            const auto& next=next_geometry[index];
            require(std::abs(next.start.x-old.start.x-10)<1e-8 && std::abs(next.end.x-old.end.x-10)<1e-8 &&
                std::abs(next.start.y-old.start.y)<1e-8 && std::abs(next.end.y-old.end.y)<1e-8 &&
                std::abs(next.sweep_radians-old.sweep_radians)<1e-10,
                "transient required-group geometry matches the pending rigid drag: id="+id+
                "; index="+std::to_string(index)+"; dx="+std::to_string(next.start.x-old.start.x)+
                "; dy="+std::to_string(next.start.y-old.start.y));
        }
    }
    QApplication::sendEvent(canvas,&release);
    QElapsedTimer wait;
    wait.start();
    while (window.document().revision()==pasted.revision() && wait.elapsed()<10000)
        QCoreApplication::processEvents(QEventLoop::AllEvents,50);
    const auto moved=window.document().snapshot();
    require(moved.revision()==pasted.revision()+1,"dragging the selected parent moves its dependent geometry once");
    const auto moved_report=sketch::build_appraisal_document_report(moved,property_id);
    require(moved_report.qualified && moved_report.calculation &&
        moved.entities().at(area.toStdString())==before.entities().at(area.toStdString()) &&
        moved.entities().at(garage.toStdString())==before.entities().at(garage.toStdString()),
        "moving the pasted parent also moves only its required copied group and restores nonoverlapping totals");
    if (sourced) require(sketch::wall_measurement_source_current(moved,moved.entities().at(pasted_id)),
        "pasted parent drag preserves current physical wall provenance");
    capture(window,QStringLiteral("appraisal-copy-%1-%2-%3").arg(metric ? "metric" : "imperial")
        .arg(sourced ? "walls" : "drawn").arg(commercial ? "commercial" : "residential"));
    require(window.undoCommand() && window.document().snapshot().entities()==pasted.entities() && window.redoCommand() &&
        window.document().snapshot().entities()==moved.entities(),"copied group canvas movement undoes and redoes atomically");
    if (sourced) {
        const auto missing_wall=moved.entities().at(pasted_id).properties.at("wall_measurement_source").at("walls").front().at("id").get<std::string>();
        require(window.selectEntity(QString::fromStdString(missing_wall)) && window.deleteSelection() &&
            window.selectEntity(QString::fromStdString(pasted_id)),"prepare a genuinely stale source copy without changing original walls");
        const auto stale=window.document().snapshot();
        require(!window.transformSelectedBoundary("0",false,false,"20 m","0 m",true) && !window.lastError().isEmpty() &&
            window.document().snapshot().entities()==stale.entities() && window.document().revision()==stale.revision(),
            "cloning missing physical sources refuses with an explanation and no partial copy");
        require(!window.copySelection() && !window.lastError().isEmpty() && window.document().snapshot().entities()==stale.entities(),
            "clipboard copy refuses missing required source walls instead of publishing broken links");
    }
}

void plain_source_repair_clones_and_pastes_its_retained_proof() {
    sketch::desktop::MainWindow seed_window;
    const auto wall_ids=rectangle_walls(seed_window);
    select_walls(seed_window,wall_ids);
    const auto area=seed_window.createMeasurementBoundaryFromSelectedWalls();
    require(!area.isEmpty(),"plain repair fixture begins with an exterior owner");
    std::vector<sketch::Entity> imported;
    const auto seeded=seed_window.document().snapshot();
    for (const auto& [id,value] : seeded.entities()) {
        auto entity=value;
        if (id==area.toStdString()) entity.properties.erase("boundary_authoring");
        imported.push_back(std::move(entity));
    }
    auto document=std::make_shared<sketch::Document>(sketch::Document::create(imported));
    sketch::desktop::MainWindow window(document);
    window.setMetricUnits(true);
    require(window.selectEntity(wall_ids.front()) && window.deleteSelection(),"replace a plain owner's source wall");
    const auto replacement=window.createStraightWall({0,0},{4,0});
    require(!replacement.isEmpty() && window.selectEntity(area),"plain source repair retains the imported owner");
    std::exception_ptr failure;
    QTimer::singleShot(0,&window,[&] {
        auto* dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget());
        try {
            require(dialog && dialog->objectName()=="wallMeasurementSourceReview","plain source opens the real repair dialog");
            auto* seed=dialog->findChild<QComboBox*>("wallMeasurementSourceSeed");
            auto* buttons=dialog->findChild<QDialogButtonBox*>("wallMeasurementSourceButtons");
            require(seed && buttons && seed->findData(replacement)>=0,"plain repair offers its replacement wall");
            seed->setCurrentIndex(seed->findData(replacement));
            require(buttons->button(QDialogButtonBox::Apply)->isEnabled(),"plain repair validates without invented input receipts");
            buttons->button(QDialogButtonBox::Apply)->click();
        } catch (...) { failure=std::current_exception(); if (dialog) dialog->reject(); }
    });
    auto* repair=window.findChild<QAction*>("replaceExteriorMeasurementSources");
    require(repair,"plain repair has an actual action");
    repair->trigger();
    if (failure) std::rethrow_exception(failure);
    const auto repaired=window.document().snapshot();
    const auto original=repaired.entities().at(area.toStdString());
    require(original.extensions.at("boundary_geometry_derivation").at("version")==2 &&
                !original.properties.contains("boundary_authoring"),"plain source repair archives geometry without fabricating receipts");
    require(window.transformSelectedBoundary("0",false,false,"10 m","0 m",true),
        "a plain source-repaired boundary can be cloned with its retained proof");
    const auto cloned=window.document().snapshot();
    const auto clone_id=window.selectedEntityId().toStdString();
    require(clone_id!=original.id && cloned.entities().at(original.id)==original &&
                cloned.entities().at(clone_id).extensions.at("boundary_geometry_derivation").at("version")==2,
        "single clone remaps the archived topology while preserving the source owner");
    require(window.undoCommand() && window.document().snapshot().entities()==repaired.entities() &&
                window.redoCommand() && window.document().snapshot().entities()==cloned.entities() && window.undoCommand(),
        "a plain source clone undoes and redoes exactly");
    require(window.selectEntity(area),"copy the original repaired owner with its current walls");
    for (qsizetype i=1;i<wall_ids.size();++i)
        require(window.selectEntity(wall_ids[i],true),"add each surviving wall to the clipboard group");
    require(window.selectEntity(replacement,true) && window.copySelection(),"copy all current sources with the repaired owner");
    const auto before_paste=window.document().snapshot();
    require(window.pasteSelection(),"group paste remaps plain source-repair proofs");
    const auto pasted=window.document().snapshot();
    std::optional<sketch::Entity> pasted_owner;
    for (const auto& [id,value] : pasted.entities())
        if (!before_paste.entities().contains(id) && value.type=="measurement_boundary" &&
            value.properties.contains("wall_measurement_source")) pasted_owner=value;
    require(pasted_owner && sketch::wall_measurement_source_current(pasted,*pasted_owner),
        "the pasted owner uses the pasted source walls and remains analytically current");
    std::set<std::string> sources;
    for (const auto& record : pasted_owner->properties.at("wall_measurement_source").at("walls")) {
        const auto id=record.at("id").get<std::string>();
        require(!before_paste.entities().contains(id) && pasted.entities().at(id).type=="wall",
            "group provenance refers to fresh pasted walls rather than original source walls");
        sources.insert(id);
    }
    const auto& operations=pasted_owner->extensions.at("boundary_geometry_derivation").at("operations");
    const auto& reviewed=operations.front().at("value").at("replacement_wall_source_ids");
    require(std::set<std::string>(reviewed.begin(),reviewed.end())==sources,
        "the persisted typed replacement proof remaps the same source identities as the provenance envelope");
    require(window.undoCommand() && window.document().snapshot().entities()==before_paste.entities() &&
                window.redoCommand() && window.document().snapshot().entities()==pasted.entities(),
        "group paste restores the complete geometry and provenance together through history");
    QTemporaryDir directory;
    sketch::desktop::MainWindow reopened;
    require(directory.isValid() && window.saveProjectAs(directory.filePath("plain-source-clones.bldproj")) &&
                reopened.openProject(directory.filePath("plain-source-clones.bldproj")) &&
                reopened.document().snapshot().entities()==pasted.entities(),
        "plain source repair and group clone proofs survive save and reopen");
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
        if (app.arguments().contains("--appraisal-copy-only")) {
            for (bool metric : {false,true}) for (bool sourced : {false,true}) for (bool commercial : {false,true})
                appraisal_area_clone_retains_its_calculation_dependencies(metric,sourced,commercial);
            std::cout << "Appraisal copy dependencies passed\n";
            return 0;
        }
        if (app.arguments().contains("--same-count-source-repair-only")) {
            for (bool metric : {false,true}) for (bool curved : {false,true})
                changed_same_count_sources_review_manual_references(metric,curved);
            std::cout << "Changed same-count source repair workflows passed\n";
            return 0;
        }
        if (app.arguments().contains("--source-repair-only")) {
            for (bool metric : {false,true}) for (bool curved : {false,true})
                replaced_wall_sources_repair_existing_appraisal_area(metric,curved);
            split_source_replacement_reviews_manual_references();
            plain_source_repair_clones_and_pastes_its_retained_proof();
            std::cout << "Exterior measurement source repair workflows passed\n";
            return 0;
        }
        selected_wall_loop_creates_and_refreshes_one_exterior_appraisal_area();
        curved_d_exterior_measurement_stays_analytic_through_refresh_and_output();
        wall_measurement_admission_failures_are_atomic();
        an_unsplit_partition_selects_its_exterior_shell();
        split_perimeter_with_t_branches_and_internal_chord();
        automatic_candidates_respect_placement_and_phase();
        level_bound_walls_share_a_physical_plane_despite_arithmetic_roundoff();
        for (bool metric : {false,true}) for (bool curved : {false,true})
            replaced_wall_sources_repair_existing_appraisal_area(metric,curved);
        split_source_replacement_reviews_manual_references();
        plain_source_repair_clones_and_pastes_its_retained_proof();
        for (bool metric : {false,true}) for (bool sourced : {false,true}) for (bool commercial : {false,true})
            appraisal_area_clone_retains_its_calculation_dependencies(metric,sourced,commercial);
        for (bool metric : {false,true}) for (bool curved : {false,true})
            changed_same_count_sources_review_manual_references(metric,curved);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
