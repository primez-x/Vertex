#include "sketch/desktop/main_window.hpp"
#include "sketch/annotation_entity_codec.hpp"
#include "sketch/boundary_dimension.hpp"
#include "sketch/physical_wall_room.hpp"
#include "sketch/document_digest.hpp"
#include "../src/desktop/plan_canvas.hpp"
#include "support/noninteractive_errors.hpp"

#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QFontDatabase>
#include <QLabel>
#include <QListWidget>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QAction>
#include <QDialog>
#include <QDialogButtonBox>
#include <QPushButton>
#include <QTimer>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {
using namespace sketch;
using namespace sketch::desktop;
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void assert_near(double actual, double expected) {
    require(std::isfinite(actual) && std::abs(actual - expected) < 1e-8,
            "physical room quantity matches independently calculated clear area");
}
template<class T> T* child(MainWindow& window, const char* name) {
    auto* value = window.findChild<T*>(QString::fromLatin1(name));
    require(value != nullptr, "physical room workspace control exists");
    return value;
}
PlanCanvas& canvas(MainWindow& window) {
    auto* result = dynamic_cast<PlanCanvas*>(child<QWidget>(window, "measurementPlanCanvas"));
    require(result != nullptr, "physical room uses actual measurement canvas");
    return *result;
}
const CanvasEntity* geometry(PlanCanvas& canvas, const QString& id) {
    const auto& values = canvas.entities();
    const auto found = std::find_if(values.begin(), values.end(),
        [&](const auto& entity) { return entity.id == id; });
    return found == values.end() ? nullptr : &*found;
}
QString label(PlanCanvas& canvas, const QString& id) {
    QString result;
    for (const auto& value : canvas.labels()) if (value.id == id) result += value.text;
    return result;
}
void click(PlanCanvas& target, Vec2 point) {
    const auto center = target.viewCenter();
    const auto scale = target.viewScale();
    const QPointF pixel(target.rect().center().x() + (point.x - center.x) * scale,
                        target.rect().center().y() - (point.y - center.y) * scale);
    QMouseEvent press(QEvent::MouseButtonPress, pixel, target.mapToGlobal(pixel.toPoint()),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&target, &press);
    QMouseEvent release(QEvent::MouseButtonRelease, pixel, target.mapToGlobal(pixel.toPoint()),
                        Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&target, &release);
    QCoreApplication::processEvents();
}
Entity wall(const char* id, Vec2 start, Vec2 end) {
    return {id, "wall", {{"property_id", "p"}, {"building_id", "b"}, {"floor_id", "f"},
        {"layer_id", "l"}, {"baseline", {{"start", {start.x, start.y}},
        {"end", {end.x, end.y}}, {"sweep_radians", 0}}}, {"thickness_m", .2},
        {"height_m", 3}, {"elevation_m", 0}}, false};
}
std::shared_ptr<Document> fixture() {
    return std::make_shared<Document>(Document::create({
        {"p", "property", {{"name", "Physical room desktop fixture"}}, false},
        {"b", "building", {{"property_id", "p"}}, false},
        {"f", "floor", {{"building_id", "b"}}, false},
        {"l", "layer", {{"floor_id", "f"}}, false},
        wall("bottom", {0, 0}, {4, 0}), wall("right", {4, 0}, {4, 3}),
        wall("top", {4, 3}, {0, 3}), wall("left", {0, 3}, {0, 0}),
        wall("island", {1, 1}, {3, 1}), make_annotation_entity("annotations", AnnotationState{})
    }));
}
const ScheduleRow* room_row(const DocumentScheduleProjection& projection, const QString& id) {
    const auto& rows = projection.snapshot.rows;
    const auto found = std::find_if(rows.begin(), rows.end(),
        [&](const auto& row) { return row.object_id == id.toStdString() && row.kind == ScheduleRowKind::room; });
    return found == rows.end() ? nullptr : &*found;
}
void current_schedule(MainWindow& window, const QString& id) {
    const auto projection = window.scheduleSnapshot();
    const auto* row = room_row(projection, id);
    require(row != nullptr, "current clear room contributes a real room schedule row");
    const auto& area = row->cells.at("gross_area");
    require(!area.editable && std::holds_alternative<ScheduleQuantity>(area.value),
            "room schedule area is a calculated SI quantity");
    const auto value = std::get<ScheduleQuantity>(area.value);
    require(value.unit == ScheduleUnit::square_metre, "room schedule retains square metre quantity");
    assert_near(value.value, 10.24);
}
void current_details(MainWindow& window) {
    require(child<QLabel>(window, "calculationNetArea")->text() == QStringLiteral("10.24 m²") &&
            child<QLabel>(window, "calculationBaseArea")->text() == QStringLiteral("10.64 m²"),
            "actual Details distinguish net clear area from clear outer boundary area");
    const auto* holes = child<QListWidget>(window, "calculationDeductions");
    require(holes->count() == 1 && holes->item(0)->text().contains("0.40 m²"),
            "actual Details list the inline physical wall island deduction");
    require(child<QLabel>(window, "calculationStatus")->text().contains("inside wall faces"),
            "actual Details explain clear room measurement basis");
}
void rejects_entity_only_dimension(const Entity& room) {
    BoundaryDimension dimension;
    dimension.id = "test-room-area-dimension";
    dimension.boundary_id = room.id;
    dimension.kind = BoundaryDimensionKind::area;
    dimension.text_position = {2, 2};
    try { (void)dimension.resolve(room); }
    catch (const std::invalid_argument& error) {
        require(QString::fromUtf8(error.what()).contains("physical", Qt::CaseInsensitive),
                "authoritative dimension resolver explains missing live physical source");
        return;
    }
    throw std::runtime_error("Entity-only area dimension certified persisted source-bound room geometry");
}
void capture(MainWindow& window, const char* name) {
    const auto directory = qEnvironmentVariable("VERTEX_TEST_CAPTURE_DIR");
    if (directory.isEmpty()) return;
    QCoreApplication::processEvents();
    require(QDir().mkpath(directory) && window.grab().save(QDir(directory).filePath(QString::fromLatin1(name))),
            "capture actual native physical room workspace");
}
void review_room(MainWindow& window,const std::function<void(QDialog&)>& interaction) {
    auto* action=child<QAction>(window,"repairPhysicalWallRoom"); std::exception_ptr failure;
    QTimer watchdog; watchdog.setSingleShot(true);
    QObject::connect(&watchdog,&QTimer::timeout,&window,[&] {
        auto* dialog=dynamic_cast<QDialog*>(QApplication::activeModalWidget());
        const auto* status=dialog?dialog->findChild<QLabel*>("physicalRoomRepairStatus"):nullptr;
        failure=std::make_exception_ptr(std::runtime_error("Repair review did not close: "+(status?status->text().toStdString():std::string("no modal"))));
        if (dialog) dialog->reject();
    });
    QTimer::singleShot(0,&window,[&] {
        auto* dialog=dynamic_cast<QDialog*>(QApplication::activeModalWidget());
        try { require(dialog && dialog->objectName()=="physicalRoomRepair","actual native room repair dialog opens"); interaction(*dialog); }
        catch (...) { failure=std::current_exception(); if (dialog) dialog->reject(); }
    });
    watchdog.start(4000); action->trigger(); watchdog.stop(); if (failure) std::rethrow_exception(failure);
}
void workflow() {
    QTemporaryDir files;
    require(files.isValid(), "physical room fixture has temporary storage");
    MainWindow window(fixture(), nullptr, files.filePath("text-library.json"));
    window.setAttribute(Qt::WA_DontShowOnScreen, true);
    window.resize(1400, 900); window.show(); QCoreApplication::processEvents();
    window.setMetricUnits(true);
    require(window.setActiveLayer("l"), "activate physical wall drawing context");

    // A distinct, declared exterior owner proves room naming does not enter appraisal GLA.
    const auto exterior = window.createBoundary({{{10, 0}, {15, 0}, 0}, {{15, 0}, {15, 5}, 0},
        {{15, 5}, {10, 5}, 0}, {{10, 5}, {10, 0}, 0}}, "living");
    require(!exterior.isEmpty() && window.selectEntity(exterior), "create independent exterior appraisal owner");
    auto* workflow = child<QComboBox>(window, "calculationWorkflow");
    workflow->setCurrentIndex(workflow->findData(QStringLiteral("appraisal")));
    require(window.editSelectedAppraisalFacts(QStringLiteral(R"({"appraisal_policy":{"policy_kind":"residential_declared","version":1,"property_kind":"detached_single_family","measurement_basis":"exterior"},"grade":"above","appraisal_facts":{"finish":"finished","access":"direct_interior","ceiling_eligibility":"standard","area_use":"dwelling","boundary_role":"measured_area"}})")),
            "exterior boundary has explicit qualifying declarations");
    const auto exterior_before = window.document().snapshot().entities().at(exterior.toStdString());
    const auto gla_before = child<QLabel>(window, "appraisalGlaTotal")->text();
    const auto property_gla_before = child<QLabel>(window, "appraisalDetailsGla")->text();
    require(gla_before.contains("25.00"), "independent declared exterior owner reports 25 square metres GLA");
    require(property_gla_before.contains("25.00"),"persistent property Details also report the qualified GLA");

    require(window.selectEntity("bottom"), "select real physical wall for room discovery");
    const auto before = window.document().snapshot();
    const auto ids = window.detectRoomBoundariesFromExistingWalls("office");
    if (ids.size() != 1) throw std::runtime_error("detect clear room: " + window.lastError().toStdString());
    const auto id = ids.front();
    const auto room = window.document().snapshot().entities().at(id.toStdString());
    require(room.type == "room_boundary" && is_physical_wall_room(room) &&
            room.properties.at("classification") == "office", "discovery creates a source-bound classified room owner");
    require(window.document().revision() == before.revision() + 1 &&
            !room.properties.contains("appraisal_facts") && !room.properties.contains("area_m2"),
            "clear room is one command without fabricated appraisal facts or claimed area");
    const auto check = physical_wall_room_checks(window.document().snapshot()).at(id.toStdString());
    require(check.current && check.diagnostic.empty() && check.holes.size() == 1,
            "current physical room query retains the isolated wall as one inline hole");
    assert_near(check.area_square_metres, 10.24);
    assert_near(std::abs(signed_area(check.boundary)), 10.64);
    assert_near(std::abs(signed_area(check.holes.front())), .4);
    const auto bounds = boundary_bounds(check.boundary);
    assert_near(bounds.minimum.x, .1); assert_near(bounds.minimum.y, .1);
    assert_near(bounds.maximum.x, 3.9); assert_near(bounds.maximum.y, 2.9);
    require(window.selectEntity("right"), "select another source wall before repeated detection");
    const auto retained = window.document().snapshot();
    require(window.detectRoomBoundariesFromExistingWalls("bedroom") == ids &&
            window.document().revision() == retained.revision() &&
            window.document().snapshot().entities() == retained.entities(),
            "repeat detection reuses room and cannot silently change office classification");
    require(window.undoCommand() && !window.document().snapshot().entities().contains(id.toStdString()),
            "Undo removes the complete source-bound room command");
    require(window.redoCommand() && window.document().snapshot().entities().at(id.toStdString()) == room,
            "Redo restores exact room identity and source descriptor");
    const auto furniture = window.createAnnotationSymbol("svg-v2-04_living-sofa-three-seat", {2.75, 2});
    if (furniture.isEmpty()) throw std::runtime_error("physical room furnishing: " + window.lastError().toStdString());
    require(window.selectEntity(id), "select clear room for actual canvas labels");
    auto& drawing = canvas(window);
    const auto* scene_room = geometry(drawing, id);
    require(scene_room && scene_room->holes.size() == 1, "actual plan scene preserves room hole geometry");
    require(label(drawing, id).contains("10.24 m²") && !label(drawing, id).contains("12.00"),
            "actual room area label reports current net clear area rather than centerline area");
    require(geometry(drawing, furniture), "actual bundled furniture symbol is visible within room scene");
    current_details(window);
    current_schedule(window, id);
    rejects_entity_only_dimension(room);
    require(window.createAreaDimension(id, {2, 2}).isEmpty(),
            "desktop cannot create misleading Entity-only source-bound area dimension");

    drawing.setOverviewMapEnabled(false);
    drawing.setTool(CanvasTool::select);
    drawing.setSelectionFilter(CanvasSelectionFilter::areas);
    drawing.setViewTransform({2, 1.5}, 150);
    require(window.selectEntity({}), "clear prior selection before hole picking");
    click(drawing, {2, 1});
    require(window.selectedEntityId() != id, "area picking excludes isolated physical wall hole");
    // An empty-canvas click intentionally starts the hybrid wall workflow.
    // Cancel that tentative wall before testing another independent pick.
    QKeyEvent escape(QEvent::KeyPress,Qt::Key_Escape,Qt::NoModifier);
    QApplication::sendEvent(&drawing,&escape); QCoreApplication::processEvents();
    require(drawing.areaIdsAt({.5,2}).contains(id),"clear room analytical interior is available for picking");
    click(drawing, {.5, 2});
    require(window.selectedEntityId() == id, "area picking includes clear room interior");
    drawing.setSelectionFilter(CanvasSelectionFilter::all);
    click(drawing,{2.75,2});
    require(window.selectedEntityId()==furniture,"room interior does not steal its furniture component pick");
    require(window.selectEntity(id),"return to source-bound room Details after component pick");
    capture(window, "physical-wall-room-white.png");
    window.setWorkspaceTheme(WorkspaceTheme::dark); QCoreApplication::processEvents();
    capture(window, "physical-wall-room-dark.png");
    window.setWorkspaceTheme(WorkspaceTheme::light);
    require(window.editSelectedClassification("bedroom"), "explicit room classification edit is available");
    require(window.selectEntity(exterior) && child<QLabel>(window, "appraisalGlaTotal")->text() == gla_before &&
            window.document().snapshot().entities().at(exterior.toStdString()) == exterior_before,
            "room discovery and classification leave exterior source and qualified GLA unchanged");

    const auto path = files.filePath("physical-room.bldproj");
    const auto saved = window.document().snapshot();
    require(window.saveProjectAs(path) && window.openProject(path) && window.selectEntity(id),
            "physical room saves and reopens through native project workflow");
    require(window.document().snapshot().entities() == saved.entities(), "save/reopen preserve room sources and inline hole");
    assert_near(physical_wall_room_checks(window.document().snapshot()).at(id.toStdString()).area_square_metres, 10.24);
    current_schedule(window, id);
    current_details(window);

    auto changed = window.document().snapshot().entities().at("bottom");
    changed.properties["thickness_m"] = .4;
    window.document().apply(ApplyEntityChanges{window.document().revision(),
        {EntityChange::upsert(changed)}, {}, "Change source wall thickness"});
    require(window.selectEntity(id), "refresh room after live physical source change");
    const auto stale = physical_wall_room_checks(window.document().snapshot()).at(id.toStdString());
    require(!stale.current && !stale.diagnostic.empty() && stale.boundary.empty() && stale.holes.empty(),
            "wall thickness change invalidates persisted room lineage instead of certifying old geometry");
    require(!geometry(drawing, id) && label(drawing, id).isEmpty(),
            "stale physical room suppresses old scene fill, hole and numeric label");
    require(child<QLabel>(window, "calculationNetArea")->text() == QStringLiteral("—") &&
            child<QLabel>(window, "calculationBaseArea")->text() == QStringLiteral("—") &&
            child<QLabel>(window, "calculationStatus")->text().contains(QString::fromStdString(stale.diagnostic)),
            "stale physical room Details suppress previous quantities and explain source repair");
    require(child<QLabel>(window, "planGeometryError")->text().contains(id),
            "stale physical room exposes actionable plan diagnostic");
    const auto schedules = window.scheduleSnapshot();
    require(!room_row(schedules, id) && std::any_of(schedules.diagnostics.begin(), schedules.diagnostics.end(),
                [&](const auto& diagnostic) { return diagnostic.find(id.toStdString()) != std::string::npos; }),
            "stale room schedule suppresses old numeric row and reports source diagnostic");
    rejects_entity_only_dimension(window.document().snapshot().entities().at(id.toStdString()));
    const auto stale_source=window.document().snapshot();
    review_room(window,[&](QDialog& dialog) {
        auto* preview=dynamic_cast<PlanCanvas*>(dialog.findChild<QWidget*>("physicalRoomRepairCanvas"));
        auto* apply=dialog.findChild<QPushButton*>("physicalRoomRepairApply");
        require(preview && apply && !apply->isEnabled(),"repair has a real preview and requires a picked interior");
        auto* correspondence=dialog.findChild<QLabel*>("physicalRoomRepairCorrespondence");
        require(correspondence && correspondence->text().contains("continues in one current space"),
            "repair displays analytical room correspondence without automatically choosing a destination");
        click(*preview,{2,1}); require(!apply->isEnabled(),"wall-material hole cannot become a repair destination");
        click(*preview,{2,2}); require(apply->isEnabled(),"actual canvas click chooses current room");
        auto* buttons=dialog.findChild<QDialogButtonBox*>(); require(buttons,"repair has cancellation controls");
        buttons->button(QDialogButtonBox::Cancel)->click();
    });
    require(window.document().snapshot().entities()==stale_source.entities() && window.document().revision()==stale_source.revision(),
        "cancel repair leaves exact stale source and revision untouched");
    // An accepted dialog can emit callbacks before its caller resumes. Retained
    // Retained history metadata is part of the captured source even when head
    // geometry, assets, identity and revision are unchanged.
    std::optional<DocumentSnapshot> expected_replacement;
    review_room(window,[&](QDialog& dialog) {
        auto* preview=dynamic_cast<PlanCanvas*>(dialog.findChild<QWidget*>("physicalRoomRepairCanvas"));
        auto* apply=dialog.findChild<QPushButton*>("physicalRoomRepairApply");
        require(preview && apply,"replacement review has real destination controls");
        click(*preview,{2,2}); require(apply->isEnabled(),"replacement review selects a current destination");
        QObject::connect(&dialog,&QDialog::accepted,&dialog,[&] {
            auto replaced=window.document().snapshot();
            auto& records=const_cast<std::vector<RevisionRecord>&>(replaced.history());
            records.back().action+=" (room review source replacement)";
            window.document()=Document::fork(replaced);
            expected_replacement=window.document().snapshot();
        });
        apply->click();
    });
    const auto replaced=window.document().snapshot();
    require(expected_replacement.has_value(),"accepted room review exercises a valid same-head retained-history replacement");
    require(replaced.document_id()==stale_source.document_id() && replaced.revision()==stale_source.revision() &&
        replaced.entities()==stale_source.entities() && replaced.assets()==stale_source.assets() &&
        expected_replacement && document_snapshot_digest(replaced)==document_snapshot_digest(*expected_replacement) &&
        replaced.history().back().action!=stale_source.history().back().action,
        "same-head replacement is preserved without committing a stale room repair");
    require(window.lastError().contains("review source changed"),"stale accepted room review explains its full-source refusal");
    window.document()=Document::fork(stale_source);
    require(window.selectEntity(id),"fresh room review can restart from the retained source");
    review_room(window,[&](QDialog& dialog) {
        auto* preview=dynamic_cast<PlanCanvas*>(dialog.findChild<QWidget*>("physicalRoomRepairCanvas"));
        auto* apply=dialog.findChild<QPushButton*>("physicalRoomRepairApply"); require(preview && apply,"repair controls exist");
        click(*preview,{2,2}); require(apply->isEnabled(),"repair destination selected");
        const auto directory=qEnvironmentVariable("VERTEX_TEST_CAPTURE_DIR");
        if (!directory.isEmpty()) require(dialog.grab().save(QDir(directory).filePath("physical-room-repair-preview.png")),"actual repair preview capture saves");
        apply->click();
    });
    const auto repaired=window.document().snapshot();
    require(repaired.revision()==stale_source.revision()+1 && repaired.entities().at(id.toStdString()).properties.at("classification")==
        stale_source.entities().at(id.toStdString()).properties.at("classification"),
        "repair retains the room identity and class in one revision");
    const auto repaired_check=physical_wall_room_checks(repaired).at(id.toStdString());
    require(repaired_check.current && repaired_check.holes.size()==1,"repair derives current physical room and hole"); assert_near(repaired_check.area_square_metres,9.86);
    if (repaired.entities().at(exterior.toStdString())!=exterior_before || child<QLabel>(window,"appraisalDetailsGla")->text()!=property_gla_before)
        throw std::runtime_error("room repair cannot change exterior appraisal owner or GLA: owner diff="+
            nlohmann::json::diff(exterior_before.properties,repaired.entities().at(exterior.toStdString()).properties).dump()+
            "; original GLA="+property_gla_before.toStdString()+"; current GLA="+child<QLabel>(window,"appraisalDetailsGla")->text().toStdString()+
            "; error="+window.lastError().toStdString());
    require(window.undoCommand() && window.document().snapshot().entities()==stale_source.entities(),"repair Undo restores retained stale evidence");
    require(window.redoCommand() && window.document().snapshot().entities()==repaired.entities(),"repair Redo restores exact reviewed destination");
    require(window.saveProjectAs(files.filePath("repaired-room.bldproj")) && window.openProject(files.filePath("repaired-room.bldproj")) && window.selectEntity(id),
        "native repaired room saves and reopens");
    require(window.document().snapshot().entities()==repaired.entities() && physical_wall_room_checks(window.document().snapshot()).at(id.toStdString()).current,
        "reopened repair replays source authority and retains identity");
    require(window.undoCommand() && window.document().snapshot().entities()==stale_source.entities(),"reopened repair Undo restores prior source evidence");
    require(window.undoCommand() && physical_wall_room_checks(window.document().snapshot()).at(id.toStdString()).current &&
            geometry(drawing, id) && label(drawing, id).contains("10.24 m²"),
            "Undo source edit repairs room projection and derived area");
    current_details(window);
}
} // namespace
int main(int argc, char** argv) {
    sketch::testing::noninteractive_errors();
    QStandardPaths::setTestModeEnabled(true);
    QApplication application(argc, argv);
    try {
        require(QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/Inter.ttf")) >= 0,
                "bundled Inter is available for actual native captures");
        application.setFont(QFont(QStringLiteral("Inter"), 10));
        workflow(); std::cout << "Physical wall room desktop checks passed\n"; return 0;
    } catch (const std::exception& error) {
        std::cerr << "physical_wall_room_desktop_tests: " << error.what() << '\n'; return 1;
    }
}
