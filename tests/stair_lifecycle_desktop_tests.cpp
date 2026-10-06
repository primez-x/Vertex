#include "sketch/desktop/main_window.hpp"
#include "sketch/desktop/building_object_dialog.hpp"
#include "sketch/building_plan_projection.hpp"
#include "sketch/document_digest.hpp"
#include "sketch/model_phases.hpp"
#include "sketch/project_organization.hpp"
#include "sketch/project_store.hpp"
#include "sketch/vertical_level_document_adapter.hpp"
#include "sketch/visualization/native_geometry_preparation.hpp"
#include "support/noninteractive_errors.hpp"
#include "../src/desktop/plan_canvas.hpp"

#include <BRepBndLib.hxx>
#include <Bnd_Box.hxx>
#include <QApplication>
#include <QAbstractButton>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QElapsedTimer>
#include <QLineEdit>
#include <QPushButton>
#include <QStandardPaths>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QUuid>
#include <algorithm>
#include <array>
#include <cmath>
#include <exception>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
using namespace sketch;
using namespace sketch::desktop;
using Json = nlohmann::json;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void require_near(double actual, double expected) {
    require(std::isfinite(actual) && std::abs(actual - expected) < 1e-7, "derived geometry differs from analytical expectation");
}
template<class T> T& control(QWidget& owner, const char* name) {
    auto* result = owner.findChild<T*>(name);
    if (!result) throw std::runtime_error(std::string("actual stair lifecycle control must exist: ") + name);
    return *result;
}
void choose(QComboBox& box, const QString& id) {
    const auto index = box.findData(id); require(index >= 0, "stable source/form must be offered");
    box.setCurrentIndex(index); QApplication::processEvents();
}
void field(QWidget& dialog, const char* name, const char* value) {
    control<QLineEdit>(dialog, name).setText(QString::fromLatin1(value));
}
void form(QWidget& dialog, const char* type, const char* kind) {
    choose(control<QComboBox>(dialog, "buildingObjectType"), QString::fromLatin1(type));
    choose(control<QComboBox>(dialog, "buildingObjectForm"), QString::fromLatin1(kind));
}
void unchanged(const DocumentSnapshot& before, const DocumentSnapshot& after) {
    require(document_snapshot_digest(before) == document_snapshot_digest(after),
            "refusal must preserve full identity, history, revision, entities and assets");
}
void atomic(MainWindow& window, const DocumentSnapshot& before, const DocumentSnapshot& after, unsigned format_floor = 52) {
    require(after.revision() == before.revision() + 1 && after.history().size() == before.history().size() + 1,
            "coordinated lifecycle edit must use one command and revision");
    require(after.assets() == before.assets(), "lifecycle command must preserve asset bytes and metadata");
    require(window.undoCommand(), "lifecycle edit must undo");
    require(window.document().snapshot().entities() == before.entities() && window.document().snapshot().assets() == before.assets(),
            "Undo must restore the exact source maps");
    const auto undone = window.document().snapshot();
    require(undone.revision() == after.revision() + 1 && undone.history().size() == after.history().size() + 1 &&
            undone.history().back().action == "undo" && undone.history().back().source_revision == before.revision(),
            "Undo must append one navigation record targeting the exact source revision");
    require(ProjectStore::required_format_version(undone) == format_floor,
            "undone semantic content must retain its exact format floor in history");
    require(window.redoCommand(), "lifecycle edit must redo");
    const auto redone = window.document().snapshot();
    require(redone.document_id() == after.document_id() && redone.entities() == after.entities() && redone.assets() == after.assets(),
            "Redo must restore exact committed entity and asset maps");
    require(redone.revision() == after.revision() + 2 && redone.history().size() == after.history().size() + 2 &&
            redone.history().back().action == "redo" && redone.history().back().source_revision == after.revision(),
            "Redo must append one navigation record targeting the exact committed revision");
}
void context(Entity& entity) {
    entity.properties.update(Json{{"property_id", "p"}, {"building_id", "b"}, {"floor_id", "f"}, {"layer_id", "l"}});
    entity.properties["vertical_placement"] = {{"version", 1}, {"mode", "level"}, {"offset_m", 0.4}};
}
Entity legacy_stair() {
    StairFlight stair{"legacy", {10, 0, 0.25}, 0, 10, 3, 0.3, 1.2,
        StairLanding{1.2, 0.15}, StairLevelConnection{"levels", "link", "lower", "upper"}};
    auto entity = encode_building_entity(stair, {{"name", "Hall stairs"}, {"vendor", {{"literal", 42}}}});
    context(entity);
    entity.properties["name"] = "Named legacy stairs";
    entity.properties["future_host"] = {{"opaque", {3, 7}}};
    entity.properties["top_landing"]["vendor"] = "retain landing";
    entity.properties["level_connection"]["vendor"] = "retain connection";
    entity.properties["quantity_entries"] = {
        {"/going_m", {{"version", 1}, {"original_expression", "300 mm"}, {"entered_unit", "mm"},
            {"exact_metres", {{"numerator", 3}, {"denominator", 10}}}}},
        {"/future_dimension_m", {{"version", 9}, {"opaque", "receipt"}}}};
    return entity;
}
std::shared_ptr<Document> fixture(bool legacy = false, bool phases = false) {
    const VerticalLevelGraph graph({{"lower", 5}, {"upper", 8}}, {{"link", "lower", "upper"}});
    std::vector<Entity> entities{{"p", "property", Json::object(), false},
        {"b", "building", {{"property_id", "p"}}, false},
        {"f", "floor", {{"building_id", "b"}, {"vertical_level_binding", VerticalLevelBinding{"levels", "lower"}.to_json()}}, false},
        {"l", "layer", {{"floor_id", "f"}}, false},
        {"other-floor", "floor", {{"building_id", "b"}}, false},
        {"other-layer", "layer", {{"floor_id", "other-floor"}}, false},
        {"levels", "vertical_levels", {{"model", Json::parse(graph.serialize())}}, false}};
    if (legacy) entities.push_back(legacy_stair());
    if (phases) entities.push_back({"phases", "model_phases", {{"model", ModelPhases::create(
        {"legacy"}, {"legacy"}, {{"remove", "Remove stairs", {"legacy"}, {}}, {"keep", "Keep stairs", {}, {}}}).to_json()}}, false});
    return std::make_shared<Document>(Document::create(std::move(entities),
        {Asset::create("opaque-asset", "application/octet-stream", {std::byte{3}, std::byte{7}}, {{"vendor", "retain"}})}));
}
void prepare(MainWindow& window, bool activate_fixture_layer = true) {
    window.setAttribute(Qt::WA_DontShowOnScreen); window.resize(1200, 800);
    window.setMetricUnits(true); window.setWorkspace(Workspace::architectural);
    window.setNativeModelViewVisible(false); window.show(); QApplication::processEvents();
    if (activate_fixture_layer) require(window.setActiveLayer("l"), "fixture drawing layer must be active");
}
void capture(QWidget& widget, const char* name) {
    const auto directory = qEnvironmentVariable("VERTEX_TEST_CAPTURE_DIR");
    if (directory.isEmpty()) return;
    require(QDir().mkpath(directory) && widget.grab().save(QDir(directory).filePath(QString::fromLatin1(name))),
            "actual offscreen editor/plan capture must save");
}
void modal(MainWindow& window, const char* button, const std::function<void(BuildingObjectDialog&)>& callback) {
    bool opened = false; std::exception_ptr failure;
    QTimer::singleShot(0, &window, [&] {
        auto* dialog = dynamic_cast<BuildingObjectDialog*>(window.findChild<QDialog*>("buildingObjectDialog"));
        try {
            require(dialog, "MainWindow must open the actual building object dialog");
            opened = true; dialog->setAttribute(Qt::WA_DontShowOnScreen); callback(*dialog);
        } catch (...) { failure = std::current_exception(); }
        if (dialog && dialog->result() != QDialog::Accepted) dialog->reject();
    });
    auto& action = control<QAbstractButton>(window, button);
    require(action.isEnabled(), "actual create/edit action must be enabled"); action.click();
    if (failure) std::rethrow_exception(failure);
    require(opened, "actual modal callback must execute");
}
StairFlight stair(const DocumentSnapshot& source, const std::string& id) {
    return decode_stair_properties(id, source.entities().at(id).properties);
}
std::string only_id(const DocumentSnapshot& source, const char* type) {
    std::string result;
    for (const auto& [id, entity] : source.entities()) if (entity.type == type) {
        require(result.empty(), "fixture must contain one entity of requested type"); result = id;
    }
    require(!result.empty(), "created semantic entity must exist"); return result;
}
void ready(MainWindow& window) {
    QElapsedTimer timer; timer.start();
    while (!window.regenerationReadyForCurrentRevision() && timer.elapsed() < 15000) {
        QApplication::processEvents(); QThread::msleep(1);
    }
    require(window.regenerationReadyForCurrentRevision(), "actual desktop 3D preparation must refresh for current revision");
}
std::array<double, 6> bounds(const TopoDS_Shape& shape) {
    Bnd_Box box; BRepBndLib::Add(shape, box); require(!box.IsVoid(), "real prepared solid must have bounds");
    std::array<double, 6> values{}; box.Get(values[0], values[1], values[2], values[3], values[4], values[5]); return values;
}
visualization::PreparedNativeGeometry geometry(MainWindow& window) {
    ready(window);
    auto result = visualization::prepare_native_geometry(window.document().snapshot(), std::nullopt);
    require(result && result->errors.empty() && result->pending.empty(), "same authoritative snapshot must prepare complete 3D geometry");
    return std::move(*result);
}
std::array<double, 4> plan_bounds(const Boundary& segments) {
    require(!segments.empty(), "actual plan object must contain projected edges");
    std::array<double, 4> result{std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity()};
    for (const auto& segment : segments) for (const auto point : {segment.start, segment.end}) {
        result[0] = std::min(result[0], point.x); result[1] = std::min(result[1], point.y);
        result[2] = std::max(result[2], point.x); result[3] = std::max(result[3], point.y);
    }
    return result;
}
std::array<double, 4> actual_plan(MainWindow& window, const std::string& id) {
    auto* canvas = dynamic_cast<PlanCanvas*>(&control<QWidget>(window, "architecturalPlanCanvas"));
    require(canvas, "actual architectural canvas must exist");
    const auto found = std::find_if(canvas->entities().begin(), canvas->entities().end(),
        [&](const auto& entity) { return entity.id.toStdString() == id; });
    require(found != canvas->entities().end(), "hosted rail must appear in actual plan geometry");
    const auto source = window.document().snapshot();
    const auto projected = project_building_plan(decode_building_entity(source.entities().at(id)), source.entities());
    const auto actual = plan_bounds(found->segments), expected = plan_bounds(projected);
    for (std::size_t i = 0; i < actual.size(); ++i) require_near(actual[i], expected[i]);
    return actual;
}
void connected_controls(BuildingObjectDialog& dialog) {
    control<QCheckBox>(dialog, "buildingObjectLevelConnectionEnabled").setChecked(true);
    field(dialog, "buildingObjectLevelGraph", "levels"); field(dialog, "buildingObjectLevelLink", "link");
    field(dialog, "buildingObjectLowerLevel", "lower"); field(dialog, "buildingObjectUpperLevel", "upper");
}
void multiflight_creation_and_property_edit() {
    MainWindow window(fixture()); prepare(window); const auto initial = window.document().snapshot();
    modal(window, "createBuildingObject", [&](BuildingObjectDialog& dialog) {
        form(dialog, "stair", "multi_flight_stair");
        control<QTableWidget>(dialog, "buildingObjectStairFlights").item(0, 0)->setText("5");
        control<QPushButton>(dialog, "buildingObjectAddStairFlight").click();
        control<QTableWidget>(dialog, "buildingObjectStairFlights").item(1, 0)->setText("7");
        auto& landings = control<QTableWidget>(dialog, "buildingObjectStairLandings");
        landings.item(0, 0)->setText("1500 mm");
        auto* turn = qobject_cast<QComboBox*>(landings.cellWidget(0, 2)); require(turn, "landing must have actual turn control");
        turn->setCurrentIndex(turn->findData(static_cast<int>(StairTurn::left_quarter)));
        field(dialog, "buildingObjectTotalRise", "3 m"); field(dialog, "buildingObjectGoing", "300 mm");
        control<QCheckBox>(dialog, "buildingObjectLandingEnabled").setChecked(true);
        field(dialog, "buildingObjectLandingDepth", "1200 mm"); connected_controls(dialog);
        capture(dialog, "stair-multiflight-creator.png");
        control<QPushButton>(dialog, "buildingObjectSubmit").click();
        require(dialog.result() == QDialog::Accepted, "actual multi-flight creator must accept valid topology");
    });
    const auto created = window.document().snapshot(); const auto id = only_id(created, "stair");
    require(stair(created, id).flights.size() == 2 && stair(created, id).riser_count == 12,
            "actual create button must commit v2 ordered flights");
    require(ProjectStore::required_format_version(created) == 51, "multi-flight stair lifecycle requires native format 51");
    atomic(window, initial, created, 51);
    auto named = created.entities().at(id);
    named.properties["name"] = "Return stairs"; named.extensions["vendor"] = {{"opaque", "retained"}};
    named.properties["flights"][0]["future_child"] = {{"payload", {1, 3}}};
    named.properties["landings"][0]["future_child"] = "landing";
    named.properties["top_landing"]["future_child"] = "top landing";
    named.properties["level_connection"]["future_child"] = "level link";
    window.document().apply(ApplyEntityChanges{window.document().revision(), {EntityChange::upsert(named)}, {}, "opaque stair fixture"});
    require(window.selectEntity(QString::fromStdString(id)), "new multi-flight stair must be selectable");
    const auto before_edit = window.document().snapshot();
    modal(window, "editBuildingObject", [&](BuildingObjectDialog& dialog) {
        field(dialog, "buildingObjectWidth", "1500 mm"); field(dialog, "buildingObjectGoing", "350 mm");
        capture(dialog, "stair-multiflight-properties.png");
        control<QPushButton>(dialog, "buildingObjectSubmit").click();
        require(dialog.result() == QDialog::Accepted, "actual property dialog must accept edited dimensions");
    });
    const auto edited = window.document().snapshot(); const auto& result = edited.entities().at(id);
    require(result.id == named.id && result.properties.at("name") == "Return stairs" && result.extensions == named.extensions,
            "dimension edit must retain selected stair identity, name and opaque metadata");
    for (const auto* key : {"flights", "landings", "top_landing", "level_connection"})
        require(result.properties.at(key) == named.properties.at(key), "dimension edit must preserve child identity, unknown fields and retained connections");
    require_near(stair(edited, id).width, 1.5); require_near(stair(edited, id).going, 0.35);
    atomic(window, before_edit, edited, 51);
    require(window.selectEntity(QString::fromStdString(id)), "edited multi-flight stair remains selectable");
    ready(window); capture(control<QWidget>(window, "architecturalPlanCanvas"), "stair-multiflight-plan.png");
}
void hosted_upgrade_geometry_failure_and_roundtrip() {
    auto document = fixture(true);
    auto shorthand = document->snapshot().entities().at("legacy");
    shorthand.properties.erase("property_id"); shorthand.properties.erase("building_id");
    document->apply(ApplyEntityChanges{document->revision(), {EntityChange::upsert(shorthand)}, {}, "legacy authored organization shorthand"});
    MainWindow window(document); prepare(window); const auto before = window.document().snapshot();
    BuildingObjectDialog creator(before, std::nullopt, true);
    form(creator, "railing", "stair_flight_railing");
    choose(control<QComboBox>(creator, "buildingObjectStairHost"), "legacy");
    field(creator, "buildingObjectHeight", "900 mm"); field(creator, "buildingObjectPostSpacing", "500 mm");
    require(creator.submit() && creator.candidate() && creator.relatedCandidates().size() == 1,
            "source-aware rail creator must return explicit one-flight host upgrade");
    const auto spacing_receipt = creator.candidate()->properties.at("quantity_entries").at("/post_spacing_m");
    require(spacing_receipt == Json{{"version", 1}, {"original_expression", "500 mm"}, {"entered_unit", "mm"},
                {"exact_metres", {{"numerator", 1}, {"denominator", 2}}}},
            "rail creator must record exact entered post spacing before controller admission");
    for (int failure = 0; failure < 3; ++failure) {
        auto forged = *creator.candidate();
        auto& receipt = forged.properties["quantity_entries"]["/post_spacing_m"];
        if (failure == 0) receipt["original_expression"] = "600 mm";
        if (failure == 1) receipt["exact_metres"]["numerator"] = 3;
        if (failure == 2) receipt["entered_unit"] = "m";
        require(window.commitBuildingObject(forged, before.revision(), false, creator.relatedCandidates()).isEmpty(),
                "forged post spacing receipts must refuse the rail and related host upgrade");
        require(window.lastError().contains("/post_spacing_m"), "spacing receipt refusal must identify its field");
        unchanged(before, window.document().snapshot());
    }
    const auto rail_id = window.commitBuildingObject(*creator.candidate(), before.revision(), false, creator.relatedCandidates());
    if (rail_id.isEmpty())
        throw std::runtime_error("rail and explicit v1 host upgrade must commit atomically: " +
                                 window.lastError().toStdString());
    auto created = window.document().snapshot(); const auto& old_host = before.entities().at("legacy");
    const auto upgraded = created.entities().at("legacy");
    auto expected = old_host;
    for (const auto* key : {"version", "form", "flights", "landings"}) expected.properties[key] = upgraded.properties.at(key);
    expected.properties["property_id"] = "p"; expected.properties["building_id"] = "b";
    require(upgraded == expected, "v1 upgrade must preserve every old field, unknown nested field and quantity receipt");
    const auto old_layout = derive_stair_layout(stair(before, "legacy")), layout = derive_stair_layout(stair(created, "legacy"));
    for (std::size_t i = 0; i < old_layout.flights.front().treads.size(); ++i) for (std::size_t corner = 0; corner < 4; ++corner) {
        const auto a = old_layout.flights.front().treads[i].footprint[corner], b = layout.flights.front().treads[i].footprint[corner];
        require_near(a.x, b.x); require_near(a.y, b.y); require_near(a.z, b.z);
    }
    const auto& rail = created.entities().at(rail_id.toStdString());
    require(rail.properties.at("quantity_entries").at("/post_spacing_m") == spacing_receipt,
            "atomic hosted rail creation must retain the validated post spacing receipt exactly");
    const auto decoded = decode_railing_properties(rail.id, rail.properties);
    require(decoded.host && decoded.host->flight_id == stair(created, "legacy").flights.front().id,
            "rail must own an exact stable flight reference");
    for (const auto* key : {"property_id", "building_id", "floor_id", "layer_id"})
        require(rail.properties.at(key) == upgraded.properties.at(key), "rail context must derive from current host");
    for (const auto* key : {"vertical_placement", "base_position_m", "orientation_rad", "length_m"})
        require(!rail.properties.contains(key), "hosted rail must have no independent world/level placement");
    atomic(window, before, created);
    require(window.selectEntity(rail_id), "owned-flight rail must be selectable");
    const auto before_rail_edit = window.document().snapshot();
    modal(window, "editBuildingObject", [&](BuildingObjectDialog& dialog) {
        require(!dialog.findChild<QLineEdit*>("buildingObjectBaseX") && !dialog.findChild<QLineEdit*>("buildingObjectLength"),
                "actual hosted property editor must expose only host-derived placement");
        field(dialog, "buildingObjectHeight", "1000 mm");
        control<QPushButton>(dialog, "buildingObjectSubmit").click();
        require(dialog.result() == QDialog::Accepted, "owned-flight property edit must accept");
    });
    created = window.document().snapshot(); atomic(window, before_rail_edit, created);
    require(created.entities().at("legacy") == upgraded, "rail dimension edit must preserve upgraded host byte-for-byte");
    require(created.entities().at(rail_id.toStdString()).properties.at("quantity_entries").at("/post_spacing_m") == spacing_receipt,
            "editing another rail dimension must preserve its exact post spacing receipt");
    // Each failure goes through the real controller; none may partially alter history or geometry.
    for (int failure = 0; failure < 4; ++failure) {
        require(window.selectEntity(failure == 2 ? "legacy" : rail_id), "select exact refused edit source");
        const auto source = window.document().snapshot();
        auto candidate = source.entities().at(failure == 2 ? "legacy" : rail_id.toStdString());
        if (failure == 0) candidate.properties["host"]["flight_id"] = "dangling-flight";
        if (failure == 1) {
            candidate.properties["floor_id"] = "other-floor"; candidate.properties["layer_id"] = "other-layer";
        }
        if (failure == 2) {
            candidate.properties["flights"].push_back(candidate.properties.at("flights").front());
            candidate.properties["riser_count"] = 20;
            candidate.properties["landings"].push_back({{"id", "duplicate-test-landing"}, {"depth_m", 1.2},
                {"thickness_m", 0.15}, {"turn", "straight"}, {"return_gap_m", 0}});
        }
        if (failure == 3) candidate.properties["vertical_placement"] = {{"version", 1}, {"mode", "absolute"}};
        if (failure == 1 || failure == 3) {
            // The property editor preserves organization and strips independent
            // rail placement. Exercise the authoritative Document boundary for
            // malformed adapter/import input rather than an editor-normalized copy.
            bool refused = false;
            try { window.document().apply(ApplyEntityChanges{source.revision(), {EntityChange::upsert(candidate)}, {}, "invalid attachment fixture"}); }
            catch (const std::exception&) { refused = true; }
            require(refused, "conflicting host context and independent rail placement must refuse atomically");
        } else require(window.commitBuildingObject(candidate, source.revision(), true).isEmpty(),
                "dangling flight and duplicate child edits must refuse");
        unchanged(source, window.document().snapshot());
    }
    const auto initial_geometry = geometry(window); const auto initial_bounds = bounds(initial_geometry.solids.at(rail_id.toStdString()).shape);
    const auto initial_plan = actual_plan(window, rail_id.toStdString());
    const auto before_move = window.document().snapshot();
    require(window.selectEntity("legacy") && window.transformSelectedArchitecturalObject("0", "2 m", "3 m", "0 m", "1", false),
            "real semantic host move must succeed");
    const auto moved = window.document().snapshot();
    require(moved.entities().at(rail_id.toStdString()) == created.entities().at(rail_id.toStdString()), "host move must retain exact rail authoring entity");
    atomic(window, before_move, moved);
    const auto moved_geometry = geometry(window); const auto moved_bounds = bounds(moved_geometry.solids.at(rail_id.toStdString()).shape);
    require(moved_geometry.solids.at(rail_id.toStdString()).content != initial_geometry.solids.at(rail_id.toStdString()).content,
            "host move must invalidate hosted native geometry content");
    for (std::size_t i = 0; i < 6; ++i) require_near(moved_bounds[i], initial_bounds[i] + (i % 3 == 0 ? 2 : i % 3 == 1 ? 3 : 0));
    const auto moved_plan = actual_plan(window, rail_id.toStdString());
    for (std::size_t i = 0; i < 4; ++i) require_near(moved_plan[i], initial_plan[i] + (i % 2 == 0 ? 2 : 3));
    const auto before_level = window.document().snapshot();
    const auto graph = VerticalLevelGraph::from_json(before_level.entities().at("levels").properties.at("model"));
    const auto candidate = prepare_vertical_level_edit(before_level, "levels", graph.with_elevation("upper", 9).with_elevation("lower", 5.5));
    const auto receipt = apply_vertical_level_edit(window.document(), candidate);
    require(receipt.affected_stairs.size() == 1 && receipt.affected_stairs.front().stair_id == "legacy",
            "level transaction must report the connected v2 host");
    require(window.selectEntity("legacy"), "refresh real desktop after typed level edit");
    const auto raised = window.document().snapshot(); require_near(stair(raised, "legacy").total_rise, 3.5);
    require(raised.entities().at(rail_id.toStdString()) == moved.entities().at(rail_id.toStdString()), "level edit must preserve authored rail entity");
    atomic(window, before_level, raised);
    const auto raised_geometry = geometry(window); const auto raised_bounds = bounds(raised_geometry.solids.at(rail_id.toStdString()).shape);
    require(raised_geometry.solids.at(rail_id.toStdString()).content != moved_geometry.solids.at(rail_id.toStdString()).content &&
            raised_bounds[2] > moved_bounds[2] + 0.49 && raised_bounds[5] > moved_bounds[5] + 0.5,
            "host level and rise edit must invalidate and raise actual hosted 3D geometry");
    (void)actual_plan(window, rail_id.toStdString());
    capture(control<QWidget>(window, "architecturalPlanCanvas"), "stair-hosted-rail-moved-plan.png");
    QTemporaryDir directory; require(directory.isValid(), "isolated stair archive directory");
    const auto path = directory.filePath("stairs.bldproj"); const auto saved = window.document().snapshot();
    require(window.saveProjectAs(path) && window.createNewProject(), "save stair lifecycle and release writer lease");
    MainWindow reopened; prepare(reopened, false); require(reopened.openProject(path) && reopened.document().is_editable(), "current stair archive must reopen editable");
    reopened.setWorkspace(Workspace::architectural); require(reopened.setActiveLayer("l"), "reopened fixture layer");
    const auto restored = reopened.document().snapshot();
    require(restored.entities() == saved.entities() && restored.assets() == saved.assets() && restored.history().size() == saved.history().size(),
            "save/reopen must retain exact stair, attachment, assets and command history");
    require(ProjectStore::required_format_version(restored) == 52, "saved lifecycle must retain native format 52");
    require(reopened.undoCommand() && reopened.document().snapshot().entities() == moved.entities() && reopened.redoCommand() &&
            reopened.document().snapshot().entities() == saved.entities(), "reopened UndoRedo must restore exact level and host maps");
    (void)actual_plan(reopened, rail_id.toStdString());
    const auto reopened_bounds = bounds(geometry(reopened).solids.at(rail_id.toStdString()).shape);
    for (std::size_t i = 0; i < 6; ++i) require_near(reopened_bounds[i], raised_bounds[i]);
}
void inactive_demolition_membership() {
    MainWindow window(fixture(true, true)); prepare(window); const auto source = window.document().snapshot();
    BuildingObjectDialog creator(source, std::nullopt, true); form(creator, "railing", "stair_flight_railing");
    choose(control<QComboBox>(creator, "buildingObjectStairHost"), "legacy"); require(creator.submit(), "phased host rail candidate");
    const auto id = window.commitBuildingObject(*creator.candidate(), source.revision(), false, creator.relatedCandidates());
    require(!id.isEmpty(), "baseline rail must commit while host is demolished in inactive alternative");
    const auto result = window.document().snapshot(); const auto phases = ModelPhases::from_json(result.entities().at("phases").properties.at("model"));
    require(phases.state(std::nullopt).at(id.toStdString()) == ModelPhase::existing &&
            phases.state(std::optional<std::string>{"remove"}).at(id.toStdString()) == ModelPhase::demolished &&
            phases.state(std::optional<std::string>{"keep"}).at(id.toStdString()) == ModelPhase::existing,
            "rail must mirror baseline and demolition membership in every alternative");
    atomic(window, source, result);
    require(window.selectRemodelingAlternative("remove") && !window.entityVisible("legacy") && !window.entityVisible(id),
            "selecting inactive demolition alternative must hide host and rail together");
    require(window.selectRemodelingAlternative("keep") && window.entityVisible("legacy") && window.entityVisible(id),
            "retained alternative must show coherent host and rail together");
}
std::string added_id(const DocumentSnapshot& before, const DocumentSnapshot& after, const char* type) {
    std::string result;
    for (const auto& [id, entity] : after.entities()) if (entity.type == type && !before.entities().contains(id)) {
        require(result.empty(), "clipboard command must create exactly one requested semantic owner"); result = id;
    }
    require(!result.empty(), "clipboard command must create requested semantic owner"); return result;
}
void clipboard_lifecycle() {
    MainWindow window(fixture(true)); prepare(window);
    BuildingObjectDialog creator(window.document().snapshot(), std::nullopt, true);
    form(creator, "railing", "stair_flight_railing");
    choose(control<QComboBox>(creator, "buildingObjectStairHost"), "legacy"); require(creator.submit(), "clipboard attachment fixture");
    const auto original_rail = window.commitBuildingObject(*creator.candidate(), window.document().revision(), false, creator.relatedCandidates());
    require(!original_rail.isEmpty(), "clipboard fixture must own a valid rail and flight");
    auto source = window.document().snapshot(); auto host = source.entities().at("legacy"); auto rail = source.entities().at(original_rail.toStdString());
    host.properties["flights"][0]["riser_count"] = 4;
    host.properties["flights"][0]["future_child"] = {{"literal_id", "legacy"}, {"payload", {2, 5}}};
    host.properties["flights"].push_back({{"id", "clipboard-upper"}, {"riser_count", 6}, {"future_child", "upper flight"}});
    host.properties["landings"].push_back({{"id", "clipboard-turn"}, {"depth_m", 1.2}, {"thickness_m", 0.15},
        {"turn", "left_quarter"}, {"return_gap_m", 0}, {"future_child", {{"literal_id", "legacy"}, {"payload", "turn"}}}});
    rail.properties["host"]["future_anchor"] = {{"literal_id", "legacy"}, {"payload", "opaque"}};
    rail.extensions["vendor"] = "clipboard rail";
    window.document().apply(ApplyEntityChanges{source.revision(), {EntityChange::upsert(host), EntityChange::upsert(rail)}, {}, "opaque clipboard fixture"});
    require(window.selectEntity("legacy"), "copy must select the exact stair root");
    source = window.document().snapshot(); require(window.copySelection(), "actual stair graph copy must succeed");
    unchanged(source, window.document().snapshot());
    if (!window.pasteSelection())
        throw std::runtime_error("actual stair graph paste must create stair plus hosted rail: " +
                                 window.lastError().toStdString());
    const auto pasted = window.document().snapshot(); const auto clone_host = added_id(source, pasted, "stair"), clone_rail = added_id(source, pasted, "railing");
    const auto& copied_host = pasted.entities().at(clone_host); const auto& copied_rail = pasted.entities().at(clone_rail);
    for (const auto* key : {"property_id", "building_id", "floor_id", "layer_id"}) {
        require(copied_host.properties.at(key) == host.properties.at(key),
                "pasted stair must carry complete explicit destination organization");
        require(copied_rail.properties.at(key) == copied_host.properties.at(key),
                "pasted hosted rail must inherit every organization reference from its copied host");
    }
    const auto old_stair = stair(source, "legacy"), new_stair = stair(pasted, clone_host);
    require(new_stair.flights.size() == old_stair.flights.size() && new_stair.flights.front().id != old_stair.flights.front().id &&
            new_stair.flights.front().id != clone_host && !pasted.entities().contains(new_stair.flights.front().id),
            "paste must allocate fresh separately owned stable child identities");
    const auto old_children = stair_child_ids(old_stair);
    for (const auto* key : {"flights", "landings"}) {
        require(copied_host.properties.at(key).size() == host.properties.at(key).size(), "clipboard topology must retain every ordered child");
        for (std::size_t i = 0; i < host.properties.at(key).size(); ++i) {
            auto expected_child = host.properties.at(key).at(i); const auto new_id = copied_host.properties.at(key).at(i).at("id").get<std::string>();
            require(std::find(old_children.begin(), old_children.end(), new_id) == old_children.end() && !pasted.entities().contains(new_id),
                    "every copied flight and landing must receive a fresh child identity");
            expected_child["id"] = new_id;
            require(copied_host.properties.at(key).at(i) == expected_child, "clipboard must preserve all ordered child unknown metadata verbatim");
        }
    }
    require(copied_host.extensions == host.extensions &&
            copied_host.properties.at("top_landing") == host.properties.at("top_landing") &&
            copied_host.properties.at("level_connection") == host.properties.at("level_connection"),
            "clipboard remap must preserve child unknowns, top landing, connection and metadata verbatim");
    const auto copied_attachment = decode_railing_properties(clone_rail, copied_rail.properties);
    require(copied_attachment.host && copied_attachment.host->stair_id == clone_host &&
            copied_attachment.host->flight_id == new_stair.flights.front().id &&
            copied_rail.properties.at("host").at("future_anchor") == rail.properties.at("host").at("future_anchor") &&
            copied_rail.extensions == rail.extensions && !copied_rail.properties.contains("vertical_placement"),
            "paste must remap only typed host/flight IDs and derive the copied rail placement from its copied host");
    require(pasted.entities().at("legacy") == host && pasted.entities().at(original_rail.toStdString()) == rail,
            "copy/paste must preserve exact original owners");
    atomic(window, source, pasted);
    require(window.selectEntity(QString::fromStdString(clone_host)), "cut must select exact copied stair root");
    const auto before_cut = window.document().snapshot(); require(window.cutSelection(), "actual cut must remove copied stair and its dependent rail");
    const auto cut = window.document().snapshot();
    require(!cut.entities().contains(clone_host) && !cut.entities().contains(clone_rail) &&
            cut.entities().at("legacy") == host && cut.entities().at(original_rail.toStdString()) == rail,
            "cut must cascade only the selected host graph");
    atomic(window, before_cut, cut);
    require(window.selectEntity(original_rail) && window.copySelection(), "standalone owned rail copy must succeed");
    const auto before_rail_paste = window.document().snapshot();
    if (!window.pasteSelection())
        throw std::runtime_error("standalone owned rail paste must retain its current host: " +
                                 window.lastError().toStdString());
    const auto rail_pasted = window.document().snapshot(); const auto solo_id = added_id(before_rail_paste, rail_pasted, "railing");
    require(rail_pasted.entities().at(solo_id).properties.at("host") == rail.properties.at("host") &&
            !rail_pasted.entities().at(solo_id).properties.contains("vertical_placement") &&
            rail_pasted.entities().at("legacy") == host,
            "standalone rail paste must retain exact current host/flight and opaque anchor metadata");
    atomic(window, before_rail_paste, rail_pasted);
}

