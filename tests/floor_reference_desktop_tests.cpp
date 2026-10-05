#include "sketch/desktop/main_window.hpp"
#include "sketch/annotation_entity_codec.hpp"
#include "sketch/boundary_entity.hpp"
#include "sketch/floor_reference.hpp"
#include "sketch/sheet_view_entity_codec.hpp"
#include "../src/desktop/plan_canvas.hpp"
#include "support/noninteractive_errors.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFontDatabase>
#include <QLabel>
#include <QPdfDocument>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
using namespace sketch;
using namespace sketch::desktop;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void events() { QCoreApplication::processEvents(); }
template<class T> T* child(MainWindow& window, const char* name) {
    auto* result = window.findChild<T*>(QString::fromLatin1(name));
    require(result != nullptr, "floor reference workspace control exists");
    return result;
}
PlanCanvas* canvas(MainWindow& window, const char* name = "measurementPlanCanvas") {
    auto* result = dynamic_cast<PlanCanvas*>(child<QWidget>(window, name));
    require(result != nullptr, "actual plan canvas exists");
    return result;
}
bool contains(const std::vector<CanvasEntity>& entities, const QString& id) {
    return std::any_of(entities.begin(), entities.end(),
        [&](const auto& entity) { return entity.id == id; });
}
bool equal_geometry(const Boundary& a, const Boundary& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(),
        [](const auto& x, const auto& y) {
            return x.start.x == y.start.x && x.start.y == y.start.y &&
                x.end.x == y.end.x && x.end.y == y.end.y && x.sweep_radians == y.sweep_radians;
        });
}
const CanvasEntity& ghost(MainWindow& window, const QString& id) {
    const auto& entities = canvas(window)->floorGhostEntities();
    const auto found = std::find_if(entities.begin(), entities.end(),
        [&](const auto& entity) { return entity.id == id; });
    require(found != entities.end(), "source geometry is projected into ghost");
    return *found;
}
FloorReferenceSettings settings(MainWindow& window) {
    const auto value = resolve_floor_reference(window.document().snapshot(), "f2");
    require(value.has_value(), "destination retains a valid floor reference");
    return *value;
}
void unchanged(MainWindow& window, const DocumentSnapshot& before) {
    const auto after = window.document().snapshot();
    require(after.revision() == before.revision() && after.entities() == before.entities() &&
            after.assets() == before.assets(), "rejected reference edit preserves document and history");
}
Boundary rectangle(double x, double y, double width, double height) {
    return {{{x, y}, {x + width, y}, 0}, {{x + width, y}, {x + width, y + height}, 0},
            {{x + width, y + height}, {x, y + height}, 0}, {{x, y + height}, {x, y}, 0}};
}
std::shared_ptr<Document> fixture() {
    return std::make_shared<Document>(Document::create({
        {"p", "property", {{"name", "Floor tracing"}}, false},
        {"b", "building", {{"property_id", "p"}, {"name", "House"}}, false},
        {"f1", "floor", {{"building_id", "b"}, {"name", "Ground"}}, false},
        {"f2", "floor", {{"building_id", "b"}, {"name", "Upper"}}, false},
        {"l1", "layer", {{"floor_id", "f1"}, {"name", "Ground plan"}}, false},
        {"l2", "layer", {{"floor_id", "f2"}, {"name", "Upper plan"}}, false},
        {"other-building", "building", {{"property_id", "p"}}, false},
        {"other-floor", "floor", {{"building_id", "other-building"}}, false},
        make_annotation_entity("annotations", AnnotationState{})
    }));
}
struct Drawing { QString source_area, source_wall, destination_area, source_dimension; };
Drawing draw(MainWindow& window) {
    window.setMetricUnits(true);
    require(window.setActiveLayer("l1"), "activate source floor");
    const auto source = window.createBoundary(rectangle(0, 0, 6, 4));
    const auto wall = window.createStraightWall({0, 0}, {6, 0});
    require(!source.isEmpty() && !wall.isEmpty(), "source floor has real boundary and wall");
    const auto source_label = window.createAnnotationLabel("note", "Source floor only", {5, 3});
    require(!source_label.isEmpty(), "source floor has real text");
    const auto source_dimension = window.createAreaDimension(source, {3, 2});
    require(!source_dimension.isEmpty(), "source floor has real associative area text");
    require(window.setActiveLayer("l2"), "activate destination floor");
    const auto destination = window.createBoundary(rectangle(1, 1, 4, 3));
    require(!destination.isEmpty(), "destination floor has real boundary");
    require(!window.createAnnotationLabel("note", "Destination floor", {2, 2}).isEmpty(),
            "destination floor has real text");
    require(window.selectEntity(destination), "select destination calculation context");
    return {source, wall, destination, source_dimension};
}
void source_sheet(MainWindow& window, const Drawing& drawing) {
    CoordinatedView view; view.id = "source-plan"; view.name = "Ground floor plan";
    view.object_ids = {drawing.source_area.toStdString(), drawing.source_wall.toStdString(),
                      drawing.source_dimension.toStdString()};
    DrawingSheet sheet; sheet.id = "source-sheet"; sheet.number = "F-01";
    sheet.title_block.title = "Saved ground floor viewport";
    sheet.viewports = {{"source-viewport", view.id, {10, 10, 300, 220}, 50}};
    auto snapshot = window.document().snapshot();
    std::string owner = "floor-sheets";
    for (const auto& [id, entity] : snapshot.entities())
        if (entity.type == kSheetViewEntityType) { owner = id; break; }
    window.document().apply(ApplyEntityChanges{snapshot.revision(),
        {EntityChange::upsert(make_sheet_view_entity(owner, SheetViewModel::create({view}, {sheet})))},
        {}, "Add saved source floor viewport"});
    require(window.selectEntity(drawing.destination_area) && window.selectOutputSheet("source-sheet"),
            "saved source floor viewport is selected while destination floor is active");
}
struct Pdf { QSizeF page; QImage image; QString text; };
Pdf pdf(MainWindow& window, const QString& path, bool sheet = false) {
    if (!(sheet ? window.exportDraftPdf(path) : window.exportSketchPdf(path)))
        throw std::runtime_error("floor tracing PDF: " + window.lastError().toStdString());
    QPdfDocument document;
    require(document.load(path) == QPdfDocument::Error::None && document.pageCount() == 1,
            "focused sketch PDF reopens as one page");
    const auto page = document.pagePointSize(0);
    auto image = document.render(0, page.scaled(QSizeF(1200, 1000), Qt::KeepAspectRatio).toSize());
    require(!image.isNull(), "focused sketch PDF renders");
    return {page, image, document.getAllText(0).text()};
}
void same(const Pdf& a, const Pdf& b) {
    require(a.page == b.page && a.image == b.image && a.text == b.text,
            "ghost visibility, opacity and offset cannot enter focused PDF page, pixels or text");
}
void capture(MainWindow& window, const QString& name) {
    const auto directory = qEnvironmentVariable("VERTEX_TEST_CAPTURE_DIR");
    if (directory.isEmpty()) return;
    require(QDir().mkpath(directory), "create native floor tracing capture directory");
    events();
    require(window.grab().save(QDir(directory).filePath(name + ".png")),
            "capture actual focused floor tracing workspace");
}