void landing_guard_full_controller_lifecycle() {
    auto document = fixture(true, true);
    const auto legacy_source = document->snapshot();
    auto canonical_host = legacy_source.entities().at("legacy");
    auto topology = stair(legacy_source, "legacy");
    topology.level_connection.reset();
    topology.flights = {{"landing-lower", 4}, {"landing-upper", 6}};
    topology.landings = {{"landing-turn", 1.2, 0.15, StairTurn::left_quarter, 0}};
    canonical_host.properties.update(encode_stair_properties(topology));
    canonical_host.properties.erase("level_connection");
    canonical_host.properties["landings"][0]["vendor"] = {{"literal", "legacy"}};
    document->apply(ApplyEntityChanges{legacy_source.revision(), {EntityChange::upsert(canonical_host)}, {}, "canonical landing fixture"});
    MainWindow window(document); prepare(window);
    std::array<std::string, 2> rails;
    for (std::size_t role = 0; role < rails.size(); ++role) {
        const auto before = window.document().snapshot();
        modal(window, "createBuildingObject", [&](BuildingObjectDialog& dialog) {
            form(dialog, "railing", "stair_landing_railing");
            choose(control<QComboBox>(dialog, "buildingObjectStairHost"), "legacy");
            control<QComboBox>(dialog, "buildingObjectStairHostLanding").setCurrentIndex(static_cast<int>(role));
            field(dialog, "buildingObjectPostSpacing", "800 mm");
            control<QPushButton>(dialog, "buildingObjectRailingPreview").click();
            require(dialog.lastError().isEmpty(), "actual landing preview must validate its current native host");
            capture(dialog, role == 0 ? "stair-connecting-landing-dialog.png" : "stair-top-landing-dialog.png");
            control<QPushButton>(dialog, "buildingObjectSubmit").click();
            require(dialog.result() == QDialog::Accepted, "actual modal must submit each landing role");
        });
        const auto after = window.document().snapshot(); rails[role] = added_id(before, after, "railing");
        const auto rail = decode_railing_properties(rails[role], after.entities().at(rails[role]).properties);
        require(rail.landing_host && rail.landing_host->role == (role == 0 ? StairLandingRole::connecting : StairLandingRole::top),
                "controller must retain the selected landing role");
        for (const auto* key : {"property_id", "building_id", "floor_id", "layer_id"})
            require(after.entities().at(rails[role]).properties.at(key) == canonical_host.properties.at(key),
                    "landing guard must inherit its host organization");
        const auto phases = ModelPhases::from_json(after.entities().at("phases").properties.at("model"));
        require(phases.state(std::nullopt).at(rails[role]) == ModelPhase::existing &&
                    phases.state(std::optional<std::string>{"remove"}).at(rails[role]) == ModelPhase::demolished,
                "landing guard must mirror all host phase alternatives");
        atomic(window, before, after, 54);
        (void)actual_plan(window, rails[role]);
        require(!geometry(window).solids.at(rails[role]).shape.IsNull(), "each landing role must reach actual 3D geometry");
    }
    auto source = window.document().snapshot();
    auto decorated = source.entities().at(rails[0]);
    decorated.properties["host"]["future_anchor"] = {{"literal", "legacy"}, {"opaque", 7}};
    decorated.extensions["vendor"] = "landing guard";
    window.document().apply(ApplyEntityChanges{source.revision(), {EntityChange::upsert(decorated)}, {}, "opaque landing metadata"});
    require(window.selectEntity(QString::fromStdString(rails[0])), "select actual landing guard for property edit");
    source = window.document().snapshot(); const auto spacing = source.entities().at(rails[0]).properties.at("quantity_entries").at("/post_spacing_m");
    modal(window, "editBuildingObject", [&](BuildingObjectDialog& dialog) {
        field(dialog, "buildingObjectHeight", "1000 mm"); control<QPushButton>(dialog, "buildingObjectSubmit").click();
        require(dialog.result() == QDialog::Accepted, "actual landing guard edit must accept");
    });
    auto after = window.document().snapshot();
    require(after.entities().at(rails[0]).properties.at("host") == source.entities().at(rails[0]).properties.at("host") &&
                after.entities().at(rails[0]).properties.at("quantity_entries").at("/post_spacing_m") == spacing,
            "controller property edit must preserve exact landing witness metadata and unchanged receipt");
    atomic(window, source, after, 54);
    const auto before_transform = window.document().snapshot();
    const auto old_bounds = bounds(geometry(window).solids.at(rails[0]).shape);
    require(window.selectEntity("legacy") && window.transformSelectedArchitecturalObject("0", "2 m", "3 m", "0 m", "1", false),
            "host translation must move both landing roles");
    after = window.document().snapshot();
    for (const auto& id : rails) require(after.entities().at(id) == before_transform.entities().at(id),
            "host translation must retain exact authored landing attachments");
    const auto moved_bounds = bounds(geometry(window).solids.at(rails[0]).shape);
    for (std::size_t i = 0; i < old_bounds.size(); ++i) require_near(moved_bounds[i], old_bounds[i] + (i % 3 == 0 ? 2 : i % 3 == 1 ? 3 : 0));
    atomic(window, before_transform, after, 54);
    source = window.document().snapshot();
    require(window.selectEntity("legacy") && window.transformSelectedArchitecturalObject("0", "0 m", "0 m", "0 m", "1.5", true),
            "host graph clone with scaling must include landing guards");
    after = window.document().snapshot(); const auto clone_host = added_id(source, after, "stair");
    std::vector<std::string> clone_rails;
    const auto cloned_stair = stair(after, clone_host);
    for (const auto& [id, entity] : after.entities()) if (entity.type == "railing" && !source.entities().contains(id)) {
        const auto rail = decode_railing_properties(id, entity.properties);
        require(rail.landing_host && rail.landing_host->stair_id == clone_host &&
                    rail.landing_host->incoming_flight_id == (rail.landing_host->role == StairLandingRole::top ? cloned_stair.flights.back().id : cloned_stair.flights.front().id),
                "cloned landing guard must remap its stable stair and incoming flight witnesses");
        if (rail.landing_host->role == StairLandingRole::connecting)
            require(rail.landing_host->landing_id == cloned_stair.landings.front().id &&
                        rail.landing_host->outgoing_flight_id == cloned_stair.flights.back().id &&
                        entity.properties.at("host").at("future_anchor") == source.entities().at(rails[0]).properties.at("host").at("future_anchor"),
                    "connecting clone must remap only typed child IDs while retaining opaque anchor content");
        const auto& old = source.entities().at(rail.landing_host->role == StairLandingRole::top ? rails[1] : rails[0]);
        require_near(rail.height, old.properties.at("height_m").get<double>() * 1.5);
        require_near(rail.thickness, old.properties.at("thickness_m").get<double>() * 1.5);
        require_near(rail.post_spacing, old.properties.at("post_spacing_m").get<double>() * 1.5);
        require(entity.properties.at("host").at("start_fraction") == old.properties.at("host").at("start_fraction") &&
                    entity.properties.at("host").at("end_fraction") == old.properties.at("host").at("end_fraction"),
                "scaled clone must preserve authored edge coverage percentages");
        clone_rails.push_back(id); (void)actual_plan(window, id);
    }
    require(clone_rails.size() == 2, "host graph clone must retain both landing roles"); atomic(window, source, after, 54);
    require(window.selectEntity(QString::fromStdString(clone_host)) && window.copySelection(), "copy cloned landing guard graph");
    source = window.document().snapshot(); require(window.pasteSelection(), "actual paste must remap complete landing graph");
    after = window.document().snapshot(); const auto paste_host = added_id(source, after, "stair");
    const auto pasted_stair = stair(after, paste_host); std::size_t pasted_rails{};
    for (const auto& [id, entity] : after.entities()) if (entity.type == "railing" && !source.entities().contains(id)) {
        const auto rail = decode_railing_properties(id, entity.properties); ++pasted_rails;
        require(rail.landing_host && rail.landing_host->stair_id == paste_host &&
                    rail.landing_host->incoming_flight_id == (rail.landing_host->role == StairLandingRole::top ? pasted_stair.flights.back().id : pasted_stair.flights.front().id),
                "pasted landing guard must point to its freshly allocated stair topology");
        if (rail.landing_host->role == StairLandingRole::connecting)
            require(rail.landing_host->landing_id == pasted_stair.landings.front().id &&
                        rail.landing_host->outgoing_flight_id == pasted_stair.flights.back().id,
                    "paste must remap connecting landing and both ordered flight witnesses");
        (void)actual_plan(window, id);
    }
    require(pasted_rails == 2, "pasted host graph must contain both dependent landing roles"); atomic(window, source, after, 54);
    QTemporaryDir directory; require(directory.isValid(), "isolated landing guard archive directory");
    const auto path = directory.filePath("landing-guards.bldproj"); const auto saved = window.document().snapshot();
    require(window.saveProjectAs(path) && window.createNewProject(), "save complete v3 landing lifecycle and release writer lease");
    MainWindow reopened; prepare(reopened, false); require(reopened.openProject(path) && reopened.document().is_editable(), "v3 landing archive must reopen editable");
    const auto restored = reopened.document().snapshot();
    require(restored.entities() == saved.entities() && restored.assets() == saved.assets() && restored.history().size() == saved.history().size() &&
                ProjectStore::required_format_version(restored) == 54,
            "save/reopen must preserve exact v3 landing graph and retained history floor");
    require(reopened.undoCommand() && reopened.document().snapshot().entities() == source.entities() && reopened.redoCommand() &&
                reopened.document().snapshot().entities() == saved.entities(), "reopened landing clipboard command must undo and redo exactly");
    (void)geometry(reopened);
    for (const auto& id : rails) (void)actual_plan(reopened, id);
}
void authored_stair_host_context_lifecycle() {
    for (const bool legacy_shorthand : {false, true}) {
        MainWindow window(fixture()); prepare(window);
        const auto initial = window.document().snapshot();
        modal(window, "createBuildingObject", [&](BuildingObjectDialog& dialog) {
            form(dialog, "stair", "multi_flight_stair");
            control<QTableWidget>(dialog, "buildingObjectStairFlights").item(0, 0)->setText("5");
            control<QPushButton>(dialog, "buildingObjectAddStairFlight").click();
            control<QTableWidget>(dialog, "buildingObjectStairFlights").item(1, 0)->setText("7");
            field(dialog, "buildingObjectTotalRise", "3 m");
            field(dialog, "buildingObjectGoing", "300 mm");
            control<QCheckBox>(dialog, "buildingObjectLandingEnabled").setChecked(true);
            field(dialog, "buildingObjectLandingDepth", "1200 mm");
            control<QPushButton>(dialog, "buildingObjectSubmit").click();
            require(dialog.result() == QDialog::Accepted, "MainWindow authored v2 stair must submit");
        });
        const auto authored = window.document().snapshot(); const auto host_id = only_id(authored, "stair");
        require(stair(authored, host_id).flights.size() == 2 && stair(authored, host_id).landings.size() == 1,
                "controller regression must author a v2 stair with flight and landing anchors");
        for (const auto& [key, value] : std::array<std::pair<const char*, const char*>, 4>{{
                 {"property_id", "p"}, {"building_id", "b"}, {"floor_id", "f"}, {"layer_id", "l"}}})
            require(authored.entities().at(host_id).properties.at(key) == value,
                    "new MainWindow stair must persist complete resolved organization");
        atomic(window, initial, authored, 51);
        if (legacy_shorthand) {
            auto host = window.document().snapshot().entities().at(host_id);
            host.properties.erase("property_id"); host.properties.erase("building_id");
            host.extensions["vendor"] = {{"opaque", "legacy authored context"}};
            host.properties["flights"][0]["future_child"] = {{"literal", host_id}};
            window.document().apply(ApplyEntityChanges{window.document().revision(), {EntityChange::upsert(host)}, {}, "v2 authored organization shorthand"});
        }
        std::array<std::string, 2> rails;
        auto before_last = window.document().snapshot();
        for (std::size_t role = 0; role < rails.size(); ++role) {
            const auto before = window.document().snapshot(); before_last = before;
            modal(window, "createBuildingObject", [&](BuildingObjectDialog& dialog) {
                form(dialog, "railing", role == 0 ? "stair_flight_railing" : "stair_landing_railing");
                choose(control<QComboBox>(dialog, "buildingObjectStairHost"), QString::fromStdString(host_id));
                field(dialog, "buildingObjectPostSpacing", "500 mm");
                control<QPushButton>(dialog, "buildingObjectSubmit").click();
                require(dialog.result() == QDialog::Accepted, "authored stair flight and landing rail creator must submit");
            });
            const auto after = window.document().snapshot(); rails[role] = added_id(before, after, "railing");
            auto expected_host = before.entities().at(host_id);
            expected_host.properties["property_id"] = "p"; expected_host.properties["building_id"] = "b";
            require(after.entities().at(host_id) == expected_host,
                    "rail admission must only fill missing host hierarchy and retain exact geometry and opaque metadata");
            const auto& rail = after.entities().at(rails[role]);
            for (const auto* key : {"property_id", "building_id", "floor_id", "layer_id"})
                require(rail.properties.at(key) == expected_host.properties.at(key),
                        "authored stair rail must persist its host's complete organization");
            const auto decoded = decode_railing_properties(rail.id, rail.properties);
            require(role == 0 ? decoded.host && decoded.host->stair_id == host_id :
                               decoded.landing_host && decoded.landing_host->stair_id == host_id,
                    "authored rail must retain the selected typed stair anchor");
            atomic(window, before, after, role == 0 ? 52 : 54);
            (void)actual_plan(window, rails[role]);
            require(!geometry(window).solids.at(rails[role]).shape.IsNull(),
                    "authored stair flight and landing rails must prepare real native geometry");
        }
        QTemporaryDir directory; require(directory.isValid(), "isolated authored stair archive directory");
        const auto path = directory.filePath("authored-stair-context.bldproj"); const auto saved = window.document().snapshot();
        require(window.saveProjectAs(path) && window.createNewProject(), "save authored stair context and release writer lease");
        MainWindow reopened; prepare(reopened, false);
        require(reopened.openProject(path) && reopened.document().is_editable(), "authored stair archive must reopen editable");
        const auto restored = reopened.document().snapshot();
        require(restored.entities() == saved.entities() && restored.assets() == saved.assets() &&
                    restored.history().size() == saved.history().size() && ProjectStore::required_format_version(restored) == 54,
                "save/reopen must retain authored stair hierarchy, both rails, assets and history");
        require(reopened.undoCommand() && reopened.document().snapshot().entities() == before_last.entities() &&
                    reopened.redoCommand() && reopened.document().snapshot().entities() == saved.entities(),
                "reopened authored landing rail command must undo and redo exact source maps");
    }
}
void actual_modal_source_guards() {
    for (int change = 0; change < 4; ++change) {
        MainWindow window(fixture(true)); prepare(window); require(window.selectEntity("legacy"), "guard source selection");
        const auto source = window.document().snapshot(); DocumentSnapshot expected = source;
        modal(window, "editBuildingObject", [&](BuildingObjectDialog& dialog) {
            field(dialog, "buildingObjectWidth", "1500 mm");
            if (change < 2) {
                auto replacement = source; auto& record = const_cast<std::vector<RevisionRecord>&>(replacement.history()).front();
                if (change == 0) record.entities.at("legacy").extensions["replacement"] = "same identity and revision";
                else record.assets.at("opaque-asset") = Asset::create("opaque-asset", "application/octet-stream", {std::byte{9}}, {{"vendor", "changed"}});
                window.document() = Document::fork(replacement);
                require(window.document().snapshot().document_id() == source.document_id() && window.document().revision() == source.revision(),
                        "guard fixture must actually replace full source with the same identity and revision");
                require(document_snapshot_digest(window.document().snapshot()) != document_snapshot_digest(source), "replacement must change complete source");
            } else if (change == 2) window.setWorkspace(Workspace::measurement);
            else require(window.selectEntity("f"), "change selected owner while actual modal is retained");
            expected = window.document().snapshot(); control<QPushButton>(dialog, "buildingObjectSubmit").click();
            require(dialog.result() == QDialog::Accepted, "valid detached edit must submit so MainWindow exercises retained-context guard");
        });
        unchanged(expected, window.document().snapshot());
    }
}
} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen"); sketch::testing::noninteractive_errors();
    QStandardPaths::setTestModeEnabled(true); QApplication app(argc, argv);
    QCoreApplication::setApplicationName("Vertex-stair-lifecycle-" + QUuid::createUuid().toString(QUuid::WithoutBraces));
    try {
        multiflight_creation_and_property_edit(); hosted_upgrade_geometry_failure_and_roundtrip();
        inactive_demolition_membership(); clipboard_lifecycle(); landing_guard_full_controller_lifecycle();
        authored_stair_host_context_lifecycle(); actual_modal_source_guards();
    } catch (const std::exception& error) { std::cerr << "stair_lifecycle_desktop_tests: " << error.what() << '\n'; return 1; }
    std::cout << "Stair lifecycle desktop tests passed\n"; return 0;
}