void workflow() {
    QTemporaryDir directory;
    require(directory.isValid(), "floor reference fixture has temporary storage");
    MainWindow window(fixture(), nullptr, directory.filePath("text-library.json"));
    window.setAttribute(Qt::WA_DontShowOnScreen, true);
    window.resize(1400, 900); window.show(); events();
    const auto drawing = draw(window);
    source_sheet(window, drawing);
    const auto sheet_before = pdf(window, directory.filePath("source-sheet-before.pdf"), true);
    require(sheet_before.text.contains("24"), "saved source viewport contains real source area dimension text");
    auto* target = canvas(window);
    const auto before = window.document().snapshot();
    auto* total = child<QLabel>(window, "calculationBuildingTotal");
    require(total->text() != "—" && !total->text().isEmpty(), "real areas expose calculation total");
    const auto total_before = total->text();
    const auto source_before = before.entities().at(drawing.source_area.toStdString());
    const auto destination_before = before.entities().at(drawing.destination_area.toStdString());
    require(std::abs(signed_area(boundary_geometry(decode_identified_boundary_entity(source_before))) - 24) < 1e-8 &&
            std::abs(signed_area(boundary_geometry(decode_identified_boundary_entity(destination_before))) - 12) < 1e-8,
            "source and destination analytical areas are independently 24 and 12 square metres");

    auto* source_control = child<QComboBox>(window, "floorReferenceSource");
    const auto source_index = source_control->findData("f1");
    require(source_index > 0, "actual floor selector offers source floor");
    source_control->setCurrentIndex(source_index);
    require(QMetaObject::invokeMethod(source_control, "activated", Qt::DirectConnection,
                Q_ARG(int, source_index)) && settings(window).source_floor_id == "f1",
            "actual source selector links destination to another floor");
    require(window.document().revision() == before.revision() + 1,
            "link is one ordinary document command");
    require(contains(target->entities(), drawing.destination_area) &&
            !contains(target->entities(), drawing.source_area) &&
            contains(target->floorGhostEntities(), drawing.source_area),
            "destination is focused with source present only in ghost projection");
    const auto sheet_on = pdf(window, directory.filePath("source-sheet-ghost-on.pdf"), true);
    require(sheet_before.page == sheet_on.page && sheet_before.image == sheet_on.image &&
            sheet_before.text == sheet_on.text,
            "saved source viewport keeps source geometry and text while another floor is focused for tracing");
    require(canvas(window, "architecturalPlanCanvas")->floorGhostEntities().empty(),
            "measurement floor reference cannot become an architectural ghost");
    require(window.document().snapshot().entities().at(drawing.source_area.toStdString()) == source_before &&
            window.document().snapshot().entities().at(drawing.destination_area.toStdString()) == destination_before &&
            total->text() == total_before, "link leaves analytical area geometry and full building total intact");
    require(window.undoCommand() && !FloorReferenceSettings::from_entity(
                window.document().snapshot().entities().at("f2")) && target->floorGhostEntities().empty(),
            "Undo removes retained link and stale ghost");
    require(window.redoCommand() && contains(target->floorGhostEntities(), drawing.source_area),
            "Redo restores reference projection");

    auto* source = child<QComboBox>(window, "floorReferenceSource");
    auto* show = child<QCheckBox>(window, "floorReferenceVisible");
    auto* opacity = child<QSpinBox>(window, "floorReferenceOpacity");
    require(source->currentData().toString() == "f1" && source->findData("other-floor") < 0 &&
            show->isChecked(), "real source control reflects link and excludes other buildings");
    const auto on = pdf(window, directory.filePath("ghost-on.pdf"));
    require(on.text.contains("Destination floor") && !on.text.contains("Source floor only"),
            "focused PDF contains destination text and excludes source text");
    target->fitView();
    // Fit intentionally excludes reference extents. Pan/zoom the capture so
    // both floors are visible; reference controls must never change that rule.
    target->setViewTransform({3, 2}, 100);
    const auto visible_image = target->grab().toImage();
    capture(window, "floor-reference-white");
    show->click(); events();
    require(!settings(window).visible && target->floorGhostEntities().empty(),
            "actual Show reference control persists visibility and removes ghost");
    require(visible_image != target->grab().toImage(), "reference is actually visible on native canvas");
    same(on, pdf(window, directory.filePath("ghost-off.pdf")));
    const auto sheet_off = pdf(window, directory.filePath("source-sheet-ghost-off.pdf"), true);
    require(sheet_before.page == sheet_off.page && sheet_before.image == sheet_off.image &&
            sheet_before.text == sheet_off.text,
            "saved source viewport output is unchanged when destination ghost is hidden");
    show->click(); opacity->setValue(45); events();
    require(settings(window).visible && settings(window).opacity == .45 && target->floorGhostOpacity() == .45,
            "actual opacity control updates persisted and rendered ghost");
    const auto geometry = ghost(window, drawing.source_area).segments;
    require(window.setFloorReference("f2", "f1", true, .45, {1.5, -.75}), "align floor ghost");
    require(target->floorGhostOffset().x == 1.5 && target->floorGhostOffset().y == -.75 &&
            equal_geometry(ghost(window, drawing.source_area).segments, geometry),
            "alignment remains a renderer offset instead of moving source geometry");
    same(on, pdf(window, directory.filePath("aligned.pdf")));
    window.setWorkspaceTheme(WorkspaceTheme::dark); events();
    capture(window, "floor-reference-dark");
    window.setWorkspaceTheme(WorkspaceTheme::light);

    // A source edit made through ordinary document history must replace cached ghost geometry.
    const auto old_bounds = boundary_bounds(ghost(window, drawing.source_wall).segments);
    auto moved = window.document().snapshot().entities().at(drawing.source_wall.toStdString());
    moved.properties["baseline"]["end"] = {8, 0};
    window.document().apply(ApplyEntityChanges{window.document().revision(),
        {EntityChange::upsert(moved)}, {}, "Extend source wall"});
    require(window.selectEntity(drawing.destination_area), "refresh live source head at destination");
    require(boundary_bounds(ghost(window, drawing.source_wall).segments).maximum.x > old_bounds.maximum.x + 1.9,
            "ghost consumes current source geometry rather than a retained copy");
    same(on, pdf(window, directory.filePath("source-edited.pdf")));
    require(total->text() == total_before, "source wall edit and ghost settings preserve area totals");
    const auto project = directory.filePath("floor-reference.bldproj");
    const auto retained = settings(window);
    require(window.saveProjectAs(project) && window.openProject(project) &&
            window.selectEntity(drawing.destination_area), "save and reopen floor tracing project");
    require(settings(window) == retained && contains(target->floorGhostEntities(), drawing.source_wall),
            "save/reopen retain source, visibility, opacity and exact offset");
    same(on, pdf(window, directory.filePath("reopened.pdf")));

    const auto clean = window.document().snapshot();
    require(!window.setFloorReference("f2", "other-floor") && window.lastError().contains("same building"),
            "cross-building reference is rejected with repairable diagnostic");
    unchanged(window, clean);
    require(!window.setFloorReference("f2", "f2"), "self reference is rejected"); unchanged(window, clean);
    require(!window.setFloorReference("f2", "f1", true, .3, {}, clean.revision() - 1),
            "stale reference revision is rejected"); unchanged(window, clean);
    require(!window.clearFloorReference("f2", clean.revision() - 1), "stale clear is rejected"); unchanged(window, clean);
    require(!window.setFloorReference("f2", "f1", true, std::numeric_limits<double>::quiet_NaN()),
            "nonfinite opacity is rejected"); unchanged(window, clean);
    require(window.clearFloorReference("f2") && target->floorGhostEntities().empty(),
            "clear removes reference and ghost");
    require(window.undoCommand() && settings(window) == retained,
            "clear is undoable with all reference settings intact");
    window.document().mark_read_only("floor tracing test");
    const auto readonly = window.document().snapshot();
    require(window.selectEntity(drawing.destination_area), "refresh read-only reference controls");
    require(!source->isEnabled() && !show->isEnabled() && !opacity->isEnabled(),
            "read-only reference controls disable mutation");
    require(!window.setFloorReference("f2", "f1", true, .3) &&
            window.lastError().contains("read-only"), "read-only reference update is rejected");
    unchanged(window, readonly);
    require(!window.clearFloorReference("f2"), "read-only reference clear is rejected"); unchanged(window, readonly);
}

void repair() {
    QTemporaryDir directory;
    require(directory.isValid(), "repair fixture has temporary storage");
    MainWindow window(fixture(), nullptr, directory.filePath("text-library.json"));
    require(window.setActiveLayer("l1"), "activate repair source floor");
    const auto source_wall = window.createStraightWall({0, 0}, {6, 0});
    require(!source_wall.isEmpty() && window.setActiveLayer("l2"), "repair source wall is real");
    const auto destination_area = window.createBoundary(rectangle(1, 1, 4, 3));
    require(!destination_area.isEmpty() && window.selectEntity(destination_area),
            "repair destination retains valid organization context");
    auto* target = canvas(window);
    auto* diagnostic = child<QLabel>(window, "floorReferenceDiagnostic");
    require(window.setFloorReference("f2", "f1") && !target->floorGhostEntities().empty(),
            "repair fixture starts with live source ghost");
    window.document().apply(ApplyEntityChanges{window.document().revision(),
        {EntityChange::erase(source_wall.toStdString()), EntityChange::erase("l1"),
         EntityChange::erase("f1")}, {}, "Delete source floor and its children"});
    require(window.selectEntity(destination_area), "refresh deleted source reference");
    require(target->floorGhostEntities().empty() && target->floorGhostLabels().empty() &&
            diagnostic->text().contains("source floor", Qt::CaseInsensitive),
            "deleted source clears stale geometry/text and exposes a repair diagnostic");
    require(window.undoCommand() && !target->floorGhostEntities().empty(),
            "restoring deleted source repairs live reference");
    Entity malformed{"bad-source-slab", "slab", {{"floor_id", "f1"}, {"layer_id", "l1"},
        {"boundary", {{{"start", "malformed"}, {"end", {4, 4}}, {"sweep_radians", 0}}}},
        {"holes", nlohmann::json::array()}, {"thickness_m", .15}, {"elevation_m", 0}}, false};
    window.document().apply(ApplyEntityChanges{window.document().revision(),
        {EntityChange::upsert(malformed)}, {}, "Malformed source geometry fixture"});
    require(window.selectEntity(destination_area), "refresh malformed source geometry");
    require(target->floorGhostEntities().empty() && target->floorGhostLabels().empty() &&
            !diagnostic->text().isEmpty(), "malformed source reports repair issue and clears previous ghost");
    require(window.undoCommand() && !target->floorGhostEntities().empty(), "Undo malformed source restores ghost");
    auto floor = window.document().snapshot().entities().at("f2");
    floor.properties["tracing_reference"]["offset_m"]["x"] = "malformed";
    window.document().apply(ApplyEntityChanges{window.document().revision(),
        {EntityChange::upsert(floor)}, {}, "Malformed reference metadata fixture"});
    require(window.selectEntity(destination_area), "refresh malformed reference metadata");
    require(target->floorGhostEntities().empty() && !diagnostic->text().isEmpty(),
            "malformed reference metadata cannot leave stale ghost");
    auto* source = child<QComboBox>(window, "floorReferenceSource");
    source->setCurrentIndex(0);
    require(QMetaObject::invokeMethod(source, "activated", Qt::DirectConnection, Q_ARG(int, 0)),
            "activate actual no-reference command");
    require(!FloorReferenceSettings::from_entity(window.document().snapshot().entities().at("f2")) &&
            diagnostic->text().isEmpty(), "actual source control clears malformed metadata to repair reference");
}
}  // namespace

int main(int argc, char** argv) {
    sketch::testing::noninteractive_errors();
    QStandardPaths::setTestModeEnabled(true);
    QApplication application(argc, argv);
    try {
        require(QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/Inter.ttf")) >= 0,
                "bundled Inter font is available for native captures and PDF equality");
        application.setFont(QFont(QStringLiteral("Inter"), 10));
        workflow(); repair(); std::cout << "Floor reference desktop checks passed\n"; return 0;
    }
    catch (const std::exception& error) {
        std::cerr << "floor_reference_desktop_tests: " << error.what() << '\n'; return 1;
    }
}
